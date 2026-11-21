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

// See andropkg_darwin_compat.h: Linux MADV_DONTNEED semantics (anonymous pages read back as
// zeros). MADV_ZERO zeroes without faulting pages in. It refuses (EPERM) read-only and PROT_NONE
// ranges; ART releases memory that way only while it is still writable (it protects after), so
// those keep the plain hint.
int andropkg_madvise(void* addr, size_t len, int advice) {
  if (advice == MADV_DONTNEED) {
    (madvise)(addr, len, MADV_ZERO);
  }
  return (madvise)(addr, len, advice);
}
