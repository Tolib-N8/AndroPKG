// andropkg: force-included into every AOSP translation unit built for Darwin.
// Maps glibc/bionic-only spellings onto their Darwin equivalents (Darwin is 64-bit
// off_t everywhere, so the *64 variants are plain aliases).
#pragma once

#if defined(__APPLE__)
#include <sys/types.h>
#include <time.h>

/* Linux clock semantics, which all of AOSP assumes: CLOCK_MONOTONIC stops while the device
 * sleeps (SystemClock.uptimeMillis, System.nanoTime, Looper and Choreographer timing share
 * it). Darwin's CLOCK_MONOTONIC keeps counting (Linux CLOCK_BOOTTIME); CLOCK_UPTIME_RAW is
 * the Linux one. Code that means "boot time" uses _CLOCK_MONOTONIC explicitly. */
#undef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC _CLOCK_UPTIME_RAW

#ifndef ANDROPKG_NO_LARGEFILE_ALIASES
typedef off_t off64_t;
#define lseek64 lseek
#define pread64 pread
#define pwrite64 pwrite
#define ftruncate64 ftruncate
#define open64 open
#define mmap64 mmap
// Struct/function aliases (LP64 Darwin's plain types are already 64-bit).
#define stat64 stat
#define fstat64 fstat
#define lstat64 lstat
#define fstatat64 fstatat
#define statvfs64 statvfs
#define fstatvfs64 fstatvfs
#define dirent64 dirent
#define readdir64 readdir
#define readdir64_r readdir_r
#define flock64 flock
#define F_SETLK64 F_SETLK
#define F_SETLKW64 F_SETLKW
#define F_GETLK64 F_GETLK
#define truncate64 truncate
#define rlimit64 rlimit
#define getrlimit64 getrlimit
#define setrlimit64 setrlimit
#endif

#endif  // __APPLE__

#if defined(__APPLE__)
#include <signal.h>

// bionic's 64-bit signal set API (same as the plain one on LP64 Darwin).
typedef sigset_t sigset64_t;
#define sigemptyset64 sigemptyset
#define sigfillset64 sigfillset
#define sigaddset64 sigaddset
#define sigdelset64 sigdelset
#define sigismember64 sigismember
#define sigprocmask64 sigprocmask
#define pthread_sigmask64 pthread_sigmask
#define sigwait64 sigwait
#define sigaction64 sigaction

#ifndef SEGV_MTEAERR
#define SEGV_MTEAERR 8   // arm64 MTE fault codes: never raised on Apple silicon
#define SEGV_MTESERR 9
#endif

// Linux madvise(MADV_REMOVE) frees backing store; MADV_FREE is the closest Darwin hint.
#include <sys/mman.h>
#ifndef MADV_REMOVE
#define MADV_REMOVE MADV_FREE
#endif
#endif  // __APPLE__

#if defined(__APPLE__)
// mremap(2) does not exist on Darwin. ART only uses it for the JIT zygote boot-image remap,
// which andropkg never runs (no JIT, no zygote); fail cleanly if it is ever reached.
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2
#define MREMAP_DONTUNMAP 4
#ifdef __cplusplus
extern "C"
#endif
void* mremap(void* old_address, size_t old_size, size_t new_size, int flags, ...);
#endif  // __APPLE__

#if defined(__APPLE__)
#ifdef __cplusplus
extern "C"
#endif
int fallocate(int fd, int mode, off_t offset, off_t len);  // ENOSYS (CMC GC only)
#endif

#if defined(__APPLE__)
#include <string.h>  // glibc/bionic headers pull this in transitively; Darwin's don't.
#include <errno.h>
#include <sys/socket.h>
/* Linux-only socket options, answered by libcore's getsockopt wrapper (values unused by
 * Darwin): the socket's address family and protocol. */
#ifndef SO_DOMAIN
#define SO_DOMAIN 0x7ff0
#endif
#ifndef SO_PROTOCOL
#define SO_PROTOCOL 0x7ff1
#endif

#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(exp)            \
  ({                                       \
    __typeof__(exp) _rc;                   \
    do {                                   \
      _rc = (exp);                         \
    } while (_rc == -1 && errno == EINTR); \
    _rc;                                   \
  })
#endif

// struct stat timespec members.
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec

// Linux spellings of IPv6 multicast membership options.
#define IPV6_ADD_MEMBERSHIP IPV6_JOIN_GROUP
#define IPV6_DROP_MEMBERSHIP IPV6_LEAVE_GROUP

// Darwin has no real-time signals. libcore interrupts threads blocked in I/O with a signal;
// use SIGIO (as OpenJDK's BSD port does).
#define ANDROPKG_BLOCKED_IO_SIGNAL SIGIO

#ifdef __cplusplus
extern "C" {
#endif
int pipe2(int fds[2], int flags);
int accept4(int fd, struct sockaddr* addr, socklen_t* len, int flags);
int dup3(int oldfd, int newfd, int flags);
int posix_fallocate(int fd, off_t offset, off_t len);
#define posix_fallocate64 posix_fallocate
ssize_t splice(int fd_in, off_t* off_in, int fd_out, off_t* off_out, size_t len, unsigned flags);
ssize_t andropkg_linux_sendfile(int out_fd, int in_fd, off_t* offset, size_t count);
int andropkg_gettid(void);
#ifdef __cplusplus
}
// Linux sendfile(out, in, off*, count); Darwin's sendfile has a different signature.
inline ssize_t sendfile(int out_fd, int in_fd, off_t* offset, size_t count) {
  return andropkg_linux_sendfile(out_fd, in_fd, offset, count);
}
#endif
#ifndef O_DIRECT
#define O_DIRECT 0
#endif
#ifndef O_NOATIME
#define O_NOATIME 0
#endif
#endif  // __APPLE__

#if defined(__APPLE__)
// ---- Linux constants Darwin lacks -----------------------------------------------------
// libcore's android.system.OsConstants are filled from these at runtime, so Java code sees
// Darwin values. Constants with no Darwin meaning get values outside every Darwin range so
// they can't alias a real constant (e.g. Linux AF_PACKET=17 is Darwin's AF_ROUTE): calls
// using them fail with EAFNOSUPPORT/ENOPROTOOPT/EINVAL as they should.
#define ANDROPKG_NO_SUCH(n) (0x7f000 + (n))
#ifndef AF_PACKET
#define AF_PACKET ANDROPKG_NO_SUCH(1)
#endif
#ifndef AF_NETLINK
#define AF_NETLINK ANDROPKG_NO_SUCH(2)
#endif
#ifndef PF_PACKET
#define PF_PACKET AF_PACKET
#endif
#ifndef PF_NETLINK
#define PF_NETLINK AF_NETLINK
#endif
#ifndef IP_MULTICAST_ALL
#define IP_MULTICAST_ALL ANDROPKG_NO_SUCH(3)
#endif
#ifndef SO_PEERCRED
#define SO_PEERCRED ANDROPKG_NO_SUCH(4)
#endif
#ifndef ENONET
#define ENONET 220  // beyond Darwin's errno range (ELAST ~106)
#endif
// Socket type flags: no-ops (iOS never exec()s; non-blocking is set via fcntl by callers).
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif
#ifndef ARPHRD_LOOPBACK
#define ARPHRD_LOOPBACK 772  // Linux if_arp.h value (hardware type number, not a Darwin enum)
#endif
// statvfs f_flag bits: Darwin defines only ST_RDONLY(1)/ST_NOSUID(2), same as Linux.
#define ST_NODEV 4
#define ST_NOEXEC 8
#define ST_SYNCHRONOUS 16
#define ST_MANDLOCK 64
#define ST_NOATIME 1024
#define ST_NODIRATIME 2048
#define ST_RELATIME 4096
#define SPLICE_F_MOVE 1
#define SPLICE_F_NONBLOCK 2
#define SPLICE_F_MORE 4
#define SPLICE_F_GIFT 8

struct ucred {  // Linux SO_PEERCRED payload (getsockopt fails on Darwin)
  pid_t pid;
  uid_t uid;
  gid_t gid;
};

#define fdatasync(fd) fsync(fd)

#ifdef __cplusplus
#include <sys/mman.h>
// Linux: unsigned char* vector; Darwin: char*.
inline int mincore(void* addr, size_t len, unsigned char* vec) {
  return ::mincore(addr, len, reinterpret_cast<char*>(vec));
}
#endif
#endif  // __APPLE__
