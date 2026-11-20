// andropkg: Linux epoll(7) on Darwin, implemented on kqueue (compat/src/epoll_kqueue.cc).
#pragma once
#include <signal.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum EPOLL_EVENTS {
  EPOLLIN = 0x001, EPOLLPRI = 0x002, EPOLLOUT = 0x004, EPOLLRDNORM = 0x040, EPOLLRDBAND = 0x080,
  EPOLLWRNORM = 0x100, EPOLLWRBAND = 0x200, EPOLLMSG = 0x400, EPOLLERR = 0x008, EPOLLHUP = 0x010,
  EPOLLRDHUP = 0x2000, EPOLLEXCLUSIVE = 1u << 28, EPOLLWAKEUP = 1u << 29,
  EPOLLONESHOT = 1u << 30, EPOLLET = 1u << 31
};
#define EPOLLIN EPOLLIN
#define EPOLLOUT EPOLLOUT
#define EPOLLERR EPOLLERR
#define EPOLLHUP EPOLLHUP
#define EPOLLRDHUP EPOLLRDHUP
#define EPOLLONESHOT EPOLLONESHOT
#define EPOLLET EPOLLET
#define EPOLLPRI EPOLLPRI

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLL_CLOEXEC 02000000

typedef union epoll_data {
  void* ptr;
  int fd;
  uint32_t u32;
  uint64_t u64;
} epoll_data_t;

struct epoll_event {  // arm64 layout (not packed, unlike x86_64)
  uint32_t events;
  epoll_data_t data;
};

int epoll_create(int size);
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event);
int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout);
int epoll_pwait(int epfd, struct epoll_event* events, int maxevents, int timeout,
                const sigset_t* sigmask);

#ifdef __cplusplus
}
#endif
