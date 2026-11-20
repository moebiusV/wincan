/* agg.c -- see agg.h */
#define _GNU_SOURCE
#include "agg.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <sqlite3.h>

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

void series_close(Series *s) { if (s->db) sqlite3_close((sqlite3 *)s->db); s->db = NULL; free(s->bars); s->bars = NULL; pthread_mutex_destroy(&s->mu); pthread_mutex_destroy(&s->rmu); }

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

/* a candle that can be believed: positive prices, the high and low around the open and close, and nothing that moved by half in 100 ms
 * (a collector killed in mid-write has left frames with garbage in them that still start with '{' and end with '}') */
static int sane(const Rec *r)
{
    if (!(r->O > 0 && r->C > 0 && r->H > 0 && r->L > 0)) return 0;                 /* also NaN */
    float mx = r->O > r->C ? r->O : r->C, mn = r->O < r->C ? r->O : r->C;
    if (r->H < mx * 0.9999f || r->L > mn * 1.0001f) return 0;
    if (r->H > mx * 1.5f || r->L < mn * 0.5f) return 0;
    return 1;
}

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
    Bar cur; int64_t curmin = 0; int have = 0, real = 0;
    int64_t t = t_first_ms;
    for (int64_t k = 0; k < n; k++) {
        Rec r; unframe(b + offs[k], &r);
        if (is_marker(&r)) { uint32_t ts; memcpy(&ts, &r.O, 4); t = (int64_t)ts * 1000; continue; }
        int64_t tc = t; t += REC_MS;
        if (!sane(&r)) continue;                                                  /* garbage: not a candle */
        if (!(r.V >= 0 && r.V < 1e13f)) r.V = 0;                                  /* a garbage volume does not spoil the price */
        int64_t m = floor_div(tc, 60000);
        if (have && m != curmin) { merge(s, curmin, &cur, before); have = 0; }
        /* a forward-filled candle (no trade: N == 0) only repeats the last price: it must not become a minute's open, high or low, as an exchange's
         * candle opens at the first trade of the minute */
        if (!have) { cur.o = r.O; cur.h = r.H; cur.l = r.L; cur.c = r.C; cur.v = r.V; cur.has = 1; curmin = m; have = 1; real = r.N != 0; }
        else if (r.N == 0) { cur.c = r.C; }
        else if (!real) { cur.o = r.O; cur.h = r.H; cur.l = r.L; cur.c = r.C; cur.v += r.V; real = 1; }
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
        if (!is_marker(&r) && sane(&r)) return r.C;
    }
    return 0;
}

/* ---- a series from a SQLite table of 1-minute candles ---- */

static Bar *slot_for(Series *s, int64_t minute)
{
    int64_t i = minute - s->base_min;
    if (i < 0) return NULL;
    if (i >= s->cap) {
        int64_t sh = i - s->cap + 1 + 1024;
        if (sh >= s->cap) memset(s->bars, 0, (size_t)s->cap * sizeof(Bar));
        else { memmove(s->bars, s->bars + sh, (size_t)(s->cap - sh) * sizeof(Bar)); memset(s->bars + (s->cap - sh), 0, (size_t)sh * sizeof(Bar)); }
        s->base_min += sh; i -= sh;
    }
    return &s->bars[i];
}

int series_open_db(Series *s, const char *dbpath, const char *dbpair, int cap)
{
    if (series_open(s, dbpath, cap) != 0) return -1;
    s->is_db = 1;
    snprintf(s->dbpath, sizeof s->dbpath, "%s", dbpath);
    snprintf(s->dbpair, sizeof s->dbpair, "%s", dbpair);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) { if (db) sqlite3_close(db); series_close(s); return -1; }
    sqlite3_busy_timeout(db, 5000);
    s->db = db;
    return 0;
}

/* the rows with ts in [lo, hi) of the pair, into the bars (a bar already there is replaced); returns the number of rows, or -1; *last_ts and *last_close describe the newest */
static int db_load(Series *s, int64_t lo, int64_t hi, int64_t *last_ts, float *last_close_out)
{
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2((sqlite3 *)s->db, "SELECT ts, open, high, low, close, volume FROM candles WHERE pair=?1 AND ts>=?2 AND ts<?3 ORDER BY ts", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, s->dbpair, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, lo); sqlite3_bind_int64(st, 3, hi);
    enum { CH = 20000 };
    typedef struct { int64_t m; Bar b; } Row;
    Row *rows = malloc(CH * sizeof(Row));
    if (!rows) { sqlite3_finalize(st); return -1; }
    int total = 0, n = 0, rc;
    for (;;) {
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            double o = sqlite3_column_double(st, 1), h = sqlite3_column_double(st, 2), l = sqlite3_column_double(st, 3), c = sqlite3_column_double(st, 4), v = sqlite3_column_double(st, 5);
            double mx = o > c ? o : c, mn = o < c ? o : c;
            if (o > 0 && h > 0 && l > 0 && c > 0 && h >= mx * 0.9999 && l <= mn * 1.0001 && h <= mx * 3 && l >= mn / 3) {          /* believable */
                int64_t ts = sqlite3_column_int64(st, 0);
                Row *r = &rows[n++];
                r->m = floor_div(ts, 60); r->b.o = (float)o; r->b.h = (float)h; r->b.l = (float)l; r->b.c = (float)c; r->b.v = (v >= 0 && v < 1e13) ? (float)v : 0; r->b.has = 1;
                total++;
                if (last_ts && ts >= *last_ts) { *last_ts = ts; if (last_close_out) *last_close_out = (float)c; }
            }
        }
        if ((rc != SQLITE_ROW && n > 0) || n == CH) {
            pthread_mutex_lock(&s->mu);
            for (int i = 0; i < n; i++) { Bar *d = slot_for(s, rows[i].m); if (d) *d = rows[i].b; }
            pthread_mutex_unlock(&s->mu);
            n = 0;
        }
        if (rc != SQLITE_ROW) break;
    }
    free(rows);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? total : -1;
}

static int db_tail(Series *s)
{
    sqlite3_stmt *st;
    /* the newest row, looked for in a bounded range: MAX(ts) over the whole view takes many seconds (it checks every row against the other tables), a range is instant */
    int64_t last = 0, first = 0; int ok = 0;
    static const int DAYS[3] = { 10, 60, 450 };
    for (int d = 0; d < 3 && !ok; d++) {
        if (sqlite3_prepare_v2((sqlite3 *)s->db, "SELECT MAX(ts) FROM candles WHERE pair=?1 AND ts>=?2", -1, &st, NULL) != SQLITE_OK) return -1;
        sqlite3_bind_text(st, 1, s->dbpair, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)time(NULL) - (int64_t)DAYS[d] * 86400);
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) { last = sqlite3_column_int64(st, 0); ok = 1; }
        sqlite3_finalize(st);
    }
    if (!ok) return -1;
    first = 0;                                                       /* the oldest row is not looked up: the history goes back as far as the window kept (416 days) */
    pthread_mutex_lock(&s->mu);
    memset(s->bars, 0, (size_t)s->cap * sizeof(Bar));
    s->base_min = floor_div(last, 60) - (s->cap - 1024);
    s->db_min_ts = first; s->db_last_ts = last;
    pthread_mutex_unlock(&s->mu);
    int64_t lo = last - 3 * 86400; if (lo < first) lo = first;
    int64_t lt = 0; float lc = 0;
    int n = db_load(s, lo, last + 1, &lt, &lc);
    if (n <= 0) return -1;
    s->db_lo_ts = lo; s->history_done = lo <= first;
    s->first_ms = lo * 1000; s->next_ms = (last + 60) * 1000; s->price = lc;
    return 0;
}

static int db_history(Series *s)
{
    if (s->history_done) return 0;
    int64_t hi = s->db_lo_ts, lo = hi - 30 * 86400, floor_ts = s->base_min * 60;
    if (lo < s->db_min_ts) lo = s->db_min_ts;
    if (lo < floor_ts) lo = floor_ts;
    int rc = db_load(s, lo, hi, NULL, NULL);
    pthread_mutex_lock(&s->mu);
    s->db_lo_ts = lo;
    if (rc < 0 || lo <= s->db_min_ts || lo <= floor_ts) s->history_done = 1;
    int more = !s->history_done;
    pthread_mutex_unlock(&s->mu);
    return more;
}

static int db_refresh(Series *s)
{
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    int64_t now = (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
    if (now - s->last_check_ms < 5000) return 0;                       /* a database is asked at most every five seconds */
    s->last_check_ms = now;
    int64_t lt = s->db_last_ts; float lc = s->price;
    /* from three hours before the newest row we have: the newest may have been rewritten, and the database fills in minutes behind its newest row later */
    int n = db_load(s, s->db_last_ts - 3 * 3600, INT64_MAX, &lt, &lc);
    if (n > 0 && lt >= s->db_last_ts) { pthread_mutex_lock(&s->mu); s->db_last_ts = lt; s->next_ms = (lt + 60) * 1000; s->price = lc; pthread_mutex_unlock(&s->mu); }
    return n < 0 ? -1 : 0;
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
    if (s->is_db) return db_tail(s);
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
    s->partial_min = (s->history_done && t_first % 60000 != 0) ? floor_div(t_first, 60000) : 0;
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
    if (s->is_db) return db_history(s);
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
    if (rc >= 0 && lo == 0 && t % 60000 != 0) s->partial_min = floor_div(t, 60000);
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
    if (s->is_db) return db_refresh(s);
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

static const Bar *bar_at(const Series *s, int64_t minute)
{
    if (!s) return NULL;
    int64_t i = minute - s->base_min;
    if (i < 0 || i >= s->cap || !s->bars[i].has) return NULL;
    return &s->bars[i];
}

static int64_t last_minute(const Series *s)
{
    if (!s) return INT64_MIN;
    for (int64_t i = s->cap - 1; i >= 0; i--) if (s->bars[i].has) return s->base_min + i;
    return INT64_MIN;
}

/*
 * The minute from the first series that has it, the first minute of a file that began mid-minute only if nothing else has it.  A database is
 * authoritative for every minute it has except those at the live edge (the last two minutes of all the data), where the ticks of the collector
 * are used: the database may not have them yet, or not in their final form.
 */
static const Bar *pick(Series **l, int n, int64_t m, int live)
{
    if (!live) for (int i = 0; i < n; i++) if (l[i] && l[i]->is_db) { const Bar *b = bar_at(l[i], m); if (b) return b; }
    const Bar *partial = NULL;
    for (int i = 0; i < n; i++) {
        const Bar *b = bar_at(l[i], m);
        if (!b) continue;
        if (l[i]->partial_min == m) { if (!partial) partial = b; continue; }
        return b;
    }
    return partial;
}

static void lock_all(Series **l, int n) { for (int i = 0; i < n; i++) if (l[i]) pthread_mutex_lock(&l[i]->mu); }
static void unlock_all(Series **l, int n) { for (int i = n - 1; i >= 0; i--) if (l[i]) pthread_mutex_unlock(&l[i]->mu); }

void series_queryN(Series **l, int cnt, int tf, int n, int64_t now_ms, Out *out)
{
    memset(out, 0, sizeof *out);
    if (n > MAX_BARS) n = MAX_BARS;
    if (n < 1) n = 1;
    lock_all(l, cnt);
    int64_t lastmin = INT64_MIN, oldest = INT64_MAX; const Series *lead = NULL; int complete = 1;
    for (int i = 0; i < cnt; i++) {
        if (!l[i]) continue;
        int64_t lm = last_minute(l[i]);
        if (lm > lastmin) lastmin = lm;
        if (!lead && lm != INT64_MIN) lead = l[i];                        /* the price and the age come from the first with data */
        if (l[i]->base_min < oldest) oldest = l[i]->base_min;
        if (!l[i]->history_done) complete = 0;
    }
    if (lead) { out->price = lead->price; out->age_ms = now_ms - lead->next_ms; }
    out->complete = complete;
    if (lastmin != INT64_MIN) {
        int64_t step = tf, off = (tf == 10080) ? 5760 : 0;                   /* weeks begin on Monday: 1970-01-05 */
        int64_t key = floor_div(lastmin - off, step) * step + off;
        static __thread Out tmp; int c2 = 0;
        for (; c2 < n; key -= step) {
            float o = 0, h = 0, lo2 = 0, c = 0, v = 0; int got = 0;
            if (key + step - 1 < oldest) break;                              /* past the oldest bar */
            for (int64_t m = key; m < key + step; m++) {
                const Bar *bb = pick(l, cnt, m, m >= lastmin - 1);
                if (!bb) continue;
                if (!got) { o = bb->o; h = bb->h; lo2 = bb->l; got = 1; } else { if (bb->h > h) h = bb->h; if (bb->l < lo2) lo2 = bb->l; }
                c = bb->c; v += bb->v;
            }
            if (!got) continue;                                              /* a gap: no bar */
            tmp.t[c2] = key * 60; tmp.o[c2] = o; tmp.h[c2] = h; tmp.l[c2] = lo2; tmp.c[c2] = c; tmp.v[c2] = v; c2++;
        }
        out->n = c2;
        for (int i = 0; i < c2; i++) {                                       /* oldest first */
            int j = c2 - 1 - i;
            out->t[i] = tmp.t[j]; out->o[i] = tmp.o[j]; out->h[i] = tmp.h[j]; out->l[i] = tmp.l[j]; out->c[i] = tmp.c[j]; out->v[i] = tmp.v[j];
        }
    }
    unlock_all(l, cnt);
}

void series_rangeN(Series **l, int cnt, int64_t since_sec, Range *r)
{
    memset(r, 0, sizeof *r);
    lock_all(l, cnt);
    int64_t last = INT64_MIN, oldest = INT64_MAX; const Series *lead = NULL;
    for (int i = 0; i < cnt; i++) {
        if (!l[i]) continue;
        int64_t lm = last_minute(l[i]);
        if (lm > last) last = lm;
        if (!lead && lm != INT64_MIN) lead = l[i];
        if (l[i]->base_min < oldest) oldest = l[i]->base_min;
    }
    if (lead) r->price = lead->price;
    if (last != INT64_MIN) {
        int64_t first = floor_div(since_sec, 60);
        if (first < oldest) first = oldest;
        for (int64_t m = first; m <= last; m++) {
            const Bar *bb = pick(l, cnt, m, m >= last - 1);
            if (!bb) continue;
            if (r->n == 0 || bb->h > r->high) { r->high = bb->h; r->high_t = m * 60; }
            if (r->n == 0 || bb->l < r->low)  { r->low = bb->l;  r->low_t = m * 60; }
            r->n++;
        }
    }
    unlock_all(l, cnt);
}

void series_query2(Series *p, Series *b2, int tf, int n, int64_t now_ms, Out *out) { Series *l[2] = { p, b2 }; series_queryN(l, 2, tf, n, now_ms, out); }
void series_range2(Series *p, Series *b2, int64_t since_sec, Range *r) { Series *l[2] = { p, b2 }; series_rangeN(l, 2, since_sec, r); }

void series_query(Series *s, int tf, int n, int64_t now_ms, Out *out) { series_query2(s, NULL, tf, n, now_ms, out); }
