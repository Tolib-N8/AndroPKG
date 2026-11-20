// andropkg: Linux-only libc entry points used by AOSP code, implemented on Darwin.
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

int apply_fd_flags(int fd, int flags) {
  if ((flags & O_CLOEXEC) && fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) return -1;
  if (flags & O_NONBLOCK) {
    int fl = fcntl(fd, F_GETFL);
    if (fl == -1 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) == -1) return -1;
  }
  return 0;
}

}  // namespace

extern "C" {

int pipe2(int fds[2], int flags) {
  if (flags & ~(O_CLOEXEC | O_NONBLOCK)) {
    errno = EINVAL;
    return -1;
  }
  if (pipe(fds) == -1) return -1;
  if (apply_fd_flags(fds[0], flags) == -1 || apply_fd_flags(fds[1], flags) == -1) {
    int e = errno;
    close(fds[0]);
    close(fds[1]);
    errno = e;
    return -1;
  }
  return 0;
}

// Linux SOCK_CLOEXEC/SOCK_NONBLOCK values (Darwin has neither).
static constexpr int kSockNonblock = 04000, kSockCloexec = 02000000;

int accept4(int fd, struct sockaddr* addr, socklen_t* len, int flags) {
  int s = accept(fd, addr, len);
  if (s == -1) return -1;
  int f = ((flags & kSockCloexec) ? O_CLOEXEC : 0) | ((flags & kSockNonblock) ? O_NONBLOCK : 0);
  if (apply_fd_flags(s, f) == -1) {
    int e = errno;
    close(s);
    errno = e;
    return -1;
  }
  return s;
}

int dup3(int oldfd, int newfd, int flags) {
  if (oldfd == newfd || (flags & ~O_CLOEXEC)) {
    errno = EINVAL;
    return -1;
  }
  if (dup2(oldfd, newfd) == -1) return -1;
  return apply_fd_flags(newfd, flags) == -1 ? -1 : newfd;
}

int posix_fallocate(int fd, off_t offset, off_t len) {
  if (offset < 0 || len <= 0) return EINVAL;
  fstore_t store = {F_ALLOCATECONTIG, F_PEOFPOSMODE, 0, offset + len, 0};
  if (fcntl(fd, F_PREALLOCATE, &store) == -1) {
    store.fst_flags = F_ALLOCATEALL;
    if (fcntl(fd, F_PREALLOCATE, &store) == -1) return errno;
  }
  struct stat st;
  if (fstat(fd, &st) == -1) return errno;
  if (st.st_size < offset + len && ftruncate(fd, offset + len) == -1) return errno;
  return 0;  // returns an error number, like the real posix_fallocate
}

ssize_t splice(int, off_t*, int, off_t*, size_t, unsigned) {
  errno = ENOSYS;
  return -1;
}

ssize_t andropkg_linux_sendfile(int out_fd, int in_fd, off_t* offset, size_t count) {
  // Linux semantics: copy from in_fd (at *offset, or its file position) to out_fd.
  char buf[64 * 1024];
  size_t done = 0;
  off_t pos = offset ? *offset : 0;
  while (done < count) {
    size_t want = count - done < sizeof buf ? count - done : sizeof buf;
    ssize_t r = offset ? pread(in_fd, buf, want, pos) : read(in_fd, buf, want);
    if (r < 0) {
      if (done > 0) break;
      return -1;
    }
    if (r == 0) break;
    ssize_t w = write(out_fd, buf, static_cast<size_t>(r));
    if (w < 0) {
      if (done > 0) break;
      return -1;
    }
    done += static_cast<size_t>(w);
    pos += w;
    if (w < r) break;
  }
  if (offset) *offset = pos;
  return static_cast<ssize_t>(done);
}

int andropkg_gettid(void) {
  uint64_t tid = 0;
  pthread_threadid_np(nullptr, &tid);
  return static_cast<int>(tid & 0x7fffffff);
}

int prctl(int option, ...) {
  va_list ap;
  va_start(ap, option);
  unsigned long arg2 = va_arg(ap, unsigned long);
  va_end(ap);
  switch (option) {
    case PR_SET_NAME:
      return pthread_setname_np(reinterpret_cast<const char*>(arg2)) == 0 ? 0 : -1;
    case PR_GET_NAME:
      return pthread_getname_np(pthread_self(), reinterpret_cast<char*>(arg2), 16) == 0 ? 0 : -1;
    case PR_SET_DUMPABLE:
    case PR_SET_NO_NEW_PRIVS:
      return 0;
    case PR_GET_DUMPABLE:
      return 1;
    default:
      errno = EINVAL;
      return -1;
  }
}

int capget(cap_user_header_t hdrp, cap_user_data_t datap) {
  if (hdrp == nullptr) {
    errno = EFAULT;
    return -1;
  }
  if (datap != nullptr) memset(datap, 0, sizeof(*datap) * 2);  // no capabilities
  return 0;
}

int capset(cap_user_header_t, const cap_user_data_t) {
  errno = EPERM;
  return -1;
}

int inotify_init(void) {
  errno = ENOSYS;
  return -1;
}
int inotify_init1(int) { return inotify_init(); }
int inotify_add_watch(int, const char*, uint32_t) {
  errno = ENOSYS;
  return -1;
}
int inotify_rm_watch(int, int) {
  errno = ENOSYS;
  return -1;
}

FILE* setmntent(const char*, const char*) {
  errno = ENOENT;
  return nullptr;
}
struct mntent* getmntent(FILE*) { return nullptr; }
struct mntent* getmntent_r(FILE*, struct mntent*, char*, int) { return nullptr; }
int endmntent(FILE* f) {
  if (f) fclose(f);
  return 1;
}

}  // extern "C"
