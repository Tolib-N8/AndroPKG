// andropkg NDK layer — libffi's closure allocator for Android images (JNA, ...).
//
// The converter replaces ffi_closure_alloc/ffi_closure_free in any image that defines them
// (converter Patches.swift, PatchKind.hookedFunctions) with these. A closure is ordinary
// writable memory; its code address is one of the pre-signed trampolines in ffi_closures.S.
// The image's own ffi_prep_closure_loc then fills the closure as usual (the template, the
// target at +16, cif/fun/user_data), which the trampoline uses.
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ndk_internal.h"

#define TRAMPOLINES 4096
#define TRAMPOLINE_SIZE 16

extern char andropkg_ffi_tramps[];
extern void *andropkg_ffi_slots[TRAMPOLINES];

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_next_free = -1;  // free list through g_link
static int g_link[TRAMPOLINES];
static int g_used;            // slots handed out at least once

NDK_EXPORT void *andropkg_ffi_closure_alloc(size_t size, void **code) {
    if (code == NULL) return NULL;
    void *closure = calloc(1, size < 64 ? 64 : size);
    if (closure == NULL) return NULL;
    pthread_mutex_lock(&g_lock);
    int i = -1;
    if (g_next_free >= 0) {
        i = g_next_free;
        g_next_free = g_link[i];
    } else if (g_used < TRAMPOLINES) {
        i = g_used++;
    }
    if (i >= 0) andropkg_ffi_slots[i] = closure;
    pthread_mutex_unlock(&g_lock);
    if (i < 0) {
        ndk_log(NDK_LOG_ERROR, "ffi_closure_alloc: all %d trampolines in use", TRAMPOLINES);
        free(closure);
        return NULL;
    }
    *code = andropkg_ffi_tramps + (size_t)i * TRAMPOLINE_SIZE;
    return closure;
}

NDK_EXPORT void andropkg_ffi_closure_free(void *closure) {
    if (closure == NULL) return;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_used; i++) {
        if (andropkg_ffi_slots[i] == closure) {
            andropkg_ffi_slots[i] = NULL;
            g_link[i] = g_next_free;
            g_next_free = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    free(closure);
}
