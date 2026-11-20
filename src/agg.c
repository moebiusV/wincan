/* agg.c -- see agg.h */
#define _GNU_SOURCE
#include "agg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

typedef struct { float O, H, L, C, V, J; uint32_t N; } Rec;

static int64_t floor_div(int64_t a, int64_t b) { int64_t q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }

int tf_minutes(const char *name)
{
    static const struct { const char *n; int m; } T[] = {
        {"1m", 1}, {"3m", 3}, {"5m", 5}, {"15m", 15}, {"30m", 30}, {"1h", 60}, {"4h", 240}, {"1d", 1440}, {"1w", 10080}, {0, 0}};
    for (int i = 0; T[i].n; i++) if (!strcmp(name, T[i].n)) return T[i].m;
    return 0;
}

int series_open(Series *s, const char *path, int cap)
{
    memset(s, 0, sizeof *s);
    snprintf(s->path, sizeof s->path, "%s", path);
    s->cap = cap;
    s->bars = calloc((size_t)cap, sizeof(Bar));
    if (!s->bars) return -1;
    pthread_mutex_init(&s->mu, NULL); pthread_mutex_init(&s->rmu, NULL);
    return 0;
}

void series_close(Series *s) { free(s->bars); s->bars = NULL; pthread_mutex_destroy(&s->mu); }

/* put one finished minute (a local run of records) into the slot, before or after what is there */
static void merge(Series *s, int64_t minute, const Bar *b, int before)
{
    int64_t i = minute - s->base_min;
    if (i < 0) return;                                       /* older than the window kept */
    if (i >= s->cap) {                                       /* slide the window forward, with room to spare */
        int64_t sh = i - s->cap + 1 + 1024;
        if (sh >= s->cap) { memset(s->bars, 0, (size_t)s->cap * sizeof(Bar)); }
        else { memmove(s->bars, s->bars + sh, (size_t)(s->cap - sh) * sizeof(Bar)); memset(s->bars + (s->cap - sh), 0, (size_t)sh * sizeof(Bar)); }
        s->base_min += sh; i -= sh;
    }
    Bar *d = &s->bars[i];
    if (!d->has) { *d = *b; return; }
    if (b->h > d->h) d->h = b->h;
    if (b->l < d->l) d->l = b->l;
    d->v += b->v;
    if (before) d->o = b->o; else d->c = b->c;
}

static int is_marker(const Rec *r) { return r->N == 0xFFFFFFFFu && r->V == -1.0f && r->J != r->J; }

static void unframe(const unsigned char *f, Rec *r) { memcpy(r, f + 1, sizeof *r); }

/* a frame starts at b[p] and, as far as there are bytes, so do the next two */
static int strong(const unsigned char *b, int64_t len, int64_t p)
{
    int n = 0;
    for (int k = 0; k < 3; k++) {
        int64_t q = p + (int64_t)REC_SIZE * k;
        if (q + REC_SIZE > len) break;
        if (b[q] != '{' || b[q + REC_SIZE - 1] != '}') return 0;
        n++;
    }
    return n > 0;
}

/*
 * The frames that start before hi in b[0..len): their offsets go to offs[]; *end is the offset after the last.  If synced, a frame is
 * expected at 0; if not, the first frame is the first place where three frames follow one another.  After a break the frames are looked for again.
 */
static int64_t locate(const unsigned char *b, int64_t len, int64_t hi, int synced, int64_t *offs, int64_t max, int64_t *end)
{
    int64_t p = 0, n = 0;
    int in_step = synced;
    *end = 0;
    while (p + REC_SIZE <= len && p < hi && n < max) {
        if (b[p] == '{' && b[p + REC_SIZE - 1] == '}' && (in_step || strong(b, len, p))) {
            offs[n++] = p; p += REC_SIZE; in_step = 1; *end = p; continue;
        }
        in_step = 0; p++;
        while (p + REC_SIZE <= len && p < hi && !(b[p] == '{' && b[p + REC_SIZE - 1] == '}' && strong(b, len, p))) p++;
    }
    return n;
}

/* the time of the first candle among the frames when the time after the last one is t_end_ms; markers set the clock */
static int64_t start_time(const unsigned char *b, const int64_t *offs, int64_t n, int64_t t_end_ms)
{
    int64_t t = t_end_ms;
    for (int64_t i = n - 1; i >= 0; i--) {
        Rec r; unframe(b + offs[i], &r);
        if (is_marker(&r)) { uint32_t ts; memcpy(&ts, &r.O, 4); t = (int64_t)ts * 1000; }
        else t -= REC_MS;
    }
    return t;
}

/* fold the frames, the first candle being at t_first_ms; returns the time of the next candle */
static int64_t fold(Series *s, const unsigned char *b, const int64_t *offs, int64_t n, int64_t t_first_ms, int before)
{
    Bar cur; int64_t curmin = 0; int have = 0;
    int64_t t = t_first_ms;
    for (int64_t k = 0; k < n; k++) {
        Rec r; unframe(b + offs[k], &r);
        if (is_marker(&r)) { uint32_t ts; memcpy(&ts, &r.O, 4); t = (int64_t)ts * 1000; continue; }
        int64_t tc = t; t += REC_MS;
        if (!(r.C > 0) || !(r.H > 0) || !(r.L > 0) || !(r.O > 0)) continue;     /* NaN, zero or negative: not a price */
        int64_t m = floor_div(tc, 60000);
        if (have && m != curmin) { merge(s, curmin, &cur, before); have = 0; }
        if (!have) { cur.o = r.O; cur.h = r.H; cur.l = r.L; cur.c = r.C; cur.v = r.V; cur.has = 1; curmin = m; have = 1; }
        else { if (r.H > cur.h) cur.h = r.H; if (r.L < cur.l) cur.l = r.L; cur.c = r.C; cur.v += r.V; }
    }
    if (have) merge(s, curmin, &cur, before);
    return t;
}

#define CHUNK_BYTES (32 << 20)
#define MAX_OFFS    (CHUNK_BYTES / REC_SIZE + 4)

/*
 * Fold the frames that start in the bytes [lo, hi) of a file of the given size.  Forward: the first candle is at *t_ms; on return *t_ms is the time of the next candle
 * and *edge the offset after the last frame.  Backward (newest chunk first): *t_ms is the time of the candle after the range; on return the time of its first candle,
 * and *edge the offset of the first frame.
 */
static int read_fold(Series *s, int fd, int64_t size, int64_t lo, int64_t hi, int64_t *t_ms, int before, int synced, int64_t *edge)
{
    unsigned char *buf = malloc((size_t)CHUNK_BYTES + 4 * REC_SIZE);
    int64_t *offs = malloc(sizeof(int64_t) * (size_t)MAX_OFFS);
    if (!buf || !offs) { free(buf); free(offs); return -1; }
    int64_t pos = before ? hi : lo;
    int first_chunk = 1, rc = 0;
    if (edge) *edge = before ? hi : lo;
    while (before ? pos > lo : pos < hi) {
        int64_t a = before ? (pos - lo > CHUNK_BYTES ? pos - CHUNK_BYTES : lo) : pos;
        int64_t top = before ? pos : (hi - pos > CHUNK_BYTES ? pos + CHUNK_BYTES : hi);         /* frames start before top */
        int64_t rd_end = top + 3 * REC_SIZE < size ? top + 3 * REC_SIZE : size;
        ssize_t got = pread(fd, buf, (size_t)(rd_end - a), (off_t)a);
        if (got != (ssize_t)(rd_end - a)) { rc = -1; break; }
        int64_t end, cnt = locate(buf, rd_end - a, top - a, before ? 0 : (first_chunk ? synced : 1), offs, MAX_OFFS, &end);
        if (cnt > 0) {
            int64_t t0 = before ? start_time(buf, offs, cnt, *t_ms) : *t_ms;
            pthread_mutex_lock(&s->mu);
            int64_t t_next = fold(s, buf, offs, cnt, t0, before);
            pthread_mutex_unlock(&s->mu);
            if (before) { *t_ms = t0; if (edge) *edge = a + offs[0]; } else { *t_ms = t_next; if (edge) *edge = a + end; }
        }
        first_chunk = 0;
        if (before) pos = a; else pos = (cnt > 0 && a + end > pos) ? a + end : top;
    }
    free(buf); free(offs);
    return rc;
}

static int stat_file(Series *s, int64_t *size, int64_t *mtime_ms)
{
    struct stat st;
    if (stat(s->path, &st) < 0) return -1;
    *size = (int64_t)st.st_size;
    *mtime_ms = (int64_t)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000;
    return 0;
}

/* the close of the newest candle in the last stretch of the file */
static float last_close(int fd, int64_t size)
{
    unsigned char b[30 * 400 + 90]; int64_t offs[500], end;
    int64_t a = size > (int64_t)sizeof b ? size - (int64_t)sizeof b : 0;
    ssize_t got = pread(fd, b, (size_t)(size - a), (off_t)a);
    if (got <= 0) return 0;
    int64_t n = locate(b, got, got, 0, offs, 500, &end);
    for (int64_t i = n - 1; i >= 0; i--) {
        Rec r; unframe(b + offs[i], &r);
        if (!is_marker(&r) && r.C > 0) return r.C;
    }
    return 0;
}

int series_valid(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0 || st.st_size < 4 * REC_SIZE) return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    int64_t size = (int64_t)st.st_size, a = size > 3000 ? size - 3000 : 0, end, offs[200];
    unsigned char b[3000];
    ssize_t got = pread(fd, b, (size_t)(size - a), (off_t)a);
    int ok = got > 0 && locate(b, got, got, 0, offs, 200, &end) >= 3;
    close(fd);
    return ok;
}

int series_tail(Series *s, int64_t recent)
{
    int64_t size, mt;
    if (stat_file(s, &size, &mt) < 0 || size < 4 * REC_SIZE) return -1;
    int fd = open(s->path, O_RDONLY);
    if (fd < 0) return -1;
    int64_t start = size > recent * REC_SIZE ? size - recent * REC_SIZE : 0;
    /* the clock: markers inside the window set it; without one, the file's end is its modification time */
    int64_t span = size - start, *offs = malloc(sizeof(int64_t) * (size_t)(span / REC_SIZE + 4)), end;
    unsigned char *buf = malloc((size_t)span);
    if (!buf || !offs) { free(buf); free(offs); close(fd); return -1; }
    if (pread(fd, buf, (size_t)span, (off_t)start) != (ssize_t)span) { free(buf); free(offs); close(fd); return -1; }
    int64_t cnt = locate(buf, span, span, start == 0, offs, span / REC_SIZE + 4, &end);
    if (cnt == 0) { free(buf); free(offs); close(fd); return -1; }
    int64_t t_first = start_time(buf, offs, cnt, mt);
    int64_t first_pos = start + offs[0];
    free(buf); free(offs);
    pthread_mutex_lock(&s->mu);
    memset(s->bars, 0, (size_t)s->cap * sizeof(Bar));
    s->base_min = floor_div(mt, 60000) - (s->cap - 1024);
    s->first_pos = first_pos; s->first_ms = t_first; s->history_done = (first_pos < REC_SIZE);
    pthread_mutex_unlock(&s->mu);
    int64_t t = t_first, edge = first_pos;
    int rc = read_fold(s, fd, size, first_pos, size, &t, 0, 1, &edge);
    s->next_ms = t; s->pos = edge;
    s->price = last_close(fd, size);
    close(fd);
    return rc;
}

int series_history(Series *s, int64_t nbytes)
{
    if (s->history_done) return 0;
    int fd = open(s->path, O_RDONLY);
    if (fd < 0) return 0;
    int64_t size, mt;
    if (stat_file(s, &size, &mt) < 0) return 0;
    int64_t hi = s->first_pos, lo = hi > nbytes ? hi - nbytes : 0;
    int64_t t = s->first_ms, edge = hi;
    int rc = read_fold(s, fd, size, lo, hi, &t, 1, 0, &edge);
    close(fd);
    pthread_mutex_lock(&s->mu);
    s->first_pos = edge; s->first_ms = t;
    if (rc < 0 || lo == 0 || floor_div(t, 60000) < s->base_min) s->history_done = 1;
    int more = !s->history_done;
    pthread_mutex_unlock(&s->mu);
    return more;
}

static int refresh_locked(Series *s);
int series_refresh(Series *s)
{
    pthread_mutex_lock(&s->rmu);
    int rc = refresh_locked(s);
    pthread_mutex_unlock(&s->rmu);
    return rc;
}

static int refresh_locked(Series *s)
{
    int64_t size, mt;
    if (stat_file(s, &size, &mt) < 0) return -1;
    if (size == s->pos) return 0;
    if (size < s->pos) return series_tail(s, 3 * 86400 * 10);          /* truncated or replaced: start again */
    if (size - s->pos < REC_SIZE) return 0;
    int fd = open(s->path, O_RDONLY);
    if (fd < 0) return -1;
    int64_t t = s->next_ms, edge = s->pos;
    int rc = read_fold(s, fd, size, s->pos, size, &t, 0, 1, &edge);
    pthread_mutex_lock(&s->mu);
    s->next_ms = t; s->pos = edge;
    s->price = last_close(fd, size);
    pthread_mutex_unlock(&s->mu);
    close(fd);
    return rc;
}

void series_query(Series *s, int tf, int n, int64_t now_ms, Out *out)
{
    memset(out, 0, sizeof *out);
    if (n > MAX_BARS) n = MAX_BARS;
    if (n < 1) n = 1;
    pthread_mutex_lock(&s->mu);
    out->price = s->price; out->age_ms = now_ms - s->next_ms; out->complete = s->history_done;
    int64_t last = -1;
    for (int64_t i = s->cap - 1; i >= 0; i--) if (s->bars[i].has) { last = i; break; }
    if (last >= 0) {
        int64_t lastmin = s->base_min + last, step = tf, off = (tf == 10080) ? 5760 : 0;    /* weeks begin on Monday: 1970-01-05 */
        int64_t key = floor_div(lastmin - off, step) * step + off;
        Out tmp; int cnt = 0;
        for (; cnt < n; key -= step) {
            float o = 0, h = 0, l = 0, c = 0, v = 0; int got = 0;
            for (int64_t m = key; m < key + step; m++) {
                int64_t i = m - s->base_min;
                if (i < 0 || i >= s->cap || !s->bars[i].has) continue;
                const Bar *b = &s->bars[i];
                if (!got) { o = b->o; h = b->h; l = b->l; got = 1; } else { if (b->h > h) h = b->h; if (b->l < l) l = b->l; }
                c = b->c; v += b->v;
            }
            if (key + step - 1 < s->base_min) break;                       /* past the oldest bar */
            if (!got) continue;                                            /* a gap: no bar */
            tmp.t[cnt] = key * 60; tmp.o[cnt] = o; tmp.h[cnt] = h; tmp.l[cnt] = l; tmp.c[cnt] = c; tmp.v[cnt] = v; cnt++;
        }
        out->n = cnt;
        for (int i = 0; i < cnt; i++) {                                    /* oldest first */
            int j = cnt - 1 - i;
            out->t[i] = tmp.t[j]; out->o[i] = tmp.o[j]; out->h[i] = tmp.h[j]; out->l[i] = tmp.l[j]; out->c[i] = tmp.c[j]; out->v[i] = tmp.v[j];
        }
    }
    pthread_mutex_unlock(&s->mu);
}
