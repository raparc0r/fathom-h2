/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fathom-avrcp: the player as Bluetooth headphones see it (AVRCP target).
 *
 * Without a media player registered with BlueZ, headphones learn nothing about
 * what's playing, and some (AirPods) then send only "play", whatever the
 * gesture. This registers one (org.bluez.Media1.RegisterPlayer, MPRIS-style
 * properties): its state, track and position come from the player's
 * /tmp/fathom-now (written while Bluetooth is on, main.c), and the commands
 * BlueZ calls on it (Play, Pause, Next, ...) go to the player as keys on a
 * uinput device whose name ends in " (AVRCP)", like BlueZ's own (input.c).
 *
 * Started and stopped by fathom-bt, with the card's libdbus.
 *
 * `fathom-avrcp pcm-open PCM` is a second, one-shot job for the player (out_bt.c),
 * which has no D-Bus of its own: it opens a bluez-alsa PCM (org.bluealsa.PCM1.Open)
 * and passes its two descriptors, the audio FIFO and the control socket (Pause,
 * Resume, Drop, Drain), back over the socket it was given as descriptor 3.
 */
#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NOW_FILE "/tmp/fathom-now"
#define OBJ      "/org/fathom/player"
#define MPRIS    "org.mpris.MediaPlayer2.Player"
#define PROPS    "org.freedesktop.DBus.Properties"

typedef struct {
    char status[12];             /* Playing, Paused, Stopped */
    long long id;
    unsigned len_ms, pos_ms;
    char title[256], artist[256], album[256];
} now_t;

static int ui_fd = -1;
static now_t cur;                /* as last told to BlueZ */
static uint32_t cur_at;          /* when (ms) */

static uint32_t ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ---- commands to the player: a small keyboard ---- */

static int uinput_open(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    static const int keys[] = {KEY_PLAY, KEY_PAUSECD, KEY_PLAYPAUSE, KEY_STOPCD, KEY_NEXTSONG, KEY_PREVIOUSSONG};
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) ioctl(fd, UI_SET_KEYBIT, keys[i]);
    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_VIRTUAL;
    snprintf(us.name, sizeof us.name, "Fathom player (AVRCP)");
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void emit(int type, int code, int value)
{
    struct input_event ie;
    memset(&ie, 0, sizeof ie);
    ie.type = (unsigned short)type;
    ie.code = (unsigned short)code;
    ie.value = value;
    if (write(ui_fd, &ie, sizeof ie) < 0) perror("fathom-avrcp: uinput");
}

static void key(int code)
{
    emit(EV_KEY, code, 1);
    emit(EV_SYN, SYN_REPORT, 0);
    emit(EV_KEY, code, 0);
    emit(EV_SYN, SYN_REPORT, 0);
}

/* ---- the player's state ---- */

static void read_now(now_t *n)
{
    memset(n, 0, sizeof *n);
    snprintf(n->status, sizeof n->status, "Stopped");
    FILE *f = fopen(NOW_FILE, "r");
    if (!f) return;
    char line[600];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        char *v = strchr(line, ' ');
        if (!v) continue;
        *v++ = '\0';
        if (!strcmp(line, "status")) snprintf(n->status, sizeof n->status, "%s", v);
        else if (!strcmp(line, "id")) n->id = atoll(v);
        else if (!strcmp(line, "len")) n->len_ms = (unsigned)strtoul(v, NULL, 10);
        else if (!strcmp(line, "pos")) n->pos_ms = (unsigned)strtoul(v, NULL, 10);
        else if (!strcmp(line, "title")) snprintf(n->title, sizeof n->title, "%s", v);
        else if (!strcmp(line, "artist")) snprintf(n->artist, sizeof n->artist, "%s", v);
        else if (!strcmp(line, "album")) snprintf(n->album, sizeof n->album, "%s", v);
    }
    fclose(f);
}

/* ---- MPRIS-style properties ---- */

static void entry_open(DBusMessageIter *dict, DBusMessageIter *e, DBusMessageIter *var, const char *k, const char *sig)
{
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, e);
    dbus_message_iter_append_basic(e, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(e, DBUS_TYPE_VARIANT, sig, var);
}

static void entry_close(DBusMessageIter *dict, DBusMessageIter *e, DBusMessageIter *var)
{
    dbus_message_iter_close_container(e, var);
    dbus_message_iter_close_container(dict, e);
}

static void put_str(DBusMessageIter *dict, const char *k, const char *v)
{
    DBusMessageIter e, var;
    entry_open(dict, &e, &var, k, "s");
    dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &v);
    entry_close(dict, &e, &var);
}

static void put_bool(DBusMessageIter *dict, const char *k, int v)
{
    DBusMessageIter e, var;
    dbus_bool_t b = v ? TRUE : FALSE;
    entry_open(dict, &e, &var, k, "b");
    dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &b);
    entry_close(dict, &e, &var);
}

static void put_i64(DBusMessageIter *dict, const char *k, int64_t v)
{
    DBusMessageIter e, var;
    dbus_int64_t x = v;
    entry_open(dict, &e, &var, k, "x");
    dbus_message_iter_append_basic(&var, DBUS_TYPE_INT64, &x);
    entry_close(dict, &e, &var);
}

static void put_metadata(DBusMessageIter *dict, const now_t *n)
{
    DBusMessageIter e, var, md, arr;
    entry_open(dict, &e, &var, "Metadata", "a{sv}");
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "{sv}", &md);
    char path[64];
    snprintf(path, sizeof path, "/org/fathom/track/%lld", n->id > 0 ? n->id : 0);
    const char *p = path, *k = "mpris:trackid";
    {
        DBusMessageIter e2, v2;
        entry_open(&md, &e2, &v2, k, "o");
        dbus_message_iter_append_basic(&v2, DBUS_TYPE_OBJECT_PATH, &p);
        entry_close(&md, &e2, &v2);
    }
    put_str(&md, "xesam:title", n->title);
    put_str(&md, "xesam:album", n->album);
    {
        DBusMessageIter e2, v2;
        const char *a = n->artist;
        entry_open(&md, &e2, &v2, "xesam:artist", "as");
        dbus_message_iter_open_container(&v2, DBUS_TYPE_ARRAY, "s", &arr);
        dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &a);
        dbus_message_iter_close_container(&v2, &arr);
        entry_close(&md, &e2, &v2);
    }
    put_i64(&md, "mpris:length", (int64_t)n->len_ms * 1000);
    dbus_message_iter_close_container(&var, &md);
    entry_close(dict, &e, &var);
}

enum { P_STATUS = 1, P_META = 2, P_POS = 4, P_CAPS = 8, P_ALL = 15 };

static void put_props(DBusMessageIter *it, const now_t *n, int which)
{
    DBusMessageIter dict;
    dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    if (which & P_CAPS) {
        put_str(&dict, "Identity", "Fathom");
        put_bool(&dict, "CanControl", 1);
        put_bool(&dict, "CanPlay", 1);
        put_bool(&dict, "CanPause", 1);
        put_bool(&dict, "CanGoNext", 1);
        put_bool(&dict, "CanGoPrevious", 1);
    }
    if (which & P_STATUS) put_str(&dict, "PlaybackStatus", n->status);
    if (which & P_META) put_metadata(&dict, n);
    if (which & P_POS) put_i64(&dict, "Position", (int64_t)n->pos_ms * 1000);
    dbus_message_iter_close_container(it, &dict);
}

/* ---- D-Bus ---- */

static DBusHandlerResult on_message(DBusConnection *c, DBusMessage *m, void *data)
{
    (void)data;
    static const struct { const char *method; int key; } cmds[] = {
        {"Play", KEY_PLAY}, {"Pause", KEY_PAUSECD}, {"PlayPause", KEY_PLAYPAUSE},
        {"Stop", KEY_STOPCD}, {"Next", KEY_NEXTSONG}, {"Previous", KEY_PREVIOUSSONG},
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        if (!dbus_message_is_method_call(m, MPRIS, cmds[i].method)) continue;
        fprintf(stderr, "fathom-avrcp: %s\n", cmds[i].method);
        key(cmds[i].key);
        if (!dbus_message_get_no_reply(m)) {
            DBusMessage *r = dbus_message_new_method_return(m);
            dbus_connection_send(c, r, NULL);
            dbus_message_unref(r);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(m, PROPS, "GetAll")) {
        DBusMessage *r = dbus_message_new_method_return(m);
        DBusMessageIter it;
        dbus_message_iter_init_append(r, &it);
        put_props(&it, &cur, P_ALL);
        dbus_connection_send(c, r, NULL);
        dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int register_player(DBusConnection *c)
{
    DBusMessage *m = dbus_message_new_method_call("org.bluez", "/org/bluez/hci0", "org.bluez.Media1", "RegisterPlayer");
    DBusMessageIter it;
    const char *obj = OBJ;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &obj);
    put_props(&it, &cur, P_ALL);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(c, m, 5000, &err);
    dbus_message_unref(m);
    if (!r) {
        static char last[200];
        if (strcmp(last, err.message ? err.message : "")) {   /* say each new reason once */
            snprintf(last, sizeof last, "%s", err.message ? err.message : "");
            fprintf(stderr, "fathom-avrcp: not registered yet: %s\n", last);
        }
        dbus_error_free(&err);
        return 0;
    }
    dbus_message_unref(r);
    fprintf(stderr, "fathom-avrcp: registered with BlueZ\n");
    return 1;
}

static void changed(DBusConnection *c, int which)
{
    DBusMessage *s = dbus_message_new_signal(OBJ, PROPS, "PropertiesChanged");
    DBusMessageIter it, inv;
    const char *iface = MPRIS;
    dbus_message_iter_init_append(s, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    put_props(&it, &cur, which);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &inv);
    dbus_message_iter_close_container(&it, &inv);
    dbus_connection_send(c, s, NULL);
    dbus_message_unref(s);
}

/* pcm-open: the PCM's FIFO and control socket to descriptor 3 (SCM_RIGHTS). */
static int pcm_open(const char *path)
{
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
    if (!c) { fprintf(stderr, "fathom-avrcp: %s\n", err.message); return 1; }
    DBusMessage *m = dbus_message_new_method_call("org.bluealsa", path, "org.bluealsa.PCM1", "Open");
    DBusMessage *r = dbus_connection_send_with_reply_and_block(c, m, 5000, &err);
    dbus_message_unref(m);
    int fds[2] = {-1, -1};
    if (!r || !dbus_message_get_args(r, &err, DBUS_TYPE_UNIX_FD, &fds[0], DBUS_TYPE_UNIX_FD, &fds[1],
                                     DBUS_TYPE_INVALID)) {
        fprintf(stderr, "fathom-avrcp: can't open %s: %s\n", path, err.message ? err.message : "?");
        return 1;
    }
    dbus_message_unref(r);
    char byte = 'P';
    struct iovec iov = {.iov_base = &byte, .iov_len = 1};
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof fds)]; } u;
    memset(&u, 0, sizeof u);
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.buf, .msg_controllen = sizeof u.buf};
    struct cmsghdr *h = CMSG_FIRSTHDR(&msg);
    h->cmsg_level = SOL_SOCKET;
    h->cmsg_type = SCM_RIGHTS;
    h->cmsg_len = CMSG_LEN(sizeof fds);
    memcpy(CMSG_DATA(h), fds, sizeof fds);
    if (sendmsg(3, &msg, 0) != 1) { perror("fathom-avrcp: sendmsg"); return 1; }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "pcm-open")) return pcm_open(argv[2]);
    ui_fd = uinput_open();
    if (ui_fd < 0) { perror("fathom-avrcp: /dev/uinput"); return 1; }
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *c = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!c) { fprintf(stderr, "fathom-avrcp: %s\n", err.message); return 1; }
    static const DBusObjectPathVTable vt = {.message_function = on_message};
    dbus_connection_register_object_path(c, OBJ, &vt, NULL);

    read_now(&cur);
    cur_at = ms_now();
    int registered = 0;
    uint32_t tried = 0;
    while (dbus_connection_read_write_dispatch(c, 500)) {
        uint32_t t = ms_now();
        if (!registered) {
            if (t - tried >= 2000 || !tried) { tried = t; registered = register_player(c); }
            if (!registered) { read_now(&cur); cur_at = t; continue; }
        }
        now_t n;
        read_now(&n);
        int which = 0;
        if (strcmp(n.status, cur.status)) which |= P_STATUS;
        if (n.id != cur.id || strcmp(n.title, cur.title)) which |= P_META | P_POS;
        /* a seek: the position isn't where playing on from the last one puts it */
        int64_t expect = (int64_t)cur.pos_ms + (!strcmp(cur.status, "Playing") ? (int64_t)(t - cur_at) : 0);
        if (llabs((int64_t)n.pos_ms - expect) > 2000) which |= P_POS;
        if (!which) continue;
        cur = n;
        cur_at = t;
        changed(c, which);
    }
    return 0;
}
