; boot.s - MuOS Multiboot1 启动入口(higher-half)
; QEMU -kernel 原生支持 Multiboot1 ELF;CPU 进入时已是 32 位保护模式。
;
; 布局说明:入口 start 必须是镜像第一个字节(ISO 路径 boot_cd.s 把 flat
; binary 拷到 0x100000 后直接跳过去)。内核链接在 0xC0100000,物理装载
; 在 0x100000;这里先建一个最小自举页目录:
;   PDE[0..1]   identity 映射物理 0..8MB   (执行低地址代码 + 自举数据)
;   PDE[768]    0xC0000000 -> 物理 0..4MB  (跳到 higher-half 继续执行)
; 打开分页后用绝对地址跳到高半区,再调用 kernel_main。

bits 32

KERNEL_VBASE equ 0xC0000000

section .text
global start
extern kernel_main

start:
    jmp real_start

align 4
    dd 0x1BADB002           ; Magic number (Multiboot1)
    dd 0x00000003           ; Flags: bit0=page align, bit1=memory info
    dd -(0x1BADB002 + 0x00000003)  ; Checksum

real_start:
    cli
    mov esi, eax            ; 保存 multiboot magic(esi 不会被下面的循环用到)

    ; --- 清空自举页目录 ---
    mov edi, boot_pd - KERNEL_VBASE
    xor eax, eax
    mov ecx, 1024
    rep stosd

    ; --- PT0: 物理 0..4MB ---
    mov edi, boot_pt0 - KERNEL_VBASE
    mov eax, 0x00000003     ; present | writable
    mov ecx, 1024
.fill0:
    mov [edi], eax
    add eax, 0x1000
    add edi, 4
    loop .fill0

    ; --- PT1: 物理 4..8MB ---
    mov edi, boot_pt1 - KERNEL_VBASE
    mov eax, 0x00400003
    mov ecx, 1024
.fill1:
    mov [edi], eax
    add eax, 0x1000
    add edi, 4
    loop .fill1

    ; --- 页目录项 ---
    mov edi, boot_pd - KERNEL_VBASE
    mov eax, boot_pt0 - KERNEL_VBASE
    or eax, 3
    mov [edi], eax              ; PDE[0]
    mov eax, boot_pt1 - KERNEL_VBASE
    or eax, 3
    mov [edi + 4], eax          ; PDE[1]
    mov eax, boot_pt0 - KERNEL_VBASE
    or eax, 3
    mov [edi + 768*4], eax      ; PDE[768] = 0xC0000000

    ; --- 打开分页 ---
    mov eax, boot_pd - KERNEL_VBASE
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80000001          ; PG | PE
    mov cr0, eax

    ; --- 绝对跳转到 higher-half 继续 ---
    mov eax, high_entry
    jmp eax

high_entry:
    mov esp, stack_top
    push ebx                    ; multiboot_info
    push esi                    ; multiboot magic
    call kernel_main

    cli
.halt:
    hlt
    jmp .halt

section .bss
align 4096
boot_pd:  resb 4096
boot_pt0: resb 4096
boot_pt1: resb 4096

align 16
stack_bottom:
    resb 16384                  ; 16KB 内核栈
stack_top:
