// GreenScope v0.6.0 — advanced telemetry, throttled polling, and refreshed dashboard layout
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <powrprof.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "PowrProf.lib")

#ifndef NTSTATUS
typedef LONG NTSTATUS;
#endif
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

typedef struct {
    ULONG ThermalStamp;
    ULONG ThermalConstant1;
    ULONG ThermalConstant2;
    ULONG Processors;
    ULONG SamplingPeriod;
    ULONG CurrentTemperature;
    ULONG PassiveTripPoint;
    ULONG CriticalTripPoint;
    UCHAR ActiveTripPointCount;
    UCHAR ActiveTripPoint[10];
} SYSTEM_THERMAL_INFORMATION;

#ifndef SystemProcessorPerformanceInformation
#define SystemProcessorPerformanceInformation 8
#endif

static int g_use_utf8 = 1; // toggle with 'A'
static const WORD COLOR_DEFAULT = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
static const WORD COLOR_CPU = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
static const WORD COLOR_MEM = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
static const WORD COLOR_DISK = FOREGROUND_RED | FOREGROUND_INTENSITY;
static const WORD COLOR_NET = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
static const WORD COLOR_BATT = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
static const WORD COLOR_INFO = FOREGROUND_GREEN | FOREGROUND_BLUE;

static SHORT win_width(){
    CONSOLE_SCREEN_BUFFER_INFO s;
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &s);
    SHORT w = (SHORT)(s.srWindow.Right - s.srWindow.Left + 1);
    return (w > 0 ? w : 100);
}

static void enable_utf8_and_vt(){
    SetConsoleOutputCP(65001);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}
static void set_color(HANDLE h, WORD attr){ SetConsoleTextAttribute(h, attr); }
static void hide_cursor(HANDLE h){ CONSOLE_CURSOR_INFO ci; GetConsoleCursorInfo(h, &ci); ci.bVisible=FALSE; SetConsoleCursorInfo(h, &ci); }
static void goto_xy(HANDLE h, SHORT x, SHORT y){ COORD c={x,y}; SetConsoleCursorPosition(h,c); }
static void clear_screen(HANDLE h){
    CONSOLE_SCREEN_BUFFER_INFO s;
    if(!GetConsoleScreenBufferInfo(h,&s)) return;
    DWORD cells = s.dwSize.X * s.dwSize.Y;
    COORD home = {0,0};
    DWORD written = 0;
    FillConsoleOutputCharacterA(h,' ',cells,home,&written);
    FillConsoleOutputAttribute(h,s.wAttributes,cells,home,&written);
    SetConsoleCursorPosition(h,home);
}

static void draw_bar(double pct, int w){
    if(pct<0)pct=0; if(pct>100)pct=100; int f=(int)(pct/100.0*w+0.5);
    putchar('['); for(int i=0;i<w;i++) putchar(i<f?'#':'-'); putchar(']'); printf(" %5.1f%%",pct);
}
static void draw_battery_bar(int pct, int charging, const char*extra){
    if (pct<0) {
        printf("Battery    : N/A");
        if(extra && *extra) printf(" %s", extra);
        printf("\n");
        return;
    }
    if (pct>100) pct=100;
    int w=20; int f=(pct*w+50)/100;
    printf("Battery    : ");
    putchar('[');
    for(int i=0;i<w;i++) putchar(i<f?'#':'-');
    putchar(']');
    printf(" %3d%%", pct);
    if(charging) printf(" (AC)");
    if(extra && *extra) printf(" %s", extra);
    printf("\n");
}
static void human_bytes(double b,char*o,size_t n){ const char*u[]={"B","KB","MB","GB","TB","PB"}; int k=0; double v=b; while(v>=1024.0&&k<5){v/=1024.0;k++;} snprintf(o,n,"%.1f %s",v,u[k]); }

static ULONGLONG ft_to_ull(FILETIME ft){ ULARGE_INTEGER u; u.LowPart=ft.dwLowDateTime; u.HighPart=ft.dwHighDateTime; return u.QuadPart; }
typedef struct {
    double usage_pct;
    double* per_core;
    int core_count;
} CpuUsage;

typedef NTSTATUS (WINAPI *NtQuerySystemInformation_t)(ULONG, PVOID, ULONG, PULONG);

typedef struct {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG InterruptCount;
} SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION;

static NtQuerySystemInformation_t pNtQuerySystemInformation = NULL;
static SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION* g_prev_core_perf = NULL;
static int g_prev_core_count = 0;
static int g_core_perf_ready = 0;

static void init_performance_queries(){
    if(pNtQuerySystemInformation) return;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if(!ntdll) ntdll = LoadLibraryW(L"ntdll.dll");
    if(ntdll){
        pNtQuerySystemInformation = (NtQuerySystemInformation_t)GetProcAddress(ntdll, "NtQuerySystemInformation");
    }
    SYSTEM_INFO info; GetSystemInfo(&info);
    g_prev_core_count = (int)info.dwNumberOfProcessors;
    if(g_prev_core_count>0){
        g_prev_core_perf = (SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION*)calloc(g_prev_core_count, sizeof(*g_prev_core_perf));
    }
}

static CpuUsage cpu_usage(){
    CpuUsage c = { .usage_pct = -1.0, .per_core = NULL, .core_count = 0 };
    init_performance_queries();
    FILETIME fi,fk,fu;
    if(!GetSystemTimes(&fi,&fk,&fu)) return c;
    static ULONGLONG pi=0,pk=0,pu=0;
    ULONGLONG i=ft_to_ull(fi),k=ft_to_ull(fk),u=ft_to_ull(fu);
    if(!pi&&!pk&&!pu){pi=i;pk=k;pu=u;}
    ULONGLONG di=i-pi, dk=k-pk, du=u-pu, dt=dk+du; pi=i;pk=k;pu=u; if(dt) c.usage_pct=(double)(dt-di)/(double)dt*100.0; else c.usage_pct=0.0;

    if(!pNtQuerySystemInformation || !g_prev_core_perf || g_prev_core_count<=0) return c;

    int cores = g_prev_core_count;
    SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION* buf = (SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION*)calloc(cores, sizeof(*buf));
    if(!buf) return c;
    ULONG retLen=0;
    NTSTATUS st = pNtQuerySystemInformation(SystemProcessorPerformanceInformation, buf, (ULONG)(sizeof(*buf)*cores), &retLen);
    if(st<0){
        free(buf);
        return c;
    }
    if(!g_core_perf_ready){
        memcpy(g_prev_core_perf, buf, sizeof(*buf)*cores);
        g_core_perf_ready = 1;
        c.per_core = (double*)calloc(cores, sizeof(double));
        if(c.per_core) c.core_count = cores;
        free(buf);
        return c;
    }
    c.per_core = (double*)calloc(cores, sizeof(double));
    if(!c.per_core){
        free(buf);
        return c;
    }
    for(int idx=0; idx<cores; ++idx){
        SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION cur = buf[idx];
        SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION prev = g_prev_core_perf[idx];
        ULONGLONG idle = cur.IdleTime.QuadPart;
        ULONGLONG kernel = cur.KernelTime.QuadPart;
        ULONGLONG user = cur.UserTime.QuadPart;
        ULONGLONG pidle = prev.IdleTime.QuadPart;
        ULONGLONG pkernel = prev.KernelTime.QuadPart;
        ULONGLONG puser = prev.UserTime.QuadPart;
        ULONGLONG dKernel = kernel - pkernel;
        ULONGLONG dUser = user - puser;
        ULONGLONG dIdle = idle - pidle;
        ULONGLONG total = dKernel + dUser;
        double usage = 0.0;
        if(total>0){
            double busy = (double)(total - dIdle);
            if(busy < 0) busy = 0;
            usage = busy / (double)total * 100.0;
        }
        c.per_core[idx] = usage;
        g_prev_core_perf[idx] = cur;
    }
    c.core_count = cores;
    free(buf);
    return c;
}
typedef struct {
    DWORD index;
    ULONGLONG inOctets;
    ULONGLONG outOctets;
} NetAdapterPrev;

typedef struct {
    double inBps, outBps;
    double peakIn, peakOut;
    DWORD adapterCount;
    struct {
        char name[128];
        double inBps;
        double outBps;
    } adapters[16];
} NetRate;

static PMIB_IFTABLE g_if_table = NULL;
static DWORD g_if_table_sz = 0;
static NetAdapterPrev* g_net_prev = NULL;
static size_t g_net_prev_count = 0;
static ULONGLONG g_net_prev_tick = 0;

static NetAdapterPrev* net_prev_for(DWORD idx){
    for(size_t i=0;i<g_net_prev_count;i++) if(g_net_prev[i].index==idx) return &g_net_prev[i];
    g_net_prev = (NetAdapterPrev*)realloc(g_net_prev, sizeof(NetAdapterPrev)*(g_net_prev_count+1));
    NetAdapterPrev* slot = &g_net_prev[g_net_prev_count++];
    slot->index = idx;
    slot->inOctets = slot->outOctets = 0;
    return slot;
}

static NetRate net_rates(){
    NetRate r; memset(&r,0,sizeof(r));
    if(!g_if_table){
        g_if_table_sz = sizeof(MIB_IFTABLE) + sizeof(MIB_IFROW)*16;
        g_if_table = (PMIB_IFTABLE)malloc(g_if_table_sz);
    }
    if(!g_if_table) return r;
    DWORD sz = g_if_table_sz;
    DWORD err = GetIfTable(g_if_table, &sz, TRUE);
    if(err==ERROR_INSUFFICIENT_BUFFER){
        PMIB_IFTABLE tmp = (PMIB_IFTABLE)realloc(g_if_table, sz);
        if(!tmp) return r;
        g_if_table = tmp;
        g_if_table_sz = sz;
        err = GetIfTable(g_if_table, &sz, TRUE);
    }
    if(err!=NO_ERROR) return r;

    ULONGLONG in=0,out=0;
    ULONGLONG now = GetTickCount64();
    double dt = 0.0;
    if(g_net_prev_tick) dt = (double)(now - g_net_prev_tick)/1000.0;
    if(dt <= 0) dt = 1.0;
    r.adapterCount = 0;
    for(DWORD i=0;i<g_if_table->dwNumEntries && r.adapterCount<16;i++){
        MIB_IFROW row = g_if_table->table[i];
        if(row.dwType==IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if(row.dwOperStatus!=IF_OPER_STATUS_OPERATIONAL) continue;
        in  += row.dwInOctets;
        out += row.dwOutOctets;
        NetAdapterPrev* prev = net_prev_for(row.dwIndex);
        double adapterIn = 0.0, adapterOut=0.0;
        if(g_net_prev_tick){
            adapterIn = (double)(row.dwInOctets - prev->inOctets)/dt;
            adapterOut= (double)(row.dwOutOctets - prev->outOctets)/dt;
        }
        prev->inOctets = row.dwInOctets;
        prev->outOctets = row.dwOutOctets;
        if(adapterIn<0) adapterIn=0;
        if(adapterOut<0) adapterOut=0;
        if(r.adapterCount < 16){
            size_t len = row.dwDescrLen<sizeof(r.adapters[0].name)-1 ? row.dwDescrLen : sizeof(r.adapters[0].name)-1;
            memcpy(r.adapters[r.adapterCount].name, row.bDescr, len);
            r.adapters[r.adapterCount].name[len]='\0';
            r.adapters[r.adapterCount].inBps = adapterIn;
            r.adapters[r.adapterCount].outBps = adapterOut;
            if(adapterIn>r.peakIn) r.peakIn = adapterIn;
            if(adapterOut>r.peakOut) r.peakOut = adapterOut;
            r.adapterCount++;
        }
    }
    if(dt>0){
        static ULONGLONG prevInTotal=0, prevOutTotal=0;
        if(prevInTotal) r.inBps = (double)(in - prevInTotal)/dt;
        if(prevOutTotal) r.outBps = (double)(out - prevOutTotal)/dt;
        prevInTotal = in;
        prevOutTotal = out;
    }
    g_net_prev_tick = now;
    return r;
}
typedef struct { double used_pct; ULONGLONG total, avail; } MemInfo;
static MemInfo mem_info(){ MEMORYSTATUSEX ms={.dwLength=sizeof(ms)}; MemInfo m={0,0,0}; if(GlobalMemoryStatusEx(&ms)){m.used_pct=(double)ms.dwMemoryLoad;m.total=ms.ullTotalPhys;m.avail=ms.ullAvailPhys;} return m; }
typedef struct { double used_pct; ULONGLONG total, freeb; } DiskInfo;
typedef struct {
    char name[8];
    double used_pct;
    ULONGLONG total;
    ULONGLONG freeb;
} DriveInfo;

typedef struct {
    DiskInfo summary;
    DriveInfo drives[16];
    int drive_count;
} DiskSnapshot;

static DiskSnapshot disk_info(){
    DiskSnapshot snap; memset(&snap,0,sizeof(snap));
    char buf[256]; DWORD len=GetLogicalDriveStringsA(sizeof(buf)-1,buf); ULONGLONG tot=0,fr=0;
    int di=0;
    for(char*p=buf;p<buf+len && di<16;p+=lstrlenA(p)+1){
        if(GetDriveTypeA(p)!=DRIVE_FIXED) continue;
        ULARGE_INTEGER fbt, tnb, tfn;
        if(GetDiskFreeSpaceExA(p,&fbt,&tnb,&tfn)){
            tot+=tnb.QuadPart; fr+=tfn.QuadPart;
            DriveInfo* drv=&snap.drives[di++];
            snprintf(drv->name, sizeof(drv->name), "%s", p);
            drv->total = tnb.QuadPart;
            drv->freeb = tfn.QuadPart;
            if(drv->total) drv->used_pct = (double)(drv->total-drv->freeb)/(double)drv->total*100.0;
        }
    }
    snap.drive_count = di;
    snap.summary.total = tot;
    snap.summary.freeb = fr;
    if(tot) snap.summary.used_pct = (double)(tot-fr)/(double)tot*100.0;
    return snap;
}
typedef struct {
    int pct;
    int charging;
    int discharging;
    int time_remaining_min;
    double rate_w;
} BatteryInfo;

static BatteryInfo query_battery(){
    BatteryInfo info = { .pct = -1, .charging = 0, .discharging = 0, .time_remaining_min = -1, .rate_w = 0.0 };
    SYSTEM_POWER_STATUS s;
    if (!GetSystemPowerStatus(&s)) return info;
    if (s.BatteryFlag == 128 /* NO_BATTERY */) return info;
    info.pct = (s.BatteryLifePercent <= 100) ? (int)s.BatteryLifePercent : -1;
    info.charging = (s.ACLineStatus == 1);
    info.discharging = (s.ACLineStatus == 0);
    if ((LONG)s.BatteryLifeTime > 0) info.time_remaining_min = s.BatteryLifeTime / 60;
    SYSTEM_BATTERY_STATE state;
    if(CallNtPowerInformation(SystemBatteryState, NULL, 0, &state, sizeof(state))==STATUS_SUCCESS){
        if(state.Rate != 0 && state.Voltage != 0){
            info.rate_w = (double)abs(state.Rate) * (double)state.Voltage / 1000000.0; // convert mW to W
            if(state.Rate < 0) info.charging = 1;
            if(state.Rate > 0) info.discharging = 1;
        }
    }
    return info;
}
static void format_uptime(char*out,size_t n){
    ULONGLONG ms = GetTickCount64();
    ULONGLONG sec = ms/1000ULL;
    ULONGLONG d = sec/86400ULL; sec%=86400ULL;
    ULONGLONG h = sec/3600ULL; sec%=3600ULL;
    ULONGLONG m = sec/60ULL; sec%=60ULL;
    snprintf(out, n, "%llud %02lluh %02llum %02llus", d, h, m, sec);
}

// ---- header (draw once), clock updater (draws inside header) ----
static void draw_header(HANDLE h){
    SHORT width = win_width();
    int inner = width - 2;
    // top
    printf("%c", g_use_utf8?'┌':'+' );
    for(int i=0;i<inner;i++) printf("%c", g_use_utf8?'─':'-' );
    printf("%c\n", g_use_utf8?'┐':'+' );
    // middle with centered title (clock will be drawn separately)
    char title[] = "GREEN SCOPE v0.6.0  —  System Monitor (Q quit, A ascii/unicode)";
    int tlen = (int)strlen(title);
    int left = (inner - tlen)/2; if (left < 0) left = 0;
    printf("%c", g_use_utf8?'│':'|' );
    for(int i=0;i<left;i++) putchar(' ');
    fwrite(title, 1, tlen, stdout);
    for(int i=0;i<inner-left-tlen;i++) putchar(' ');
    printf("%c\n", g_use_utf8?'│':'|' );
    // bottom
    printf("%c", g_use_utf8?'└':'+' );
    for(int i=0;i<inner;i++) printf("%c", g_use_utf8?'─':'-' );
    printf("%c\n", g_use_utf8?'┘':'+' );
}

static void update_clock_in_header(HANDLE h){
    SYSTEMTIME lt; GetLocalTime(&lt);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d  %02d:%02d:%02d",
             lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
    SHORT width = win_width();
    int inner = width - 2;
    int clen = (int)strlen(buf);
    int x = 1 + inner - clen; if (x < 1) x = 1;
    goto_xy(h, (SHORT)x, 1);
    printf("%s", buf);
}

// ---- main ----
static void draw_history(double*buf, int count, int width){
    static const char* blocks = "▁▂▃▄▅▆▇█";
    static const char ascii_blocks[] = ".:-=+*#%@";
    if(count<=0){ for(int i=0;i<width;i++) putchar(' '); return; }
    double max = 0.0;
    for(int i=0;i<count;i++) if(buf[i]>max) max=buf[i];
    if(max<=0) max=1.0;
    for(int i=0;i<width;i++){
        double v = buf[i];
        int level = (int)((v/max)*7.0 + 0.5);
        if(level<0) level=0; if(level>7) level=7;
        if(g_use_utf8) putchar(blocks[level]); else putchar(ascii_blocks[level]);
    }
}

static void history_push(double*ring, int len, int*count, int*pos, double value){
    if(len<=0) return;
    ring[*pos] = value;
    *pos = (*pos + 1) % len;
    if(*count < len) (*count)++;
}

static void history_to_linear(double*ring, double*out, int len, int count, int pos){
    if(count>len) count=len;
    for(int i=0;i<count;i++){
        int idx = (pos + len - count + i) % len;
        out[i] = ring[idx];
    }
    for(int i=count;i<len;i++) out[i]=0;
}

static double* g_cpu_hist=NULL, *g_mem_hist=NULL, *g_net_hist=NULL;
static int g_hist_len=60;
static int g_cpu_pos=0, g_cpu_count=0;
static int g_mem_pos=0, g_mem_count=0;
static int g_net_pos=0, g_net_count=0;

static void ensure_history(){
    if(!g_cpu_hist){
        g_cpu_hist = (double*)calloc(g_hist_len, sizeof(double));
        g_mem_hist = (double*)calloc(g_hist_len, sizeof(double));
        g_net_hist = (double*)calloc(g_hist_len, sizeof(double));
    }
}

typedef struct {
    CpuUsage cpu;
    MemInfo mem;
    DiskSnapshot disk;
    NetRate net;
    BatteryInfo battery;
    char uptime[64];
    double net_combined_bps;
    double temperature_c;
} Metrics;

static double query_temperature(){
    SYSTEM_THERMAL_INFORMATION info;
    if(CallNtPowerInformation(SystemThermalInformation, NULL, 0, &info, sizeof(info))==STATUS_SUCCESS){
        if(info.CurrentTemperature>0){
            return (double)info.CurrentTemperature / 10.0 - 273.15;
        }
    }
    return -273.15;
}

static Metrics collect_metrics(ULONGLONG now, ULONGLONG*last_disk, DiskSnapshot*cached_disk){
    Metrics m; memset(&m,0,sizeof(m));
    m.cpu = cpu_usage();
    m.mem = mem_info();
    static NetRate cached_net;
    static ULONGLONG last_net = 0;
    if(!last_net || now - last_net >= 500){
        cached_net = net_rates();
        last_net = now;
    }
    m.net = cached_net;
    m.net_combined_bps = m.net.inBps + m.net.outBps;
    if(!*last_disk || now - *last_disk > 5000){
        *cached_disk = disk_info();
        *last_disk = now;
    }
    m.disk = *cached_disk;
    static BatteryInfo cached_batt;
    static ULONGLONG last_batt = 0;
    if(!last_batt || now - last_batt > 5000){
        cached_batt = query_battery();
        last_batt = now;
    }
    m.battery = cached_batt;
    format_uptime(m.uptime, sizeof m.uptime);
    static double cached_temp = -273.15;
    static ULONGLONG last_temp = 0;
    if(!last_temp || now - last_temp > 2000){
        cached_temp = query_temperature();
        last_temp = now;
    }
    m.temperature_c = cached_temp;
    return m;
}

static void render_metrics(HANDLE h, Metrics*m){
    SHORT y = 4;
    const int barw=40;
    set_color(h, COLOR_CPU);
    goto_xy(h,0,y++); printf("CPU Usage  : "); draw_bar(m->cpu.usage_pct<0?0:m->cpu.usage_pct, barw); printf("\n");
    if(m->cpu.per_core && m->cpu.core_count>0){
        goto_xy(h,2,y++); printf("Per-core   : ");
        for(int i=0;i<m->cpu.core_count;i++){
            printf("C%-2d %5.1f%%  ", i, m->cpu.per_core[i]);
            if((i+1)%4==0 && i+1<m->cpu.core_count){ printf("\n"); goto_xy(h,14,y++); }
        }
        printf("\n");
        y++;
    }
    set_color(h, COLOR_MEM);
    char t[32],a[32]; human_bytes((double)m->mem.total,t,sizeof t); human_bytes((double)m->mem.avail,a,sizeof a);
    goto_xy(h,0,y++); printf("Memory     : "); draw_bar(m->mem.used_pct, barw); printf("  total %s, free %s\n", t,a);
    set_color(h, COLOR_DISK);
    char dt[32],df[32]; human_bytes((double)m->disk.summary.total,dt,sizeof dt); human_bytes((double)m->disk.summary.freeb,df,sizeof df);
    goto_xy(h,0,y++); printf("Disk (all) : "); draw_bar(m->disk.summary.used_pct, barw); printf("  total %s, free %s\n", dt,df);
    if(m->disk.drive_count>0){
        goto_xy(h,2,y++); printf("By drive   : ");
        for(int i=0;i<m->disk.drive_count;i++){
            DriveInfo*d=&m->disk.drives[i]; char used[32],freeb[32];
            human_bytes((double)d->total,used,sizeof used); human_bytes((double)d->freeb,freeb,sizeof freeb);
            printf("%s %5.1f%% (%s total, %s free)  ", d->name, d->used_pct, used, freeb);
            if((i+1)%2==0 && i+1<m->disk.drive_count){ printf("\n"); goto_xy(h,15,y++); }
        }
        printf("\n");
        y++;
    }
    set_color(h, COLOR_NET);
    char rin[32], rout[32]; human_bytes(m->net.inBps,rin,sizeof rin); human_bytes(m->net.outBps,rout,sizeof rout);
    goto_xy(h,0,y++); printf("Network    :  in %s/s   out %s/s\n", rin, rout);
    if(m->net.adapterCount>0){
        goto_xy(h,2,y++); printf("Adapters   : ");
        for(DWORD i=0;i<m->net.adapterCount;i++){
            char in[32],out[32];
            human_bytes(m->net.adapters[i].inBps,in,sizeof in);
            human_bytes(m->net.adapters[i].outBps,out,sizeof out);
            printf("%s ↑%s/s ↓%s/s  ", m->net.adapters[i].name, out, in);
            if((i+1)%2==0 && i+1<m->net.adapterCount){ printf("\n"); goto_xy(h,14,y++); }
        }
        printf("\n");
        y++;
    }
    set_color(h, COLOR_BATT);
    char extra[96]="";
    char detail[96]="";
    if(m->battery.time_remaining_min>=0){
        snprintf(detail, sizeof detail, "%s%dm left", m->battery.charging?"est charge ":"est runtime ", m->battery.time_remaining_min);
    }
    if(m->battery.rate_w>0){
        char buf[32]; snprintf(buf,sizeof buf,"%s%.1fW", m->battery.charging?"chg ":"draw ", m->battery.rate_w);
        if(detail[0]){ strncat(detail, ", ", sizeof(detail)-strlen(detail)-1); }
        strncat(detail, buf, sizeof(detail)-strlen(detail)-1);
    }
    if(detail[0]){
        snprintf(extra, sizeof extra, "(%s)", detail);
    }
    goto_xy(h,0,y++); draw_battery_bar(m->battery.pct, m->battery.charging, extra[0]?extra:NULL);
    set_color(h, COLOR_INFO);
    if(m->temperature_c>-200){
        goto_xy(h,0,y++);
        if(g_use_utf8) printf("Thermals   : %5.1f °C\n", m->temperature_c);
        else printf("Thermals   : %5.1f C\n", m->temperature_c);
    }else{
        goto_xy(h,0,y++); printf("Thermals   : N/A\n");
    }
    goto_xy(h,0,y++); printf("Uptime     :  %s\n", m->uptime);

    set_color(h, COLOR_DEFAULT);
    double cpuHist[60]={0}, memHist[60]={0}, netHist[60]={0};
    ensure_history();
    history_to_linear(g_cpu_hist, cpuHist, g_hist_len, g_cpu_count, g_cpu_pos);
    history_to_linear(g_mem_hist, memHist, g_hist_len, g_mem_count, g_mem_pos);
    history_to_linear(g_net_hist, netHist, g_hist_len, g_net_count, g_net_pos);
    int histWidth = g_hist_len<60?g_hist_len:60;
    int cpuCount = g_cpu_count < histWidth ? g_cpu_count : histWidth;
    int memCount = g_mem_count < histWidth ? g_mem_count : histWidth;
    int netCount = g_net_count < histWidth ? g_net_count : histWidth;
    goto_xy(h,0,y++); printf("History CPU:"); goto_xy(h,12,y-1); draw_history(cpuHist, cpuCount, histWidth); printf("\n");
    goto_xy(h,0,y++); printf("History MEM:"); goto_xy(h,12,y-1); draw_history(memHist, memCount, histWidth); printf("\n");
    goto_xy(h,0,y++); printf("History NET:"); goto_xy(h,12,y-1); draw_history(netHist, netCount, histWidth); printf("\n");

    SHORT w = win_width();
    goto_xy(h,0,y++);
    for(int i=0;i<w;i++) printf("%c", g_use_utf8?'─':'-');
    printf("\nTip: A toggles ASCII/Unicode borders. Q quits. Refresh 0.25s (heavy metrics throttled).\n");
    set_color(h, COLOR_DEFAULT);
}

int main(){
    HANDLE h=GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTitleA("GreenScope - System Monitor");
    enable_utf8_and_vt();
    set_color(h, COLOR_DEFAULT);
    hide_cursor(h);

    clear_screen(h);
    draw_header(h);

    ensure_history();
    CpuUsage initial = cpu_usage(); if(initial.per_core) free(initial.per_core);
    net_rates();

    ULONGLONG last_disk = 0;
    DiskSnapshot cached_disk; memset(&cached_disk,0,sizeof(cached_disk));
    ULONGLONG last_hist_update = 0;
    for(;;){
        if(GetAsyncKeyState('Q') & 0x8000) break;
        if(GetAsyncKeyState('A') & 0x0001){
            g_use_utf8 = !g_use_utf8;
            clear_screen(h);
            draw_header(h);
        }

        update_clock_in_header(h);
        ULONGLONG now = GetTickCount64();
        Metrics metrics = collect_metrics(now, &last_disk, &cached_disk);
        render_metrics(h, &metrics);

        if(metrics.cpu.per_core) free(metrics.cpu.per_core);

        if(!last_hist_update || now - last_hist_update >= 1000){
            history_push(g_cpu_hist, g_hist_len, &g_cpu_count, &g_cpu_pos, metrics.cpu.usage_pct>=0?metrics.cpu.usage_pct:0);
            history_push(g_mem_hist, g_hist_len, &g_mem_count, &g_mem_pos, metrics.mem.used_pct);
            history_push(g_net_hist, g_hist_len, &g_net_count, &g_net_pos, metrics.net_combined_bps);
            last_hist_update = now;
        }

        Sleep(250);
    }
    set_color(h,7);
    clear_screen(h);
    printf("GreenScope closed.\n");
    return 0;
}
