// andropkg: epoll(7) emulation on kqueue.
//
// Each epoll fd is a kqueue fd. Registrations live in a table keyed by (epfd, fd); kevent udata
// points at the registration so one epoll_event can be reported per fd even when both the read
// and the write filter fire. Level-triggered by default; EPOLLET -> EV_CLEAR,
// EPOLLONESHOT -> EV_DISPATCH (re-armed by EPOLL_CTL_MOD).
#include <sys/epoll.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace {

struct Registration {
  int fd;
  uint32_t events;
  epoll_data_t data;
};

std::mutex g_lock;
std::map<std::pair<int, int>, std::unique_ptr<Registration>> g_regs;

int apply(int epfd, Registration* r, bool add_read, bool add_write, bool del_read, bool del_write) {
  struct kevent ch[4];
  int n = 0;
  uint16_t extra = (r->events & EPOLLET) ? EV_CLEAR : 0;
  if (r->events & EPOLLONESHOT) extra |= EV_DISPATCH;
  if (add_read) EV_SET(&ch[n++], r->fd, EVFILT_READ, EV_ADD | EV_ENABLE | extra, 0, 0, r);
  if (add_write) EV_SET(&ch[n++], r->fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | extra, 0, 0, r);
  if (del_read) EV_SET(&ch[n++], r->fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  if (del_write) EV_SET(&ch[n++], r->fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
  if (n == 0) return 0;
  // EV_RECEIPT: report per-change errors without draining pending events.
  for (int i = 0; i < n; i++) ch[i].flags |= EV_RECEIPT;
  struct kevent res[4];
  int got = kevent(epfd, ch, n, res, n, nullptr);
  if (got < 0) return -1;
  for (int i = 0; i < got; i++) {
    if ((res[i].flags & EV_ERROR) && res[i].data != 0 &&
        !(res[i].data == ENOENT && (del_read || del_write))) {
      errno = static_cast<int>(res[i].data);
      return -1;
    }
  }
  return 0;
}

}  // namespace

extern "C" int epoll_create1(int flags) {
  int kq = kqueue();
  if (kq < 0) return -1;
  if (flags & EPOLL_CLOEXEC) fcntl(kq, F_SETFD, FD_CLOEXEC);
  std::lock_guard<std::mutex> l(g_lock);
  // A recycled fd number must not inherit stale registrations.
  for (auto it = g_regs.lower_bound({kq, INT32_MIN}); it != g_regs.end() && it->first.first == kq;) {
    it = g_regs.erase(it);
  }
  return kq;
}

extern "C" int epoll_create(int size) {
  if (size <= 0) {
    errno = EINVAL;
    return -1;
  }
  return epoll_create1(0);
}

extern "C" int epoll_ctl(int epfd, int op, int fd, struct epoll_event* ev) {
  std::lock_guard<std::mutex> l(g_lock);
  auto key = std::make_pair(epfd, fd);
  auto it = g_regs.find(key);
  switch (op) {
    case EPOLL_CTL_ADD: {
      if (it != g_regs.end()) {
        errno = EEXIST;
        return -1;
      }
      auto reg = std::make_unique<Registration>(Registration{fd, ev->events, ev->data});
      if (apply(epfd, reg.get(), ev->events & (EPOLLIN | EPOLLPRI | EPOLLRDHUP),
                ev->events & EPOLLOUT, false, false) != 0) {
        return -1;
      }
      g_regs.emplace(key, std::move(reg));
      return 0;
    }
    case EPOLL_CTL_MOD: {
      if (it == g_regs.end()) {
        errno = ENOENT;
        return -1;
      }
      Registration* r = it->second.get();
      uint32_t old = r->events;
      r->events = ev->events;
      r->data = ev->data;
      bool want_r = ev->events & (EPOLLIN | EPOLLPRI | EPOLLRDHUP), want_w = ev->events & EPOLLOUT;
      bool had_r = old & (EPOLLIN | EPOLLPRI | EPOLLRDHUP), had_w = old & EPOLLOUT;
      // Re-adding also re-arms EV_DISPATCH (oneshot) filters.
      return apply(epfd, r, want_r, want_w, had_r && !want_r, had_w && !want_w);
    }
    case EPOLL_CTL_DEL: {
      if (it == g_regs.end()) {
        errno = ENOENT;
        return -1;
      }
      Registration* r = it->second.get();
      apply(epfd, r, false, false, r->events & (EPOLLIN | EPOLLPRI | EPOLLRDHUP), r->events & EPOLLOUT);
      g_regs.erase(it);
      return 0;
    }
    default:
      errno = EINVAL;
      return -1;
  }
}

extern "C" int epoll_pwait(int epfd, struct epoll_event* events, int maxevents, int timeout,
                           const sigset_t* sigmask) {
  if (maxevents <= 0) {
    errno = EINVAL;
    return -1;
  }
  sigset_t old;
  if (sigmask) pthread_sigmask(SIG_SETMASK, sigmask, &old);
  std::vector<struct kevent> kev(static_cast<size_t>(maxevents) * 2);
  timespec ts, *tsp = nullptr;
  if (timeout >= 0) {
    ts = {timeout / 1000, (timeout % 1000) * 1000000L};
    tsp = &ts;
  }
  int n = kevent(epfd, nullptr, 0, kev.data(), static_cast<int>(kev.size()), tsp);
  int saved = errno;
  if (sigmask) pthread_sigmask(SIG_SETMASK, &old, nullptr);
  if (n < 0) {
    errno = saved;
    return -1;
  }
  std::lock_guard<std::mutex> l(g_lock);
  int out = 0;
  for (int i = 0; i < n; i++) {
    auto* r = static_cast<Registration*>(kev[i].udata);
    if (r == nullptr || g_regs.find({epfd, r->fd}) == g_regs.end()) continue;  // raced with DEL
    uint32_t e = 0;
    if (kev[i].filter == EVFILT_READ) e |= EPOLLIN & r->events;
    if (kev[i].filter == EVFILT_WRITE) e |= EPOLLOUT;
    if (kev[i].flags & EV_EOF) e |= EPOLLHUP | (r->events & EPOLLRDHUP);
    if (kev[i].flags & EV_ERROR) e |= EPOLLERR;
    int j = 0;
    for (; j < out; j++) {
      if (events[j].data.u64 == r->data.u64 && j < out) break;  // merge read+write of one fd
    }
    if (j < out) {
      events[j].events |= e;
    } else if (out < maxevents) {
      events[out].events = e;
      events[out].data = r->data;
      out++;
    }
  }
  return out;
}

extern "C" int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout) {
  return epoll_pwait(epfd, events, maxevents, timeout, nullptr);
}
