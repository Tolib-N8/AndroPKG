// andropkg ELF loader: links Android arm64 ELF shared objects that the converter has
// wrapped into signed Mach-O dylibs (see converter/Sources/AndroPkgCore/ElfWrap.swift).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <stdbool.h>

#include "andropkg/elf.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Resolves an undefined symbol (e.g. "malloc", "__android_log_print") to a host
/// implementation. Return NULL if unknown.
typedef void *(*apk_resolver_fn)(const char *name, void *ctx);

typedef struct apk_image apk_image;

/// Resolver that also sees the referencing image and the symbol (type, binding).
typedef void *(*apk_resolver_ex_fn)(const apk_image *img, const char *name, const Elf64_Sym *sym, void *ctx);

/// Mirrors WrapInfo in ElfWrap.swift (section __TEXT,__elfwrap).
typedef struct {
    char magic[8];  // "APKELF01"
    uint32_t version, flags;
    uint64_t elf_vm_offset;   // ELF vaddr 0, relative to the Mach-O header
    uint64_t elf_vm_size;
    uint64_t tramp_vm_offset; // 0 if nothing was rewritten
    uint64_t data_vm_offset;  // [0] svc dispatcher, [1] TSD byte offset of the bionic TP
    uint32_t svc_count, tp_read_count, tp_write_count, reserved;
    // version >= 3: an ELF linked for 4 KB pages, whose writable segments were moved up by
    // `shift` (a multiple of 4 KB) to 16 KB pages of their own; ELF vaddrs >= split_vaddr are at +shift.
    uint64_t split_vaddr;
    uint64_t shift;
} apk_wrap_info;

/// dlopen()s the wrapped dylib, applies ELF relocations, and runs DT_INIT/init_array.
/// Returns NULL on failure with a message in `err`.
apk_image *apk_image_open(const char *wrapped_dylib_path, apk_resolver_fn resolver, void *ctx,
                          char *err, size_t errlen);

/// Two-phase loading (what a linker with DT_NEEDED needs):
/// dlopen()s and parses the wrapped image — no relocations yet.
apk_image *apk_image_load(const char *wrapped_dylib_path, char *err, size_t errlen);
/// Applies relocations, resolving undefined symbols through `resolver`.
bool apk_image_link(apk_image *img, apk_resolver_ex_fn resolver, void *ctx, char *err, size_t errlen);
void apk_image_run_init(const apk_image *img);
void apk_image_run_fini(const apk_image *img);
size_t apk_image_needed_count(const apk_image *img);
const char *apk_image_needed(const apk_image *img, size_t i);
const char *apk_image_path(const apk_image *img);
const Elf64_Phdr *apk_image_phdr(const apk_image *img, int *phnum);
const Elf64_Sym *apk_image_symtab(const apk_image *img);
const char *apk_image_strtab(const apk_image *img);
size_t apk_image_symcount(const apk_image *img);

/// Looks up an exported (defined, global/weak) ELF dynamic symbol.
void *apk_image_sym(const apk_image *img, const char *name);

uintptr_t apk_image_base(const apk_image *img);
const char *apk_image_soname(const apk_image *img);
const apk_wrap_info *apk_image_wrap_info(const apk_image *img);

#ifdef __cplusplus
}
#endif
