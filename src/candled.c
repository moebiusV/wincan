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
#include <arpa/inet.h>

#define MAX_PAIRS 256
static const char *dir = ".";
typedef struct { char name[24]; Series s; int started; } Coin;
static Coin coins[MAX_PAIRS];
static int ncoins;
static pthread_mutex_t reg = PTHREAD_MUTEX_INITIALIZER;

static int64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static int valid_pair(const char *p)
{
    size_t n = strlen(p);
    if (n < 1 || n > 16) return 0;
    for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)p[i])) return 0;
    return 1;
}

static void *history_thread(void *arg)
{
    Coin *c = arg;
    while (series_history(&c->s, 4 << 20)) ;
    return NULL;
}

/* find or create the series of a pair; the first request folds the newest day at once and starts the history scan */
static Coin *coin_get(const char *name)
{
    pthread_mutex_lock(&reg);
    Coin *c = NULL;
    for (int i = 0; i < ncoins; i++) if (!strcmp(coins[i].name, name)) { c = &coins[i]; break; }
    if (!c) {
        char path[768]; snprintf(path, sizeof path, "%s/%s.bin", dir, name);
        if (!series_valid(path) || ncoins >= MAX_PAIRS) { pthread_mutex_unlock(&reg); return NULL; }
        c = &coins[ncoins];
        snprintf(c->name, sizeof c->name, "%s", name);
        if (series_open(&c->s, path, SERIES_CAP) != 0 || series_tail(&c->s, 3 * 86400 * 10) != 0) { pthread_mutex_unlock(&reg); return NULL; }
        ncoins++;
        pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &at, history_thread, c) == 0) c->started = 1;
        pthread_attr_destroy(&at);
    }
    pthread_mutex_unlock(&reg);
    return c;
}

static int by_mtime_desc(const void *a, const void *b)
{
    const struct { char n[64]; time_t t; } *x = a, *y = b;
    return (y->t > x->t) - (y->t < x->t);
}

static int pairs_json(char *buf, size_t cap)
{
    struct { char n[64]; time_t t; } list[MAX_PAIRS];
    int n = 0;
    DIR *d = opendir(dir);
    if (!d) return snprintf(buf, cap, "[]");
    struct dirent *e;
    while ((e = readdir(d)) && n < MAX_PAIRS) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".bin") || l - 4 >= sizeof list[0].n || l - 4 > 16) continue;
        char path[768]; snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st; if (stat(path, &st) < 0 || !series_valid(path)) continue;
        memcpy(list[n].n, e->d_name, l - 4); list[n].n[l - 4] = 0;
        if (!valid_pair(list[n].n)) continue;
        list[n].t = st.st_mtime; n++;
    }
    closedir(d);
    qsort(list, (size_t)n, sizeof list[0], by_mtime_desc);
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
    int h = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", code, status, len);
    if (write(fd, head, (size_t)h) < 0) return;
    for (int off = 0; off < len; ) { ssize_t w = write(fd, body + off, (size_t)(len - off)); if (w <= 0) return; off += (int)w; }
}

static void handle(int fd)
{
    char req[2048]; ssize_t got = 0;
    while (got < (ssize_t)sizeof req - 1) {
        ssize_t r = read(fd, req + got, sizeof req - 1 - (size_t)got);
        if (r <= 0) break;
        got += r; req[got] = 0;
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
    }
    if (got <= 0) return;
    req[got] = 0;
    static char body[64 * 1024];
    if (strncmp(req, "GET ", 4)) { respond(fd, 405, "Method Not Allowed", "{\"error\":\"GET only\"}", 20); return; }
    char *path = req + 4, *sp = strchr(path, ' ');
    if (!sp) { respond(fd, 400, "Bad Request", "{\"error\":\"bad request\"}", 23); return; }
    *sp = 0;
    char *q = strchr(path, '?'); if (q) *q++ = 0;
    if (!strcmp(path, "/pairs")) { int n = pairs_json(body, sizeof body); respond(fd, 200, "OK", body, n); return; }
    if (!strcmp(path, "/candles")) {
        char pair[40] = "", tfs[16] = "1m", ns[16] = "60";
        qparam(q, "pair", pair, sizeof pair); qparam(q, "tf", tfs, sizeof tfs); qparam(q, "n", ns, sizeof ns);
        int tf = tf_minutes(tfs), n = atoi(ns);
        if (!valid_pair(pair) || !tf || n < 1) { respond(fd, 400, "Bad Request", "{\"error\":\"pair, tf (1m 3m 5m 15m 30m 1h 4h 1d 1w) or n wrong\"}", 62); return; }
        if (n > MAX_BARS) n = MAX_BARS;
        Coin *c = coin_get(pair);
        if (!c) { respond(fd, 404, "Not Found", "{\"error\":\"no such pair\"}", 24); return; }
        series_refresh(&c->s);
        static Out out;
        series_query(&c->s, tf, n, now_ms(), &out);
        int o = snprintf(body, sizeof body, "{\"pair\":\"%s\",\"tf\":\"%s\",\"price\":%.8g,\"age_ms\":%lld,\"complete\":%s,\"n\":%d,",
                         pair, tfs, (double)out.price, (long long)out.age_ms, out.complete ? "true" : "false", out.n);
        arr(body, sizeof body, &o, "t", NULL, out.t, out.n); o += snprintf(body + o, sizeof body - (size_t)o, ",");
        arr(body, sizeof body, &o, "o", out.o, NULL, out.n); o += snprintf(body + o, sizeof body - (size_t)o, ",");
        arr(body, sizeof body, &o, "h", out.h, NULL, out.n); o += snprintf(body + o, sizeof body - (size_t)o, ",");
        arr(body, sizeof body, &o, "l", out.l, NULL, out.n); o += snprintf(body + o, sizeof body - (size_t)o, ",");
        arr(body, sizeof body, &o, "c", out.c, NULL, out.n); o += snprintf(body + o, sizeof body - (size_t)o, ",");
        arr(body, sizeof body, &o, "v", out.v, NULL, out.n); o += snprintf(body + o, sizeof body - (size_t)o, "}");
        respond(fd, 200, "OK", body, o);
        return;
    }
    respond(fd, 404, "Not Found", "{\"error\":\"not found\"}", 21);
}

int main(int argc, char **argv)
{
    int port = 8766; const char *bind_addr = "0.0.0.0";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bind") && i + 1 < argc) bind_addr = argv[++i];
        else if (!strcmp(argv[i], "--version")) { puts("candled " PACKAGE_VERSION); return 0; }
        else { fprintf(stderr, "usage: candled --dir DIR [--port N] [--bind ADDR]\n"); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, bind_addr, &a.sin_addr) != 1) { fprintf(stderr, "candled: bad address %s\n", bind_addr); return 2; }
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 16) < 0) { perror("candled: bind"); return 1; }
    fprintf(stderr, "candled: serving %s on %s:%d\n", dir, bind_addr, port);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; perror("accept"); sleep(1); continue; }
        struct timeval tv = { 2, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        handle(fd);
        close(fd);
    }
}
