#!/usr/bin/env python3
"""candle-writer.py -- write candles in the on-disk bin formats candled reads, as a working example of candled-data(5).

    candle-writer.py --out DIR [--pair ZEC] [--minutes 180] [--price 100] [--seed 1]

makes, in DIR (made if need be):

    candles/ZEC.bin        the collector's stream of 30-byte frames: a candle for every 100 ms, a time marker every minute
    bins/ZECUSDT.bin       the websocket daemon's file of 80-byte one-minute records
    expected.json          the minute bars that were written, for checking what candled serves
    candled.conf           a configuration that points candled at both directories

and prints the last close.  The minutes end at the last whole minute before now, so the data are fresh.
Standard library only.

A candle supply of your own can be as small as this: write the frames, or the records, as the format says.
"""
import argparse, json, os, random, struct, time

MARKER_N = 0xFFFFFFFF            # a time marker: N = 0xFFFFFFFF, V = -1, J = NaN
TICKS_PER_MINUTE = 600           # one candle for each 100 ms


def minute_bars(n, price, seed, end_minute):
    """n minute bars (oldest first) of a random walk: [minute_start_seconds, o, h, l, c, v]"""
    rnd = random.Random(seed)
    bars = []
    for i in range(n):
        o = price
        c = o * (1 + rnd.gauss(0, 0.0008))
        top, bot = max(o, c), min(o, c)
        h = top * (1 + abs(rnd.gauss(0, 0.0004)))
        l = bot * (1 - abs(rnd.gauss(0, 0.0004)))
        v = 5 + rnd.random() * 20
        bars.append([(end_minute - n + i) * 60, o, h, l, c, v])
        price = c
    return bars


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def frame(o, h, l, c, v, j, n):
    """'{' + float O,H,L,C,V,J + uint32 N + '}' : 30 bytes, in the machine's byte order (little-endian on x86 and ARM)"""
    return b'{' + struct.pack('<ffffffI', o, h, l, c, v, j, n) + b'}'


def marker(seconds, millis=0):
    """a time marker: the time of the next candle.  Whole seconds go in the four bytes of O; the milliseconds, if any, as the payload of the NaN in J
    (payload = ms + 1, none = 0).  H, L and C are not used; V = -1 and N = 0xFFFFFFFF say that this is a marker."""
    j_bits = 0x7FC00000 | (millis + 1 if millis else 0)        # a quiet NaN, with the payload in its low bits
    return b'{' + struct.pack('<IffffII', seconds, 0.0, 0.0, 0.0, -1.0, j_bits, MARKER_N) + b'}'


def write_frames(path, bars):
    """per minute: a marker, then 600 candles that walk from the open to the close, the high on candle 100 and the low on candle 400,
    so that folding them gives back exactly the minute bar"""
    out = bytearray()
    for (t, o, h, l, c, v) in bars:
        out += marker(t)
        for k in range(TICKS_PER_MINUTE):
            a = o + (c - o) * k / TICKS_PER_MINUTE
            b = o + (c - o) * (k + 1) / TICKS_PER_MINUTE
            hi, lo = max(a, b), min(a, b)
            if k == 100: hi = max(hi, h)
            if k == 400: lo = min(lo, l)
            o_k = o if k == 0 else a
            c_k = c if k == TICKS_PER_MINUTE - 1 else b
            out += frame(o_k, hi, lo, c_k, v / TICKS_PER_MINUTE, 0.0, 1)
    out += marker(bars[-1][0] + 60)
    with open(path, 'wb') as f:
        f.write(out)


def write_bins80(path, bars):
    """int64 ts (seconds, a multiple of 60); five doubles: open, high, low, close, volume; three more doubles; int trades; 4 bytes padding"""
    with open(path, 'wb') as f:
        for (t, o, h, l, c, v) in bars:
            f.write(struct.pack('<q8di4x', t, o, h, l, c, v, 0.0, 0.0, 0.0, 600))


def main():
    ap = argparse.ArgumentParser(description="write candles in every format candled reads")
    ap.add_argument("--out", required=True)
    ap.add_argument("--pair", default="ZEC", help="the coin, in all the files (default ZEC)")
    ap.add_argument("--minutes", type=int, default=180)
    ap.add_argument("--price", type=float, default=100.0)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    end_minute = int(time.time()) // 60
    for d in ("candles", "bins"):
        os.makedirs(os.path.join(a.out, d), exist_ok=True)
    bars = minute_bars(a.minutes, a.price, a.seed, end_minute)
    write_frames(os.path.join(a.out, "candles", a.pair + ".bin"), bars)
    write_bins80(os.path.join(a.out, "bins", a.pair + "USDT.bin"), bars)
    with open(os.path.join(a.out, "expected.json"), "w") as f:
        json.dump({"pair": a.pair, "bars": bars}, f)
    with open(os.path.join(a.out, "candled.conf"), "w") as f:
        f.write('listen 127.0.0.1\nport 8766\ntls yes\ncandles "%s"\nlive-bins "%s"\n' % (os.path.join(a.out, "candles"), os.path.join(a.out, "bins")))
    print("last close %.6g" % bars[-1][4])


if __name__ == "__main__":
    main()
