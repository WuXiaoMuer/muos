; syscall6.s - 6-argument Linux i386 syscall helper for Ring3 code.
; int muos_pselect6(int nfds, unsigned* r, unsigned* w, unsigned* e,
;                   unsigned* ts, unsigned* sig);
; The 6th argument travels in ebp, which C inline asm cannot reserve while
; also pinning ebx/ecx/edx/esi/edi, so this single call is assembly.

bits 32
section .text
global muos_pselect6
muos_pselect6:
    push ebp
    push ebx
    push esi
    push edi
    mov eax, 308                ; SYS_pselect6
    mov ebx, [esp + 20]         ; nfds
    mov ecx, [esp + 24]         ; readfds
    mov edx, [esp + 28]         ; writefds
    mov esi, [esp + 32]         ; exceptfds
    mov edi, [esp + 36]         ; timeout (ignored by MuOS)
    mov ebp, [esp + 40]         ; sigmask (ignored by MuOS)
    int 0x80
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
