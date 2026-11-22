// andropkg NDK layer — sockets, name resolution, epoll and eventfd for Android code.
//
// Linux and Darwin share the BSD socket API but not its binary details: sockaddr has a length
// byte on Darwin and AF_INET6 is 10 on Linux, 30 on Darwin; socket options, levels, MSG_* and
// SOCK_* flags have different values; msghdr's sizes differ. Everything crossing the boundary
// is translated here. epoll runs on kqueue (an epoll fd is a kqueue fd plus a registry of the
// interest list); an eventfd is a pipe whose read end is the fd (readable iff the counter is
// non-zero) plus the counter.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ndk_internal.h"

#define RET(expr) do { __typeof__(expr) _r = (expr); if (_r == -1) ndk_fix_errno(); return _r; } while (0)
static int fail(int linux_errno) { errno = ndk_errno_to_darwin(linux_errno); ndk_fix_errno(); return -1; }

// ---- address families and sockaddrs ----------------------------------------------------------
enum { L_AF_UNSPEC = 0, L_AF_UNIX = 1, L_AF_INET = 2, L_AF_INET6 = 10, L_AF_NETLINK = 16, L_AF_PACKET = 17 };

static int family_to_darwin(int f) {
    switch (f) {
    case L_AF_UNSPEC: return AF_UNSPEC;
    case L_AF_UNIX: return AF_UNIX;
    case L_AF_INET: return AF_INET;
    case L_AF_INET6: return AF_INET6;
    }
    return -1;
}
static int family_to_linux(int f) {
    switch (f) {
    case AF_UNSPEC: return L_AF_UNSPEC;
    case AF_UNIX: return L_AF_UNIX;
    case AF_INET: return L_AF_INET;
    case AF_INET6: return L_AF_INET6;
    }
    return f;
}

/// A Linux sockaddr into Darwin's. Returns the Darwin length, or -1 (errno set).
static int addr_to_darwin(const void *in, socklen_t len, struct sockaddr_storage *out) {
    memset(out, 0, sizeof *out);
    if (!in || len < 2) return fail(22);
    uint16_t fam = *(const uint16_t *)in;
    switch (fam) {
    case L_AF_INET: {
        struct sockaddr_in *s = (struct sockaddr_in *)out;
        if (len < 8) return fail(22);
        s->sin_len = sizeof *s;
        s->sin_family = AF_INET;
        memcpy(&s->sin_port, (const char *)in + 2, 2);
        memcpy(&s->sin_addr, (const char *)in + 4, 4);
        return sizeof *s;
    }
    case L_AF_INET6: {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)out;
        if (len < 24) return fail(22);
        s->sin6_len = sizeof *s;
        s->sin6_family = AF_INET6;
        memcpy(&s->sin6_port, (const char *)in + 2, 2);
        memcpy(&s->sin6_flowinfo, (const char *)in + 4, 4);
        memcpy(&s->sin6_addr, (const char *)in + 8, 16);
        if (len >= 28) memcpy(&s->sin6_scope_id, (const char *)in + 24, 4);
        return sizeof *s;
    }
    case L_AF_UNIX: {
        struct sockaddr_un *s = (struct sockaddr_un *)out;
        size_t n = len > 2 ? len - 2 : 0;
        const char *path = (const char *)in + 2;
        if (n > 0 && path[0] == 0) return fail(97);  // abstract namespace: EAFNOSUPPORT
        if (n >= sizeof s->sun_path) n = sizeof s->sun_path - 1;
        s->sun_family = AF_UNIX;
        memcpy(s->sun_path, path, n);
        s->sun_len = (uint8_t)(offsetof(struct sockaddr_un, sun_path) + strnlen(s->sun_path, sizeof s->sun_path));
        return s->sun_len;
    }
    case L_AF_UNSPEC: {  // (connect(AF_UNSPEC) dissolves a UDP association)
        out->ss_len = 16;
        out->ss_family = AF_UNSPEC;
        return 16;
    }
    }
    return fail(97);
}

/// A Darwin sockaddr into the app's Linux buffer (truncated as the kernel does; *len updated).
static void addr_to_linux(const struct sockaddr *in, void *out, socklen_t *len) {
    if (!out || !len) return;
    uint8_t buf[128] = {0};
    socklen_t n = 0;
    switch (in->sa_family) {
    case AF_INET: {
        const struct sockaddr_in *s = (const struct sockaddr_in *)in;
        *(uint16_t *)buf = L_AF_INET;
        memcpy(buf + 2, &s->sin_port, 2);
        memcpy(buf + 4, &s->sin_addr, 4);
        n = 16;
        break;
    }
    case AF_INET6: {
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)in;
        *(uint16_t *)buf = L_AF_INET6;
        memcpy(buf + 2, &s->sin6_port, 2);
        memcpy(buf + 4, &s->sin6_flowinfo, 4);
        memcpy(buf + 8, &s->sin6_addr, 16);
        memcpy(buf + 24, &s->sin6_scope_id, 4);
        n = 28;
        break;
    }
    case AF_UNIX: {
        const struct sockaddr_un *s = (const struct sockaddr_un *)in;
        *(uint16_t *)buf = L_AF_UNIX;
        size_t p = strnlen(s->sun_path, sizeof s->sun_path);
        memcpy(buf + 2, s->sun_path, p);
        n = (socklen_t)(2 + p + 1);
        break;
    }
    default:
        *(uint16_t *)buf = (uint16_t)family_to_linux(in->sa_family);
        n = 16;
    }
    memcpy(out, buf, n < *len ? n : *len);
    *len = n;
}

// ---- sockets -----------------------------------------------------------------------------------
enum { L_SOCK_NONBLOCK = 0x800, L_SOCK_CLOEXEC = 0x80000 };

static int apply_fd_flags(int fd, int nonblock, int cloexec) {
    if (nonblock) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (cloexec) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

static void no_sigpipe(int fd) {  // Linux apps pass MSG_NOSIGNAL; Darwin has a socket option
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
}

// ANDROPKG_LOG_NDKNET=1: log native name lookups, connects and socket errors.
static int net_trace(void) {
    static int on = -1;
    if (on < 0) on = getenv("ANDROPKG_LOG_NDKNET") != NULL;
    return on;
}
static const char *fmt_addr(const struct sockaddr *sa, char *buf, size_t n) {
    char ip[64] = "?";
    int port = 0;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        inet_ntop(AF_INET, &in->sin_addr, ip, sizeof ip);
        port = ntohs(in->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in = (const struct sockaddr_in6 *)sa;
        inet_ntop(AF_INET6, &in->sin6_addr, ip, sizeof ip);
        port = ntohs(in->sin6_port);
    } else {
        snprintf(buf, n, "family %d", sa->sa_family);
        return buf;
    }
    snprintf(buf, n, "%s:%d", ip, port);
    return buf;
}

NDK_EXPORT int bionic_socket(int family, int type, int protocol) {
    int df = family_to_darwin(family);
    if (df < 0) return fail(97);  // EAFNOSUPPORT (netlink, packet)
    int fd = socket(df, type & 0xf, protocol);
    if (fd < 0) RET(fd);
    no_sigpipe(fd);
    return apply_fd_flags(fd, type & L_SOCK_NONBLOCK, type & L_SOCK_CLOEXEC);
}

NDK_EXPORT int bionic_socketpair(int family, int type, int protocol, int sv[2]) {
    int df = family_to_darwin(family);
    if (df < 0) return fail(97);
    int r = socketpair(df, type & 0xf, protocol, sv);
    if (r < 0) RET(r);
    for (int i = 0; i < 2; i++) {
        no_sigpipe(sv[i]);
        apply_fd_flags(sv[i], type & L_SOCK_NONBLOCK, type & L_SOCK_CLOEXEC);
    }
    return 0;
}

NDK_EXPORT int bionic_bind(int fd, const void *addr, socklen_t len) {
    struct sockaddr_storage ss;
    int n = addr_to_darwin(addr, len, &ss);
    if (n < 0) return -1;
    RET(bind(fd, (struct sockaddr *)&ss, (socklen_t)n));
}
NDK_EXPORT int bionic_connect(int fd, const void *addr, socklen_t len) {
    struct sockaddr_storage ss;
    int n = addr_to_darwin(addr, len, &ss);
    if (n < 0) return -1;
    int r = connect(fd, (struct sockaddr *)&ss, (socklen_t)n);
    if (net_trace()) {
        int e = errno;
        char b[96];
        ndk_log(4, "net connect fd %d %s -> %d%s%s", fd, fmt_addr((struct sockaddr *)&ss, b, sizeof b), r,
                r < 0 ? " " : "", r < 0 ? strerror(e) : "");
        errno = e;
    }
    RET(r);
}
NDK_EXPORT int bionic_listen(int fd, int backlog) { RET(listen(fd, backlog)); }
NDK_EXPORT int bionic_shutdown(int fd, int how) { RET(shutdown(fd, how)); }

NDK_EXPORT int bionic_accept4(int fd, void *addr, socklen_t *len, int flags) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    int c = accept(fd, (struct sockaddr *)&ss, &sl);
    if (c < 0) RET(c);
    if (addr) addr_to_linux((struct sockaddr *)&ss, addr, len);
    no_sigpipe(c);
    return apply_fd_flags(c, flags & L_SOCK_NONBLOCK, flags & L_SOCK_CLOEXEC);
}
NDK_EXPORT int bionic_accept(int fd, void *addr, socklen_t *len) { return bionic_accept4(fd, addr, len, 0); }

NDK_EXPORT int bionic_getsockname(int fd, void *addr, socklen_t *len) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    int r = getsockname(fd, (struct sockaddr *)&ss, &sl);
    if (r < 0) RET(r);
    addr_to_linux((struct sockaddr *)&ss, addr, len);
    return 0;
}
NDK_EXPORT int bionic_getpeername(int fd, void *addr, socklen_t *len) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    int r = getpeername(fd, (struct sockaddr *)&ss, &sl);
    if (r < 0) RET(r);
    addr_to_linux((struct sockaddr *)&ss, addr, len);
    return 0;
}

// ---- socket options ------------------------------------------------------------------------
enum { OPT_IGNORE = -2, OPT_UNSUPPORTED = -1 };

static int map_option(int level, int opt, int *dlevel) {
    switch (level) {
    case 1:  // SOL_SOCKET
        *dlevel = SOL_SOCKET;
        switch (opt) {
        case 1: return SO_DEBUG;
        case 2: return SO_REUSEADDR;
        case 3: return SO_TYPE;
        case 4: return SO_ERROR;
        case 5: return SO_DONTROUTE;
        case 6: return SO_BROADCAST;
        case 7: return SO_SNDBUF;
        case 8: return SO_RCVBUF;
        case 9: return SO_KEEPALIVE;
        case 10: return SO_OOBINLINE;
        case 13: return SO_LINGER_SEC;  // (Linux's linger is in seconds)
        case 15: return SO_REUSEPORT;
        case 18: return SO_RCVLOWAT;
        case 19: return SO_SNDLOWAT;
        case 20: return SO_RCVTIMEO;
        case 21: return SO_SNDTIMEO;
        case 29: return SO_TIMESTAMP;
        case 30: return SO_ACCEPTCONN;
        case 12: case 16: case 25: case 32: case 33: case 36: case 46: return OPT_IGNORE;  // PRIORITY,
            // PASSCRED, BINDTODEVICE, SNDBUFFORCE, RCVBUFFORCE, MARK, BUSY_POLL
        }
        return OPT_UNSUPPORTED;
    case IPPROTO_TCP:
        *dlevel = IPPROTO_TCP;
        switch (opt) {
        case 1: return TCP_NODELAY;
        case 2: return TCP_MAXSEG;
        case 4: return TCP_KEEPALIVE;  // TCP_KEEPIDLE
        case 5: return TCP_KEEPINTVL;
        case 6: return TCP_KEEPCNT;
        case 25: return TCP_NOTSENT_LOWAT;
        case 3: case 9: case 12: case 13: case 18: case 23: case 30: return OPT_IGNORE;  // CORK,
            // DEFER_ACCEPT, QUICKACK, CONGESTION, USER_TIMEOUT, FASTOPEN, FASTOPEN_CONNECT
        }
        return OPT_UNSUPPORTED;
    case IPPROTO_IP:
        *dlevel = IPPROTO_IP;
        switch (opt) {
        case 1: return IP_TOS;
        case 2: return IP_TTL;
        case 32: return IP_MULTICAST_IF;
        case 33: return IP_MULTICAST_TTL;
        case 34: return IP_MULTICAST_LOOP;
        case 35: return IP_ADD_MEMBERSHIP;
        case 36: return IP_DROP_MEMBERSHIP;
        case 8: case 10: case 11: case 15: return OPT_IGNORE;  // PKTINFO, MTU_DISCOVER, RECVERR, FREEBIND
        }
        return OPT_UNSUPPORTED;
    case IPPROTO_IPV6:
        *dlevel = IPPROTO_IPV6;
        switch (opt) {
        case 16: return IPV6_UNICAST_HOPS;
        case 17: return IPV6_MULTICAST_IF;
        case 18: return IPV6_MULTICAST_HOPS;
        case 19: return IPV6_MULTICAST_LOOP;
        case 20: return IPV6_JOIN_GROUP;
        case 21: return IPV6_LEAVE_GROUP;
        case 26: return IPV6_V6ONLY;
        case 67: return IPV6_TCLASS;
        case 23: case 25: case 49: return OPT_IGNORE;  // MTU_DISCOVER, RECVERR, RECVPKTINFO
        }
        return OPT_UNSUPPORTED;
    }
    return OPT_UNSUPPORTED;
}

NDK_EXPORT int bionic_setsockopt(int fd, int level, int opt, const void *val, socklen_t len) {
    int dlevel;
    int dopt = map_option(level, opt, &dlevel);
    if (dopt == OPT_IGNORE) return 0;
    if (dopt == OPT_UNSUPPORTED) {
        if (net_trace()) ndk_log(4, "net setsockopt fd %d level %d opt %d: unsupported", fd, level, opt);
        return fail(92);  // ENOPROTOOPT
    }
    int r = setsockopt(fd, dlevel, dopt, val, len);
    if (r < 0 && net_trace()) {
        int e = errno;
        ndk_log(4, "net setsockopt fd %d level %d opt %d -> %s", fd, level, opt, strerror(e));
        errno = e;
    }
    RET(r);
}

NDK_EXPORT int bionic_getsockopt(int fd, int level, int opt, void *val, socklen_t *len) {
    if (level == 1 && (opt == 38 || opt == 39) && val && len && *len >= 4) {  // SO_PROTOCOL, SO_DOMAIN
        int v = 0;
        socklen_t l = sizeof v;
        if (opt == 39) {
            struct sockaddr_storage ss;
            socklen_t sl = sizeof ss;
            if (getsockname(fd, (struct sockaddr *)&ss, &sl) < 0) RET(-1);
            v = family_to_linux(ss.ss_family);
        } else {
            int type = 0;
            getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &l);
            v = type == SOCK_STREAM ? IPPROTO_TCP : type == SOCK_DGRAM ? IPPROTO_UDP : 0;
        }
        *(int *)val = v;
        *len = 4;
        return 0;
    }
    int dlevel;
    int dopt = map_option(level, opt, &dlevel);
    if (dopt == OPT_IGNORE) {
        if (val && len && *len >= 4) {
            *(int *)val = 0;
            *len = 4;
        }
        return 0;
    }
    if (dopt == OPT_UNSUPPORTED) return fail(92);
    int r = getsockopt(fd, dlevel, dopt, val, len);
    if (r < 0) RET(r);
    if (level == 1 && opt == 4 && val) *(int *)val = ndk_errno_to_linux(*(int *)val);  // SO_ERROR
    return 0;
}

// ---- send/recv -------------------------------------------------------------------------------
static int msg_to_darwin(int f) {
    int d = f & (MSG_OOB | MSG_PEEK | MSG_DONTROUTE);
    if (f & 0x40) d |= MSG_DONTWAIT;
    if (f & 0x80) d |= MSG_EOR;
    if (f & 0x100) d |= MSG_WAITALL;
    return d;  // (MSG_NOSIGNAL: SO_NOSIGPIPE on every socket; MSG_MORE, MSG_FASTOPEN: dropped)
}
static int msg_to_linux(int d) {
    int f = d & (MSG_OOB | MSG_PEEK | MSG_DONTROUTE);
    if (d & MSG_CTRUNC) f |= 0x8;
    if (d & MSG_TRUNC) f |= 0x20;
    if (d & MSG_EOR) f |= 0x80;
    return f;
}

static ssize_t net_io(const char *op, int fd, ssize_t r) {
    if (r < 0 && net_trace() && errno != EAGAIN) {
        int e = errno;
        ndk_log(4, "net %s fd %d -> %s", op, fd, strerror(e));
        errno = e;
    }
    return r;
}
NDK_EXPORT ssize_t bionic_send(int fd, const void *b, size_t n, int flags) { RET(net_io("send", fd, send(fd, b, n, msg_to_darwin(flags)))); }
NDK_EXPORT ssize_t bionic_recv(int fd, void *b, size_t n, int flags) { RET(net_io("recv", fd, recv(fd, b, n, msg_to_darwin(flags)))); }
NDK_EXPORT ssize_t bionic___recvfrom_chk(int fd, void *b, size_t n, size_t blen, int flags, void *addr, socklen_t *len);
NDK_EXPORT ssize_t bionic___sendto_chk(int fd, const void *b, size_t n, size_t blen, int flags, const void *addr, socklen_t len);

NDK_EXPORT ssize_t bionic_sendto(int fd, const void *b, size_t n, int flags, const void *addr, socklen_t len) {
    if (!addr) RET(sendto(fd, b, n, msg_to_darwin(flags), NULL, 0));
    struct sockaddr_storage ss;
    int sl = addr_to_darwin(addr, len, &ss);
    if (sl < 0) return -1;
    RET(sendto(fd, b, n, msg_to_darwin(flags), (struct sockaddr *)&ss, (socklen_t)sl));
}
NDK_EXPORT ssize_t bionic_recvfrom(int fd, void *b, size_t n, int flags, void *addr, socklen_t *len) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    ssize_t r = recvfrom(fd, b, n, msg_to_darwin(flags), addr ? (struct sockaddr *)&ss : NULL, addr ? &sl : NULL);
    if (r < 0) RET(r);
    if (addr && len) {
        if (sl > 0) addr_to_linux((struct sockaddr *)&ss, addr, len);
        else *len = 0;
    }
    return r;
}
NDK_EXPORT ssize_t bionic___sendto_chk(int fd, const void *b, size_t n, size_t blen, int flags, const void *addr, socklen_t len) {
    return bionic_sendto(fd, b, n, flags, addr, len);
}
NDK_EXPORT ssize_t bionic___recvfrom_chk(int fd, void *b, size_t n, size_t blen, int flags, void *addr, socklen_t *len) {
    return bionic_recvfrom(fd, b, n, flags, addr, len);
}

// Linux arm64 struct msghdr: size_t iovlen and controllen.
typedef struct {
    void *name;
    socklen_t namelen;
    struct iovec *iov;
    size_t iovlen;
    void *control;
    size_t controllen;
    int flags;
} l_msghdr;

NDK_EXPORT ssize_t bionic_sendmsg(int fd, const l_msghdr *m, int flags) {
    struct sockaddr_storage ss;
    struct msghdr d = {0};
    if (m->name) {
        int sl = addr_to_darwin(m->name, m->namelen, &ss);
        if (sl < 0) return -1;
        d.msg_name = &ss;
        d.msg_namelen = (socklen_t)sl;
    }
    d.msg_iov = m->iov;
    d.msg_iovlen = (int)m->iovlen;  // (control messages are not carried: SCM_RIGHTS & co)
    RET(sendmsg(fd, &d, msg_to_darwin(flags)));
}
NDK_EXPORT ssize_t bionic_recvmsg(int fd, l_msghdr *m, int flags) {
    struct sockaddr_storage ss;
    struct msghdr d = {0};
    d.msg_name = m->name ? &ss : NULL;
    d.msg_namelen = m->name ? sizeof ss : 0;
    d.msg_iov = m->iov;
    d.msg_iovlen = (int)m->iovlen;
    ssize_t r = recvmsg(fd, &d, msg_to_darwin(flags));
    if (r < 0) RET(r);
    if (m->name) {
        if (d.msg_namelen > 0) addr_to_linux((struct sockaddr *)&ss, m->name, &m->namelen);
        else m->namelen = 0;
    }
    m->controllen = 0;
    m->flags = msg_to_linux(d.msg_flags);
    return r;
}

// ---- names -------------------------------------------------------------------------------------
// bionic's struct addrinfo is BSD's (same as Darwin's); the sockaddrs and families inside differ,
// and AI_NUMERICSERV is 8 in bionic, 0x1000 on Darwin.
NDK_EXPORT int bionic_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                                  struct addrinfo **res) {
    struct addrinfo h, *dres = NULL;
    if (hints) {
        h = *hints;
        int fam = family_to_darwin(hints->ai_family);
        if (fam < 0) return EAI_FAMILY;
        h.ai_family = fam;
        h.ai_flags = (hints->ai_flags & ~8) | ((hints->ai_flags & 8) ? AI_NUMERICSERV : 0);
        h.ai_addr = NULL;
        h.ai_canonname = NULL;
        h.ai_next = NULL;
    }
    int r = getaddrinfo(node, service, hints ? &h : NULL, &dres);
    if (net_trace()) {
        char b[96] = "";
        if (r == 0 && dres) fmt_addr(dres->ai_addr, b, sizeof b);
        ndk_log(4, "net getaddrinfo %s:%s -> %d %s", node ? node : "(null)", service ? service : "", r,
                r == 0 ? b : gai_strerror(r));
    }
    if (r != 0) {
        if (r == EAI_SYSTEM) ndk_fix_errno();
        return r;  // (EAI_* values: the same BSD numbering in bionic)
    }
    struct addrinfo *head = NULL, **tail = &head;
    for (struct addrinfo *p = dres; p; p = p->ai_next) {
        struct addrinfo *a = calloc(1, sizeof *a + 128);
        if (!a) break;
        *a = *p;
        a->ai_family = family_to_linux(p->ai_family);
        a->ai_addr = (struct sockaddr *)(a + 1);
        socklen_t len = 128;
        addr_to_linux(p->ai_addr, a->ai_addr, &len);
        a->ai_addrlen = len;
        a->ai_canonname = p->ai_canonname ? strdup(p->ai_canonname) : NULL;
        a->ai_next = NULL;
        *tail = a;
        tail = &a->ai_next;
    }
    freeaddrinfo(dres);
    *res = head;
    return head ? 0 : EAI_MEMORY;
}

NDK_EXPORT void bionic_freeaddrinfo(struct addrinfo *a) {
    while (a) {
        struct addrinfo *n = a->ai_next;
        free(a->ai_canonname);
        free(a);  // (ai_addr lives in the same allocation)
        a = n;
    }
}

NDK_EXPORT const char *bionic_gai_strerror(int e) { return gai_strerror(e); }

NDK_EXPORT int bionic_getnameinfo(const void *addr, socklen_t len, char *host, socklen_t hostlen, char *serv,
                                  socklen_t servlen, int flags) {
    struct sockaddr_storage ss;
    int sl = addr_to_darwin(addr, len, &ss);
    if (sl < 0) return EAI_FAMILY;
    return getnameinfo((struct sockaddr *)&ss, (socklen_t)sl, host, hostlen, serv, servlen, flags);
}

// struct hostent is the same layout; h_addrtype is a family.
static struct hostent *fix_hostent(struct hostent *h) {
    if (h) h->h_addrtype = family_to_linux(h->h_addrtype);
    return h;
}
NDK_EXPORT struct hostent *bionic_gethostbyname(const char *name) { return fix_hostent(gethostbyname(name)); }
NDK_EXPORT struct hostent *bionic_gethostbyname2(const char *name, int af) {
    return fix_hostent(gethostbyname2(name, family_to_darwin(af)));
}
NDK_EXPORT struct hostent *bionic_gethostbyaddr(const void *addr, socklen_t len, int type) {
    return fix_hostent(gethostbyaddr(addr, len, family_to_darwin(type)));
}

NDK_EXPORT int bionic_inet_pton(int af, const char *src, void *dst) {
    int d = family_to_darwin(af);
    if (d < 0) return fail(97);
    RET(inet_pton(d, src, dst));
}
NDK_EXPORT const char *bionic_inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    int d = family_to_darwin(af);
    if (d < 0) {
        fail(97);
        return NULL;
    }
    const char *r = inet_ntop(d, src, dst, size);
    if (!r) ndk_fix_errno();
    return r;
}

// ---- eventfd -------------------------------------------------------------------------------------
enum { L_EFD_SEMAPHORE = 1, L_EFD_NONBLOCK = 0x800, L_EFD_CLOEXEC = 0x80000 };
typedef struct {
    int writeFd;
    uint64_t counter;
    int flags;
} EventFd;
#define MAX_EVENTFD 4096
static EventFd *g_eventfds[MAX_EVENTFD];
static pthread_mutex_t g_eventfd_lock = PTHREAD_MUTEX_INITIALIZER;

static EventFd *eventfd_of(int fd) { return fd >= 0 && fd < MAX_EVENTFD ? g_eventfds[fd] : NULL; }

NDK_EXPORT int bionic_eventfd(unsigned initval, int flags) {
    int p[2];
    if (pipe(p) < 0) RET(-1);
    if (p[0] >= MAX_EVENTFD) {
        close(p[0]);
        close(p[1]);
        return fail(24);  // EMFILE
    }
    fcntl(p[0], F_SETFL, O_NONBLOCK);  // (we wait ourselves when the app's fd is blocking)
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    if (flags & L_EFD_CLOEXEC) fcntl(p[0], F_SETFD, FD_CLOEXEC);
    EventFd *e = calloc(1, sizeof *e);
    e->writeFd = p[1];
    e->counter = initval;
    e->flags = flags;
    if (initval) (void)!write(p[1], "x", 1);
    pthread_mutex_lock(&g_eventfd_lock);
    g_eventfds[p[0]] = e;
    pthread_mutex_unlock(&g_eventfd_lock);
    return p[0];
}

/// read() on an eventfd: true if `fd` is one (the result in *ret).
bool ndk_eventfd_read(int fd, void *buf, size_t n, ssize_t *ret) {
    EventFd *e = eventfd_of(fd);
    if (!e) return false;
    if (n < 8) {
        *ret = fail(22);
        return true;
    }
    for (;;) {
        pthread_mutex_lock(&g_eventfd_lock);
        if (e->counter > 0) {
            uint64_t v = (e->flags & L_EFD_SEMAPHORE) ? 1 : e->counter;
            e->counter -= v;
            if (e->counter == 0) {
                char drain[16];
                while (read(fd, drain, sizeof drain) > 0) {}
            }
            pthread_mutex_unlock(&g_eventfd_lock);
            memcpy(buf, &v, 8);
            *ret = 8;
            return true;
        }
        pthread_mutex_unlock(&g_eventfd_lock);
        if (e->flags & L_EFD_NONBLOCK) {
            *ret = fail(11);  // EAGAIN
            return true;
        }
        struct pollfd p = {fd, POLLIN, 0};
        poll(&p, 1, -1);  // until a write makes the pipe readable
    }
}

/// write() on an eventfd: true if `fd` is one.
bool ndk_eventfd_write(int fd, const void *buf, size_t n, ssize_t *ret) {
    EventFd *e = eventfd_of(fd);
    if (!e) return false;
    uint64_t v;
    if (n < 8) {
        *ret = fail(22);
        return true;
    }
    memcpy(&v, buf, 8);
    if (v == UINT64_MAX) {
        *ret = fail(22);
        return true;
    }
    pthread_mutex_lock(&g_eventfd_lock);
    bool wasZero = e->counter == 0;
    e->counter += v;
    if (wasZero && v) (void)!write(e->writeFd, "x", 1);
    pthread_mutex_unlock(&g_eventfd_lock);
    *ret = 8;
    return true;
}

/// fcntl(F_SETFL/F_GETFL) on an eventfd: its blocking mode is ours to keep (the pipe never blocks).
bool ndk_eventfd_nonblock(int fd, int set, int *flags) {
    EventFd *e = eventfd_of(fd);
    if (!e) return false;
    if (set >= 0) e->flags = set ? (e->flags | L_EFD_NONBLOCK) : (e->flags & ~L_EFD_NONBLOCK);
    if (flags) *flags = (e->flags & L_EFD_NONBLOCK) ? 0x800 : 0;
    return true;
}

void ndk_eventfd_close(int fd) {
    EventFd *e = eventfd_of(fd);
    if (!e) return;
    pthread_mutex_lock(&g_eventfd_lock);
    g_eventfds[fd] = NULL;
    pthread_mutex_unlock(&g_eventfd_lock);
    close(e->writeFd);
    free(e);
}

NDK_EXPORT int bionic_eventfd_read(int fd, uint64_t *v) {
    ssize_t r;
    if (!ndk_eventfd_read(fd, v, 8, &r)) return fail(22);
    return r == 8 ? 0 : -1;
}
NDK_EXPORT int bionic_eventfd_write(int fd, uint64_t v) {
    ssize_t r;
    if (!ndk_eventfd_write(fd, &v, 8, &r)) return fail(22);
    return r == 8 ? 0 : -1;
}

// ---- epoll on kqueue ----------------------------------------------------------------------------
enum {
    EPOLLIN = 1, EPOLLPRI = 2, EPOLLOUT = 4, EPOLLERR = 8, EPOLLHUP = 0x10, EPOLLRDHUP = 0x2000,
    EPOLLONESHOT = 1u << 30, EPOLLET = 1u << 31,
};
typedef struct { uint32_t events; uint64_t data; } l_epoll_event;  // arm64: not packed, 16 bytes
_Static_assert(sizeof(l_epoll_event) == 16, "epoll_event");

typedef struct { int fd; uint32_t events; uint64_t data; } Interest;
typedef struct {
    pthread_mutex_t lock;
    Interest *items;
    int count, cap;
} Epoll;
#define MAX_EPOLL 4096
static Epoll *g_epolls[MAX_EPOLL];

static Epoll *epoll_of(int fd) { return fd >= 0 && fd < MAX_EPOLL ? g_epolls[fd] : NULL; }

NDK_EXPORT int bionic_epoll_create1(int flags) {
    int kq = kqueue();
    if (kq < 0) RET(kq);
    if (kq >= MAX_EPOLL) {
        close(kq);
        return fail(24);
    }
    if (flags & 0x80000) fcntl(kq, F_SETFD, FD_CLOEXEC);
    Epoll *e = calloc(1, sizeof *e);
    pthread_mutex_init(&e->lock, NULL);
    g_epolls[kq] = e;
    return kq;
}
NDK_EXPORT int bionic_epoll_create(int size) { return size <= 0 ? fail(22) : bionic_epoll_create1(0); }

void ndk_epoll_close(int fd) {
    Epoll *e = epoll_of(fd);
    if (!e) return;
    g_epolls[fd] = NULL;
    pthread_mutex_destroy(&e->lock);
    free(e->items);
    free(e);
}

static Interest *find_interest(Epoll *e, int fd) {
    for (int i = 0; i < e->count; i++)
        if (e->items[i].fd == fd) return &e->items[i];
    return NULL;
}

static int register_filters(int kq, int fd, uint32_t ev, bool add) {
    struct kevent k[2];
    int n = 0;
    uint16_t flags = add ? EV_ADD | EV_ENABLE : EV_DELETE;
    if (add) {
        if (ev & EPOLLET) flags |= EV_CLEAR;
        if (ev & EPOLLONESHOT) flags |= EV_DISPATCH;
    }
    if (!add || (ev & (EPOLLIN | EPOLLPRI | EPOLLRDHUP))) EV_SET(&k[n++], fd, EVFILT_READ, flags, 0, 0, NULL);
    if (!add || (ev & EPOLLOUT)) EV_SET(&k[n++], fd, EVFILT_WRITE, flags, 0, 0, NULL);
    if (n == 0) return 0;
    // EV_RECEIPT: per-filter results (a delete of an absent filter isn't an error here)
    for (int i = 0; i < n; i++) k[i].flags |= EV_RECEIPT;
    struct kevent out[2];
    int r = kevent(kq, k, n, out, n, NULL);
    if (r < 0) return -1;
    for (int i = 0; i < r; i++)
        if ((out[i].flags & EV_ERROR) && out[i].data != 0 && add) {
            errno = (int)out[i].data;
            return -1;
        }
    return 0;
}

NDK_EXPORT int bionic_epoll_ctl(int epfd, int op, int fd, l_epoll_event *ev) {
    Epoll *e = epoll_of(epfd);
    if (!e) return fail(9);  // EBADF
    pthread_mutex_lock(&e->lock);
    Interest *it = find_interest(e, fd);
    int r = 0;
    switch (op) {
    case 1:  // EPOLL_CTL_ADD
        if (it) {
            r = fail(17);  // EEXIST
            break;
        }
        if (register_filters(epfd, fd, ev->events, true) < 0) {
            ndk_fix_errno();
            r = -1;
            break;
        }
        if (e->count == e->cap) {
            e->cap = e->cap ? e->cap * 2 : 16;
            e->items = realloc(e->items, (size_t)e->cap * sizeof *e->items);
        }
        e->items[e->count++] = (Interest){fd, ev->events, ev->data};
        break;
    case 3:  // EPOLL_CTL_MOD
        if (!it) {
            r = fail(2);  // ENOENT
            break;
        }
        register_filters(epfd, fd, 0, false);
        if (register_filters(epfd, fd, ev->events, true) < 0) {
            ndk_fix_errno();
            r = -1;
            break;
        }
        it->events = ev->events;
        it->data = ev->data;
        break;
    case 2:  // EPOLL_CTL_DEL
        if (!it) {
            r = fail(2);
            break;
        }
        register_filters(epfd, fd, 0, false);
        *it = e->items[--e->count];
        break;
    default:
        r = fail(22);
    }
    pthread_mutex_unlock(&e->lock);
    return r;
}

NDK_EXPORT int bionic_epoll_wait(int epfd, l_epoll_event *events, int max, int timeout) {
    Epoll *e = epoll_of(epfd);
    if (!e) return fail(9);
    if (max <= 0) return fail(22);
    struct kevent k[128];
    int want = max * 2 < 128 ? max * 2 : 128;
    struct timespec ts, *tp = NULL;
    if (timeout >= 0) {
        ts.tv_sec = timeout / 1000;
        ts.tv_nsec = (long)(timeout % 1000) * 1000000L;
        tp = &ts;
    }
    int n = kevent(epfd, NULL, 0, k, want, tp);
    if (n < 0) RET(n);
    int out = 0, outFds[128];
    pthread_mutex_lock(&e->lock);
    for (int i = 0; i < n; i++) {
        int fd = (int)k[i].ident;
        Interest *it = find_interest(e, fd);
        if (!it) continue;  // removed meanwhile
        uint32_t ev = 0;
        if (k[i].filter == EVFILT_READ) {
            ev |= EPOLLIN & it->events;
            if (k[i].flags & EV_EOF) ev |= EPOLLRDHUP | ((it->events & EPOLLOUT) ? 0 : EPOLLHUP) | (EPOLLIN & it->events);
        } else if (k[i].filter == EVFILT_WRITE) {
            ev |= EPOLLOUT;
            if (k[i].flags & EV_EOF) ev |= EPOLLHUP;
        }
        if ((k[i].flags & EV_ERROR) || ((k[i].flags & EV_EOF) && k[i].fflags)) ev |= EPOLLERR;
        int j;  // one event per fd (its read and write filters fire separately)
        for (j = 0; j < out; j++)
            if (outFds[j] == fd) break;
        if (j < out) {
            events[j].events |= ev;
        } else if (out < max) {
            events[out].events = ev;
            events[out].data = it->data;
            outFds[out++] = fd;
        }
    }
    pthread_mutex_unlock(&e->lock);
    return out;
}
NDK_EXPORT int bionic_epoll_pwait(int epfd, l_epoll_event *events, int max, int timeout, const void *sigmask) {
    return bionic_epoll_wait(epfd, events, max, timeout);
}
