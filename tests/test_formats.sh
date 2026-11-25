#! /bin/sh
# test_formats.sh -- the on-disk formats of candled-data(5): data written in them with contrib/candle-writer.py are read
# back through candled, each kind of file alone and both together.  Loopback, a random port.
#   CANDLED=path/to/candled srcdir=path/to/source sh tests/test_formats.sh
set -u
CANDLED=${CANDLED:-./candled}
srcdir=${srcdir:-.}
{ command -v python3 >/dev/null 2>&1 && command -v curl >/dev/null 2>&1; } || { echo "test_formats: python3 and curl are needed; skipped"; exit 77; }
T=$(mktemp -d)
PID=""
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null; wait 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
fails=0; n=0
ok()   { n=$((n + 1)); echo "ok $n - $1"; }
fail() { n=$((n + 1)); fails=$((fails + 1)); echo "not ok $n - $1"; }
check() { if eval "$2"; then ok "$1"; else fail "$1"; fi; }

python3 "$srcdir/contrib/candle-writer.py" --out "$T/d" --pair ZEC --minutes 120 >/dev/null || { echo "the writer failed"; exit 1; }
echo "# no settings" > "$T/empty.conf"

# start candled on a loopback port with the given sources; PORT is set; waits until it answers
start() {
    PORT=$(( 20000 + $(od -An -N2 -tu2 /dev/urandom | tr -d ' ') % 30000 ))
    "$CANDLED" --config "$T/empty.conf" --no-tls --bind 127.0.0.1 --port "$PORT" "$@" >"$T/log" 2>&1 &
    PID=$!
    i=0; while [ $i -lt 60 ]; do curl -s --max-time 1 "http://127.0.0.1:$PORT/pairs" >/dev/null 2>&1 && break; sleep 0.1; i=$((i + 1)); done
}
stop() { kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; PID=""; }

# compare what candled serves for the pair with what the writer wrote: tf minutes, the last NB bars
compare() {
python3 - "$PORT" "$T/d/expected.json" "$1" "$2" <<'EOF'
import json, sys, urllib.request
port, expfile, tf_name, nb = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
step = {"1m": 1, "5m": 5, "15m": 15, "1h": 60}[tf_name]
# the written minute bars [start seconds, o, h, l, c, v], gathered into bars of `step` minutes the way candled does it
agg, order = {}, []
for t, o, h, l, c, v in json.load(open(expfile))["bars"]:
    k = (t // 60) // step * step
    if k not in agg:
        agg[k] = [k * 60, o, h, l, c, v]; order.append(k)
    else:
        a = agg[k]; a[2] = max(a[2], h); a[3] = min(a[3], l); a[4] = c; a[5] += v
want = [agg[k] for k in order[1:]][-nb:]                      # not the oldest bar: it may be partial
got = json.load(urllib.request.urlopen("http://127.0.0.1:%s/candles?pair=ZEC&tf=%s&n=%d" % (port, tf_name, nb + 3), timeout=10))
index = {t: i for i, t in enumerate(got["t"])}
bad = 0
for w in want:
    if w[0] not in index:
        print("missing bar", w[0]); bad += 1; continue
    i = index[w[0]]
    for name, served, written in (("o", got["o"][i], w[1]), ("h", got["h"][i], w[2]), ("l", got["l"][i], w[3]), ("c", got["c"][i], w[4]), ("v", got["v"][i], w[5])):
        if abs(served - written) > 3e-4 * max(abs(written), 1):
            print("bar", w[0], name, "served", served, "written", written); bad += 1
sys.exit(1 if bad else 0)
EOF
}
pairs() { curl -s --max-time 5 "http://127.0.0.1:$PORT/pairs"; }

# 1. the stream of 30-byte frames
start --dir "$T/d/candles"
check "frames: the pair is listed from the file name" 'pairs | grep -q "\"ZEC\""'
check "frames: the minute bars are what was written" 'compare 1m 5'
check "frames: five-minute bars are made from them" 'compare 5m 4'
check "frames: fifteen-minute and hourly bars too" 'compare 15m 3 && compare 1h 1'
stop

# 2. the websocket daemon's 80-byte records
start --dir "$T/d/none" --live-bins "$T/d/bins"
check "live-bins: the minute bars are what was written" 'compare 1m 5'
check "live-bins: its pairs are not listed by /pairs (only the files of --dir and --backfill are)" '[ "$(pairs)" = "[]" ]'
stop

# 3. both together: the same bars, whichever source answers
start --dir "$T/d/candles" --live-bins "$T/d/bins"
check "both together: the minute bars are what was written" 'compare 1m 5'
check "both together: the pair is listed" 'pairs | grep -q ZEC'
stop

# 4. a pair name that is not letters and digits, or too long, is refused
start --dir "$T/d/candles"
check "a bad pair name is refused (400)" '[ "$(curl -s -o /dev/null -w %{http_code} "http://127.0.0.1:$PORT/candles?pair=../etc&tf=1m&n=1")" = 400 ]'
stop

[ $fails = 0 ] && echo "all $n format checks passed" || echo "$fails of $n format checks failed"
[ $fails = 0 ]
