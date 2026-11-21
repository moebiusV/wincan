/* heat.c -- see heat.h */
#include "heat.h"

const double HEAT_PCT[HEAT_N] = { 1, 2, 5, 10, 20 };
const double HEAT_TIER_WEIGHT[HEAT_N] = { 0.10, 0.15, 0.25, 0.30, 0.20 };

void heat_model(const HeatIn *in, HeatOut *out)
{
    double ls = in->ls > 0 ? in->ls : 1.0;
    double lf = ls / (1.0 + ls);                        /* the long share of the accounts */
    double long_oi = 2.0 * lf * in->oi_usd, short_oi = 2.0 * (1.0 - lf) * in->oi_usd;   /* a ratio of 1 gives each side the whole open interest */
    double cl = 0, cs = 0, sw = 0, nw = 0, sx = 0, nx = 0;
    for (int i = 0; i < HEAT_N; i++) {
        out->liq_long[i] = long_oi * HEAT_TIER_WEIGHT[i];
        out->liq_short[i] = short_oi * HEAT_TIER_WEIGHT[i];
        cl += out->liq_long[i]; cs += out->liq_short[i];
        out->press_long[i] = in->bid[i] > 0 ? cl / in->bid[i] : 0;
        out->press_short[i] = in->ask[i] > 0 ? cs / in->ask[i] : 0;
        double m = out->liq_long[i] + out->liq_short[i], d = HEAT_PCT[i];
        double signed_x = (out->liq_short[i] - out->liq_long[i]);          /* the side's net mass at +d (shorts) minus at -d (longs) */
        sw += m; sx += d * signed_x;                                        /* sum of x_i m_i with x = -d for longs, +d for shorts */
        nw += m / (d * d); nx += (d * signed_x) / (d * d);
    }
    out->cog_lin = sw > 0 ? sx / sw : 0;
    out->cog_newton = nw > 0 ? nx / nw : 0;
}
