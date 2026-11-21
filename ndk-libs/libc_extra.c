// andropkg NDK layer — the rest of what real NDK libraries import: extended attributes,
// passwd/group, syslog, wide printf, signal sets, sendfile, inotify (on kqueue), mntent, and a
// few virtual /proc and /sys files native code commonly reads.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <wchar.h>

#include "ndk_internal.h"

extern const char *andropkg_translate_path(const char *path, char *buf, size_t len) __attribute__((weak));
static const char *tpath(const char *p, char *buf, size_t n) {
    return (p && andropkg_translate_path) ? andropkg_translate_path(p, buf, n) : p;
}

// ---- extended attributes (Linux API; SELinux labels as Android has them) ------------------------

static const char kSelinuxLabel[] = "u:object_r:app_data_file:s0";

static ssize_t xattr_get(const char *path, int fd, const char *name, void *value, size_t size, int options) {
    if (strcmp(name, "security.selinux") == 0) {
        size_t n = sizeof kSelinuxLabel;  // including the NUL, as the kernel returns it
        if (size == 0) return (ssize_t)n;
        if (size < n) { errno = 34; return -1; }  // ERANGE
        memcpy(value, kSelinuxLabel, n);
        return (ssize_t)n;
    }
    char b[1024];
    ssize_t r = path ? getxattr(tpath(path, b, sizeof b), name, value, size, 0, options) : fgetxattr(fd, name, value, size, 0, options);
    if (r < 0) ndk_fix_errno();
    return r;
}
NDK_EXPORT ssize_t bionic_getxattr(const char *p, const char *n, void *v, size_t s) { return xattr_get(p, -1, n, v, s, 0); }
NDK_EXPORT ssize_t bionic_lgetxattr(const char *p, const char *n, void *v, size_t s) { return xattr_get(p, -1, n, v, s, XATTR_NOFOLLOW); }
NDK_EXPORT ssize_t bionic_fgetxattr(int fd, const char *n, void *v, size_t s) { return xattr_get(NULL, fd, n, v, s, 0); }

static int xattr_flags(int f) { return (f & 1 ? XATTR_CREATE : 0) | (f & 2 ? XATTR_REPLACE : 0); }
static int xattr_set(const char *path, int fd, const char *name, const void *value, size_t size, int flags, int options) {
    if (strncmp(name, "security.", 9) == 0) { errno = 95; return -1; }  // EOPNOTSUPP
    char b[1024];
    int r = path ? setxattr(tpath(path, b, sizeof b), name, value, size, 0, options | xattr_flags(flags))
                 : fsetxattr(fd, name, value, size, 0, options | xattr_flags(flags));
    if (r < 0) ndk_fix_errno();
    return r;
}
NDK_EXPORT int bionic_setxattr(const char *p, const char *n, const void *v, size_t s, int f) { return xattr_set(p, -1, n, v, s, f, 0); }
NDK_EXPORT int bionic_lsetxattr(const char *p, const char *n, const void *v, size_t s, int f) { return xattr_set(p, -1, n, v, s, f, XATTR_NOFOLLOW); }
NDK_EXPORT int bionic_fsetxattr(int fd, const char *n, const void *v, size_t s, int f) { return xattr_set(NULL, fd, n, v, s, f, 0); }

static ssize_t xattr_list(const char *path, int fd, char *list, size_t size, int options) {
    char b[1024];
    ssize_t r = path ? listxattr(tpath(path, b, sizeof b), list, size, options) : flistxattr(fd, list, size, options);
    if (r < 0) ndk_fix_errno();
    return r;
}
NDK_EXPORT ssize_t bionic_listxattr(const char *p, char *l, size_t s) { return xattr_list(p, -1, l, s, 0); }
NDK_EXPORT ssize_t bionic_llistxattr(const char *p, char *l, size_t s) { return xattr_list(p, -1, l, s, XATTR_NOFOLLOW); }
NDK_EXPORT ssize_t bionic_flistxattr(int fd, char *l, size_t s) { return xattr_list(NULL, fd, l, s, 0); }
NDK_EXPORT int bionic_removexattr(const char *p, const char *n) { char b[1024]; int r = removexattr(tpath(p, b, sizeof b), n, 0); if (r) ndk_fix_errno(); return r; }
NDK_EXPORT int bionic_lremovexattr(const char *p, const char *n) { char b[1024]; int r = removexattr(tpath(p, b, sizeof b), n, XATTR_NOFOLLOW); if (r) ndk_fix_errno(); return r; }
NDK_EXPORT int bionic_fremovexattr(int fd, const char *n) { int r = fremovexattr(fd, n, 0); if (r) ndk_fix_errno(); return r; }

// ---- passwd / group: Android's synthetic users (u0_a<N> for app uids) ----------------------------

struct b_passwd { char *pw_name, *pw_passwd; uint32_t pw_uid, pw_gid; char *pw_gecos, *pw_dir, *pw_shell; };
struct b_group { char *gr_name, *gr_passwd; uint32_t gr_gid; char **gr_mem; };

static void user_name(uint32_t id, char *out, size_t n) {
    if (id == 0) strlcpy(out, "root", n);
    else if (id == 1000) strlcpy(out, "system", n);
    else if (id >= 10000 && id < 20000) snprintf(out, n, "u0_a%u", id - 10000);
    else snprintf(out, n, "u0_a%u", id % 100000);
}
static __thread struct b_passwd t_pw;
static __thread char t_pwname[32];
static __thread struct b_group t_gr;
static __thread char t_grname[32];
static __thread char *t_grmem[2];

static int fill_pw(uint32_t uid, struct b_passwd *pw, char *buf, size_t len) {
    char name[32];
    user_name(uid, name, sizeof name);
    size_t need = strlen(name) + 1 + 2 + strlen("/data") + 1 + strlen("/system/bin/sh") + 1;
    if (len < need) return 34;
    char *p = buf;
    pw->pw_name = p; p = stpcpy(p, name) + 1;
    pw->pw_passwd = p; p = stpcpy(p, "") + 1;
    pw->pw_gecos = pw->pw_passwd;
    pw->pw_dir = p; p = stpcpy(p, "/data") + 1;
    pw->pw_shell = p; stpcpy(p, "/system/bin/sh");
    pw->pw_uid = pw->pw_gid = uid;
    return 0;
}
NDK_EXPORT int bionic_getpwuid_r(uint32_t uid, struct b_passwd *pw, char *buf, size_t len, struct b_passwd **res) {
    int r = fill_pw(uid, pw, buf, len);
    *res = r ? NULL : pw;
    return r;
}
NDK_EXPORT struct b_passwd *bionic_getpwuid(uint32_t uid) {
    static __thread char buf[128];
    return fill_pw(uid, &t_pw, buf, sizeof buf) ? NULL : &t_pw;
}
static uint32_t id_from_name(const char *n) {
    if (!strcmp(n, "root")) return 0;
    if (!strcmp(n, "system")) return 1000;
    if (!strncmp(n, "u0_a", 4)) return 10000 + (uint32_t)atoi(n + 4);
    return (uint32_t)-1;
}
NDK_EXPORT struct b_passwd *bionic_getpwnam(const char *n) {
    uint32_t id = id_from_name(n);
    return id == (uint32_t)-1 ? NULL : bionic_getpwuid(id);
}
NDK_EXPORT int bionic_getpwnam_r(const char *n, struct b_passwd *pw, char *buf, size_t len, struct b_passwd **res) {
    uint32_t id = id_from_name(n);
    if (id == (uint32_t)-1) { *res = NULL; return 0; }
    return bionic_getpwuid_r(id, pw, buf, len, res);
}
static struct b_group *fill_gr(uint32_t gid) {
    user_name(gid, t_grname, sizeof t_grname);
    t_gr.gr_name = t_grname;
    t_gr.gr_passwd = "";
    t_gr.gr_gid = gid;
    t_grmem[0] = t_grname;
    t_grmem[1] = NULL;
    t_gr.gr_mem = t_grmem;
    return &t_gr;
}
NDK_EXPORT struct b_group *bionic_getgrgid(uint32_t gid) { return fill_gr(gid); }
NDK_EXPORT struct b_group *bionic_getgrnam(const char *n) {
    uint32_t id = id_from_name(n);
    return id == (uint32_t)-1 ? NULL : fill_gr(id);
}
NDK_EXPORT int bionic_getgrgid_r(uint32_t gid, struct b_group *g, char *buf, size_t len, struct b_group **res) {
    struct b_group *s = fill_gr(gid);
    if (len < strlen(s->gr_name) + 1 + 2 * sizeof(char *)) { *res = NULL; return 34; }
    char **mem = (char **)buf;
    char *name = buf + 2 * sizeof(char *);
    strcpy(name, s->gr_name);
    mem[0] = name;
    mem[1] = NULL;
    *g = (struct b_group){name, "", gid, mem};
    *res = g;
    return 0;
}
NDK_EXPORT int bionic_getgrnam_r(const char *n, struct b_group *g, char *buf, size_t len, struct b_group **res) {
    uint32_t id = id_from_name(n);
    if (id == (uint32_t)-1) { *res = NULL; return 0; }
    return bionic_getgrgid_r(id, g, buf, len, res);
}
NDK_EXPORT char *bionic_getlogin(void) { return bionic_getpwuid(getuid())->pw_name; }
NDK_EXPORT int bionic_getgroups(int n, uint32_t *list) { if (n > 0) list[0] = getgid(); return 1; }

// ---- syslog -> logcat --------------------------------------------------------------------------

static char g_ident[64] = "syslog";
NDK_EXPORT void bionic_openlog(const char *ident, int opt, int fac) { (void)opt; (void)fac; if (ident) strlcpy(g_ident, ident, sizeof g_ident); }
NDK_EXPORT void bionic_closelog(void) {}
NDK_EXPORT int bionic_setlogmask(int mask) { return mask; }

#define VA_PARAMS                                                                                   \
    uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5, uint64_t x6,       \
    uint64_t x7, double d0, double d1, double d2, double d3, double d4, double d5, double d6,        \
    double d7, uint64_t s0, uint64_t s1, uint64_t s2, uint64_t s3, uint64_t s4, uint64_t s5,         \
    uint64_t s6, uint64_t s7, uint64_t s8, uint64_t s9, uint64_t s10, uint64_t s11, uint64_t s12,    \
    uint64_t s13, uint64_t s14, uint64_t s15
#define VA_START(v, named)                                                                          \
    uint64_t _gr[8] = {x0, x1, x2, x3, x4, x5, x6, x7};                                             \
    double _fr[8] = {d0, d1, d2, d3, d4, d5, d6, d7};                                               \
    uint64_t _st[16] = {s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13, s14, s15};      \
    ndk_va v = {.gr = _gr + (named), .gn = 8 - (named), .fr = _fr, .fn = 8, .stack = _st}

extern int __android_log_write(int prio, const char *tag, const char *msg);
static void syslog_v(int pri, const char *fmt, ndk_va *v) {
    uint64_t slots[128];
    char f[2048], msg[2048];
    ndk_printf_args(fmt, v, slots, 128, f, sizeof f);
    vsnprintf(msg, sizeof msg, f, (va_list)(void *)slots);
    int lvl = pri & 7;  // LOG_EMERG..LOG_DEBUG -> android priorities
    int prio = lvl <= 3 ? 6 : lvl == 4 ? 5 : lvl <= 6 ? 4 : 3;
    __android_log_write(prio, g_ident, msg);
}
NDK_EXPORT void bionic_syslog(VA_PARAMS) { VA_START(v, 2); syslog_v((int)x0, (const char *)x1, &v); }
NDK_EXPORT void bionic_vsyslog(int pri, const char *fmt, void *ap) {
    uint64_t gr[8]; double fr[8]; ndk_va v;
    ndk_va_from_list(&v, (ndk_aapcs_va_list *)ap, gr, fr, 8);
    syslog_v(pri, fmt, &v);
}

// ---- wide printf: types from an ASCII copy of the format, formatting by Darwin ----------------

static int wide_v(wchar_t *out, size_t n, const wchar_t *wfmt, ndk_va *v) {
    char narrow[2048];
    size_t i = 0;
    for (; wfmt[i] && i + 1 < sizeof narrow; i++) narrow[i] = wfmt[i] < 0x80 ? (char)wfmt[i] : 'x';
    narrow[i] = 0;
    uint64_t slots[128];
    char f[2048];
    ndk_printf_args(narrow, v, slots, 128, f, sizeof f);
    return vswprintf(out, n, wfmt, (va_list)(void *)slots);
}
NDK_EXPORT int bionic_swprintf(VA_PARAMS) { VA_START(v, 3); return wide_v((wchar_t *)x0, (size_t)x1, (const wchar_t *)x2, &v); }
NDK_EXPORT int bionic_vswprintf(wchar_t *o, size_t n, const wchar_t *f, void *ap) {
    uint64_t gr[8]; double fr[8]; ndk_va v;
    ndk_va_from_list(&v, (ndk_aapcs_va_list *)ap, gr, fr, 8);
    return wide_v(o, n, f, &v);
}
NDK_EXPORT wint_t bionic_getwc(FILE *f) { extern wint_t bionic_fgetwc(FILE *); return bionic_fgetwc(f); }
NDK_EXPORT wint_t bionic_ungetwc(wint_t c, FILE *f) { return ungetwc(c, f); }

// ---- signal sets (bionic sigset_t: 64-bit mask, Linux numbering) ---------------------------------

static const int kLinuxToDarwinSig[65] = {
    [1] = SIGHUP, [2] = SIGINT, [3] = SIGQUIT, [4] = SIGILL, [5] = SIGTRAP, [6] = SIGABRT, [7] = SIGBUS,
    [8] = SIGFPE, [9] = SIGKILL, [10] = SIGUSR1, [11] = SIGSEGV, [12] = SIGUSR2, [13] = SIGPIPE,
    [14] = SIGALRM, [15] = SIGTERM, [17] = SIGCHLD, [18] = SIGCONT, [19] = SIGSTOP, [20] = SIGTSTP,
    [21] = SIGTTIN, [22] = SIGTTOU, [23] = SIGURG, [24] = SIGXCPU, [25] = SIGXFSZ, [26] = SIGVTALRM,
    [27] = SIGPROF, [28] = SIGWINCH, [29] = SIGIO, [31] = SIGSYS,
};
int ndk_signal_to_darwin(int s) { return s > 0 && s < 65 ? kLinuxToDarwinSig[s] : 0; }

NDK_EXPORT int bionic_sigemptyset(uint64_t *set) { *set = 0; return 0; }
NDK_EXPORT int bionic_sigfillset(uint64_t *set) { *set = ~0ULL; return 0; }
NDK_EXPORT int bionic_sigaddset(uint64_t *set, int s) { if (s < 1 || s > 64) { errno = 22; return -1; } *set |= 1ULL << (s - 1); return 0; }
NDK_EXPORT int bionic_sigdelset(uint64_t *set, int s) { if (s < 1 || s > 64) { errno = 22; return -1; } *set &= ~(1ULL << (s - 1)); return 0; }
NDK_EXPORT int bionic_sigismember(const uint64_t *set, int s) { if (s < 1 || s > 64) { errno = 22; return -1; } return (int)((*set >> (s - 1)) & 1); }

static sigset_t to_darwin_set(const uint64_t *set) {
    sigset_t d;
    sigemptyset(&d);
    for (int s = 1; s <= 64; s++) {
        // Never let native code block the runtime's own signals (ART's signal catcher, faults).
        if (s == 3 || s == 10 || s == 11 || s == 7 || s == 4 || s == 5) continue;
        int ds = ndk_signal_to_darwin(s);
        if (ds && (*set >> (s - 1) & 1)) sigaddset(&d, ds);
    }
    return d;
}
static void from_darwin_set(const sigset_t *d, uint64_t *set) {
    *set = 0;
    for (int s = 1; s <= 64; s++) {
        int ds = ndk_signal_to_darwin(s);
        if (ds && sigismember(d, ds)) *set |= 1ULL << (s - 1);
    }
}
static int darwin_how(int how) { return how == 0 ? SIG_BLOCK : how == 1 ? SIG_UNBLOCK : SIG_SETMASK; }
NDK_EXPORT int bionic_pthread_sigmask(int how, const uint64_t *set, uint64_t *old) {
    sigset_t d, o;
    if (set) d = to_darwin_set(set);
    int r = pthread_sigmask(darwin_how(how), set ? &d : NULL, &o);
    if (!r && old) from_darwin_set(&o, old);
    return ndk_errno_to_linux(r);
}
NDK_EXPORT int bionic_sigprocmask(int how, const uint64_t *set, uint64_t *old) {
    int r = bionic_pthread_sigmask(how, set, old);
    if (r) { errno = r; return -1; }
    return 0;
}
NDK_EXPORT int bionic_sigpending(uint64_t *set) { sigset_t d; sigpending(&d); from_darwin_set(&d, set); return 0; }
NDK_EXPORT int bionic_raise(int s) { int ds = ndk_signal_to_darwin(s); return ds ? raise(ds) : 0; }
NDK_EXPORT int bionic_kill(int pid, int s) {
    if (pid != getpid() && pid != 0) { errno = 1; return -1; }  // EPERM: there are no other processes
    if (s == 0) return 0;
    return bionic_raise(s);
}
NDK_EXPORT int bionic_pthread_kill(pthread_t t, int s) {
    int ds = ndk_signal_to_darwin(s);
    return s == 0 ? 0 : ds ? ndk_errno_to_linux(pthread_kill(t, ds)) : 22;
}

// ---- sendfile (Linux: file -> any fd) ----------------------------------------------------------

NDK_EXPORT ssize_t bionic_sendfile(int out, int in, off_t *off, size_t count) {
    char buf[65536];
    ssize_t total = 0;
    off_t pos = off ? *off : 0;
    while ((size_t)total < count) {
        size_t want = count - (size_t)total < sizeof buf ? count - (size_t)total : sizeof buf;
        ssize_t r = off ? pread(in, buf, want, pos) : read(in, buf, want);
        if (r < 0) { ndk_fix_errno(); return total ? total : -1; }
        if (r == 0) break;
        ssize_t w = write(out, buf, (size_t)r);
        if (w < 0) { ndk_fix_errno(); return total ? total : -1; }
        total += w;
        pos += w;
        if (w < r) break;
    }
    if (off) *off = pos;
    return total;
}
NDK_EXPORT ssize_t bionic_sendfile64(int out, int in, off_t *off, size_t count) { return bionic_sendfile(out, in, off, count); }

// ---- inotify on kqueue: events are written to a pipe as struct inotify_event ------------------

typedef struct { int wd; int fd; uint32_t mask; } watch;
typedef struct { int kq; int rd, wr; watch w[256]; int nw; pthread_mutex_t m; } inotify_inst;
static inotify_inst *g_ino[64];

static void *inotify_thread(void *p) {
    inotify_inst *in = p;
    struct kevent ev[16];
    for (;;) {
        int n = kevent(in->kq, NULL, 0, ev, 16, NULL);
        if (n < 0) { if (errno == EINTR) continue; return NULL; }
        for (int i = 0; i < n; i++) {
            int wd = (int)(intptr_t)ev[i].udata;
            uint32_t f = ev[i].fflags, mask = 0;
            if (f & NOTE_WRITE) mask |= 0x100 | 0x2;  // IN_CREATE | IN_MODIFY (kqueue can't tell which)
            if (f & NOTE_EXTEND) mask |= 0x2;
            if (f & NOTE_ATTRIB) mask |= 0x4;          // IN_ATTRIB
            if (f & NOTE_DELETE) mask |= 0x400;        // IN_DELETE_SELF
            if (f & NOTE_RENAME) mask |= 0x800;        // IN_MOVE_SELF
            struct { int wd; uint32_t mask, cookie, len; } e = {wd, mask, 0, 0};
            write(in->wr, &e, sizeof e);
        }
    }
}

NDK_EXPORT int bionic_inotify_init1(int flags) {
    inotify_inst *in = calloc(1, sizeof *in);
    int p[2];
    if (pipe(p) < 0) { ndk_fix_errno(); free(in); return -1; }
    in->rd = p[0];
    in->wr = p[1];
    in->kq = kqueue();
    pthread_mutex_init(&in->m, NULL);
    if (flags & 04000) fcntl(in->rd, F_SETFL, O_NONBLOCK);  // IN_NONBLOCK
    if (flags & 02000000) fcntl(in->rd, F_SETFD, FD_CLOEXEC);
    if (in->rd < 64) g_ino[in->rd] = in;
    pthread_t t;
    pthread_create(&t, NULL, inotify_thread, in);
    pthread_detach(t);
    return in->rd;
}
NDK_EXPORT int bionic_inotify_init(void) { return bionic_inotify_init1(0); }
NDK_EXPORT int bionic_inotify_add_watch(int ifd, const char *path, uint32_t mask) {
    inotify_inst *in = ifd >= 0 && ifd < 64 ? g_ino[ifd] : NULL;
    if (!in) { errno = 22; return -1; }
    char b[1024];
    int fd = open(tpath(path, b, sizeof b), O_EVTONLY | O_CLOEXEC);
    if (fd < 0) { ndk_fix_errno(); return -1; }
    pthread_mutex_lock(&in->m);
    int wd = in->nw < 256 ? ++in->nw : -1;
    if (wd > 0) {
        in->w[wd - 1] = (watch){wd, fd, mask};
        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME,
               0, (void *)(intptr_t)wd);
        kevent(in->kq, &ev, 1, NULL, 0, NULL);
    }
    pthread_mutex_unlock(&in->m);
    if (wd < 0) { close(fd); errno = 28; }  // ENOSPC
    return wd;
}
NDK_EXPORT int bionic_inotify_rm_watch(int ifd, int wd) {
    inotify_inst *in = ifd >= 0 && ifd < 64 ? g_ino[ifd] : NULL;
    if (!in || wd < 1 || wd > in->nw || in->w[wd - 1].fd < 0) { errno = 22; return -1; }
    close(in->w[wd - 1].fd);  // closing removes the kevent
    in->w[wd - 1].fd = -1;
    return 0;
}

// ---- mntent ------------------------------------------------------------------------------------

static const char kMounts[] =
    "/dev/block/dm-0 / ext4 ro,seclabel,relatime 0 0\n"
    "tmpfs /dev tmpfs rw,seclabel,nosuid,relatime,mode=755 0 0\n"
    "proc /proc proc rw,relatime,gid=3009,hidepid=invisible 0 0\n"
    "sysfs /sys sysfs rw,seclabel,relatime 0 0\n"
    "/dev/block/dm-1 /system ext4 ro,seclabel,relatime 0 0\n"
    "/dev/block/dm-5 /data f2fs rw,lazytime,seclabel,nosuid,nodev,noatime 0 0\n"
    "/dev/fuse /storage/emulated fuse rw,lazytime,nosuid,nodev,noexec,noatime,user_id=0,group_id=0,allow_other 0 0\n";

struct b_mntent { char *mnt_fsname, *mnt_dir, *mnt_type, *mnt_opts; int mnt_freq, mnt_passno; };
NDK_EXPORT FILE *bionic_setmntent(const char *path, const char *mode) {
    (void)path; (void)mode;
    return fmemopen((void *)kMounts, sizeof kMounts - 1, "r");
}
NDK_EXPORT struct b_mntent *bionic_getmntent_r(FILE *f, struct b_mntent *e, char *buf, int len) {
    if (!fgets(buf, len, f)) return NULL;
    char *save = NULL;
    e->mnt_fsname = strtok_r(buf, " \n", &save);
    e->mnt_dir = strtok_r(NULL, " \n", &save);
    e->mnt_type = strtok_r(NULL, " \n", &save);
    e->mnt_opts = strtok_r(NULL, " \n", &save);
    char *freq = strtok_r(NULL, " \n", &save), *pass = strtok_r(NULL, " \n", &save);
    e->mnt_freq = freq ? atoi(freq) : 0;
    e->mnt_passno = pass ? atoi(pass) : 0;
    return e;
}
NDK_EXPORT struct b_mntent *bionic_getmntent(FILE *f) {
    static __thread struct b_mntent e;
    static __thread char buf[512];
    return bionic_getmntent_r(f, &e, buf, sizeof buf);
}
NDK_EXPORT int bionic_endmntent(FILE *f) { if (f) fclose(f); return 1; }
NDK_EXPORT char *bionic_hasmntopt(const struct b_mntent *e, const char *opt) { return strstr(e->mnt_opts, opt); }

// ---- process-level calls that have no meaning in one iOS process ----------------------------------

NDK_EXPORT int bionic_vfork(void) { errno = 38; return -1; }
NDK_EXPORT int bionic_fork(void) { errno = 38; return -1; }
NDK_EXPORT int bionic_waitpid(int pid, int *status, int options) { (void)pid; (void)status; (void)options; errno = 10; return -1; }  // ECHILD
NDK_EXPORT int bionic_execvp(const char *f, char *const argv[]) { (void)f; (void)argv; errno = 13; return -1; }
NDK_EXPORT int bionic_execv(const char *f, char *const argv[]) { (void)f; (void)argv; errno = 13; return -1; }
NDK_EXPORT int bionic_execve(const char *f, char *const argv[], char *const envp[]) { (void)f; (void)argv; (void)envp; errno = 13; return -1; }
NDK_EXPORT void bionic__exit(int status) { _exit(status); }
NDK_EXPORT int bionic_mount(const char *s, const char *t, const char *fs, unsigned long f, const void *d) {
    (void)s; (void)t; (void)fs; (void)f; (void)d;
    errno = 1;
    return -1;
}
NDK_EXPORT int bionic_mkstemp64(char *t) { int r = mkstemp(t); if (r < 0) ndk_fix_errno(); return r; }

// ---- virtual /proc and /sys files ------------------------------------------------------------------

/// Content for a virtual kernel file, or NULL (then the path is opened for real).
const char *ndk_virtual_file(const char *path, char *buf, size_t n) {
    if (!strcmp(path, "/proc/cpuinfo")) {
        int cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
        size_t o = 0;
        for (int i = 0; i < cpus && o < n; i++)
            o += (size_t)snprintf(buf + o, n - o,
                                  "processor\t: %d\nBogoMIPS\t: 48.00\nFeatures\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp asimdrdm jscvt fcma lrcpc dcpop sha3 asimddp sha512 asimdfhm\n"
                                  "CPU implementer\t: 0x61\nCPU architecture: 8\nCPU variant\t: 0x0\nCPU part\t: 0x022\nCPU revision\t: 0\n\n", i);
        return buf;
    }
    if (!strcmp(path, "/proc/meminfo")) {
        uint64_t mem = 0;
        size_t sz = sizeof mem;
        sysctlbyname("hw.memsize", &mem, &sz, NULL, 0);
        snprintf(buf, n, "MemTotal:       %llu kB\nMemFree:        %llu kB\nMemAvailable:   %llu kB\nBuffers:               0 kB\nCached:                0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n",
                 (unsigned long long)(mem / 1024), (unsigned long long)(mem / 4096), (unsigned long long)(mem / 2048));
        return buf;
    }
    if (!strcmp(path, "/proc/mounts") || !strcmp(path, "/proc/self/mounts")) return kMounts;
    if (!strcmp(path, "/proc/version")) return "Linux version 6.1.0-andropkg (android-build@andropkg) #1 SMP PREEMPT\n";
    if (!strcmp(path, "/sys/devices/system/cpu/possible") || !strcmp(path, "/sys/devices/system/cpu/present") ||
        !strcmp(path, "/sys/devices/system/cpu/online")) {
        snprintf(buf, n, "0-%ld\n", sysconf(_SC_NPROCESSORS_ONLN) - 1);
        return buf;
    }
    if (!strcmp(path, "/proc/self/cmdline")) return "app_process64";
    return NULL;
}

/// An fd reading `content` (an unlinked temporary file).
int ndk_virtual_fd(const char *content) {
    char tmpl[1024];
    snprintf(tmpl, sizeof tmpl, "%s/andropkg-vfile-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    unlink(tmpl);
    write(fd, content, strlen(content));
    lseek(fd, 0, SEEK_SET);
    return fd;
}
