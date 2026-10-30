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

; ---------------------------------------------------------------------------
; muos_sendto(int fd, const void* buf, unsigned len, int flags,
;             const void* addr, int alen)   -> SYS_sendto (369)
; ---------------------------------------------------------------------------
global muos_sendto
muos_sendto:
    push ebp
    push ebx
    push esi
    push edi
    mov eax, 369
    mov ebx, [esp + 20]
    mov ecx, [esp + 24]
    mov edx, [esp + 28]
    mov esi, [esp + 32]
    mov edi, [esp + 36]
    mov ebp, [esp + 40]
    int 0x80
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; ---------------------------------------------------------------------------
; muos_recvfrom(int fd, void* buf, unsigned len, int flags,
;               void* addr, int* alen)        -> SYS_recvfrom (371)
; ---------------------------------------------------------------------------
global muos_recvfrom
muos_recvfrom:
    push ebp
    push ebx
    push esi
    push edi
    mov eax, 371
    mov ebx, [esp + 20]
    mov ecx, [esp + 24]
    mov edx, [esp + 28]
    mov esi, [esp + 32]
    mov edi, [esp + 36]
    mov ebp, [esp + 40]
    int 0x80
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; ---------------------------------------------------------------------------
; muos_thread_start(void* fn, void* arg, void* stack_top)
; clone(CLONE_VM|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD) with a prepared
; child stack. The child resumes inside this same int 0x80 continuation
; (eax=0) on its own stack, calls fn(arg), then exit(0).
; ---------------------------------------------------------------------------
global muos_thread_start
muos_thread_start:
    push ebp
    push ebx
    push esi
    push edi
    mov ebp, [esp + 20]         ; fn (callee-saved: survives int 0x80)
    mov esi, [esp + 24]         ; arg
    mov eax, [esp + 28]         ; stack_top
    and eax, 0xFFFFFFF0
    sub eax, 4
    mov [eax], esi              ; child stack: [esp] = arg
    mov ebx, 0x00010D00         ; VM|FILES|SIGHAND|THREAD
    mov ecx, eax                ; child stack pointer
    xor edx, edx                ; ptid
    xor esi, esi                ; tls
    xor edi, edi                ; ctid
    mov eax, 120                ; SYS_clone
    int 0x80

    test eax, eax
    jz .child

    pop edi
    pop esi
    pop ebx
    pop ebp
    ret                         ; parent: pid
.child:
    mov eax, [esp]              ; arg
    push eax
    call ebp                    ; fn(arg)
    xor eax, eax                ; exit(0) if the thread returns
    push eax
    mov eax, 1
    int 0x80
.hang:
    jmp .hang
