// andropkg: Darwin's xattr API takes extra (position, options) arguments; provide the Linux
// signatures as C++ overloads (C callers must use the Darwin form).
#pragma once
#include_next <sys/xattr.h>
#ifdef __cplusplus
inline ssize_t getxattr(const char* p, const char* n, void* v, size_t s) { return ::getxattr(p, n, v, s, 0, 0); }
inline ssize_t lgetxattr(const char* p, const char* n, void* v, size_t s) { return ::getxattr(p, n, v, s, 0, XATTR_NOFOLLOW); }
inline ssize_t fgetxattr(int fd, const char* n, void* v, size_t s) { return ::fgetxattr(fd, n, v, s, 0, 0); }
inline int setxattr(const char* p, const char* n, const void* v, size_t s, int f) { return ::setxattr(p, n, v, s, 0, f); }
inline int lsetxattr(const char* p, const char* n, const void* v, size_t s, int f) { return ::setxattr(p, n, v, s, 0, f | XATTR_NOFOLLOW); }
inline int fsetxattr(int fd, const char* n, const void* v, size_t s, int f) { return ::fsetxattr(fd, n, v, s, 0, f); }
inline ssize_t listxattr(const char* p, char* l, size_t s) { return ::listxattr(p, l, s, 0); }
inline ssize_t llistxattr(const char* p, char* l, size_t s) { return ::listxattr(p, l, s, XATTR_NOFOLLOW); }
inline ssize_t flistxattr(int fd, char* l, size_t s) { return ::flistxattr(fd, l, s, 0); }
inline int removexattr(const char* p, const char* n) { return ::removexattr(p, n, 0); }
inline int lremovexattr(const char* p, const char* n) { return ::removexattr(p, n, XATTR_NOFOLLOW); }
inline int fremovexattr(int fd, const char* n) { return ::fremovexattr(fd, n, 0); }
#endif
