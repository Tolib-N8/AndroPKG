// andropkg: Linux-only memory APIs.
#include <errno.h>
#include <stddef.h>
#include <sys/mman.h>

void* mremap(void* old_address, size_t old_size, size_t new_size, int flags, ...) {
  (void)old_address; (void)old_size; (void)new_size; (void)flags;
  errno = ENOSYS;
  return MAP_FAILED;
}

int fallocate(int fd, int mode, off_t offset, off_t len) {
  (void)fd; (void)mode; (void)offset; (void)len;
  errno = ENOSYS;
  return -1;
}
