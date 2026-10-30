/* Multiboot/ISO 装载器在关闭分页时跳到 e_entry,因此入口必须是物理地址。 */
ENTRY(entry_phys)

/* Higher-half 内核:虚拟地址 0xC0100000,物理装载地址仍是 0x100000。
 * AT(...) 指定每个段的 LMA,Multiboot/ISO 装载器按物理地址放好镜像,
 * 引导期自举页表把 0xC0000000+ 映射到物理 0。 */

SECTIONS
{
    . = 0xC0100000;

    .text   : AT(ADDR(.text)   - 0xC0000000) { *(.text) }
    .rodata : AT(ADDR(.rodata) - 0xC0000000) { *(.rodata) }
    .data   : AT(ADDR(.data)   - 0xC0000000) { *(.data) }
    .bss    : AT(ADDR(.bss)    - 0xC0000000) {
        *(COMMON)
        *(.bss)
    }

    /* 内核结束标记(虚拟地址);物理结束 = _kernel_end - 0xC0000000 */
    _kernel_end = .;

    /* .text 的物理起始地址(即 start),供 Multiboot 入口使用 */
    entry_phys = ADDR(.text) - 0xC0000000;
}
