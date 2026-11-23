// andropkg NDK layer — pre-signed trampolines for libffi closures (ndk-libs/ffi_closures.c).
//
// Linux libffi writes each closure's trampoline into memory it then maps executable, which
// iOS forbids. Here every trampoline exists ahead of time, signed with this library: trampoline
// i loads closure i (its slot) into x17 and jumps to the address libffi stored at closure+16,
// exactly what libffi's own AArch64 trampoline does (ffi_closure_SYSV takes the closure in x17).
#define ANDROPKG_FFI_TRAMPOLINES 4096

    .text
    .p2align 4
    .globl _andropkg_ffi_tramps
_andropkg_ffi_tramps:
    .set slot, 0
    .rept ANDROPKG_FFI_TRAMPOLINES
    adrp    x17, _andropkg_ffi_slots+(8*slot)@PAGE
    ldr     x17, [x17, _andropkg_ffi_slots+(8*slot)@PAGEOFF]
    ldr     x16, [x17, #16]
    br      x16
    .set slot, slot + 1
    .endr

    .globl _andropkg_ffi_slots
    .zerofill __DATA, __bss, _andropkg_ffi_slots, 8 * ANDROPKG_FFI_TRAMPOLINES, 3
