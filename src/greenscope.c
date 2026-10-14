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

#define GS_VERSION "0.5.2"

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

// ---- console state: saved at startup, restored on Q, Ctrl+C or window close ----
static HANDLE g_out, g_in;
static WORD g_orig_attr = 7;
static CONSOLE_CURSOR_INFO g_orig_cursor;
static DWORD g_orig_out_mode, g_orig_in_mode;
static int g_have_attr, g_have_cursor, g_have_out_mode, g_have_in_mode;
static volatile LONG g_restored = 0;

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
    printf("\nTip: A toggles ASCII/Unicode borders. Q quits. Refresh 1s." EOL);
    fflush(stdout);
}

// ---- main ----
int main(){
    HANDLE h=GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE in=GetStdHandle(STD_INPUT_HANDLE);
    g_out=h; g_in=in;
    save_console();
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
