/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The player on the H2: its clock. The H2 has none that runs while it is
 * off: the last time known is kept and put back at a start, and the network
 * sets it (NTP, or one HTTPS answer where NTP gets none); About > Clock sets
 * it by hand (main_int.h). Out of main.c, where it stood. */
#include "main_int.h"

/* ---- a clock that never goes back ----
 * The H2 has no running real-time clock: every boot starts in 1970. History
 * ("recently played") and the shuffle's freshness only need time to move
 * forward, so the clock is saved now and then and, at start-up, set to the
 * saved value if it is behind. Time spent switched off isn't counted. */

void clock_restore(void)
{
    long saved = settings_get_int("clock.last", 0);
    if (saved > 0 && time(NULL) < saved) {
        struct timeval tv = {saved + 1, 0};
        if (settimeofday(&tv, NULL)) perror("settimeofday");
    }
}

void clock_save(void) { settings_set_int("clock.last", (int)time(NULL)); }

/* ---- the clock from HTTPS, where NTP gets no answer ----
 * fathom-ntp (the udhcpc hook) keeps trying NTP while Wi-Fi is up, and counts
 * the tries that got no answer in /var/run/fathom-ntp-fails. While the clock
 * is plainly unset (before 2025) one is enough, else three: then the time is
 * taken from an HTTPS answer of the update check's host instead (netclock.c,
 * which says why its certificate's dates aren't checked), on a worker thread.
 * Once for each new count, so no more often than NTP is tried (1, 5, 15, then
 * every 30 minutes), until it has worked once. And at once when a secure
 * connection has just failed for want of the time (netclock_want: Find
 * stations, covers, the update check), at most every 30 s.
 *
 * The clock at a start is the last shutdown's (clock_restore): set, to look
 * at, and as many days behind as the H2 was off, so a server whose
 * certificate is newer than that is refused until NTP answers. Until the
 * network has given the time once since the player started
 * (netclock_confirmed), a connection that couldn't be opened asks for it here
 * as well (netclock_net_failed), set or not. The answer only ever moves the
 * clock forward (netclock_take), as before. */
#define CLOCK_HOST "api.github.com"          /* (UPDATE_API's) */
#define NTP_FAILS "/var/run/fathom-ntp-fails"
static volatile int g_https_clock_busy;     /* a worker is asking */
static volatile int g_https_clock_set;      /* it set the clock: for net_poll to keep */
static volatile int g_https_clock_done;     /* it had an answer: not asked again */
static volatile int g_clock_by_hand;        /* set in Settings since (it may be a little ahead) */

/* The time from one HTTPS answer of CLOCK_HOST, set if later (netclock_take):
 * the player's netclock fetcher, so one at a time (netclock_fetch, _ensure).
 * Blocking, seconds: worker threads only. 0 if the clock is set after it. */
int clock_from_https(void)
{
    int64_t t = 0;
    const char *ca = ff_ca_file();
    if (!ca || netclock_https_date(CLOCK_HOST, 443, ca, &t) != 0) {
        fprintf(stderr, "clock: no answer from %s\n", CLOCK_HOST);
        return -1;
    }
    int64_t now = (int64_t)time(NULL);
    if (netclock_take(t, now, g_clock_by_hand ? NETCLOCK_HAND_BACK : 0)) {
        struct timeval tv = {(time_t)t, 0};
        if (settimeofday(&tv, NULL)) { perror("settimeofday"); return -1; }
        fprintf(stderr, "clock: %lld s behind %s's time\n", (long long)(t - now), CLOCK_HOST);
        g_https_clock_set = g_https_clock_done = 1;
        g_clock_by_hand = 0;
    } else {
        fprintf(stderr, "clock: %s's time (%lld) isn't later: kept\n", CLOCK_HOST, (long long)t);
        g_https_clock_done = 1;              /* (the clock is fine as it is) */
    }
    netclock_set_confirmed(1);               /* (the network's time, taken or agreed with) */
    return netclock_unset() ? -1 : 0;
}

static void *https_clock_thread(void *arg)
{
    (void)arg;
    netclock_fetch();
    g_https_clock_busy = 0;
    return NULL;
}

/* Before every network connection (ff_set_before_net, on its thread): with
 * the clock unset, the time first, then the connection, which then succeeds:
 * Find stations, covers, podcasts, the update check, a station over https. */
void before_net(void)
{
    if (!netclock_unset() || !wifi_connected()) return;
    fprintf(stderr, "clock: unset before a secure connection: asking %s first\n", CLOCK_HOST);
    netclock_ensure();
}

static void https_clock_poll(void)
{
    static int last, ticks, asked_up;
    static uint32_t asked_at, up_since;      /* (clock_ms32; 0: never, not up) */
    if (g_https_clock_done || g_https_clock_busy || ++ticks % 10) return;   /* (every 5 s) */
    /* a fresh install (no clock.last), say: still unset 10 s after Wi-Fi joined,
     * NTP's first try not answered yet: asked then, once for each joining */
    int up = wifi_connected();
    if (!up) up_since = asked_up = 0;
    else if (!up_since) up_since = clock_ms32() | 1;
    int start = up && !asked_up && netclock_unset() && clock_ms32() - up_since >= 10000;
    /* (doubtful: unset, or the last shutdown's time and nothing from the network yet) */
    int want = netclock_wanted() && netclock_doubtful() &&
               (!asked_at || clock_ms32() - asked_at >= 30000);   /* (a network that blocks it too: not hammered) */
    int fails = 0;
    FILE *f = fopen(NTP_FAILS, "re");
    if (f) {
        if (fscanf(f, "%d", &fails) != 1) fails = 0;
        fclose(f);
    }
    if (fails < last) last = 0;              /* counting again (a new address) */
    int ntp = fails != last && (fails >= 3 || (fails >= 1 && netclock_unset()));
    if (!up || (!ntp && !want && !start)) return;
    if (ntp) last = fails;
    if (start) asked_up = 1;
    asked_at = clock_ms32() | 1;
    if (ntp) fprintf(stderr, "clock: no answer from NTP (%d): asking %s\n", fails, CLOCK_HOST);
    else if (start) fprintf(stderr, "clock: unset 10 s after Wi-Fi joined: asking %s\n", CLOCK_HOST);
    else fprintf(stderr, "clock: %s, and a connection failed: asking %s\n",
                 netclock_unset() ? "unset" : "not confirmed by the network since the start", CLOCK_HOST);
    g_https_clock_busy = 1;
    if (thread_detached(https_clock_thread, NULL, 192 * 1024)) g_https_clock_busy = 0;
}

/* About > Clock, set by hand (UTC): kept for the next start at once. NTP, or
 * an HTTPS answer (which may then move it back a day at most), sets it again
 * when Wi-Fi can. */
int clock_set_by_hand(int64_t t)
{
    struct timeval tv = {(time_t)t, 0};
    if (netclock_unset_at(t) || settimeofday(&tv, NULL)) { perror("settimeofday"); return -1; }
    fprintf(stderr, "clock: set by hand\n");
    clock_save();
    settings_save();
    if (g_batlog_on) batlog_clock_set(&g_batlog, time(NULL), clock_s());
    g_clock_by_hand = 1;
    g_https_clock_done = 0;                  /* (an HTTPS answer may set it right again) */
    netclock_set_confirmed(0);
    return 0;
}

/* Twice a second (net_poll): the clock was set from the network, by NTP
 * (fathom-ntp leaves /var/run/fathom-clock) or by the HTTPS answer above:
 * kept for the next start, and the battery's history told; then whether to
 * ask by HTTPS. */
void clock_poll(void)
{
    int by_https = g_https_clock_set;
    g_https_clock_set = 0;
    if (!access("/var/run/fathom-clock", F_OK) || by_https) {
        unlink("/var/run/fathom-clock");
        fprintf(stderr, "clock: set by %s\n", by_https ? CLOCK_HOST "'s answer" : "NTP");
        netclock_set_confirmed(1);
        clock_save();
        if (g_batlog_on) batlog_clock_set(&g_batlog, time(NULL), clock_s());   /* (how long it was off, known now) */
    }
    https_clock_poll();
}
