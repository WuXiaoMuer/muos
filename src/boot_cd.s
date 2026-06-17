; boot_cd.s - Minimal El Torito bootloader for MuOS (no GRUB needed)
; Loaded by BIOS via El Torito "no emulation", loads flat kernel binary
; and jumps to it in 32-bit protected mode with multiboot info.

bits 16
org 0x7C00

start:
    ; Jump over El Torito Boot Info Table (bytes 8-63)
    ; pycdlib writes bi_pvd/bi_file/bi_length/bi_csum here.
    jmp short real_start
    nop

    ; Reserve 56 bytes for Boot Info Table (offset 8-63)
    times 64 - ($ - $$) db 0

real_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, 0x7C00

    ; Save boot drive (DL from BIOS)
    mov [boot_drive], dl

    ; Enable A20 gate (fast method)
    in al, 0x92
    or al, 2
    out 0x92, al

    ; Load GDT
    lgdt [gdtr]

    ; Switch to protected mode
    mov eax, cr0
    or al, 1
    mov cr0, eax

    ; Far jump to flush prefetch queue and enter 32-bit mode
    jmp 0x08:pmode_entry

bits 32
pmode_entry:
    ; Set up data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    ; Copy kernel flat binary from after bootloader to 1MB
    ; Kernel starts at 0x7E00 (right after boot sector + padding)
    ; Kernel size is at a known offset
    mov esi, kernel_start
    mov edi, 0x100000          ; 1MB, where the kernel expects to be loaded
    mov ecx, [kernel_size_ptr]
    rep movsb

    ; Set up multiboot info for kernel_main(magic, mb_info)
    ; EAX = 0x2BADB002 (multiboot magic)
    ; EBX = 0 (no multiboot info struct, kernel handles this)
    mov eax, 0x2BADB002
    xor ebx, ebx

    ; Jump to kernel entry point
    jmp 0x08:0x100000

    ; We never get here

; ── GDT ─────────────────────────────────────────────────────
align 8
gdt:
    dq 0                       ; null descriptor
    dq 0x00CF9A000000FFFF      ; 32-bit code, ring 0, 0-4GB
    dq 0x00CF92000000FFFF      ; 32-bit data, ring 0, 0-4GB
gdt_end:

gdtr:
    dw gdt_end - gdt - 1
    dd gdt

; ── Data ─────────────────────────────────────────────────────
boot_drive: db 0

; Kernel size placeholder at fixed offset 4092 (patched by Python)
times 4092 - ($ - $$) db 0
kernel_size_ptr: dd 0

; Kernel flat binary starts here (offset 4096)
kernel_start:
