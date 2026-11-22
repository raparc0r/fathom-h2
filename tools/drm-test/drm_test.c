/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * drm-test: show a test pattern through DRM/KMS (dumb buffer + SETCRTC),
 * the same path the player's display backend uses. Bring-up tool only.
 *
 *   drm-test [seconds]   colour bars, a white corner square, and a moving
 *                        stripe for the given time (default 5); prints fps
 */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <errno.h>

static int io(int fd, unsigned long req, void *arg)
{
    int r;
    do r = ioctl(fd, req, arg); while (r == -1 && errno == EINTR);
    if (r) perror("ioctl");
    return r;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static uint16_t rgb(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x1f));
}

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 5;
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("/dev/dri/card0"); return 1; }

    uint32_t conn_ids[4], crtc_ids[4], enc_ids[4], fb_ids[4];
    struct drm_mode_card_res res = {0};
    if (io(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) return 1;
    res.connector_id_ptr = (uintptr_t)conn_ids; res.count_connectors = res.count_connectors > 4 ? 4 : res.count_connectors;
    res.crtc_id_ptr = (uintptr_t)crtc_ids;      res.count_crtcs = res.count_crtcs > 4 ? 4 : res.count_crtcs;
    res.encoder_id_ptr = (uintptr_t)enc_ids;    res.count_encoders = res.count_encoders > 4 ? 4 : res.count_encoders;
    res.fb_id_ptr = (uintptr_t)fb_ids;          res.count_fbs = res.count_fbs > 4 ? 4 : res.count_fbs;
    if (io(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) return 1;

    struct drm_mode_modeinfo modes[4];
    struct drm_mode_get_connector conn = {.connector_id = conn_ids[0]};
    if (io(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn)) return 1;
    conn.modes_ptr = (uintptr_t)modes; conn.count_modes = conn.count_modes > 4 ? 4 : conn.count_modes;
    conn.count_props = 0; conn.count_encoders = 0;
    if (io(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) || !conn.count_modes) return 1;
    int w = modes[0].hdisplay, h = modes[0].vdisplay;
    printf("connector %u, crtc %u, mode %dx%d\n", conn_ids[0], crtc_ids[0], w, h);

    struct drm_mode_create_dumb cd = {.width = w, .height = h, .bpp = 16};
    if (io(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) return 1;
    struct drm_mode_fb_cmd fbc = {.width = w, .height = h, .pitch = cd.pitch, .bpp = 16, .depth = 16, .handle = cd.handle};
    if (io(fd, DRM_IOCTL_MODE_ADDFB, &fbc)) return 1;
    struct drm_mode_map_dumb md = {.handle = cd.handle};
    if (io(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) return 1;
    uint16_t *px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)md.offset);
    if (px == MAP_FAILED) { perror("mmap"); return 1; }
    int stride = cd.pitch / 2;

    static const uint32_t bars[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0x7FDFC4, 0xF2A65A, 0x16203A, 0xC3D3E4};
    struct drm_mode_crtc crtc = {.crtc_id = crtc_ids[0], .fb_id = fbc.fb_id, .set_connectors_ptr = (uintptr_t)&conn_ids[0],
                                 .count_connectors = 1, .mode = modes[0], .mode_valid = 1};
    struct drm_mode_fb_dirty_cmd dirty = {.fb_id = fbc.fb_id};
    int frames = 0;
    double t0 = now();
    for (;;) {
        double t = now() - t0;
        if (t >= secs && frames) break;
        int stripe = (int)(t * 80) % w;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint16_t c = rgb(bars[x * 8 / w]);
                if (x < 40 && y < 40) c = 0xffff;                    /* origin marker */
                if (x >= stripe && x < stripe + 12 && y > h / 2) c = 0;
                px[y * stride + x] = c;
            }
        if (frames == 0) { if (io(fd, DRM_IOCTL_MODE_SETCRTC, &crtc)) return 1; }
        else if (io(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty)) return 1;
        frames++;
    }
    double el = now() - t0;
    printf("%d frames in %.2f s: %.1f fps (full-screen updates)\n", frames, el, frames / el);
    return 0;
}
