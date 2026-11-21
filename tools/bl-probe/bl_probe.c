/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * bl-probe: the H2's backlight PWM (X1000 TCU channel 0 on PC25) through
 * /dev/mem, to measure it and to try other PWM settings by eye without a new
 * kernel. Debug tool only.
 *
 *   bl-probe regs                 the channel's registers, decoded
 *   bl-probe scope [SECONDS]      sample the PC25 pin against the counter: duty
 *                                 seen on the pin, samples that disagree with
 *                                 the registers (glitches), register rewrites
 *   bl-probe phase [SECONDS]      the counter's phase against the system clock,
 *                                 sampled with the CPU idling in between: a
 *                                 jump means the PWM clock stopped or the
 *                                 counter was restarted
 *   bl-probe set PERIOD_NS PERMILLE [pclk|ext]
 *                                 one setting, kept until the kernel next
 *                                 writes the brightness
 *   bl-probe follow PERIOD_NS [pclk|ext] [SCALE]
 *                                 keep PERIOD_NS while following the kernel's
 *                                 brightness (its duty fraction, times SCALE,
 *                                 default 1), until killed; then the kernel's
 *                                 own setting is put back
 *
 * The kernel (pwm-jz4740 + pwm-backlight) programs a 30 us period from PCLK
 * (100.8 MHz, TDFR 3024: the counter runs 0..TDFR, so 3025 counts) with the
 * output high from count 0 to the duty value. The stock firmware programs the same period from EXT (24 MHz, 720
 * counts), low first and high for the last "duty" counts.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CPM_BASE  0x10000000u
#define TCU_BASE  0x10002000u
#define GPIO_BASE 0x10010000u

#define CPCCR     0x00
#define CPAPCR    0x10
#define CLKGR     0x20
#define LCR       0x04

#define TER       0x10
#define TESR      0x14
#define TECR      0x18
#define TSR       0x1c
#define TSCR      0x3c
#define TDFR0     0x40
#define TDHR0     0x44
#define TCNT0     0x48
#define TCSR0     0x4c

#define TCSR_PCLK      (1u << 0)
#define TCSR_RTC       (1u << 1)
#define TCSR_EXT       (1u << 2)
#define TCSR_SRC_MASK  0x7u
#define TCSR_PRESCALE(x) (((x) >> 3) & 7u)
#define TCSR_PWM_EN    (1u << 7)
#define TCSR_INITL     (1u << 8)
#define TCSR_SD        (1u << 9)

#define PC_PIN    (0x200 + 0x00)
#define PC25      (1u << 25)

#define EXT_HZ    24000000u

static volatile uint32_t *cpm, *tcu, *gpio;

static uint32_t rd(volatile uint32_t *b, unsigned off) { return b[off / 4]; }
static void wr(volatile uint32_t *b, unsigned off, uint32_t v) { b[off / 4] = v; }

static volatile uint32_t *map(int fd, uint32_t base)
{
    void *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    return p;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* APLL output: X1000 CPAPCR M[30:24] N[22:18] OD[17:16]: 24 MHz * (M+1) / (N+1) / 2^OD
 * (the H2 runs it at 1008 MHz). Only APLL is decoded: it's what PCLK uses. */
static double apll_hz(void)
{
    uint32_t v = rd(cpm, CPAPCR);
    unsigned m = ((v >> 24) & 0x7f) + 1, n = ((v >> 18) & 0x1f) + 1, od = 1u << ((v >> 16) & 3);
    return 24e6 * m / n / od;
}

static double pclk_hz(void)
{
    uint32_t v = rd(cpm, CPCCR);
    unsigned pdiv = ((v >> 16) & 0xf) + 1;
    /* SEL_H2PLL (bits 25:24) picks the AHB2/APB source: 1 = APLL */
    if (((v >> 24) & 3) != 1)
        return 0;
    return apll_hz() / pdiv;
}

static double src_hz(uint32_t tcsr)
{
    double base = (tcsr & TCSR_PCLK) ? pclk_hz() : (tcsr & TCSR_EXT) ? EXT_HZ : (tcsr & TCSR_RTC) ? 32768 : 0;
    return base / (1u << (2 * TCSR_PRESCALE(tcsr)));
}

/* High on the pin for this counter value, as the registers say (TCU1 mode:
 * the init level until the half match, then the other level until the full
 * match restarts the count). */
static int expected_high(uint32_t cnt, uint32_t tdhr, uint32_t tcsr)
{
    int init = !!(tcsr & TCSR_INITL);
    return cnt < tdhr ? init : !init;
}

static int cmd_regs(void)
{
    uint32_t tcsr = rd(tcu, TCSR0), tdfr = rd(tcu, TDFR0), tdhr = rd(tcu, TDHR0);
    double hz = src_hz(tcsr);
    int init = !!(tcsr & TCSR_INITL);
    printf("CPCCR %08x CPAPCR %08x CLKGR %08x (TCU clock %s) LCR %08x\n", rd(cpm, CPCCR), rd(cpm, CPAPCR),
           rd(cpm, CLKGR), (rd(cpm, CLKGR) >> 18) & 1 ? "gated" : "on", rd(cpm, LCR));
    printf("APLL %.1f MHz, PCLK %.2f MHz\n", apll_hz() / 1e6, pclk_hz() / 1e6);
    printf("TER %08x TSR %08x TCSR0 %08x TDFR0 %u TDHR0 %u TCNT0 %u PC25 %d\n", rd(tcu, TER), rd(tcu, TSR), tcsr,
           tdfr, tdhr, rd(tcu, TCNT0), !!(rd(gpio, PC_PIN) & PC25));
    printf("source %s /%u = %.3f MHz, PWM %s, init level %s, shutdown %s\n",
           tcsr & TCSR_PCLK ? "PCLK" : tcsr & TCSR_EXT ? "EXT" : tcsr & TCSR_RTC ? "RTC" : "none",
           1u << (2 * TCSR_PRESCALE(tcsr)), hz / 1e6, tcsr & TCSR_PWM_EN ? "on" : "off", init ? "high" : "low",
           tcsr & TCSR_SD ? "abrupt" : "graceful");
    if (hz > 0 && tdfr) {
        uint32_t high = init ? (tdhr < tdfr ? tdhr : tdfr) : (tdhr < tdfr ? tdfr - tdhr : 0);
        /* the counter runs 0..TDFR: TDFR + 1 counts a period (measured with "phase") */
        printf("period %.3f us (%.2f kHz), high %u counts = %.3f us = %.2f%% (= %.1f/255)\n", (tdfr + 1) / hz * 1e6,
               hz / (tdfr + 1) / 1e3, high, high / hz * 1e6, 100.0 * high / (tdfr + 1), 255.0 * high / (tdfr + 1));
    }
    return 0;
}

static int cmd_scope(double secs)
{
    uint32_t tcsr = rd(tcu, TCSR0), tdfr = rd(tcu, TDFR0), tdhr = rd(tcu, TDHR0);
    unsigned long long n = 0, used = 0, high = 0, bad = 0, wraps = 0, rewrites = 0, skipped = 0;
    unsigned long long bad_hist[16] = { 0 };
    uint32_t prev = rd(tcu, TCNT0), maxwin = 0;
    double t0 = now_s(), t = t0, first_bad = -1, last_bad = -1;

    printf("scope: TDFR %u TDHR %u TCSR %08x for %.1f s\n", tdfr, tdhr, tcsr, secs);
    while (t - t0 < secs) {
        for (int i = 0; i < 4096; i++) {
            uint32_t c1 = rd(tcu, TCNT0);
            uint32_t pin = rd(gpio, PC_PIN) & PC25;
            uint32_t c2 = rd(tcu, TCNT0);
            n++;
            if (c1 < prev)
                wraps++;
            prev = c2;
            if (c2 < c1) {          /* wrapped between the reads */
                skipped++;
                continue;
            }
            uint32_t win = c2 - c1;
            if (win > maxwin)
                maxwin = win;
            /* only samples whose window doesn't straddle an edge */
            if ((c1 <= tdhr && c2 + 2 >= tdhr) || c1 < 2 || c2 + 2 >= tdfr) {
                skipped++;
                continue;
            }
            used++;
            int h = !!pin, e = expected_high(c1, tdhr, tcsr);
            high += h;
            if (h != e) {
                bad++;
                bad_hist[(unsigned long long)c1 * 16 / (tdfr ? tdfr : 1) & 15]++;
                last_bad = now_s() - t0;
                if (first_bad < 0)
                    first_bad = last_bad;
            }
        }
        if (rd(tcu, TDFR0) != tdfr || rd(tcu, TDHR0) != tdhr || rd(tcu, TCSR0) != tcsr) {
            rewrites++;
            printf("  %.3f s: registers now TDFR %u TDHR %u TCSR %08x\n", now_s() - t0, rd(tcu, TDFR0),
                   rd(tcu, TDHR0), rd(tcu, TCSR0));
            tdfr = rd(tcu, TDFR0), tdhr = rd(tcu, TDHR0), tcsr = rd(tcu, TCSR0);
        }
        t = now_s();
    }
    printf("samples %llu (used %llu, at an edge or wrap %llu), widest read window %u counts\n", n, used, skipped,
           maxwin);
    printf("pin high in %.3f%% of the used samples (registers say %.3f%%)\n", used ? 100.0 * high / used : 0,
           tdfr ? 100.0 * (tcsr & TCSR_INITL ? tdhr : tdfr - tdhr) / tdfr : 0);
    printf("disagreeing samples %llu", bad);
    if (bad)
        printf(" (first at %.3f s, last at %.3f s)", first_bad, last_bad);
    printf("\n");
    if (bad) {
        printf("  by counter sixteenth:");
        for (int i = 0; i < 16; i++)
            printf(" %llu", bad_hist[i]);
        printf("\n");
    }
    printf("counter wraps seen %llu over %.2f s (%.1f /s; a period every %.2f us would be %.1f /s, "
           "reads are too slow to see all of them)\n",
           wraps, t - t0, wraps / (t - t0), tdfr / src_hz(tcsr) * 1e6, src_hz(tcsr) / (tdfr ? tdfr : 1));
    printf("register rewrites during the run %llu\n", rewrites);
    return bad ? 2 : 0;
}

/*
 * Phase of the free-running counter against the system OST (OST2, 1.5 MHz
 * from the 24 MHz crystal, like PCLK's PLL), sampled every ~2 ms with the CPU
 * idle in between. A steady PWM clock keeps the phase within the OST's tick
 * (67 counts); a clock that stops in idle, or a restarted counter, moves it.
 */
#define OST_BASE  0x12000000u
#define OST2CNTL  0x20
static volatile uint32_t *ost;

static int cmd_phase(double secs)
{
    uint32_t tcsr = rd(tcu, TCSR0), tdfr = rd(tcu, TDFR0) + 1;   /* counts 0..TDFR */
    double hz = src_hz(tcsr), ost_hz = 1.5e6;
    long jumps = 0, samples = 0, retries = 0;
    double worst = 0, sum = 0, ref = 0, prev = 0;
    uint32_t o0 = 0;
    uint64_t ticks = 0;
    if (!tdfr || hz <= 0) {
        fprintf(stderr, "phase: the PWM isn't running\n");
        return 1;
    }
    printf("phase: %.3f MHz, TDFR %u, against OST2 (%.1f MHz), every 2 ms for %.0f s\n", hz / 1e6, tdfr,
           ost_hz / 1e6, secs);
    for (;;) {
        uint32_t a, c, b;
        do {
            a = rd(ost, OST2CNTL);
            c = rd(tcu, TCNT0);
            b = rd(ost, OST2CNTL);
            if (b - a > 1)
                retries++;
        } while (b - a > 1);
        if (samples == 0)
            o0 = a;
        ticks = (uint32_t)(a - o0);     /* < 48 min: fits 32 bits */
        double t = (ticks + 0.5 * (b - a)) / ost_hz;
        double res = fmod(c - t * hz, tdfr);
        if (res < 0) res += tdfr;
        if (samples == 0)
            ref = res, prev = res;
        double rel = res - ref, step = res - prev;
        while (rel > tdfr / 2.0) rel -= tdfr;
        while (rel < -(double)tdfr / 2.0) rel += tdfr;
        while (step > tdfr / 2.0) step -= tdfr;
        while (step < -(double)tdfr / 2.0) step += tdfr;
        if (step > 150 || step < -150) {
            jumps++;
            if (jumps <= 10)
                printf("  %.3f s: phase moved %.0f counts (%.2f us)\n", t, step, step / hz * 1e6);
        }
        prev = res;
        if (getenv("BL_PHASE_RAW"))
            printf("raw %.7f %u %u %u\n", t, a - o0, b - a, c);
        if (fabs(rel) > worst)
            worst = fabs(rel);
        sum += rel;
        samples++;
        if (t >= secs)
            break;
        struct timespec d = { 0, 2000000 };
        nanosleep(&d, NULL);
    }
    printf("samples %ld (retried %ld), phase jumps > 150 counts: %ld, widest drift from the start %.0f counts "
           "(%.2f us), mean %.1f\n", samples, retries, jumps, worst, worst / hz * 1e6, sum / (samples ? samples : 1));
    return jumps ? 2 : 0;
}

/* Wait until the counter has just restarted, so new compare values take
 * effect from the next period without a long or short one in between. */
static void wait_period_start(void)
{
    uint32_t tdfr = rd(tcu, TDFR0);
    if (!(rd(tcu, TER) & 1) || tdfr < 64)
        return;
    double t0 = now_s();
    uint32_t prev = rd(tcu, TCNT0);
    while (now_s() - t0 < 0.01) {
        uint32_t c = rd(tcu, TCNT0);
        if (c < prev && c < tdfr / 8)
            return;
        prev = c;
    }
}

struct pwmset {
    uint32_t src;          /* TCSR_PCLK or TCSR_EXT */
    unsigned prescale;
    uint32_t tdfr, tdhr;
};

static struct pwmset compute(double period_ns, double frac, uint32_t src)
{
    struct pwmset s = { src, 0, 0, 0 };
    double hz = src == TCSR_EXT ? EXT_HZ : pclk_hz();
    double counts = period_ns * 1e-9 * hz;
    while (counts > 65536 && s.prescale < 5) {
        counts /= 4;
        s.prescale++;
    }
    uint32_t n = counts > 65536 ? 65536 : (uint32_t)(counts + 0.5);   /* counts a period: TDFR + 1 */
    s.tdfr = n - 1;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    s.tdhr = (uint32_t)(frac * n + 0.5);
    if (s.tdhr > s.tdfr) s.tdhr = s.tdfr;
    return s;
}

/* Program a setting with the kernel's polarity (init level high, high until
 * TDHR). Same source and prescaler: new values at a period start, counter
 * running. Otherwise the counter is stopped, reconfigured and restarted. */
static void apply(const struct pwmset *s)
{
    uint32_t tcsr = rd(tcu, TCSR0);
    uint32_t want = (tcsr & ~(TCSR_SRC_MASK | (7u << 3))) | s->src | (s->prescale << 3) | TCSR_PWM_EN | TCSR_INITL;
    if ((tcsr & (TCSR_SRC_MASK | (7u << 3))) == (want & (TCSR_SRC_MASK | (7u << 3))) && (rd(tcu, TER) & 1)) {
        wait_period_start();
        if (s->tdfr >= rd(tcu, TDFR0)) {
            wr(tcu, TDFR0, s->tdfr);
            wr(tcu, TDHR0, s->tdhr);
        } else {
            wr(tcu, TDHR0, s->tdhr);
            wr(tcu, TDFR0, s->tdfr);
        }
        wr(tcu, TCSR0, want);
        return;
    }
    wr(tcu, TECR, 1);
    wr(tcu, TCSR0, want);
    wr(tcu, TCNT0, 0);
    wr(tcu, TDHR0, s->tdhr);
    wr(tcu, TDFR0, s->tdfr);
    wr(tcu, TESR, 1);
}

static uint32_t src_arg(const char *s)
{
    if (!s || !strcmp(s, "pclk")) return TCSR_PCLK;
    if (!strcmp(s, "ext")) return TCSR_EXT;
    fprintf(stderr, "source: pclk or ext\n");
    exit(1);
}

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

/* the kernel's duty fraction from its registers (its polarity: high until TDHR) */
static double kernel_frac(uint32_t tdfr, uint32_t tdhr)
{
    return (double)(tdhr < tdfr ? tdhr : tdfr) / (tdfr + 1.0);
}

static int cmd_follow(double period_ns, uint32_t src, double scale)
{
    uint32_t k_tcsr = rd(tcu, TCSR0), k_tdfr = rd(tcu, TDFR0), k_tdhr = rd(tcu, TDHR0);
    /* the source and prescaler the kernel's clock framework believes in (it
     * only rewrites the prescaler, so its later writes may carry ours) */
    const uint32_t k_clk = k_tcsr & (TCSR_SRC_MASK | (7u << 3));
    struct pwmset cur = compute(period_ns, kernel_frac(k_tdfr, k_tdhr) * scale, src);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);
    printf("follow: period %.0f ns from %s: TDFR %u (prescale /%u), duty x %.2f; kernel's was TDFR %u TDHR %u "
           "TCSR %08x\n", period_ns, src == TCSR_EXT ? "EXT" : "PCLK", cur.tdfr, 1u << (2 * cur.prescale), scale,
           k_tdfr, k_tdhr, k_tcsr);
    fflush(stdout);
    apply(&cur);
    while (!stop) {
        struct timespec d = { 0, 100000000 };
        nanosleep(&d, NULL);
        uint32_t tcsr = rd(tcu, TCSR0), tdfr = rd(tcu, TDFR0), tdhr = rd(tcu, TDHR0);
        if (tdfr == cur.tdfr && tdhr == cur.tdhr && (tcsr & (TCSR_SRC_MASK | (7u << 3))) == (cur.src | (cur.prescale << 3)))
            continue;
        /* the kernel wrote the brightness: take its fraction, keep our period */
        k_tcsr = tcsr, k_tdfr = tdfr, k_tdhr = tdhr;
        cur = compute(period_ns, kernel_frac(tdfr, tdhr) * scale, src);
        apply(&cur);
        printf("  kernel wrote TDFR %u TDHR %u (%.1f/255): now TDFR %u TDHR %u\n", tdfr, tdhr,
               255 * kernel_frac(tdfr, tdhr), cur.tdfr, cur.tdhr);
        fflush(stdout);
    }
    /* back to what the kernel last programmed, from the same source */
    wr(tcu, TECR, 1);
    wr(tcu, TCSR0, (k_tcsr & ~(TCSR_SRC_MASK | (7u << 3))) | k_clk);
    wr(tcu, TCNT0, 0);
    wr(tcu, TDHR0, k_tdhr);
    wr(tcu, TDFR0, k_tdfr);
    wr(tcu, TESR, 1);
    printf("follow: stopped, kernel's TDFR %u TDHR %u TCSR %08x back\n", k_tdfr, k_tdhr, rd(tcu, TCSR0));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: bl-probe regs | scope [S] | phase [S] | set PERIOD_NS PERMILLE [pclk|ext] | "
                        "follow PERIOD_NS [pclk|ext] [SCALE]\n");
        return 1;
    }
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("/dev/mem");
        return 1;
    }
    cpm = map(fd, CPM_BASE);
    tcu = map(fd, TCU_BASE);
    gpio = map(fd, GPIO_BASE);
    ost = map(fd, OST_BASE);
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (!strcmp(argv[1], "regs"))
        return cmd_regs();
    if (!strcmp(argv[1], "scope"))
        return cmd_scope(argc > 2 ? atof(argv[2]) : 2);
    if (!strcmp(argv[1], "phase"))
        return cmd_phase(argc > 2 ? atof(argv[2]) : 10);
    if (!strcmp(argv[1], "set") && argc >= 4) {
        struct pwmset s = compute(atof(argv[2]), atof(argv[3]) / 1000.0, src_arg(argc > 4 ? argv[4] : NULL));
        apply(&s);
        return cmd_regs();
    }
    if (!strcmp(argv[1], "follow") && argc >= 3)
        return cmd_follow(atof(argv[2]), src_arg(argc > 3 ? argv[3] : NULL), argc > 4 ? atof(argv[4]) : 1);
    fprintf(stderr, "bl-probe: unknown command\n");
    return 1;
}
