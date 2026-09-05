ENTRY(start)

SECTIONS
{
    . = 0x100000;   /* 1 MB - 内核加载位置 */

    .text      : { *(.text) }
    .rodata    : { *(.rodata) }
    .data      : { *(.data) }
    .bss       : {
        *(COMMON)
        *(.bss)
    }

    /* 内核结束标记，供内存管理器使用 */
    _kernel_end = .;
}
