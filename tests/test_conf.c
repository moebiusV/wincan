/* test_conf.c -- candled.conf(5): the parser accepts the files in tests/conf/accept, reads their values,
 * and refuses the files in tests/conf/reject.  Run from `make check` (srcdir is the source directory). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "candled_conf.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static const char *sd;
static char path[512];
static const char *at(const char *dir, const char *name) { snprintf(path, sizeof path, "%s/tests/conf/%s/%s", sd, dir, name); return path; }

int main(void)
{
    static const char *accept[] = { "empty.conf", "minimal.conf", "full.conf", "continuation.conf" };
    static const char *reject[] = { "unknown-directive.conf", "port-too-big.conf", "port-zero.conf", "bad-fingerprint.conf",
                                    "missing-argument.conf", "unterminated-string.conf", "bad-flag.conf" };
    struct candled_conf c;
    size_t i;
    sd = getenv("srcdir"); if (!sd) sd = ".";

    for (i = 0; i < sizeof accept / sizeof *accept; i++) {
        candled_conf_init(&c);
        CHECK(candled_conf_parse(at("accept", accept[i]), &c) == 0, "%s should be accepted", accept[i]);
        candled_conf_free(&c);
    }
    for (i = 0; i < sizeof reject / sizeof *reject; i++) {
        candled_conf_init(&c);
        fprintf(stderr, "(expected error) ");
        CHECK(candled_conf_parse(at("reject", reject[i]), &c) == -1, "%s should be refused", reject[i]);
        candled_conf_free(&c);
    }

    candled_conf_init(&c);
    CHECK(c.tls == -1 && c.port == 0 && c.listen_addr == NULL, "init leaves everything unset");
    CHECK(candled_conf_parse(at("accept", "full.conf"), &c) == 0, "full.conf");
    CHECK(c.listen_addr && !strcmp(c.listen_addr, "127.0.0.1"), "listen");
    CHECK(c.port == 8766, "port %d", c.port);
    CHECK(c.tls == 1 && c.insecure == 0 && c.official_klines == 1, "flags %d %d %d", c.tls, c.insecure, c.official_klines);
    CHECK(c.certificate && !strcmp(c.certificate, "/etc/candled/server.crt"), "certificate");
    CHECK(c.key && !strcmp(c.key, "/etc/candled/server.key"), "key");
    CHECK(c.authorized && !strcmp(c.authorized, "/etc/candled/authorized-clients"), "authorized");
    CHECK(c.nauthorize == 2, "two authorize lines, got %zu", c.nauthorize);
    CHECK(c.nauthorize == 2 && !strncmp(c.authorize[1], "SHA256:FEDCBA", 13), "the second fingerprint");
    CHECK(c.candles && !strcmp(c.candles, "/var/lib/candled/candles"), "candles");
    CHECK(c.backfill && !strcmp(c.backfill, "/var/lib/candled/candles-spot"), "backfill");
    CHECK(c.database && !strcmp(c.database, "/var/lib/candled/candles.db"), "database");
    CHECK(c.live_bins && !strcmp(c.live_bins, "/var/lib/candled/bins"), "live-bins");
    CHECK(c.heat_db && !strcmp(c.heat_db, "/var/lib/candled/heat.db"), "heat-db");
    candled_conf_free(&c);

    candled_conf_init(&c);
    CHECK(candled_conf_parse(at("accept", "continuation.conf"), &c) == 0, "continuation.conf");
    CHECK(c.listen_addr && !strcmp(c.listen_addr, "*"), "the wildcard address");
    CHECK(c.candles && !strcmp(c.candles, "/var/lib/candled/candles"), "a continued line");
    candled_conf_free(&c);

    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("all %d config checks passed\n", (int)(sizeof accept / sizeof *accept + sizeof reject / sizeof *reject) + 18);
    return 0;
}
