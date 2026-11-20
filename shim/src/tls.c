#include "andropkg/tls.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    void *neg[3];    // slots -3..-1
    void *slots[8];  // slots 0..7; the thread pointer is &slots[0]
    // TODO(M4): static ELF TLS blocks (PT_TLS of loaded images) follow here.
} bionic_tcb;

static pthread_key_t g_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static uintptr_t g_guard;

static void tcb_free(void *tp) { free((char *)tp - offsetof(bionic_tcb, slots)); }

static void tls_init(void) {
    if (pthread_key_create(&g_key, tcb_free) != 0) abort();
    arc4random_buf(&g_guard, sizeof g_guard);
}

uint64_t andropkg_tls_tsd_offset(void) {
    pthread_once(&g_once, tls_init);
    return (uint64_t)g_key * sizeof(void *);
}

uintptr_t andropkg_tls_stack_guard(void) {
    pthread_once(&g_once, tls_init);
    return g_guard;
}

void **andropkg_tls_current(void) {
    pthread_once(&g_once, tls_init);
    return pthread_getspecific(g_key);
}

void **andropkg_tls_attach_thread(void) {
    void **tp = andropkg_tls_current();
    if (tp) return tp;
    bionic_tcb *tcb = calloc(1, sizeof *tcb);
    if (!tcb) abort();
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    tcb->slots[BIONIC_TLS_SLOT_THREAD_ID] = (void *)(uintptr_t)(tid & 0x7fffffff);
    tcb->slots[BIONIC_TLS_SLOT_STACK_GUARD] = (void *)g_guard;
    tp = tcb->slots;
    pthread_setspecific(g_key, tp);
    return tp;
}

bool andropkg_tls_selfcheck(char *detail, size_t len) {
    void **tp = andropkg_tls_attach_thread();
    uintptr_t tsd;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tsd));
    void **via_tsd = ((void ***)(tsd & ~(uintptr_t)7))[andropkg_tls_tsd_offset() / sizeof(void *)];
    snprintf(detail, len, "key=%lu tsd_base=%#lx tp=%p tsd[key]=%p", (unsigned long)g_key,
             (unsigned long)tsd, (void *)tp, (void *)via_tsd);
    return via_tsd == tp;
}
