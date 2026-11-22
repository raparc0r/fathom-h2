/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* What an output last wrote, and a fade-out over it (pcmring.h). */
#include "audio/pcmring.h"

#include <stdlib.h>
#include <string.h>

int pcmring_init(pcmring_t *r, unsigned cap, unsigned frame_bytes)
{
    memset(r, 0, sizeof *r);
    if (!cap || !frame_bytes) return -1;
    r->buf = malloc((size_t)cap * frame_bytes);
    if (!r->buf) return -1;
    r->cap = cap;
    r->frame_bytes = frame_bytes;
    return 0;
}

void pcmring_free(pcmring_t *r)
{
    free(r->buf);
    memset(r, 0, sizeof *r);
}

void pcmring_reset(pcmring_t *r) { r->written = 0; }

void pcmring_put(pcmring_t *r, const void *frames, unsigned n)
{
    if (!r->buf || !n) return;
    const uint8_t *src = frames;
    if (n > r->cap) {                                /* (only the last cap frames matter) */
        src += (size_t)(n - r->cap) * r->frame_bytes;
        r->written += n - r->cap;
        n = r->cap;
    }
    unsigned at = (unsigned)(r->written % r->cap);
    unsigned first = r->cap - at < n ? r->cap - at : n;
    memcpy(r->buf + (size_t)at * r->frame_bytes, src, (size_t)first * r->frame_bytes);
    if (n > first) memcpy(r->buf, src + (size_t)first * r->frame_bytes, (size_t)(n - first) * r->frame_bytes);
    r->written += n;
}

int pcmring_get(const pcmring_t *r, unsigned back, unsigned n, void *out)
{
    if (!r->buf || back > r->cap || back > r->written || n > back) return -1;
    uint64_t from = r->written - back;
    uint8_t *dst = out;
    for (unsigned k = 0; k < n; ) {
        unsigned at = (unsigned)((from + k) % r->cap);
        unsigned run = r->cap - at < n - k ? r->cap - at : n - k;
        memcpy(dst + (size_t)k * r->frame_bytes, r->buf + (size_t)at * r->frame_bytes, (size_t)run * r->frame_bytes);
        k += run;
    }
    return 0;
}

void pcmring_unput(pcmring_t *r, unsigned n)
{
    r->written = n < r->written ? r->written - n : 0;
}

static int32_t s24_3(const uint8_t *p)
{
    int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
    return v & 0x800000 ? v - 0x1000000 : v;
}

void pcm_fade_out(void *frames, unsigned n, unsigned ch, int format)
{
    if (!n) return;
    for (unsigned i = 0; i < n; i++) {
        int64_t g = n > 1 ? (int64_t)(n - 1 - i) * 65536 / (n - 1) : 0;   /* 1 -> 0, in 1/65536 */
        for (unsigned c = 0; c < ch; c++) {
            size_t k = (size_t)i * ch + c;
            switch (format) {
            case PCMRING_S16: {
                int16_t *s = (int16_t *)frames + k;
                *s = (int16_t)(*s * g / 65536);
                break;
            }
            case PCMRING_S24_4:
            case PCMRING_S32: {
                int32_t *s = (int32_t *)frames + k;
                *s = (int32_t)((int64_t)*s * g / 65536);
                break;
            }
            default: {
                uint8_t *p = (uint8_t *)frames + 3 * k;
                int32_t v = (int32_t)((int64_t)s24_3(p) * g / 65536);
                p[0] = (uint8_t)v;
                p[1] = (uint8_t)(v >> 8);
                p[2] = (uint8_t)(v >> 16);
                break;
            }
            }
        }
    }
}
