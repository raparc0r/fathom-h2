/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* How long a request takes on the H2, through the player's own HTTP client
 * (FFmpeg and mbedTLS, the card's certificates): for timing TLS handshakes.
 *   fathom-httptime URL [TIMES]
 * Each try is a new connection (nothing is resumed): its time, and what came. */
#include "audio/ff_util.h"
#include "net/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <psa/crypto.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s URL [TIMES]\n", argv[0]); return 2; }
    int times = argc > 2 ? atoi(argv[2]) : 3;
    static char buf[256 * 1024];
    /* (HTTPTIME_PSA=1: mbedTLS's PSA layer started first, which is what lets
     * it offer TLS 1.3) */
    if (getenv("HTTPTIME_PSA")) printf("psa_crypto_init: %d\n", (int)psa_crypto_init());
    ff_set_ca_file("/mnt/sd/fathom/share/ca-certificates.crt");
    http_set_version("httptime");
    for (int i = 0; i < times; i++) {
        double t0 = now();
        long n = http_get(argv[1], buf, sizeof buf, NULL);
        printf("%d: %.0f ms, %ld bytes\n", i + 1, (now() - t0) * 1000, n);
        fflush(stdout);
    }
    return 0;
}
