/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * fmprobe: does the BCM43438's FM receiver answer on this board, and does it
 * hear anything? Talks HCI straight to hci0 through the kernel's user channel
 * (no BlueZ), as tools/bluetooth/hcitest.c does. hci0 must exist and be down
 * (fathom-bluetooth on). It only listens.
 *
 *   fmprobe regs                    FM on, registers 0x00..0x1f read and printed, FM off
 *   fmprobe scan [FROM TO STEP]     FM on, each frequency (kHz) tuned and its signal read, FM off
 *   fmprobe raw ADDR MODE BYTES...  one FC15 command, its reply printed (hex arguments)
 *
 *   fmprobe seq STEP...             steps in one go: wREG,BYTE.. write, rREG,N read, sMS wait
 *
 * Broadcom's FM registers are reached by the vendor command 0xFC15:
 * [register][0 write, 1 read][the bytes to write, or how many to read]; the
 * registers and the order of a tune are as Broadcom's own V4L2 driver has them
 * (fmdrv_main.h, fmdrv_rx.c in vendors' kernels).
 *
 * What it found on the H2 (2026-11-26): the receiver answers, tunes and
 * searches, and hears nothing: -128 to -112 dBm over the whole band with
 * headphones plugged in, no station found by its own search. Its antenna
 * input isn't connected to anything that receives.
 *
 * Built by hand with the Buildroot toolchain (it is in no build):
 *   mipsel-buildroot-linux-musl-gcc -Os -static -o fmprobe tools/bluetooth/fmprobe.c
 */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define AF_BLUETOOTH_ 31
#define BTPROTO_HCI_ 1
#define HCI_CHANNEL_USER 1
struct sockaddr_hci_ { sa_family_t hci_family; unsigned short hci_dev; unsigned short hci_channel; };

static int g_fd, g_verbose;

static uint32_t ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static int send_cmd(uint16_t opcode, const void *p, uint8_t n)
{
    uint8_t b[260] = {0x01, (uint8_t)opcode, (uint8_t)(opcode >> 8), n};
    if (n) memcpy(b + 4, p, n);
    return write(g_fd, b, 4u + n) == 4 + n ? 0 : -1;
}

static int read_event(uint8_t *b, int cap, int ms)
{
    struct pollfd pf = {.fd = g_fd, .events = POLLIN};
    if (poll(&pf, 1, ms) <= 0) return 0;
    int n = (int)read(g_fd, b, (size_t)cap);
    return n > 0 && b[0] == 0x04 ? n : 0;
}

/* the Command Complete's return parameters (status first): their length, -1 none, -2 refused (status in r[0]) */
static int command(uint16_t opcode, const void *p, uint8_t n, uint8_t *r, int cap)
{
    if (send_cmd(opcode, p, n)) return -1;
    uint8_t b[260];
    uint32_t until = ms_now() + 2000;
    while ((int32_t)(until - ms_now()) > 0) {
        int len = read_event(b, sizeof b, 200);
        if (g_verbose && len > 0) {
            printf("    event:");
            for (int i = 0; i < len; i++) printf(" %02x", b[i]);
            printf("\n");
        }
        if (len >= 6 && b[1] == 0x0e && (b[4] | b[5] << 8) == opcode) {
            int m = len - 6 < cap ? len - 6 : cap;
            memcpy(r, b + 6, (size_t)m);
            return m;
        }
        if (len >= 7 && b[1] == 0x0f && (b[5] | b[6] << 8) == opcode && b[3] != 0) { r[0] = b[3]; return -2; }
    }
    return -1;
}

static int fm_write(uint8_t reg, const uint8_t *v, int n)
{
    uint8_t p[8] = {reg, 0x00}, r[32];
    memcpy(p + 2, v, (size_t)n);
    int m = command(0xfc15, p, (uint8_t)(2 + n), r, sizeof r);
    return m >= 1 && r[0] == 0 ? 0 : m == -2 ? -(int)r[0] - 100 : -1;
}

/* n bytes of a register into v; the reply's whole return parameters into raw (rn bytes) */
static int fm_read(uint8_t reg, uint8_t *v, int n, uint8_t *raw, int *rn)
{
    uint8_t p[3] = {reg, 0x01, (uint8_t)n}, r[32];
    int m = command(0xfc15, p, 3, r, sizeof r);
    if (raw && rn) { *rn = m > 0 ? m : 0; if (m > 0) memcpy(raw, r, (size_t)m); }
    if (m < 1 || r[0]) return -1;
    /* the value: the reply's last n bytes (after the status, and whatever it echoes) */
    if (m < 1 + n) return -1;
    memcpy(v, r + m - n, (size_t)n);
    return 0;
}

static void hex(const char *what, const uint8_t *b, int n)
{
    printf("%s", what);
    for (int i = 0; i < n; i++) printf(" %02x", b[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "regs";
    g_fd = socket(AF_BLUETOOTH_, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI_);
    if (g_fd < 0) { perror("socket"); return 1; }
    struct sockaddr_hci_ a = {AF_BLUETOOTH_, 0, HCI_CHANNEL_USER};
    int tries = 0;
    while (bind(g_fd, (struct sockaddr *)&a, sizeof a)) {
        if (errno != EBUSY || ++tries > 20) { perror("bind hci0"); return 1; }
        usleep(500000);
    }
    uint8_t r[260], v[4], raw[32];
    int n = command(0x1001, NULL, 0, r, sizeof r), rn = 0;
    if (n < 9 || r[0]) { fprintf(stderr, "the controller doesn't answer\n"); return 1; }
    printf("controller: HCI %u rev 0x%04x, LMP %u subversion 0x%04x, manufacturer %u\n",
           r[1], r[2] | r[3] << 8, r[4], r[7] | r[8] << 8, r[5] | r[6] << 8);
    n = command(0x0c14, NULL, 0, r, sizeof r);
    if (n > 1 && !r[0]) printf("name: %.248s\n", (const char *)r + 1);

    if (!strcmp(what, "raw") && argc >= 4) {
        uint8_t p[16];
        int k = 0;
        for (int i = 2; i < argc && k < 16; i++) p[k++] = (uint8_t)strtoul(argv[i], NULL, 16);
        g_verbose = 1;
        hex("  sent fc15:", p, k);
        n = command(0xfc15, p, (uint8_t)k, r, sizeof r);
        if (n == -2) printf("  refused: status 0x%02x\n", r[0]);
        else if (n < 0) printf("  no answer\n");
        else hex("  answer:", r, n);
        return 0;
    }

    if (!strcmp(what, "seq")) {
        /* a run of steps in one go (the FM core forgets all when hci0 is closed):
         * wREG,BYTE,BYTE.. write; rREG,N read N bytes; sMS wait; all numbers hex but MS */
        for (int i = 2; i < argc; i++) {
            const char *t = argv[i];
            if (t[0] == 's') { usleep((useconds_t)atoi(t + 1) * 1000); printf("  (waited %s ms)\n", t + 1); continue; }
            uint8_t p[16];
            int k = 0;
            char tmp[128];
            snprintf(tmp, sizeof tmp, "%s", t + 1);
            for (char *tok = strtok(tmp, ","); tok && k < 14; tok = strtok(NULL, ",")) {
                p[k == 0 ? 0 : k + 1] = (uint8_t)strtoul(tok, NULL, 16);
                k++;
            }
            if (k < 1) continue;
            p[1] = t[0] == 'r' ? 0x01 : 0x00;
            int np = k + 1;
            n = command(0xfc15, p, (uint8_t)np, r, sizeof r);
            printf("  %c reg %02x:", t[0], p[0]);
            if (t[0] == 'w') for (int j = 2; j < np; j++) printf(" %02x", p[j]);
            if (n == -2) printf("  -> refused, status 0x%02x\n", r[0]);
            else if (n < 0) printf("  -> no answer\n");
            else { printf("  ->"); for (int j = 0; j < n; j++) printf(" %02x", r[j]); printf("\n"); }
        }
        return 0;
    }

    /* FM on: the first FC15 says whether this firmware has the FM commands at all */
    g_verbose = 1;
    uint8_t on = 0x01, off = 0x00;
    printf("FM on (register 0x00 <- 01):\n");
    int w = fm_write(0x00, &on, 1);
    if (w) { printf("  the chip refused or didn't answer (%d): no FM in this firmware?\n", w); return 2; }
    usleep(100000);
    g_verbose = 0;
    if (fm_read(0x00, v, 1, raw, &rn) == 0) { printf("register 0x00 reads %02x;", v[0]); hex(" whole answer:", raw, rn); }
    else { hex("register 0x00 can't be read; answer:", raw, rn); }

    if (!strcmp(what, "regs")) {
        for (int reg = 0; reg < 0x20; reg++) {
            if (fm_read((uint8_t)reg, v, 1, raw, &rn) == 0) printf("  reg 0x%02x = %02x\n", reg, v[0]);
            else { printf("  reg 0x%02x:", reg); hex(" (no value)", raw, rn); }
        }
    } else {
        int from = argc > 2 ? atoi(argv[2]) : 87500, to = argc > 3 ? atoi(argv[3]) : 108000, step = argc > 4 ? atoi(argv[4]) : 100;
        /* As Broadcom's own V4L2 driver does it (fmdrv_main.c, fmdrv_rx.c, fm_public.h):
         * after FM on, 300 ms; FM_CTRL (0x01) = stereo auto, the west band (87.5-108);
         * a frequency is its kHz above 64 MHz, two bytes; the flags' mask (0x10) = 0003;
         * SCH_TUNE (0x09) = 1 (a preset tune); flags (0x12): bit 0 done, bit 1 failed.
         * RSSI (0x0f) is dBm as a signed byte; SNR at 0xdf. */
        usleep(300000);
        uint8_t ctrl = 0x02;
        fm_write(0x01, &ctrl, 1);
        printf("scan %d to %d kHz, every %d: frequency, RSSI dBm, SNR, carrier error, flags, ms to tune, frequency read back\n", from, to, step);
        for (int f = from; f <= to; f += step) {
            int val = f - 64000;
            uint8_t fr[2] = {(uint8_t)val, (uint8_t)(val >> 8)}, mask[2] = {0x03, 0x00}, mode = 0x01;
            uint8_t rssi = 0, car = 0, snr = 0, flg[2] = {0, 0}, back[2] = {0, 0};
            if (fm_write(0x0a, fr, 2)) { printf("%d: frequency refused\n", f); break; }
            fm_write(0x10, mask, 2);
            if (fm_write(0x09, &mode, 1)) { printf("%d: tune refused\n", f); break; }
            uint32_t t0 = ms_now();
            int flags = 0;
            while (ms_now() - t0 < 1500) {
                usleep(15000);
                if (fm_read(0x12, flg, 2, NULL, NULL) == 0) flags |= flg[0] | flg[1] << 8;
                if (flags & 0x03) break;
            }
            int took = (int)(ms_now() - t0);
            usleep(60000);                       /* (the signal's figures settle) */
            fm_read(0x0f, &rssi, 1, NULL, NULL);
            fm_read(0xdf, &snr, 1, NULL, NULL);
            fm_read(0x0e, &car, 1, NULL, NULL);
            fm_read(0x0a, back, 2, NULL, NULL);
            printf("%d %d %u %d %04x %d %d\n", f, (int8_t)rssi, snr, (int8_t)car, flags, took, (back[0] | back[1] << 8) + 64000);
        }
    }
    g_verbose = 0;
    fm_write(0x00, &off, 1);
    printf("FM off\n");
    return 0;
}
