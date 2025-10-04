// GreenScope v0.5.1 — revert to stable UI (no map), fix clock inside header, minimize flicker
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "iphlpapi.lib")

static int g_use_utf8 = 1; // toggle with 'A'

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

static void draw_bar(double pct, int w){
    if(pct<0)pct=0; if(pct>100)pct=100; int f=(int)(pct/100.0*w+0.5);
    putchar('['); for(int i=0;i<w;i++) putchar(i<f?'#':'-'); putchar(']'); printf(" %5.1f%%",pct);
}
static void draw_battery_bar(int pct, int charging){
    if (pct<0) { printf("Battery    : N/A\n"); return; }
    if (pct>100) pct=100;
    int w=20; int f=(pct*w+50)/100;
    printf("Battery    : ");
    putchar('[');
    for(int i=0;i<w;i++) putchar(i<f?'#':'-');
    putchar(']');
    printf(" %3d%% %s\n", pct, charging ? "(AC)" : "");
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
typedef struct { double inBps, outBps; } NetRate;
static NetRate net_rates(){
    static ULONGLONG pIn=0,pOut=0,pTick=0;
    ULONGLONG in=0,out=0;
    DWORD sz=0;
    if (GetIfTable(NULL,&sz,TRUE)==ERROR_INSUFFICIENT_BUFFER){
        PMIB_IFTABLE t=(PMIB_IFTABLE)malloc(sz);
        if (t && GetIfTable(t,&sz,TRUE)==NO_ERROR){
            for(DWORD i=0;i<t->dwNumEntries;i++){
                MIB_IFROW row=t->table[i];
                if(row.dwType==IF_TYPE_SOFTWARE_LOOPBACK) continue;
                if(row.dwOperStatus!=IF_OPER_STATUS_OPERATIONAL) continue;
                in  += row.dwInOctets;
                out += row.dwOutOctets;
            }
        }
        if (t) free(t);
    }
    NetRate r={0,0};
    ULONGLONG now=GetTickCount64();
    if(pTick && now!=pTick){
        double dt=(double)(now-pTick)/1000.0;
        r.inBps=(double)(in-pIn)/dt;
        r.outBps=(double)(out-pOut)/dt;
    }
    pIn=in; pOut=out; pTick=now;
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

// ---- header (draw once), clock updater (draws inside header) ----
static void draw_header(HANDLE h){
    SHORT width = win_width();
    int inner = width - 2;
    // top
    printf("%c", g_use_utf8?'┌':'+' );
    for(int i=0;i<inner;i++) printf("%c", g_use_utf8?'─':'-' );
    printf("%c\n", g_use_utf8?'┐':'+' );
    // middle with centered title (clock will be drawn separately)
    char title[] = "GREEN SCOPE v0.5.1  —  System Monitor (Q quit, A ascii/unicode)";
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
int main(){
    HANDLE h=GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTitleA("GreenScope - System Monitor");
    enable_utf8_and_vt();
    set_color(h, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    hide_cursor(h);

    system("cls"); // clear once
    draw_header(h);

    const int barw=40; cpu_usage(); net_rates();

    for(;;){
        if(GetAsyncKeyState('Q') & 0x8000) break;
        if(GetAsyncKeyState('A') & 0x0001){
            g_use_utf8 = !g_use_utf8;
            system("cls");
            draw_header(h);
        }

        update_clock_in_header(h);

        SHORT y = 4;
        double cpu=cpu_usage(); MemInfo m=mem_info(); DiskInfo d=disk_info(); NetRate n=net_rates();

        goto_xy(h,0,y++); printf("CPU Usage  : "); draw_bar(cpu<0?0:cpu, barw); printf("\n");
        char t[32],a[32]; human_bytes((double)m.total,t,sizeof t); human_bytes((double)m.avail,a,sizeof a);
        goto_xy(h,0,y++); printf("Memory     : "); draw_bar(m.used_pct, barw); printf("  total %s, free %s\n", t,a);
        char dt[32],df[32]; human_bytes((double)d.total,dt,sizeof dt); human_bytes((double)d.freeb,df,sizeof df);
        goto_xy(h,0,y++); printf("Disk (all) : "); draw_bar(d.used_pct, barw); printf("  total %s, free %s\n", dt,df);
        char rin[32], rout[32]; human_bytes(n.inBps,rin,sizeof rin); human_bytes(n.outBps,rout,sizeof rout);
        goto_xy(h,0,y++); printf("Network    :  in %s/s   out %s/s                                              \n", rin, rout);
        int charging=0; int bpct=query_battery(&charging);
        goto_xy(h,0,y++); draw_battery_bar(bpct, charging);
        char up[64]; format_uptime(up, sizeof up);
        goto_xy(h,0,y++); printf("Uptime     :  %s\n", up);

        SHORT w = win_width();
        goto_xy(h,0,y++);
        for(int i=0;i<w;i++) printf("%c", g_use_utf8?'─':'-');
        printf("\nTip: A toggles ASCII/Unicode borders. Q quits. Refresh 1s.\n");

        Sleep(1000);
    }
    set_color(h,7);
    system("cls");
    printf("GreenScope closed.\n");
    return 0;
}
