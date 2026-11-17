/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fathom-bitperfect (on the H2): play files back to back through the player's
 * own engine and ALSA output, and record every byte written to the card.
 *
 *   fathom-bitperfect [-d /dev/snd/pcmCxD0p] TAP.raw FILE...
 *
 * The card's playback volume is set to its minimum first (the noise in the
 * test files is full scale) and put back after; the engine's gain stays 1.0.
 * A card with no volume control is refused unless BP_NO_VOLUME_OK=1 (after
 * turning the DAC down by hand).
 * For each track it prints the format the card runs at ("pcmC0D0p S24_LE
 * 96000"), which compare.py needs to read the tap. Stop the player first
 * (/etc/init.d/S90fathom stop): it holds the card.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio/engine.h"
#include "audio/mixer_alsa.h"
#include "audio/output.h"

static void nap(int ms)
{
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&t, NULL);
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/snd/pcmC0D0p";
    int a = 1;
    if (a + 1 < argc && !strcmp(argv[a], "-d")) { dev = argv[a + 1]; a += 2; }
    if (argc - a < 2) {
        fprintf(stderr, "usage: %s [-d /dev/snd/pcmCxD0p] TAP.raw FILE...\n", argv[0]);
        return 2;
    }
    const char *tap_path = argv[a++];
    char **files = argv + a;
    int nfiles = argc - a;

    /* the card's volume control, from the PCM node: pcmC1D0p -> controlC1 */
    char ctl[64] = "", vol[44] = "";
    const char *c = strstr(dev, "pcmC");
    if (c) snprintf(ctl, sizeof ctl, "/dev/snd/controlC%d", atoi(c + 4));
    mixer_t m = {.fd = -1};
    int saved = -1;
    if (ctl[0] && mixer_find_playback_volume(ctl, vol) == 0 && mixer_open(&m, ctl, vol) == 0) {
        saved = mixer_get(&m);
        mixer_set(&m, m.min);
        printf("volume: %s \"%s\" %d -> %d (min)\n", ctl, vol, saved, m.min);
    } else if (!getenv("BP_NO_VOLUME_OK")) {
        /* the files are full-scale noise: never play them at an unknown volume */
        fprintf(stderr, "no volume control on %s: turn the DAC all the way down, then run again with "
                        "BP_NO_VOLUME_OK=1\n", dev);
        return 1;
    }

    audio_output_t out;
    if (out_alsa_create(&out, dev)) { fprintf(stderr, "cannot open %s\n", dev); return 1; }
    FILE *tap = fopen(tap_path, "wb");
    if (!tap) { perror(tap_path); return 1; }
    out_alsa_set_tap(out.ctx, tap);

    engine_t *e = engine_new(&out);
    engine_play(e, files[0], 0);
    int next = 1;
    uint32_t seq = 0;
    for (;;) {
        eng_status_t st;
        engine_status(e, &st);
        if (st.seq != seq) {             /* a track started: what the card runs at */
            seq = st.seq;
            char d[96];
            nap(100);
            out.describe(out.ctx, d, sizeof d);
            printf("track %d: %s: %s\n", (int)st.track_id, files[st.track_id], d);
        }
        if (!st.next_queued && next < nfiles && st.state == ENG_PLAYING) {
            engine_queue_next(e, files[next], next);
            next++;
        }
        if (st.error) { fprintf(stderr, "cannot play %s\n", files[st.track_id]); break; }
        if (st.ended || (st.state == ENG_STOPPED && seq)) break;
        nap(20);
    }
    engine_free(e);
    fclose(tap);
    if (saved >= 0) mixer_set(&m, saved);
    mixer_close(&m);
    return 0;
}
