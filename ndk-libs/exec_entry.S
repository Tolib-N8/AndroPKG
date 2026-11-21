// andropkg_exe_enter(entry, sp): enter an ELF executable's _start on the stack it expects
// (sp -> argc, argv..., envp..., auxv...). Never returns: exit() longjmps out (exec.c).
    .text
    .globl _andropkg_exe_enter
    .p2align 2
_andropkg_exe_enter:
    mov     sp, x1
    mov     x29, #0
    mov     x30, #0
    br      x0
