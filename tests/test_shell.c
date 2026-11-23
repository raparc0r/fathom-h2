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

#include "tmpdir.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

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
    CHECK(mkdtemp(test_tmp(dir, sizeof dir, "shell")) != NULL);
    snprintf(pids, sizeof pids, "%s/pids", dir);
    snprintf(cmd, sizeof cmd,
             "echo early; sleep 60 & echo $! > %s; (sleep 60; echo late) & echo $! >> %s; echo $$ >> %s; sleep 60",
             pids, pids, pids);
    long t0 = mono_ms();
    CHECK(sh_run_for(cmd, out, sizeof out, 1) == 124);
    long took = mono_ms() - t0;
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
    rmdir(dir);

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

int main(void)
{
    test_sh_run();
    test_wpa_status();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("shell: ok\n");
    return 0;
}
