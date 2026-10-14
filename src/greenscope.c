// GreenScope — green-on-black console system monitor. Version: see GS_VERSION below.
#define _WIN32_WINNT 0x0600
#include <winsock2.h> // before windows.h; with ws2ipdef.h it unlocks MIB_IF_ROW2/GetIfTable2 in iphlpapi.h
#include <windows.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GS_VERSION "0.5.3"

static int g_use_utf8 = 1; // toggle with 'A'

// Box characters as \u escapes: a '┌' char constant is multi-byte and gcc keeps only one byte of it,
// and escapes don't depend on how the source file happens to be encoded.
#define BX_TL "\u250C"
#define BX_TR "\u2510"
#define BX_BL "\u2514"
#define BX_BR "\u2518"
#define BX_H  "\u2500"
#define BX_V  "\u2502"
static const char *bx(const char *uni, const char *ascii){ return g_use_utf8 ? uni : ascii; }

// End of line: clear whatever a longer previous frame left to the right of the cursor.
#define EOL "\x1b[K\n"
#define VT_CLEAR "\x1b[2J\x1b[H"
// Clear from the cursor to the end of the screen: drops rows left over when the frame gets shorter.
#define VT_CLEAR_BELOW "\x1b[J"

// ---- console state: saved at startup, restored on Q, Ctrl+C or window close ----
static HANDLE g_out, g_in;
static WORD g_orig_attr = 7;
static CONSOLE_CURSOR_INFO g_orig_cursor;
static DWORD g_orig_out_mode, g_orig_in_mode;
static int g_have_attr, g_have_cursor, g_have_out_mode, g_have_in_mode;
static volatile LONG g_restored = 0;
static void gpu_shutdown(void);

static void save_console(void){
    CONSOLE_SCREEN_BUFFER_INFO s;
    if (GetConsoleScreenBufferInfo(g_out, &s)) { g_orig_attr = s.wAttributes; g_have_attr = 1; }
    g_have_cursor   = GetConsoleCursorInfo(g_out, &g_orig_cursor) != 0;
    g_have_out_mode = GetConsoleMode(g_out, &g_orig_out_mode) != 0;
    g_have_in_mode  = GetConsoleMode(g_in, &g_orig_in_mode) != 0;
}
// Runs from the main thread on Q and from the Ctrl handler's thread on Ctrl+C/close; only the first caller acts.
static void restore_console(void){
    if (InterlockedExchange(&g_restored, 1)) return;
    fputs(VT_CLEAR, stdout); // while VT mode is still on
    fflush(stdout);
    if (g_have_attr) SetConsoleTextAttribute(g_out, g_orig_attr);
    if (g_have_cursor) SetConsoleCursorInfo(g_out, &g_orig_cursor);
    if (g_have_out_mode) SetConsoleMode(g_out, g_orig_out_mode);
    if (g_have_in_mode) SetConsoleMode(g_in, g_orig_in_mode);
    gpu_shutdown();
}
static BOOL WINAPI ctrl_handler(DWORD type){
    (void)type;
    restore_console();
    return FALSE; // let the default handler end the process
}

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
// Window input gives us WINDOW_BUFFER_SIZE_EVENT; processed input keeps Ctrl+C as a signal, not a key.
static void enable_key_input(HANDLE in){
    DWORD mode = 0;
    if (GetConsoleMode(in, &mode)) {
        SetConsoleMode(in, mode | ENABLE_WINDOW_INPUT | ENABLE_PROCESSED_INPUT);
    }
}
static void set_color(HANDLE h, WORD attr){ SetConsoleTextAttribute(h, attr); }
static void hide_cursor(HANDLE h){ CONSOLE_CURSOR_INFO ci; GetConsoleCursorInfo(h, &ci); ci.bVisible=FALSE; SetConsoleCursorInfo(h, &ci); }
static void goto_xy(HANDLE h, SHORT x, SHORT y){ COORD c={x,y}; SetConsoleCursorPosition(h,c); }

static void draw_bar(double pct, int w){
    if(pct<0) pct=0;
    if(pct>100) pct=100;
    int f=(int)(pct/100.0*w+0.5);
    putchar('['); for(int i=0;i<w;i++) putchar(i<f?'#':'-'); putchar(']'); printf(" %5.1f%%",pct);
}
static void draw_battery_bar(int pct, int charging){
    if (pct<0) { printf("Battery    : N/A" EOL); return; }
    if (pct>100) pct=100;
    int w=20; int f=(pct*w+50)/100;
    printf("Battery    : ");
    putchar('[');
    for(int i=0;i<w;i++) putchar(i<f?'#':'-');
    putchar(']');
    printf(" %3d%% %s" EOL, pct, charging ? "(AC)" : "");
}
static void human_bytes(double b,char*o,size_t n){ const char*u[]={"B","KB","MB","GB","TB","PB"}; int k=0; double v=b; while(v>=1024.0&&k<5){v/=1024.0;k++;} snprintf(o,n,"%.1f %s",v,u[k]); }

static ULONGLONG ft_to_ull(FILETIME ft){ ULARGE_INTEGER u; u.LowPart=ft.dwLowDateTime; u.HighPart=ft.dwHighDateTime; return u.QuadPart; }
static double cpu_usage(){
    static ULONGLONG pi=0,pk=0,pu=0; FILETIME fi,fk,fu;
    if(!GetSystemTimes(&fi,&fk,&fu)) return -1.0;
    ULONGLONG i=ft_to_ull(fi),k=ft_to_ull(fk),u=ft_to_ull(fu);
    if(!pi&&!pk&&!pu){pi=i;pk=k;pu=u;return -1.0;}
    ULONGLONG di=i-pi, dk=k-pk, du=u-pu, dt=dk+du; pi=i;pk=k;pu=u; if(!dt) return 0.0;
    return (double)(dt-di)/(double)dt*100.0;
}

// Per-interface counters from the last tick. Rates come from per-interface deltas so an adapter
// that appears, disappears or resets its counters (Wi-Fi off/on) contributes 0 instead of a spike.
#define MAX_IFS 64
typedef struct { ULONG64 luid, in, out; } IfPrev;
static IfPrev g_ifs[MAX_IFS];
static int g_nifs = 0;

typedef struct { double inBps, outBps; } NetRate;
static NetRate net_rates(){
    static ULONGLONG pTick=0;
    ULONG64 din=0, dout=0;
    IfPrev cur[MAX_IFS]; int ncur=0;
    PMIB_IF_TABLE2 t=NULL;
    // GetIfTable2 has 64-bit octet counters; GetIfTable's 32-bit ones wrap every 4 GiB.
    if (GetIfTable2(&t)==NO_ERROR){
        for(ULONG i=0;i<t->NumEntries && ncur<MAX_IFS;i++){
            MIB_IF_ROW2 *row=&t->Table[i];
            if(row->OperStatus!=IfOperStatusUp) continue;
            if(row->Type==IF_TYPE_SOFTWARE_LOOPBACK) continue;
            // Filter interfaces (e.g. packet-capture or QoS shims) repeat the real adapter's traffic.
            if(row->InterfaceAndOperStatusFlags.FilterInterface) continue;
            IfPrev c={row->InterfaceLuid.Value, row->InOctets, row->OutOctets};
            for(int j=0;j<g_nifs;j++){
                if(g_ifs[j].luid!=c.luid) continue;
                if(c.in>=g_ifs[j].in)   din +=c.in -g_ifs[j].in;
                if(c.out>=g_ifs[j].out) dout+=c.out-g_ifs[j].out;
                break;
            }
            cur[ncur++]=c;
        }
        FreeMibTable(t);
    }
    memcpy(g_ifs, cur, sizeof(IfPrev)*(size_t)ncur);
    g_nifs=ncur;

    NetRate r={0,0};
    ULONGLONG now=GetTickCount64();
    if(pTick && now!=pTick){
        double dt=(double)(now-pTick)/1000.0;
        r.inBps=(double)din/dt;
        r.outBps=(double)dout/dt;
    }
    pTick=now;
    return r;
}
typedef struct { double used_pct; ULONGLONG total, avail; } MemInfo;
static MemInfo mem_info(){ MEMORYSTATUSEX ms={.dwLength=sizeof(ms)}; MemInfo m={0,0,0}; if(GlobalMemoryStatusEx(&ms)){m.used_pct=(double)ms.dwMemoryLoad;m.total=ms.ullTotalPhys;m.avail=ms.ullAvailPhys;} return m; }
typedef struct { double used_pct; ULONGLONG total, freeb; } DiskInfo;
static DiskInfo disk_info(){
    char buf[256]; DWORD len=GetLogicalDriveStringsA(sizeof(buf)-1,buf); ULONGLONG tot=0,fr=0;
    for(char*p=buf;p<buf+len;p+=lstrlenA(p)+1){
        if(GetDriveTypeA(p)!=DRIVE_FIXED) continue;
        ULARGE_INTEGER fbt, tnb, tfn;
        if(GetDiskFreeSpaceExA(p,&fbt,&tnb,&tfn)){ tot+=tnb.QuadPart; fr+=tfn.QuadPart; }
    }
    DiskInfo d={0,tot,fr}; if(tot) d.used_pct=(double)(tot-fr)/(double)tot*100.0; return d;
}
static int query_battery(int*charging){
    SYSTEM_POWER_STATUS s;
    if (!GetSystemPowerStatus(&s)) return -1;
    if (s.BatteryFlag == 128 /* NO_BATTERY */) return -1;
    if (charging) *charging = (s.ACLineStatus==1);
    if (s.BatteryLifePercent <= 100) return (int)s.BatteryLifePercent;
    return -1;
}
static void format_uptime(char*out,size_t n){
    ULONGLONG ms = GetTickCount64();
    ULONGLONG sec = ms/1000ULL;
    ULONGLONG d = sec/86400ULL; sec%=86400ULL;
    ULONGLONG h = sec/3600ULL; sec%=3600ULL;
    ULONGLONG m = sec/60ULL; sec%=60ULL;
    snprintf(out, n, "%llud %02lluh %02llum %02llus", d, h, m, sec);
}

// ==== NVIDIA GPU via NVML ====================================================================
// NVML (the library nvidia-smi uses) ships with every NVIDIA driver as nvml.dll. We load it at
// runtime, so there is no SDK, no link flag, and nothing breaks on a PC without NVIDIA.
// Only the handful of types we need are declared here; values match NVIDIA's nvml.h.
typedef int nvmlReturn_t;
typedef struct nvmlDevice_st *nvmlDevice_t;
typedef struct { unsigned int gpu, memory; } nvmlUtilization_t;
typedef struct { unsigned long long total, free, used; } nvmlMemory_t;
#define NVML_SUCCESS 0
#define NVML_TEMPERATURE_GPU 0
#define NVML_CLOCK_GRAPHICS 0
#define NVML_CLOCK_MEM 2
#define NVML_DEVICE_NAME_V2_BUFFER_SIZE 96

typedef nvmlReturn_t (*nvml_void_fn)(void);
typedef nvmlReturn_t (*nvml_count_fn)(unsigned int *);
typedef nvmlReturn_t (*nvml_handle_fn)(unsigned int, nvmlDevice_t *);
typedef nvmlReturn_t (*nvml_name_fn)(nvmlDevice_t, char *, unsigned int);
typedef nvmlReturn_t (*nvml_util_fn)(nvmlDevice_t, nvmlUtilization_t *);
typedef nvmlReturn_t (*nvml_mem_fn)(nvmlDevice_t, nvmlMemory_t *);
typedef nvmlReturn_t (*nvml_uint_fn)(nvmlDevice_t, unsigned int *);
typedef nvmlReturn_t (*nvml_enum_uint_fn)(nvmlDevice_t, int, unsigned int *);
typedef nvmlReturn_t (*nvml_ull_fn)(nvmlDevice_t, unsigned long long *);

static struct {
    HMODULE dll;
    nvml_void_fn init, shutdown;
    nvml_count_fn count;
    nvml_handle_fn handle;
    nvml_name_fn name;
    nvml_util_fn util;
    nvml_mem_fn mem;
    nvml_enum_uint_fn temp, clock;
    nvml_uint_fn power, power_limit, fan;
    nvml_ull_fn reasons;
} g_nvml;

enum { GPU_NONE, GPU_OK, GPU_LOST };
static int g_gpu_state = GPU_NONE;
static nvmlDevice_t g_gpu_dev;
static unsigned int g_gpu_count;
static char g_gpu_name[NVML_DEVICE_NAME_V2_BUFFER_SIZE];
static ULONGLONG g_gpu_retry_at;
// Ctrl+C runs restore_console() on another thread; this lock keeps it from unloading the DLL
// while the main thread is inside an NVML call.
static CRITICAL_SECTION g_gpu_lock;

// Turns the clocks-event-reasons bitmask into a few words. Worst cause first.
static const char *gpu_limit_words(unsigned long long reasons){
    if (reasons & (0x40ULL | 0x20ULL)) return "TOO HOT";      // HW / SW thermal slowdown
    if (reasons & (0x8ULL | 0x80ULL))  return "HW slowdown";  // HW slowdown / power brake
    if (reasons & 0x4ULL)              return "power cap";    // boosting up to the power limit: normal under load
    if (reasons & 0x1ULL)              return "idle";
    return "none";
}

// Try System32 only first, so a planted nvml.dll elsewhere on the search path can't be picked up.
static HMODULE nvml_load_dll(void){
    HMODULE m = LoadLibraryExW(L"nvml.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (m) return m;
    // Older drivers keep it next to nvidia-smi instead.
    wchar_t path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramW6432", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return NULL;
    if (wcslen(path) + wcslen(L"\\NVIDIA Corporation\\NVSMI\\nvml.dll") >= MAX_PATH) return NULL;
    wcscat(path, L"\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    return LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}

// Init NVML and grab GPU 0. Caller holds g_gpu_lock.
static int nvml_open_device(void){
    if (g_nvml.init() != NVML_SUCCESS) return 0;
    if (g_nvml.count(&g_gpu_count) != NVML_SUCCESS || g_gpu_count == 0
        || g_nvml.handle(0, &g_gpu_dev) != NVML_SUCCESS){
        g_nvml.shutdown();
        return 0;
    }
    if (!g_nvml.name || g_nvml.name(g_gpu_dev, g_gpu_name, sizeof g_gpu_name) != NVML_SUCCESS)
        snprintf(g_gpu_name, sizeof g_gpu_name, "NVIDIA GPU");
    return 1;
}

static void gpu_init(void){
    InitializeCriticalSection(&g_gpu_lock);
    g_nvml.dll = nvml_load_dll();
    if (!g_nvml.dll) return;
    // GetProcAddress returns a generic FARPROC; go through void* to cast it to the real signature.
#define NVML_SYM(field, type, sym) g_nvml.field = (type)(void *)GetProcAddress(g_nvml.dll, sym)
    NVML_SYM(init,        nvml_void_fn,      "nvmlInit_v2");
    NVML_SYM(shutdown,    nvml_void_fn,      "nvmlShutdown");
    NVML_SYM(count,       nvml_count_fn,     "nvmlDeviceGetCount_v2");
    NVML_SYM(handle,      nvml_handle_fn,    "nvmlDeviceGetHandleByIndex_v2");
    NVML_SYM(name,        nvml_name_fn,      "nvmlDeviceGetName");
    NVML_SYM(util,        nvml_util_fn,      "nvmlDeviceGetUtilizationRates");
    NVML_SYM(mem,         nvml_mem_fn,       "nvmlDeviceGetMemoryInfo");
    NVML_SYM(temp,        nvml_enum_uint_fn, "nvmlDeviceGetTemperature");
    NVML_SYM(power,       nvml_uint_fn,      "nvmlDeviceGetPowerUsage");
    NVML_SYM(power_limit, nvml_uint_fn,      "nvmlDeviceGetEnforcedPowerLimit");
    NVML_SYM(clock,       nvml_enum_uint_fn, "nvmlDeviceGetClockInfo");
    NVML_SYM(fan,         nvml_uint_fn,      "nvmlDeviceGetFanSpeed");
    NVML_SYM(reasons,     nvml_ull_fn,       "nvmlDeviceGetCurrentClocksEventReasons");
    // Same bitmask under its older name, for drivers that predate the rename.
    if (!g_nvml.reasons) NVML_SYM(reasons, nvml_ull_fn, "nvmlDeviceGetCurrentClocksThrottleReasons");
#undef NVML_SYM
    if (g_nvml.init && g_nvml.shutdown && g_nvml.count && g_nvml.handle && g_nvml.util && g_nvml.mem
        && nvml_open_device()){
        g_gpu_state = GPU_OK;
        return;
    }
    FreeLibrary(g_nvml.dll);
    memset(&g_nvml, 0, sizeof g_nvml);
}

static void gpu_shutdown(void){
    if (!g_nvml.dll) return;
    EnterCriticalSection(&g_gpu_lock);
    if (g_gpu_state == GPU_OK) g_nvml.shutdown();
    FreeLibrary(g_nvml.dll);
    memset(&g_nvml, 0, sizeof g_nvml);
    g_gpu_state = GPU_NONE;
    LeaveCriticalSection(&g_gpu_lock);
}

// One sample of GPU 0. -1 in a field means "couldn't read it", shown as "--".
typedef struct {
    double util_pct, mem_pct;
    unsigned long long mem_used, mem_total;
    int temp_c, watts, watts_limit, fan_pct, core_mhz, mem_mhz;
    int have_reasons; unsigned long long reasons;
} GpuSample;

// Returns the state the sample was taken in: GPU_OK fills *s, otherwise there is nothing to show.
static int gpu_sample(GpuSample *s){
    if (!g_nvml.dll) return GPU_NONE;
    EnterCriticalSection(&g_gpu_lock);
    int state = g_gpu_state;
    // After a driver update or reset, try to reconnect, but no more than once every 5 s.
    if (state == GPU_LOST && GetTickCount64() >= g_gpu_retry_at){
        if (nvml_open_device()) state = g_gpu_state = GPU_OK;
        else g_gpu_retry_at = GetTickCount64() + 5000;
    }
    if (state == GPU_OK){
        nvmlUtilization_t u; nvmlMemory_t m;
        if (g_nvml.util(g_gpu_dev, &u) != NVML_SUCCESS || g_nvml.mem(g_gpu_dev, &m) != NVML_SUCCESS){
            g_nvml.shutdown();
            state = g_gpu_state = GPU_LOST;
            g_gpu_retry_at = GetTickCount64() + 5000;
        } else {
            unsigned int v;
            s->util_pct = u.gpu;
            s->mem_used = m.used; s->mem_total = m.total;
            s->mem_pct = m.total ? (double)m.used / (double)m.total * 100.0 : 0.0;
            // Power is in milliwatts; round to whole watts.
            s->temp_c      = (g_nvml.temp  && g_nvml.temp(g_gpu_dev, NVML_TEMPERATURE_GPU, &v) == NVML_SUCCESS) ? (int)v : -1;
            s->watts       = (g_nvml.power && g_nvml.power(g_gpu_dev, &v) == NVML_SUCCESS) ? (int)((v + 500) / 1000) : -1;
            s->watts_limit = (g_nvml.power_limit && g_nvml.power_limit(g_gpu_dev, &v) == NVML_SUCCESS) ? (int)((v + 500) / 1000) : -1;
            s->fan_pct     = (g_nvml.fan   && g_nvml.fan(g_gpu_dev, &v) == NVML_SUCCESS) ? (int)v : -1;
            s->core_mhz    = (g_nvml.clock && g_nvml.clock(g_gpu_dev, NVML_CLOCK_GRAPHICS, &v) == NVML_SUCCESS) ? (int)v : -1;
            s->mem_mhz     = (g_nvml.clock && g_nvml.clock(g_gpu_dev, NVML_CLOCK_MEM, &v) == NVML_SUCCESS) ? (int)v : -1;
            s->have_reasons = g_nvml.reasons && g_nvml.reasons(g_gpu_dev, &s->reasons) == NVML_SUCCESS;
        }
    }
    LeaveCriticalSection(&g_gpu_lock);
    return state;
}

// "--" for a field NVML couldn't give us.
static const char *gpu_field(char *buf, size_t n, const char *fmt, int v){
    if (v < 0) return "--";
    snprintf(buf, n, fmt, v);
    return buf;
}

// Draws the GPU rows starting at row *y and advances it: three rows normally, one when there's no GPU to show.
static void draw_gpu(HANDLE h, SHORT *y, int barw){
    GpuSample s;
    int state = gpu_sample(&s);
    if (state == GPU_NONE){
        goto_xy(h,0,(*y)++); printf("GPU        :  not available (no NVIDIA driver found)" EOL);
        return;
    }
    if (state == GPU_LOST){
        goto_xy(h,0,(*y)++); printf("GPU        :  lost contact, retrying" EOL);
        return;
    }
    goto_xy(h,0,(*y)++); printf("GPU        : "); draw_bar(s.util_pct, barw); printf("  %s", g_gpu_name);
    if (g_gpu_count > 1) printf(" (GPU 1 of %u)", g_gpu_count);
    printf(EOL);

    char used[32], total[32]; human_bytes((double)s.mem_used,used,sizeof used); human_bytes((double)s.mem_total,total,sizeof total);
    goto_xy(h,0,(*y)++); printf("VRAM       : "); draw_bar(s.mem_pct, barw); printf("  used %s of %s" EOL, used, total);

    char tb[16], wb[16], lb[16], fb[16], cb[16], mb[16];
    const char *deg = g_use_utf8 ? "°C" : "C";
    goto_xy(h,0,(*y)++);
    printf("GPU detail :  %s%s   %s / %s W   fan %s   core %s MHz   mem %s MHz   limit: %s" EOL,
           gpu_field(tb,sizeof tb,"%d",s.temp_c), s.temp_c < 0 ? "" : deg,
           gpu_field(wb,sizeof wb,"%d",s.watts), gpu_field(lb,sizeof lb,"%d",s.watts_limit),
           gpu_field(fb,sizeof fb,"%d%%",s.fan_pct),
           gpu_field(cb,sizeof cb,"%d",s.core_mhz), gpu_field(mb,sizeof mb,"%d",s.mem_mhz),
           s.have_reasons ? gpu_limit_words(s.reasons) : "--");
}
// ==== end NVIDIA GPU ==========================================================================

// ---- header (draw once), clock updater (draws inside header) ----
static void draw_header(void){
    SHORT width = win_width();
    int inner = width - 2;
    // top
    fputs(bx(BX_TL,"+"), stdout);
    for(int i=0;i<inner;i++) fputs(bx(BX_H,"-"), stdout);
    fputs(bx(BX_TR,"+"), stdout); putchar('\n');
    // middle with centered title (clock will be drawn separately)
    char title[] = "GREEN SCOPE v" GS_VERSION "  \u2014  System Monitor (Q quit, A ascii/unicode)";
    int tlen = (int)strlen(title);
    // Center on screen columns, not bytes: the em dash is 3 UTF-8 bytes but 1 column wide.
    int tcols = 0; for(int i=0;i<tlen;i++) if(((unsigned char)title[i] & 0xC0) != 0x80) tcols++;
    int left = (inner - tcols)/2; if (left < 0) left = 0;
    fputs(bx(BX_V,"|"), stdout);
    for(int i=0;i<left;i++) putchar(' ');
    fwrite(title, 1, tlen, stdout);
    for(int i=0;i<inner-left-tcols;i++) putchar(' ');
    fputs(bx(BX_V,"|"), stdout); putchar('\n');
    // bottom
    fputs(bx(BX_BL,"+"), stdout);
    for(int i=0;i<inner;i++) fputs(bx(BX_H,"-"), stdout);
    fputs(bx(BX_BR,"+"), stdout); putchar('\n');
}

static void update_clock_in_header(HANDLE h){
    SYSTEMTIME lt; GetLocalTime(&lt);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d  %02d:%02d:%02d",
             lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
    SHORT width = win_width();
    int inner = width - 2;
    int clen = (int)strlen(buf);
    int x = 1 + inner - clen; if (x < 1) x = 1;
    goto_xy(h, (SHORT)x, 1);
    printf("%s", buf);
}

static void draw_metrics(HANDLE h, int barw){
    update_clock_in_header(h);

    SHORT y = 4;
    double cpu=cpu_usage(); MemInfo m=mem_info(); DiskInfo d=disk_info(); NetRate n=net_rates();

    goto_xy(h,0,y++); printf("CPU Usage  : "); draw_bar(cpu<0?0:cpu, barw); printf(EOL);
    char t[32],a[32]; human_bytes((double)m.total,t,sizeof t); human_bytes((double)m.avail,a,sizeof a);
    goto_xy(h,0,y++); printf("Memory     : "); draw_bar(m.used_pct, barw); printf("  total %s, free %s" EOL, t,a);
    draw_gpu(h, &y, barw);
    char dt[32],df[32]; human_bytes((double)d.total,dt,sizeof dt); human_bytes((double)d.freeb,df,sizeof df);
    goto_xy(h,0,y++); printf("Disk (all) : "); draw_bar(d.used_pct, barw); printf("  total %s, free %s" EOL, dt,df);
    char rin[32], rout[32]; human_bytes(n.inBps,rin,sizeof rin); human_bytes(n.outBps,rout,sizeof rout);
    goto_xy(h,0,y++); printf("Network    :  in %s/s   out %s/s" EOL, rin, rout);
    int charging=0; int bpct=query_battery(&charging);
    goto_xy(h,0,y++); draw_battery_bar(bpct, charging);
    char up[64]; format_uptime(up, sizeof up);
    goto_xy(h,0,y++); printf("Uptime     :  %s" EOL, up);

    SHORT w = win_width();
    goto_xy(h,0,y++);
    for(int i=0;i<w;i++) fputs(bx(BX_H,"-"), stdout);
    printf("\nTip: A toggles ASCII/Unicode borders. Q quits. Refresh 1s." EOL VT_CLEAR_BELOW);
    fflush(stdout);
}

// ---- main ----
int main(){
    HANDLE h=GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE in=GetStdHandle(STD_INPUT_HANDLE);
    g_out=h; g_in=in;
    save_console();
    gpu_init(); // before the Ctrl handler, which may shut it down
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    SetConsoleTitleA("GreenScope - System Monitor");
    enable_utf8_and_vt();
    enable_key_input(in);
    set_color(h, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    hide_cursor(h);

    fputs(VT_CLEAR, stdout); // clear once
    draw_header();
    SHORT drawn_width = win_width();

    const int barw=40; cpu_usage(); net_rates();
    ULONGLONG next_tick = GetTickCount64() + 1000;
    int quit = 0;

    // Wait on console input so keys act at once (and only when this console has focus),
    // while the metrics still refresh on a steady 1 s tick.
    while(!quit){
        int redraw = 0;
        ULONGLONG now = GetTickCount64();
        DWORD wait_ms = now < next_tick ? (DWORD)(next_tick - now) : 0;
        if (WaitForSingleObject(in, wait_ms) == WAIT_OBJECT_0){
            DWORD pending = 0;
            while (!quit && GetNumberOfConsoleInputEvents(in, &pending) && pending > 0){
                INPUT_RECORD rec; DWORD got = 0;
                if (!ReadConsoleInputA(in, &rec, 1, &got) || got == 0) break;
                if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown){
                    char c = rec.Event.KeyEvent.uChar.AsciiChar;
                    if (c=='q' || c=='Q') quit = 1;
                    else if (c=='a' || c=='A'){ g_use_utf8 = !g_use_utf8; redraw = 1; }
                } else if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT){
                    redraw = 1;
                }
            }
        }
        if (quit) break;

        // Some hosts resize the window without a buffer-size event, so also check the width each tick.
        now = GetTickCount64();
        if (!redraw && now >= next_tick && win_width() != drawn_width) redraw = 1;
        if (redraw){
            fputs(VT_CLEAR, stdout);
            draw_header();
            drawn_width = win_width();
        }
        if (redraw || now >= next_tick){
            draw_metrics(h, barw);
            next_tick = now + 1000;
        }
    }
    restore_console();
    printf("GreenScope closed.\n");
    return 0;
}
