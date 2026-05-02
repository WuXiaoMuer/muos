/* linker.ld - 内核链接脚本
 * 将内核加载到物理地址 1MB 处 (0x100000)
 */

ENTRY(start)

SECTIONS
{
    . = 0x100000;   /* 1 MB */

    .multiboot :
    {
        *(.multiboot)
    }

    .text :
    {
        *(.text)
    }

    .rodata :
    {
        *(.rodata)
    }

    .data :
    {
        *(.data)
    }

    .bss :
    {
        *(COMMON)
        *(.bss)
    }
}
