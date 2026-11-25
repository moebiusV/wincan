/*
 * candled -- a small read-only HTTP server that serves candles, as JSON, from the raw .bin files of a collector.
 *
 *   GET /pairs                              ["BTC","ETH",...]            (recently written files first)
 *   GET /heat?pair=BTC                      the modelled liquidation ladder (heat.h) from a liquidation-map database (--heat-db)
 *   GET /candles?pair=BTC&tf=5m&n=60        {"pair":..,"tf":..,"price":..,"age_ms":..,"complete":..,"t":[..],"o":[..],...}
 *
 * See candled(8).
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "agg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <stdarg.h>
#include <syslog.h>
#include <getopt.h>
#include "heat.h"
#include "candled_conf.h"
#include "tls.h"

#define MAX_PAIRS 512
static const char *heatdb = NULL;      /* the liquidation-map database: order-book bands, open interest and long/short ratios, for /heat (candled(8) has its schema) */
static const char *dir = ".", *dir2 = NULL, *dbpath = NULL, *binsdir = NULL;      /* the files of the live collector, the backfill files (spot), and the database whose candles table is authoritative */
/* the sources of a coin, in order: the live files (100 ms ticks), the database, the websocket daemon's bins (closed minutes), the backfill files */
#define NSRC 5
typedef struct { char name[24]; Series s[NSRC]; int has[NSRC]; time_t checked; volatile int64_t last_req_ms, last_refresh_ms; volatile int64_t rest_ms; volatile double mark; volatile int64_t mark_ms; } Coin;
static int rest_enabled;
static char dbnames[MAX_PAIRS][24]; static int ndbnames; static pthread_mutex_t dbnames_mu = PTHREAD_MUTEX_INITIALIZER;
static Coin coins[MAX_PAIRS];
static int ncoins;
static pthread_mutex_t reg = PTHREAD_MUTEX_INITIALIZER;

static int64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static void refresh_coin(Coin *c)
{
    for (int i = 0; i < NSRC; i++) if (c->has[i]) series_refresh(&c->s[i]);
    c->last_refresh_ms = now_ms();
}

static void coin_list(Coin *c, Series **l) { for (int i = 0; i < NSRC; i++) l[i] = c->has[i] ? &c->s[i] : NULL; }

/*
 * The upkeep of the rolling windows, apart from the requests: every loaded pair is brought up to date once a minute, and a pair that someone has asked
 * for in the last five seconds every 100 ms, so an answer to a request is only a read of memory.  (Clients poll about once a second.)
 */
static void *refresher(void *arg)
{
    (void)arg;
    for (;;) {
        struct timespec ts = { 0, 100 * 1000000L }; nanosleep(&ts, NULL);
        int64_t t = now_ms();
        for (int i = 0; i < ncoins; i++) {                      /* ncoins only grows, and a Coin is complete before it is counted */
            Coin *c = &coins[i];
            int watched = t - c->last_req_ms < 5000;
            if (watched || t - c->last_refresh_ms >= 60000) refresh_coin(c);
        }
    }
    return NULL;
}

static int valid_pair(const char *p)
{
    size_t n = strlen(p);
    if (n < 1 || n > 16) return 0;
    for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)p[i])) return 0;
    return 1;
}

static void *history_thread(void *arg)
{
    Series *s = arg;
    while (series_history(s, 4 << 20)) ;
    return NULL;
}

/* open the file NAME.bin of a directory if it is a usable candle file: the newest three days at once, the rest in a background thread */
static int open_series(Series *s, const char *d, const char *name)
{
    char path[768]; snprintf(path, sizeof path, "%s/%s.bin", d, name);
    if (!series_valid(path)) return 0;
    if (series_open(s, path, SERIES_CAP) != 0) return 0;
    if (series_tail(s, 3 * 86400 * 10) != 0) { series_close(s); return 0; }
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &at, history_thread, s);
    pthread_attr_destroy(&at);
    return 1;
}

/* open the pair of a coin in the database (BTC is BTCUSDT there): the newest three days at once, the rest in a background thread */
static int open_db_series(Series *s, const char *name)
{
    char pair[40]; snprintf(pair, sizeof pair, "%sUSDT", name);
    if (series_open_db(s, dbpath, pair, SERIES_CAP) != 0) return 0;
    if (series_tail(s, 0) != 0) { series_close(s); return 0; }
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &at, history_thread, s);
    pthread_attr_destroy(&at);
    return 1;
}

/* the file NAMEUSDT.bin of the websocket daemon's directory: its newest days at once (no history thread: the database has the history) */
static int open_bin_series(Series *s, const char *name)
{
    char path[768]; snprintf(path, sizeof path, "%s/%sUSDT.bin", binsdir, name);
    if (access(path, R_OK) != 0) return 0;
    if (series_open_bin(s, path, SERIES_CAP) != 0) return 0;
    if (series_tail(s, 0) != 0) { series_close(s); return 0; }
    return 1;
}

/* the pairs that the database has candles for recently, once at the start and then every hour (the query takes a while on a big database) */
static void *dbnames_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sqlite3 *db = NULL;
        if (sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL) == SQLITE_OK) {
            sqlite3_busy_timeout(db, 5000);
            sqlite3_stmt *st;
            if (sqlite3_prepare_v2(db, "SELECT pair FROM candles_kucoin_futures WHERE ts > ?1 GROUP BY pair ORDER BY pair", -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL) - 3 * 86400);
                char names[MAX_PAIRS][24]; int n = 0;
                while (sqlite3_step(st) == SQLITE_ROW && n < MAX_PAIRS) {
                    const char *p = (const char *)sqlite3_column_text(st, 0);
                    size_t l = p ? strlen(p) : 0;
                    if (l < 5 || l - 4 > 16 || strcmp(p + l - 4, "USDT")) continue;
                    char nm[24]; memcpy(nm, p, l - 4); nm[l - 4] = 0;
                    if (!valid_pair(nm)) continue;
                    memcpy(names[n++], nm, strlen(nm) + 1);
                }
                sqlite3_finalize(st);
                pthread_mutex_lock(&dbnames_mu); memcpy(dbnames, names, sizeof names); ndbnames = n; pthread_mutex_unlock(&dbnames_mu);
            }
            sqlite3_close(db);
        }
        sleep(3600);
    }
    return NULL;
}

/* find or create the coin of a pair; a primary file that appears later (the perp collector started after this server) is picked up within ten seconds */
static Coin *coin_get(const char *name)
{
    pthread_mutex_lock(&reg);
    Coin *c = NULL;
    for (int i = 0; i < ncoins; i++) if (!strcmp(coins[i].name, name)) { c = &coins[i]; break; }
    if (!c) {
        if (ncoins >= MAX_PAIRS) { pthread_mutex_unlock(&reg); return NULL; }
        c = &coins[ncoins];
        memset(c, 0, sizeof *c);
        snprintf(c->name, sizeof c->name, "%s", name);
        c->has[0] = open_series(&c->s[0], dir, name);
        c->has[1] = dbpath ? open_db_series(&c->s[1], name) : 0;
        c->has[2] = binsdir ? open_bin_series(&c->s[2], name) : 0;
        c->has[3] = dir2 ? open_series(&c->s[3], dir2, name) : 0;
        c->has[4] = rest_enabled && series_open_rest(&c->s[4], 2880) == 0;       /* the official candles of the last two days, put in as minutes close */
        if (!c->has[0] && !c->has[1] && !c->has[2] && !c->has[3]) { pthread_mutex_unlock(&reg); return NULL; }
        refresh_coin(c);                                          /* opening the sources took a while: bring them up to date before the first answer */
        c->checked = time(NULL); c->last_req_ms = c->last_refresh_ms = now_ms();
        __sync_synchronize();
        ncoins++;
    } else if ((!c->has[0] || (dir2 && !c->has[3])) && time(NULL) - c->checked > 10) {
        c->checked = time(NULL);
        if (!c->has[0]) c->has[0] = open_series(&c->s[0], dir, name);
        if (dir2 && !c->has[3]) c->has[3] = open_series(&c->s[3], dir2, name);
    }
    pthread_mutex_unlock(&reg);
    return c;
}

typedef struct { char n[64]; time_t t; } PairEnt;

/* ---- the exchange's official candles ---- */

struct Buf { char *p; size_t n, cap; };
static size_t buf_write(char *d, size_t sz, size_t nm, void *ud)
{
    struct Buf *b = ud; size_t add = sz * nm;
    if (b->n + add + 1 > b->cap) { b->cap = (b->n + add + 1) * 2; b->p = realloc(b->p, b->cap); if (!b->p) return 0; }
    memcpy(b->p + b->n, d, add); b->n += add; b->p[b->n] = 0;
    return add;
}

/* KuCoin Futures klines of a coin for the minutes from..to (seconds), put into its official series; returns the number of minutes taken */
static int fetch_official(Coin *c, CURL *curl, int64_t from_s, int64_t to_s, int64_t now_min)
{
    char sym[48], url[300];
    snprintf(sym, sizeof sym, "%sUSDTM", strcmp(c->name, "BTC") == 0 ? "XBT" : c->name);
    snprintf(url, sizeof url, "https://api-futures.kucoin.com/api/v1/kline/query?symbol=%s&granularity=1&from=%lld&to=%lld", sym, (long long)from_s * 1000, (long long)to_s * 1000);
    struct Buf b = { NULL, 0, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    int taken = 0;
    if (curl_easy_perform(curl) == CURLE_OK && b.p) {
        const char *p = strstr(b.p, "\"data\":[");
        if (p) {
            p += 8;
            while ((p = strchr(p, '[')) != NULL) {            /* a row: [time ms, open, high, low, close, volume, turnover] */
                p++;
                double v[7]; int k = 0;
                while (k < 7) { char *e; v[k] = strtod(p, &e); if (e == p) break; k++; p = e; while (*p == ',' || *p == ' ') p++; }
                int64_t mm = (int64_t)(v[0] / 60000.0);
                if (k >= 6 && mm < now_min) { series_put(&c->s[4], mm, v[1], v[2], v[3], v[4], v[5]); taken++; }        /* closed minutes only: the one forming is not final */
            }
        }
    }
    free(b.p);
    return taken;
}

/* the mark price (what an exchange values a position at: recomputed every second, whether or not anyone trades) of a coin, for its P&L */
static void fetch_mark(Coin *c, CURL *curl)
{
    char url[240];
    snprintf(url, sizeof url, "https://api-futures.kucoin.com/api/v1/mark-price/%sUSDTM/current", strcmp(c->name, "BTC") == 0 ? "XBT" : c->name);
    struct Buf b = { NULL, 0, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (curl_easy_perform(curl) == CURLE_OK && b.p) {
        const char *p = strstr(b.p, "\"value\":");
        if (p) { double v = strtod(p + 8, NULL); if (v > 0) { c->mark = v; c->mark_ms = now_ms(); } }
    }
    free(b.p);
}

/*
 * The exchange's official candle of a minute replaces what was built, whenever it can be had.  For a pair someone is looking at, the last three closed minutes are
 * fetched every three seconds (the minute still forming never: it is not final), and whatever the exchange has then is put in; a fetch that failed is tried again a
 * second later.  There is no timing to get right: an official candle that arrives late, or is revised, is put in when it is seen.
 */
static void *official_thread(void *arg)
{
    (void)arg;
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "candled");
    for (;;) {
        struct timespec ts = { 0, 250 * 1000000L }; nanosleep(&ts, NULL);
        int64_t t = now_ms(), cur = t / 60000;
        for (int i = 0; i < ncoins; i++) {
            Coin *c = &coins[i];
            if (!c->has[4] || t - c->last_req_ms >= 5000) continue;     /* only a pair someone is looking at */
            if (t - c->mark_ms >= 1000) fetch_mark(c, curl);            /* its mark price, every second */
            if (t - c->rest_ms >= 3000) {
                int got = fetch_official(c, curl, (cur - 3) * 60, cur * 60, cur);
                c->rest_ms = got > 0 ? t : t - 2000;                    /* nothing came: again in a second */
            }
        }
    }
    return NULL;
}

static int by_mtime_desc(const void *a, const void *b)
{
    const PairEnt *x = a, *y = b;
    return (y->t > x->t) - (y->t < x->t);
}

static void scan_dir(const char *d, PairEnt *list, int *n)
{
    DIR *dp = opendir(d);
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp)) && *n < MAX_PAIRS) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".bin") || l - 4 >= sizeof list[0].n || l - 4 > 16) continue;
        char path[768]; snprintf(path, sizeof path, "%s/%s", d, e->d_name);
        struct stat st; if (stat(path, &st) < 0 || !series_valid(path)) continue;
        if (st.st_mtime < time(NULL) - 3 * 86400) continue;           /* not written for three days: the collector is not on it */
        char nm[64]; memcpy(nm, e->d_name, l - 4); nm[l - 4] = 0;
        if (!valid_pair(nm)) continue;
        int k; for (k = 0; k < *n; k++) if (!strcmp(list[k].n, nm)) break;
        if (k < *n) { if (st.st_mtime > list[k].t) list[k].t = st.st_mtime; continue; }
        memcpy(list[*n].n, nm, strlen(nm) + 1); list[*n].t = st.st_mtime; (*n)++;
    }
    closedir(dp);
}

static int pairs_json(char *buf, size_t cap)
{
    PairEnt list[MAX_PAIRS]; int n = 0;
    scan_dir(dir, list, &n);
    if (dir2) scan_dir(dir2, list, &n);
    qsort(list, (size_t)n, sizeof list[0], by_mtime_desc);            /* the coins with live files first, the most recently written first */
    if (dbpath) {                                                       /* then the others that the database has, by name */
        pthread_mutex_lock(&dbnames_mu);
        for (int i = 0; i < ndbnames && n < MAX_PAIRS; i++) {
            int k; for (k = 0; k < n; k++) if (!strcmp(list[k].n, dbnames[i])) break;
            if (k == n) { memcpy(list[n].n, dbnames[i], strlen(dbnames[i]) + 1); list[n].t = 0; n++; }
        }
        pthread_mutex_unlock(&dbnames_mu);
    }
    int o = snprintf(buf, cap, "[");
    for (int i = 0; i < n; i++) o += snprintf(buf + o, cap - (size_t)o, "%s\"%s\"", i ? "," : "", list[i].n);
    o += snprintf(buf + o, cap - (size_t)o, "]");
    return o;
}

static void arr(char *buf, size_t cap, int *o, const char *key, const float *v, const int64_t *t, int n)
{
    *o += snprintf(buf + *o, cap - (size_t)*o, "\"%s\":[", key);
    for (int i = 0; i < n; i++) {
        if (t) *o += snprintf(buf + *o, cap - (size_t)*o, "%s%lld", i ? "," : "", (long long)t[i]);
        else *o += snprintf(buf + *o, cap - (size_t)*o, "%s%.8g", i ? "," : "", (double)v[i]);
    }
    *o += snprintf(buf + *o, cap - (size_t)*o, "]");
}

/* a signature of the last two bars (start, open, high, low, close, volume): the same as the client's means nothing has changed, as in a second without a trade */
static void signature(const Out *o, char *sig)
{
    uint64_t h = 1469598103934665603ULL;
    for (int i = o->n > 2 ? o->n - 2 : 0; i < o->n; i++) {
        uint32_t w[5]; memcpy(&w[0], &o->o[i], 4); memcpy(&w[1], &o->h[i], 4); memcpy(&w[2], &o->l[i], 4); memcpy(&w[3], &o->c[i], 4); memcpy(&w[4], &o->v[i], 4);
        uint64_t t = (uint64_t)o->t[i];
        for (int k = 0; k < 8; k++) { h ^= (t >> (8 * k)) & 0xff; h *= 1099511628211ULL; }
        for (int j = 0; j < 5; j++) for (int k = 0; k < 4; k++) { h ^= (w[j] >> (8 * k)) & 0xff; h *= 1099511628211ULL; }
    }
    snprintf(sig, 20, "%016llx", (unsigned long long)h);
}

static const char *qparam(const char *q, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = q; p && *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1; size_t i = 0;
            while (*p && *p != '&' && i + 1 < cap) out[i++] = *p++;
            out[i] = 0; return out;
        }
        p = strchr(p, '&'); if (p) p++;
    }
    return NULL;
}

/* the connection of this thread: TLS (the client has been let in by its key), or plain on a loopback address */
static __thread SSL *conn_ssl;
static ssize_t conn_read(int fd, void *b, size_t n)
{
    if (conn_ssl) { int r = SSL_read(conn_ssl, b, (int)n); return r > 0 ? r : -1; }
    return read(fd, b, n);
}
static ssize_t conn_write(int fd, const void *b, size_t n)
{
    if (conn_ssl) { int r = SSL_write(conn_ssl, b, (int)n); return r > 0 ? r : -1; }
    return write(fd, b, n);
}

static void respond(int fd, int code, const char *status, const char *body, int len)
{
    char head[256];
    int h = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nCache-Control: no-store\r\nConnection: keep-alive\r\nKeep-Alive: timeout=60\r\n\r\n", code, status, len);
    if (conn_write(fd, head, (size_t)h) < 0) return;
    for (int off = 0; off < len; ) { ssize_t w = conn_write(fd, body + off, (size_t)(len - off)); if (w <= 0) return; off += (int)w; }
}

/* GET /heat?pair=ZEC: the modelled liquidation ladder, one bucket per percentage point (0-1 .. 19-20 per cent from the price), longs below, shorts above.
 * The ladder itself is the database's (the table heat2: the cohort model over Binance's, Hyperliquid's and KuCoin's open-interest history); the pressure is
 * made here from the newest order-book bands (the liquidations up to a distance over the bid, for longs, or ask, for shorts, depth to that distance,
 * the depth between the bands' distances 1 2 5 10 20 per cent read off linearly).  Without a heat2 row the simple five-tier model of heat.h stands in. */
#define HB 20
static double depth_at(const double *cum, double d)       /* cumulative depth at d per cent, from the values at 1 2 5 10 20 */
{
    static const double X[6] = { 0, 1, 2, 5, 10, 20 };
    double y[6]; y[0] = 0; for (int i = 0; i < 5; i++) y[i + 1] = cum[i];
    for (int i = 1; i < 6; i++) if (d <= X[i]) return y[i - 1] + (d - X[i - 1]) / (X[i] - X[i - 1]) * (y[i] - y[i - 1]);
    return y[5];
}
static int parse_csv(const char *s, double *dst, int max) { int n = 0; while (s && *s && n < max) { char *e; dst[n++] = strtod(s, &e); if (e == s) break; s = *e == ',' ? e + 1 : e; } return n; }

static int heat_json(const char *pair, char *body, size_t cap, int *code)
{
    char sym[48];
    snprintf(sym, sizeof sym, "%sUSDTM", !strcmp(pair, "BTC") ? "XBT" : pair);
    sqlite3 *db = NULL; sqlite3_stmt *st = NULL; HeatIn in; HeatOut o; memset(&in, 0, sizeof in);
    double LL[HB] = { 0 }, SS[HB] = { 0 }, cog = 0, cogn = 0, price = 0, mid = 0, bcov = 0, acov = 0, mark = 0;
    long long ts_h = 0, ts_band = 0, ts_oi = 0; int have_oi = 0, have_band = 0, have_h = 0; char src[96] = "simple";
    *code = 503;
    if (!heatdb) return 0;
    if (sqlite3_open_v2(heatdb, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) { sqlite3_close(db); return 0; }
    sqlite3_busy_timeout(db, 3000);
    if (sqlite3_prepare_v2(db, "SELECT ts, oi_usd, mark FROM oi WHERE pair=?1 ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, sym, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) { ts_oi = sqlite3_column_int64(st, 0); in.oi_usd = sqlite3_column_double(st, 1); mark = sqlite3_column_double(st, 2); have_oi = 1; }
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db, "SELECT ts, global_ls FROM bn WHERE pair=?1 AND global_ls IS NOT NULL ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, sym, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) in.ls = sqlite3_column_double(st, 1);
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db, "SELECT ts, mid, bid_cov, ask_cov, b1, b2, b5, b10, b20, a1, a2, a5, a10, a20 FROM book_bands WHERE pair=?1 ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, sym, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            ts_band = sqlite3_column_int64(st, 0); mid = sqlite3_column_double(st, 1); bcov = sqlite3_column_double(st, 2); acov = sqlite3_column_double(st, 3);
            for (int i = 0; i < HEAT_N; i++) { in.bid[i] = sqlite3_column_double(st, 4 + i); in.ask[i] = sqlite3_column_double(st, 9 + i); }
            have_band = 1;
        }
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db, "SELECT ts, price, cog, cogn, liq_long, liq_short, src FROM heat2 WHERE pair=?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, sym, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            ts_h = sqlite3_column_int64(st, 0); price = sqlite3_column_double(st, 1); cog = sqlite3_column_double(st, 2); cogn = sqlite3_column_double(st, 3);
            parse_csv((const char *)sqlite3_column_text(st, 4), LL, HB); parse_csv((const char *)sqlite3_column_text(st, 5), SS, HB);
            snprintf(src, sizeof src, "%s", sqlite3_column_text(st, 6) ? (const char *)sqlite3_column_text(st, 6) : ""); have_h = 1;
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    if (!have_oi) { *code = 404; return 0; }
    long long now = (long long)time(NULL);
    if (!have_h || now - ts_h > 600) {                        /* no ladder (or an old one): the simple model, its five tiers at the bars of 1 2 5 10 20 per cent */
        heat_model(&in, &o); memset(LL, 0, sizeof LL); memset(SS, 0, sizeof SS);
        for (int i = 0; i < HEAT_N; i++) { int b = (int)HEAT_PCT[i] - 1; LL[b] = o.liq_long[i]; SS[b] = o.liq_short[i]; }
        cog = o.cog_lin; cogn = o.cog_newton; snprintf(src, sizeof src, "simple"); ts_h = ts_oi;
    }
    double PL[HB], PS[HB], cl = 0, cs = 0;
    for (int i = 0; i < HB; i++) {
        cl += LL[i]; cs += SS[i];
        double dl = depth_at(in.bid, i + 1), da = depth_at(in.ask, i + 1);
        PL[i] = dl > 0 ? cl / dl : 0; PS[i] = da > 0 ? cs / da : 0;
    }
    int n = snprintf(body, cap, "{\"pair\":\"%s\",\"sym\":\"%s\",\"src\":\"%s\",\"age_s\":%lld,\"ladder_age_s\":%lld,\"book_age_s\":%lld,\"mark\":%.8g,\"price\":%.8g,\"oi_usd\":%.6g,"
                     "\"bid_cov\":%.2f,\"ask_cov\":%.2f,\"cog\":%.4f,\"cogn\":%.4f,", pair, sym, src, now - ts_oi, now - ts_h, have_band ? now - ts_band : -1, mark, price > 0 ? price : mid, in.oi_usd, bcov, acov, cog, cogn);
    const char *nm[4] = { "L", "S", "pl", "ps" }; const double *v[4] = { LL, SS, PL, PS };
    for (int k = 0; k < 4; k++) {
        n += snprintf(body + n, cap - (size_t)n, "\"%s\":[", nm[k]);
        for (int i = 0; i < HB; i++) n += snprintf(body + n, cap - (size_t)n, "%s%.6g", i ? "," : "", v[k][i]);
        n += snprintf(body + n, cap - (size_t)n, "]%s", k < 3 ? "," : "}");
    }
    *code = 200;
    return n;
}

/* one request; returns 1 to keep the connection for the next */
static int handle(int fd, char *body, size_t bodycap, Out *out)
{
    char req[2048]; ssize_t got = 0;
    while (got < (ssize_t)sizeof req - 1) {
        ssize_t r = conn_read(fd, req + got, sizeof req - 1 - (size_t)got);
        if (r <= 0) return 0;                                        /* closed, or idle for too long */
        got += r; req[got] = 0;
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
    }
    if (got <= 0) return 0;
    if (strncmp(req, "GET ", 4)) { respond(fd, 405, "Method Not Allowed", "{\"error\":\"GET only\"}", 20); return 0; }
    char *path = req + 4, *sp = strchr(path, ' ');
    if (!sp) { respond(fd, 400, "Bad Request", "{\"error\":\"bad request\"}", 23); return 0; }
    *sp = 0;
    char *q = strchr(path, '?'); if (q) *q++ = 0;
    if (!strcmp(path, "/pairs")) { int n = pairs_json(body, bodycap); respond(fd, 200, "OK", body, n); return 1; }
    if (!strcmp(path, "/candles")) {
        char pair[40] = "", tfs[16] = "1m", ns[16] = "60", seen[24] = "";
        qparam(q, "pair", pair, sizeof pair); qparam(q, "tf", tfs, sizeof tfs); qparam(q, "n", ns, sizeof ns); qparam(q, "sig", seen, sizeof seen);
        int tf = tf_minutes(tfs), n = atoi(ns);
        if (!valid_pair(pair) || !tf || n < 1) { respond(fd, 400, "Bad Request", "{\"error\":\"pair, tf (1m 3m 5m 15m 30m 1h 4h 1d 1w) or n wrong\"}", 62); return 1; }
        if (n > MAX_BARS) n = MAX_BARS;
        Coin *c = coin_get(pair);
        if (!c) { respond(fd, 404, "Not Found", "{\"error\":\"no such pair\"}", 24); return 1; }
        { int64_t t = now_ms(); if (t - c->last_req_ms >= 5000) refresh_coin(c);       /* nobody was watching: it is up to a minute old */
          c->last_req_ms = t; }
        { Series *l[NSRC]; coin_list(c, l); series_queryN(l, NSRC, tf, n, now_ms(), out); }
        char sig[24]; signature(out, sig);
        if (seen[0] && !strcmp(seen, sig)) {                       /* nothing changed since the client's last answer: a few bytes only */
            int u = snprintf(body, bodycap, "{\"pair\":\"%s\",\"tf\":\"%s\",\"unchanged\":true,\"sig\":\"%s\",\"price\":%.8g,\"mark\":%.8g,\"age_ms\":%lld,\"complete\":%s}",
                             pair, tfs, sig, (double)out->price, (double)c->mark, (long long)out->age_ms, out->complete ? "true" : "false");
            respond(fd, 200, "OK", body, u);
            return 1;
        }
        int o = snprintf(body, bodycap, "{\"pair\":\"%s\",\"tf\":\"%s\",\"sig\":\"%s\",\"price\":%.8g,\"mark\":%.8g,\"age_ms\":%lld,\"complete\":%s,\"n\":%d,",
                         pair, tfs, sig, (double)out->price, (double)c->mark, (long long)out->age_ms, out->complete ? "true" : "false", out->n);
        arr(body, bodycap, &o, "t", NULL, out->t, out->n); o += snprintf(body + o, bodycap - (size_t)o, ",");
        arr(body, bodycap, &o, "o", out->o, NULL, out->n); o += snprintf(body + o, bodycap - (size_t)o, ",");
        arr(body, bodycap, &o, "h", out->h, NULL, out->n); o += snprintf(body + o, bodycap - (size_t)o, ",");
        arr(body, bodycap, &o, "l", out->l, NULL, out->n); o += snprintf(body + o, bodycap - (size_t)o, ",");
        arr(body, bodycap, &o, "c", out->c, NULL, out->n); o += snprintf(body + o, bodycap - (size_t)o, ",");
        arr(body, bodycap, &o, "v", out->v, NULL, out->n); o += snprintf(body + o, bodycap - (size_t)o, "}");
        respond(fd, 200, "OK", body, o);
        return 1;
    }
    if (!strcmp(path, "/heat")) {
        char pair[40] = ""; int code = 0;
        qparam(q, "pair", pair, sizeof pair);
        if (!valid_pair(pair)) { respond(fd, 400, "Bad Request", "{\"error\":\"pair wrong\"}", 23); return 1; }
        int n = heat_json(pair, body, bodycap, &code);
        if (code == 200) respond(fd, 200, "OK", body, n);
        else if (code == 404) respond(fd, 404, "Not Found", "{\"error\":\"no heat data for this pair\"}", 36);
        else respond(fd, 503, "Service Unavailable", "{\"error\":\"heat not configured or unreadable\"}", 43);
        return 1;
    }
    if (!strcmp(path, "/range")) {
        char pair[40] = "", ss[24] = "0";
        qparam(q, "pair", pair, sizeof pair); qparam(q, "since", ss, sizeof ss);
        if (!valid_pair(pair)) { respond(fd, 400, "Bad Request", "{\"error\":\"pair wrong\"}", 23); return 1; }
        Coin *c = coin_get(pair);
        if (!c) { respond(fd, 404, "Not Found", "{\"error\":\"no such pair\"}", 24); return 1; }
        { int64_t t = now_ms(); if (t - c->last_req_ms >= 5000) refresh_coin(c); c->last_req_ms = t; }
        Range r; { Series *l[NSRC]; coin_list(c, l); series_rangeN(l, NSRC, atoll(ss), now_ms(), &r); }
        int o = snprintf(body, bodycap, "{\"pair\":\"%s\",\"since\":%lld,\"n\":%d,\"high\":%.8g,\"high_t\":%lld,\"low\":%.8g,\"low_t\":%lld,\"price\":%.8g}",
                         pair, (long long)atoll(ss), r.n, r.high, (long long)r.high_t, r.low, (long long)r.low_t, (double)r.price);
        respond(fd, 200, "OK", body, o);
        return 1;
    }
    respond(fd, 404, "Not Found", "{\"error\":\"not found\"}", 21);
    return 1;
}

static volatile int nconn;
static pthread_mutex_t nconn_mu = PTHREAD_MUTEX_INITIALIZER;
static int use_tls, daemonized;

/* a line of the log: on standard error, or in the system log once the program is a daemon */
static void logmsg(const char *fmt, ...)
{
    char b[512]; va_list ap;
    va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (daemonized) syslog(LOG_INFO, "%s", b); else fprintf(stderr, "candled: %s\n", b);
}

/* a connection stays open for as many requests as the client sends, until it closes or is idle for a minute */
static void *serve(void *arg)
{
    int fd = (int)(intptr_t)arg;
    struct timeval tv = { 60, 0 }, hs = { 10, 0 };
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    conn_ssl = NULL;
    if (use_tls) {                                       /* the handshake, then the client's key must be one that is let in */
        char fp[TLS_FP_LEN]; const char *why = "";
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &hs, sizeof hs); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &hs, sizeof hs);
        conn_ssl = tls_accept(fd, fp, &why);
        if (!conn_ssl) { logmsg("refused a connection: %s", why); goto done; }
        if (!tls_authz_check(fp)) {
            logmsg("refused a client whose key is not authorized: %s  (to let it in: candled --authorize %s)", fp, fp);
            goto done;
        }
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char *body = malloc(64 * 1024); Out *out = malloc(sizeof *out);
    if (body && out) while (handle(fd, body, 64 * 1024, out)) ;
    free(body); free(out);
done:
    if (conn_ssl) { SSL_shutdown(conn_ssl); SSL_free(conn_ssl); conn_ssl = NULL; }
    close(fd);
    pthread_mutex_lock(&nconn_mu); nconn--; pthread_mutex_unlock(&nconn_mu);
    return NULL;
}

#ifndef SYSCONFDIR
#define SYSCONFDIR "/etc"
#endif
#ifndef STATEDIR
#define STATEDIR "/var/lib/candled"
#endif
#ifndef PACKAGE_URL
#define PACKAGE_URL "https://github.com/moebiusV/wincan"
#endif

static const char *pidfile_path;
static void on_term(int sig) { (void)sig; if (pidfile_path) unlink(pidfile_path); _exit(0); }

static void usage(FILE *f)
{
    fputs(
"Usage: candled [OPTION]...\n"
"Serve candles, as JSON, from a collector's files, to wincan and other clients.\n"
"\n"
"The settings come from the configuration file (candled.conf(5)); an option overrides it.\n"
"  -c, --config=FILE        the configuration file (default " SYSCONFDIR "/candled.conf, if there is one)\n"
"  -t, --check-config       read the configuration, say whether it is good, and exit\n"
"      --dir=DIR            the directory of the collector's .bin files\n"
"      --backfill=DIR       a second directory of .bin files, for the time before --dir has data\n"
"      --db=FILE            a SQLite database of one-minute candles\n"
"      --live-bins=DIR      a websocket daemon's directory of one-minute candles\n"
"      --heat-db=FILE       the liquidation-map database for /heat\n"
"      --official-klines    fetch the exchange's own candles and the mark price\n"
"      --bind=ADDRESS       the IPv4 address to listen on (default 127.0.0.1)\n"
"      --port=N             the TCP port (default 8766)\n"
"      --no-tls             plain HTTP (only on a loopback address, unless --insecure)\n"
"      --insecure           allow plain HTTP on any address\n"
"      --certificate=FILE   the server's certificate (PEM)\n"
"      --key=FILE           the server's private key (PEM)\n"
"      --authorized=FILE    the file of the fingerprints of the clients that are let in\n"
"  -D, --daemon             run in the background, logging to the system log\n"
"  -P, --pidfile=FILE       write the process id to FILE\n"
"\n"
"Keys (see candled(8)):\n"
"      --gen-cert           make the server's key and certificate, and print the fingerprint\n"
"      --force              with --gen-cert, replace existing files\n"
"      --fingerprint        print the fingerprint of the server's certificate\n"
"      --authorize=FINGERPRINT   let in the client whose certificate has this SHA-256 fingerprint\n"
"      --comment=TEXT       with --authorize, a note for the file\n"
"\n"
"  -h, --help               display this help and exit\n"
"  -V, --version            output version information and exit\n"
"\n"
"Report bugs at " PACKAGE_URL "/issues\n", f);
}

static void version(void)
{
    puts("candled (wincan) " PACKAGE_VERSION "\n"
         "Copyright (C) 2026 David Walther\n"
         "License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>.\n"
         "This is free software: you are free to change and redistribute it.\n"
         "There is NO WARRANTY, to the extent permitted by law.");
}

static int is_loopback(const char *a) { return !strncmp(a, "127.", 4); }
static char *joined(const char *dir_, const char *name) { size_t n = strlen(dir_) + strlen(name) + 2; char *r = malloc(n); if (r) snprintf(r, n, "%s/%s", dir_, name); return r; }

int main(int argc, char **argv)
{
    enum { O_DIR = 256, O_BACKFILL, O_DB, O_LIVE, O_HEAT, O_OFFICIAL, O_BIND, O_PORT, O_NOTLS, O_INSECURE, O_CERT, O_KEY, O_AUTHORIZED,
           O_GENCERT, O_FORCE, O_FPR, O_AUTHORIZE, O_COMMENT };
    static const struct option longopts[] = {
        { "config", 1, 0, 'c' }, { "check-config", 0, 0, 't' }, { "daemon", 0, 0, 'D' }, { "pidfile", 1, 0, 'P' },
        { "help", 0, 0, 'h' }, { "version", 0, 0, 'V' },
        { "dir", 1, 0, O_DIR }, { "backfill", 1, 0, O_BACKFILL }, { "db", 1, 0, O_DB }, { "live-bins", 1, 0, O_LIVE }, { "heat-db", 1, 0, O_HEAT },
        { "official-klines", 0, 0, O_OFFICIAL }, { "bind", 1, 0, O_BIND }, { "port", 1, 0, O_PORT },
        { "no-tls", 0, 0, O_NOTLS }, { "insecure", 0, 0, O_INSECURE }, { "certificate", 1, 0, O_CERT }, { "key", 1, 0, O_KEY }, { "authorized", 1, 0, O_AUTHORIZED },
        { "gen-cert", 0, 0, O_GENCERT }, { "force", 0, 0, O_FORCE }, { "fingerprint", 0, 0, O_FPR }, { "authorize", 1, 0, O_AUTHORIZE }, { "comment", 1, 0, O_COMMENT },
        { 0, 0, 0, 0 } };
    const char *cfgpath = NULL, *c_dir = NULL, *c_back = NULL, *c_db = NULL, *c_live = NULL, *c_heat = NULL, *c_bind = NULL, *c_cert = NULL, *c_key = NULL, *c_authz = NULL;
    const char *c_authorize = NULL, *c_comment = NULL, *c_pid = NULL;
    int c_official = -1, c_port = 0, c_tls = -1, c_insecure = -1, check = 0, gencert = 0, force = 0, fpr = 0, daemon_mode = 0, opt;
    struct candled_conf cf; candled_conf_init(&cf);

    while ((opt = getopt_long(argc, argv, "c:tDP:hV", longopts, NULL)) != -1) {
        switch (opt) {
        case 'c': cfgpath = optarg; break;           case 't': check = 1; break;
        case 'D': daemon_mode = 1; break;            case 'P': c_pid = optarg; break;
        case 'h': usage(stdout); return 0;           case 'V': version(); return 0;
        case O_DIR: c_dir = optarg; break;           case O_BACKFILL: c_back = optarg; break;
        case O_DB: c_db = optarg; break;             case O_LIVE: c_live = optarg; break;
        case O_HEAT: c_heat = optarg; break;         case O_OFFICIAL: c_official = 1; break;
        case O_BIND: c_bind = optarg; break;         case O_PORT: c_port = atoi(optarg); if (c_port < 1 || c_port > 65535) { fprintf(stderr, "candled: bad port %s\n", optarg); return 2; } break;
        case O_NOTLS: c_tls = 0; break;              case O_INSECURE: c_insecure = 1; break;
        case O_CERT: c_cert = optarg; break;         case O_KEY: c_key = optarg; break;      case O_AUTHORIZED: c_authz = optarg; break;
        case O_GENCERT: gencert = 1; break;          case O_FORCE: force = 1; break;         case O_FPR: fpr = 1; break;
        case O_AUTHORIZE: c_authorize = optarg; break;  case O_COMMENT: c_comment = optarg; break;
        default: fputs("Try 'candled --help' for more information.\n", stderr); return 2;
        }
    }
    if (optind < argc) { fprintf(stderr, "candled: unexpected argument %s\nTry 'candled --help' for more information.\n", argv[optind]); return 2; }

    /* the configuration file: the one named, which must be there, or the default one if there is one */
    {   char *dflt = joined(SYSCONFDIR, "candled.conf");
        const char *path = cfgpath ? cfgpath : dflt;
        if (cfgpath || (dflt && access(dflt, R_OK) == 0)) {
            if (candled_conf_parse(path, &cf) != 0) { fprintf(stderr, "candled: %s is not a good configuration\n", path); return 2; }
        }
        free(dflt);
    }
    {   /* what is in force: an option, else the file, else the built-in value */
        char *d_cert = joined(SYSCONFDIR, "candled/server.crt"), *d_key = joined(SYSCONFDIR, "candled/server.key"), *d_authz = joined(SYSCONFDIR, "candled/authorized-clients");
        const char *bind_addr = c_bind ? c_bind : cf.listen_addr ? cf.listen_addr : "127.0.0.1";
        int port = c_port ? c_port : cf.port ? cf.port : 8766;
        const char *certfile = c_cert ? c_cert : cf.certificate ? cf.certificate : d_cert;
        const char *keyfile = c_key ? c_key : cf.key ? cf.key : d_key;
        const char *authzfile = c_authz ? c_authz : cf.authorized ? cf.authorized : d_authz;
        int tls = c_tls != -1 ? c_tls : cf.tls != -1 ? cf.tls : 1;
        int insecure = c_insecure != -1 ? c_insecure : cf.insecure != -1 ? cf.insecure : 0;
        dir = c_dir ? c_dir : cf.candles ? cf.candles : STATEDIR "/candles";
        dir2 = c_back ? c_back : cf.backfill; dbpath = c_db ? c_db : cf.database; binsdir = c_live ? c_live : cf.live_bins; heatdb = c_heat ? c_heat : cf.heat_db;
        rest_enabled = c_official != -1 ? c_official : cf.official_klines != -1 ? cf.official_klines : 0;
        if (!strcmp(bind_addr, "*")) bind_addr = "0.0.0.0";

        if (gencert) {
            char fp[TLS_FP_LEN];
            if (tls_gen_cert(certfile, keyfile, force, fp) != 0) return 1;
            printf("certificate: %s\nprivate key: %s\nserver fingerprint: %s\n", certfile, keyfile, fp);
            printf("A client pins this fingerprint (wincan: server_pin in wincan.ini); to see it again: candled --fingerprint\n");
            return 0;
        }
        if (fpr) {
            char fp[TLS_FP_LEN];
            if (tls_cert_fingerprint(certfile, fp) != 0) return 1;
            printf("%s\n", fp);
            return 0;
        }
        if (c_authorize) {
            int r = tls_authz_add(authzfile, c_authorize, c_comment);
            if (r < 0) return 1;
            printf(r ? "that client was already authorized (%s)\n" : "authorized: added to %s\n", authzfile);
            return 0;
        }
        if (!tls && !is_loopback(bind_addr) && !insecure) {
            fprintf(stderr, "candled: refusing to serve plain HTTP on %s: it would be open to the network.  Use TLS, listen on 127.0.0.1, or say 'insecure yes'.\n", bind_addr);
            return 2;
        }
        if (check) {
            printf("candled: the configuration is good (listening on %s:%d, %s, candles from %s)\n", bind_addr, port, tls ? "TLS with client keys" : "plain HTTP", dir);
            if (tls && access(certfile, R_OK) != 0) printf("candled: note: no server certificate yet at %s; make one with candled --gen-cert\n", certfile);
            return 0;
        }
        if (tls) {
            if (access(certfile, R_OK) != 0 || access(keyfile, R_OK) != 0) {
                fprintf(stderr, "candled: no server certificate and key (%s, %s).  Make them with: candled --gen-cert\n", certfile, keyfile);
                return 2;
            }
            if (tls_server_init(certfile, keyfile) != 0) return 1;
            tls_authz_set(authzfile, cf.authorize, cf.nauthorize);
        }
        use_tls = tls;
        pidfile_path = c_pid;

        signal(SIGPIPE, SIG_IGN);
        int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
        if (inet_pton(AF_INET, bind_addr, &a.sin_addr) != 1) { fprintf(stderr, "candled: bad address %s\n", bind_addr); return 2; }
        if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 16) < 0) { perror("candled: bind"); return 1; }
        if (daemon_mode) {                                  /* before any thread is made: the children of a fork have only the calling thread */
            if (daemon(1, 0) != 0) { perror("candled: daemon"); return 1; }
            openlog("candled", LOG_PID, LOG_DAEMON); daemonized = 1;
        }
        if (c_pid) { FILE *pf = fopen(c_pid, "w"); if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); } else logmsg("cannot write %s: %s", c_pid, strerror(errno)); }
        signal(SIGTERM, on_term); signal(SIGINT, on_term);
        if (rest_enabled) { curl_global_init(CURL_GLOBAL_DEFAULT); pthread_t ot; pthread_attr_t oa; pthread_attr_init(&oa); pthread_attr_setdetachstate(&oa, PTHREAD_CREATE_DETACHED); pthread_create(&ot, &oa, official_thread, NULL); pthread_attr_destroy(&oa); }
        { pthread_t rt; pthread_attr_t ra; pthread_attr_init(&ra); pthread_attr_setdetachstate(&ra, PTHREAD_CREATE_DETACHED); pthread_create(&rt, &ra, refresher, NULL); pthread_attr_destroy(&ra); }
        if (dbpath) { pthread_t dt; pthread_attr_t da; pthread_attr_init(&da); pthread_attr_setdetachstate(&da, PTHREAD_CREATE_DETACHED); pthread_create(&dt, &da, dbnames_thread, NULL); pthread_attr_destroy(&da); }
        logmsg("serving %s%s%s%s%s on %s:%d (%s)", dir, dbpath ? ", the candles of " : "", dbpath ? dbpath : "", dir2 ? ", backfilled from " : "", dir2 ? dir2 : "", bind_addr, port, tls ? "TLS, clients authorized by key" : "plain HTTP");
        for (;;) {
            int fd = accept(ls, NULL, NULL);
            if (fd < 0) { if (errno == EINTR) continue; logmsg("accept: %s", strerror(errno)); sleep(1); continue; }
            pthread_mutex_lock(&nconn_mu); int busy = nconn >= 32; if (!busy) nconn++; pthread_mutex_unlock(&nconn_mu);
            if (busy) { close(fd); continue; }
            pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&th, &at, serve, (void *)(intptr_t)fd) != 0) { pthread_mutex_lock(&nconn_mu); nconn--; pthread_mutex_unlock(&nconn_mu); close(fd); }
            pthread_attr_destroy(&at);
        }
    }
}
