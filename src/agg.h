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
#ifndef WINCAN_AGG_H
#define WINCAN_AGG_H

#include <stdint.h>
#include <pthread.h>

#define REC_SIZE      30                /* frame: '{' + 28 bytes + '}' */
#define REC_MS        100
#define SERIES_CAP    600000          /* minutes kept: 416 days, enough for 60 weekly bars */
#define MAX_BARS      500

typedef struct { float o, h, l, c, v; unsigned char has, real; } Bar;      /* real: made from at least one candle with trades, not only forward-filled ones */

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
    pthread_mutex_t mu;               /* the bars */
    pthread_mutex_t rmu;              /* one refresh at a time */
    /* a series can instead be read from a SQLite table of 1-minute candles (candles_kucoin_futures of a database) */
    int             is_db;
    int             is_rest;          /* bars put in by hand: the exchange's official candles, fetched when a minute has closed */
    int             is_bin;           /* a file of 80-byte one-minute candles (the websocket daemon's data/candles/<PAIR>.bin): int64 ts, 8 doubles (open high low close volume ...), int trades, padding */
    char            dbpath[512], dbpair[32];
    void           *db;               /* sqlite3 * */
    int64_t         db_min_ts, db_last_ts, db_lo_ts;       /* seconds: the oldest row of the pair, the newest loaded, the oldest loaded */
    int64_t         last_check_ms;
    int64_t         partial_min;      /* the first minute of a file that began mid-minute: used only if no other source has that minute */
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
/* the same from two series: a minute is taken from the primary where it has one, else from the backfill (either may be NULL) */
/* the highest high and lowest low of the minutes from since_sec to now, and the minute (seconds) of each; n is the number of minutes with data */
typedef struct { double high, low; int64_t high_t, low_t; int n; float price; } Range;
void series_range2(Series *primary, Series *backfill, int64_t since_sec, int64_t now_ms, Range *r);
/* the same from any number of series, in order of priority */
void series_queryN(Series **list, int count, int tf_minutes, int n, int64_t now_ms, Out *out);
void series_rangeN(Series **list, int count, int64_t since_sec, int64_t now_ms, Range *r);
/* a series of 1-minute candles read from a SQLite database (table candles_kucoin_futures: pair, ts, open, high, low, close, volume), read-only; 0 on success */
int  series_open_db(Series *s, const char *dbpath, const char *dbpair, int cap);
/* a file of 80-byte one-minute candles (closed minutes only, appended by a websocket daemon); only the newest days are read; 0 on success */
int  series_open_bin(Series *s, const char *path, int cap);
/* a series that is filled with series_put: the exchange's own final candles, as soon as it has them; cap in minutes */
int  series_open_rest(Series *s, int cap_minutes);
void series_put(Series *s, int64_t minute, double o, double h, double l, double c, double v);
void series_query2(Series *primary, Series *backfill, int tf_minutes, int n, int64_t now_ms, Out *out);
int  tf_minutes(const char *name);                               /* "5m" -> 5; 0 when not one of the nine */

#endif
