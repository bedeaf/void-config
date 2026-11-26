// thaw: watches an X11 desktop for freezes and fixes the ones caused by the compositor.
//
// Symptom it targets: the screen and keyboard freeze, but the mouse pointer still moves
// (Xorg moves the pointer from its input thread, so the pointer says nothing about the rest).
//
// Every second thaw does two checks:
//   1. Is the X server answering? A blocked XSync is noticed by a second thread. After a
//      while that thread kills the compositor, because a compositor that hangs while holding
//      a server grab freezes everything. If X answers again, the compositor was the cause.
//      If not, it is the X server or the GPU, and the log says so with exact times.
//   2. Is the compositor still drawing? A 1x1 window in a screen corner switches between two
//      colours that look the same to the eye (#0a0a0a / #0b0b0b), and thaw reads the pixel
//      back from the screen. If X answers but the pixel stops following for a few seconds,
//      the compositor has stopped painting and is restarted. A miss does not count while
//      something legitimately hides the probe: the X screen saver (`xset s blank` blanks
//      the output without DPMS, and the compositor may stop at the missing vblanks), a
//      window above it (a locker, a menu) or a running screen locker (xsecurelock draws
//      on the composite overlay, above everything the compositor paints).
//
// The compositor is found through the owner of the _NET_WM_CM_Sn selection and the XRes
// extension, never by process name, so thaw cannot kill the wrong process.
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XRes.h>
#include <X11/extensions/dpms.h>
#include <X11/Xlib-xcb.h>
#include <xcb/screensaver.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const char *restart_cmd = "picom --daemon";
static const char *lockers = "xsecurelock,i3lock,slock,xlock,xscreensaver"; /* -L */
static int interval_ms = 1000;   /* one check per second */
static int settle_ms = 200;      /* time the compositor gets to paint the probe (>= 10 frames) */
static int fail_limit = 5;       /* consecutive missed frames before a restart */
static int stall_log_s = 3;      /* X silent this long: log it */
static int stall_kill_s = 6;     /* X silent this long: kill the compositor */
static int dry_run, verbose;
static char logpath[PATH_MAX];
static FILE *logfp;

static _Atomic long long last_answer_ms;  /* last time an X request returned */
static atomic_int in_request;             /* main thread is waiting for X */
static atomic_int comp_pid;               /* compositor PID, cached while X is healthy */
static atomic_int need_restart;           /* monitor thread killed the compositor */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static void sleep_ms(int ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) && errno == EINTR)
        ;
}

static void logmsg(const char *fmt, ...) {
    char ts[32];
    time_t t = time(NULL);
    va_list ap;

    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    pthread_mutex_lock(&log_lock);
    for (int i = 0; i < 2; i++) {
        FILE *f = i == 0 ? logfp : (verbose && logfp != stderr ? stderr : NULL);
        if (!f)
            continue;
        fprintf(f, "%s ", ts);
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fflush(f);
    }
    pthread_mutex_unlock(&log_lock);
}

/* ------------------------------------------------------------- process helpers */

static int read_file(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return (int)n;
}

/* comm matches one of the comma-separated names in list */
static int comm_in_list(const char *comm, const char *list) {
    size_t len = strlen(comm);

    for (const char *p = list; *p; p += strcspn(p, ",") + (p[strcspn(p, ",")] == ',')) {
        size_t n = strcspn(p, ",");
        if (n == len && !strncmp(p, comm, n))
            return 1;
    }
    return 0;
}

/* First process whose comm is one of the comma-separated names. Not used to find the
 * compositor (that comes from XRes); only for the log snapshot and screen lockers. */
static pid_t pid_by_comm(const char *names) {
    char path[64], c[32];
    struct dirent *e;
    pid_t found = 0;
    DIR *d = opendir("/proc");

    if (!d)
        return 0;
    while (!found && (e = readdir(d))) {
        char *end;
        long n = strtol(e->d_name, &end, 10);
        if (*end || n <= 0)
            continue;
        snprintf(path, sizeof path, "/proc/%ld/comm", n);
        if (read_file(path, c, sizeof c) > 0) {
            c[strcspn(c, "\n")] = '\0';
            if (comm_in_list(c, names))
                found = (pid_t)n;
        }
    }
    closedir(d);
    return found;
}

static void describe(const char *label, pid_t pid) {
    char path[64], stat[1024], wchan[64] = "?";
    char state = '?';
    unsigned long ut = 0, st = 0;
    long tick = sysconf(_SC_CLK_TCK);

    if (pid <= 0) {
        logmsg("  %-10s not running", label);
        return;
    }
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    if (read_file(path, stat, sizeof stat) > 0) {
        char *rp = strrchr(stat, ')');
        if (rp)
            sscanf(rp + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &state, &ut, &st);
    }
    snprintf(path, sizeof path, "/proc/%d/wchan", (int)pid);
    if (read_file(path, wchan, sizeof wchan) <= 0)
        strcpy(wchan, "?");
    logmsg("  %-10s pid %-7d state %c  cpu %.1fs  waiting in %s", label, (int)pid, state,
           (double)(ut + st) / (double)tick, wchan);
}

static void snapshot(void) {
    describe("compositor", comp_pid);
    describe("dwm", pid_by_comm("dwm"));
    describe("Xorg", pid_by_comm("Xorg"));
}

static int alive(pid_t pid) {
    return pid > 0 && kill(pid, 0) == 0;
}

static void kill_compositor(pid_t pid) {
    if (pid <= 0)
        return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 20 && alive(pid); i++)
        sleep_ms(100);
    if (alive(pid)) {
        kill(pid, SIGKILL);
        for (int i = 0; i < 20 && alive(pid); i++)
            sleep_ms(100);
    }
}

static void start_compositor(void) {
    pid_t child = fork();

    if (child == 0) {
        char out[PATH_MAX + 16];
        int fd;

        setsid();
        if ((fd = open("/dev/null", O_RDONLY)) >= 0)
            dup2(fd, 0);
        snprintf(out, sizeof out, "%s.compositor", logpath);
        if ((fd = open(out, O_WRONLY | O_CREAT | O_APPEND, 0600)) >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
        }
        execl("/bin/sh", "sh", "-c", restart_cmd, (char *)NULL);
        _exit(127);
    }
    logmsg("started compositor: %s", restart_cmd);
}

/* ------------------------------------------------------------- monitor thread */

static void *monitor(void *arg) {
    long long stalled_since = 0, killed_at = 0;
    int logged = 0, killed = 0;

    (void)arg;
    for (;;) {
        sleep_ms(500);
        long long t = now_ms(), last = last_answer_ms;
        if (in_request && t - last > stall_log_s * 1000LL) {
            if (!logged) {
                stalled_since = last;
                logmsg("FREEZE: X server not answering for %llds (screen and keyboard stuck, "
                       "pointer may still move)", (t - last) / 1000);
                snapshot();
                logged = 1;
            }
            if (!killed && t - last > stall_kill_s * 1000LL && alive(comp_pid)) {
                if (dry_run) {
                    logmsg("dry run: would kill compositor %d to release a possible server grab",
                           (int)comp_pid);
                } else {
                    logmsg("killing compositor %d (it may hold a server grab)", (int)comp_pid);
                    kill_compositor(comp_pid);
                    need_restart = 1;
                }
                killed = 1;
                killed_at = now_ms();
            }
        } else if (logged && !in_request) {
            long long back = now_ms();
            const char *why = "";
            if (killed && !dry_run)
                why = back - killed_at < 1500
                    ? " (right after the compositor was killed: the compositor caused it)"
                    : " (not right after the compositor was killed: something else caused it)";
            logmsg("X server answers again after %.1fs%s", (back - stalled_since) / 1000.0, why);
            logged = killed = 0;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------- X side */

static Display *dpy;
static Atom cm_atom;

static int ignore_error(Display *d, XErrorEvent *e) {
    (void)d; (void)e;
    return 0;
}

static int io_error(Display *d) {
    (void)d;
    logmsg("lost the X connection, exiting");
    exit(0);
}

/* X requests are wrapped so the monitor thread knows when we are waiting for the server. */
#define XCALL(expr) do { in_request = 1; expr; last_answer_ms = now_ms(); in_request = 0; } while (0)

static pid_t compositor_pid(void) {
    Window owner;
    XResClientIdSpec spec;
    XResClientIdValue *ids = NULL;
    long n = 0;
    pid_t pid = 0;

    XCALL(owner = XGetSelectionOwner(dpy, cm_atom));
    if (owner == None)
        return 0;
    spec.client = owner;
    spec.mask = XRES_CLIENT_ID_PID_MASK;
    in_request = 1;
    if (XResQueryClientIds(dpy, 1, &spec, &n, &ids) == Success) {
        for (long i = 0; i < n && pid <= 0; i++)
            pid = XResGetClientPid(&ids[i]);
        XResClientIdsDestroy(n, ids);
    }
    last_answer_ms = now_ms();
    in_request = 0;
    return pid > 0 ? pid : -1; /* -1: a compositor runs but its PID is unknown */
}

static int screen_off(void) {
    CARD16 level;
    BOOL on;
    int dummy;

    if (!DPMSQueryExtension(dpy, &dummy, &dummy))
        return 0;
    if (!DPMSInfo(dpy, &level, &on))
        return 0;
    return on && level != DPMSModeOn;
}

/* A mapped window stacked above the probe covers its pixel: the X screen saver, an
 * override-redirect locker or menu. Then the pixel says nothing about the compositor. */
static int probe_covered(Window root, Window probe, int x, int y) {
    Window r, p, *kids = NULL;
    unsigned int n;
    int above = 0, covered = 0;

    if (!XQueryTree(dpy, root, &r, &p, &kids, &n))
        return 0;
    for (unsigned int i = 0; i < n && !covered; i++) {
        XWindowAttributes a;

        if (kids[i] == probe) {
            above = 1;
            continue;
        }
        if (above && XGetWindowAttributes(dpy, kids[i], &a) && a.map_state == IsViewable &&
            a.class == InputOutput && x >= a.x && y >= a.y &&
            x < a.x + a.width + 2 * a.border_width && y < a.y + a.height + 2 * a.border_width)
            covered = 1;
    }
    if (kids)
        XFree(kids);
    return covered;
}

/* The X screen saver is active (blanking or a saver window); DPMS may still be "on". */
static int saver_active(Window root) {
    xcb_connection_t *c = XGetXCBConnection(dpy);
    xcb_screensaver_query_info_reply_t *r =
        xcb_screensaver_query_info_reply(c, xcb_screensaver_query_info(c, root), NULL);
    int on = r && r->state == XCB_SCREENSAVER_STATE_ON;

    free(r);
    return on;
}

static void usage(void) {
    fprintf(stderr,
            "usage: thaw [-c CMD] [-L NAMES] [-l LOG] [-f N] [-n] [-v]\n"
            "  -c CMD  command that starts the compositor (default: \"picom --daemon\")\n"
            "  -L NAMES  comma-separated screen locker process names; while one runs a\n"
            "          stopped probe is not blamed on the compositor\n"
            "          (default: xsecurelock,i3lock,slock,xlock,xscreensaver)\n"
            "  -l LOG  log file (default: $XDG_STATE_HOME/thaw/thaw.log)\n"
            "  -f N    missed frames in a row before a restart (default: 5)\n"
            "  -n      dry run: only log, never kill or restart anything\n"
            "  -v      also print the log to stderr\n");
    exit(2);
}

int main(int argc, char **argv) {
    int opt, scr, x, y, fails = 0, phase = 0;
    unsigned long prev_got = ~0UL;
    long long grace_until = 0;
    Window root, probe;
    XSetWindowAttributes wa = { 0 };
    pthread_t th;
    char atomname[32];
    const char *state;

    while ((opt = getopt(argc, argv, "c:L:l:f:nvh")) != -1) {
        switch (opt) {
        case 'c': restart_cmd = optarg; break;
        case 'L': lockers = optarg; break;
        case 'l': snprintf(logpath, sizeof logpath, "%s", optarg); break;
        case 'f': fail_limit = atoi(optarg) > 0 ? atoi(optarg) : 5; break;
        case 'n': dry_run = 1; break;
        case 'v': verbose++; break;
        default: usage();
        }
    }
    if (!*logpath) {
        char dir[PATH_MAX - 16];
        state = getenv("XDG_STATE_HOME");
        if (state && *state)
            snprintf(dir, sizeof dir, "%s/thaw", state);
        else
            snprintf(dir, sizeof dir, "%s/.local/state/thaw", getenv("HOME") ? getenv("HOME") : "/tmp");
        mkdir(dir, 0700);
        snprintf(logpath, sizeof logpath, "%s/thaw.log", dir);
    }
    if (!(logfp = fopen(logpath, "a")))
        logfp = stderr;
    signal(SIGCHLD, SIG_IGN); /* compositor launchers are reaped automatically */

    if (!(dpy = XOpenDisplay(NULL))) {
        logmsg("cannot open display");
        return 1;
    }
    XSetErrorHandler(ignore_error);
    XSetIOErrorHandler(io_error);
    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    snprintf(atomname, sizeof atomname, "_NET_WM_CM_S%d", scr);
    cm_atom = XInternAtom(dpy, atomname, False);

    /* bottom-left corner: usually wallpaper or a window edge, far from the bar */
    x = 0;
    y = DisplayHeight(dpy, scr) - 1;
    wa.override_redirect = True;
    wa.background_pixel = 0x0a0a0a;
    probe = XCreateWindow(dpy, root, x, y, 1, 1, 0, CopyFromParent, InputOutput, CopyFromParent,
                          CWOverrideRedirect | CWBackPixel, &wa);
    XStoreName(dpy, probe, "thaw-probe");
    XMapRaised(dpy, probe);
    XCALL(XSync(dpy, False));

    pthread_create(&th, NULL, monitor, NULL);
    logmsg("thaw started (display %s, compositor command: %s%s)", DisplayString(dpy), restart_cmd,
           dry_run ? ", dry run" : "");

    for (;;) {
        long long tick = now_ms();
        pid_t pid = compositor_pid();

        if (pid != 0)
            comp_pid = pid;
        if (pid == 0) {
            fails = 0;
            if (need_restart && !dry_run) {
                need_restart = 0;
                start_compositor();
                grace_until = now_ms() + 5000;
            }
            sleep_ms(interval_ms);
            continue;
        }
        need_restart = 0;

        int off;
        XCALL(off = screen_off());
        if (off || now_ms() < grace_until) {
            fails = 0;
            sleep_ms(interval_ms);
            continue;
        }

        unsigned long want = (phase ^= 1) ? 0x0b0b0b : 0x0a0a0a; /* never pure black: a frozen screen reads back as black */
        XCALL(XSetWindowBackground(dpy, probe, want); XClearWindow(dpy, probe);
              XRaiseWindow(dpy, probe); XSync(dpy, False));
        sleep_ms(settle_ms);

        XImage *img;
        unsigned long got = ~0UL;
        XCALL(img = XGetImage(dpy, root, x, y, 1, 1, AllPlanes, ZPixmap));
        if (img) {
            got = XGetPixel(img, 0, 0) & 0xffffff;
            XDestroyImage(img);
        }

        if (verbose > 1)
            logmsg("check: compositor %d, want %06lx, got %06lx, misses %d", (int)pid, want, got, fails);
        /* A frozen screen keeps showing the last frame, which matches every other "want".
         * So a check only passes if the pixel follows the new colour AND has changed. */
        int ok = got == want && got != prev_got;
        prev_got = got;
        if (!ok) {
            /* Only on a miss, so the common case costs nothing extra. */
            int covered, saver = 0;
            pid_t locker = 0;

            XCALL(covered = probe_covered(root, probe, x, y));
            if (!covered)
                XCALL(saver = saver_active(root));
            if (!covered && !saver)
                locker = pid_by_comm(lockers);
            if (covered || saver || locker > 0) {
                if (verbose > 1)
                    logmsg("check: probe hidden by %s, not counted",
                           covered ? "a window above it"
                           : saver ? "the X screen saver" : "a running screen locker");
                fails = 0;
                goto next;
            }
        }
        if (ok) {
            if (fails >= 2)
                logmsg("compositor is drawing again after %d missed checks", fails);
            fails = 0;
        } else if (++fails == fail_limit) {
            logmsg("FREEZE: compositor stopped drawing for %ds while X answers "
                   "(screen looks frozen, keys go to windows you cannot see)", fail_limit);
            snapshot();
            if (dry_run) {
                logmsg("dry run: would restart the compositor");
            } else if (pid > 0) {
                kill_compositor(pid);
                start_compositor();
                grace_until = now_ms() + 5000;
            } else {
                logmsg("compositor PID unknown (no XRes), not restarting");
            }
            fails = 0;
        }

    next:;
        long long left = interval_ms - (now_ms() - tick);
        if (left > 0)
            sleep_ms((int)left);
    }
}
