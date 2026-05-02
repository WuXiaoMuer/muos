; boot.s - Multiboot1 启动入口 (QEMU -kernel 原生支持)
; QEMU Windows 版对 multiboot1 ELF 支持良好，无需 GRUB。
; CPU 进入时已处于 32 位保护模式。

; ===== Multiboot1 Header (必须 4 字节对齐) =====
section .multiboot
align 4
    dd 0x1BADB002           ; Magic number
    dd 0x00000000           ; Flags
    dd -(0x1BADB002 + 0x00000000)  ; Checksum

; ===== 代码段 =====
section .text
bits 32

global start
extern kernel_main

start:
    cli                         ; 关中断
    mov esp, stack_top          ; 设置栈顶
    call kernel_main            ; 跳转到 C 内核

    ; kernel_main 返回后永久停机
    cli
.halt:
    hlt
    jmp .halt

; ===== BSS 段：栈空间 =====
section .bss
align 16
stack_bottom:
    resb 16384                  ; 16KB 栈空间
stack_top:
