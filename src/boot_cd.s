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

    ; ── VBE: ask the BIOS for a linear framebuffer graphics mode ──
    ; Descriptor goes to 0x6F00, VBE info block to 0x7000, mode info to
    ; 0x7200. Nothing here is used by the kernel image at 0x100000.
    xor ax, ax
    mov es, ax
    mov di, 0x7000
    mov ax, 0x4F00
    mov cx, 0x5642            ; 'VB'
    mov dx, 0x4532            ; 'E2' -> VBE 2.0 info
    int 0x10
    cmp ax, 0x004F
    jne vbe_done
    cmp dword [0x7000], 0x41534556   ; 'VESA'
    jne vbe_done

    mov si, vbe_modes
vbe_try:
    mov cx, [si]
    cmp cx, 0xFFFF
    je vbe_done
    add si, 2

    mov ax, 0x4F01            ; get mode information
    mov di, 0x7200
    int 0x10
    cmp ax, 0x004F
    jne vbe_try

    mov ax, [0x7200]          ; mode attributes
    test ax, 0x0080           ; linear framebuffer supported
    jz vbe_try
    test ax, 0x0001           ; mode supported
    jz vbe_try
    mov al, [0x7200 + 0x19]   ; bits per pixel
    cmp al, 16
    jb vbe_try

    mov bx, cx
    or bx, 0x4000             ; use linear framebuffer
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne vbe_try

    ; Mode is set: publish the descriptor for the kernel.
    xor eax, eax
    mov dword [0x6F04], eax
    mov dword [0x6F08], eax
    mov dword [0x6F0C], eax
    mov dword [0x6F10], eax
    mov dword [0x6F14], eax
    mov dword [0x6F18], eax

    mov eax, [0x7200 + 0x28]  ; PhysBasePtr
    mov [0x6F04], eax
    mov ax, [0x7200 + 0x12]   ; XResolution
    mov [0x6F08], ax
    mov ax, [0x7200 + 0x14]   ; YResolution
    mov [0x6F0C], ax
    mov al, [0x7200 + 0x19]   ; bits per pixel
    mov [0x6F10], al
    mov ax, [0x7200 + 0x10]   ; BytesPerScanLine
    mov [0x6F14], ax
    mov [0x6F18], cx          ; mode number
    mov dword [0x6F00], 0x31454256   ; 'VBE1' (written last)

vbe_done:
    ; ── E820 memory map -> minimal Multiboot info at 0x5600 ──
    ; The El Torito loader has no Multiboot header, so it builds the info
    ; struct itself: flags = MEM|MMAP, E820 entries (with the Multiboot
    ; size prefix) at 0x5800.  EBX is handed to the kernel, which then uses
    ; the BIOS map instead of the 32MB mem_upper fallback.
    mov ax, 0
    mov es, ax
    mov ds, ax
    mov dword [0x5700], 0          ; entry count
    mov dword [0x5704], 0          ; mem_lower (KB)
    mov dword [0x5708], 0          ; mem_upper (KB)
    xor ebx, ebx                   ; continuation value
e820_loop:
    mov ax, 0
    mov es, ax
    mov ds, ax
    mov eax, 0xE820
    mov edx, 0x534D4150            ; 'SMAP'
    mov ecx, 24
    mov di, 0x5C00
    int 0x15
    jc e820_done
    cmp eax, 0x534D4150
    jne e820_done
    mov ax, 0
    mov es, ax
    mov ds, ax
    mov eax, [0x5700]
    cmp eax, 24
    jae e820_done
    imul eax, eax, 24
    mov edi, 0x5800
    add edi, eax
    mov dword [edi], 20            ; Multiboot entry size field
    mov eax, [0x5C00]              ; base low
    mov [edi + 4], eax
    mov eax, [0x5C04]
    mov [edi + 8], eax
    mov eax, [0x5C08]              ; length low
    mov [edi + 12], eax
    mov eax, [0x5C0C]
    mov [edi + 16], eax
    mov eax, [0x5C10]              ; type
    mov [edi + 20], eax
    inc dword [0x5700]
    cmp eax, 1
    jne e820_next
    mov eax, [0x5C00]
    cmp eax, 0x00100000
    jae e820_high
    mov edx, [0x5C08]
    mov ecx, 0x00100000
    sub ecx, eax
    cmp edx, ecx
    jbe e820_low_len
    mov edx, ecx
e820_low_len:
    shr edx, 10
    add [0x5704], edx
    jmp e820_next
e820_high:
    mov edx, [0x5C08]
    shr edx, 10
    add [0x5708], edx
e820_next:
    test ebx, ebx
    jz e820_done
    jmp e820_loop
e820_done:
    mov ax, 0
    mov es, ax
    mov ds, ax
    cmp dword [0x5700], 0
    jne e820_have
    mov dword [0x5704], 640        ; BIOS without E820: 640KB + 32MB fallback
    mov dword [0x5708], 32768
e820_have:
    mov eax, 0x41                  ; MEM | MMAP
    mov [0x5600], eax
    mov eax, [0x5704]
    mov [0x5604], eax
    mov eax, [0x5708]
    mov [0x5608], eax
    xor eax, eax
    mov edi, 0x560C
    mov ecx, 7                     ; boot_device .. syms[3] (28 bytes)
    rep stosd
    mov eax, [0x5700]
    imul eax, eax, 24
    mov [0x562C], eax              ; mmap_length
    mov dword [0x5630], 0x5800     ; mmap_addr
    mov dword [0x5640], 0          ; mods_count (flags bit3 clear)

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
    ; EAX = 0x2BADB002 (multiboot magic), EBX = our synthetic info struct
    mov eax, 0x2BADB002
    mov ebx, 0x5600

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

; Preferred VBE modes (32/24/16 bpp, 800x600 and up); 0xFFFF terminates.
vbe_modes:
    dw 0x114        ; 800x600x32
    dw 0x117        ; 1024x768x32
    dw 0x113        ; 800x600x24
    dw 0x116        ; 1024x768x24
    dw 0x112        ; 800x600x16
    dw 0xFFFF

; Kernel size placeholder at fixed offset 4092 (patched by Python)
times 4092 - ($ - $$) db 0
kernel_size_ptr: dd 0

; Kernel flat binary starts here (offset 4096)
kernel_start:
