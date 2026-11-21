// andropkg NDK layer — bionic file/system calls on Darwin: flag values, structure layouts,
// *at() conventions, errno; Android paths (/sdcard, /storage/emulated/0, /system, ...) go
// through the runtime's path translation (runtime/compat/src/paths.cc) like libcore's do.
#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/mount.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "ndk_internal.h"

extern const char *andropkg_translate_path(const char *path, char *buf, size_t len) __attribute__((weak));

#define PATH(p) char _pb_##p[1024]; const char *t_##p = (p && andropkg_translate_path) ? andropkg_translate_path(p, _pb_##p, sizeof _pb_##p) : p
#define RET(expr) do { __typeof__(expr) _r = (expr); if (_r == -1) ndk_fix_errno(); return _r; } while (0)

// ---- flags -----------------------------------------------------------------------------------------

enum {  // Linux arm64 open(2) flags
    L_O_ACCMODE = 3, L_O_CREAT = 0100, L_O_EXCL = 0200, L_O_NOCTTY = 0400, L_O_TRUNC = 01000,
    L_O_APPEND = 02000, L_O_NONBLOCK = 04000, L_O_DSYNC = 010000, L_O_ASYNC = 020000,
    L_O_DIRECTORY = 040000, L_O_NOFOLLOW = 0100000, L_O_DIRECT = 0200000, L_O_LARGEFILE = 0400000,
    L_O_NOATIME = 01000000, L_O_CLOEXEC = 02000000, L_O_SYNC = 04010000, L_O_PATH = 010000000,
};

static int open_flags(int f) {
    int d = f & L_O_ACCMODE;
    if (f & L_O_CREAT) d |= O_CREAT;
    if (f & L_O_EXCL) d |= O_EXCL;
    if (f & L_O_NOCTTY) d |= O_NOCTTY;
    if (f & L_O_TRUNC) d |= O_TRUNC;
    if (f & L_O_APPEND) d |= O_APPEND;
    if (f & L_O_NONBLOCK) d |= O_NONBLOCK;
    if ((f & L_O_SYNC) == L_O_SYNC) d |= O_SYNC;
    else if (f & L_O_DSYNC) d |= O_DSYNC;
    if (f & L_O_ASYNC) d |= O_ASYNC;
    if (f & L_O_DIRECTORY) d |= O_DIRECTORY;
    if (f & L_O_NOFOLLOW) d |= O_NOFOLLOW;
    if (f & L_O_CLOEXEC) d |= O_CLOEXEC;
    if (f & L_O_PATH) d = (d & ~O_ACCMODE) | O_RDONLY;  // closest: a read-only descriptor
    return d;
}

static int linux_open_flags(int d) {  // for F_GETFL
    int f = d & O_ACCMODE;
    if (d & O_APPEND) f |= L_O_APPEND;
    if (d & O_NONBLOCK) f |= L_O_NONBLOCK;
    if (d & O_SYNC) f |= L_O_SYNC;
    if (d & O_ASYNC) f |= L_O_ASYNC;
    return f | L_O_LARGEFILE;
}

static int at_fd(int fd) { return fd == -100 ? AT_FDCWD : fd; }
static int at_flags(int f) {
    int d = 0;
    if (f & 0x100) d |= AT_SYMLINK_NOFOLLOW;
    if (f & 0x200) d |= AT_REMOVEDIR;  // also AT_EACCESS for faccessat (handled there)
    if (f & 0x400) d |= AT_SYMLINK_FOLLOW;
    return d;
}

// ---- open / close / read / write ---------------------------------------------------------------

NDK_EXPORT int bionic_open(const char *path, int flags, int mode) {
    char vbuf[16384];
    const char *v = path ? ndk_virtual_file(path, vbuf, sizeof vbuf) : NULL;
    if (v) return ndk_virtual_fd(v);
    PATH(path);
    RET(open(t_path, open_flags(flags), mode));
}
NDK_EXPORT int bionic_open64(const char *path, int flags, int mode) { return bionic_open(path, flags, mode); }
NDK_EXPORT int bionic___open_2(const char *path, int flags) { return bionic_open(path, flags, 0); }
NDK_EXPORT int bionic_openat(int dirfd, const char *path, int flags, int mode) {
    if (path && path[0] == '/') return bionic_open(path, flags, mode);
    PATH(path);
    RET(openat(at_fd(dirfd), t_path, open_flags(flags), mode));
}
NDK_EXPORT int bionic_openat64(int dirfd, const char *path, int flags, int mode) { return bionic_openat(dirfd, path, flags, mode); }
NDK_EXPORT int bionic___openat_2(int dirfd, const char *path, int flags) { return bionic_openat(dirfd, path, flags, 0); }
NDK_EXPORT int bionic_creat(const char *path, int mode) { return bionic_open(path, L_O_CREAT | 1 /* O_WRONLY */ | L_O_TRUNC, mode); }
NDK_EXPORT int bionic_close(int fd) {
    ndk_eventfd_close(fd);
    ndk_epoll_close(fd);
    RET(close(ndk_proc_fd(fd)));
}
NDK_EXPORT ssize_t bionic_read(int fd, void *b, size_t n) {
    ssize_t r;
    if (ndk_eventfd_read(fd, b, n, &r)) return r;
    RET(read(ndk_proc_fd(fd), b, n));
}
NDK_EXPORT ssize_t bionic___read_chk(int fd, void *b, size_t n, size_t blen) { (void)blen; return bionic_read(fd, b, n); }
NDK_EXPORT ssize_t bionic_write(int fd, const void *b, size_t n) {
    ssize_t r;
    if (ndk_eventfd_write(fd, b, n, &r)) return r;
    RET(write(ndk_proc_fd(fd), b, n));
}
NDK_EXPORT ssize_t bionic_pread(int fd, void *b, size_t n, off_t o) { RET(pread(fd, b, n, o)); }
NDK_EXPORT ssize_t bionic_pread64(int fd, void *b, size_t n, off_t o) { RET(pread(fd, b, n, o)); }
NDK_EXPORT ssize_t bionic___pread_chk(int fd, void *b, size_t n, off_t o, size_t blen) { (void)blen; return bionic_pread(fd, b, n, o); }
NDK_EXPORT ssize_t bionic___pread64_chk(int fd, void *b, size_t n, off_t o, size_t blen) { (void)blen; return bionic_pread(fd, b, n, o); }
NDK_EXPORT ssize_t bionic_pwrite(int fd, const void *b, size_t n, off_t o) { RET(pwrite(fd, b, n, o)); }
NDK_EXPORT ssize_t bionic_pwrite64(int fd, const void *b, size_t n, off_t o) { RET(pwrite(fd, b, n, o)); }
NDK_EXPORT ssize_t bionic_readv(int fd, const struct iovec *v, int n) { RET(readv(ndk_proc_fd(fd), v, n)); }
NDK_EXPORT ssize_t bionic_writev(int fd, const struct iovec *v, int n) { RET(writev(ndk_proc_fd(fd), v, n)); }
NDK_EXPORT off_t bionic_lseek(int fd, off_t o, int w) { RET(lseek(fd, o, w)); }
NDK_EXPORT off_t bionic_lseek64(int fd, off_t o, int w) { RET(lseek(fd, o, w)); }
NDK_EXPORT int bionic_dup(int fd) { RET(dup(ndk_proc_fd(fd))); }
NDK_EXPORT int bionic_dup2(int a, int b) { RET(dup2(a, b)); }
NDK_EXPORT int bionic_dup3(int a, int b, int flags) {
    int r = dup2(a, b);
    if (r >= 0 && (flags & L_O_CLOEXEC)) fcntl(b, F_SETFD, FD_CLOEXEC);
    RET(r);
}
NDK_EXPORT int bionic_pipe(int fds[2]) { RET(pipe(fds)); }
NDK_EXPORT int bionic_pipe2(int fds[2], int flags) {
    int r = pipe(fds);
    if (r == 0) {
        for (int i = 0; i < 2; i++) {
            if (flags & L_O_CLOEXEC) fcntl(fds[i], F_SETFD, FD_CLOEXEC);
            if (flags & L_O_NONBLOCK) fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
        }
    }
    RET(r);
}
NDK_EXPORT int bionic_fsync(int fd) { RET(fsync(fd)); }
NDK_EXPORT int bionic_fdatasync(int fd) { RET(fsync(fd)); }
NDK_EXPORT int bionic_ftruncate(int fd, off_t n) { RET(ftruncate(fd, n)); }
NDK_EXPORT int bionic_ftruncate64(int fd, off_t n) { RET(ftruncate(fd, n)); }
NDK_EXPORT int bionic_truncate(const char *p, off_t n) { PATH(p); RET(truncate(t_p, n)); }
NDK_EXPORT int bionic_isatty(int fd) { int r = isatty(ndk_proc_fd(fd)); if (!r) ndk_fix_errno(); return r; }
NDK_EXPORT int bionic_flock(int fd, int op) { RET(flock(fd, op)); }  // LOCK_* values match

// fcntl(fd, cmd, ...): the optional argument arrives in x2 either way.
struct l_flock { short l_type, l_whence; off_t l_start, l_len; int l_pid; };
NDK_EXPORT int bionic_fcntl(int fd, int cmd, uintptr_t arg) {
    switch (cmd) {
        case 0: RET(fcntl(fd, F_DUPFD, (int)arg));
        case 1030: RET(fcntl(fd, F_DUPFD_CLOEXEC, (int)arg));
        case 1: RET(fcntl(fd, F_GETFD));
        case 2: RET(fcntl(fd, F_SETFD, (int)arg));
        case 3: {
            int r = fcntl(fd, F_GETFL), ef;
            if (r < 0) { ndk_fix_errno(); return r; }
            if (ndk_eventfd_nonblock(fd, -1, &ef)) return (linux_open_flags(r) & ~0x800) | ef;
            return linux_open_flags(r);
        }
        case 4:
            if (ndk_eventfd_nonblock(fd, ((int)arg & 0x800) != 0, NULL)) return 0;
            RET(fcntl(fd, F_SETFL, open_flags((int)arg) & (O_APPEND | O_NONBLOCK | O_ASYNC)));
        case 5: case 6: case 7: {  // F_GETLK, F_SETLK, F_SETLKW
            struct l_flock *lf = (struct l_flock *)arg;
            struct flock df = {.l_start = lf->l_start, .l_len = lf->l_len, .l_pid = lf->l_pid,
                               .l_type = lf->l_type == 0 ? F_RDLCK : lf->l_type == 1 ? F_WRLCK : F_UNLCK,
                               .l_whence = lf->l_whence};
            int r = fcntl(fd, cmd == 5 ? F_GETLK : cmd == 6 ? F_SETLK : F_SETLKW, &df);
            if (r < 0) { ndk_fix_errno(); return r; }
            if (cmd == 5) {
                lf->l_type = df.l_type == F_RDLCK ? 0 : df.l_type == F_WRLCK ? 1 : 2;
                lf->l_whence = df.l_whence;
                lf->l_start = df.l_start;
                lf->l_len = df.l_len;
                lf->l_pid = df.l_pid;
            }
            return r;
        }
        case 8: RET(fcntl(fd, F_SETOWN, (int)arg));
        case 9: RET(fcntl(fd, F_GETOWN));
        case 1031: return 0;    // F_SETPIPE_SZ: accept
        case 1032: return 65536;  // F_GETPIPE_SZ
        default:
            errno = 22;
            return -1;
    }
}

// ioctl(fd, request, ...)
NDK_EXPORT int bionic_ioctl(int fd, unsigned long req, uintptr_t arg) {
    switch (req) {
        case 0x541B: RET(ioctl(fd, FIONREAD, (int *)arg));   // FIONREAD
        case 0x5421: RET(ioctl(fd, FIONBIO, (int *)arg));    // FIONBIO
        case 0x5451: RET(fcntl(fd, F_SETFD, FD_CLOEXEC));    // FIOCLEX
        case 0x5450: RET(fcntl(fd, F_SETFD, 0));             // FIONCLEX
        default:
            errno = 25;  // ENOTTY
            return -1;
    }
}

// ---- stat ---------------------------------------------------------------------------------------

struct l_stat {  // bionic arm64 (asm-generic) struct stat
    uint64_t st_dev, st_ino;
    uint32_t st_mode, st_nlink, st_uid, st_gid;
    uint64_t st_rdev, pad1;
    int64_t st_size;
    int32_t st_blksize, pad2;
    int64_t st_blocks;
    struct timespec st_atim, st_mtim, st_ctim;
    uint32_t unused4, unused5;
};
_Static_assert(sizeof(struct l_stat) == 128, "bionic struct stat");

static void to_lstat(const struct stat *s, struct l_stat *o) {
    memset(o, 0, sizeof *o);
    o->st_dev = (uint64_t)s->st_dev;
    o->st_ino = s->st_ino;
    o->st_mode = s->st_mode;  // S_IF* and permission bits match
    o->st_nlink = s->st_nlink;
    o->st_uid = s->st_uid;
    o->st_gid = s->st_gid;
    o->st_rdev = (uint64_t)s->st_rdev;
    o->st_size = s->st_size;
    o->st_blksize = s->st_blksize;
    o->st_blocks = s->st_blocks;
    o->st_atim = s->st_atimespec;
    o->st_mtim = s->st_mtimespec;
    o->st_ctim = s->st_ctimespec;
}

#define STAT_WRAP(call, out) do { struct stat _s; int _r = (call); if (_r) { ndk_fix_errno(); return _r; } to_lstat(&_s, (out)); return 0; } while (0)
NDK_EXPORT int bionic_stat(const char *p, struct l_stat *o) { PATH(p); STAT_WRAP(stat(t_p, &_s), o); }
NDK_EXPORT int bionic_lstat(const char *p, struct l_stat *o) { PATH(p); STAT_WRAP(lstat(t_p, &_s), o); }
NDK_EXPORT int bionic_fstat(int fd, struct l_stat *o) { STAT_WRAP(fstat(ndk_proc_fd(fd), &_s), o); }
NDK_EXPORT int bionic_fstatat(int dirfd, const char *p, struct l_stat *o, int flags) {
    PATH(p);
    STAT_WRAP(fstatat(at_fd(dirfd), t_p, &_s, at_flags(flags) & AT_SYMLINK_NOFOLLOW), o);
}
NDK_EXPORT int bionic_stat64(const char *p, struct l_stat *o) { return bionic_stat(p, o); }
NDK_EXPORT int bionic_lstat64(const char *p, struct l_stat *o) { return bionic_lstat(p, o); }
NDK_EXPORT int bionic_fstat64(int fd, struct l_stat *o) { return bionic_fstat(fd, o); }
NDK_EXPORT int bionic_fstatat64(int d, const char *p, struct l_stat *o, int f) { return bionic_fstatat(d, p, o, f); }

struct l_statfs {  // bionic arm64 struct statfs
    uint64_t f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t f_fsid[2];
    uint64_t f_namelen, f_frsize, f_flags, f_spare[4];
};
static void to_lstatfs(const struct statfs *s, struct l_statfs *o) {
    memset(o, 0, sizeof *o);
    o->f_type = 0xEF53;  // EXT4_SUPER_MAGIC: what Android apps expect for /data
    o->f_bsize = s->f_bsize;
    o->f_blocks = s->f_blocks;
    o->f_bfree = s->f_bfree;
    o->f_bavail = s->f_bavail;
    o->f_files = s->f_files;
    o->f_ffree = s->f_ffree;
    o->f_namelen = 255;
    o->f_frsize = s->f_bsize;
}
NDK_EXPORT int bionic_statfs(const char *p, struct l_statfs *o) {
    PATH(p);
    struct statfs s;
    int r = statfs(t_p, &s);
    if (r) { ndk_fix_errno(); return r; }
    to_lstatfs(&s, o);
    return 0;
}
NDK_EXPORT int bionic_fstatfs(int fd, struct l_statfs *o) {
    struct statfs s;
    int r = fstatfs(fd, &s);
    if (r) { ndk_fix_errno(); return r; }
    to_lstatfs(&s, o);
    return 0;
}
NDK_EXPORT int bionic_statfs64(const char *p, struct l_statfs *o) { return bionic_statfs(p, o); }
NDK_EXPORT int bionic_fstatfs64(int fd, struct l_statfs *o) { return bionic_fstatfs(fd, o); }

struct l_statvfs {  // bionic LP64 struct statvfs
    unsigned long f_bsize, f_frsize;
    uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail;
    unsigned long f_fsid, f_flag, f_namemax;
    uint32_t spare[6];
};
static void to_lstatvfs(const struct statvfs *s, struct l_statvfs *o) {
    memset(o, 0, sizeof *o);
    o->f_bsize = s->f_bsize;
    o->f_frsize = s->f_frsize;
    o->f_blocks = s->f_blocks;
    o->f_bfree = s->f_bfree;
    o->f_bavail = s->f_bavail;
    o->f_files = s->f_files;
    o->f_ffree = s->f_ffree;
    o->f_favail = s->f_favail;
    o->f_fsid = s->f_fsid;
    o->f_namemax = 255;
}
NDK_EXPORT int bionic_statvfs(const char *p, struct l_statvfs *o) {
    PATH(p);
    struct statvfs s;
    int r = statvfs(t_p, &s);
    if (r) { ndk_fix_errno(); return r; }
    to_lstatvfs(&s, o);
    return 0;
}
NDK_EXPORT int bionic_fstatvfs(int fd, struct l_statvfs *o) {
    struct statvfs s;
    int r = fstatvfs(fd, &s);
    if (r) { ndk_fix_errno(); return r; }
    to_lstatvfs(&s, o);
    return 0;
}
NDK_EXPORT int bionic_statvfs64(const char *p, struct l_statvfs *o) { return bionic_statvfs(p, o); }
NDK_EXPORT int bionic_fstatvfs64(int fd, struct l_statvfs *o) { return bionic_fstatvfs(fd, o); }

// ---- path operations ------------------------------------------------------------------------------

NDK_EXPORT int bionic_access(const char *p, int mode) { PATH(p); RET(access(t_p, mode)); }
NDK_EXPORT int bionic_faccessat(int d, const char *p, int mode, int flags) {
    PATH(p);
    RET(faccessat(at_fd(d), t_p, mode, (flags & 0x200) ? AT_EACCESS : 0));
}
NDK_EXPORT int bionic_unlink(const char *p) { PATH(p); RET(unlink(t_p)); }
NDK_EXPORT int bionic_unlinkat(int d, const char *p, int flags) { PATH(p); RET(unlinkat(at_fd(d), t_p, at_flags(flags) & AT_REMOVEDIR)); }
NDK_EXPORT int bionic_rmdir(const char *p) { PATH(p); RET(rmdir(t_p)); }
NDK_EXPORT int bionic_mkdir(const char *p, mode_t m) { PATH(p); RET(mkdir(t_p, m)); }
NDK_EXPORT int bionic_mkdirat(int d, const char *p, mode_t m) { PATH(p); RET(mkdirat(at_fd(d), t_p, m)); }
NDK_EXPORT int bionic_renameat(int a, const char *p, int b, const char *q) { PATH(p); PATH(q); RET(renameat(at_fd(a), t_p, at_fd(b), t_q)); }
NDK_EXPORT int bionic_chdir(const char *p) { PATH(p); RET(chdir(t_p)); }
NDK_EXPORT int bionic_fchdir(int fd) { RET(fchdir(fd)); }
NDK_EXPORT char *bionic_getcwd(char *b, size_t n) { char *r = getcwd(b, n); if (!r) ndk_fix_errno(); return r; }
NDK_EXPORT char *bionic___getcwd_chk(char *b, size_t n, size_t blen) { (void)blen; return bionic_getcwd(b, n); }
NDK_EXPORT char *bionic_realpath(const char *p, char *out) { PATH(p); char *r = realpath(t_p, out); if (!r) ndk_fix_errno(); return r; }
NDK_EXPORT ssize_t bionic_readlink(const char *p, char *b, size_t n) {
    if (strcmp(p, "/proc/self/exe") == 0) {  // used to find "the executable"
        const char *exe = "/system/bin/app_process64";
        size_t l = strlen(exe) < n ? strlen(exe) : n;
        memcpy(b, exe, l);
        return (ssize_t)l;
    }
    PATH(p);
    RET(readlink(t_p, b, n));
}
NDK_EXPORT ssize_t bionic___readlink_chk(const char *p, char *b, size_t n, size_t blen) { (void)blen; return bionic_readlink(p, b, n); }
NDK_EXPORT ssize_t bionic_readlinkat(int d, const char *p, char *b, size_t n) { PATH(p); RET(readlinkat(at_fd(d), t_p, b, n)); }
NDK_EXPORT int bionic_symlink(const char *a, const char *b) { PATH(b); RET(symlink(a, t_b)); }
NDK_EXPORT int bionic_link(const char *a, const char *b) { PATH(a); PATH(b); RET(link(t_a, t_b)); }
NDK_EXPORT int bionic_chmod(const char *p, mode_t m) { PATH(p); RET(chmod(t_p, m)); }
NDK_EXPORT int bionic_fchmod(int fd, mode_t m) { RET(fchmod(fd, m)); }
NDK_EXPORT int bionic_fchmodat(int d, const char *p, mode_t m, int f) { PATH(p); RET(fchmodat(at_fd(d), t_p, m, at_flags(f))); }
NDK_EXPORT int bionic_chown(const char *p, uid_t u, gid_t g) { (void)p; (void)u; (void)g; return 0; }
NDK_EXPORT int bionic_fchown(int fd, uid_t u, gid_t g) { (void)fd; (void)u; (void)g; return 0; }
NDK_EXPORT int bionic_lchown(const char *p, uid_t u, gid_t g) { (void)p; (void)u; (void)g; return 0; }
NDK_EXPORT int bionic_utimes(const char *p, const struct timeval t[2]) { PATH(p); RET(utimes(t_p, t)); }
NDK_EXPORT int bionic_utimensat(int d, const char *p, const struct timespec t[2], int f) {
    PATH(p);
    RET(utimensat(at_fd(d), t_p, t, at_flags(f) & AT_SYMLINK_NOFOLLOW));
}
NDK_EXPORT int bionic_futimens(int fd, const struct timespec t[2]) { RET(futimens(fd, t)); }
NDK_EXPORT int bionic_mkfifo(const char *p, mode_t m) { PATH(p); RET(mkfifo(t_p, m)); }

// ---- directories (bionic struct dirent: d_ino, d_off, d_reclen, d_type, d_name[256]) -----------

struct l_dirent { uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256]; };
typedef struct { DIR *dir; struct l_dirent ent; } b_dir;

NDK_EXPORT b_dir *bionic_opendir(const char *p) {
    PATH(p);
    DIR *d = opendir(t_p);
    if (!d) { ndk_fix_errno(); return NULL; }
    b_dir *b = calloc(1, sizeof *b);
    b->dir = d;
    return b;
}
NDK_EXPORT b_dir *bionic_fdopendir(int fd) {
    DIR *d = fdopendir(fd);
    if (!d) { ndk_fix_errno(); return NULL; }
    b_dir *b = calloc(1, sizeof *b);
    b->dir = d;
    return b;
}
NDK_EXPORT struct l_dirent *bionic_readdir(b_dir *b) {
    struct dirent *e = readdir(b->dir);
    if (!e) return NULL;
    b->ent.d_ino = e->d_ino;
    b->ent.d_off = (int64_t)e->d_seekoff;
    b->ent.d_type = e->d_type;  // DT_* values match
    strlcpy(b->ent.d_name, e->d_name, sizeof b->ent.d_name);
    b->ent.d_reclen = (uint16_t)(offsetof(struct l_dirent, d_name) + strlen(b->ent.d_name) + 1);
    return &b->ent;
}
NDK_EXPORT struct l_dirent *bionic_readdir64(b_dir *b) { return bionic_readdir(b); }
NDK_EXPORT int bionic_readdir_r(b_dir *b, struct l_dirent *out, struct l_dirent **res) {
    struct l_dirent *e = bionic_readdir(b);
    if (e) { *out = *e; *res = out; } else *res = NULL;
    return 0;
}
NDK_EXPORT int bionic_closedir(b_dir *b) { int r = closedir(b->dir); free(b); return r; }
NDK_EXPORT void bionic_rewinddir(b_dir *b) { rewinddir(b->dir); }
NDK_EXPORT int bionic_dirfd(b_dir *b) { return dirfd(b->dir); }
NDK_EXPORT long bionic_telldir(b_dir *b) { return telldir(b->dir); }
NDK_EXPORT void bionic_seekdir(b_dir *b, long l) { seekdir(b->dir, l); }
NDK_EXPORT int bionic_alphasort(const struct l_dirent **a, const struct l_dirent **b) { return strcoll((*a)->d_name, (*b)->d_name); }
NDK_EXPORT int bionic_scandir(const char *p, struct l_dirent ***out, int (*filter)(const struct l_dirent *),
                              int (*cmp)(const struct l_dirent **, const struct l_dirent **)) {
    b_dir *d = bionic_opendir(p);
    if (!d) return -1;
    struct l_dirent **list = NULL;
    int n = 0, cap = 0;
    for (struct l_dirent *e; (e = bionic_readdir(d));) {
        if (filter && !filter(e)) continue;
        if (n == cap) list = realloc(list, sizeof *list * (size_t)(cap = cap ? cap * 2 : 16));
        list[n] = malloc(sizeof *e);
        *list[n++] = *e;
    }
    bionic_closedir(d);
    if (cmp) qsort(list, (size_t)n, sizeof *list, (int (*)(const void *, const void *))cmp);
    *out = list;
    return n;
}

// ---- memory mapping ------------------------------------------------------------------------------

NDK_EXPORT void *bionic_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    int d = flags & 3;  // MAP_SHARED / MAP_PRIVATE
    if (flags & 0x10) d |= MAP_FIXED;
    if (flags & 0x20) d |= MAP_ANON;
    if (flags & 0x4000) d |= MAP_NORESERVE;
    void *r = mmap(addr, len, prot, d, (flags & 0x20) ? -1 : fd, off);
    if (r == MAP_FAILED) ndk_fix_errno();
    return r;
}
NDK_EXPORT void *bionic_mmap64(void *a, size_t l, int p, int f, int fd, off_t o) { return bionic_mmap(a, l, p, f, fd, o); }
NDK_EXPORT int bionic_munmap(void *a, size_t l) { RET(munmap(a, l)); }
NDK_EXPORT int bionic_mprotect(void *a, size_t l, int p) { RET(mprotect(a, l, p)); }
NDK_EXPORT int bionic_madvise(void *a, size_t l, int adv) {
    // MADV_NORMAL..MADV_DONTNEED (0..4) match; Linux MADV_FREE (8) -> Darwin MADV_FREE; others: hints.
    int d = adv <= 4 ? adv : adv == 8 ? MADV_FREE : -1;
    if (d < 0) return 0;
    RET(madvise(a, l, d));
}
NDK_EXPORT int bionic_msync(void *a, size_t l, int f) {
    int d = (f & 1 ? MS_ASYNC : 0) | (f & 2 ? MS_INVALIDATE : 0) | (f & 4 ? MS_SYNC : 0);
    RET(msync(a, l, d));
}
NDK_EXPORT int bionic_mlock(const void *a, size_t l) { (void)a; (void)l; return 0; }
NDK_EXPORT int bionic_munlock(const void *a, size_t l) { (void)a; (void)l; return 0; }
NDK_EXPORT int bionic_mlockall(int f) { (void)f; return 0; }
NDK_EXPORT int bionic_munlockall(void) { return 0; }
NDK_EXPORT int bionic_mincore(void *a, size_t l, unsigned char *v) { RET(mincore(a, l, (char *)v)); }

// ---- polling -----------------------------------------------------------------------------------------

NDK_EXPORT int bionic_poll(struct pollfd *f, nfds_t n, int timeout) { RET(poll(f, n, timeout)); }
NDK_EXPORT int bionic___poll_chk(struct pollfd *f, nfds_t n, int timeout, size_t flen) { (void)flen; return bionic_poll(f, n, timeout); }
NDK_EXPORT int bionic_ppoll(struct pollfd *f, nfds_t n, const struct timespec *ts, const void *sigmask) {
    (void)sigmask;
    int ms = ts ? (int)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000) : -1;
    return bionic_poll(f, n, ms);
}
NDK_EXPORT int bionic_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t) { RET(select(n, r, w, e, t)); }
NDK_EXPORT int bionic_pselect(int n, fd_set *r, fd_set *w, fd_set *e, const struct timespec *ts, const void *mask) {
    (void)mask;
    RET(pselect(n, r, w, e, ts, NULL));
}

// ---- time ------------------------------------------------------------------------------------------

static clockid_t darwin_clock(int c) {
    switch (c) {
        case 0: case 5: case 8: return CLOCK_REALTIME;     // REALTIME, _COARSE, _ALARM
        case 4: return CLOCK_MONOTONIC_RAW;
        case 2: return CLOCK_PROCESS_CPUTIME_ID;
        case 3: return CLOCK_THREAD_CPUTIME_ID;
        default:
            // MONOTONIC (1), MONOTONIC_COARSE (6), BOOTTIME (7), BOOTTIME_ALARM (9): the clock ART
            // uses for System.nanoTime(), so native and Java timestamps agree.
            return c < 0 ? CLOCK_THREAD_CPUTIME_ID : CLOCK_MONOTONIC;
    }
}
NDK_EXPORT int bionic_clock_gettime(int c, struct timespec *ts) { RET(clock_gettime(darwin_clock(c), ts)); }
NDK_EXPORT int bionic_clock_getres(int c, struct timespec *ts) { RET(clock_getres(darwin_clock(c), ts)); }
NDK_EXPORT int bionic_clock_nanosleep(int c, int flags, const struct timespec *req, struct timespec *rem) {
    struct timespec rel = *req;
    if (flags & 1) {  // TIMER_ABSTIME
        struct timespec now;
        clock_gettime(darwin_clock(c), &now);
        rel.tv_sec = req->tv_sec - now.tv_sec;
        rel.tv_nsec = req->tv_nsec - now.tv_nsec;
        if (rel.tv_nsec < 0) { rel.tv_nsec += 1000000000; rel.tv_sec--; }
        if (rel.tv_sec < 0) return 0;
    }
    return nanosleep(&rel, rem) ? ndk_errno_to_linux(errno) : 0;
}
NDK_EXPORT int bionic_nanosleep(const struct timespec *req, struct timespec *rem) { RET(nanosleep(req, rem)); }
NDK_EXPORT int bionic_usleep(useconds_t us) { RET(usleep(us)); }
NDK_EXPORT unsigned bionic_sleep(unsigned s) { return sleep(s); }
NDK_EXPORT int bionic_gettimeofday(struct timeval *tv, void *tz) {
    if (tv) {
        memset(tv, 0, sizeof *tv);  // Darwin's tv_usec is 32-bit: clear bionic's upper half
        gettimeofday(tv, NULL);
    }
    if (tz) memset(tz, 0, 8);
    return 0;
}
NDK_EXPORT int bionic_settimeofday(const struct timeval *tv, const void *tz) { (void)tv; (void)tz; errno = 1; return -1; }

// ---- identity, limits, uname -----------------------------------------------------------------

struct l_utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };
NDK_EXPORT int bionic_uname(struct l_utsname *u) {
    memset(u, 0, sizeof *u);
    strlcpy(u->sysname, "Linux", 65);
    strlcpy(u->nodename, "localhost", 65);
    strlcpy(u->release, "6.1.0-andropkg", 65);
    strlcpy(u->version, "#1 SMP PREEMPT", 65);
    strlcpy(u->machine, "aarch64", 65);
    strlcpy(u->domainname, "(none)", 65);
    return 0;
}

static int darwin_rlimit(int r) {
    switch (r) {
        case 0: return RLIMIT_CPU;
        case 1: return RLIMIT_FSIZE;
        case 2: return RLIMIT_DATA;
        case 3: return RLIMIT_STACK;
        case 4: return RLIMIT_CORE;
        case 5: return RLIMIT_RSS;
        case 6: return RLIMIT_NPROC;
        case 7: return RLIMIT_NOFILE;
        case 8: return RLIMIT_MEMLOCK;
        case 9: return RLIMIT_AS;
        default: return -1;
    }
}
NDK_EXPORT int bionic_getrlimit(int r, struct rlimit *l) {
    int d = darwin_rlimit(r);
    if (d < 0) { l->rlim_cur = l->rlim_max = RLIM_INFINITY; return 0; }
    RET(getrlimit(d, l));
}
NDK_EXPORT int bionic_setrlimit(int r, const struct rlimit *l) {
    int d = darwin_rlimit(r);
    if (d < 0) return 0;
    int rr = setrlimit(d, l);
    return rr ? 0 : 0;  // best effort
}
NDK_EXPORT int bionic_getrlimit64(int r, struct rlimit *l) { return bionic_getrlimit(r, l); }
NDK_EXPORT int bionic_setrlimit64(int r, const struct rlimit *l) { return bionic_setrlimit(r, l); }
NDK_EXPORT int bionic_prlimit(int pid, int r, const struct rlimit *n, struct rlimit *o) {
    (void)pid;
    if (o) bionic_getrlimit(r, o);
    if (n) bionic_setrlimit(r, n);
    return 0;
}
NDK_EXPORT int bionic_getpriority(int which, int who) { (void)which; (void)who; return 0; }
NDK_EXPORT int bionic_setpriority(int which, int who, int prio) { (void)which; (void)who; (void)prio; return 0; }
NDK_EXPORT int bionic_getpid(void) { return getpid(); }
NDK_EXPORT int bionic_getppid(void) { return getppid(); }
NDK_EXPORT uid_t bionic_getuid(void) { return getuid(); }
NDK_EXPORT uid_t bionic_geteuid(void) { return geteuid(); }
NDK_EXPORT gid_t bionic_getgid(void) { return getgid(); }
NDK_EXPORT gid_t bionic_getegid(void) { return getegid(); }
NDK_EXPORT int bionic_gethostname(char *b, size_t n) { strlcpy(b, "localhost", n); return 0; }
NDK_EXPORT long bionic_fpathconf(int fd, int name) { (void)fd; return name == 3 /* _PC_NAME_MAX */ ? 255 : name == 4 /* _PC_PATH_MAX */ ? 4096 : -1; }
NDK_EXPORT long bionic_pathconf(const char *p, int name) { (void)p; return bionic_fpathconf(-1, name); }

// prctl(option, ...): names only.
NDK_EXPORT int bionic_prctl(int option, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5) {
    (void)a3; (void)a4; (void)a5;
    switch (option) {
        case 15: {  // PR_SET_NAME
            char buf[16];
            strlcpy(buf, (const char *)a2, sizeof buf);
            pthread_setname_np(buf);
            return 0;
        }
        case 16:  // PR_GET_NAME
            pthread_getname_np(pthread_self(), (char *)a2, 16);
            return 0;
        default:
            return 0;
    }
}
