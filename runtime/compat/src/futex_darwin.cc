// andropkg: futex(2) emulation for ART on Darwin, built on the public os_sync API
// (iOS 17.4+ / macOS 14.4+). ART needs futexes for Monitor inflation, Mutex/CondVar and
// Thread park/unpark.
#include <linux/futex.h>

#include <errno.h>
#include <limits.h>
#include <os/os_sync_wait_on_address.h>
#include <stdint.h>
#include <time.h>

namespace {

constexpr os_sync_wait_on_address_flags_t kWaitFlags = OS_SYNC_WAIT_ON_ADDRESS_NONE;
constexpr os_sync_wake_by_address_flags_t kWakeFlags = OS_SYNC_WAKE_BY_ADDRESS_NONE;

uint64_t to_ns(const timespec* ts) {
  return static_cast<uint64_t>(ts->tv_sec) * 1000000000ull + static_cast<uint64_t>(ts->tv_nsec);
}

int wait(volatile int* uaddr, int val, const timespec* rel) {
  // os_sync compares atomically too; the early check just matches Linux's EAGAIN contract.
  if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != val) {
    errno = EAGAIN;
    return -1;
  }
  void* addr = const_cast<int*>(uaddr);
  uint64_t expected = static_cast<uint32_t>(val);
  int r = rel == nullptr
      ? os_sync_wait_on_address(addr, expected, sizeof(int), kWaitFlags)
      : os_sync_wait_on_address_with_timeout(addr, expected, sizeof(int), kWaitFlags,
                                             OS_CLOCK_MACH_ABSOLUTE_TIME, to_ns(rel));
  return r < 0 ? -1 : 0;  // errno (ETIMEDOUT/EINTR/EFAULT) already set by os_sync
}

int wake(volatile int* uaddr, int count) {
  void* addr = const_cast<int*>(uaddr);
  int r = count == 1 ? os_sync_wake_by_address_any(addr, sizeof(int), kWakeFlags)
                     : os_sync_wake_by_address_all(addr, sizeof(int), kWakeFlags);
  if (r < 0 && errno == ENOENT) return 0;  // nobody waiting
  return r < 0 ? -1 : (count == 1 ? 1 : 0);
}

}  // namespace

extern "C" int andropkg_futex(volatile int* uaddr, int op, int val, const timespec* timeout,
                              volatile int* uaddr2, int val3) {
  (void)uaddr2;
  switch (op & FUTEX_CMD_MASK) {
    case FUTEX_WAIT:
      return wait(uaddr, val, timeout);
    case FUTEX_WAIT_BITSET: {
      // Absolute deadline (CLOCK_REALTIME if FUTEX_CLOCK_REALTIME, else CLOCK_MONOTONIC).
      if (timeout == nullptr) return wait(uaddr, val, nullptr);
      timespec now;
      clock_gettime((op & FUTEX_CLOCK_REALTIME) ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
      int64_t rel_ns = static_cast<int64_t>(to_ns(timeout)) - static_cast<int64_t>(to_ns(&now));
      if (rel_ns <= 0) {
        errno = ETIMEDOUT;
        return -1;
      }
      timespec rel = {static_cast<time_t>(rel_ns / 1000000000), static_cast<long>(rel_ns % 1000000000)};
      return wait(uaddr, val, &rel);
    }
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET:
      return wake(uaddr, val);
    case FUTEX_CMP_REQUEUE:
      if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != val3) {
        errno = EAGAIN;
        return -1;
      }
      [[fallthrough]];
    case FUTEX_REQUEUE:
      // Requeueing is an optimisation: waking everyone is correct (they re-contend uaddr2).
      return wake(uaddr, INT_MAX);
    default:
      errno = ENOSYS;
      return -1;
  }
}
