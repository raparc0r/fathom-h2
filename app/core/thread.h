/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Starting a thread nobody waits for, and making the calling thread yield to
 * the others: the same few lines stood wherever a worker is started. */
#ifndef FATHOM_THREAD_H
#define FATHOM_THREAD_H

#include <pthread.h>
#include <stddef.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

/* fn(arg) on a detached thread, with a stack of `stack` bytes (0: the C
 * library's own size; the H2's RAM is why most ask for a small one). 0, or -1
 * if it couldn't be started. */
static inline int thread_detached(void *(*fn)(void *), void *arg, size_t stack)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (stack) pthread_attr_setstacksize(&a, stack);
    int r = pthread_create(&t, &a, fn, arg);
    pthread_attr_destroy(&a);
    return r == 0 ? 0 : -1;
}

/* The calling thread's niceness (19: behind the music and the interface on
 * the H2's one core). On Linux a thread has its own; elsewhere this does
 * nothing, as before. (A file that calls it needs syscall() declared: its
 * own _GNU_SOURCE or _DEFAULT_SOURCE, as those that did this by hand had.) */
static inline void thread_nice(int n)
{
#ifdef __linux__
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), n);
#else
    (void)n;
#endif
}

#endif
