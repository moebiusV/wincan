/*
 * wincan -- a small, frameless, always-on-top candle chart window for Windows, fed by candled.
 *
 *   wincan [--server HOST:PORT] [--pair BTC] [--tf 5m] [--shot FILE.bmp]
 *
 * Win32 + GDI + WinHTTP, no other dependency.  See wincan(1).
 */
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "0.1.0"
#endif

#define WM_DATA     (WM_APP + 1)
#define WM_TRAY     (WM_APP + 2)
#define WM_SHOWIT   (WM_APP + 3)
#define ID_HOTKEY   1
#define ID_TRAY     1
#define IDM_SHOW    100
#define IDM_EXIT    101
#define MAX_BARS    500
#define MAX_PAIRS   512
#define BASE_W      300
#define BASE_H      175

static const char *TFS[] = { "1m", "3m", "5m", "15m", "30m", "1h", "4h", "1d", "1w" };
#define NTF 9

typedef struct {
    double entry, size;                  /* size in coins, negative for a short */
    double leverage;                     /* 0 = not given (the margin is then the whole value) */
    double realized;                     /* profit or loss already taken on this position, dollars */
    long long opened;                    /* when the position was entered (Unix seconds) */
    double hi, lo; long long hi_t, lo_t; int have_range;      /* the highest and lowest price since, and when (from the server) */
} Position;

static struct {
    HWND   hwnd;
    char   host[128]; int port;
    char   pair[24]; int tf;
    int    scale;                         /* percent: 100 = 200x200 */
    char   pairs[MAX_PAIRS][24]; int npairs;
    Position pos[MAX_PAIRS];              /* by index in pairs[] */
    /* data (under cs) */
    CRITICAL_SECTION cs;
    int    n; double t[MAX_BARS], o[MAX_BARS], h[MAX_BARS], l[MAX_BARS], c[MAX_BARS], v[MAX_BARS];
    double price, mark; double age_ms; int complete, have, failed;      /* mark: the exchange's mark price, 0 if none */
    char   data_pair[24]; int data_tf;
    struct { char pair[24]; int tf, n, complete, need_full; double t[MAX_BARS], o[MAX_BARS], h[MAX_BARS], l[MAX_BARS], c[MAX_BARS], v[MAX_BARS], price, mark, age_ms; DWORD when, full_when; char sig[24]; } *cache; int ncache;      /* every timeframe looked at is kept, so going back to one is instant */
    char   ip[64];                       /* the server's address, found once: resolving a .local name takes a third of a second */
    HANDLE wake; volatile int quit;
    /* ui */
    int    ind[4], hover;                 /* the toggles on/off: Bollinger bands, 9 MA, 200 MA, volume candles; the dot the mouse is over (-1: none) */
    int    active, dragging, editing, tracking, hidden;   /* editing: the position editor is open */
    POINT  drag_c0; RECT drag_r0;          /* a drag: where the cursor and the window were when the button went down */
    int    field_last;                    /* the box last edited: e resumes there (kept in the settings) */
    int    field, fresh[4];                /* the box being typed in (0 entry price, 1 amount, 2 leverage, 3 realized P&L); a box's first key replaces what it held */
    char   edit[4][32];
    HWND   prev;                         /* the window that had the keyboard before a click */
    char   ini[MAX_PATH], logpath[MAX_PATH];
    volatile int close_req;              /* x was pressed: the worker closes the trade at the mark */
    struct { int valid; DWORD when; double pct, usd, mfe, mae; char pair[24]; } last;     /* the trade closed last, shown for a while */
    const char *shot; int shot_done; double shot_entry, shot_size, shot_lev, shot_real; int shot_active, shot_editor, test_close;
    UINT   taskbar_created;
    HICON  icon;
} A;

/* ---------------------------------------------------------------- settings */

static void ini_path(void)
{
    char dir[MAX_PATH - 16];
    const char *ad = getenv("APPDATA");
    const char *od = getenv("WINCAN_DIR");                    /* for tests: another directory for the settings and the log */
    if (od) snprintf(dir, sizeof dir, "%s", od); else snprintf(dir, sizeof dir, "%s\\wincan", ad ? ad : ".");
    CreateDirectoryA(dir, NULL);
    snprintf(A.ini, sizeof A.ini, "%s\\wincan.ini", dir);
    snprintf(A.logpath, sizeof A.logpath, "%s\\trades.csv", dir);
}

static int pair_index(const char *p) { for (int i = 0; i < A.npairs; i++) if (!strcmp(A.pairs[i], p)) return i; return -1; }

static void ini_get_pos(const char *pair, Position *p)
{
    char buf[160] = "";
    GetPrivateProfileStringA("positions", pair, "", buf, sizeof buf, A.ini);
    memset(p, 0, sizeof *p);
    if (buf[0]) sscanf(buf, "%lf,%lf,%lld,%lf,%lf", &p->entry, &p->size, &p->opened, &p->leverage, &p->realized);
}

static void save_pos(const char *pair, const Position *p)
{
    char buf[160];
    if (p->entry > 0 && p->size != 0) snprintf(buf, sizeof buf, "%.10g,%.10g,%lld,%.10g,%.10g", p->entry, p->size, p->opened, p->leverage, p->realized);
    else buf[0] = 0;
    WritePrivateProfileStringA("positions", pair, buf[0] ? buf : NULL, A.ini);
}

static Position *cur_pos(void)
{
    static Position none;
    if (A.shot && A.shot_entry > 0) { memset(&none, 0, sizeof none); none.entry = A.shot_entry; none.size = A.shot_size; none.leverage = A.shot_lev; none.realized = A.shot_real; none.opened = 1; return &none; }
    int i = pair_index(A.pair);
    if (i < 0) { ini_get_pos(A.pair, &none); return &none; }
    return &A.pos[i];
}

static void save_all(void)
{
    if (A.shot) return;                  /* a test picture changes no settings */
    char b[192];
    WritePrivateProfileStringA("wincan", "pair", A.pair, A.ini);
    WritePrivateProfileStringA("wincan", "tf", TFS[A.tf], A.ini);
    snprintf(b, sizeof b, "%d", A.scale); WritePrivateProfileStringA("wincan", "scale", b, A.ini);
    { char ef[8]; snprintf(ef, sizeof ef, "%d", A.field_last); WritePrivateProfileStringA("wincan", "editfield", ef, A.ini); }
    if (A.ip[0]) {                                              /* the server's address: used at once next time, without a lookup of its name (only for the same server setting) */
        char sv[200]; snprintf(sv, sizeof sv, "%s:%d", A.host, A.port);
        WritePrivateProfileStringA("wincan", "serverip", A.ip, A.ini); WritePrivateProfileStringA("wincan", "serverip_for", sv, A.ini);
    }
    WritePrivateProfileStringA("wincan", "bollinger", A.ind[0] ? "1" : "0", A.ini);
    WritePrivateProfileStringA("wincan", "ma9", A.ind[1] ? "1" : "0", A.ini);
    WritePrivateProfileStringA("wincan", "ma200", A.ind[2] ? "1" : "0", A.ini);
    WritePrivateProfileStringA("wincan", "volcandles", A.ind[3] ? "1" : "0", A.ini);
    snprintf(b, sizeof b, "%s:%d", A.host, A.port); WritePrivateProfileStringA("wincan", "server", b, A.ini);
    RECT r;
    if (A.hwnd && !A.shot && GetWindowRect(A.hwnd, &r)) {
        snprintf(b, sizeof b, "%ld", r.left); WritePrivateProfileStringA("wincan", "x", b, A.ini);
        snprintf(b, sizeof b, "%ld", r.top);  WritePrivateProfileStringA("wincan", "y", b, A.ini);
    }
}

static void parse_server(const char *s)
{
    const char *c = strrchr(s, ':');
    if (c) { snprintf(A.host, sizeof A.host, "%.*s", (int)(c - s), s); A.port = atoi(c + 1); }
    else { snprintf(A.host, sizeof A.host, "%s", s); A.port = 8766; }
    if (A.port <= 0) A.port = 8766;
}

/* ---------------------------------------------------------------- the network (a worker thread) */

static void resolve(void)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    A.ip[0] = 0;
    if (getaddrinfo(A.host, NULL, &hints, &res) == 0 && res) {
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        snprintf(A.ip, sizeof A.ip, "%s", inet_ntoa(sa->sin_addr));
    }
    if (res) freeaddrinfo(res);
}

static HINTERNET g_sess, g_conn;                  /* kept open: one connection, reused for every request */
static volatile LONG g_reset;                     /* set when the machine wakes from sleep: the old connection is dead, whatever it looks like */

static void http_close(void)
{
    if (g_conn) WinHttpCloseHandle(g_conn);
    if (g_sess) WinHttpCloseHandle(g_sess);
    g_conn = g_sess = NULL;
}

static char *http_get(const char *path)
{
    wchar_t wpath[256];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 256);
    if (InterlockedExchange(&g_reset, 0)) { http_close(); A.ip[0] = 0; resolve(); }      /* after a suspend: a new connection, the name looked up anew (the network may have changed) */
    if (!g_conn) {                                  /* only at the start, or after a failure */
        wchar_t whost[160];
        MultiByteToWideChar(CP_UTF8, 0, A.ip[0] ? A.ip : A.host, -1, whost, 160);
        g_sess = WinHttpOpen(L"wincan/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
        if (!g_sess) return NULL;
        WinHttpSetTimeouts(g_sess, 10000, 3000, 3000, 5000);          /* the lookup of a .local name takes up to several seconds, now and then */
        g_conn = WinHttpConnect(g_sess, whost, (INTERNET_PORT)A.port, 0);
        if (!g_conn) { http_close(); return NULL; }
    }
    char *out = NULL; int ok = 0;
    HINTERNET r = WinHttpOpenRequest(g_conn, L"GET", wpath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &sz, NULL);
        size_t cap = 8192, len = 0; char *buf = malloc(cap);
        DWORD got = 0; ok = 1;
        while (buf && WinHttpReadData(r, buf + len, (DWORD)(cap - len - 1), &got) && got > 0) {      /* read it all: the connection is then free for the next request */
            len += got;
            if (cap - len < 4096) { cap *= 2; char *n = realloc(buf, cap); if (!n) { free(buf); buf = NULL; break; } buf = n; }
        }
        if (buf) { buf[len] = 0; if (code == 200) out = buf; else free(buf); }
    }
    if (r) WinHttpCloseHandle(r);
    if (!ok) http_close();                          /* the connection broke: start again, with the name looked up anew */
    return out;
}

/* the numbers of  "key":[ ... ]  */
static int get_arr(const char *body, const char *key, double *dst, int max)
{
    char pat[16]; snprintf(pat, sizeof pat, "\"%s\":[", key);
    const char *p = strstr(body, pat);
    if (!p) return 0;
    p += strlen(pat);
    int n = 0;
    while (*p && *p != ']' && n < max) { char *e; dst[n++] = strtod(p, &e); if (e == p) break; p = e; if (*p == ',') p++; }
    return n;
}

static double get_num(const char *body, const char *key)
{
    char pat[24]; snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(body, pat);
    return p ? strtod(p + strlen(pat), NULL) : 0;
}

static int get_str(const char *body, const char *key, char *out, size_t cap)
{
    char pat[24]; snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(body, pat);
    if (!p) return 0;
    p += strlen(pat); size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
    out[i] = 0; return 1;
}

static void fetch_pairs(void)
{
    char *b = http_get("/pairs");
    if (!b) return;
    char list[MAX_PAIRS][24]; int n = 0;
    for (const char *p = b; (p = strchr(p, '"')) != NULL && n < MAX_PAIRS; ) {
        const char *e = strchr(p + 1, '"');
        if (!e || e - p - 1 >= 24 || e == p + 1) break;
        memcpy(list[n], p + 1, (size_t)(e - p - 1)); list[n][e - p - 1] = 0; n++; p = e + 1;
    }
    free(b);
    if (n == 0) return;
    EnterCriticalSection(&A.cs);
    Position keep[MAX_PAIRS]; memcpy(keep, A.pos, sizeof keep);
    char old[MAX_PAIRS][24]; int nold = A.npairs; memcpy(old, A.pairs, sizeof old);
    memcpy(A.pairs, list, sizeof list); A.npairs = n;
    for (int i = 0; i < n; i++) {
        A.pos[i].entry = A.pos[i].size = 0;
        int found = 0;
        for (int j = 0; j < nold; j++) if (!strcmp(old[j], list[i])) { A.pos[i] = keep[j]; found = 1; break; }
        if (!found) ini_get_pos(list[i], &A.pos[i]);
    }
    LeaveCriticalSection(&A.cs);
}

static void show_cached(void);

static int find_slot(const char *pair, int tf) { for (int i = 0; i < A.ncache; i++) if (A.cache[i].tf == tf && !strcmp(A.cache[i].pair, pair)) return i; return -1; }

static void iso(long long t, char *out, size_t cap)
{
    time_t tt = (time_t)t; struct tm *g = gmtime(&tt);
    if (!g || t <= 0) { if (cap) out[0] = 0; return; }
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", g);
}

/* the best and worst of the trade as a percentage from the entry and in dollars: the highest and lowest price since entry, the entry itself and the exit counting */
/* a position's figures at a price: the profit or loss in dollars (the move times the size, then what was already realized), the margin tied up (the value divided
 * by the leverage; the whole value if no leverage is given) and the return on that margin, in per cent */
typedef struct { double unreal, total, margin, pct, pct_unreal; } Fig;
static Fig figures(const Position *p, double price)
{
    Fig f;
    f.unreal = (price - p->entry) * p->size; f.total = f.unreal + p->realized;
    double notional = p->entry * fabs(p->size);
    f.margin = p->leverage > 0 ? notional / p->leverage : notional;
    f.pct = f.margin > 0 ? f.total / f.margin * 100.0 : 0;
    f.pct_unreal = f.margin > 0 ? f.unreal / f.margin * 100.0 : 0;
    return f;
}

/* the best and the worst the trade went through, from the entry: the unrealized dollars and as a return on the margin (what was realized is not part of it, it is logged apart); the highest and lowest price since entry count, and the entry */
static void excursions(const Position *p, double hi, double lo, double *mfe, double *mae, double *mfe_usd, double *mae_usd)
{
    if (hi < p->entry) hi = p->entry;
    if (lo > p->entry) lo = p->entry;
    double best = p->size > 0 ? hi : lo, worst = p->size > 0 ? lo : hi;
    Fig fb = figures(p, best), fw = figures(p, worst);
    *mfe = fb.margin > 0 ? fb.unreal / fb.margin * 100.0 : 0; *mae = fw.margin > 0 ? fw.unreal / fw.margin * 100.0 : 0; *mfe_usd = fb.unreal; *mae_usd = fw.unreal;
}

static void parse_range(const char *b, Position *p)
{
    double hi = get_num(b, "high"), lo = get_num(b, "low"); int n = (int)get_num(b, "n");
    if (n > 0 && hi > 0 && lo > 0) { p->hi = hi; p->lo = lo; p->hi_t = (long long)get_num(b, "high_t"); p->lo_t = (long long)get_num(b, "low_t"); p->have_range = 1; }
}

/* x: close the trade of the pair on show at the mark, and log it: the exit, the profit or loss, and the best and worst the trade went through */
static void do_close(void)
{
    char pair[24]; Position p; double price = 0;
    EnterCriticalSection(&A.cs);
    snprintf(pair, sizeof pair, "%s", A.pair);
    int i = pair_index(pair), sl = find_slot(pair, A.tf);
    if (i < 0) { LeaveCriticalSection(&A.cs); return; }
    p = A.pos[i]; price = sl >= 0 ? A.cache[sl].price : A.price;
    if (sl >= 0 && A.cache[sl].mark > 0) price = A.cache[sl].mark;                 /* closed at the mark, as an exchange values a position */
    LeaveCriticalSection(&A.cs);
    if (!(p.entry > 0 && p.size != 0 && price > 0)) return;
    if (p.opened <= 0) p.opened = (long long)time(NULL);
    char path[160]; snprintf(path, sizeof path, "/range?pair=%s&since=%lld", pair, p.opened);
    char *b = http_get(path);
    if (b) { parse_range(b, &p); free(b); }
    double hi = p.have_range ? p.hi : price, lo = p.have_range ? p.lo : price;
    if (price > hi) hi = price;
    if (price < lo) lo = price;
    Fig fg = figures(&p, price);
    double usd = fg.total, pct = fg.pct;
    double mfe, mae, mfe_usd, mae_usd; excursions(&p, hi, lo, &mfe, &mae, &mfe_usd, &mae_usd);
    long long now = (long long)time(NULL);
    char t_open[32], t_close[32], t_hi[32], t_lo[32];
    iso(p.opened, t_open, sizeof t_open); iso(now, t_close, sizeof t_close); iso(p.have_range ? p.hi_t : now, t_hi, sizeof t_hi); iso(p.have_range ? p.lo_t : now, t_lo, sizeof t_lo);
    int fresh = GetFileAttributesA(A.logpath) == INVALID_FILE_ATTRIBUTES;
    FILE *f = fopen(A.logpath, "ab");
    if (f) {
        if (fresh) fprintf(f, "closed_utc,pair,side,leverage,entry,size,opened_utc,exit,unrealized_usd,realized_usd,total_usd,margin_usd,roe_pct,highest,highest_utc,lowest,lowest_utc,max_favorable_pct,max_adverse_pct,max_favorable_usd,max_adverse_usd,duration_s\r\n");
        fprintf(f, "%s,%s,%s,%.6g,%.10g,%.10g,%s,%.10g,%.2f,%.2f,%.2f,%.2f,%.4f,%.10g,%s,%.10g,%s,%.4f,%.4f,%.2f,%.2f,%lld\r\n", t_close, pair, p.size > 0 ? "long" : "short", p.leverage, p.entry, p.size, t_open,
                price, fg.unreal, p.realized, fg.total, fg.margin, fg.pct, hi, t_hi, lo, t_lo, mfe, mae, mfe_usd, mae_usd, now - p.opened);
        fclose(f);
    }
    EnterCriticalSection(&A.cs);
    i = pair_index(pair);
    if (i >= 0) { memset(&A.pos[i], 0, sizeof A.pos[i]); save_pos(pair, &A.pos[i]); }
    A.last.valid = 1; A.last.when = GetTickCount(); A.last.pct = pct; A.last.usd = usd; A.last.mfe = mfe; A.last.mae = mae; snprintf(A.last.pair, sizeof A.last.pair, "%s", pair);
    LeaveCriticalSection(&A.cs);
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    DWORD last_pairs = 0;
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    static double t[MAX_BARS], o[MAX_BARS], h[MAX_BARS], l[MAX_BARS], c[MAX_BARS], v[MAX_BARS];
    while (!A.quit) {
        if (!A.ip[0]) resolve();          /* the address is looked up only when none is known (it is kept in the settings) and after a failure */
        DWORD now = GetTickCount();
        if (A.close_req) { A.close_req = 0; do_close(); PostMessage(A.hwnd, WM_DATA, 0, 0); }
        if (last_pairs == 0 || now - last_pairs > 60000) { fetch_pairs(); last_pairs = now; PostMessage(A.hwnd, WM_DATA, 0, 0); }
        char pair[24], sig[24] = ""; int tf, bars, incremental = 0;
        EnterCriticalSection(&A.cs);
        snprintf(pair, sizeof pair, "%s", A.pair); tf = A.tf;
        bars = (BASE_W * A.scale / 100 - 8) / 4 + 205; if (bars > MAX_BARS) bars = MAX_BARS; if (bars < 10) bars = 10;      /* what is shown, and 200 more for the longest average */
        int sl = find_slot(pair, tf);
        /* once the history is whole, a second's poll asks only for the last two bars and sends the signature it has: "unchanged" costs a few bytes */
        if (sl >= 0 && A.cache[sl].n >= 2 && A.cache[sl].complete && !A.cache[sl].need_full && A.cache[sl].sig[0]
            && now - A.cache[sl].full_when < 300000 && A.cache[sl].n >= bars - 8) { incremental = 1; snprintf(sig, sizeof sig, "%s", A.cache[sl].sig); }
        LeaveCriticalSection(&A.cs);
        char path[200];
        if (incremental) snprintf(path, sizeof path, "/candles?pair=%s&tf=%s&n=2&sig=%s", pair, TFS[tf], sig);
        else snprintf(path, sizeof path, "/candles?pair=%s&tf=%s&n=%d", pair, TFS[tf], bars);
        char *b = http_get(path);
        if (!b) { resolve(); b = http_get(path); }                                /* the connection was lost (a restarted server, say) or the address changed: look the name up anew and try once more */
        EnterCriticalSection(&A.cs);
        if (b) {
            { static char saved_ip[64]; if (A.ip[0] && strcmp(saved_ip, A.ip)) { snprintf(saved_ip, sizeof saved_ip, "%s", A.ip); PostMessage(A.hwnd, WM_APP + 4, 0, 0); } }
            double price = get_num(b, "price"), mark = get_num(b, "mark"), age = get_num(b, "age_ms"); int complete = strstr(b, "\"complete\":true") != NULL;
            char nsig[24] = ""; get_str(b, "sig", nsig, sizeof nsig);
            int slot = find_slot(pair, tf);
            if (strstr(b, "\"unchanged\":true") && slot >= 0) {                      /* nothing moved: only the age and the price may have */
                A.cache[slot].price = price > 0 ? price : A.cache[slot].price; A.cache[slot].mark = mark; A.cache[slot].age_ms = age; A.cache[slot].complete = complete;
                A.failed = 0;
            } else {
                int n = get_arr(b, "t", t, MAX_BARS);
                if (n > 0 && get_arr(b, "o", o, MAX_BARS) == n && get_arr(b, "h", h, MAX_BARS) == n && get_arr(b, "l", l, MAX_BARS) == n && get_arr(b, "c", c, MAX_BARS) == n) {
                    if (get_arr(b, "v", v, MAX_BARS) != n) for (int q = 0; q < n; q++) v[q] = 0;               /* no volumes: the volume candles are then the standard ones */
                    if (slot < 0) {
                        if (A.ncache < 40) slot = A.ncache++;
                        else { slot = 0; for (int i = 1; i < A.ncache; i++) if (A.cache[i].when < A.cache[slot].when) slot = i; }       /* the oldest goes */
                        A.cache[slot].n = 0; A.cache[slot].need_full = 0;
                    }
                    snprintf(A.cache[slot].pair, sizeof A.cache[slot].pair, "%s", pair); A.cache[slot].tf = tf;
                    if (incremental && A.cache[slot].n >= 2) {
                        /* merge the last bars: a bar that is there is replaced (it settled), a newer one is added; a hole means a full fetch next time */
                        double step = (tf == 0 ? 1 : tf == 1 ? 3 : tf == 2 ? 5 : tf == 3 ? 15 : tf == 4 ? 30 : tf == 5 ? 60 : tf == 6 ? 240 : tf == 7 ? 1440 : 10080) * 60.0;
                        for (int j = 0; j < n; j++) {
                            int cn = A.cache[slot].n, k = -1;
                            for (int q = cn - 1; q >= 0 && q >= cn - 4; q--) if (A.cache[slot].t[q] == t[j]) { k = q; break; }
                            if (k < 0) {
                                if (t[j] > A.cache[slot].t[cn - 1]) {
                                    if (t[j] - A.cache[slot].t[cn - 1] > step * 1.5 + 60) { A.cache[slot].need_full = 1; continue; }
                                    if (cn >= MAX_BARS) { memmove(A.cache[slot].t, A.cache[slot].t + 1, sizeof(double) * (MAX_BARS - 1)); memmove(A.cache[slot].o, A.cache[slot].o + 1, sizeof(double) * (MAX_BARS - 1)); memmove(A.cache[slot].h, A.cache[slot].h + 1, sizeof(double) * (MAX_BARS - 1)); memmove(A.cache[slot].l, A.cache[slot].l + 1, sizeof(double) * (MAX_BARS - 1)); memmove(A.cache[slot].c, A.cache[slot].c + 1, sizeof(double) * (MAX_BARS - 1)); memmove(A.cache[slot].v, A.cache[slot].v + 1, sizeof(double) * (MAX_BARS - 1)); cn--; }
                                    k = cn; A.cache[slot].n = cn + 1;
                                } else continue;
                            }
                            A.cache[slot].t[k] = t[j]; A.cache[slot].o[k] = o[j]; A.cache[slot].h[k] = h[j]; A.cache[slot].l[k] = l[j]; A.cache[slot].c[k] = c[j]; A.cache[slot].v[k] = v[j];
                        }
                    } else {
                        memcpy(A.cache[slot].t, t, sizeof t); memcpy(A.cache[slot].o, o, sizeof o); memcpy(A.cache[slot].h, h, sizeof h); memcpy(A.cache[slot].l, l, sizeof l); memcpy(A.cache[slot].c, c, sizeof c); memcpy(A.cache[slot].v, v, sizeof v);
                        A.cache[slot].n = n; A.cache[slot].need_full = 0; A.cache[slot].full_when = GetTickCount();
                    }
                    snprintf(A.cache[slot].sig, sizeof A.cache[slot].sig, "%s", nsig);
                    A.cache[slot].complete = complete; A.cache[slot].price = price; A.cache[slot].mark = mark; A.cache[slot].age_ms = age; A.cache[slot].when = GetTickCount();
                    A.failed = 0;
                } else A.failed = 1;
            }
            if (slot >= 0 && !strcmp(A.pair, pair) && A.tf == tf) show_cached();       /* still the one asked for */
            free(b);
        } else { A.failed = 1; if (!g_conn) A.ip[0] = 0; }          /* the connection is gone: look the name up again next time */
        LeaveCriticalSection(&A.cs);
        /* while a position is open: its highest and lowest price since entry, every five seconds, asked of the server (so a window that was closed meanwhile loses nothing) */
        { static DWORD last_range; DWORD t2 = GetTickCount();
          if (t2 - last_range > 5000) {
              last_range = t2;
              EnterCriticalSection(&A.cs);
              int pi = pair_index(pair); Position pp = pi >= 0 ? A.pos[pi] : (Position){0};
              LeaveCriticalSection(&A.cs);
              if (pp.entry > 0 && pp.size != 0) {
                  if (pp.opened <= 0) { EnterCriticalSection(&A.cs); if (pi >= 0 && A.pos[pi].opened <= 0) { A.pos[pi].opened = (long long)time(NULL); save_pos(pair, &A.pos[pi]); pp.opened = A.pos[pi].opened; } LeaveCriticalSection(&A.cs); }
                  char rp[160]; snprintf(rp, sizeof rp, "/range?pair=%s&since=%lld", pair, pp.opened);
                  char *rb = http_get(rp);
                  if (rb) { EnterCriticalSection(&A.cs); pi = pair_index(pair); if (pi >= 0) parse_range(rb, &A.pos[pi]); LeaveCriticalSection(&A.cs); free(rb); }
              }
          } }
        PostMessage(A.hwnd, WM_DATA, 0, 0);
        WaitForSingleObject(A.wake, 1000);
    }
    return 0;
}

static void wake(void) { SetEvent(A.wake); }

/* ---------------------------------------------------------------- drawing */

static void fmt_price(double p, char *out, size_t cap)
{
    if (p >= 1000) snprintf(out, cap, "%.1f", p);
    else if (p >= 100) snprintf(out, cap, "%.2f", p);
    else if (p >= 1) snprintf(out, cap, "%.4f", p);
    else snprintf(out, cap, "%.6g", p);
}

static void fmt_money(double v, char *out, size_t cap)
{
    double a = fabs(v);
    snprintf(out, cap, "%s$%.2f", v < 0 ? "-" : "+", a);                   /* always dollars and cents */
}

static HFONT font(int px, int bold) { return CreateFontA(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI"); }

static void text(HDC dc, HFONT f, COLORREF col, int x, int y, const char *s, int align /* 0 left, 1 right, 2 center */)
{
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, col); SetBkMode(dc, TRANSPARENT);
    SIZE sz; GetTextExtentPoint32A(dc, s, (int)strlen(s), &sz);
    int px = align == 1 ? x - sz.cx : align == 2 ? x - sz.cx / 2 : x;
    TextOutA(dc, px, y, s, (int)strlen(s));
    SelectObject(dc, old);
}

static void rect(HDC dc, int x0, int y0, int x1, int y1, COLORREF c)
{
    RECT r = { x0, y0, x1, y1 };
    HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b);
}

#define COL_BG    RGB(16, 20, 26)
#define COL_UP    RGB(38, 166, 91)
#define COL_DOWN  RGB(224, 70, 70)
#define COL_TXT   RGB(214, 220, 226)
#define COL_DIM   RGB(120, 130, 140)
#define COL_ENTRY RGB(240, 200, 60)
#define COL_BB    RGB(170, 120, 230)
#define COL_MA9   RGB(255, 160, 40)
#define COL_MA200 RGB(70, 200, 240)
static const COLORREF IND_COL[4] = { COL_BB, COL_MA9, COL_MA200, RGB(205, 205, 215) };
static const char *IND_NAME[4] = { "Bollinger bands (20, 2)", "9-period moving average", "200-period moving average", "Volume candles (width = volume)" };

/* the centre of the dot of an indicator, and its radius, in a window W by H at the factor u */
static void dot_pos(int k, int W, int H, double u, int *cx, int *cy, int *r) { *cx = W - (int)(14 * u) - k * (int)(13 * u); *cy = H - (int)(8 * u); *r = (int)(3.5 * u + 0.5); }

static void paint(HDC dc, int W, int H)
{
    double u = W / (double)BASE_W;                         /* scale factor */
    #define U(v) ((int)((v) * u + 0.5))
    rect(dc, 0, 0, W, H, COL_BG);
    HFONT fb = font(U(13), 1), fn = font(U(11), 0), fs = font(U(9), 0), fp = font(U(15), 1);

    EnterCriticalSection(&A.cs);
    char pair[24]; snprintf(pair, sizeof pair, "%s", A.pair);
    int tf = A.tf, n = A.have && !strcmp(A.data_pair, pair) && A.data_tf == A.tf ? A.n : 0;
    int failed = A.failed, have = A.have, complete = A.complete;
    double price = A.price, age = A.age_ms;
    double pnl_price = A.mark > 0 ? A.mark : A.price;                   /* the P&L is at the mark price, which moves every second, like KuCoin's; the header shows the last trade */
    static double t[MAX_BARS], o[MAX_BARS], h[MAX_BARS], l[MAX_BARS], c[MAX_BARS], v[MAX_BARS];
    memcpy(o, A.o, sizeof o); memcpy(h, A.h, sizeof h); memcpy(l, A.l, sizeof l); memcpy(c, A.c, sizeof c); memcpy(v, A.v, sizeof v); (void)t;
    LeaveCriticalSection(&A.cs);
    Position pos = *cur_pos();
    char buf[96], buf2[160];

    /* header */
    snprintf(buf, sizeof buf, "%s  %s", pair, TFS[tf]);
    text(dc, fb, COL_TXT, U(6), U(2), buf, 0);
    if (n > 0) { fmt_price(price, buf, sizeof buf); text(dc, fp, COL_TXT, W - U(6), U(1), buf, 1); }
    int top = U(22), bot = H - U(40);
    if (!have || n == 0) {
        text(dc, fn, failed ? COL_DOWN : COL_DIM, W / 2, H / 2 - U(8), failed ? "no connection to the server" : "loading...", 2);
    } else {
        /* the chart */
        int rpad = U(12);                                          /* black space at the right, to see the newest candle move */
        int vis = (W - U(8) - rpad) / 4; if (vis < 10) vis = 10;
        int dn = n < vis ? n : vis, base = n - dn;                      /* the bars shown are base .. n-1 */
        static double ma9[MAX_BARS], ma200[MAX_BARS], bbm[MAX_BARS], bbu[MAX_BARS], bbl[MAX_BARS];
        static unsigned char ok9[MAX_BARS], ok200[MAX_BARS], okbb[MAX_BARS];
        for (int i = 0; i < n; i++) {
            ok9[i] = ok200[i] = okbb[i] = 0;
            if (i >= 8)   { double sum = 0; for (int k = i - 8; k <= i; k++) sum += c[k]; ma9[i] = sum / 9; ok9[i] = 1; }
            if (i >= 199) { double sum = 0; for (int k = i - 199; k <= i; k++) sum += c[k]; ma200[i] = sum / 200; ok200[i] = 1; }
            if (i >= 19)  { double sum = 0, sq = 0; for (int k = i - 19; k <= i; k++) sum += c[k];
                            double m = sum / 20; for (int k = i - 19; k <= i; k++) sq += (c[k] - m) * (c[k] - m);
                            double sd = sqrt(sq / 20); bbm[i] = m; bbu[i] = m + 2 * sd; bbl[i] = m - 2 * sd; okbb[i] = 1; }
        }
        double hi = h[base], lo = l[base];
        for (int i = base + 1; i < n; i++) { if (h[i] > hi) hi = h[i]; if (l[i] < lo) lo = l[i]; }
        if (A.ind[0]) for (int i = base; i < n; i++) if (okbb[i]) { if (bbu[i] > hi) hi = bbu[i]; if (bbl[i] < lo) lo = bbl[i]; }
        double range = hi - lo; if (range <= 0) range = hi * 0.001 + 1e-9;
        if (pos.entry > 0 && pos.size != 0 && pos.entry < hi + range && pos.entry > lo - range) {      /* the entry line is in the picture if near */
            if (pos.entry > hi) hi = pos.entry;
            if (pos.entry < lo) lo = pos.entry;
            range = hi - lo;
        }
        double pad = range * 0.04; hi += pad; lo -= pad; range = hi - lo;
        int cw = W - U(8) - rpad, ch = bot - top;
        #define Y(p) (top + (int)((hi - (p)) / range * ch + 0.5))
        int step = dn > 0 ? cw / dn : 4; if (step < 2) step = 2;
        int bw = step - 1; if (bw % 2 == 0) bw--; if (bw < 1) bw = 1;       /* a body of an odd width has a middle pixel for the wick */
        int x0 = U(4) + (cw - step * dn);                        /* the newest bar at the right */
        /* where each bar goes: the slots are evenly spaced; standard candles are all as wide as each other.  Volume candles are drawn as KuCoin's chart draws them, a rule
         * measured from two screenshots of its chart against the real volumes of the minutes (volume-candles.txt): no candle is thinner than standard (7 pixels there); the
         * width starts to grow at the mean volume on show and grows with the logarithm of the volume, in an odd number of pixels:
         *     width / standard = max(1, 1.10 + 1.57 * ln(volume / mean volume))        (about 7.7 + 11 ln(volume / mean) pixels where the standard is 7)
         * The wide candles overlap their neighbours (the later is drawn over the earlier). */
        static int xl[MAX_BARS], cxs[MAX_BARS], bwid[MAX_BARS];
        double vmean = 0; int vcount = 0;
        for (int i = base; i < n; i++) if (v[i] > 0) { vmean += v[i]; vcount++; }
        vmean = vcount ? vmean / vcount : 0;
        for (int i = base; i < n; i++) {
            int slot = x0 + (i - base) * step, wd = bw;
            if (A.ind[3] && vmean > 0 && v[i] > 0) {
                double f = 1.10 + 1.57 * log(v[i] / vmean);
                if (f < 1.0) f = 1.0;
                if (f > 4.0) f = 4.0;
                wd = 2 * (int)(((bw * f) - 1.0) / 2.0 + 0.5) + 1;                    /* the nearest odd number of pixels: the wick is in the middle */
                if (wd < bw) wd = bw;
            }
            bwid[i] = wd; xl[i] = slot + (bw - wd) / 2; cxs[i] = xl[i] + wd / 2;
        }
        HRGN clip = CreateRectRgn(0, top, W, bot); SelectClipRgn(dc, clip);          /* the lines stay in the chart */
        #define LINE(arr, okarr, col, style) do { HPEN pn = CreatePen(style, 1, col); HGDIOBJ op = SelectObject(dc, pn); int started = 0; \
            for (int i = base; i < n; i++) { if (!okarr[i]) { started = 0; continue; } int px = cxs[i], py = Y(arr[i]); \
                if (!started) { MoveToEx(dc, px, py, NULL); started = 1; } else LineTo(dc, px, py); } \
            SelectObject(dc, op); DeleteObject(pn); } while (0)
        if (A.ind[0]) { LINE(bbu, okbb, COL_BB, PS_SOLID); LINE(bbl, okbb, COL_BB, PS_SOLID); LINE(bbm, okbb, RGB(110, 80, 150), PS_SOLID); }
        if (A.ind[2]) LINE(ma200, ok200, COL_MA200, PS_SOLID);
        if (A.ind[1]) LINE(ma9, ok9, COL_MA9, PS_SOLID);
        SelectClipRgn(dc, NULL); DeleteObject(clip);
        for (int i = base; i < n; i++) {
            COLORREF col = c[i] >= o[i] ? COL_UP : COL_DOWN;                          /* every candle alike: the one forming is drawn as the others */
            int cx = cxs[i], x = xl[i], w = bwid[i];                                  /* the middle pixel of the body, for the wick */
            rect(dc, cx, Y(h[i]), cx + 1, Y(l[i]) + 1, col);
            int yo = Y(o[i]), yc = Y(c[i]); if (yo > yc) { int tmp = yo; yo = yc; yc = tmp; }
            rect(dc, x, yo, x + w, yc + 1 > yo + 1 ? yc + 1 : yo + 1, col);
        }
        if (pos.entry > 0 && pos.size != 0) {
            int y = Y(pos.entry);
            HPEN pen = CreatePen(PS_DOT, 1, COL_ENTRY); HGDIOBJ old = SelectObject(dc, pen);
            SetBkMode(dc, TRANSPARENT); MoveToEx(dc, U(4), y, NULL); LineTo(dc, W - U(4), y); SelectObject(dc, old); DeleteObject(pen);
        }
        /* the highest high and the lowest low of the bars on show: a dotted line across the window, and its value above (or below) it at the left, away from the candle forming at the right */
        int ihi = base, ilo = base;
        for (int i = base + 1; i < n; i++) { if (h[i] > h[ihi]) ihi = i; if (l[i] < l[ilo]) ilo = i; }
        for (int k = 0; k < 2; k++) {
            int idx = k == 0 ? ihi : ilo, yy = Y(k == 0 ? h[idx] : l[idx]), xs = U(4);
            HPEN pen = CreatePen(PS_DOT, 1, RGB(100, 112, 128)); HGDIOBJ old = SelectObject(dc, pen);
            SetBkMode(dc, TRANSPARENT); MoveToEx(dc, xs, yy, NULL); LineTo(dc, W - U(2), yy); SelectObject(dc, old); DeleteObject(pen);
            fmt_price(k == 0 ? h[idx] : l[idx], buf, sizeof buf);
            int ty = k == 0 ? yy - U(11) : yy + U(1);
            if (ty < top - U(2)) ty = yy + U(1);                                  /* no room above: below the line */
            if (ty > bot - U(9)) ty = yy - U(11);                                 /* no room below: above it */
            text(dc, fs, RGB(150, 162, 178), U(5), ty, buf, 0);
        }
        /* the line of the price now, in the colour of the candle that is forming */
        double mx = c[n - 1]; int ypl = Y(mx);
        { HPEN pen = CreatePen(PS_SOLID, 1, c[n - 1] >= o[n - 1] ? COL_UP : COL_DOWN); HGDIOBJ old = SelectObject(dc, pen); MoveToEx(dc, U(4), ypl, NULL); LineTo(dc, W - U(4), ypl); SelectObject(dc, old); DeleteObject(pen); }
        if (failed) text(dc, fs, COL_DOWN, U(5), top - U(1), "no connection: showing the last data", 0);          /* the chart stays, as it was */
        else if (age > 20000) { snprintf(buf, sizeof buf, "%d s old", (int)(age / 1000)); text(dc, fs, COL_ENTRY, U(5), top - U(1), buf, 0); }
        else if (!complete) text(dc, fs, COL_DIM, U(5), top - U(1), "history loading", 0);
    }

    /* the footer: profit and loss */
    int fy = H - U(38);
    if (A.editing) {                                             /* the position editor: a panel over the lower part of the chart; Tab goes between the boxes */
        static const char *LAB[4] = { "Entry price", "Amount (- short)", "Leverage (x)", "Realized P&L ($)" };
        int y0 = H - U(98);
        rect(dc, 0, y0, W, H, RGB(20, 25, 33));
        { HPEN pn = CreatePen(PS_SOLID, 1, RGB(70, 80, 95)); HGDIOBJ op = SelectObject(dc, pn); MoveToEx(dc, 0, y0, NULL); LineTo(dc, W, y0); SelectObject(dc, op); DeleteObject(pn); }
        for (int k = 0; k < 4; k++) {
            int ry = y0 + U(4) + k * U(19);
            text(dc, fn, k == A.field ? COL_ENTRY : COL_DIM, U(8), ry + U(1), LAB[k], 0);
            int bx0 = U(118), bx1 = W - U(10);
            rect(dc, bx0, ry, bx1, ry + U(17), k == A.field ? RGB(40, 48, 60) : RGB(28, 34, 43));
            if (k == A.field) { HPEN pn = CreatePen(PS_SOLID, 1, COL_ENTRY); HGDIOBJ op = SelectObject(dc, pn), ob = SelectObject(dc, GetStockObject(NULL_BRUSH)); Rectangle(dc, bx0, ry, bx1, ry + U(17)); SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pn); }
            snprintf(buf, sizeof buf, "%s%s", A.edit[k], k == A.field ? "_" : "");
            text(dc, fn, COL_TXT, bx0 + U(4), ry + U(1), buf, 0);
        }
        text(dc, fs, COL_DIM, U(8), y0 + U(4) + 4 * U(19) + U(2), "Tab: next   Enter or mouse out: done   Esc: cancel", 0);
    } else if (pos.entry > 0 && pos.size != 0 && n > 0) {
        Fig fg = figures(&pos, pnl_price);                       /* as KuCoin's panel: the unrealized profit and its return on the initial margin (value / leverage); the realized is shown apart */
        double usd = fg.unreal, pct = fg.pct_unreal;
        COLORREF col = usd > 0 ? COL_UP : usd < 0 ? COL_DOWN : COL_TXT;
        snprintf(buf, sizeof buf, "%+.2f%%", pct); fmt_money(usd, buf2, sizeof buf2);
        text(dc, fp, col, U(6), fy, buf, 0);
        text(dc, fp, col, W - U(6), fy, buf2, 1);
        if (A.active) text(dc, fs, COL_DIM, U(6), fy + U(22), "up/down time  left/right coin  e position  x close  q quit", 0);
        else {
            fmt_price(pos.entry, buf, sizeof buf);
            char lv[16] = "", rl[48] = "";
            if (pos.leverage > 0) snprintf(lv, sizeof lv, "  %.4gx", pos.leverage);
            if (pos.realized != 0) snprintf(rl, sizeof rl, "  real %+.2f  net %+.2f", pos.realized, fg.total);
            if (pos.have_range) { double mfe, mae, a1, a2; excursions(&pos, pos.hi > pnl_price ? pos.hi : pnl_price, pos.lo < pnl_price ? pos.lo : pnl_price, &mfe, &mae, &a1, &a2);
                snprintf(buf2, sizeof buf2, "%.6g @ %s%s%s  best %+.1f%% worst %+.1f%%", pos.size, buf, lv, rl, mfe, mae); }
            else snprintf(buf2, sizeof buf2, "%.6g @ %s%s%s", pos.size, buf, lv, rl);
            if (strlen(buf2) > 46) snprintf(buf2, sizeof buf2, "%.6g @ %s%s%s", pos.size, buf, lv, rl);       /* the dots take the right end of the line: the best and worst go if it would run into them */
            text(dc, fs, COL_DIM, U(6), fy + U(22), buf2, 0);
        }
    } else if (A.last.valid && GetTickCount() - A.last.when < 30000 && !strcmp(A.last.pair, pair)) {      /* the trade just closed */
        COLORREF col = A.last.usd > 0 ? COL_UP : A.last.usd < 0 ? COL_DOWN : COL_TXT;
        snprintf(buf, sizeof buf, "closed %+.2f%%", A.last.pct); fmt_money(A.last.usd, buf2, sizeof buf2);
        text(dc, fp, col, U(6), fy, buf, 0); text(dc, fp, col, W - U(6), fy, buf2, 1);
        snprintf(buf, sizeof buf, "best %+.2f%%  worst %+.2f%%   logged", A.last.mfe, A.last.mae);
        text(dc, fs, COL_DIM, U(6), fy + U(22), buf, 0);
    } else {
        text(dc, fn, COL_DIM, U(6), fy + U(4), "no position", 0);
        text(dc, fs, COL_DIM, U(6), fy + U(22), A.active ? "e enter a position   q quit" : "click, then e to enter a position", 0);
    }
    for (int k = 0; k < 4; k++) {                                /* the toggles: a filled dot is on, a ring is off */
        int cx, cy, r; dot_pos(k, W, H, u, &cx, &cy, &r);
        HBRUSH br = A.ind[k] ? CreateSolidBrush(IND_COL[k]) : (HBRUSH)GetStockObject(NULL_BRUSH);
        HPEN pn = CreatePen(PS_SOLID, 1, IND_COL[k]); HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pn);
        Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pn); if (A.ind[k]) DeleteObject(br);
    }
    if (A.hover >= 0 && A.hover < 4) {                           /* the label of the dot the mouse is on */
        char lab[64]; snprintf(lab, sizeof lab, "%s: %s", IND_NAME[A.hover], A.ind[A.hover] ? "on" : "off");
        int cx, cy, r; dot_pos(3, W, H, u, &cx, &cy, &r);
        rect(dc, W / 3, H - U(17), cx - U(8), H - U(2), COL_BG);
        text(dc, fs, IND_COL[A.hover], cx - U(10), H - U(16), lab, 1);
    }
    if (A.active) {                                              /* the window has the keyboard */
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 130, 200)); HGDIOBJ oldp = SelectObject(dc, pen), oldb = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 0, 0, W, H); SelectObject(dc, oldp); SelectObject(dc, oldb); DeleteObject(pen);
    }
    DeleteObject(fb); DeleteObject(fn); DeleteObject(fs); DeleteObject(fp);
    #undef U
    #undef Y
}

static int win_w(void) { return BASE_W * A.scale / 100; }
static int win_h(void) { return BASE_H * A.scale / 100; }

static int save_bmp(const char *file, int W, int H)
{
    HDC sc = GetDC(NULL), dc = CreateCompatibleDC(sc);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits; HBITMAP bm = CreateDIBSection(sc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(dc, bm);
    paint(dc, W, H);
    BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(sizeof fh + sizeof bi.bmiHeader + (size_t)W * H * 4), 0, 0, (DWORD)(sizeof fh + sizeof bi.bmiHeader) };
    FILE *f = fopen(file, "wb");
    int ok = f != NULL;
    if (f) {
        BITMAPINFOHEADER h = bi.bmiHeader; h.biHeight = H;                   /* bottom-up for the file */
        fwrite(&fh, sizeof fh, 1, f); fwrite(&h, sizeof h, 1, f);
        for (int y = H - 1; y >= 0; y--) fwrite((char *)bits + (size_t)y * W * 4, 4, (size_t)W, f);
        fclose(f);
    }
    SelectObject(dc, old); DeleteObject(bm); DeleteDC(dc); ReleaseDC(NULL, sc);
    return ok;
}

/* ---------------------------------------------------------------- the window */

static void resize_window(void)
{
    SetWindowPos(A.hwnd, HWND_TOPMOST, 0, 0, win_w(), win_h(), SWP_NOMOVE | SWP_NOACTIVATE);
    InvalidateRect(A.hwnd, NULL, FALSE);
}

static void set_passive(int passive)             /* WS_EX_NOACTIVATE: a click must not take the keyboard unless asked */
{
    LONG_PTR ex = GetWindowLongPtr(A.hwnd, GWL_EXSTYLE);
    if (passive) ex |= WS_EX_NOACTIVATE; else ex &= ~(LONG_PTR)WS_EX_NOACTIVATE;
    SetWindowLongPtr(A.hwnd, GWL_EXSTYLE, ex);
}

static void take_keyboard(void)
{
    if (A.active) return;
    HWND fg = GetForegroundWindow();
    A.prev = (fg && fg != A.hwnd) ? fg : NULL;
    set_passive(0);
    SetForegroundWindow(A.hwnd);
    SetFocus(A.hwnd);
    A.active = 1;
    InvalidateRect(A.hwnd, NULL, FALSE);
}

static void give_up_keyboard(void)
{
    if (!A.active) return;
    A.active = 0; A.editing = 0;
    set_passive(1);
    if (A.prev && IsWindow(A.prev) && IsWindowVisible(A.prev) && !IsIconic(A.prev)) SetForegroundWindow(A.prev);
    A.prev = NULL;
    InvalidateRect(A.hwnd, NULL, FALSE);
}

static void show_hide(int show)
{
    if (show) { A.hidden = 0; ShowWindow(A.hwnd, SW_SHOWNOACTIVATE); SetWindowPos(A.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); wake(); }
    else { give_up_keyboard(); A.hidden = 1; ShowWindow(A.hwnd, SW_HIDE); }
}

static HICON make_icon(void)
{
    enum { N = 16 };
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = N; bi.bmiHeader.biHeight = -N; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits; HDC sc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(sc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP mask = CreateBitmap(N, N, 1, 1, NULL);
    DWORD *px = bits;
    for (int i = 0; i < N * N; i++) px[i] = 0xFF10141A;
    struct { int x, y0, y1, by0, by1; DWORD c; } cs[] = { {3, 4, 12, 6, 11, 0xFFE04646}, {7, 2, 10, 3, 8, 0xFF26A65B}, {11, 1, 9, 2, 6, 0xFF26A65B} };
    for (int k = 0; k < 3; k++) {
        for (int y = cs[k].y0; y <= cs[k].y1; y++) px[y * N + cs[k].x] = cs[k].c;
        for (int y = cs[k].by0; y <= cs[k].by1; y++) { px[y * N + cs[k].x - 1] = cs[k].c; px[y * N + cs[k].x + 1] = cs[k].c; px[y * N + cs[k].x] = cs[k].c; }
    }
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask); ReleaseDC(NULL, sc);
    return ic;
}

static void tray_add(void)
{
    NOTIFYICONDATAA nd; memset(&nd, 0, sizeof nd);
    nd.cbSize = sizeof nd; nd.hWnd = A.hwnd; nd.uID = ID_TRAY; nd.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nd.uCallbackMessage = WM_TRAY; nd.hIcon = A.icon;
    snprintf(nd.szTip, sizeof nd.szTip, "wincan: click to show or hide");
    Shell_NotifyIconA(NIM_ADD, &nd);
}
static void tray_del(void) { NOTIFYICONDATAA nd; memset(&nd, 0, sizeof nd); nd.cbSize = sizeof nd; nd.hWnd = A.hwnd; nd.uID = ID_TRAY; Shell_NotifyIconA(NIM_DELETE, &nd); }

/* what was last fetched for the pair and timeframe now chosen (under cs), or nothing; the next fetch replaces it a moment later */
static void show_cached(void)
{
    A.have = 0; A.n = 0;
    for (int i = 0; i < A.ncache; i++) if (A.cache[i].tf == A.tf && !strcmp(A.cache[i].pair, A.pair)) {
        memcpy(A.o, A.cache[i].o, sizeof A.o); memcpy(A.h, A.cache[i].h, sizeof A.h); memcpy(A.l, A.cache[i].l, sizeof A.l); memcpy(A.c, A.cache[i].c, sizeof A.c); memcpy(A.v, A.cache[i].v, sizeof A.v);
        A.n = A.cache[i].n; A.price = A.cache[i].price; A.mark = A.cache[i].mark; A.age_ms = A.cache[i].age_ms; A.complete = A.cache[i].complete; A.have = 1;
        snprintf(A.data_pair, sizeof A.data_pair, "%s", A.pair); A.data_tf = A.tf;
        break;
    }
}

static void change_pair(int dir)
{
    EnterCriticalSection(&A.cs);
    int i = pair_index(A.pair), n = A.npairs;
    if (n > 0) { i = i < 0 ? 0 : (i + dir + n) % n; snprintf(A.pair, sizeof A.pair, "%s", A.pairs[i]); }
    show_cached();
    LeaveCriticalSection(&A.cs);
    save_all();                                              /* kept at once: a restart, or the program being ended, loses nothing */
    wake(); InvalidateRect(A.hwnd, NULL, FALSE);
}

static void change_tf(int dir)
{
    EnterCriticalSection(&A.cs);
    A.tf = (A.tf + dir + NTF) % NTF;
    show_cached();
    LeaveCriticalSection(&A.cs);
    save_all();
    wake(); InvalidateRect(A.hwnd, NULL, FALSE);
}

static void open_editor(void)
{
    Position *p = cur_pos();
    A.editing = 1; A.field = A.field_last >= 0 && A.field_last < 4 ? A.field_last : 0;     /* resume where the last edit was */
    for (int k = 0; k < 4; k++) { A.fresh[k] = 1; A.edit[k][0] = 0; }
    if (p->entry > 0) snprintf(A.edit[0], sizeof A.edit[0], "%.10g", p->entry);
    if (p->size != 0) snprintf(A.edit[1], sizeof A.edit[1], "%.10g", p->size);
    if (p->leverage > 0) snprintf(A.edit[2], sizeof A.edit[2], "%.10g", p->leverage);
    if (p->realized != 0) snprintf(A.edit[3], sizeof A.edit[3], "%.10g", p->realized);
    InvalidateRect(A.hwnd, NULL, FALSE);
}

static void commit_edit(void)                     /* Enter, or the mouse leaving: all the boxes; a box left empty keeps what it held (type 0 to clear a number) */
{
    Position *p = cur_pos();
    int had = p->entry > 0 && p->size != 0;
    if (A.edit[0][0]) { double v = strtod(A.edit[0], NULL); p->entry = v > 0 ? v : 0; }
    if (A.edit[1][0]) p->size = strtod(A.edit[1], NULL);
    if (A.edit[2][0]) { double v = strtod(A.edit[2], NULL); p->leverage = v > 0 ? v : 0; }
    if (A.edit[3][0]) p->realized = strtod(A.edit[3], NULL);
    if (!(p->entry > 0 && p->size != 0)) { memset(p, 0, sizeof *p); }
    else if (!had || p->opened <= 0) { p->opened = (long long)time(NULL); p->have_range = 0; }      /* a new position starts now; correcting one keeps its time */
    save_pos(A.pair, p);
    A.editing = 0;
    InvalidateRect(A.hwnd, NULL, FALSE);
}

static void quit_app(void) { save_all(); tray_del(); A.quit = 1; wake(); DestroyWindow(A.hwnd); }

/*
 * Snap a window rectangle that is being dragged: whenever an edge of the window comes within SNAP pixels of an edge of the screen's usable area, from any direction
 * (inside or outside), the window is moved so that the two coincide.  The window's left or right edge is matched against the screen's left and right edges, its top or
 * bottom edge against the screen's top and bottom edges, so it can sit flush inside an edge or flush just outside it; the nearest of the four wins on each axis, the
 * insides first when equal.  Nothing keeps the window on the screen: past SNAP pixels from an edge it is free, and it may be partly off the screen.
 */
/* 11 at 100% scaling (the usual snap distances are 8 to 20, 10 the commonest): 17 physical pixels on the author's 150% screen (16.5, in whole program pixels), by his choice, for the
 * mystical significance of 17.  wincan is not DPI-aware, so Windows scales every coordinate it sees by the screen's scaling: 11 here is 16.5 physical pixels at 150%, 22 at 200%: the
 * distance scales with the screen by itself (a second scaling in the code would double it). */
#define SNAP 11
static void snap_axis(LONG *lo, LONG *hi, LONG wlo, LONG whi)
{
    LONG best = SNAP + 1, shift = 0, d;
    d = *lo - wlo; if (labs(d) < best) { best = labs(d); shift = -d; }       /* the window's low edge to the screen's low edge: flush inside */
    d = *hi - whi; if (labs(d) < best) { best = labs(d); shift = -d; }       /* its high edge to the screen's high edge: flush inside */
    d = *lo - whi; if (labs(d) < best) { best = labs(d); shift = -d; }       /* its low edge to the screen's high edge: just outside, beyond it */
    d = *hi - wlo; if (labs(d) < best) { best = labs(d); shift = -d; }       /* its high edge to the screen's low edge: just outside, before it */
    if (best <= SNAP) { *lo += shift; *hi += shift; }
}
static void snap_rect(RECT *r, const RECT *work)
{
    snap_axis(&r->left, &r->right, work->left, work->right);
    snap_axis(&r->top, &r->bottom, work->top, work->bottom);
}

static int test_snap(const char *file)
{
    RECT work = { 0, 0, 1280, 760 };                      /* a screen 1280 x 800 with a taskbar of 40 at the bottom */
    struct { RECT in, out; const char *what; } T[] = {
        { { 4, 100, 304, 275 },     { 0, 100, 300, 275 },      "left edge near the screen's left edge: flush inside" },
        { { 11, 100, 311, 275 },    { 0, 100, 300, 275 },      "11 pixels from it: still snaps (the distance is 11)" },
        { { 12, 100, 312, 275 },    { 12, 100, 312, 275 },     "12 pixels from it: left alone" },
        { { 975, 100, 1275, 275 },  { 980, 100, 1280, 275 },   "right edge near the screen's right edge: flush inside" },
        { { -292, 100, 8, 275 },    { -300, 100, 0, 275 },     "from outside on the left: its right edge to the screen's left edge, parked just outside" },
        { { 1290, 100, 1590, 275 }, { 1280, 100, 1580, 275 },  "from outside on the right: its left edge to the screen's right edge" },
        { { 500, 3, 800, 178 },     { 500, 0, 800, 175 },      "top edge near the screen's top: flush inside" },
        { { 500, 580, 800, 755 },   { 500, 585, 800, 760 },    "bottom edge near the bottom of the usable area (the taskbar): flush inside" },
        { { 500, -180, 800, -5 },   { 500, -175, 800, 0 },     "from above: its bottom edge to the screen's top edge" },
        { { 500, 770, 800, 945 },   { 500, 760, 800, 935 },    "from below: its top edge to the screen's bottom edge" },
        { { -150, 100, 150, 275 },  { -150, 100, 150, 275 },   "half off the left of the screen: left alone (no clamping)" },
        { { -5, 100, 295, 275 },    { 0, 100, 300, 275 },      "just past the left edge: flush inside" },
        { { 1100, 3, 1400, 178 },   { 1100, 0, 1400, 175 },    "near the top and partly off the right: snapped at the top only" },
        { { 1200, 700, 1500, 875 }, { 1200, 700, 1500, 875 },  "far off the bottom right: left alone" },
    };
    FILE *f = fopen(file, "w"); int bad = 0;
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        RECT r = T[i].in; snap_rect(&r, &work);
        int ok = r.left == T[i].out.left && r.top == T[i].out.top && r.right == T[i].out.right && r.bottom == T[i].out.bottom;
        if (!ok) bad++;
        if (f) fprintf(f, "%s: %s  (%ld,%ld,%ld,%ld)\n", ok ? "ok  " : "FAIL", T[i].what, r.left, r.top, r.right, r.bottom);
    }
    if (f) { fprintf(f, "%s\n", bad ? "FAILED" : "ALL OK"); fclose(f); }
    return bad;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        int W = win_w(), H = win_h();
        HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, W, H); HGDIOBJ old = SelectObject(mem, bm);
        paint(mem, W, H);
        BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bm); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_DATA:
        InvalidateRect(h, NULL, FALSE);
        if (A.test_close) {                                       /* a test: close the trade once there is a price, then leave */
            static int asked; EnterCriticalSection(&A.cs); int ready = A.have && A.price > 0; LeaveCriticalSection(&A.cs);
            if (A.last.valid) { quit_app(); return 0; }
            if (ready && !asked) { asked = 1; A.close_req = 1; wake(); }
        }
        if (A.shot && !A.shot_done) {
            EnterCriticalSection(&A.cs); int ready = A.have && A.complete; LeaveCriticalSection(&A.cs);
            static int tries; if (ready || ++tries > 12) { A.shot_done = 1; save_bmp(A.shot, win_w(), win_h()); quit_app(); }
        }
        return 0;
    case WM_MOUSEACTIVATE: return A.active ? MA_ACTIVATE : MA_NOACTIVATE;
    case WM_LBUTTONDOWN: {
        int mx = (short)LOWORD(l), my = (short)HIWORD(l), W = win_w(), H = win_h(); double u = W / (double)BASE_W;
        for (int k = 0; k < 4; k++) {                                          /* a click on a dot toggles it */
            int cx, cy, r; dot_pos(k, W, H, u, &cx, &cy, &r);
            if (abs(mx - cx) <= r + 3 && abs(my - cy) <= r + 3) { A.ind[k] = !A.ind[k]; save_all(); InvalidateRect(h, NULL, FALSE); return 0; }
        }
        }
        take_keyboard();
        /* a click and drag moves the window, by a drag of its own and not Windows' move loop: the position is always the window's at the start plus how far the cursor has moved,
         * with the snap applied to that, so nothing adds up from one step to the next (the system's loop built each position on the last one, and the snapping drifted) */
        A.dragging = 1; GetCursorPos(&A.drag_c0); GetWindowRect(h, &A.drag_r0); SetCapture(h);
        return 0;
    case WM_LBUTTONUP:
        if (A.dragging) {
            A.dragging = 0; ReleaseCapture(); save_all();
            { POINT pt; RECT r; GetCursorPos(&pt); GetWindowRect(h, &r); if (!PtInRect(&r, pt)) give_up_keyboard(); }
        }
        return 0;
    case WM_CAPTURECHANGED: A.dragging = 0; return 0;
    case WM_RBUTTONUP: {
        POINT pt; GetCursorPos(&pt);
        HMENU mn = CreatePopupMenu();
        AppendMenuA(mn, MF_STRING, IDM_SHOW, "Hide to the tray");
        AppendMenuA(mn, MF_STRING, IDM_EXIT, "Exit");
        SetForegroundWindow(h);
        int cmd = TrackPopupMenu(mn, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
        DestroyMenu(mn);
        if (cmd == IDM_SHOW) show_hide(0); else if (cmd == IDM_EXIT) quit_app();
        return 0; }
    case WM_MOUSEMOVE: {
        if (A.dragging) {
            if (!(w & MK_LBUTTON)) { A.dragging = 0; ReleaseCapture(); return 0; }
            POINT pt; GetCursorPos(&pt);
            RECT r = A.drag_r0; OffsetRect(&r, pt.x - A.drag_c0.x, pt.y - A.drag_c0.y);
            MONITORINFO mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
            if (GetMonitorInfo(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) snap_rect(&r, &mi.rcWork);
            SetWindowPos(h, NULL, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        int mx = (short)LOWORD(l), my = (short)HIWORD(l), W = win_w(), H = win_h(), hv = -1; double u = W / (double)BASE_W;
        for (int k = 0; k < 4; k++) { int cx, cy, r; dot_pos(k, W, H, u, &cx, &cy, &r); if (abs(mx - cx) <= r + 3 && abs(my - cy) <= r + 3) hv = k; }
        if (hv != A.hover) { A.hover = hv; InvalidateRect(h, NULL, FALSE); }
        }
        if (!A.tracking) { TRACKMOUSEEVENT te = { sizeof te, TME_LEAVE, h, 0 }; TrackMouseEvent(&te); A.tracking = 1; }
        return 0;
    case WM_MOUSELEAVE:
        A.tracking = 0;
        if (A.hover != -1) { A.hover = -1; InvalidateRect(h, NULL, FALSE); }
        if (A.active && !A.dragging) {                                              /* the mouse out of the window: it gives the keyboard back at once, */
            if (A.editing) commit_edit();                                           /* and that, like Enter, means the position has been entered */
            give_up_keyboard();
        }
        return 0;
    case WM_KILLFOCUS: if (A.active && !A.dragging) { A.active = 0; A.editing = 0; set_passive(1); InvalidateRect(h, NULL, FALSE); } return 0;
    case WM_KEYDOWN:
        if (A.editing) {
            if (w == VK_ESCAPE) A.editing = 0;
            else if (w == VK_DOWN) { A.field = (A.field + 1) % 4; A.field_last = A.field; }
            else if (w == VK_UP) { A.field = (A.field + 3) % 4; A.field_last = A.field; }
            InvalidateRect(h, NULL, FALSE); return 0;
        }
        switch (w) {
        case VK_UP: change_tf(1); break;
        case VK_DOWN: change_tf(-1); break;
        case VK_RIGHT: change_pair(1); break;
        case VK_LEFT: change_pair(-1); break;
        case VK_ESCAPE: give_up_keyboard(); break;
        case VK_DELETE: { Position *p = cur_pos(); p->entry = p->size = 0; save_pos(A.pair, p); InvalidateRect(h, NULL, FALSE); break; }
        }
        return 0;
    case WM_CHAR:
        if (A.editing) {
            char *e = A.edit[A.field]; size_t n = strlen(e);
            if (w == '\r') { commit_edit(); return 0; }
            else if (w == '\t') { A.field = (A.field + 1) % 4; A.field_last = A.field; }
            else if (w == '\b') { A.fresh[A.field] = 0; if (n) e[n - 1] = 0; }
            else if ((w >= '0' && w <= '9') || w == '.' || w == '-' || w == ',') {
                if (A.fresh[A.field]) { e[0] = 0; n = 0; A.fresh[A.field] = 0; }      /* the first key replaces what the box held */
                if (n < sizeof A.edit[0] - 1) { e[n] = w == ',' ? '.' : (char)w; e[n + 1] = 0; }
            }
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        switch (w) {
        case 'q': case 'Q': quit_app(); break;
        case 'x': case 'X': { Position *p = cur_pos(); if (p->entry > 0 && p->size != 0) { A.close_req = 1; wake(); } break; }       /* close the trade at the mark, and log it */
        case 'e': case 'E': open_editor(); break;
        case 't': case 'T': show_hide(0); break;
        case '+': case '=': if (A.scale < 400) { A.scale += 25; resize_window(); save_all(); wake(); } break;
        case '-': if (A.scale > 75) { A.scale -= 25; resize_window(); save_all(); wake(); } break;
        }
        return 0;
    case WM_HOTKEY: show_hide(A.hidden); return 0;
    case WM_POWERBROADCAST:                                    /* the laptop suspends and resumes: the connection to the server is gone; make a new one at the next poll */
        if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND || w == PBT_APMSUSPEND) InterlockedExchange(&g_reset, 1);
        return TRUE;
    case WM_APP + 4: save_all(); return 0;                     /* the server's address worked: keep it */
    case WM_SHOWIT: show_hide(1); return 0;                    /* a second copy was started and found this one: show it */
    case WM_TRAY:
        if (l == WM_LBUTTONUP) show_hide(A.hidden);
        else if (l == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU mn = CreatePopupMenu();
            AppendMenuA(mn, MF_STRING, IDM_SHOW, A.hidden ? "Show" : "Hide to the tray");
            AppendMenuA(mn, MF_STRING, IDM_EXIT, "Exit");
            SetForegroundWindow(h);
            int cmd = TrackPopupMenu(mn, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
            DestroyMenu(mn);
            if (cmd == IDM_SHOW) show_hide(A.hidden); else if (cmd == IDM_EXIT) quit_app();
        }
        return 0;
    case WM_EXITSIZEMOVE: save_all(); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default:
        if (m == A.taskbar_created && A.taskbar_created) tray_add();
    }
    return DefWindowProc(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hp; (void)cmd; (void)show;
    InitializeCriticalSection(&A.cs);
    A.cache = calloc(40, sizeof *A.cache);
    ini_path();
    char buf[128];
    GetPrivateProfileStringA("wincan", "server", "localhost:8766", buf, sizeof buf, A.ini); parse_server(buf);
    GetPrivateProfileStringA("wincan", "pair", "BTC", A.pair, sizeof A.pair, A.ini);
    char tfs[8]; GetPrivateProfileStringA("wincan", "tf", "5m", tfs, sizeof tfs, A.ini);
    A.tf = 2; for (int i = 0; i < NTF; i++) if (!strcmp(tfs, TFS[i])) A.tf = i;
    { char ipb[64] = "", forb[200] = "", now_sv[200]; GetPrivateProfileStringA("wincan", "serverip", "", ipb, sizeof ipb, A.ini); GetPrivateProfileStringA("wincan", "serverip_for", "", forb, sizeof forb, A.ini);
      snprintf(now_sv, sizeof now_sv, "%s:%d", A.host, A.port); if (!strcmp(forb, now_sv)) snprintf(A.ip, sizeof A.ip, "%s", ipb); }
    A.field_last = GetPrivateProfileIntA("wincan", "editfield", 0, A.ini); A.ind[0] = GetPrivateProfileIntA("wincan", "bollinger", 1, A.ini); A.ind[1] = GetPrivateProfileIntA("wincan", "ma9", 1, A.ini); A.ind[2] = GetPrivateProfileIntA("wincan", "ma200", 1, A.ini); A.ind[3] = GetPrivateProfileIntA("wincan", "volcandles", 0, A.ini); A.hover = -1;
    A.scale = GetPrivateProfileIntA("wincan", "scale", 100, A.ini); if (A.scale < 75 || A.scale > 400) A.scale = 100;
    int server_given = 0, argc; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        char a[256]; WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, a, sizeof a, NULL, NULL);
        char nx[256] = ""; if (i + 1 < argc) WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, nx, sizeof nx, NULL, NULL);
        if (!strcmp(a, "--server") && i + 1 < argc) { parse_server(nx); i++; server_given = 1; A.ip[0] = 0; }
        else if (!strcmp(a, "--pair") && i + 1 < argc) { snprintf(A.pair, sizeof A.pair, "%s", nx); i++; }
        else if (!strcmp(a, "--tf") && i + 1 < argc) { for (int k = 0; k < NTF; k++) if (!strcmp(nx, TFS[k])) A.tf = k; i++; }
        else if (!strcmp(a, "--shot") && i + 1 < argc) { static char shot[260]; snprintf(shot, sizeof shot, "%s", nx); A.shot = shot; i++; }
        else if (!strcmp(a, "--shot-position") && i + 1 < argc) { sscanf(nx, "%lf,%lf,%lf,%lf", &A.shot_entry, &A.shot_size, &A.shot_lev, &A.shot_real); i++; }
        else if (!strcmp(a, "--shot-active")) A.shot_active = 1;
        else if (!strcmp(a, "--scale") && i + 1 < argc) { A.scale = atoi(nx); i++; }
        else if (!strcmp(a, "--test-close")) A.test_close = 1;
        else if (!strcmp(a, "--test-snap") && i + 1 < argc) { exit(test_snap(nx) ? 1 : 0); }
        else if (!strcmp(a, "--shot-editor")) A.shot_editor = 1;
        else if (!strcmp(a, "--shot-hover") && i + 1 < argc) { A.hover = atoi(nx); i++; }
        else if (!strcmp(a, "--shot-off") && i + 1 < argc) { A.ind[atoi(nx) % 4] = 0; i++; }
        else if (!strcmp(a, "--shot-on") && i + 1 < argc) { A.ind[atoi(nx) % 4] = 1; i++; }
        else if (!strcmp(a, "--version")) { MessageBoxA(NULL, "wincan " PACKAGE_VERSION, "wincan", MB_OK); return 0; }
    }
    if (server_given && !A.shot) save_all();                       /* the server given on the command line is kept */
    if (!A.shot && !A.test_close) {                              /* one copy only: another that finds this one says so (it shows the window) and leaves */
        HANDLE once = CreateMutexA(NULL, TRUE, "Local\\wincan-single-instance");
        if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND other = FindWindowA("wincan", NULL);
            if (other) PostMessage(other, WM_SHOWIT, 0, 0);
            return 0;
        }
    }
    A.wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    A.taskbar_created = RegisterWindowMessageA("TaskbarCreated");
    A.icon = make_icon();
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc; wc.hInstance = hi; wc.lpszClassName = "wincan"; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hIcon = A.icon;
    RegisterClassA(&wc);
    RECT wa; SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    A.active = A.shot_active;
    if (A.shot_editor) { A.editing = 1; A.field = 2; snprintf(A.edit[0], sizeof A.edit[0], "61000"); snprintf(A.edit[1], sizeof A.edit[1], "0.5"); snprintf(A.edit[2], sizeof A.edit[2], "10"); snprintf(A.edit[3], sizeof A.edit[3], "-12.5"); }
    int W = win_w(), H = win_h();
    /* the first time, the top left corner of the screen; a saved place is kept if a good part of the window can be seen on a screen (all the monitors count) */
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN), vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int x = GetPrivateProfileIntA("wincan", "x", wa.left + 8, A.ini), y = GetPrivateProfileIntA("wincan", "y", wa.top + 8, A.ini);
    int seen_w = (x + W < vx + vw ? x + W : vx + vw) - (x > vx ? x : vx), seen_h = (y + H < vy + vh ? y + H : vy + vh) - (y > vy ? y : vy);
    if (seen_w < 60 || seen_h < 40) { x = wa.left + 8; y = wa.top + 8; }          /* it may be partly off the screen, but not so far that it cannot be found */
    A.hwnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_APPWINDOW | WS_EX_NOACTIVATE, "wincan", "wincan", WS_POPUP, x, y, W, H, NULL, NULL, hi, NULL);
    if (!A.hwnd) return 1;
    tray_add();
    RegisterHotKey(A.hwnd, ID_HOTKEY, MOD_CONTROL | MOD_ALT, 'C');
    ShowWindow(A.hwnd, A.shot ? SW_HIDE : SW_SHOWNOACTIVATE);
    CreateThread(NULL, 0, worker, NULL, 0, NULL);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessage(&msg); }
    UnregisterHotKey(A.hwnd, ID_HOTKEY);
    return 0;
}
