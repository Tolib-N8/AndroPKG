// andropkg ELF loader — links Android arm64 ELF images that dyld has already mapped
// (via the converter's Mach-O wrapper) and therefore are already code-signed.
//
// dyld does: mapping, page protections, code-signature validation.
// We do:     ELF relocations (RELA, APS2-packed RELA, RELR), symbol resolution,
//            svc-trampoline dispatcher install, DT_INIT / init_array.
#include "andropkg/loader.h"
#include "andropkg/elf.h"
#include "andropkg/syscall.h"
#include "andropkg/tls.h"

#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct apk_image {
    char *path;
    void *dl_handle;
    uintptr_t base;  // runtime address of ELF vaddr 0
    const apk_wrap_info *info;
    const Elf64_Dyn *dynamic;
    const Elf64_Sym *symtab;
    const char *strtab;
    const uint32_t *gnu_hash;
    const uint32_t *sysv_hash;
    const char *soname;
    apk_resolver_fn resolver;
    apk_resolver_ex_fn resolver_ex;
    void *resolver_ctx;
    // Parsed dynamic section (apk_image_load), used by apk_image_link / init.
    uint64_t rela, relasz, jmprel, pltrelsz, relr, relrsz, aps2, aps2sz;
    uint64_t init, init_array, init_arraysz, fini, fini_array, fini_arraysz;
    const Elf64_Phdr *phdr;
    int phnum;
    const char *needed[64];
    size_t nneeded;
    bool linked;
};

static void set_err(char *err, size_t len, const char *fmt, ...) {
    if (!err || !len) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, len, fmt, ap);
    va_end(ap);
}

// ---------------------------------------------------------------------------------------
// Locating the wrapped image after dlopen.

static const struct mach_header_64 *find_loaded_header(const char *path) {
    char want[PATH_MAX], have[PATH_MAX];
    if (!realpath(path, want)) return NULL;
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
        const char *name = _dyld_get_image_name(i);
        if (name && realpath(name, have) && strcmp(want, have) == 0)
            return (const struct mach_header_64 *)_dyld_get_image_header(i);
    }
    return NULL;
}

// ---------------------------------------------------------------------------------------
// Symbol lookup.

static uint32_t gnu_hash(const char *s) {
    uint32_t h = 5381;
    for (; *s; s++) h = h * 33 + (uint8_t)*s;
    return h;
}

static uint32_t sysv_hash(const char *s) {
    uint32_t h = 0, g;
    while (*s) {
        h = (h << 4) + (uint8_t)*s++;
        g = h & 0xf0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static bool sym_is_export(const Elf64_Sym *s) {
    int bind = ELF64_ST_BIND(s->st_info);
    return s->st_shndx != SHN_UNDEF && (bind == STB_GLOBAL || bind == STB_WEAK);
}

static const Elf64_Sym *lookup_gnu(const apk_image *img, const char *name) {
    const uint32_t *t = img->gnu_hash;
    uint32_t nbuckets = t[0], symoffset = t[1], bloom_size = t[2], bloom_shift = t[3];
    const uint64_t *bloom = (const uint64_t *)(t + 4);
    const uint32_t *buckets = (const uint32_t *)(bloom + bloom_size);
    const uint32_t *chain = buckets + nbuckets;

    uint32_t h = gnu_hash(name);
    uint64_t word = bloom[(h / 64) % bloom_size];
    uint64_t mask = (1ull << (h % 64)) | (1ull << ((h >> bloom_shift) % 64));
    if ((word & mask) != mask) return NULL;

    uint32_t idx = buckets[h % nbuckets];
    if (idx < symoffset) return NULL;
    for (;; idx++) {
        uint32_t h2 = chain[idx - symoffset];
        const Elf64_Sym *s = &img->symtab[idx];
        if ((h | 1) == (h2 | 1) && strcmp(name, img->strtab + s->st_name) == 0 && sym_is_export(s))
            return s;
        if (h2 & 1) return NULL;
    }
}

static const Elf64_Sym *lookup_sysv(const apk_image *img, const char *name) {
    const uint32_t *t = img->sysv_hash;
    uint32_t nbucket = t[0];
    const uint32_t *bucket = t + 2, *chain = bucket + nbucket;
    for (uint32_t i = bucket[sysv_hash(name) % nbucket]; i != 0; i = chain[i]) {
        const Elf64_Sym *s = &img->symtab[i];
        if (strcmp(name, img->strtab + s->st_name) == 0 && sym_is_export(s)) return s;
    }
    return NULL;
}

static const Elf64_Sym *lookup_export(const apk_image *img, const char *name) {
    if (img->gnu_hash) return lookup_gnu(img, name);
    if (img->sysv_hash) return lookup_sysv(img, name);
    return NULL;
}

void *apk_image_sym(const apk_image *img, const char *name) {
    const Elf64_Sym *s = lookup_export(img, name);
    if (!s || ELF64_ST_TYPE(s->st_info) == STT_TLS) return NULL;
    return (void *)(img->base + s->st_value);
}

// Resolves the symbol referenced by a relocation. Defined symbols bind to this image
// (like -Bsymbolic); undefined ones go to the host resolver (shim libc, NDK libs, ...).
static bool resolve_reloc_sym(const apk_image *img, uint32_t symidx, uintptr_t *out,
                              char *err, size_t errlen) {
    if (symidx == 0) { *out = 0; return true; }
    const Elf64_Sym *s = &img->symtab[symidx];
    const char *name = img->strtab + s->st_name;
    if (s->st_shndx != SHN_UNDEF) {
        *out = img->base + s->st_value;
        return true;
    }
    void *p = img->resolver_ex ? img->resolver_ex(img, name, s, img->resolver_ctx)
              : img->resolver  ? img->resolver(name, img->resolver_ctx)
                               : NULL;
    if (!p && ELF64_ST_BIND(s->st_info) != STB_WEAK) {
        set_err(err, errlen, "%s: undefined symbol \"%s\"", img->soname, name);
        return false;
    }
    *out = (uintptr_t)p;
    return true;
}

// ---------------------------------------------------------------------------------------
// Relocations.

static bool apply_rela(const apk_image *img, const Elf64_Rela *r, char *err, size_t errlen) {
    uint32_t type = ELF64_R_TYPE(r->r_info);
    uint64_t *where = (uint64_t *)(img->base + r->r_offset);
    uintptr_t s;
    switch (type) {
    case R_AARCH64_NONE:
        return true;
    case R_AARCH64_RELATIVE:
        *where = img->base + r->r_addend;
        return true;
    case R_AARCH64_ABS64:
    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_JUMP_SLOT:
        if (!resolve_reloc_sym(img, ELF64_R_SYM(r->r_info), &s, err, errlen)) return false;
        *where = s + r->r_addend;
        return true;
    case R_AARCH64_IRELATIVE:
        *where = ((uintptr_t (*)(void))(img->base + r->r_addend))();
        return true;
    default:
        // TODO(M4): TLS_TPREL64 / TLSDESC once the bionic TLS layout spike lands.
        set_err(err, errlen, "%s: unsupported relocation type %u at 0x%llx", img->soname, type,
                (unsigned long long)r->r_offset);
        return false;
    }
}

static bool apply_rela_table(const apk_image *img, const Elf64_Rela *t, size_t bytes, char *err,
                             size_t errlen) {
    for (size_t i = 0; i < bytes / sizeof(Elf64_Rela); i++)
        if (!apply_rela(img, &t[i], err, errlen)) return false;
    return true;
}

static int64_t sleb128(const uint8_t **p, const uint8_t *end, bool *ok) {
    int64_t v = 0;
    unsigned shift = 0;
    uint8_t b;
    do {
        if (*p >= end || shift >= 64) { *ok = false; return 0; }
        b = *(*p)++;
        v |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) v |= -(INT64_C(1) << shift);
    return v;
}

// Android "APS2" packed relocations (lld --pack-dyn-relocs=android; clang default for API>=28).
static bool apply_aps2(const apk_image *img, const uint8_t *p, size_t bytes, char *err,
                       size_t errlen) {
    enum { BY_INFO = 1, BY_OFFSET_DELTA = 2, BY_ADDEND = 4, HAS_ADDEND = 8 };
    const uint8_t *end = p + bytes;
    if (bytes < 4 || memcmp(p, "APS2", 4) != 0) {
        set_err(err, errlen, "%s: bad APS2 packed relocation header", img->soname);
        return false;
    }
    p += 4;
    bool ok = true;
    int64_t remaining = sleb128(&p, end, &ok);
    Elf64_Rela r = {.r_offset = (uint64_t)sleb128(&p, end, &ok)};
    while (ok && remaining > 0) {
        int64_t group_size = sleb128(&p, end, &ok);
        int64_t flags = sleb128(&p, end, &ok);
        int64_t offset_delta = 0;
        if (flags & BY_OFFSET_DELTA) offset_delta = sleb128(&p, end, &ok);
        if (flags & BY_INFO) r.r_info = (uint64_t)sleb128(&p, end, &ok);
        if ((flags & HAS_ADDEND) && (flags & BY_ADDEND)) r.r_addend += sleb128(&p, end, &ok);
        else if (!(flags & HAS_ADDEND)) r.r_addend = 0;
        if (group_size <= 0 || group_size > remaining) ok = false;
        for (int64_t i = 0; ok && i < group_size; i++) {
            r.r_offset += (flags & BY_OFFSET_DELTA) ? offset_delta : sleb128(&p, end, &ok);
            if (!(flags & BY_INFO)) r.r_info = (uint64_t)sleb128(&p, end, &ok);
            if ((flags & HAS_ADDEND) && !(flags & BY_ADDEND)) r.r_addend += sleb128(&p, end, &ok);
            if (ok && !apply_rela(img, &r, err, errlen)) return false;
        }
        remaining -= group_size;
    }
    if (!ok) set_err(err, errlen, "%s: malformed APS2 packed relocations", img->soname);
    return ok;
}

static void apply_relr(const apk_image *img, const uint64_t *t, size_t bytes) {
    uint64_t *where = NULL;
    for (size_t i = 0; i < bytes / sizeof(uint64_t); i++) {
        uint64_t e = t[i];
        if ((e & 1) == 0) {
            where = (uint64_t *)(img->base + e);
            *where++ += img->base;
        } else {
            for (unsigned bit = 0; (e >>= 1) != 0; bit++)
                if (e & 1) where[bit] += img->base;
            where += 63;
        }
    }
}

// ---------------------------------------------------------------------------------------

apk_image *apk_image_load(const char *path, char *err, size_t errlen) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        set_err(err, errlen, "dlopen: %s", dlerror());
        return NULL;
    }
    const struct mach_header_64 *mh = find_loaded_header(path);
    unsigned long info_size = 0;
    const apk_wrap_info *info =
        mh ? (const apk_wrap_info *)getsectiondata(mh, "__TEXT", "__elfwrap", &info_size) : NULL;
    if (!info || info_size < sizeof(*info) || memcmp(info->magic, "APKELF01", 8) != 0 ||
        info->version != 2) {
        set_err(err, errlen, "%s: not an andropkg-wrapped ELF (missing __TEXT,__elfwrap)", path);
        dlclose(h);
        return NULL;
    }

    apk_image *img = calloc(1, sizeof(*img));
    img->path = strdup(path);
    img->dl_handle = h;
    img->info = info;
    img->base = (uintptr_t)mh + info->elf_vm_offset;
    img->soname = img->path;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img->base;
    img->phdr = (const Elf64_Phdr *)(img->base + eh->e_phoff);
    img->phnum = eh->e_phnum;
    for (int i = 0; i < img->phnum; i++)
        if (img->phdr[i].p_type == PT_DYNAMIC)
            img->dynamic = (const Elf64_Dyn *)(img->base + img->phdr[i].p_vaddr);
    if (!img->dynamic) {
        set_err(err, errlen, "%s: no PT_DYNAMIC", path);
        dlclose(h);
        free(img->path);
        free(img);
        return NULL;
    }

    uint64_t soname = 0;
    uint64_t needed_off[64];
    for (const Elf64_Dyn *d = img->dynamic; d->d_tag != DT_NULL; d++) {
        uint64_t v = d->d_val;
        switch (d->d_tag) {
        case DT_SYMTAB: img->symtab = (const Elf64_Sym *)(img->base + v); break;
        case DT_STRTAB: img->strtab = (const char *)(img->base + v); break;
        case DT_GNU_HASH: img->gnu_hash = (const uint32_t *)(img->base + v); break;
        case DT_HASH: img->sysv_hash = (const uint32_t *)(img->base + v); break;
        case DT_SONAME: soname = v; break;
        case DT_NEEDED: if (img->nneeded < 64) needed_off[img->nneeded++] = v; break;
        case DT_RELA: img->rela = v; break;
        case DT_RELASZ: img->relasz = v; break;
        case DT_JMPREL: img->jmprel = v; break;
        case DT_PLTRELSZ: img->pltrelsz = v; break;
        case DT_RELR: case DT_ANDROID_RELR: img->relr = v; break;
        case DT_RELRSZ: case DT_ANDROID_RELRSZ: img->relrsz = v; break;
        case DT_ANDROID_RELA: img->aps2 = v; break;
        case DT_ANDROID_RELASZ: img->aps2sz = v; break;
        case DT_INIT: img->init = v; break;
        case DT_INIT_ARRAY: img->init_array = v; break;
        case DT_INIT_ARRAYSZ: img->init_arraysz = v; break;
        case DT_FINI: img->fini = v; break;
        case DT_FINI_ARRAY: img->fini_array = v; break;
        case DT_FINI_ARRAYSZ: img->fini_arraysz = v; break;
        }
    }
    if (img->strtab && soname) img->soname = img->strtab + soname;
    for (size_t i = 0; img->strtab && i < img->nneeded; i++) img->needed[i] = img->strtab + needed_off[i];

    // Wire the instruction trampolines (see converter Patches.swift) to the shim.
    if (info->data_vm_offset) {
        void **data = (void **)((uintptr_t)mh + info->data_vm_offset);
        data[0] = (void *)andropkg_svc_entry;
        data[1] = (void *)(uintptr_t)andropkg_tls_tsd_offset();
        data[2] = (void *)andropkg_tp_slowpath;
    }
    return img;
}

bool apk_image_link(apk_image *img, apk_resolver_ex_fn resolver, void *ctx, char *err, size_t errlen) {
    if (img->linked) return true;
    img->resolver_ex = resolver;
    img->resolver_ctx = ctx;
    // Same order as the bionic linker: RELR first (no symbols), then RELA/APS2, then PLT.
    if (img->relr) apply_relr(img, (const uint64_t *)(img->base + img->relr), img->relrsz);
    if (img->rela &&
        !apply_rela_table(img, (const Elf64_Rela *)(img->base + img->rela), img->relasz, err, errlen))
        return false;
    if (img->aps2 && !apply_aps2(img, (const uint8_t *)(img->base + img->aps2), img->aps2sz, err, errlen))
        return false;
    if (img->jmprel &&
        !apply_rela_table(img, (const Elf64_Rela *)(img->base + img->jmprel), img->pltrelsz, err, errlen))
        return false;
    img->linked = true;
    return true;
}

void apk_image_run_init(const apk_image *img) {
    if (img->init) ((void (*)(void))(img->base + img->init))();
    if (img->init_array) {
        void (**fns)(void) = (void (**)(void))(img->base + img->init_array);
        for (size_t i = 0; i < img->init_arraysz / sizeof(void *); i++)
            if (fns[i] && (uintptr_t)fns[i] != UINTPTR_MAX) fns[i]();
    }
}

void apk_image_run_fini(const apk_image *img) {
    if (img->fini_array) {
        void (**fns)(void) = (void (**)(void))(img->base + img->fini_array);
        for (size_t i = img->fini_arraysz / sizeof(void *); i-- > 0;)
            if (fns[i] && (uintptr_t)fns[i] != UINTPTR_MAX) fns[i]();
    }
    if (img->fini) ((void (*)(void))(img->base + img->fini))();
}

apk_image *apk_image_open(const char *path, apk_resolver_fn resolver, void *ctx, char *err,
                          size_t errlen) {
    apk_image *img = apk_image_load(path, err, errlen);
    if (!img) return NULL;
    img->resolver = resolver;
    img->resolver_ctx = ctx;
    if (!apk_image_link(img, NULL, ctx, err, errlen)) {
        dlclose(img->dl_handle);
        free(img->path);
        free(img);
        return NULL;
    }
    apk_image_run_init(img);
    return img;
}

size_t apk_image_needed_count(const apk_image *img) { return img->nneeded; }
const char *apk_image_needed(const apk_image *img, size_t i) { return i < img->nneeded ? img->needed[i] : NULL; }
const char *apk_image_path(const apk_image *img) { return img->path; }
const Elf64_Phdr *apk_image_phdr(const apk_image *img, int *phnum) {
    *phnum = img->phnum;
    return img->phdr;
}
const Elf64_Sym *apk_image_symtab(const apk_image *img) { return img->symtab; }
const char *apk_image_strtab(const apk_image *img) { return img->strtab; }
size_t apk_image_symcount(const apk_image *img) {
    // DT_HASH nchain, or the highest index reachable through DT_GNU_HASH.
    if (img->sysv_hash) return img->sysv_hash[1];
    if (!img->gnu_hash) return 0;
    uint32_t nbucket = img->gnu_hash[0], symoffset = img->gnu_hash[1], bloom = img->gnu_hash[2];
    const uint32_t *buckets = img->gnu_hash + 4 + bloom * 2;
    const uint32_t *chains = buckets + nbucket;
    uint32_t last = 0;
    for (uint32_t b = 0; b < nbucket; b++) if (buckets[b] > last) last = buckets[b];
    if (last < symoffset) return symoffset;
    while (!(chains[last - symoffset] & 1)) last++;
    return last + 1;
}

uintptr_t apk_image_base(const apk_image *img) { return img->base; }
const char *apk_image_soname(const apk_image *img) { return img->soname; }
const apk_wrap_info *apk_image_wrap_info(const apk_image *img) { return img->info; }
