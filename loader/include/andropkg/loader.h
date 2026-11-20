// andropkg ELF loader: links Android arm64 ELF shared objects that the converter has
// wrapped into signed Mach-O dylibs (see converter/Sources/AndroPkgCore/ElfWrap.swift).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Resolves an undefined symbol (e.g. "malloc", "__android_log_print") to a host
/// implementation. Return NULL if unknown.
typedef void *(*apk_resolver_fn)(const char *name, void *ctx);

typedef struct apk_image apk_image;

/// Mirrors WrapInfo in ElfWrap.swift (section __TEXT,__elfwrap).
typedef struct {
    char magic[8];  // "APKELF01"
    uint32_t version, flags;
    uint64_t elf_vm_offset;   // ELF vaddr 0, relative to the Mach-O header
    uint64_t elf_vm_size;
    uint64_t tramp_vm_offset; // 0 if nothing was rewritten
    uint64_t data_vm_offset;  // [0] svc dispatcher, [1] TSD byte offset of the bionic TP
    uint32_t svc_count, tp_read_count, tp_write_count, reserved;
} apk_wrap_info;

/// dlopen()s the wrapped dylib, applies ELF relocations, and runs DT_INIT/init_array.
/// Returns NULL on failure with a message in `err`.
apk_image *apk_image_open(const char *wrapped_dylib_path, apk_resolver_fn resolver, void *ctx,
                          char *err, size_t errlen);

/// Looks up an exported (defined, global/weak) ELF dynamic symbol.
void *apk_image_sym(const apk_image *img, const char *name);

uintptr_t apk_image_base(const apk_image *img);
const char *apk_image_soname(const apk_image *img);
const apk_wrap_info *apk_image_wrap_info(const apk_image *img);

#ifdef __cplusplus
}
#endif
