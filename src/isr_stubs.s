; isr_stubs.s - ISR/IRQ assembly stubs for MuOS (with scheduler)
; NASM syntax, 32-bit ELF

bits 32

extern isr_handler
extern irq_handler
extern task_schedule
extern current_task_esp

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
.no_switch:
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret

; ---------------------------------------------------------------------------
; Scheduler variables (kept for symbol compatibility; not actually used)
; ---------------------------------------------------------------------------
section .bss
global need_reschedule
need_reschedule: resb 1

global current_task_ptr
current_task_ptr: resd 1

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
