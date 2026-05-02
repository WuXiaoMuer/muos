; boot_grub.s - Multiboot2 启动入口 (备用方案)
; 需要 grub-mkrescue 制作 ISO 才能启动
; 新版 QEMU Windows 版用 -kernel 加载 multiboot2 ELF 会报错，
; 因此默认改用 boot.s（裸 ELF），保留此文件供以后制作 GRUB ISO 时使用。

section .multiboot
align 8
header_start:
    dd 0xE85250D6                           ; Multiboot2 魔数
    dd 0                                    ; 架构：0 = i386
    dd header_end - header_start            ; Header 长度
    dd -(0xE85250D6 + 0 + (header_end - header_start)) ; 校验和
    ; 结束标签 (必须)
    dw 0                                    ; type = 0
    dw 0                                    ; flags = 0
    dd 8                                    ; size = 8
header_end:

section .text
bits 32

global start
extern kernel_main

start:
    ; 设置栈顶指针
    mov esp, stack_top

    ; 调用 C 语言内核主函数
    call kernel_main

    ; kernel_main 返回后，永久停机
    cli
.halt:
    hlt
    jmp .halt

section .bss
align 16
stack_bottom:
    resb 16384      ; 16KB 栈空间
stack_top:
