/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * device/input.c on the host: its events come from a pipe instead of
 * /dev/input (tests/fake holds the Linux headers it needs). A headphone jack
 * change is never lost, however full the queue is.
 */
#include "../device/input.c"

#include <fcntl.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int wfd;

static void send_ev(int type, int code, int value)
{
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = (uint16_t)type;
    e.code = (uint16_t)code;
    e.value = value;
    if (write(wfd, &e, sizeof e) != (ssize_t)sizeof e) failures++;
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

    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("input: all checks passed\n");
    return failures != 0;
}
