// andropkg NDK layer — in-process "processes": an app's Android ELF executable (e.g. a helper
// it runs with ProcessBuilder) runs on a thread of this process, since iOS has no fork/exec.
//
// libcore's UNIXProcess.forkAndExec hands such programs to andropkg_ndk_spawn (patched in
// runtime/patches/libcore.patch). The executable is linked once; every run restores its
// writable segments, runs its initializers, and enters its ELF entry point on a fresh stack
// laid out as the kernel would (argc, argv, envp, auxv). bionic's __libc_init then calls main.
// On that thread (and threads it starts) fds 0/1/2 and stdin/stdout/stderr are the pipes
// UNIXProcess made, and exit() — or returning from main — ends only that "process".
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "andropkg/tls.h"
#include "ndk_internal.h"

struct ndk_proc {
    int pid;
    ndk_lib *exe;
    char **argv, **envp;
    int argc, envc;
    int fd[3];
    FILE *stream[3];
    int status;
    bool done;
    jmp_buf exit_jump;
    struct { void (*fn)(void *); void *arg; } atexit[32];
    int natexit;
    pthread_mutex_t m;
    pthread_cond_t c;
    ndk_proc *next;
};

static __thread ndk_proc *t_proc;
static pthread_mutex_t g_procs_lock = PTHREAD_MUTEX_INITIALIZER;
static ndk_proc *g_procs;
static _Atomic int g_next_pid = 0x40000000;  // far from real pids

ndk_proc *ndk_current_proc(void) { return t_proc; }
void ndk_set_current_proc(ndk_proc *p) { t_proc = p; }
int ndk_proc_fd(int fd) { return (t_proc && fd >= 0 && fd <= 2) ? t_proc->fd[fd] : fd; }
void *ndk_proc_stream(void *f) {
    if (!t_proc) return f;
    if (f == stdin) return t_proc->stream[0];
    if (f == stdout) return t_proc->stream[1];
    if (f == stderr) return t_proc->stream[2];
    return f;
}

bool ndk_proc_atexit(void (*fn)(void *), void *arg) {
    if (!t_proc || t_proc->natexit >= 32) return false;
    t_proc->atexit[t_proc->natexit].fn = fn;
    t_proc->atexit[t_proc->natexit].arg = arg;
    t_proc->natexit++;
    return true;
}

void ndk_proc_exit(int status) {
    ndk_proc *p = t_proc;
    if (!p) return;
    p->status = status & 0xff;
    longjmp(p->exit_jump, 1);
}

void andropkg_exe_enter(uintptr_t entry, void *sp);  // exec_entry.S

/// bionic's __libc_init, as reached from an executable's _start: run main, then exit.
NDK_EXPORT void bionic___libc_init(void *raw_args, void (*onexit)(void), int (*slingshot)(int, char **, char **),
                                   const void *structors) {
    (void)onexit; (void)structors;
    long *args = raw_args;
    int argc = (int)args[0];
    char **argv = (char **)(args + 1);
    char **envp = argv + argc + 1;
    int r = slingshot(argc, argv, envp);
    extern void bionic_exit(int);
    bionic_exit(r);
}

static void *proc_main(void *arg) {
    ndk_proc *p = arg;
    t_proc = p;
    andropkg_tls_attach_thread();
    ndk_executable_begin(p->exe);
    // Initial stack: argc, argv[], NULL, envp[], NULL, auxv (AT_PAGESZ, AT_NULL).
    size_t stack_size = 8 << 20;
    void *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    size_t words = 1 + (size_t)p->argc + 1 + (size_t)p->envc + 1 + 4;
    uint64_t *sp = (uint64_t *)(((uintptr_t)stack + stack_size - 4096 - words * 8) & ~(uintptr_t)15);
    size_t k = 0;
    sp[k++] = (uint64_t)p->argc;
    for (int i = 0; i < p->argc; i++) sp[k++] = (uint64_t)(uintptr_t)p->argv[i];
    sp[k++] = 0;
    for (int i = 0; i < p->envc; i++) sp[k++] = (uint64_t)(uintptr_t)p->envp[i];
    sp[k++] = 0;
    sp[k++] = 6;  // AT_PAGESZ
    sp[k++] = (uint64_t)getpagesize();
    sp[k++] = 0;  // AT_NULL
    sp[k++] = 0;
    ndk_log(NDK_LOG_INFO, "process %d: %s", p->pid, p->argv[0]);
    if (setjmp(p->exit_jump) == 0) andropkg_exe_enter(ndk_lib_entry(p->exe), sp);
    // exit(): run this process's atexit handlers, flush and close its stdio.
    for (int i = p->natexit; i-- > 0;) p->atexit[i].fn(p->atexit[i].arg);
    t_proc = NULL;
    for (int i = 0; i < 3; i++) {
        if (p->stream[i]) fclose(p->stream[i]);  // also closes fd[i]
        else if (p->fd[i] >= 0) close(p->fd[i]);
    }
    munmap(stack, stack_size);
    ndk_executable_end(p->exe);
    ndk_log(NDK_LOG_INFO, "process %d exited with %d", p->pid, p->status);
    pthread_mutex_lock(&p->m);
    p->done = true;
    pthread_cond_broadcast(&p->c);
    pthread_mutex_unlock(&p->m);
    return NULL;
}

static char **copy_vector(const char *const *v, int *count) {
    int n = 0;
    while (v && v[n]) n++;
    char **out = calloc((size_t)n + 1, sizeof *out);
    for (int i = 0; i < n; i++) out[i] = strdup(v[i]);
    *count = n;
    return out;
}

/// Starts `prog` (an Android ELF executable) as an in-process "process" reading `in` and writing
/// `out`/`err` (taken over). Returns its pid, or -1 with errno set.
NDK_EXPORT int andropkg_ndk_spawn(const char *prog, const char *const *argv, const char *const *envp, const char *dir,
                                  int in, int out, int err) {
    (void)dir;
    char msg[1024];
    ndk_lib *exe = ndk_load_executable(prog, msg, sizeof msg);
    if (!exe) {
        ndk_log(NDK_LOG_ERROR, "cannot run %s: %s", prog, msg);
        errno = ENOEXEC;
        return -1;
    }
    ndk_proc *p = calloc(1, sizeof *p);
    p->pid = atomic_fetch_add(&g_next_pid, 1);
    p->exe = exe;
    p->argv = copy_vector(argv, &p->argc);
    extern char **bionic_environ;
    p->envp = copy_vector(envp ? envp : (const char *const *)bionic_environ, &p->envc);
    p->fd[0] = in;
    p->fd[1] = out;
    p->fd[2] = err;
    p->stream[0] = fdopen(in, "r");
    p->stream[1] = fdopen(out, "w");
    p->stream[2] = fdopen(err, "w");
    if (p->stream[2]) setvbuf(p->stream[2], NULL, _IONBF, 0);
    pthread_mutex_init(&p->m, NULL);
    pthread_cond_init(&p->c, NULL);
    pthread_mutex_lock(&g_procs_lock);
    p->next = g_procs;
    g_procs = p;
    pthread_mutex_unlock(&g_procs_lock);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    pthread_create(&t, &a, proc_main, p);
    pthread_attr_destroy(&a);
    return p->pid;
}

static ndk_proc *find_proc(int pid, bool unlink) {
    pthread_mutex_lock(&g_procs_lock);
    ndk_proc **pp = &g_procs;
    while (*pp && (*pp)->pid != pid) pp = &(*pp)->next;
    ndk_proc *p = *pp;
    if (p && unlink) *pp = p->next;
    pthread_mutex_unlock(&g_procs_lock);
    return p;
}

/// Waits for an in-process "process"; returns its exit status (or -1 if unknown).
NDK_EXPORT int andropkg_ndk_wait(int pid) {
    ndk_proc *p = find_proc(pid, false);
    if (!p) return -1;
    pthread_mutex_lock(&p->m);
    while (!p->done) pthread_cond_wait(&p->c, &p->m);
    pthread_mutex_unlock(&p->m);
    int status = p->status;
    find_proc(pid, true);
    return status;
}

NDK_EXPORT bool andropkg_ndk_is_spawned(int pid) { return pid >= 0x40000000; }

/// destroy(): a thread can't be killed; its output is cut off and its result ignored.
NDK_EXPORT void andropkg_ndk_kill(int pid) { ndk_log(NDK_LOG_INFO, "process %d: destroy requested (runs to completion)", pid); }
