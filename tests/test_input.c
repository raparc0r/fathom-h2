/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * device/input.c on the host: its events come from a pipe instead of
 * /dev/input (tests/fake holds the Linux headers it needs). A headphone jack
 * change is never lost, however full the queue is. Power wakes the screen as
 * it goes down, and turns it off as it comes back up (unless the screen went
 * off by itself meanwhile).
 */
#include "../device/input.c"

#include <fcntl.h>

#include "test_util.h"

static int wfd;

static void send_ev(int type, int code, int value)
{
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = (uint16_t)type;
    e.code = (uint16_t)code;
    e.value = value;
    if (write(wfd, &e, sizeof e) != (ssize_t)sizeof e) t_fails++;
}

/* Everything input_next() gives now: the headphones' states it told, in order. */
static int drain(int *hp, int cap, int *wheel)
{
    input_event_t ev;
    int n = 0;
    *wheel = 0;
    while (input_next(&ev, 0)) {
        if (ev.kind == IN_JACKS && n < cap) hp[n++] = ev.hp;
        if (ev.kind == IN_UI) (*wheel)++;
    }
    return n;
}

/* What Power says at `now`: how many IN_POWER (switch the screen) and IN_POWER_OFF. */
static void power_at(uint32_t now, int *sw, int *off)
{
    input_event_t ev;
    *sw = *off = 0;
    while (input_next(&ev, now)) {
        if (ev.kind == IN_POWER) (*sw)++;
        if (ev.kind == IN_POWER_OFF) (*off)++;
    }
}

/* Power going down (1) or coming up (0) at `now`. */
static void power_key(int down, uint32_t now, int *sw, int *off)
{
    send_ev(EV_KEY, KEY_POWER, down);
    power_at(now, sw, off);
}

static void test_power(void)
{
    int sw, off;

    /* the screen on (as at the start): nothing on the press, off on the release */
    power_key(1, 1000, &sw, &off);
    CHECK(sw == 0 && off == 0);
    CHECK(input_timeout(1000) == POWER_OFF_MS);
    power_key(0, 1120, &sw, &off);
    CHECK(sw == 1 && off == 0);
    CHECK(input_timeout(1120) == -1);

    /* the screen off: on at once, on the press; its release says nothing more */
    input_set_screen(0);
    power_key(1, 5000, &sw, &off);
    CHECK(sw == 1 && off == 0);
    input_set_screen(1);                 /* (the caller woke the screen) */
    power_key(0, 5150, &sw, &off);
    CHECK(sw == 0 && off == 0);
    /* ...and the next press is an ordinary one: off on its release */
    power_key(1, 6000, &sw, &off);
    CHECK(sw == 0 && off == 0);
    power_key(0, 6100, &sw, &off);
    CHECK(sw == 1 && off == 0);

    /* the screen on, Power held 2 s: power-off, the screen never switched */
    power_key(1, 10000, &sw, &off);
    power_at(11999, &sw, &off);
    CHECK(sw == 0 && off == 0);
    power_at(12000, &sw, &off);
    CHECK(sw == 0 && off == 1);
    power_at(12500, &sw, &off);          /* (once) */
    CHECK(sw == 0 && off == 0);
    power_key(0, 13000, &sw, &off);
    CHECK(sw == 0 && off == 0);

    /* the screen off, Power held 2 s: on at the press, then power-off, nothing on the release */
    input_set_screen(0);
    power_key(1, 20000, &sw, &off);
    CHECK(sw == 1 && off == 0);
    input_set_screen(1);
    power_at(22000, &sw, &off);
    CHECK(sw == 0 && off == 1);
    power_key(0, 22300, &sw, &off);
    CHECK(sw == 0 && off == 0);

    /* the screen off, and the caller doesn't wake it (it had something else to
     * do with the key: charging mode): still only one event for the press */
    input_set_screen(0);
    power_key(1, 30000, &sw, &off);
    CHECK(sw == 1);
    power_key(0, 30100, &sw, &off);
    CHECK(sw == 0);
    power_key(1, 31000, &sw, &off);      /* still off: the next press says so again */
    CHECK(sw == 1);
    power_key(0, 31100, &sw, &off);
    CHECK(sw == 0);

    /* pressed with the screen on, which then goes off by itself (its timeout)
     * before the release: the release says nothing, the screen is off as the
     * press wanted it (it switched it, which turned the screen back on) */
    input_set_screen(1);
    power_key(1, 40000, &sw, &off);
    CHECK(sw == 0);
    input_set_screen(0);
    power_key(0, 40500, &sw, &off);
    CHECK(sw == 0 && off == 0);
    /* ...and the next press, the screen off, wakes it as ever */
    power_key(1, 41000, &sw, &off);
    CHECK(sw == 1 && off == 0);
    input_set_screen(1);
    power_key(0, 41100, &sw, &off);
    CHECK(sw == 0 && off == 0);
    /* the same, Power held on for its 2 s: power-off all the same */
    power_key(1, 45000, &sw, &off);
    CHECK(sw == 0);
    input_set_screen(0);
    power_at(47000, &sw, &off);
    CHECK(sw == 0 && off == 1);
    power_key(0, 47200, &sw, &off);
    CHECK(sw == 0 && off == 0);
    input_set_screen(1);

    /* a release with no press before it (the key was down as the player started): nothing */
    power_key(0, 50000, &sw, &off);
    CHECK(sw == 0 && off == 0);
}

int main(void)
{
    int p[2];
    if (pipe(p)) { perror("pipe"); return 1; }
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    fds[0] = p[0];
    nfds = 1;
    wfd = p[1];
    track_key = -1;
    jack_hp = 1;

    /* a fast turn of the wheel fills the queue; the headphones come out in it */
    int hp[8], wheel;
    send_ev(EV_REL, 0, 40);
    send_ev(EV_SW, SW_HEADPHONE_INSERT, 0);
    int n = drain(hp, 8, &wheel);
    CHECK(n == 1 && hp[0] == 0);
    CHECK(wheel >= QUEUE - 1);

    /* out and back in while it's full: the unplug is still told, then the plug */
    send_ev(EV_SW, SW_HEADPHONE_INSERT, 1);
    send_ev(EV_REL, 0, 40);
    send_ev(EV_SW, SW_HEADPHONE_INSERT, 0);
    send_ev(EV_SW, SW_HEADPHONE_INSERT, 1);
    n = drain(hp, 8, &wheel);
    CHECK(n == 3 && hp[0] == 1 && hp[1] == 0 && hp[2] == 1);

    /* with room, each change as it came */
    send_ev(EV_SW, SW_HEADPHONE_INSERT, 0);
    send_ev(EV_SW, SW_HEADPHONE_INSERT, 1);
    n = drain(hp, 8, &wheel);
    CHECK(n == 2 && hp[0] == 0 && hp[1] == 1);

    test_power();

    return t_result();
}
