/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * alsa-steps: open, configure, prepare and start a playback stream one step at
 * a time, logging (and syncing) each step to a file first, with a pause after.
 * If the device resets, the log's last line names the step. Bring-up tool.
 *
 *   alsa-steps LOGFILE [RATE [BITS]]   plays a quiet 1 kHz tone (-60 dBFS) for 2 s;
 *                                      BITS 16 (S16_LE, default) or 24 (S24_LE)
 *   alsa-steps LOGFILE RATE BITS SECS  just play the tone for SECS seconds (no pauses)
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <tinyalsa/asoundlib.h>

static FILE *lf;

static void step(const char *fmt, ...)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    va_list ap;
    va_start(ap, fmt);
    fprintf(lf, "[%ld.%03ld] ", (long)t.tv_sec, t.tv_nsec / 1000000);
    vfprintf(lf, fmt, ap);
    fputc('\n', lf);
    va_end(ap);
    fflush(lf);
    fsync(fileno(lf));
    sync();
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s LOGFILE [RATE]\n", argv[0]); return 2; }
    unsigned rate = argc > 2 ? (unsigned)atoi(argv[2]) : 44100;
    int bits = argc > 3 ? atoi(argv[3]) : 16;
    int secs = argc > 4 ? atoi(argv[4]) : 0;
    lf = fopen(argv[1], "a");
    if (!lf) { perror(argv[1]); return 1; }

    struct pcm_config cfg = {
        .channels = 2, .rate = rate, .period_size = 1024, .period_count = 4,
        .format = bits == 24 ? PCM_FORMAT_S24_LE : PCM_FORMAT_S16_LE,
        .start_threshold = 0, .stop_threshold = 0,
    };
    if (secs > 0) {
        struct pcm *p = pcm_open(0, 0, PCM_OUT, &cfg);
        if (!p || !pcm_is_ready(p)) { step("pcm_open failed: %s", p ? pcm_get_error(p) : "?"); return 1; }
        unsigned n = cfg.period_size;
        void *b = malloc(n * 2 * sizeof(int32_t));
        unsigned long ph = 0;
        step("tone: %u Hz, %d-bit, %d s", rate, bits, secs);
        for (unsigned long done = 0; done < (unsigned long)secs * rate; done += n) {
            for (unsigned i = 0; i < n; i++, ph++) {
                double x = 0.001 * sin(2 * M_PI * 1000.0 * ph / rate);
                if (bits == 24) ((int32_t *)b)[2 * i] = ((int32_t *)b)[2 * i + 1] = (int32_t)(8388607 * x);
                else ((int16_t *)b)[2 * i] = ((int16_t *)b)[2 * i + 1] = (int16_t)(32767 * x);
            }
            int w = pcm_writei(p, b, n);
            if (done < 8 * n || w != (int)n)
                step("write @%lu -> %d (%s)", done, w, w < 0 ? pcm_get_error(p) : "ok");
            if (w < 0) break;
        }
        step("tone done");
        pcm_close(p);
        return 0;
    }

    step("1 pcm_open (hw_params: MCLK via the clock framework; sw_params), rate %u, %d-bit", rate, bits);
    sleep(1);
    struct pcm *pcm = pcm_open(0, 0, PCM_OUT, &cfg);
    if (!pcm || !pcm_is_ready(pcm)) { step("pcm_open failed: %s", pcm ? pcm_get_error(pcm) : "?"); return 1; }
    step("1 done");
    sleep(1);

    step("2 pcm_prepare (DAPM powers amplifier and switch)");
    sleep(1);
    if (pcm_prepare(pcm)) { step("prepare failed: %s", pcm_get_error(pcm)); return 1; }
    step("2 done");
    sleep(1);

    unsigned n = cfg.period_size * 2;
    void *buf = malloc(n * 2 * sizeof(int32_t));
    for (unsigned i = 0; i < n; i++) {
        double x = 0.001 * sin(2 * M_PI * 1000 * i / rate);   /* -60 dBFS */
        if (bits == 24) {
            int32_t v = (int32_t)(8388607 * x);               /* S24_LE: low 24 bits of 32 */
            ((int32_t *)buf)[2 * i] = ((int32_t *)buf)[2 * i + 1] = v;
        } else {
            int16_t v = (int16_t)(32767 * x);
            ((int16_t *)buf)[2 * i] = ((int16_t *)buf)[2 * i + 1] = v;
        }
    }

    step("3 first write (fills the buffer; start_threshold may trigger START: AIC + DMA)");
    sleep(1);
    int r = pcm_writei(pcm, buf, n);
    step("3 done (%d frames)", r);
    sleep(1);

    step("4 pcm_start (if not already running)");
    pcm_start(pcm);
    step("4 done");

    for (int i = 0; i < 40; i++) {
        r = pcm_writei(pcm, buf, n);
        if (r < 0) { step("write %d failed: %s", i, pcm_get_error(pcm)); break; }
        if (i % 10 == 0) step("5 streaming, write %d ok", i);
    }
    step("6 closing");
    pcm_close(pcm);
    step("6 done");
    return 0;
}
