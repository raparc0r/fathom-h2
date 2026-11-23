/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* What the player's workers (wifi.c, bt.c, share.c, ssh.c) need to run the
 * system's scripts and read the network's state. Safe from any thread. */
#ifndef FATHOM_SHELL_H
#define FATHOM_SHELL_H

#include <stddef.h>

/* Runs a shell command; its first `cap` bytes of output to out (NULL: none
 * kept, all read). Returns the exit status, -1 if it couldn't run. */
int  sh_run(const char *cmd, char *out, size_t cap);
/* The same, given up after secs (what it started killed with it; 124). sh_run
 * waits SH_RUN_SECS: the longest any of the scripts' commands should take
 * (Wi-Fi on waits for the chip twice, 45 s each). Before, a command that
 * never returned (bluetoothctl waiting for a bluetoothd that never came up)
 * left its worker "Starting" for good, and Bluetooth couldn't be turned off. */
#define SH_RUN_SECS 150
int  sh_run_for(const char *cmd, char *out, size_t cap, int secs);
/* A worker's timed wait that the clock being set can't stretch: the H2 starts
 * in 1970 and NTP sets the clock seconds after Wi-Fi joins; a clock set ahead
 * by hand and then put right stepped it back, and a wait measured on it then
 * lasted that much longer (hours without a status look). mono_cond_init before
 * the worker starts (the condition variable then counts on CLOCK_MONOTONIC);
 * mono_cond_wait_ms waits up to ms: 0 if woken, ETIMEDOUT. */
#include <pthread.h>
void mono_cond_init(pthread_cond_t *cv);

/* The start of the player: Wi-Fi, Bluetooth, SSH and the network drive each
 * have a worker thread whose first work is a script (modules loaded, daemons
 * started), and on the one CPU those ran through the drawing of Home: a third
 * of a second of every start. Each worker calls sh_wait_home() before its
 * first work; main calls sh_home_shown() once Home is on screen. A worker
 * waits max_ms at most (Home is late when the first scan's screen comes
 * before it: the radios start then all the same). */
void sh_home_shown(void);
void sh_wait_home(int max_ms);
int  mono_cond_wait_ms(pthread_cond_t *cv, pthread_mutex_t *mu, int ms);
/* s as one single-quoted shell word ('it'\''s), cut to fit cap. */
void sh_quote(const char *s, char *out, size_t cap);
/* An interface's IPv4 address, "" without one. */
void if_ip(const char *name, char *out, size_t cap);
/* wpa_supplicant's answer to STATUS, read (KEY=VALUE lines): its state as it
 * names it (COMPLETED once joined), the network's name as its real bytes (the
 * escapes it prints undone the way fathom-wifi undoes them: wifi.c compares
 * the two), the address if it knows one. Returns 1 if the answer had a
 * wpa_state (anything else, "FAIL" or an answer cut short, isn't a status). */
typedef struct { char state[24]; char ssid[33]; char ip[16]; } wpa_status_t;
int  wpa_status_parse(const char *reply, wpa_status_t *out);
/* KEY's whole-number value in such an answer (SIGNAL_POLL's RSSI) into *out:
 * 1 if the answer has one. */
int  wpa_reply_int(const char *reply, const char *key, int *out);

#endif
