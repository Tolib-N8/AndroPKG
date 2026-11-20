// andropkg: Linux eventfd(2) on Darwin, emulated with an anonymous FIFO opened O_RDWR (one fd
// that is readable, writable and kqueue/epoll-pollable). Each 8-byte write is one value; a read
// returns one value (not the summed counter), which is enough for wake-up fds (libutils Looper).
#pragma once
#include <stdint.h>

#define EFD_SEMAPHORE 00000001
#define EFD_CLOEXEC 02000000
#define EFD_NONBLOCK 00004000

typedef uint64_t eventfd_t;

#ifdef __cplusplus
extern "C" {
#endif
int eventfd(unsigned int initval, int flags);
int eventfd_read(int fd, eventfd_t* value);
int eventfd_write(int fd, eventfd_t value);
#ifdef __cplusplus
}
#endif
