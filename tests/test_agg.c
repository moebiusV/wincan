/* test_agg.c -- the aggregation against a brute-force computation on synthetic framed files */
#define _GNU_SOURCE
#include "../src/agg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/time.h>

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
    const char *p = "/tmp/candlewin-test.bin";
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
    const char *p = "/tmp/candlewin-test2.bin";
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
    const char *ps = "/tmp/candlewin-test-spot.bin", *pp = "/tmp/candlewin-test-perp.bin";
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

int main(void)
{
    check_backfill();
    check_break();
    check_file("with markers", 1);
    check_file("without markers", 0);
    CHECK("tf names", tf_minutes("1w") == 10080 && tf_minutes("4h") == 240 && tf_minutes("7m") == 0);
    CHECK("unframed file is not valid", series_valid("/etc/hostname") == 0);
    printf("%d passed, %d failed\n", ok, bad);
    return bad ? 1 : 0;
}
