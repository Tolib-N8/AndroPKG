// andropkg NDK layer — the linker: app ELF libraries, their DT_NEEDED closure, symbol
// resolution, and the libdl API the libraries themselves use (dlopen, dlsym, dladdr,
// dl_iterate_phdr — which C++ exception unwinding relies on).
//
// Each ELF image was wrapped by the converter into a signed Mach-O (lib.so -> lib.so.dylib,
// next to it or in $ANDROPKG_NDK_WRAPPED_DIR); dyld maps it, loader/ links it.
//
// Undefined symbols resolve like the bionic linker for an app library: the system (global)
// group first — here the NDK system libraries this layer implements — then the library itself
// and its dependencies breadth-first. A function nobody provides binds to a stub that logs its
// name when called (loading never fails over an unused import).
#include <dlfcn.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ndk_internal.h"

struct ndk_lib {
    apk_image *img;
    char *path;   // the ELF path the app asked for
    char *name;   // basename (what DT_NEEDED and dlopen("libfoo.so") use)
    ndk_lib **deps;
    size_t ndeps;
    bool linked, initialized;
    uintptr_t start, end;  // mapped ELF range
};

static pthread_mutex_t g_lock;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static ndk_lib *g_libs[256];
static _Atomic size_t g_nlibs;
static char g_libdir[PATH_MAX];
static unsigned long long g_adds;

static void init_lock(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_lock, &a);
}
static void lock(void) {
    pthread_once(&g_once, init_lock);
    pthread_mutex_lock(&g_lock);
}
static void unlock(void) { pthread_mutex_unlock(&g_lock); }

void ndk_set_library_dir(const char *dir) {
    if (dir && !g_libdir[0]) strlcpy(g_libdir, dir, sizeof g_libdir);
}

bool ndk_is_android_pc(uintptr_t pc) {
    size_t n = atomic_load_explicit(&g_nlibs, memory_order_acquire);
    for (size_t i = 0; i < n; i++)
        if (pc >= g_libs[i]->start && pc < g_libs[i]->end) return true;
    return false;
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static ndk_lib *find_loaded(const char *path_or_name) {
    const char *name = base_name(path_or_name);
    size_t n = atomic_load(&g_nlibs);
    for (size_t i = 0; i < n; i++) {
        ndk_lib *l = g_libs[i];
        if (strcmp(l->path, path_or_name) == 0 || strcmp(l->name, name) == 0) return l;
        const char *so = apk_image_soname(l->img);
        if (so && strcmp(base_name(so), name) == 0) return l;
    }
    return NULL;
}

/// The wrapped Mach-O for an ELF path, or NULL.
static bool wrapped_path(const char *elf, char *out, size_t cap) {
    struct stat st;
    snprintf(out, cap, "%s.dylib", elf);
    if (stat(out, &st) == 0) return true;
    const char *dir = getenv("ANDROPKG_NDK_WRAPPED_DIR");
    if (dir) {
        snprintf(out, cap, "%s/%s.dylib", dir, base_name(elf));
        if (stat(out, &st) == 0) return true;
    }
    return false;
}

/// Finds the ELF file for a DT_NEEDED / dlopen name: next to `near`, then the library dir.
static bool locate(const char *name, const char *near, char *out, size_t cap) {
    struct stat st;
    if (strchr(name, '/')) {
        strlcpy(out, name, cap);
        return stat(out, &st) == 0 || wrapped_path(out, (char[PATH_MAX]){0}, PATH_MAX);
    }
    char dirbuf[PATH_MAX];
    if (near) {
        strlcpy(dirbuf, near, sizeof dirbuf);
        snprintf(out, cap, "%s/%s", dirname(dirbuf), name);
        char w[PATH_MAX];
        if (stat(out, &st) == 0 || wrapped_path(out, w, sizeof w)) return true;
    }
    if (g_libdir[0]) {
        snprintf(out, cap, "%s/%s", g_libdir, name);
        char w[PATH_MAX];
        if (stat(out, &st) == 0 || wrapped_path(out, w, sizeof w)) return true;
    }
    return false;
}

// ---- symbol resolution ---------------------------------------------------------------------

static void *search_group(ndk_lib *root, const char *name) {
    // Breadth-first over root and its dependencies.
    ndk_lib *queue[256];
    size_t head = 0, tail = 0;
    queue[tail++] = root;
    while (head < tail) {
        ndk_lib *l = queue[head++];
        void *p = apk_image_sym(l->img, name);
        if (p) return p;
        for (size_t i = 0; i < l->ndeps && tail < 256; i++) {
            bool seen = false;
            for (size_t j = 0; j < tail; j++) seen |= queue[j] == l->deps[i];
            if (!seen) queue[tail++] = l->deps[i];
        }
    }
    return NULL;
}

static void *resolve(const apk_image *img, const char *name, const Elf64_Sym *sym, void *ctx) {
    (void)img;
    void *p = ndk_system_impl(name);
    if (p) return p;
    p = search_group((ndk_lib *)ctx, name);
    if (p) return p;
    if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) return NULL;
    int type = ELF64_ST_TYPE(sym->st_info);
    if (type == STT_FUNC || type == STT_NOTYPE || ndk_system_known(name)) return ndk_trap_stub(name);
    return NULL;  // an undefined data object: a real link error
}

// ---- loading -------------------------------------------------------------------------------

static ndk_lib *load_locked(const char *elf_path, char *err, size_t errlen) {
    ndk_lib *l = find_loaded(elf_path);
    if (l) return l;
    char wrapped[PATH_MAX];
    if (!wrapped_path(elf_path, wrapped, sizeof wrapped)) {
        snprintf(err, errlen, "%s: no converted image (%s.dylib); run the APK through the andropkg converter",
                 elf_path, base_name(elf_path));
        return NULL;
    }
    apk_image *img = apk_image_load(wrapped, err, errlen);
    if (!img) return NULL;
    if (atomic_load(&g_nlibs) >= 256) {
        snprintf(err, errlen, "too many native libraries");
        return NULL;
    }
    l = calloc(1, sizeof *l);
    l->img = img;
    l->path = strdup(elf_path);
    l->name = strdup(base_name(elf_path));
    int phnum = 0;
    const Elf64_Phdr *ph = apk_image_phdr(img, &phnum);
    uintptr_t base = apk_image_base(img), hi = 0;
    for (int i = 0; i < phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
    l->start = base;
    l->end = base + hi;
    // Registered before its dependencies so that cycles terminate.
    g_libs[atomic_load(&g_nlibs)] = l;
    atomic_fetch_add_explicit(&g_nlibs, 1, memory_order_release);
    g_adds++;

    size_t n = apk_image_needed_count(img);
    l->deps = calloc(n ? n : 1, sizeof *l->deps);
    for (size_t i = 0; i < n; i++) {
        const char *need = apk_image_needed(img, i);
        if (ndk_is_system_lib(need)) continue;
        char path[PATH_MAX];
        if (!locate(need, elf_path, path, sizeof path)) {
            snprintf(err, errlen, "dlopen failed: library \"%s\" needed by \"%s\" not found", need, l->name);
            return NULL;
        }
        ndk_lib *d = load_locked(path, err, errlen);
        if (!d) return NULL;
        l->deps[l->ndeps++] = d;
    }
    if (!apk_image_link(img, resolve, l, err, errlen)) return NULL;
    l->linked = true;
    ndk_log(NDK_LOG_INFO, "loaded %s (%zu deps) at %#lx", l->name, l->ndeps, (unsigned long)base);
    l->initialized = true;
    apk_image_run_init(img);  // dependencies ran theirs when they finished loading
    return l;
}

ndk_lib *ndk_load(const char *path_or_name, char *err, size_t errlen) {
    lock();
    char path[PATH_MAX];
    ndk_lib *l = NULL;
    if (locate(path_or_name, NULL, path, sizeof path)) {
        l = load_locked(path, err, errlen);
    } else {
        l = find_loaded(path_or_name);
        if (!l) snprintf(err, errlen, "dlopen failed: library \"%s\" not found", path_or_name);
    }
    unlock();
    return l;
}

void *ndk_sym(ndk_lib *lib, const char *name) { return lib ? search_group(lib, name) : NULL; }

// ---- libdl for Android code ------------------------------------------------------------------

#define BIONIC_RTLD_DEFAULT ((void *)0)
#define BIONIC_RTLD_NEXT ((void *)-1L)

typedef struct {
    uint64_t magic;  // "SYSLIB" handle for dlopen of an NDK system library
    char name[64];
} sys_handle;
static const uint64_t kSysMagic = 0x42494c5359534e41ULL;  // "ANSYSLIB"

static __thread char t_dlerror[512];
static __thread bool t_dlerror_set;

static void set_dlerror(const char *msg) {
    strlcpy(t_dlerror, msg, sizeof t_dlerror);
    t_dlerror_set = true;
}

NDK_EXPORT void *bionic_dlopen(const char *filename, int flags) {
    (void)flags;
    if (!filename) {
        static sys_handle self = {kSysMagic, "main"};
        return &self;
    }
    if (ndk_is_system_lib(base_name(filename))) {
        sys_handle *h = calloc(1, sizeof *h);
        h->magic = kSysMagic;
        strlcpy(h->name, base_name(filename), sizeof h->name);
        return h;
    }
    char err[512];
    ndk_lib *l = ndk_load(filename, err, sizeof err);
    if (!l) set_dlerror(err);
    return l;
}

NDK_EXPORT void *bionic_android_dlopen_ext(const char *filename, int flags, const void *extinfo) {
    (void)extinfo;
    return bionic_dlopen(filename, flags);
}

NDK_EXPORT void *bionic_dlsym(void *handle, const char *symbol) {
    void *p = NULL;
    if (handle == BIONIC_RTLD_DEFAULT || handle == BIONIC_RTLD_NEXT ||
        ((sys_handle *)handle)->magic == kSysMagic) {
        p = ndk_system_impl(symbol);
        if (!p && (handle == BIONIC_RTLD_DEFAULT || handle == BIONIC_RTLD_NEXT)) {
            lock();
            size_t n = atomic_load(&g_nlibs);
            for (size_t i = 0; i < n && !p; i++) p = apk_image_sym(g_libs[i]->img, symbol);
            unlock();
        }
    } else {
        p = search_group((ndk_lib *)handle, symbol);
    }
    if (!p) {
        char msg[300];
        snprintf(msg, sizeof msg, "undefined symbol: %s", symbol);
        set_dlerror(msg);
    }
    return p;
}

NDK_EXPORT void *bionic_dlvsym(void *handle, const char *symbol, const char *version) {
    (void)version;
    return bionic_dlsym(handle, symbol);
}

NDK_EXPORT int bionic_dlclose(void *handle) {
    (void)handle;  // libraries stay mapped (as with most real-world Android code)
    return 0;
}

NDK_EXPORT char *bionic_dlerror(void) {
    if (!t_dlerror_set) return NULL;
    t_dlerror_set = false;
    return t_dlerror;
}

typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} bionic_Dl_info;

NDK_EXPORT int bionic_dladdr(const void *addr, bionic_Dl_info *info) {
    uintptr_t a = (uintptr_t)addr;
    lock();
    size_t n = atomic_load(&g_nlibs);
    for (size_t i = 0; i < n; i++) {
        ndk_lib *l = g_libs[i];
        if (a < l->start || a >= l->end) continue;
        info->dli_fname = l->path;
        info->dli_fbase = (void *)l->start;
        info->dli_sname = NULL;
        info->dli_saddr = NULL;
        const Elf64_Sym *syms = apk_image_symtab(l->img);
        const char *strs = apk_image_strtab(l->img);
        size_t count = apk_image_symcount(l->img);
        uintptr_t best = 0;
        for (size_t k = 0; syms && k < count; k++) {
            if (syms[k].st_shndx == SHN_UNDEF || syms[k].st_value == 0) continue;
            uintptr_t s = l->start + syms[k].st_value;
            if (s <= a && a < s + (syms[k].st_size ? syms[k].st_size : 1) && s >= best) {
                best = s;
                info->dli_sname = strs + syms[k].st_name;
                info->dli_saddr = (void *)s;
            }
        }
        unlock();
        return 1;
    }
    unlock();
    Dl_info d;
    if (!dladdr(addr, &d)) return 0;
    info->dli_fname = d.dli_fname;
    info->dli_fbase = d.dli_fbase;
    info->dli_sname = d.dli_sname;
    info->dli_saddr = d.dli_saddr;
    return 1;
}

struct bionic_dl_phdr_info {
    uint64_t dlpi_addr;
    const char *dlpi_name;
    const Elf64_Phdr *dlpi_phdr;
    uint16_t dlpi_phnum;
    unsigned long long dlpi_adds;
    unsigned long long dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
};

NDK_EXPORT int bionic_dl_iterate_phdr(int (*cb)(struct bionic_dl_phdr_info *, size_t, void *), void *data) {
    lock();
    int r = 0;
    size_t n = atomic_load(&g_nlibs);
    for (size_t i = 0; i < n && r == 0; i++) {
        ndk_lib *l = g_libs[i];
        struct bionic_dl_phdr_info info = {0};
        int phnum = 0;
        info.dlpi_addr = apk_image_base(l->img);
        info.dlpi_name = l->path;
        info.dlpi_phdr = apk_image_phdr(l->img, &phnum);
        info.dlpi_phnum = (uint16_t)phnum;
        info.dlpi_adds = g_adds;
        r = cb(&info, sizeof info, data);
    }
    unlock();
    return r;
}

// ---- ART: System.loadLibrary of an Android ELF (runtime/patches/art.patch) -------------------

void ndk_jni_install(void);

NDK_EXPORT void *andropkg_ndk_open(const char *path_in, char **error) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, ndk_jni_install);
    // lib<x>.dylib -> lib<x>.so (prepare-native-libs.sh links the name System.mapLibraryName uses).
    char real[PATH_MAX];
    const char *path = realpath(path_in, real) ? real : path_in;
    char dir[PATH_MAX];
    strlcpy(dir, path, sizeof dir);
    ndk_set_library_dir(dirname(dir));
    char err[1024] = "";
    ndk_lib *l = ndk_load(path, err, sizeof err);
    if (!l) {
        ndk_log(NDK_LOG_ERROR, "%s", err);
        if (error) *error = strdup(err);
    }
    return l;
}

NDK_EXPORT void *andropkg_ndk_sym(void *handle, const char *name) { return ndk_sym(handle, name); }

NDK_EXPORT void andropkg_ndk_close(void *handle) { (void)handle; }
