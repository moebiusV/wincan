/* heat.h -- the modelled liquidation ladder behind wincan's heat strip.
 *
 * Positions are assumed to be spread over five leverage tiers (about 100x, 50x, 20x, 10x and 5x), whose liquidation prices lie about 1, 2, 5, 10 and
 * 20 per cent from the price (the author's levels).  The open interest is split between longs and shorts by the Binance account long/short ratio,
 * longs liquidating below the price and shorts above it.  The share of each side in each tier is an assumption (HEAT_TIER_WEIGHT), not data;
 * calibrating it against real liquidations is a later step.  The order book gives the other half: pressure = the liquidations up to a distance
 * divided by the order-book depth that must absorb them (bids for long liquidations, which are forced sells; asks for shorts, forced buys). */
#ifndef HEAT_H
#define HEAT_H

#define HEAT_N 5
extern const double HEAT_PCT[HEAT_N];           /* 1 2 5 10 20 */
extern const double HEAT_TIER_WEIGHT[HEAT_N];   /* the share of a side's open interest in each tier; sums to 1 */

typedef struct {
    double oi_usd;          /* open interest, quote currency (one side) */
    double ls;              /* long/short account ratio; <= 0 means unknown (taken as 1) */
    double bid[HEAT_N];     /* cumulative bid notional within 1 2 5 10 20 per cent */
    double ask[HEAT_N];
} HeatIn;

typedef struct {
    double liq_long[HEAT_N], liq_short[HEAT_N];     /* the liquidation notional of each tier */
    double press_long[HEAT_N], press_short[HEAT_N]; /* cumulative liquidations up to the tier / the book depth to the same distance */
    double cog_lin;         /* per cent from the price (negative: below): the mass-weighted mean position */
    double cog_newton;      /* the same, each cluster weighted by mass / distance^2: where the pull on a test mass at the price comes from */
} HeatOut;

void heat_model(const HeatIn *in, HeatOut *out);

#endif
