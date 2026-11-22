/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * fathom-hcitest: is the Bluetooth chip alive? Talks HCI straight to hci0
 * through the kernel's user channel (no BlueZ, no D-Bus), so it runs on the
 * bare system:
 *
 *   fathom-hcitest [SECONDS]
 *
 * Prints the controller's version and address, then looks for devices nearby
 * for SECONDS (default 8): classic inquiry, then a Bluetooth LE scan. It only
 * listens: it never pairs or connects. hci0 must be down (it is, unless
 * something else uses it); the kernel brings it up for us and down after.
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

struct sockaddr_hci_ {
    sa_family_t hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

static int g_fd;

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

/* Reads one event within ms; returns its length (b[0] = 0x04, b[1] = code), 0 on timeout. */
static int read_event(uint8_t *b, int cap, int ms)
{
    struct pollfd pf = {.fd = g_fd, .events = POLLIN};
    if (poll(&pf, 1, ms) <= 0) return 0;
    int n = (int)read(g_fd, b, (size_t)cap);
    return n > 0 && b[0] == 0x04 ? n : 0;
}

/* Sends a command and waits for its Command Complete; returns the return
 * parameters' length (status at r[0]) or -1. */
static int command(uint16_t opcode, const void *p, uint8_t n, uint8_t *r, int cap)
{
    if (send_cmd(opcode, p, n)) return -1;
    uint8_t b[260];
    uint32_t until = ms_now() + 2000;
    while ((int32_t)(until - ms_now()) > 0) {
        int len = read_event(b, sizeof b, 200);
        if (len >= 6 && b[1] == 0x0e && (b[4] | b[5] << 8) == opcode) {
            int m = len - 6 < cap ? len - 6 : cap;
            memcpy(r, b + 6, (size_t)m);
            return m;
        }
        if (len >= 7 && b[1] == 0x0f && (b[5] | b[6] << 8) == opcode && b[3] != 0) return -1;
    }
    return -1;
}

#define MAX_SEEN 64
static uint8_t g_seen[MAX_SEEN][6];
static int g_nseen;

static void seen(const uint8_t *a, const char *how, int rssi)
{
    for (int i = 0; i < g_nseen; i++)
        if (!memcmp(g_seen[i], a, 6)) return;
    if (g_nseen < MAX_SEEN) memcpy(g_seen[g_nseen++], a, 6);
    printf("  %s %02X:%02X:%02X:%02X:%02X:%02X rssi %d\n", how, a[5], a[4], a[3], a[2], a[1], a[0], rssi);
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 8;
    if (secs < 2) secs = 2;
    g_fd = socket(AF_BLUETOOTH_, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI_);
    if (g_fd < 0) { perror("socket (no Bluetooth in the kernel?)"); return 1; }
    struct sockaddr_hci_ a = {AF_BLUETOOTH_, 0, HCI_CHANNEL_USER};
    int tries = 0;      /* busy while the kernel loads the firmware patch, or for 2 s after */
    while (bind(g_fd, (struct sockaddr *)&a, sizeof a)) {
        if (errno != EBUSY || ++tries > 10) { perror("bind hci0 (is it up, or missing?)"); return 1; }
        usleep(500000);
    }

    uint8_t r[260];
    int n = command(0x1001, NULL, 0, r, sizeof r);           /* Read Local Version */
    if (n < 9 || r[0]) { fprintf(stderr, "the controller doesn't answer\n"); return 1; }
    printf("controller: HCI %u rev 0x%04x, LMP %u subversion 0x%04x, manufacturer %u\n",
           r[1], r[2] | r[3] << 8, r[4], r[7] | r[8] << 8, r[5] | r[6] << 8);
    n = command(0x1009, NULL, 0, r, sizeof r);               /* Read BD_ADDR */
    if (n >= 7 && !r[0])
        printf("address: %02X:%02X:%02X:%02X:%02X:%02X\n", r[6], r[5], r[4], r[3], r[2], r[1]);
    n = command(0x0c14, NULL, 0, r, sizeof r);               /* Read Local Name */
    if (n > 1 && !r[0]) printf("name: %.248s\n", (const char *)r + 1);

    /* classic inquiry, with RSSI */
    uint8_t mode = 1;
    command(0x0c45, &mode, 1, r, sizeof r);
    int len = secs / 2 * 100 / 128;                          /* units of 1.28 s */
    if (len < 1) len = 1;
    uint8_t inq[5] = {0x33, 0x8b, 0x9e, (uint8_t)len, 0};    /* GIAC */
    printf("inquiry (%.1f s):\n", len * 1.28);
    if (send_cmd(0x0401, inq, 5) == 0) {
        uint32_t until = ms_now() + (uint32_t)(len * 1280 + 1500);
        uint8_t b[260];
        while ((int32_t)(until - ms_now()) > 0) {
            int m = read_event(b, sizeof b, 200);
            if (m < 3) continue;
            if (b[1] == 0x01) break;                             /* Inquiry Complete */
            if (b[1] == 0x22 || b[1] == 0x02) {                  /* Inquiry Result (with RSSI) */
                int k = b[3], sz = 14;                           /* both forms: 14 bytes each */
                for (int i = 0; i < k && 4 + (i + 1) * sz <= m; i++) {
                    const uint8_t *e = b + 4 + i * sz;
                    seen(e, "classic", b[1] == 0x22 ? (int8_t)e[13] : 0);
                }
            }
        }
    }
    int classic = g_nseen;

    /* LE scan: passive, 60 ms window every 100 ms */
    uint8_t sp[7] = {0x00, 0xa0, 0x00, 0x60, 0x00, 0x00, 0x00};
    uint8_t on[2] = {0x01, 0x00}, off[2] = {0x00, 0x00};
    printf("LE scan (%d s):\n", secs - secs / 2);
    if (command(0x200b, sp, 7, r, sizeof r) >= 1 && command(0x200c, on, 2, r, sizeof r) >= 1 && !r[0]) {
        uint32_t until = ms_now() + (uint32_t)(secs - secs / 2) * 1000;
        uint8_t b[260];
        while ((int32_t)(until - ms_now()) > 0) {
            int m = read_event(b, sizeof b, 200);
            if (m < 15 || b[1] != 0x3e || b[3] != 0x02) continue;  /* LE Advertising Report */
            int k = b[4];
            const uint8_t *e = b + 5;
            for (int i = 0; i < k && e + 9 <= b + m; i++) {
                int dl = e[8];
                if (e + 10 + dl > b + m) break;
                seen(e + 2, "LE     ", (int8_t)e[9 + dl]);
                e += 10 + dl;
            }
        }
        command(0x200c, off, 2, r, sizeof r);
    } else {
        printf("  (LE scan refused)\n");
    }
    printf("found %d classic and %d LE devices\n", classic, g_nseen - classic);
    close(g_fd);
    return 0;
}
