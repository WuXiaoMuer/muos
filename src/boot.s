; boot.s - MuOS Multiboot1 启动入口
; QEMU -kernel 原生支持 Multiboot1 ELF
; CPU 进入时已处于 32 位保护模式

section .multiboot
align 4
    dd 0x1BADB002           ; Magic number (Multiboot1)
    dd 0x00000003           ; Flags: bit0=page align, bit1=memory info
    dd -(0x1BADB002 + 0x00000003)  ; Checksum

section .text
bits 32
global start
extern kernel_main

start:
    cli                     ; 关中断

    mov esp, stack_top      ; 设置 16KB 内核栈

    ; 传递 Multiboot 信息给 kernel_main
    push ebx                ; multiboot_info 指针
    push eax                ; multiboot magic number
    call kernel_main

    ; kernel_main 不应返回，如果返回则停机
    cli
.halt:
    hlt
    jmp .halt

section .bss
align 16
stack_bottom:
    resb 16384              ; 16KB 栈空间
stack_top:
