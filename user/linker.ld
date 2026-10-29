ENTRY(_start)

SECTIONS
{
    /* Classic Linux i386 ET_EXEC address; the kernel's user sandbox
     * starts here (see include/user.h). */
    . = 0x08048000;

    .text   : { *(.text)   *(.text.*) }
    .rodata : { *(.rodata) *(.rodata.*) }
    .data   : { *(.data)   *(.data.*) }
    .bss    : { *(COMMON)  *(.bss) *(.bss.*) }

    /DISCARD/ : { *(.eh_frame) *(.eh_frame_hdr) *(.comment) *(.note*) }
}
