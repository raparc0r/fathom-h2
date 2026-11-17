/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "input.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "ui/ui.h"

#define LONG_PRESS_MS 600
#define POWER_OFF_MS 2000         /* hold Power this long to power off */
#define REPEAT_DELAY_MS 400       /* volume keys: first repeat */
#define REPEAT_MS 120             /* then every 120 ms */
#define SEEK_HOLD_MS 500          /* Prev/Next held this long: seek instead of skip */
#define SEEK_MS 200               /* then a seek step every 200 ms */
#define QUEUE 16

static int fds[INPUT_MAX_FDS], nfds;
static char fd_path[INPUT_MAX_FDS][24];   /* /dev/input/eventN */
static int fd_remote[INPUT_MAX_FDS];      /* headphones' buttons (AVRCP), not the player's keys */
static int watch_fd = -1;                 /* inotify on /dev/input: devices that come later */
static input_event_t queue[QUEUE];
static int q_head, q_tail;
static int back_down;            /* Back held: long-press pending */
static uint32_t back_since;
static int back_long_fired;
static int power_down, power_off_fired;
static int menu_down, menu_long_fired, menu_chord;   /* Menu held; quick settings sent; wheel turned */
static uint32_t menu_since;
static int track_key;             /* Prev/Next held: UI_KEY_PREV or UI_KEY_NEXT, -1 if none */
static int track_seeking;         /* held past SEEK_HOLD_MS */
static uint32_t track_since, track_next;
static uint32_t power_since;
static int vol_held;              /* +1, -1 or 0 */
static uint32_t vol_next;         /* next repeat time */

static int jack_hp, jack_lo;


static void push(input_kind_t kind, int key)
{
    int next = (q_tail + 1) % QUEUE;
    if (next == q_head) return;   /* full: drop */
    queue[q_tail] = (input_event_t){kind, key, jack_hp, jack_lo};
    q_tail = next;
}

/* Read the switch state of every device (gpio-keys carries the jack detects). */
static void read_switches(void)
{
    for (int i = 0; i < nfds; i++) {
        unsigned long sw[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
        if (ioctl(fds[i], EVIOCGSW(sizeof sw), sw) < 0) continue;
        const int bpl = 8 * sizeof(long);
        jack_hp |= !!(sw[SW_HEADPHONE_INSERT / bpl] & (1ul << (SW_HEADPHONE_INSERT % bpl)));
        jack_lo |= !!(sw[SW_LINEOUT_INSERT / bpl] & (1ul << (SW_LINEOUT_INSERT % bpl)));
    }
}

void input_jacks(int *hp, int *lo)
{
    *hp = jack_hp;
    *lo = jack_lo;
}

/* Opens /dev/input/<node> unless it's open already. BlueZ makes one device per
 * pair of headphones, "<name> (AVRCP)", for the buttons on them. */
static void add_device(const char *node)
{
    if (strncmp(node, "event", 5) || nfds >= INPUT_MAX_FDS) return;
    char path[24], name[64] = "";
    snprintf(path, sizeof path, "/dev/input/%.10s", node);
    for (int i = 0; i < nfds; i++)
        if (!strcmp(fd_path[i], path)) return;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    size_t len = strlen(name);
    int remote = len > 8 && !strcmp(name + len - 8, " (AVRCP)");
    fprintf(stderr, "input: %s (%s)%s\n", path, name, remote ? ": headphone buttons" : "");
    snprintf(fd_path[nfds], sizeof fd_path[nfds], "%s", path);
    fd_remote[nfds] = remote;
    fds[nfds++] = fd;
}

static void drop_device(int i)
{
    fprintf(stderr, "input: %s gone\n", fd_path[i]);
    close(fds[i]);
    nfds--;
    fds[i] = fds[nfds];
    fd_remote[i] = fd_remote[nfds];
    memcpy(fd_path[i], fd_path[nfds], sizeof fd_path[i]);
}

static void watch_devices(void)
{
    char buf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n;
    while ((n = read(watch_fd, buf, sizeof buf)) > 0)
        for (char *p = buf; p < buf + n; p += sizeof(struct inotify_event) + ((struct inotify_event *)p)->len) {
            const struct inotify_event *e = (const struct inotify_event *)p;
            if (e->len && (e->mask & (IN_CREATE | IN_ATTRIB))) add_device(e->name);
        }
}

int input_open(void)
{
    track_key = -1;
    watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (watch_fd >= 0 && inotify_add_watch(watch_fd, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
        close(watch_fd);
        watch_fd = -1;
    }
    DIR *d = opendir("/dev/input");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) add_device(e->d_name);
    closedir(d);
    read_switches();
    return nfds;
}

int input_pollfds(struct pollfd *p, int max)
{
    int n = 0;
    for (int i = 0; i < nfds && n < max; i++) p[n++] = (struct pollfd){.fd = fds[i], .events = POLLIN};
    if (watch_fd >= 0 && n < max) p[n++] = (struct pollfd){.fd = watch_fd, .events = POLLIN};
    return n;
}

static void on_key(int code, int value, uint32_t now)
{
    if (value == 2) return;   /* autorepeat */
    int press = value == 1;
    switch (code) {
    case KEY_PLAYPAUSE: if (press) push(IN_UI, UI_KEY_OK); break;
    case KEY_MENU:      /* short: context menu; held: quick settings; held + wheel: brightness */
        if (press) { menu_down = 1; menu_since = now; menu_long_fired = 0; menu_chord = 0; }
        else if (menu_down) {
            menu_down = 0;
            if (!menu_long_fired && !menu_chord) push(IN_UI, UI_KEY_MENU);
        }
        break;
    case KEY_NEXTSONG:
    case KEY_PREVIOUSSONG: {   /* short: skip (on release); held: seek */
        int k = code == KEY_NEXTSONG ? UI_KEY_NEXT : UI_KEY_PREV;
        if (press) { track_key = k; track_since = now; track_seeking = 0; }
        else if (track_key == k) {
            if (!track_seeking) push(IN_UI, k);
            track_key = -1;
        }
        break;
    }
    case KEY_POWER:     /* short press: screen on/off; held 2 s: power off */
        if (press) { power_down = 1; power_since = now; power_off_fired = 0; }
        else if (power_down) {
            power_down = 0;
            if (!power_off_fired) push(IN_POWER, 0);
        }
        break;
    case KEY_BACK:
        if (press) { back_down = 1; back_since = now; back_long_fired = 0; }
        else if (back_down) {
            back_down = 0;
            if (!back_long_fired) push(IN_UI, UI_KEY_BACK);
        }
        break;
    case KEY_VOLUMEUP:
    case KEY_VOLUMEDOWN: {
        int d = code == KEY_VOLUMEUP ? 1 : -1;
        if (press) { push(IN_VOLUME, d); vol_held = d; vol_next = now + REPEAT_DELAY_MS; }
        else if (vol_held == d) vol_held = 0;
        break;
    }
    default: break;
    }
}

/* The headphones' buttons (AVRCP), on the press: from BlueZ's uinput device
 * (KEY_PLAYCD toggles: headphones that can't see the player's state send only
 * Play) or from fathom-avrcp, which tells them the state (KEY_PLAY only plays).
 * Pause and Stop only pause. */
static void on_remote_key(int code, int value)
{
    if (value != 1) return;
    switch (code) {
    case KEY_PLAYCD: case KEY_PLAYPAUSE: push(IN_MEDIA, IN_MEDIA_TOGGLE); break;
    case KEY_PLAY: push(IN_MEDIA, IN_MEDIA_PLAY); break;   /* fathom-avrcp: BlueZ's Play */
    case KEY_PAUSECD: case KEY_PAUSE: case KEY_STOPCD: push(IN_MEDIA, IN_MEDIA_PAUSE); break;
    case KEY_NEXTSONG: push(IN_MEDIA, IN_MEDIA_NEXT); break;
    case KEY_PREVIOUSSONG: push(IN_MEDIA, IN_MEDIA_PREV); break;
    case KEY_VOLUMEUP: push(IN_VOLUME, 1); break;
    case KEY_VOLUMEDOWN: push(IN_VOLUME, -1); break;
    default: break;
    }
}

int input_next(input_event_t *ev, uint32_t now)
{
    struct input_event ie;
    for (int i = 0; i < nfds; i++) {
        ssize_t r;
        while ((r = read(fds[i], &ie, sizeof ie)) == (ssize_t)sizeof ie) {
            if (fd_remote[i]) {
                if (ie.type == EV_KEY) on_remote_key(ie.code, ie.value);
                continue;
            }
            if (ie.type == EV_KEY) on_key(ie.code, ie.value, now);
            else if (ie.type == EV_SW && (ie.code == SW_HEADPHONE_INSERT || ie.code == SW_LINEOUT_INSERT)) {
                if (ie.code == SW_HEADPHONE_INSERT) jack_hp = !!ie.value;
                else jack_lo = !!ie.value;
                push(IN_JACKS, 0);
            }
            else if (ie.type == EV_REL && ie.value) {
                /* clockwise = down the list; with Menu held: brighter */
                if (menu_down) menu_chord = 1;
                int up = menu_down ? UI_KEY_DIMMER : UI_KEY_UP, down = menu_down ? UI_KEY_BRIGHTER : UI_KEY_DOWN;
                for (int n = ie.value > 0 ? ie.value : -ie.value; n > 0; n--)
                    push(IN_UI, ie.value > 0 ? down : up);
            }
        }
        if (r < 0 && errno == ENODEV) drop_device(i--);   /* headphones gone */
    }
    if (watch_fd >= 0) watch_devices();   /* after: a node may come back under the same name */
    if (vol_held && (int32_t)(now - vol_next) >= 0) {
        push(IN_VOLUME, vol_held);
        vol_next = now + REPEAT_MS;
    }
    if (power_down && !power_off_fired && now - power_since >= POWER_OFF_MS) {
        power_off_fired = 1;
        push(IN_POWER_OFF, 0);
    }
    if (track_key >= 0) {
        if (!track_seeking && now - track_since >= SEEK_HOLD_MS) { track_seeking = 1; track_next = now; }
        if (track_seeking && (int32_t)(now - track_next) >= 0) {
            push(IN_UI, track_key == UI_KEY_NEXT ? UI_KEY_SEEK_FWD : UI_KEY_SEEK_BACK);
            track_next = now + SEEK_MS;
        }
    }
    if (menu_down && !menu_long_fired && !menu_chord && now - menu_since >= LONG_PRESS_MS) {
        menu_long_fired = 1;
        push(IN_UI, UI_KEY_MENU_LONG);
    }
    if (back_down && !back_long_fired && now - back_since >= LONG_PRESS_MS) {
        back_long_fired = 1;
        push(IN_UI, UI_KEY_BACK_LONG);
    }
    if (q_head == q_tail) return 0;
    *ev = queue[q_head];
    q_head = (q_head + 1) % QUEUE;
    return 1;
}

int input_timeout(uint32_t now)
{
    int t = -1;
    if (back_down && !back_long_fired) {
        uint32_t el = now - back_since;
        t = el >= LONG_PRESS_MS ? 0 : (int)(LONG_PRESS_MS - el);
    }
    if (track_key >= 0) {
        int v = !track_seeking ? (now - track_since >= SEEK_HOLD_MS ? 0 : (int)(SEEK_HOLD_MS - (now - track_since)))
                               : ((int32_t)(track_next - now) <= 0 ? 0 : (int)(track_next - now));
        if (t < 0 || v < t) t = v;
    }
    if (menu_down && !menu_long_fired && !menu_chord) {
        uint32_t el = now - menu_since;
        int v = el >= LONG_PRESS_MS ? 0 : (int)(LONG_PRESS_MS - el);
        if (t < 0 || v < t) t = v;
    }
    if (vol_held) {
        int v = (int32_t)(vol_next - now) <= 0 ? 0 : (int)(vol_next - now);
        if (t < 0 || v < t) t = v;
    }
    if (power_down && !power_off_fired) {
        uint32_t el = now - power_since;
        int v = el >= POWER_OFF_MS ? 0 : (int)(POWER_OFF_MS - el);
        if (t < 0 || v < t) t = v;
    }
    return t;
}
