#include <sys/eventfd.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int eventfd(unsigned int initval, int flags) {
  const char* tmp = getenv("TMPDIR");
  char path[1024];
  snprintf(path, sizeof path, "%s/.andropkg-efd-XXXXXX", tmp && *tmp ? tmp : "/tmp");
  int tries = 0;
  for (;;) {
    char p[1024];
    memcpy(p, path, sizeof p);
    if (mktemp(p) == nullptr) return -1;
    if (mkfifo(p, 0600) == 0) {
      int fd = open(p, O_RDWR | O_NONBLOCK);  // O_RDWR: never blocks waiting for a peer
      int saved = errno;
      unlink(p);
      if (fd < 0) {
        errno = saved;
        return -1;
      }
      if (!(flags & EFD_NONBLOCK)) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
      if (flags & EFD_CLOEXEC) fcntl(fd, F_SETFD, FD_CLOEXEC);
      if (initval != 0) {
        uint64_t v = initval;
        if (write(fd, &v, sizeof v) != sizeof v) {
          close(fd);
          return -1;
        }
      }
      return fd;
    }
    if (errno != EEXIST || ++tries > 16) return -1;
  }
}

extern "C" int eventfd_read(int fd, eventfd_t* value) {
  return read(fd, value, sizeof *value) == sizeof *value ? 0 : -1;
}

extern "C" int eventfd_write(int fd, eventfd_t value) {
  return write(fd, &value, sizeof value) == sizeof value ? 0 : -1;
}
