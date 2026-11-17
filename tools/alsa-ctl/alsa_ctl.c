/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * alsa-ctl: list a card's mixer controls, or set a volume. Bring-up tool.
 *
 *   alsa-ctl CARD                    list every control (name, range, values, dB range)
 *   alsa-ctl CARD volume             the first "... Playback Volume" control: value and dB
 *   alsa-ctl CARD volume -40dB       set it (all channels); also a raw value, or min
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <sound/asound.h>

#include "audio/mixer_alsa.h"

static void show(const char *ctl, const char *name)
{
    mixer_t m;
    if (mixer_open(&m, ctl, name)) { printf("  %s: (not an integer control)\n", name); return; }
    long lo, hi;
    printf("  %s: %d (range %d..%d, %d ch)", name, mixer_get(&m), m.min, m.max, m.channels);
    if (mixer_db_range(&m, &lo, &hi) == 0) printf(", %.2f..%.2f dB", lo / 100.0, hi / 100.0);
    printf("\n");
    mixer_close(&m);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: alsa-ctl CARD [volume [VALUE|min|NdB]]\n"); return 2; }
    char ctl[32];
    snprintf(ctl, sizeof ctl, "/dev/snd/controlC%d", atoi(argv[1]));

    if (argc == 2) {
        int fd = open(ctl, O_RDONLY | O_CLOEXEC);
        if (fd < 0) { perror(ctl); return 1; }
        struct snd_ctl_card_info ci;
        memset(&ci, 0, sizeof ci);
        if (ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &ci) == 0) printf("card %s: %s (%s)\n", argv[1], ci.name, ci.longname);
        struct snd_ctl_elem_list list;
        memset(&list, 0, sizeof list);
        ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list);
        list.space = list.count;
        list.pids = calloc(list.count ? list.count : 1, sizeof *list.pids);
        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list)) { perror("ELEM_LIST"); return 1; }
        close(fd);
        for (unsigned i = 0; i < list.used; i++) show(ctl, (const char *)list.pids[i].name);
        return 0;
    }

    char name[44];
    if (mixer_find_playback_volume(ctl, name)) { fprintf(stderr, "no Playback Volume control\n"); return 1; }
    if (argc > 3) {
        mixer_t m;
        if (mixer_open(&m, ctl, name)) { fprintf(stderr, "%s: cannot open\n", name); return 1; }
        const char *v = argv[3];
        size_t l = strlen(v);
        long lo, hi;
        int raw;
        if (!strcmp(v, "min")) {
            raw = m.min;
        } else if (l > 2 && !strcmp(v + l - 2, "dB")) {
            if (mixer_db_range(&m, &lo, &hi)) { fprintf(stderr, "no dB range: give a raw value\n"); return 1; }
            raw = mixer_value_for_db(&m, (long)(atof(v) * 100));
        } else {
            raw = atoi(v);
        }
        if (mixer_set(&m, raw) < 0) { perror("set"); return 1; }
        mixer_close(&m);
    }
    show(ctl, name);
    return 0;
}
