/*
 * agg.h -- rolling aggregation of the 100 ms candles of a collector's .bin file into 1-minute bars,
 * and of the bars into any whole number of minutes (1m 3m 5m 15m 30m 1h 4h 1d 1w).
 *
 * The file is a stream of 30-byte frames, no header:  '{'  28-byte payload  '}'
 * A candle payload (one per 100 ms, forward-filled):  float O, H, L, C, V, J;  uint32 N
 * A timestamp marker (once a minute) is a payload with N = 0xFFFFFFFF, V = -1, J = NaN, and the Unix time in
 * seconds in the first four bytes (the O field): the next candle begins at that time.
 * Between markers the candles are 100 ms apart; without one (the start of a file, or a stalled collector) the
 * file's modification time stands for the end.  A partial frame at the end is ignored until it is complete.
 * A collector that was killed in mid-write leaves a partial frame, after which the frames sit at another offset
 * (a few bytes later); the parser finds them again by the '{' ... '}' pattern of three frames in a row.
 */
#ifndef CANDLEWIN_AGG_H
#define CANDLEWIN_AGG_H

#include <stdint.h>
#include <pthread.h>

#define REC_SIZE      30                /* frame: '{' + 28 bytes + '}' */
#define REC_MS        100
#define SERIES_CAP    600000          /* minutes kept: 416 days, enough for 60 weekly bars */
#define MAX_BARS      500

typedef struct { float o, h, l, c, v; unsigned char has; } Bar;

typedef struct {
    char            path[512];
    Bar            *bars;             /* bars[i] is the minute base_min + i */
    int             cap;
    int64_t         base_min;
    int64_t         pos;              /* byte offset after the last frame folded at the tail: where to read next */
    int64_t         next_ms;          /* time of the next candle to come */
    int64_t         first_pos;        /* byte offset of the first frame folded so far */
    int64_t         first_ms;         /* time of the first candle folded */
    int             history_done;
    float           price;            /* the last close */
    pthread_mutex_t mu;
} Series;

typedef struct {
    int     n;
    int     complete;                 /* 1 when the older history has all been folded in */
    float   price;
    int64_t age_ms;                   /* now minus the file's modification time */
    int64_t t[MAX_BARS];              /* start of each bar, seconds since the epoch (UTC) */
    float   o[MAX_BARS], h[MAX_BARS], l[MAX_BARS], c[MAX_BARS], v[MAX_BARS];
} Out;

int  series_valid(const char *path);                              /* 1 if the end of the file holds frames */
int  series_open(Series *s, const char *path, int cap);          /* 0 on success */
void series_close(Series *s);
int  series_tail(Series *s, int64_t recent_recs);                /* fold the newest records, anchoring the clock on the file's mtime; 0 on success */
int  series_history(Series *s, int64_t nrecs);                   /* fold nrecs older records; returns 1 while there is more, 0 when done */
int  series_refresh(Series *s);                                  /* fold what the collector has appended since; 0 on success */
void series_query(Series *s, int tf_minutes, int n, int64_t now_ms, Out *out);
int  tf_minutes(const char *name);                               /* "5m" -> 5; 0 when not one of the nine */

#endif
