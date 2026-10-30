; ld_start.s - entry stub for ld-mu.so
; The kernel enters the interpreter with the program's original Linux-style
; stack: [argc][argv...][NULL][envp...][NULL][auxv pairs][AT_NULL].
bits 32
section .text
global _start
extern ld_main

_start:
    xor ebp, ebp
    mov eax, esp            ; pointer to argc (the original stack)
    push eax
    call ld_main            ; hidden symbol: resolved at link time
    ud2
