/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* device/shell.c on the host: commands run for the workers (their output, exit
 * status, deadline, and what a deadline leaves behind: nothing), and
 * wpa_supplicant's answers read (what wifi.c asks it while connected, in place
 * of `fathom-wifi status`). */
#define _GNU_SOURCE   /* (as shell.c: pipe2, on Linux) */
#include <pthread.h>
#ifdef __APPLE__   /* (macOS has no pthread_condattr_setclock; the timed waits aren't tested here) */
#define pthread_condattr_setclock(a, c) ((void)(a))
#endif
#include "../device/shell.c"

#include <sys/stat.h>

#include "test_util.h"

/* pid is gone, within 3 s */
static int gone(pid_t pid)
{
    for (int i = 0; i < 300; i++) {
        if (kill(pid, 0) != 0 && errno == ESRCH) return 1;
        usleep(10000);
    }
    return 0;
}

static void *many(void *arg)
{
    long id = (long)arg, bad = 0;
    for (int i = 0; i < 25; i++) {
        char cmd[64], want[32], out[64];
        snprintf(cmd, sizeof cmd, "echo %ld-%d; exit %d", id, i, i);
        snprintf(want, sizeof want, "%ld-%d\n", id, i);
        if (sh_run(cmd, out, sizeof out) != i || strcmp(out, want)) bad++;
    }
    return (void *)bad;
}

/* fork() runs these; posix_spawn doesn't (it never copies the process) */
static int forks;
static void forked(void) { forks++; }

static void test_sh_run(void)
{
    static char big[400000];
    char out[256], cmd[1024], q[128];
    pthread_atfork(forked, NULL, NULL);

    /* what it printed and how it ended; what it wrote to stderr isn't taken */
    CHECK(sh_run("echo hello; exit 3", out, sizeof out) == 3 && !strcmp(out, "hello\n"));
    strcpy(out, "stale");
    CHECK(sh_run("true", out, sizeof out) == 0 && !out[0]);
    CHECK(sh_run("{ echo no >&2; echo yes; } 2>/dev/null", out, sizeof out) == 0 && !strcmp(out, "yes\n"));
    CHECK(sh_run("/no/such/program 2>/dev/null", out, sizeof out) == 127);
    CHECK(sh_run("kill -9 $$", out, sizeof out) == -1);            /* (ended by a signal) */
    /* the player's environment (the scripts' PATH); nothing to read on its input */
    setenv("FATHOM_TEST_SHELL", "from the player", 1);
    CHECK(sh_run("echo \"$FATHOM_TEST_SHELL\"", out, sizeof out) == 0 && !strcmp(out, "from the player\n"));
    CHECK(sh_run_for("cat; echo end", out, sizeof out, 10) == 0 && !strcmp(out, "end\n"));
    /* a name as one word, whatever is in it */
    const char *name = "it's a \"net\" $HOME `id` \\ ; & | * end";
    sh_quote(name, q, sizeof q);
    snprintf(cmd, sizeof cmd, "printf %%s %s", q);
    CHECK(sh_run(cmd, out, sizeof out) == 0 && !strcmp(out, name));

    /* More than a pipe holds (64 KB), and more than there's room for: all of
     * it read, so the command isn't left waiting to write; the first of it
     * kept, the status its own */
    const char *lots = "head -c 300000 /dev/zero | tr '\\0' x; exit 7";
    CHECK(sh_run(lots, big, sizeof big) == 7 && strlen(big) == 300000 && strspn(big, "x") == 300000);
    memset(out, 'y', sizeof out);
    CHECK(sh_run(lots, out, 100) == 7 && strlen(out) == 99 && strspn(out, "x") == 99 && out[100] == 'y');
    CHECK(sh_run(lots, NULL, 0) == 7);
    out[0] = 'y';
    CHECK(sh_run(lots, out, 1) == 7 && !out[0]);

    /* A deadline: 124 after about that long, what came before it kept, and
     * everything the command started ended with it, those still holding its
     * output included (they'd keep a plain read waiting for ever) */
    char dir[256], pids[300];
    CHECK(t_mkdtemp(dir, sizeof dir, "shell") != NULL);
    snprintf(pids, sizeof pids, "%s/pids", dir);
    snprintf(cmd, sizeof cmd,
             "echo early; sleep 60 & echo $! > %s; (sleep 60; echo late) & echo $! >> %s; echo $$ >> %s; sleep 60",
             pids, pids, pids);
    long t0 = (long)clock_ms();
    CHECK(sh_run_for(cmd, out, sizeof out, 1) == 124);
    long took = (long)clock_ms() - t0;
    CHECK(took >= 1000 && took < 4000);
    CHECK(!strcmp(out, "early\n"));
    FILE *f = fopen(pids, "r");
    int a = 0, b = 0, c = 0;
    CHECK(f && fscanf(f, "%d %d %d", &a, &b, &c) == 3);
    if (f) fclose(f);
    CHECK(a > 1 && b > 1 && c > 1 && a != b && c != getpid());
    if (a > 1 && b > 1 && c > 1) CHECK(gone(a) && gone(b) && gone(c));
    /* one that ends in time isn't cut */
    CHECK(sh_run_for("sleep 0.2; echo done", out, sizeof out, 5) == 0 && !strcmp(out, "done\n"));
    unlink(pids);

    /* What the log says of one that was cut: the helper's name and no more.
     * Its arguments can be a password (fathom-wifi connect NAME PASSWORD; the
     * network drive's, printed into fathom-share), and the log is a file on
     * the card. */
    char helper[300], logf[300], said[600];
    snprintf(helper, sizeof helper, "%s/fathom-helper", dir);
    snprintf(logf, sizeof logf, "%s/log", dir);
    f = fopen(helper, "w");
    CHECK(f != NULL);
    if (f) { fputs("#!/bin/sh\nsleep 60\n", f); fclose(f); }
    chmod(helper, 0755);
    const char *forms[] = {
        "%s connect 'Home net' 'not-a-real-secret' >/dev/null 2>&1",
        "printf '%%s\\n' 'not-a-real-secret' | %s on '/mnt/sd/Music' 2>&1 >/dev/null",
        "%s connect 'it'\\''s' 'x'\\'' /not-a-real-secret' >/dev/null 2>&1",
        /* (the shell becoming the command, its output sent elsewhere: nothing
         * holds the pipe open, as with the H2's shell and the forms above.
         * The deadline holds all the same: it was never looked at once the
         * pipe had closed, and such a helper was waited for without end) */
        "exec %s connect 'Home net' 'not-a-real-secret' >/dev/null 2>&1",
    };
    for (int i = 0; i < 4; i++) {
        snprintf(cmd, sizeof cmd, forms[i], helper);
        fflush(stderr);
        int keep = dup(2), lf = open(logf, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        CHECK(keep >= 0 && lf >= 0);
        dup2(lf, 2);
        int st = sh_run_for(cmd, NULL, 0, 1);
        fflush(stderr);
        dup2(keep, 2);
        close(keep);
        close(lf);
        CHECK(st == 124);
        memset(said, 0, sizeof said);
        f = fopen(logf, "r");
        CHECK(f && fread(said, 1, sizeof said - 1, f) > 0);
        if (f) fclose(f);
        CHECK(strstr(said, "fathom-helper") != NULL);
        CHECK(!strstr(said, "secret") && !strstr(said, "Home") && !strstr(said, "Music") && !strstr(said, "connect"));
    }
    unlink(helper);
    unlink(logf);
    rmdir(dir);
    /* (the name alone, on the lines the workers build) */
    cmd_name("/usr/sbin/fathom-wifi connect 'a b' '/c d' >/dev/null 2>&1", said, sizeof said);
    CHECK(!strcmp(said, "fathom-wifi"));
    cmd_name("printf '%s\\n' 'pw' | /usr/sbin/fathom-share on '/mnt/sd/Music' 2>&1 >/dev/null", said, sizeof said);
    CHECK(!strcmp(said, "fathom-share"));
    cmd_name("sleep 60", said, sizeof said);
    CHECK(!strcmp(said, "sleep"));
    cmd_name("'all quoted'", said, sizeof said);
    CHECK(!strcmp(said, "?"));
    cmd_name("", said, sizeof said);
    CHECK(!strcmp(said, "?"));
    cmd_name("/usr/sbin/a-helper-with-a-name-far-longer-than-there-is-room-for-in-the-log x", said, 16);
    CHECK(strlen(said) == 15);

    /* two workers at once (Wi-Fi's and Bluetooth's): each its own output and status */
    pthread_t th[4];
    for (long i = 0; i < 4; i++) pthread_create(&th[i], NULL, many, (void *)i);
    for (int i = 0; i < 4; i++) {
        void *bad = (void *)1;
        pthread_join(th[i], &bad);
        CHECK(bad == NULL);
    }

    /* and through all of that the process was never forked (on the H2: its
     * pages never write-protected under the audio thread) */
    CHECK(forks == 0);
}

static void test_wpa_status(void)
{
    wpa_status_t s;
    int v;

    /* connected, as wpa_supplicant 2.11 answers STATUS */
    const char *joined =
        "bssid=00:11:22:33:44:55\nfreq=2437\nssid=Home\nid=0\nmode=station\nwifi_generation=4\n"
        "pairwise_cipher=CCMP\ngroup_cipher=CCMP\nkey_mgmt=WPA2-PSK\nwpa_state=COMPLETED\n"
        "ip_address=192.168.1.5\naddress=aa:bb:cc:dd:ee:ff\nuuid=0f0e0d0c-0b0a-0908-0706-050403020100\n";
    CHECK(wpa_status_parse(joined, &s) == 1);
    CHECK(!strcmp(s.state, "COMPLETED") && !strcmp(s.ssid, "Home") && !strcmp(s.ip, "192.168.1.5"));

    /* joined, no address yet; nothing joined (no ssid line at all) */
    CHECK(wpa_status_parse("bssid=00:11:22:33:44:55\nssid=Home\nid=0\nwpa_state=COMPLETED\naddress=aa:bb:cc:dd:ee:ff\n", &s) == 1);
    CHECK(!strcmp(s.state, "COMPLETED") && !strcmp(s.ssid, "Home") && !s.ip[0]);
    CHECK(wpa_status_parse("wpa_state=SCANNING\naddress=aa:bb:cc:dd:ee:ff\n", &s) == 1);
    CHECK(!strcmp(s.state, "SCANNING") && !s.ssid[0] && !s.ip[0]);

    /* "bssid=" isn't "ssid="; a key is only a key at a line's start; the first one counts */
    CHECK(wpa_status_parse("bssid=x\nwpa_state=COMPLETED\n", &s) == 1 && !s.ssid[0]);
    CHECK(wpa_status_parse("ssid=A ssid=B\nwpa_state=COMPLETED\nssid=C\n", &s) == 1 && !strcmp(s.ssid, "A ssid=B"));

    /* not a status: an error, another command's answer, nothing, a state cut off */
    CHECK(wpa_status_parse("FAIL\n", &s) == 0 && !s.state[0]);
    CHECK(wpa_status_parse("OK\n", &s) == 0);
    CHECK(wpa_status_parse("", &s) == 0);
    CHECK(wpa_status_parse("ssid=Home\nwpa_state=", &s) == 0);
    CHECK(wpa_status_parse("RSSI=-50\nLINKSPEED=65\nNOISE=9999\nFREQUENCY=2437\n", &s) == 0);
    /* an answer without its last newline */
    CHECK(wpa_status_parse("ssid=Home\nwpa_state=COMPLETED", &s) == 1 && !strcmp(s.state, "COMPLETED"));

    /* names as wpa_supplicant escapes them, back to their bytes: the same pairs
     * as tests/test_fathom_wifi.sh (the script's names and these must agree) */
    CHECK(wpa_status_parse("ssid=Anna\\xe2\\x80\\x99s iPhone\nwpa_state=COMPLETED\n", &s) == 1);
    CHECK(!strcmp(s.ssid, "Anna\xe2\x80\x99s iPhone"));
    CHECK(wpa_status_parse("ssid=Bob \\\"5G\\\" \\\\ 100%\nwpa_state=COMPLETED\n", &s) == 1);
    CHECK(!strcmp(s.ssid, "Bob \"5G\" \\ 100%"));
    CHECK(wpa_status_parse("ssid=-dash | bar\nwpa_state=COMPLETED\n", &s) == 1 && !strcmp(s.ssid, "-dash | bar"));
    /* control characters, however written, are "?" (as the script's) */
    CHECK(wpa_status_parse("ssid=a\\tb\\nc\\rd\\ee\\x01f\\x7fg\\x00h\nwpa_state=COMPLETED\n", &s) == 1);
    CHECK(!strcmp(s.ssid, "a?b?c?d?e?f?g?h"));
    /* escapes that aren't: a lone backslash at the end, \x without two hex digits */
    CHECK(wpa_status_parse("ssid=end\\\nwpa_state=COMPLETED\n", &s) == 1 && !strcmp(s.ssid, "end?"));
    CHECK(wpa_status_parse("ssid=a\\xZZb\nwpa_state=COMPLETED\n", &s) == 1 && !strcmp(s.ssid, "a?b"));
    /* the longest name there is (32 bytes), every one escaped: whole; anything longer is cut, not overrun */
    char long_name[400] = "ssid=", want[33] = "";
    for (int i = 0; i < 32; i++) { strcat(long_name, "\\xc3"); want[i] = '\xc3'; }
    strcat(long_name, "\nwpa_state=COMPLETED\n");
    CHECK(wpa_status_parse(long_name, &s) == 1 && !strcmp(s.ssid, want));
    CHECK(wpa_status_parse("ssid=0123456789012345678901234567890123456789\nwpa_state=COMPLETED\n", &s) == 1);
    CHECK(strlen(s.ssid) == 32);
    CHECK(wpa_status_parse("wpa_state=COMPLETED\nip_address=111.222.333.444.555.666\n", &s) == 1 && strlen(s.ip) == 15);
    CHECK(wpa_status_parse("wpa_state=A_STATE_NAME_FAR_LONGER_THAN_ANY_THERE_IS\n", &s) == 1 && strlen(s.state) == 23);

    /* SIGNAL_POLL */
    v = 1;
    CHECK(wpa_reply_int("RSSI=-50\nLINKSPEED=65\nNOISE=9999\nFREQUENCY=2437\n", "RSSI", &v) == 1 && v == -50);
    CHECK(wpa_reply_int("RSSI=-50\nLINKSPEED=65\nNOISE=9999\nFREQUENCY=2437\n", "FREQUENCY", &v) == 1 && v == 2437);
    v = 7;
    CHECK(wpa_reply_int("FAIL\n", "RSSI", &v) == 0 && v == 7);
    CHECK(wpa_reply_int("AVG_RSSI=-60\n", "RSSI", &v) == 0);
    CHECK(wpa_reply_int("RSSI=\n", "RSSI", &v) == 0);
    CHECK(wpa_reply_int("RSSI=-\n", "RSSI", &v) == 0);
    CHECK(wpa_reply_int("RSSI=abc\n", "RSSI", &v) == 0 && v == 7);
}

/* A server looked for by its pid file, and what's done when it's gone (the
 * network drive, SSH): started again three times in a row at most. */
static void test_revive(void)
{
    char tmpl[600], p[700];
    char *dir = t_mkdtemp(tmpl, sizeof tmpl, "fathom-shell-pid");
    CHECK(dir != NULL);
    if (!dir) return;
    snprintf(p, sizeof p, "%s/server.pid", dir);
    CHECK(sh_pid_alive(p) == 0);                       /* no file */
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%ld\n", (long)getpid()); fclose(f); }
    CHECK(sh_pid_alive(p) == 1);                       /* a process that is there */
    pid_t child = fork();
    if (child == 0) _exit(0);
    CHECK(child > 0);
    waitpid(child, NULL, 0);
    f = fopen(p, "w");
    if (f) { fprintf(f, "%ld\n", (long)child); fclose(f); }
    CHECK(sh_pid_alive(p) == 0);                       /* one that has gone */
    const char *junk[] = {"", "\n", "none\n", "0\n", "-1\n"};   /* (0 and -1: never handed to kill()) */
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++) {
        f = fopen(p, "w");
        if (f) { fputs(junk[i], f); fclose(f); }
        CHECK(sh_pid_alive(p) == 0);
    }
    unlink(p);
    rmdir(dir);

    /* there: nothing to do */
    sh_revive_t r = {0};
    uint32_t t = 4294900000u;                          /* (the clock wraps on the way) */
    CHECK(sh_revive(&r, 1, t) == SH_WELL && r.revived == 0);
    /* gone at three looks running, a minute apart: started again each time; at the fourth, given up */
    CHECK(sh_revive(&r, 0, t += SH_LOOK_MS) == SH_REVIVE && r.revived == 1);
    CHECK(sh_revive(&r, 0, t += SH_LOOK_MS) == SH_REVIVE && r.revived == 2);
    CHECK(sh_revive(&r, 0, t += SH_LOOK_MS) == SH_REVIVE && r.revived == 3);
    CHECK(sh_revive(&r, 0, t += SH_LOOK_MS) == SH_GIVE_UP);
    CHECK(sh_revive(&r, 0, t += SH_LOOK_MS) == SH_GIVE_UP);
    /* up for a few minutes between two of them is still "in a row" */
    r = (sh_revive_t){0};
    CHECK(sh_revive(&r, 0, t += SH_LOOK_MS) == SH_REVIVE);
    CHECK(sh_revive(&r, 1, t += SH_LOOK_MS) == SH_WELL && r.revived == 1);
    CHECK(sh_revive(&r, 1, t += 3 * SH_LOOK_MS) == SH_WELL && r.revived == 1);
    CHECK(sh_revive(&r, 0, t += SH_LOOK_MS) == SH_REVIVE && r.revived == 2);
    CHECK(sh_revive(&r, 0, t += SH_LOOK_MS) == SH_REVIVE && r.revived == 3);
    CHECK(sh_revive(&r, 1, t += 4 * SH_LOOK_MS) == SH_WELL && r.revived == 3);
    CHECK(sh_revive(&r, 0, t += SH_LOOK_MS) == SH_GIVE_UP);
    /* up for five minutes since it was last started again: the count from nothing */
    r = (sh_revive_t){0};
    CHECK(sh_revive(&r, 0, t += SH_LOOK_MS) == SH_REVIVE);
    CHECK(sh_revive(&r, 0, t += SH_LOOK_MS) == SH_REVIVE);
    CHECK(sh_revive(&r, 0, t += SH_LOOK_MS) == SH_REVIVE);
    CHECK(sh_revive(&r, 1, t += 5 * SH_LOOK_MS) == SH_WELL && r.revived == 0);
    CHECK(sh_revive(&r, 0, t += SH_LOOK_MS) == SH_REVIVE && r.revived == 1);
}

int main(void)
{
    test_sh_run();
    test_wpa_status();
    test_revive();
    return t_result();
}
