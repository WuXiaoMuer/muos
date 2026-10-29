; isr_stubs.s - ISR/IRQ assembly stubs for MuOS (with scheduler)
; NASM syntax, 32-bit ELF

bits 32

extern isr_handler
extern irq_handler
extern syscall_handler
extern user_kernel_esp_save

; ---------------------------------------------------------------------------
; Common ISR handler (exceptions)
; ---------------------------------------------------------------------------
isr_common:
    cli
    cld
    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call isr_handler
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret

; ---------------------------------------------------------------------------
; Common IRQ handler (with scheduler check)
; ---------------------------------------------------------------------------
irq_common:
    cli
    cld
    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call irq_handler
    add esp, 4
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret

; ---------------------------------------------------------------------------
; int 0x80 syscall entry (DPL=3 gate, see idt.c)
; ---------------------------------------------------------------------------
global syscall_entry
syscall_entry:
    cli
    push 0              ; no CPU error code
    push 0x80           ; vector number
    jmp syscall_common

syscall_common:
    cli
    cld
    pusha
    push ds
    push es
    push fs
    push gs

    ; Kernel segments for the C handler; user values were saved on the stack.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call syscall_handler
    add esp, 4

    ; sys_exit/user fault never returns here (see user_return_to_kernel),
    ; so reaching this point means a normal return to Ring3.
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret

; ---------------------------------------------------------------------------
; Ring3 entry/exit helpers
;
; user_enter(entry, user_stack):
;   saves the callee-saved registers + ESP of the kernel caller so
;   user_return_to_kernel can resume it after exit/fault, then builds a
;   Ring3 iret frame and drops to CPL3.
;
; user_return_to_kernel():
;   called from C while on the TSS Ring0 stack; discards that stack and
;   resumes the kernel caller saved by user_enter. Never returns.
; ---------------------------------------------------------------------------
global user_enter
global user_return_to_kernel

user_enter:
    cli
    mov ecx, [esp + 8]      ; user stack pointer
    mov edx, [esp + 4]      ; entry point
    push ebp
    push ebx
    push esi
    push edi
    mov [user_kernel_esp_save], esp

    ; Ring3 data selectors must be live before iret: DS/ES/FS/GS are not
    ; reloaded by iret and a DPL0 selector would fault on first access.
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push 0x23               ; SS  (user data, RPL3)
    push ecx                ; ESP (user stack)
    push 0x202              ; EFLAGS (IF=1, IOPL=0)
    push 0x1B               ; CS  (user code, RPL3)
    push edx                ; EIP
    iret

user_return_to_kernel:
    cli
    cld
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov esp, [user_kernel_esp_save]
    pop edi
    pop esi
    pop ebx
    pop ebp
    sti
    ret

; ---------------------------------------------------------------------------
; Ignore stub: silently dismiss unexpected vectors (0x30-0xFF).
; Receives a plain interrupt frame (no error code / pushed vector).
; Vector 0x80 is reserved for the future int 0x80 syscall interface.
; ---------------------------------------------------------------------------
global isr_ignore
isr_ignore:
    iret

; ---------------------------------------------------------------------------
; Macros
; ---------------------------------------------------------------------------
section .text
%macro ISR_NOERR 1
global isr%1
isr%1:
    cli
    push 0
    push %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    cli
    push %1
    jmp isr_common
%endmacro

%macro IRQ_STUB 2
global irq%1
irq%1:
    cli
    push 0
    push %2
    jmp irq_common
%endmacro

; --- Exceptions ---
ISR_NOERR  0    ; Division by zero
ISR_NOERR  1    ; Debug
ISR_NOERR  2    ; NMI
ISR_NOERR  3    ; Breakpoint
ISR_NOERR  4    ; Overflow
ISR_NOERR  5    ; Bound range exceeded
ISR_NOERR  6    ; Invalid opcode
ISR_NOERR  7    ; Device not available
ISR_ERR    8    ; Double fault
ISR_NOERR  9    ; Coprocessor segment overrun
ISR_ERR    10   ; Invalid TSS
ISR_ERR    11   ; Segment not present
ISR_ERR    12   ; Stack fault
ISR_ERR    13   ; General protection fault
ISR_ERR    14   ; Page fault
ISR_NOERR  15   ; Reserved
ISR_NOERR  16   ; x87 FPU error
ISR_ERR    17   ; Alignment check
ISR_NOERR  18   ; Machine check
ISR_NOERR  19   ; SIMD exception
ISR_NOERR  20   ; Virtualization exception
ISR_NOERR  21   ; Control protection exception
ISR_NOERR  22   ; Reserved
ISR_NOERR  23   ; Reserved
ISR_NOERR  24   ; Reserved
ISR_NOERR  25   ; Reserved
ISR_NOERR  26   ; Reserved
ISR_NOERR  27   ; Reserved
ISR_NOERR  28   ; Hypervisor injection
ISR_NOERR  29   ; VMM communication
ISR_NOERR  30   ; Security exception
ISR_NOERR  31   ; Reserved

; --- IRQs ---
IRQ_STUB  0,  0x20
IRQ_STUB  1,  0x21
IRQ_STUB  2,  0x22
IRQ_STUB  3,  0x23
IRQ_STUB  4,  0x24
IRQ_STUB  5,  0x25
IRQ_STUB  6,  0x26
IRQ_STUB  7,  0x27
IRQ_STUB  8,  0x28
IRQ_STUB  9,  0x29
IRQ_STUB  10, 0x2A
IRQ_STUB  11, 0x2B
IRQ_STUB  12, 0x2C
IRQ_STUB  13, 0x2D
IRQ_STUB  14, 0x2E
IRQ_STUB  15, 0x2F
