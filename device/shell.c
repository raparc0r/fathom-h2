/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shell helpers shared by the workers that drive fathom-wifi, fathom-bt,
 * fathom-share and fathom-ssh (see shell.h). */
#include "shell.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

int sh_run(const char *cmd, char *out, size_t cap)
{
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    size_t got = 0;
    if (out && cap) {
        size_t r;
        while (got + 1 < cap && (r = fread(out + got, 1, cap - 1 - got, f)) > 0) got += r;
        out[got] = '\0';
    }
    char sink[256];
    while (fread(sink, 1, sizeof sink, f) > 0) {}   /* (the rest read, so it never blocks writing) */
    int st = pclose(f);
    return st < 0 ? -1 : (st >> 8) & 0xff;
}

void sh_quote(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    if (cap < 3) { if (cap) out[0] = '\0'; return; }
    out[o++] = '\'';
    for (; *s && o + 5 < cap; s++) {
        if (*s == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }
        else out[o++] = *s;
    }
    out[o++] = '\'';
    out[o] = '\0';
}

void if_ip(const char *name, char *out, size_t cap)
{
    out[0] = '\0';
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs)) return;
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || strcmp(i->ifa_name, name)) continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out, (socklen_t)cap);
        break;
    }
    freeifaddrs(ifs);
}
