/*
 * candled -- a small read-only HTTP server that serves candles, as JSON, from the raw .bin files of a collector.
 *
 *   GET /pairs                              ["BTC","ETH",...]            (recently written files first)
 *   GET /candles?pair=BTC&tf=5m&n=60        {"pair":..,"tf":..,"price":..,"age_ms":..,"complete":..,"t":[..],"o":[..],...}
 *
 * See candled(8).
 */
#define _GNU_SOURCE
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

#define MAX_PAIRS 512
static const char *dir = ".", *dir2 = NULL, *dbpath = NULL, *binsdir = NULL;      /* the files of the live collector, the backfill files (spot), and the database whose candles table is authoritative */
/* the sources of a coin, in order: the live files (100 ms ticks), the database, the websocket daemon's bins (closed minutes), the backfill files */
#define NSRC 5
typedef struct { char name[24]; Series s[NSRC]; int has[NSRC]; time_t checked; volatile int64_t last_req_ms, last_refresh_ms; int64_t rest_min; int rest_final; volatile double mark; volatile int64_t mark_ms; } Coin;
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
 * The exchange's official candle of a minute replaces what was made of ticks and files as soon as it can be had.  For the pairs being watched: one second after a
 * minute has closed, and again three seconds later (a candle can still change a little at the close); a pair that was not watched is fetched once, at once, when it is.
 */
static void *official_thread(void *arg)
{
    (void)arg;
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "candled");
    for (;;) {
        struct timespec ts = { 0, 250 * 1000000L }; nanosleep(&ts, NULL);
        int64_t t = now_ms(), cur = t / 60000, closed = cur - 1;        /* the minute that has just closed */
        double into = (double)(t - cur * 60000) / 1000.0;               /* seconds since it closed */
        for (int i = 0; i < ncoins; i++) {
            Coin *c = &coins[i];
            if (!c->has[4] || t - c->last_req_ms >= 5000) continue;     /* only a pair someone is looking at */
            if (t - c->mark_ms >= 1000) fetch_mark(c, curl);            /* its mark price, every second */
            if (c->rest_min != closed && into >= 1.0) {
                fetch_official(c, curl, (closed - 2) * 60, (closed + 1) * 60, cur);
                c->rest_min = closed; c->rest_final = into >= 4.0;
            } else if (c->rest_min == closed && !c->rest_final && into >= 4.0) {
                fetch_official(c, curl, (closed - 1) * 60, (closed + 1) * 60, cur);
                c->rest_final = 1;
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

static void respond(int fd, int code, const char *status, const char *body, int len)
{
    char head[256];
    int h = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nCache-Control: no-store\r\nConnection: keep-alive\r\nKeep-Alive: timeout=60\r\n\r\n", code, status, len);
    if (write(fd, head, (size_t)h) < 0) return;
    for (int off = 0; off < len; ) { ssize_t w = write(fd, body + off, (size_t)(len - off)); if (w <= 0) return; off += (int)w; }
}

/* one request; returns 1 to keep the connection for the next */
static int handle(int fd, char *body, size_t bodycap, Out *out)
{
    char req[2048]; ssize_t got = 0;
    while (got < (ssize_t)sizeof req - 1) {
        ssize_t r = read(fd, req + got, sizeof req - 1 - (size_t)got);
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

/* a connection stays open for as many requests as the client sends, until it closes or is idle for a minute */
static void *serve(void *arg)
{
    int fd = (int)(intptr_t)arg;
    struct timeval tv = { 60, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    char *body = malloc(64 * 1024); Out *out = malloc(sizeof *out);
    if (body && out) while (handle(fd, body, 64 * 1024, out)) ;
    free(body); free(out); close(fd);
    pthread_mutex_lock(&nconn_mu); nconn--; pthread_mutex_unlock(&nconn_mu);
    return NULL;
}

int main(int argc, char **argv)
{
    int port = 8766; const char *bind_addr = "0.0.0.0";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--backfill") && i + 1 < argc) dir2 = argv[++i];
        else if (!strcmp(argv[i], "--db") && i + 1 < argc) dbpath = argv[++i];
        else if (!strcmp(argv[i], "--live-bins") && i + 1 < argc) binsdir = argv[++i];
        else if (!strcmp(argv[i], "--official-klines")) rest_enabled = 1;
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bind") && i + 1 < argc) bind_addr = argv[++i];
        else if (!strcmp(argv[i], "--version")) { puts("candled " PACKAGE_VERSION); return 0; }
        else { fprintf(stderr, "usage: candled --dir DIR [--db FILE] [--live-bins DIR] [--official-klines] [--backfill DIR] [--port N] [--bind ADDR]\n"); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, bind_addr, &a.sin_addr) != 1) { fprintf(stderr, "candled: bad address %s\n", bind_addr); return 2; }
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 16) < 0) { perror("candled: bind"); return 1; }
    if (rest_enabled) { curl_global_init(CURL_GLOBAL_DEFAULT); pthread_t ot; pthread_attr_t oa; pthread_attr_init(&oa); pthread_attr_setdetachstate(&oa, PTHREAD_CREATE_DETACHED); pthread_create(&ot, &oa, official_thread, NULL); pthread_attr_destroy(&oa); }
    { pthread_t rt; pthread_attr_t ra; pthread_attr_init(&ra); pthread_attr_setdetachstate(&ra, PTHREAD_CREATE_DETACHED); pthread_create(&rt, &ra, refresher, NULL); pthread_attr_destroy(&ra); }
    if (dbpath) { pthread_t dt; pthread_attr_t da; pthread_attr_init(&da); pthread_attr_setdetachstate(&da, PTHREAD_CREATE_DETACHED); pthread_create(&dt, &da, dbnames_thread, NULL); pthread_attr_destroy(&da); }
    fprintf(stderr, "candled: serving %s%s%s%s%s on %s:%d\n", dir, dbpath ? ", the candles of " : "", dbpath ? dbpath : "", dir2 ? ", backfilled from " : "", dir2 ? dir2 : "", bind_addr, port);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; perror("accept"); sleep(1); continue; }
        pthread_mutex_lock(&nconn_mu); int busy = nconn >= 32; if (!busy) nconn++; pthread_mutex_unlock(&nconn_mu);
        if (busy) { close(fd); continue; }
        pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &at, serve, (void *)(intptr_t)fd) != 0) { pthread_mutex_lock(&nconn_mu); nconn--; pthread_mutex_unlock(&nconn_mu); close(fd); }
        pthread_attr_destroy(&at);
    }
}
