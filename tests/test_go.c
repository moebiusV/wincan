#include <stdio.h>
#include <string.h>
#include "../src/gomatch.h"
static int fails, n;
#define CHECK(c, what) do { n++; if (!(c)) { fails++; printf("FAIL %s\n", what); } } while (0)
static int has(const char (*P)[24], const int *m, int cnt, const char *s) { for (int i = 0; i < cnt; i++) if (!strcmp(P[m[i]], s)) return i; return -1; }
int main(void)
{
    const char P[][24] = { "AAVE", "ADA", "BTC", "BTCDOM", "ETH", "ETC", "SBTC", "WBTC", "DOGE", "ZEC", "XRP", "TRX" };
    int N = (int)(sizeof P / sizeof P[0]), m[5], c;
    c = go_match(P, N, "", m); CHECK(c == 5 && m[0] == 0 && m[4] == 4, "nothing typed: the first five");
    c = go_match(P, N, "BTC", m); CHECK(c == 4 && !strcmp(P[m[0]], "BTC") && !strcmp(P[m[1]], "BTCDOM"), "exact first, then prefix matches");
    CHECK(has(P, m, c, "SBTC") == 2 || has(P, m, c, "SBTC") == 3, "then the pairs that only contain it");
    c = go_match(P, N, "ET", m); CHECK(c == 2 && !strcmp(P[m[0]], "ETH") && !strcmp(P[m[1]], "ETC"), "prefix matches in list order");
    c = go_match(P, N, "ZZ", m); CHECK(c == 0, "no match");
    c = go_match(P, N, "E", m); CHECK(c == 5 && !strcmp(P[m[0]], "ETH") && !strcmp(P[m[1]], "ETC"), "at most five; prefix matches before the ones that merely contain it");
    c = go_match(P, N, "ZEC", m); CHECK(c == 1 && !strcmp(P[m[0]], "ZEC"), "an exact pair alone");
    printf(fails ? "%d checks, %d FAILED\n" : "all %d checks passed\n", n, fails ? fails : 0);
    return fails != 0;
}
