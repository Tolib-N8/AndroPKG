// andropkg: inotify does not exist on Darwin; calls fail with ENOSYS (WatchService unsupported).
#pragma once
#include <stdint.h>
#define IN_ACCESS 0x1
#define IN_MODIFY 0x2
#define IN_ATTRIB 0x4
#define IN_CLOSE_WRITE 0x8
#define IN_CLOSE_NOWRITE 0x10
#define IN_OPEN 0x20
#define IN_MOVED_FROM 0x40
#define IN_MOVED_TO 0x80
#define IN_CREATE 0x100
#define IN_DELETE 0x200
#define IN_DELETE_SELF 0x400
#define IN_MOVE_SELF 0x800
#define IN_UNMOUNT 0x2000
#define IN_Q_OVERFLOW 0x4000
#define IN_IGNORED 0x8000
#define IN_ONLYDIR 0x01000000
#define IN_DONT_FOLLOW 0x02000000
#define IN_MASK_ADD 0x20000000
#define IN_ISDIR 0x40000000
#define IN_ONESHOT 0x80000000
#define IN_NONBLOCK 04000
#define IN_CLOEXEC 02000000
struct inotify_event {
  int wd;
  uint32_t mask, cookie, len;
  char name[];
};
#ifdef __cplusplus
extern "C" {
#endif
int inotify_init(void);
int inotify_init1(int flags);
int inotify_add_watch(int fd, const char* path, uint32_t mask);
int inotify_rm_watch(int fd, int wd);
#ifdef __cplusplus
}
#endif
