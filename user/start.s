; start.s - Ring3 C runtime entry (MuOS user app)
; The kernel builds a Linux-style stack: [argc][argv0][NULL]...[strings]
bits 32
section .text
global _start
extern main
extern muos_exit

_start:
    xor ebp, ebp
    mov eax, [esp]          ; argc
    lea ebx, [esp + 4]      ; argv

    ; Align the stack for the SysV i386 ABI before calling main:
    ; esp must be 16-byte aligned at the call instruction.
    and esp, 0xFFFFFFF0
    sub esp, 8
    push ebx                ; argv
    push eax                ; argc
    call main
    push eax
    call muos_exit
.hang:
    jmp .hang               ; unreachable (exit never returns)
