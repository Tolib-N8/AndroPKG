// andropkg_svc_entry — emulates the Linux arm64 syscall trap boundary.
//
// Reached from a converter trampoline:
//     stp x16, x30, [sp, #-16]!   ; trampoline saves x16 + x30
//     adrp/ldr x16, <slot>        ; slot holds &andropkg_svc_entry
//     blr x16
//     ldp x16, x30, [sp], #16
//     b   <site + 4>
//
// A real kernel preserves every register except x0, so we save everything the C ABI
// lets andropkg_syscall clobber: x1-x15, x17, NZCV, FPSR/FPCR and all of q0-q31.

    .text
    .globl _andropkg_svc_entry
    .p2align 2
_andropkg_svc_entry:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    sub     sp, sp, #(144 + 512)

    stp     x1, x2, [sp, #0]
    stp     x3, x4, [sp, #16]
    stp     x5, x6, [sp, #32]
    stp     x7, x8, [sp, #48]
    stp     x9, x10, [sp, #64]
    stp     x11, x12, [sp, #80]
    stp     x13, x14, [sp, #96]
    stp     x15, x17, [sp, #112]
    mrs     x9, nzcv
    mrs     x10, fpsr
    stp     x9, x10, [sp, #128]
    add     x9, sp, #144
    stp     q0, q1, [x9, #0]
    stp     q2, q3, [x9, #32]
    stp     q4, q5, [x9, #64]
    stp     q6, q7, [x9, #96]
    stp     q8, q9, [x9, #128]
    stp     q10, q11, [x9, #160]
    stp     q12, q13, [x9, #192]
    stp     q14, q15, [x9, #224]
    stp     q16, q17, [x9, #256]
    stp     q18, q19, [x9, #288]
    stp     q20, q21, [x9, #320]
    stp     q22, q23, [x9, #352]
    stp     q24, q25, [x9, #384]
    stp     q26, q27, [x9, #416]
    stp     q28, q29, [x9, #448]
    stp     q30, q31, [x9, #480]

    // andropkg_syscall(nr = x8, a0..a5 = x0..x5)
    mov     x6, x5
    mov     x5, x4
    mov     x4, x3
    mov     x3, x2
    mov     x2, x1
    mov     x1, x0
    mov     x0, x8
    bl      _andropkg_syscall

    add     x9, sp, #144
    ldp     q0, q1, [x9, #0]
    ldp     q2, q3, [x9, #32]
    ldp     q4, q5, [x9, #64]
    ldp     q6, q7, [x9, #96]
    ldp     q8, q9, [x9, #128]
    ldp     q10, q11, [x9, #160]
    ldp     q12, q13, [x9, #192]
    ldp     q14, q15, [x9, #224]
    ldp     q16, q17, [x9, #256]
    ldp     q18, q19, [x9, #288]
    ldp     q20, q21, [x9, #320]
    ldp     q22, q23, [x9, #352]
    ldp     q24, q25, [x9, #384]
    ldp     q26, q27, [x9, #416]
    ldp     q28, q29, [x9, #448]
    ldp     q30, q31, [x9, #480]
    ldp     x9, x10, [sp, #128]
    msr     nzcv, x9
    msr     fpsr, x10
    ldp     x1, x2, [sp, #0]
    ldp     x3, x4, [sp, #16]
    ldp     x5, x6, [sp, #32]
    ldp     x7, x8, [sp, #48]
    ldp     x9, x10, [sp, #64]
    ldp     x11, x12, [sp, #80]
    ldp     x13, x14, [sp, #96]
    ldp     x15, x17, [sp, #112]

    add     sp, sp, #(144 + 512)
    ldp     x29, x30, [sp], #16
    ret

// andropkg_tp_slowpath — a thread with no bionic TCB yet read its thread pointer.
//
// Reached from a converter tp-read trampoline (stp x16, x30 already done by it):
//     adrp/ldr x16, <slot 2>;  blr x16      -> returns the TP in x16
// Every other register is preserved (the trampoline sits inside arbitrary compiled code).
    .globl _andropkg_tp_slowpath
    .p2align 2
_andropkg_tp_slowpath:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    sub     sp, sp, #(160 + 512)
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    mrs     x9, nzcv
    mrs     x10, fpsr
    stp     x17, x9, [sp, #128]
    str     x10, [sp, #144]
    add     x9, sp, #160
    stp     q0, q1, [x9, #0]
    stp     q2, q3, [x9, #32]
    stp     q4, q5, [x9, #64]
    stp     q6, q7, [x9, #96]
    stp     q8, q9, [x9, #128]
    stp     q10, q11, [x9, #160]
    stp     q12, q13, [x9, #192]
    stp     q14, q15, [x9, #224]
    stp     q16, q17, [x9, #256]
    stp     q18, q19, [x9, #288]
    stp     q20, q21, [x9, #320]
    stp     q22, q23, [x9, #352]
    stp     q24, q25, [x9, #384]
    stp     q26, q27, [x9, #416]
    stp     q28, q29, [x9, #448]
    stp     q30, q31, [x9, #480]

    bl      _andropkg_tls_attach_thread
    mov     x16, x0

    add     x9, sp, #160
    ldp     q0, q1, [x9, #0]
    ldp     q2, q3, [x9, #32]
    ldp     q4, q5, [x9, #64]
    ldp     q6, q7, [x9, #96]
    ldp     q8, q9, [x9, #128]
    ldp     q10, q11, [x9, #160]
    ldp     q12, q13, [x9, #192]
    ldp     q14, q15, [x9, #224]
    ldp     q16, q17, [x9, #256]
    ldp     q18, q19, [x9, #288]
    ldp     q20, q21, [x9, #320]
    ldp     q22, q23, [x9, #352]
    ldp     q24, q25, [x9, #384]
    ldp     q26, q27, [x9, #416]
    ldp     q28, q29, [x9, #448]
    ldp     q30, q31, [x9, #480]
    ldp     x17, x9, [sp, #128]
    ldr     x10, [sp, #144]
    msr     nzcv, x9
    msr     fpsr, x10
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    add     sp, sp, #(160 + 512)
    ldp     x29, x30, [sp], #16
    ret
