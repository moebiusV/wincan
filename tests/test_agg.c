/* test_agg.c -- the aggregation against a brute-force computation on synthetic framed files */
#define _GNU_SOURCE
#include "../src/agg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <sqlite3.h>

typedef struct { float O, H, L, C, V, J; uint32_t N; } Rec;
static int bad = 0, ok = 0;
#define CHECK(name, cond) do { if (cond) ok++; else { bad++; printf("FAIL: %s\n", name); } } while (0)

static Rec mk(long i)
{
    Rec r; float base = 100.0f + (float)(i % 977) * 0.01f + (float)(i / 3000) * 0.5f;
    r.O = base; r.C = base + ((i % 7) - 3) * 0.002f; r.H = fmaxf(r.O, r.C) + 0.003f; r.L = fminf(r.O, r.C) - 0.003f;
    r.V = (float)(1 + i % 5); r.J = 0; r.N = 1; return r;
}
static void frame(FILE *f, const Rec *r) { fputc('{', f); fwrite(r, sizeof *r, 1, f); fputc('}', f); }
static void marker(FILE *f, long long ms)
{
    Rec m; memset(&m, 0, sizeof m); uint32_t ts = (uint32_t)(ms / 1000); memcpy(&m.O, &ts, 4);
    m.V = -1.0f; m.J = __builtin_nanf(""); m.N = 0xFFFFFFFFu; frame(f, &m);
}
/* candles i in [from, to) at t0 + 100 ms each; a marker before every 600th (each whole minute from t0) when markers != 0; stray bytes at the end */
static void append(const char *p, long from, long to, long long t0, int markers, int stray)
{
    FILE *f = fopen(p, "ab");
    for (long i = from; i < to; i++) {
        if (markers && i % 600 == 0) marker(f, t0 + i * 100LL);
        Rec r = mk(i); frame(f, &r);
    }
    for (int i = 0; i < stray; i++) fputc('{', f);
    fclose(f);
}
static void set_mtime(const char *p, long long ms)
{
    struct timeval tv[2]; tv[0].tv_sec = tv[1].tv_sec = ms / 1000; tv[0].tv_usec = tv[1].tv_usec = (ms % 1000) * 1000; utimes(p, tv);
}

static void check_file(const char *label, int markers)
{
    const char *p = "/tmp/wincan-test.bin";
    unlink(p);
    long N = 3 * 3600 * 10 + 4567;
    long long t0 = 1700000000000LL + 37000;
    if (!markers) t0 = 0;
    char name[96];
    /* with markers the first candle is at t0 (a whole minute plus 37 s: markers are written every 600 candles from t0, so on the grid of t0);
     * without them the file's modification time says where the end is */
    long long end_ms = markers ? t0 + (N - 1) * 100LL : 1700000000000LL + 37000;
    append(p, 0, N, t0, markers, 11);
    set_mtime(p, end_ms + 100);
    if (!markers) t0 = end_ms + 100 - N * 100LL;                 /* the first candle's time, counted back from the end */

    Series s; CHECK("open", series_open(&s, p, 20000) == 0);
    CHECK("valid", series_valid(p) == 0 || series_valid(p) == 1);
    CHECK("tail", series_tail(&s, 50000) == 0);
    while (series_history(&s, 100000 * REC_SIZE)) ;
    snprintf(name, sizeof name, "%s: history done", label); CHECK(name, s.history_done == 1);

    Out o; long long now = end_ms + 1500;
    series_query(&s, 5, 20, now, &o);
    snprintf(name, sizeof name, "%s: 20 bars", label); CHECK(name, o.n == 20);
    long long key = (end_ms / 60000 / 5) * 5 * 60000;
    double H = -1, L = 1e9, V = 0, C = 0, O0 = 0; int got = 0;
    for (long i = 0; i < N; i++) {
        long long t = t0 + i * 100LL;
        if (t < key || t >= key + 5 * 60000) continue;
        Rec r = mk(i); if (!got) { O0 = r.O; got = 1; }
        if (r.H > H) H = r.H;
        if (r.L < L) L = r.L;
        V += r.V; C = r.C;
    }
    snprintf(name, sizeof name, "%s: last 5m start", label); CHECK(name, o.t[o.n - 1] == key / 1000);
    snprintf(name, sizeof name, "%s: last 5m open", label);  CHECK(name, fabs(o.o[o.n - 1] - O0) < 1e-4);
    snprintf(name, sizeof name, "%s: last 5m high", label);  CHECK(name, fabs(o.h[o.n - 1] - H) < 1e-4);
    snprintf(name, sizeof name, "%s: last 5m low", label);   CHECK(name, fabs(o.l[o.n - 1] - L) < 1e-4);
    snprintf(name, sizeof name, "%s: last 5m close", label); CHECK(name, fabs(o.c[o.n - 1] - C) < 1e-4);
    snprintf(name, sizeof name, "%s: last 5m volume", label); CHECK(name, fabs(o.v[o.n - 1] - V) < 1e-2);
    snprintf(name, sizeof name, "%s: price", label); CHECK(name, fabs(o.price - mk(N - 1).C) < 1e-4);
    snprintf(name, sizeof name, "%s: age", label); CHECK(name, o.age_ms >= 1400 && o.age_ms <= 1700);

    Out h1, m5; series_query(&s, 60, 2, now, &h1); series_query(&s, 5, 40, now, &m5);
    double hv = 0; int cnt = 0;
    for (int i = 0; i < m5.n; i++) if (m5.t[i] >= h1.t[0] && m5.t[i] < h1.t[0] + 3600) { hv += m5.v[i]; cnt++; }
    snprintf(name, sizeof name, "%s: 1h = twelve 5m", label); CHECK(name, cnt == 12 && fabs(hv - h1.v[0]) < 1e-1);

    /* the collector finishes its record and appends 1500 candles */
    if (truncate(p, (off_t)s.pos)) return;
    append(p, N, N + 1500, t0, markers, 3);
    set_mtime(p, end_ms + 100 + 150000);
    snprintf(name, sizeof name, "%s: refresh", label); CHECK(name, series_refresh(&s) == 0);
    series_query(&s, 1, 3, end_ms + 151000, &o);
    snprintf(name, sizeof name, "%s: three 1m bars", label); CHECK(name, o.n == 3);
    snprintf(name, sizeof name, "%s: new price", label); CHECK(name, fabs(o.price - mk(N + 1499).C) < 1e-4);
    long long lastkey = ((t0 + (N + 1499) * 100LL) / 60000) * 60;
    if (markers) { snprintf(name, sizeof name, "%s: last minute", label); CHECK(name, o.t[o.n - 1] == lastkey); }

    Out w; series_query(&s, 10080, 1, now, &w);
    CHECK("week starts on Monday", ((w.t[0] / 86400) + 3) % 7 == 0);
    Out d; series_query(&s, 1440, 1, now, &d);
    CHECK("day at midnight", d.t[0] % 86400 == 0);
    series_close(&s); unlink(p);
}

/* a collector killed in mid-write: a partial frame in the middle of the file, after which the frames sit 11 bytes later */
static void check_break(void)
{
    const char *p = "/tmp/wincan-test2.bin";
    unlink(p);
    long N = 40000; long long t0 = 1700000040000LL;
    FILE *f = fopen(p, "wb");
    for (long i = 0; i < N; i++) {
        if (i % 600 == 0) marker(f, t0 + i * 100LL);
        if (i == 15001 || i == 30002) { for (int k = 0; k < 11; k++) fputc('{', f); }       /* the break, with stray '{' bytes to confuse */
        Rec r = mk(i); frame(f, &r);
    }
    fclose(f);
    set_mtime(p, t0 + N * 100LL);
    Series s; series_open(&s, p, 20000);
    CHECK("break: tail", series_tail(&s, 5000) == 0);
    while (series_history(&s, 400000)) ;
    Out o; series_query(&s, 1, 100, t0 + N * 100LL, &o);
    CHECK("break: bars", o.n >= 60);
    /* the 1-minute bar that holds candle 17999 (minute 29, candles 17400..17999) against brute force */
    Out all; series_query(&s, 1, 500, t0 + N * 100LL, &all);
    int found = 0; double V = 0;
    for (long i = 17400; i < 18000; i++) V += mk(i).V;
    for (int i = 0; i < all.n; i++) if (all.t[i] == (t0 / 1000) + 29 * 60) { found = 1; CHECK("break: bar volume after the break", fabs(all.v[i] - V) < 1e-2); }
    CHECK("break: bar found", found);
    /* a bar before the second break (candle 9000: minute 15) */
    double V2 = 0; for (long i = 9000; i < 9600; i++) V2 += mk(i).V;
    int found2 = 0;
    for (int i = 0; i < all.n; i++) if (all.t[i] == (t0 / 1000) + 15 * 60) { found2 = 1; CHECK("break: bar volume before the breaks", fabs(all.v[i] - V2) < 1e-2); }
    CHECK("break: early bar found", found2);
    series_close(&s); unlink(p);
}

/* the perp file starts two hours into the spot file: before that the chart is spot, after it perp */
static void check_backfill(void)
{
    const char *ps = "/tmp/wincan-test-spot.bin", *pp = "/tmp/wincan-test-perp.bin";
    unlink(ps); unlink(pp);
    long long t0 = 1700000040000LL;                              /* a whole minute */
    long NS = 3 * 36000;                                         /* three hours of spot */
    FILE *f = fopen(ps, "wb");
    for (long i = 0; i < NS; i++) { if (i % 600 == 0) marker(f, t0 + i * 100LL); Rec r = mk(i); frame(f, &r); }
    fclose(f);
    set_mtime(ps, t0 + NS * 100LL);
    long NP = 36000;                                             /* an hour of perp, from the end of the second hour */
    long long tp = t0 + 7200000LL;
    f = fopen(pp, "wb");
    for (long j = 0; j < NP; j++) { if (j % 600 == 0) marker(f, tp + j * 100LL); Rec r = mk(j); r.O *= 1.001f; r.H *= 1.001f; r.L *= 1.001f; r.C *= 1.001f; frame(f, &r); }
    fclose(f);
    set_mtime(pp, tp + NP * 100LL);
    Series sp, pe; series_open(&sp, ps, 20000); series_open(&pe, pp, 20000);
    series_tail(&sp, 5000); while (series_history(&sp, 400000)) ;
    series_tail(&pe, 5000); while (series_history(&pe, 400000)) ;
    Out o; series_query2(&pe, &sp, 1, 200, tp + NP * 100LL, &o);
    CHECK("backfill: 180 minutes", o.n == 180);
    int found_s = 0, found_p = 0;
    for (int i = 0; i < o.n; i++) {
        long long minute = (o.t[i] - t0 / 1000) / 60;
        if (minute == 30) { found_s = 1; CHECK("backfill: spot before the perp starts", fabs(o.c[i] - mk(30 * 600 + 599).C) < 1e-3); }
        if (minute == 150) { found_p = 1; CHECK("backfill: perp after it starts", fabs(o.c[i] - 1.001f * mk(30 * 600 + 599).C) < 1e-2); }
    }
    CHECK("backfill: spot bar found", found_s); CHECK("backfill: perp bar found", found_p);
    Out one; series_query2(&pe, &sp, 60, 3, tp + NP * 100LL, &one);
    CHECK("backfill: the price is the perp's", fabs(one.price - 1.001f * mk(NP - 1).C) < 1e-2);
    Out only; series_query2(NULL, &sp, 60, 3, tp + NP * 100LL, &only);
    CHECK("backfill: spot alone", only.n == 3 && fabs(only.price - mk(NS - 1).C) < 1e-3);
    series_close(&sp); series_close(&pe); unlink(ps); unlink(pp);
}

/* frames that look right (brace, 28 bytes, brace) but hold garbage must not spoil the bar */
static void check_garbage(void)
{
    const char *p = "/tmp/wincan-test3.bin";
    unlink(p);
    long N = 12000; long long t0 = 1700000040000LL;
    FILE *f = fopen(p, "wb");
    for (long i = 0; i < N; i++) {
        if (i % 600 == 0) marker(f, t0 + i * 100LL);
        Rec r = mk(i);
        if (i == 700) r.H = 3.4e38f;                      /* a garbage high */
        if (i == 701) r.L = 9.18e-41f;                    /* a garbage (denormal) low */
        if (i == 702) r.V = 1.1e22f;                      /* a garbage volume */
        frame(f, &r);
    }
    fclose(f);
    set_mtime(p, t0 + N * 100LL);
    Series s; series_open(&s, p, 20000);
    series_tail(&s, 20000); while (series_history(&s, 400000)) ;
    Out o; series_query(&s, 1, 20, t0 + N * 100LL, &o);
    int ok2 = o.n > 0;
    for (int i = 0; i < o.n; i++) if (!(o.h[i] < 200 && o.l[i] > 50 && o.v[i] < 1e6)) ok2 = 0;
    CHECK("garbage: highs, lows and volumes stay believable", ok2);
    series_close(&s); unlink(p);
}

static void check_range(void)
{
    const char *p = "/tmp/wincan-test4.bin";
    unlink(p);
    long N = 36000; long long t0 = 1700000040000LL;
    FILE *f = fopen(p, "wb");
    for (long i = 0; i < N; i++) { if (i % 600 == 0) marker(f, t0 + i * 100LL); Rec r = mk(i); frame(f, &r); }
    fclose(f);
    set_mtime(p, t0 + N * 100LL);
    Series s; series_open(&s, p, 20000); series_tail(&s, 50000); while (series_history(&s, 400000)) ;
    long long since = t0 / 1000 + 20 * 60;                       /* from minute 20 on */
    Range r; series_range2(&s, NULL, since, t0 + N * 100LL, &r);
    double hi = -1, lo = 1e9; for (long i = 20 * 600; i < N; i++) { Rec x = mk(i); if (x.H > hi) hi = x.H; if (x.L < lo) lo = x.L; }
    CHECK("range: 40 minutes", r.n == 40);
    CHECK("range: high", fabs(r.high - hi) < 1e-3);
    CHECK("range: low", fabs(r.low - lo) < 1e-3);
    CHECK("range: the times are inside it", r.high_t >= since && r.low_t >= since);
    series_close(&s); unlink(p);
}

/* a minute that begins with forward-filled candles opens at its first trade, not at the previous close */
static void check_fill(void)
{
    const char *p = "/tmp/wincan-test5.bin";
    unlink(p);
    long long t0 = 1700000040000LL;
    FILE *f = fopen(p, "wb");
    marker(f, t0);
    for (int i = 0; i < 600; i++) { Rec r; r.O = r.H = r.L = r.C = 100; r.V = 1; r.J = 0; r.N = 1; if (i == 0) { r.H = 101; r.L = 99; } frame(f, &r); }      /* minute 0: real */
    for (int i = 0; i < 50; i++) { Rec r; r.O = r.H = r.L = r.C = 100; r.V = 0; r.J = 0; r.N = 0; frame(f, &r); }                                      /* minute 1 begins quiet */
    for (int i = 50; i < 600; i++) { Rec r; r.O = 105; r.H = 106; r.L = 104; r.C = 105.5f; r.V = 2; r.J = 0; r.N = 3; frame(f, &r); }
    fclose(f);
    set_mtime(p, t0 + 1200 * 100LL);
    Series s; series_open(&s, p, 20000); series_tail(&s, 5000); while (series_history(&s, 400000)) ;
    Out o; series_query(&s, 1, 5, t0 + 1200 * 100LL, &o);
    CHECK("fill: two bars", o.n == 2);
    CHECK("fill: the second minute opens at its first trade", fabs(o.o[1] - 105) < 1e-4);
    CHECK("fill: its low is not the previous close", fabs(o.l[1] - 104) < 1e-4);
    CHECK("fill: its high", fabs(o.h[1] - 106) < 1e-4);
    CHECK("fill: its close", fabs(o.c[1] - 105.5) < 1e-4);
    series_close(&s); unlink(p);
}

/* the database is authoritative except at the live edge: the last two minutes come from the collector's files, and so do the minutes the database lacks */
static void check_database(void)
{
    const char *pdb = "/tmp/wincan-test.db", *pf = "/tmp/wincan-test6.bin";
    unlink(pdb); unlink(pf);
    long long t0 = ((long long)time(NULL) / 60 - 400) * 60000LL; long M0 = (long)(t0 / 60000);        /* recent: the database is looked at over the last days only */
    sqlite3 *db; sqlite3_open(pdb, &db);
    sqlite3_exec(db, "CREATE TABLE candles (pair TEXT, ts INTEGER, open REAL, high REAL, low REAL, close REAL, volume REAL)", 0, 0, 0);
    sqlite3_exec(db, "BEGIN", 0, 0, 0);
    char sql[256];
    for (int i = 0; i < 180; i++) {
        snprintf(sql, sizeof sql, "INSERT INTO candles VALUES ('TSTUSDT', %ld, %d, %d, %d, %d, 10)", (M0 + i) * 60, 1000 + i, 1002 + i, 999 + i, 1001 + i);
        sqlite3_exec(db, sql, 0, 0, 0);
    }
    sqlite3_exec(db, "COMMIT", 0, 0, 0);
    /* the collector's file: the last 30 minutes (150..179), a price of 200 + (minute - 150) in each */
    FILE *f = fopen(pf, "wb");
    for (int i = 0; i < 18000; i++) {
        if (i % 600 == 0) marker(f, t0 + (150LL * 60000) + (long long)i * 100LL);
        Rec r; r.O = r.H = r.L = r.C = (float)(200 + i / 600); r.V = 1; r.J = 0; r.N = 1; frame(f, &r);
    }
    fclose(f);
    set_mtime(pf, t0 + 180LL * 60000);
    Series sf, sd; series_open(&sf, pf, 20000); series_tail(&sf, 20000); while (series_history(&sf, 400000)) ;
    CHECK("db: open", series_open_db(&sd, pdb, "TSTUSDT", 20000) == 0);
    CHECK("db: tail", series_tail(&sd, 0) == 0);
    while (series_history(&sd, 0)) ;
    Series *l[3] = { &sf, &sd, NULL };
    long long NOW = t0 + 179LL * 60000 + 30000;                       /* half way into minute 179: the one forming */
    Out o; series_queryN(l, 3, 1, 200, NOW, &o);
    int ok1 = 0, ok2 = 0, ok3 = 0, ok4 = 0;
    for (int i = 0; i < o.n; i++) {
        long m = (long)(o.t[i] / 60) - M0;
        if (m == 100) ok1 = fabs(o.o[i] - 1100) < 1e-3 && fabs(o.c[i] - 1101) < 1e-3;             /* old, in both: the database */
        if (m == 160) ok2 = fabs(o.o[i] - 1160) < 1e-3;                                           /* in both, not at the edge: the database */
        if (m == 178) ok3 = fabs(o.o[i] - 1178) < 1e-3;                                           /* closed: the database, though the ticks have it too */
        if (m == 179) ok4 = fabs(o.c[i] - 229) < 1e-3;
    }
    CHECK("db: an old minute from the database", ok1);
    CHECK("db: a recent minute that the database has, from the database", ok2);
    CHECK("db: a closed minute from the database, only the minute forming from the ticks", ok3 && ok4);
    /* the database lags: rows only up to minute 139: the minutes after come from the file, and where neither has any there is no bar */
    sqlite3_exec(db, "DELETE FROM candles WHERE ts >= (SELECT MIN(ts) FROM candles) + 140 * 60", 0, 0, 0);
    Series sd2; series_open_db(&sd2, pdb, "TSTUSDT", 20000); series_tail(&sd2, 0); while (series_history(&sd2, 0)) ;
    Series *l2[3] = { &sf, &sd2, NULL };
    series_queryN(l2, 3, 1, 200, NOW, &o);
    int has145 = 0, ok155 = 0, ok139 = 0;
    for (int i = 0; i < o.n; i++) {
        long m = (long)(o.t[i] / 60) - M0;
        if (m == 145) has145 = 1;
        if (m == 155) ok155 = fabs(o.c[i] - 205) < 1e-3;
        if (m == 139) ok139 = fabs(o.o[i] - 1139) < 1e-3;
    }
    CHECK("db lag: no bar where neither source has one", !has145);
    CHECK("db lag: the file fills in after the database's last row", ok155 && ok139);
    /* the database grows: a refresh picks up the new rows */
    snprintf(sql, sizeof sql, "INSERT INTO candles VALUES ('TSTUSDT', %ld, 1500, 1502, 1499, 1501, 10)", (M0 + 140) * 60);
    sqlite3_exec(db, sql, 0, 0, 0);
    sd2.last_check_ms = 0;
    CHECK("db: refresh", series_refresh(&sd2) == 0);
    series_queryN(l2, 3, 1, 200, NOW, &o);
    int ok140 = 0; for (int i = 0; i < o.n; i++) if ((long)(o.t[i] / 60) - M0 == 140) ok140 = fabs(o.o[i] - 1500) < 1e-3;
    CHECK("db: a new row is seen after a refresh", ok140);
    series_close(&sf); series_close(&sd); series_close(&sd2); sqlite3_close(db); unlink(pdb); unlink(pf);
}

/* the websocket daemon's bins: for closed minutes the exchange's own candles beat the ticks, the database beats both, and the forming minute comes from the ticks */
static void check_bins(void)
{
    const char *pdb = "/tmp/wincan-test7.db", *pf = "/tmp/wincan-test7.bin", *pb = "/tmp/wincan-test7b.bin";
    unlink(pdb); unlink(pf); unlink(pb);
    long long t0 = ((long long)time(NULL) / 60 - 400) * 60000LL; long M0 = (long)(t0 / 60000);
    sqlite3 *db; sqlite3_open(pdb, &db);
    sqlite3_exec(db, "CREATE TABLE candles (pair TEXT, ts INTEGER, open REAL, high REAL, low REAL, close REAL, volume REAL)", 0, 0, 0);
    sqlite3_exec(db, "BEGIN", 0, 0, 0);
    char sql[256];
    for (int i = 0; i < 140; i++) {                                   /* the database lags: minutes 0..139 only */
        snprintf(sql, sizeof sql, "INSERT INTO candles VALUES ('TSTUSDT', %ld, %d, %d, %d, %d, 10)", (M0 + i) * 60, 1000 + i, 1002 + i, 999 + i, 1001 + i);
        sqlite3_exec(db, sql, 0, 0, 0);
    }
    sqlite3_exec(db, "COMMIT", 0, 0, 0);
    FILE *f = fopen(pf, "wb");                                        /* ticks: minutes 150..179, a price of 200 + (minute - 150) */
    for (int i = 0; i < 18000; i++) {
        if (i % 600 == 0) marker(f, t0 + (150LL * 60000) + (long long)i * 100LL);
        Rec r; r.O = r.H = r.L = r.C = (float)(200 + i / 600); r.V = 1; r.J = 0; r.N = 1; frame(f, &r);
    }
    fclose(f);
    set_mtime(pf, t0 + 180LL * 60000);
    f = fopen(pb, "wb");                                              /* the daemon's bins: closed minutes 100..177, 3000 + i */
    for (int i = 100; i < 178; i++) {
        unsigned char rec[80]; memset(rec, 0, sizeof rec);
        int64_t ts = (int64_t)(M0 + i) * 60; double d[8] = { 3000.0 + i, 3002.0 + i, 2999.0 + i, 3001.0 + i, 5, 0, 0, 0 };
        memcpy(rec, &ts, 8); memcpy(rec + 8, d, sizeof d); fwrite(rec, 1, 80, f);
    }
    fclose(f);
    Series sf, sd, sb; series_open(&sf, pf, 20000); series_tail(&sf, 20000); while (series_history(&sf, 400000)) ;
    series_open_db(&sd, pdb, "TSTUSDT", 20000); series_tail(&sd, 0); while (series_history(&sd, 0)) ;
    CHECK("bins: open", series_open_bin(&sb, pb, 20000) == 0);
    CHECK("bins: tail", series_tail(&sb, 0) == 0);
    Series *l[4] = { &sf, &sd, &sb, NULL };
    Out o; series_queryN(l, 4, 1, 200, t0 + 179LL * 60000 + 30000, &o);
    int ok_db = 0, ok_old = 0, ok_bin = 0, ok_live = 0, ok_edge = 0;
    for (int i = 0; i < o.n; i++) {
        long m = (long)(o.t[i] / 60) - M0;
        if (m == 90)  ok_old = fabs(o.o[i] - 1090) < 1e-3;           /* only the database has it */
        if (m == 120) ok_db = fabs(o.o[i] - 1120) < 1e-3;            /* the database and the bins: the database */
        if (m == 155) ok_bin = fabs(o.o[i] - 3155) < 1e-3;           /* the bins and the ticks, not the database: the bins */
        if (m == 178) ok_live = fabs(o.o[i] - 228) < 1e-3;           /* closed, but neither the database nor the bins have it: the ticks */
        if (m == 179) ok_edge = fabs(o.c[i] - 229) < 1e-3;
    }
    CHECK("bins: a minute only the database has", ok_old);
    CHECK("bins: the database over the bins", ok_db);
    CHECK("bins: the bins over the ticks where the database lacks the minute", ok_bin);
    CHECK("bins: the ticks where nothing official has the minute, and for the minute forming", ok_live && ok_edge);
    /* the daemon appends a closed minute: a refresh reads it */
    f = fopen(pb, "ab");
    { unsigned char rec[80]; memset(rec, 0, sizeof rec); int64_t ts = (int64_t)(M0 + 178) * 60; double d[8] = { 4000, 4002, 3999, 4001, 5, 0, 0, 0 }; memcpy(rec, &ts, 8); memcpy(rec + 8, d, sizeof d); fwrite(rec, 1, 80, f); }
    fclose(f);
    CHECK("bins: refresh", series_refresh(&sb) == 0);
    int has178 = 0; Series *l2[4] = { NULL, NULL, &sb, NULL };
    series_queryN(l2, 4, 1, 200, t0 + 180LL * 60000, &o);
    for (int i = 0; i < o.n; i++) if ((long)(o.t[i] / 60) - M0 == 178) has178 = fabs(o.o[i] - 4000) < 1e-3;
    CHECK("bins: the appended minute is seen", has178);
    series_close(&sf); series_close(&sd); series_close(&sb); sqlite3_close(db); unlink(pdb); unlink(pf); unlink(pb);
}

/* a marker with milliseconds in the payload of its NaN: the candles after it begin at that millisecond */
static void marker_ms_frame(FILE *f, long long ms)
{
    Rec m; memset(&m, 0, sizeof m); uint32_t secs = (uint32_t)(ms / 1000), jb = 0x7FC00000u | (uint32_t)(ms % 1000 + 1); memcpy(&m.O, &secs, 4);
    m.V = -1.0f; memcpy(&m.J, &jb, 4); m.N = 0xFFFFFFFFu; frame(f, &m);
}
static void check_marker_ms(void)
{
    const char *p = "/tmp/wincan-test8.bin"; unlink(p);
    long long B = 1700000040000LL + 600000LL;                        /* a minute boundary */
    FILE *f = fopen(p, "wb");
    marker_ms_frame(f, B - 3000 - 700);                              /* the next candle begins at B - 3.7 s */
    for (int i = 0; i < 37; i++) { Rec r; r.O = r.H = r.L = r.C = 100; r.V = 1; r.J = 0; r.N = 1; frame(f, &r); }    /* B - 3.7 s up to B */
    for (int i = 0; i < 30; i++) { Rec r; r.O = r.H = r.L = r.C = 200; r.V = 1; r.J = 0; r.N = 1; frame(f, &r); }    /* from B */
    fclose(f);
    set_mtime(p, B + 3000);
    Series s; series_open(&s, p, 20000); series_tail(&s, 5000); while (series_history(&s, 400000)) ;
    Out o; series_queryN((Series *[]){ &s }, 1, 1, 5, B + 1000, &o);
    CHECK("marker ms: two bars", o.n == 2);
    CHECK("marker ms: the minute before closes at 100", o.n == 2 && fabs(o.c[0] - 100) < 1e-4);
    CHECK("marker ms: the minute from B opens at 200, exactly", o.n == 2 && fabs(o.o[1] - 200) < 1e-4);
    series_close(&s); unlink(p);
}

/* the minute forming is built over many refreshes: when it begins quiet (forward-filled candles) and its first trades come in a later refresh, the open is the first trade */
static void check_forming(void)
{
    const char *p = "/tmp/wincan-test10.bin"; unlink(p);
    long long t0 = ((long long)time(NULL) / 60 - 5) * 60000LL;          /* a minute boundary some minutes ago */
    FILE *f = fopen(p, "wb");
    marker(f, t0);
    for (int i = 0; i < 600; i++) { Rec r; r.O = r.H = r.L = r.C = 100; r.V = 1; r.J = 0; r.N = 1; frame(f, &r); }        /* minute 0: trades at 100 */
    for (int i = 0; i < 40; i++) { Rec r; r.O = r.H = r.L = r.C = 100; r.V = 0; r.J = 0; r.N = 0; frame(f, &r); }          /* minute 1 begins quiet: fills at 100 */
    fclose(f);
    set_mtime(p, t0 + 640 * 100LL);
    Series s; series_open(&s, p, 20000); series_tail(&s, 5000); while (series_history(&s, 400000)) ;
    Out o; series_queryN((Series *[]){ &s }, 1, 1, 5, t0 + 640 * 100LL + 100, &o);
    CHECK("forming: only fills so far, the bar is flat at the last price", o.n == 2 && fabs(o.o[1] - 100) < 1e-4 && fabs(o.h[1] - 100) < 1e-4 && fabs(o.l[1] - 100) < 1e-4);
    /* the first trades arrive in a later refresh: 105 up to 106, and a low of 104 */
    f = fopen(p, "ab");
    for (int i = 0; i < 20; i++) { Rec r; r.O = 105; r.H = 106; r.L = 104; r.C = 105.5f; r.V = 2; r.J = 0; r.N = 3; frame(f, &r); }
    fclose(f);
    set_mtime(p, t0 + 660 * 100LL);
    CHECK("forming: refresh", series_refresh(&s) == 0);
    series_queryN((Series *[]){ &s }, 1, 1, 5, t0 + 660 * 100LL + 100, &o);
    CHECK("forming: the open is the first trade, not the last price of the minute before", o.n == 2 && fabs(o.o[1] - 105) < 1e-4);
    CHECK("forming: the low is not the fill", o.n == 2 && fabs(o.l[1] - 104) < 1e-4 && fabs(o.h[1] - 106) < 1e-4);
    /* more fills, then more trades: nothing is disturbed */
    f = fopen(p, "ab");
    for (int i = 0; i < 10; i++) { Rec r; r.O = r.H = r.L = r.C = 105.5f; r.V = 0; r.J = 0; r.N = 0; frame(f, &r); }
    for (int i = 0; i < 5; i++) { Rec r; r.O = 103; r.H = 103.5f; r.L = 102.5f; r.C = 103; r.V = 1; r.J = 0; r.N = 1; frame(f, &r); }
    fclose(f);
    set_mtime(p, t0 + 675 * 100LL);
    series_refresh(&s);
    series_queryN((Series *[]){ &s }, 1, 1, 5, t0 + 675 * 100LL + 100, &o);
    CHECK("forming: the open stays, the low and the close follow", o.n == 2 && fabs(o.o[1] - 105) < 1e-4 && fabs(o.l[1] - 102.5) < 1e-4 && fabs(o.c[1] - 103) < 1e-4 && fabs(o.h[1] - 106) < 1e-4);
    series_close(&s); unlink(p);
}

int main(void)
{
    check_forming();
    check_marker_ms();
    check_bins();
    check_database();
    check_fill();
    check_range();
    check_garbage();
    check_backfill();
    check_break();
    check_file("with markers", 1);
    check_file("without markers", 0);
    CHECK("tf names", tf_minutes("1w") == 10080 && tf_minutes("4h") == 240 && tf_minutes("7m") == 0);
    CHECK("unframed file is not valid", series_valid("/etc/hostname") == 0);
    printf("%d passed, %d failed\n", ok, bad);
    return bad ? 1 : 0;
}
