/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Hardware volume through an ALSA mixer control (the ES9018K2M's own
 * attenuator on the H2): kernel control ioctls only, no alsa-lib.
 */
#include "audio/mixer_alsa.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <sound/asound.h>
#include <sound/tlv.h>

static void elem_id(struct snd_ctl_elem_id *id, const char *name)
{
    memset(id, 0, sizeof *id);
    id->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    snprintf((char *)id->name, sizeof id->name, "%s", name);
}

int mixer_open(mixer_t *m, const char *ctl_dev, const char *control)
{
    memset(m, 0, sizeof *m);
    m->fd = open(ctl_dev ? ctl_dev : "/dev/snd/controlC0", O_RDWR | O_CLOEXEC);
    if (m->fd < 0) return -1;
    snprintf(m->name, sizeof m->name, "%s", control);
    struct snd_ctl_elem_info info;
    memset(&info, 0, sizeof info);
    elem_id(&info.id, control);
    if (ioctl(m->fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) || info.type != SNDRV_CTL_ELEM_TYPE_INTEGER) {
        close(m->fd);
        m->fd = -1;
        return -1;
    }
    m->min = (int)info.value.integer.min;
    m->max = (int)info.value.integer.max;
    m->channels = (int)info.count;
    return 0;
}

int mixer_get(mixer_t *m)
{
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    elem_id(&v.id, m->name);
    if (m->fd < 0 || ioctl(m->fd, SNDRV_CTL_IOCTL_ELEM_READ, &v)) return -1;
    return (int)v.value.integer.value[0];
}

int mixer_set(mixer_t *m, int value)
{
    if (m->fd < 0) return -1;
    if (value < m->min) value = m->min;
    if (value > m->max) value = m->max;
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    elem_id(&v.id, m->name);
    for (int i = 0; i < m->channels && i < 128; i++) v.value.integer.value[i] = value;
    return ioctl(m->fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) ? -1 : value;
}

void mixer_close(mixer_t *m)
{
    if (m->fd >= 0) close(m->fd);
    m->fd = -1;
}

int mixer_db_range(mixer_t *m, long *min_cdb, long *max_cdb)
{
    if (m->fd < 0) return -1;
    struct { struct snd_ctl_tlv hdr; unsigned int data[32]; } t;
    memset(&t, 0, sizeof t);
    struct snd_ctl_elem_info info;
    memset(&info, 0, sizeof info);
    elem_id(&info.id, m->name);
    if (ioctl(m->fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info)) return -1;
    t.hdr.numid = info.id.numid;
    t.hdr.length = sizeof t.data;
    if (ioctl(m->fd, SNDRV_CTL_IOCTL_TLV_READ, &t)) return -1;
    unsigned type = t.data[0], len = t.data[1];
    const int *v = (const int *)&t.data[2];
    if (len < 8) return -1;
    switch (type) {
    case SNDRV_CTL_TLVT_DB_SCALE:               /* min, step (low 16 bits) */
        *min_cdb = v[0];
        *max_cdb = v[0] + (long)(v[1] & 0xffff) * (m->max - m->min);
        return 0;
    case SNDRV_CTL_TLVT_DB_MINMAX:
    case SNDRV_CTL_TLVT_DB_MINMAX_MUTE:
    case SNDRV_CTL_TLVT_DB_LINEAR:              /* not linear in dB, but close enough to start safe */
        *min_cdb = v[0];
        *max_cdb = v[1];
        return 0;
    }
    return -1;
}

int mixer_value_for_db(mixer_t *m, long cdb)
{
    long lo, hi;
    if (mixer_db_range(m, &lo, &hi) || hi <= lo) return -1;
    if (cdb <= lo) return m->min;
    if (cdb >= hi) return m->max;
    return m->min + (int)(((cdb - lo) * (long)(m->max - m->min) + (hi - lo) / 2) / (hi - lo));
}

long mixer_db_for_value(mixer_t *m, int value)
{
    long lo, hi;
    if (mixer_db_range(m, &lo, &hi) || m->max <= m->min) return 0;
    return lo + (hi - lo) * (long)(value - m->min) / (m->max - m->min);
}

int mixer_find_playback_volume(const char *ctl_dev, char *name)
{
    int fd = open(ctl_dev ? ctl_dev : "/dev/snd/controlC0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    struct snd_ctl_elem_list list;
    memset(&list, 0, sizeof list);
    int found = -1;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) == 0 && list.count) {
        list.space = list.count;
        list.pids = calloc(list.count, sizeof *list.pids);
        if (list.pids && ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) == 0) {
            for (unsigned i = 0; i < list.used && found < 0; i++) {
                const char *n = (const char *)list.pids[i].name;
                size_t l = strlen(n);
                if (list.pids[i].iface != SNDRV_CTL_ELEM_IFACE_MIXER || l < 16 ||
                    strcmp(n + l - 16, " Playback Volume"))
                    continue;
                struct snd_ctl_elem_info info;
                memset(&info, 0, sizeof info);
                info.id = list.pids[i];
                if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == 0 && info.type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
                    snprintf(name, 44, "%s", n);
                    found = 0;
                }
            }
        }
        free(list.pids);
    }
    close(fd);
    return found;
}

static int enum_info(int fd, const char *control, struct snd_ctl_elem_info *info, unsigned item)
{
    memset(info, 0, sizeof *info);
    elem_id(&info->id, control);
    info->value.enumerated.item = item;
    return ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, info) || info->type != SNDRV_CTL_ELEM_TYPE_ENUMERATED ? -1 : 0;
}

int mixer_enum_count(const char *ctl_dev, const char *control)
{
    int fd = open(ctl_dev ? ctl_dev : "/dev/snd/controlC0", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct snd_ctl_elem_info info;
    int n = enum_info(fd, control, &info, 0) ? -1 : (int)info.value.enumerated.items;
    close(fd);
    return n;
}

int mixer_enum_get(const char *ctl_dev, const char *control, char *name, int cap)
{
    int fd = open(ctl_dev ? ctl_dev : "/dev/snd/controlC0", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    elem_id(&v.id, control);
    int item = -1;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) == 0) {
        item = (int)v.value.enumerated.item[0];
        struct snd_ctl_elem_info info;
        if (name && cap > 0 && enum_info(fd, control, &info, (unsigned)item) == 0)
            snprintf(name, (size_t)cap, "%s", info.value.enumerated.name);
    }
    close(fd);
    return item;
}

int mixer_enum_set(const char *ctl_dev, const char *control, int item)
{
    int fd = open(ctl_dev ? ctl_dev : "/dev/snd/controlC0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    elem_id(&v.id, control);
    v.value.enumerated.item[0] = (unsigned)item;
    int r = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) ? -1 : 0;
    close(fd);
    return r;
}

int mixer_ctl_set(const char *ctl_dev, const char *control, int value)
{
    int fd = open(ctl_dev ? ctl_dev : "/dev/snd/controlC0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    struct snd_ctl_elem_info info;
    memset(&info, 0, sizeof info);
    elem_id(&info.id, control);
    int r = -1;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == 0) {
        struct snd_ctl_elem_value v;
        memset(&v, 0, sizeof v);
        v.id = info.id;
        for (unsigned i = 0; i < info.count && i < 128; i++) {
            if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) v.value.enumerated.item[i] = (unsigned)value;
            else v.value.integer.value[i] = value;
        }
        if (info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN || info.type == SNDRV_CTL_ELEM_TYPE_INTEGER ||
            info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED)
            r = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) ? -1 : 0;
    }
    close(fd);
    return r;
}

void mixer_dump(const char *ctl_dev, FILE *out)
{
    int fd = open(ctl_dev ? ctl_dev : "/dev/snd/controlC0", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { fprintf(out, "mixer: cannot open %s\n", ctl_dev ? ctl_dev : "/dev/snd/controlC0"); return; }
    struct snd_ctl_card_info card;
    memset(&card, 0, sizeof card);
    if (ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &card) == 0)
        fprintf(out, "mixer: card %d '%s' '%s' driver '%s'\n", card.card, card.id, card.name, card.driver);
    struct snd_ctl_elem_list list;
    memset(&list, 0, sizeof list);
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) == 0 && list.count) {
        list.space = list.count;
        list.pids = calloc(list.count, sizeof *list.pids);
        if (list.pids && ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) == 0) {
            for (unsigned i = 0; i < list.used; i++) {
                struct snd_ctl_elem_info info;
                memset(&info, 0, sizeof info);
                info.id = list.pids[i];
                if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info)) continue;
                struct snd_ctl_elem_value v;
                memset(&v, 0, sizeof v);
                v.id = info.id;
                int have = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) == 0;
                fprintf(out, "mixer: '%s' type %d count %u", (const char *)info.id.name, info.type, info.count);
                if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER)
                    fprintf(out, " range %ld..%ld", info.value.integer.min, info.value.integer.max);
                if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
                    fprintf(out, " items:");
                    for (unsigned k = 0; k < info.value.enumerated.items && k < 16; k++) {
                        struct snd_ctl_elem_info e = info;
                        e.value.enumerated.item = k;
                        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &e) == 0)
                            fprintf(out, " [%u]%s", k, e.value.enumerated.name);
                    }
                }
                if (have) {
                    fprintf(out, " =");
                    for (unsigned k = 0; k < info.count && k < 4; k++)
                        fprintf(out, " %ld", info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED
                                                 ? (long)v.value.enumerated.item[k] : v.value.integer.value[k]);
                }
                fprintf(out, "\n");
            }
        }
        free(list.pids);
    }
    close(fd);
}
