// andropkg NDK layer — setjmp/longjmp for Android code.
//
// setjmp returns twice, so it can't be wrapped by a C function: these are tail branches into
// Darwin's own, which then save the Android caller's registers (LR, SP, callee-saved). Darwin's
// sigjmp_buf (49 ints, 196 bytes) fits in bionic's jmp_buf (32 longs, 256 bytes), and the buffer
// is opaque to the app; both halves of a pair are Darwin's.
    .text
    .p2align 2
    .globl _bionic_setjmp
_bionic_setjmp:
    b _setjmp
    .globl _bionic__setjmp
_bionic__setjmp:
    b __setjmp
    .globl _bionic_sigsetjmp
_bionic_sigsetjmp:
    b _sigsetjmp
    .globl _bionic_longjmp
_bionic_longjmp:
    b _longjmp
    .globl _bionic__longjmp
_bionic__longjmp:
    b __longjmp
    .globl _bionic_siglongjmp
_bionic_siglongjmp:
    b _siglongjmp
