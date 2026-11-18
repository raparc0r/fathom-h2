/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * rg-bench: how long volume levelling takes to measure a track, and whether the
 * faster measurement (app/audio/loudness.c) still gives what 0.8.3's did
 * (loudness_v1.c, a frozen copy). Runs on the H2 (fathom-rg-bench, static) and
 * on the host.
 *
 *   rg-bench [-p PASSES] [-k] FILE...
 *   rg-bench --selftest
 *
 * Each file is read in passes, each from the card (its pages are dropped from
 * the cache first, unless -k), default "read,decode,old,new":
 *   read    the file's bytes only, 64 KB at a time: what the card gives
 *   decode  decoded (decoder.c, as levelling does), nothing else
 *   old     decoded and measured the 0.8.3 way
 *   new     decoded and measured as now (what the player does)
 *   ref     decoded and measured in double precision (BS.1770 as libebur128
 *           does it): the yardstick. Slow in the H2's soft float.
 *   hash    decoded, and a checksum of every sample printed: the same file
 *           through two builds (say, with libFLAC built two ways) must give
 *           the same checksum, or a decoder changed the sound
 * For each: wall and CPU time, the time inside the measurement alone, the
 * loudness (LUFS), the gain levelling would store (-18 LUFS reference), and
 * the sample peak. The comparison at the end says whether old and new agree
 * to the bit.
 *
 * --selftest runs synthetic signals (sines, white and pink noise, at 44.1 to
 * 192 kHz, cut into odd lengths) through old, new and ref: exit 0 if old and
 * new agree exactly and both are within 0.05 dB of ref.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "audio/decoder.h"
#include "audio/loudness.h"
#include "loudness_ref.h"
#include "loudness_v1.h"

enum { P_READ, P_DECODE, P_OLD, P_NEW, P_REF, P_HASH, NPASS };
static const char *pass_name[NPASS] = {"read", "decode", "old", "new", "ref", "hash"};

static double now(clockid_t c)
{
    struct timespec t;
    clock_gettime(c, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

typedef struct {
    int ran, ok;
    double wall, cpu, meas;    /* seconds: all, CPU, inside the measurement */
    double lufs, peak, mb;
    uint32_t hash;
    long frames;
    unsigned rate;
    int channels, bits;
} result_t;

static void drop_cache(const char *path)
{
#ifdef POSIX_FADV_DONTNEED                /* (Linux; a Mac keeps the file cached) */
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd);
#else
    (void)path;
#endif
}

static void run_read(const char *path, result_t *r)
{
    static char buf[64 * 1024];
    double w0 = now(CLOCK_MONOTONIC), c0 = now(CLOCK_PROCESS_CPUTIME_ID);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    long long total = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) total += n;
    close(fd);
    r->wall = now(CLOCK_MONOTONIC) - w0;
    r->cpu = now(CLOCK_PROCESS_CPUTIME_ID) - c0;
    r->mb = (double)total / 1e6;
    r->ok = n == 0;
}

static void run_decode(const char *path, int pass, result_t *r)
{
    double w0 = now(CLOCK_MONOTONIC), c0 = now(CLOCK_PROCESS_CPUTIME_ID);
    r->hash = 2166136261u;
    audio_format_t f;
    decoder_t *d = decoder_open(path, &f);
    if (!d) return;
    r->rate = f.rate;
    r->channels = f.channels;
    r->bits = f.bits;
    loudness_t *ln = NULL;
    v1_loudness_t *lo = NULL;
    loudness_ref_t *lr = NULL;
    if (pass == P_NEW && !(ln = loudness_new(f.rate, f.channels))) goto out;
    if (pass == P_OLD && !(lo = v1_loudness_new(f.rate, f.channels))) goto out;
    if (pass == P_REF && !(lr = loudness_ref_new(f.rate, f.channels))) goto out;
    static int32_t buf[4096 * 2];       /* as levelling reads (device/main.c, rg_measure) */
    long n;
    while ((n = decoder_read(d, buf, 4096)) > 0) {
        r->frames += n;
        if (pass == P_DECODE) continue;
        if (pass == P_HASH) {                     /* FNV-1a over the samples' bytes */
            for (long i = 0; i < n * f.channels; i++)
                for (int b = 0; b < 32; b += 8) r->hash = (r->hash ^ (((uint32_t)buf[i] >> b) & 0xff)) * 16777619u;
            continue;
        }
        double m0 = now(CLOCK_MONOTONIC);
        if (ln) loudness_add(ln, buf, n);
        else if (lo) v1_loudness_add(lo, buf, n);
        else loudness_ref_add(lr, buf, n);
        r->meas += now(CLOCK_MONOTONIC) - m0;
    }
    double m0 = now(CLOCK_MONOTONIC);
    if (ln) { r->lufs = loudness_lufs(ln); r->peak = loudness_peak(ln); }
    if (lo) { r->lufs = v1_loudness_lufs(lo); r->peak = v1_loudness_peak(lo); }
    if (lr) { r->lufs = loudness_ref_lufs(lr); r->peak = loudness_ref_peak(lr); }
    r->meas += now(CLOCK_MONOTONIC) - m0;
    r->ok = n == 0;
out:
    loudness_free(ln);
    v1_loudness_free(lo);
    loudness_ref_free(lr);
    decoder_close(d);
    r->wall = now(CLOCK_MONOTONIC) - w0;
    r->cpu = now(CLOCK_PROCESS_CPUTIME_ID) - c0;
}

static int bench(const char *path, const int *passes, int keep)
{
    result_t r[NPASS];
    memset(r, 0, sizeof r);
    printf("%s\n", path);
    for (int i = 0; passes[i] >= 0; i++) {
        int p = passes[i];
        if (!keep) drop_cache(path);
        memset(&r[p], 0, sizeof r[p]);          /* (a pass named twice: the second counts) */
        r[p].ran = 1;
        if (p == P_READ) run_read(path, &r[p]);
        else run_decode(path, p, &r[p]);
        if (!r[p].ok) { printf("  %-6s  failed\n", pass_name[p]); continue; }
        if (p == P_READ) {
            printf("  %-6s  %6.2f s wall  %6.2f s CPU  %.1f MB, %.1f MB/s\n", pass_name[p], r[p].wall, r[p].cpu,
                   r[p].mb, r[p].mb / r[p].wall);
            continue;
        }
        double secs = r[p].rate ? (double)r[p].frames / r[p].rate : 0;
        printf("  %-6s  %6.2f s wall  %6.2f s CPU", pass_name[p], r[p].wall, r[p].cpu);
        if (p == P_HASH) printf("  checksum %08x, %ld frames\n", r[p].hash, r[p].frames);
        else if (p == P_DECODE) printf("  %u Hz, %d ch, %d bits, %.1f s of audio (%.0fx real time)\n", r[p].rate,
                                  r[p].channels, r[p].bits, secs, r[p].wall > 0 ? secs / r[p].wall : 0);
        else printf("  measuring %6.2f s  %8.3f LUFS  gain %+7.3f dB  peak %.6f\n", r[p].meas, r[p].lufs,
                    isinf(r[p].lufs) ? 0.0 : LOUDNESS_REFERENCE - r[p].lufs, r[p].peak);
    }
    int bad = 0;
    if (r[P_OLD].ok && r[P_NEW].ok) {
        int same = r[P_OLD].lufs == r[P_NEW].lufs && r[P_OLD].peak == r[P_NEW].peak;
        printf("  old vs new: %s (%+.6f dB, peak %+.6f)", same ? "identical" : "DIFFERENT",
               r[P_NEW].lufs - r[P_OLD].lufs, r[P_NEW].peak - r[P_OLD].peak);
        if (r[P_NEW].meas > 0) printf("; measuring %.1fx faster, the whole pass %.2fx", r[P_OLD].meas / r[P_NEW].meas,
                                      r[P_OLD].wall / r[P_NEW].wall);
        printf("\n");
        bad |= !same;
    }
    if (r[P_REF].ok && r[P_NEW].ok)
        printf("  new vs ref: %+.4f dB, peak %+.6f\n", r[P_NEW].lufs - r[P_REF].lufs, r[P_NEW].peak - r[P_REF].peak);
    return bad;
}

/* ---------- --selftest ---------- */

static uint32_t g_rng = 0x2545F491u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

typedef enum { SIG_SINE, SIG_WHITE, SIG_PINK, SIG_MIX } sigkind_t;

/* `secs` of a signal at `dbfs` (the sine's peak; the noises' RMS) into all
 * three measurements, fed in odd-sized pieces, quantised to `bits`. */
static int selftest_one(const char *name, sigkind_t sig, unsigned rate, int ch, int bits, double dbfs, double secs)
{
    loudness_t *ln = loudness_new(rate, ch);
    v1_loudness_t *lo = v1_loudness_new(rate, ch);
    loudness_ref_t *lr = loudness_ref_new(rate, ch);
    double amp = pow(10.0, dbfs / 20.0), b[2][7] = {{0}};
    long total = (long)(rate * secs), done = 0, piece = 1;
    int32_t buf[2 * 3000];
    while (done < total) {
        long n = piece < total - done ? piece : total - done;
        piece = piece * 7 % 2999 + 1;    /* 1, 8, 57, 400, 2801, ...: across sub-block ends */
        for (long i = 0; i < n; i++) {
            double t = (double)(done + i) / rate;
            for (int c = 0; c < ch; c++) {
                double v;
                if (sig == SIG_SINE) v = sin(2 * M_PI * 1000.0 * t);
                else if (sig == SIG_MIX) v = 0.5 * sin(2 * M_PI * 55.0 * t + c) + 0.3 * sin(2 * M_PI * 440.0 * t) +
                                             0.2 * sin(2 * M_PI * 7000.0 * t + 2 * c);
                else {
                    double w = ((double)rnd() / 4294967296.0 - 0.5) * 3.4641016;   /* RMS 1 */
                    if (sig == SIG_WHITE) v = w;
                    else {                    /* Paul Kellet's pink filter */
                        double *k = b[c];
                        k[0] = 0.99886 * k[0] + w * 0.0555179;
                        k[1] = 0.99332 * k[1] + w * 0.0750759;
                        k[2] = 0.96900 * k[2] + w * 0.1538520;
                        k[3] = 0.86650 * k[3] + w * 0.3104856;
                        k[4] = 0.55000 * k[4] + w * 0.5329522;
                        k[5] = -0.7616 * k[5] - w * 0.0168980;
                        v = (k[0] + k[1] + k[2] + k[3] + k[4] + k[5] + k[6] + w * 0.5362) * 0.2;
                        k[6] = w * 0.115926;
                    }
                }
                v *= amp;
                if (v > 0.999999) v = 0.999999;
                if (v < -1.0) v = -1.0;
                int32_t q = (int32_t)lrint(v * (double)(1 << (bits - 1)));
                buf[i * ch + c] = (int32_t)((uint32_t)q << (32 - bits));
            }
        }
        loudness_add(ln, buf, n);
        v1_loudness_add(lo, buf, n);
        loudness_ref_add(lr, buf, n);
        done += n;
    }
    double vn = loudness_lufs(ln), vo = v1_loudness_lufs(lo), vr = loudness_ref_lufs(lr);
    double pn = loudness_peak(ln), po = v1_loudness_peak(lo), pr = loudness_ref_peak(lr);
    int same = vn == vo && pn == po;
    int close = fabs(vn - vr) < 0.05 && fabs(pn - pr) < 1.0 / 8388608.0 * 2;
    printf("  %-10s %6u Hz %d ch %2d bit %+6.1f dB: new %9.4f  old %9.4f  ref %9.4f LUFS  (new-ref %+.4f)  peak %.6f  %s%s\n",
           name, rate, ch, bits, dbfs, vn, vo, vr, vn - vr, pn, same ? "identical" : "DIFFERENT",
           close ? "" : "  FAR FROM REF");
    loudness_free(ln);
    v1_loudness_free(lo);
    loudness_ref_free(lr);
    return !(same && close);
}

static int selftest(void)
{
    static const unsigned rates[] = {44100, 48000, 96000, 192000};
    int bad = 0;
    for (int i = 0; i < 4; i++) {
        bad += selftest_one("sine", SIG_SINE, rates[i], 2, 24, -20, 10);
        bad += selftest_one("pink", SIG_PINK, rates[i], 2, 24, -20, 10);
    }
    bad += selftest_one("sine", SIG_SINE, 44100, 2, 16, -20, 10);
    bad += selftest_one("sine", SIG_SINE, 48000, 1, 24, -23, 10);
    bad += selftest_one("white", SIG_WHITE, 44100, 2, 16, -10, 10);
    bad += selftest_one("white", SIG_WHITE, 192000, 2, 24, -3, 5);
    bad += selftest_one("pink", SIG_PINK, 44100, 2, 16, -40, 10);
    bad += selftest_one("mix", SIG_MIX, 44100, 2, 16, -1, 10);
    bad += selftest_one("mix", SIG_MIX, 96000, 2, 24, 0, 5);
    bad += selftest_one("sine", SIG_SINE, 44100, 2, 24, 0, 3);
    printf(bad ? "selftest: %d FAILED\n" : "selftest: all passed\n", bad);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    int passes[NPASS + 1] = {P_READ, P_DECODE, P_OLD, P_NEW, -1}, keep = 0, i = 1;
    if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest();
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-k")) keep = 1;
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            int n = 0;
            char *list = argv[++i], *save = NULL;
            for (char *t = strtok_r(list, ",", &save); t && n < NPASS; t = strtok_r(NULL, ",", &save)) {
                int p = 0;
                while (p < NPASS && strcmp(t, pass_name[p])) p++;
                if (p == NPASS) { fprintf(stderr, "unknown pass %s\n", t); return 2; }
                passes[n++] = p;
            }
            passes[n] = -1;
        } else break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: %s [-p read,decode,old,new,ref,hash] [-k] FILE...\n       %s --selftest\n", argv[0], argv[0]);
        return 2;
    }
    int bad = 0;
    for (; i < argc; i++) bad |= bench(argv[i], passes, keep);
    return bad;
}
