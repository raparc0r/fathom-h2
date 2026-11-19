/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "disp_drm.h"

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define BL_PATH "/sys/class/backlight/backlight/brightness"

static struct {
    int fd;
    uint32_t fb_id, pitch;
    uint8_t *map;
    size_t size;
    int w, h;
} D = {.fd = -1};

static const uint16_t *g_last;

static int io(unsigned long req, void *arg)
{
    int r;
    do r = ioctl(D.fd, req, arg); while (r == -1 && errno == EINTR);
    return r;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    (void)px;   /* direct mode: LVGL already drew into the dumb buffer */
    struct drm_clip_rect clip = {(uint16_t)a->x1, (uint16_t)a->y1, (uint16_t)(a->x2 + 1), (uint16_t)(a->y2 + 1)};
    struct drm_mode_fb_dirty_cmd dirty = {.fb_id = D.fb_id, .num_clips = 1, .clips_ptr = (uintptr_t)&clip};
    io(DRM_IOCTL_MODE_DIRTYFB, &dirty);   /* synchronous: the panel has the pixels when it returns */
    lv_display_flush_ready(disp);
}

void disp_drm_set_last_frame(const uint16_t *rgb565) { g_last = rgb565; }

void disp_drm_show_last_frame(void)
{
    if (D.fd < 0 || !D.map || !g_last) return;
    for (int y = 0; y < D.h; y++)
        memcpy(D.map + (size_t)y * D.pitch, g_last + (size_t)y * D.w, (size_t)D.w * 2);
    struct drm_clip_rect clip = {0, 0, (uint16_t)D.w, (uint16_t)D.h};
    struct drm_mode_fb_dirty_cmd dirty = {.fb_id = D.fb_id, .num_clips = 1, .clips_ptr = (uintptr_t)&clip};
    ioctl(D.fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
}

lv_display_t *disp_drm_create(const char *card)
{
    D.fd = open(card, O_RDWR | O_CLOEXEC);
    if (D.fd < 0) { perror(card); return NULL; }

    uint32_t conns[4], crtcs[4], encs[4], fbs[4];
    struct drm_mode_card_res res = {0};
    if (io(DRM_IOCTL_MODE_GETRESOURCES, &res)) goto fail;
    res.count_connectors = res.count_connectors > 4 ? 4 : res.count_connectors;
    res.count_crtcs = res.count_crtcs > 4 ? 4 : res.count_crtcs;
    res.count_encoders = res.count_encoders > 4 ? 4 : res.count_encoders;
    res.count_fbs = res.count_fbs > 4 ? 4 : res.count_fbs;
    res.connector_id_ptr = (uintptr_t)conns; res.crtc_id_ptr = (uintptr_t)crtcs;
    res.encoder_id_ptr = (uintptr_t)encs;    res.fb_id_ptr = (uintptr_t)fbs;
    if (io(DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_connectors || !res.count_crtcs) goto fail;

    struct drm_mode_modeinfo modes[4];
    struct drm_mode_get_connector c = {.connector_id = conns[0]};
    if (io(DRM_IOCTL_MODE_GETCONNECTOR, &c)) goto fail;
    c.count_modes = c.count_modes > 4 ? 4 : c.count_modes;
    c.modes_ptr = (uintptr_t)modes; c.count_props = 0; c.count_encoders = 0;
    if (io(DRM_IOCTL_MODE_GETCONNECTOR, &c) || !c.count_modes) goto fail;
    D.w = modes[0].hdisplay;
    D.h = modes[0].vdisplay;

    struct drm_mode_create_dumb cd = {.width = (uint32_t)D.w, .height = (uint32_t)D.h, .bpp = 16};
    if (io(DRM_IOCTL_MODE_CREATE_DUMB, &cd)) goto fail;
    D.pitch = cd.pitch;
    D.size = cd.size;
    struct drm_mode_fb_cmd fb = {.width = (uint32_t)D.w, .height = (uint32_t)D.h, .pitch = cd.pitch,
                                 .bpp = 16, .depth = 16, .handle = cd.handle};
    if (io(DRM_IOCTL_MODE_ADDFB, &fb)) goto fail;
    D.fb_id = fb.fb_id;
    struct drm_mode_map_dumb md = {.handle = cd.handle};
    if (io(DRM_IOCTL_MODE_MAP_DUMB, &md)) goto fail;
    D.map = mmap(NULL, D.size, PROT_READ | PROT_WRITE, MAP_SHARED, D.fd, (off_t)md.offset);
    if (D.map == MAP_FAILED) goto fail;
    memset(D.map, 0, D.size);

    struct drm_mode_crtc crtc = {.crtc_id = crtcs[0], .fb_id = D.fb_id, .set_connectors_ptr = (uintptr_t)&conns[0],
                                 .count_connectors = 1, .mode = modes[0], .mode_valid = 1};
    if (io(DRM_IOCTL_MODE_SETCRTC, &crtc)) goto fail;

    lv_display_t *disp = lv_display_create(D.w, D.h);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    /* Direct mode needs LVGL's stride to match the dumb buffer's pitch. */
    if (D.pitch != (uint32_t)lv_draw_buf_width_to_stride((uint32_t)D.w, LV_COLOR_FORMAT_RGB565)) {
        fprintf(stderr, "drm: pitch %u does not match LVGL stride\n", D.pitch);
        goto fail;
    }
    lv_display_set_buffers(disp, D.map, NULL, (uint32_t)D.size, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);
    fprintf(stderr, "drm: %dx%d, pitch %u\n", D.w, D.h, D.pitch);
    return disp;

fail:
    perror("drm");
    close(D.fd);
    D.fd = -1;
    return NULL;
}

void disp_drm_set_backlight_raw(int value)
{
    FILE *f = fopen(BL_PATH, "we");
    if (!f) return;
    fprintf(f, "%d\n", value < 0 ? 0 : value);
    fclose(f);
}

int disp_drm_backlight_max(void)
{
    static int max;
    if (!max) {
        FILE *f = fopen("/sys/class/backlight/backlight/max_brightness", "re");
        if (!f || fscanf(f, "%d", &max) != 1 || max <= 0) max = 255;
        if (f) fclose(f);
    }
    return max;
}