#include <stdio.h>
#include <math.h>
#include "../src/heat.h"
static int fails, n;
#define CHECK(c, what) do { n++; if (!(c)) { fails++; printf("FAIL %s\n", what); } } while (0)
int main(void)
{
    HeatIn in = { .oi_usd = 1000, .ls = 1, .bid = { 100, 200, 500, 1000, 2000 }, .ask = { 100, 200, 500, 1000, 2000 } };
    HeatOut o; heat_model(&in, &o);
    double sl = 0, ss = 0; for (int i = 0; i < HEAT_N; i++) { sl += o.liq_long[i]; ss += o.liq_short[i]; }
    CHECK(fabs(sl - 1000) < 1e-9 && fabs(ss - 1000) < 1e-9, "ratio 1: each side has the whole open interest");
    CHECK(fabs(o.cog_lin) < 1e-9 && fabs(o.cog_newton) < 1e-9, "a balanced book has its centre of gravity at the price");
    CHECK(fabs(o.liq_long[0] - 100) < 1e-9 && fabs(o.liq_long[2] - 250) < 1e-9, "tier amounts follow the weights");
    CHECK(fabs(o.press_long[0] - 1.0) < 1e-9 && fabs(o.press_long[4] - 0.5) < 1e-9, "pressure: cumulative liquidations over depth (100/100, 1000/2000)");
    in.ls = 3;                                                      /* longs crowded: 3 to 1 */
    heat_model(&in, &o);
    sl = ss = 0; for (int i = 0; i < HEAT_N; i++) { sl += o.liq_long[i]; ss += o.liq_short[i]; }
    CHECK(fabs(sl - 1500) < 1e-9 && fabs(ss - 500) < 1e-9, "ratio 3: longs 1500, shorts 500");
    CHECK(o.cog_lin < 0 && o.cog_newton < 0, "crowded longs pull the centre of gravity below the price");
    CHECK(fabs(o.cog_newton) < fabs(o.cog_lin), "the Newtonian centre sits nearer the price (the near tiers weigh more)");
    in.ls = -1; heat_model(&in, &o); CHECK(fabs(o.cog_lin) < 1e-9, "an unknown ratio is taken as 1");
    in.oi_usd = 0; heat_model(&in, &o); CHECK(o.cog_lin == 0 && o.cog_newton == 0, "no open interest, no pull");
    printf(fails ? "%d checks, %d FAILED\n" : "all %d checks passed\n", n, fails ? fails : 0);
    if (fails) return 1;
    return 0;
}
