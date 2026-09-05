; boot.s - MuOS Multiboot1 启动入口
; QEMU -kernel 原生支持 Multiboot1 ELF
; CPU 进入时已处于 32 位保护模式
;
; 布局说明:入口 start 必须位于内核镜像的第一个字节——
; El Torito ISO 路径(boot_cd.s 将 flat binary 拷到 0x100000 后
; 直接跳过去)依赖这一点。Multiboot 头通过 jmp 跨过,仍位于
; 镜像前 8KB 内,满足 Multiboot1 规范对头的扫描要求。

section .text
bits 32
global start
extern kernel_main

start:
    jmp real_start

align 4
    dd 0x1BADB002           ; Magic number (Multiboot1)
    dd 0x00000003           ; Flags: bit0=page align, bit1=memory info
    dd -(0x1BADB002 + 0x00000003)  ; Checksum

real_start:
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
