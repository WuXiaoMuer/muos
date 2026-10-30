ENTRY(_start)

/* Position-independent (ET_DYN) test image. Link addresses are relative
 * to 0; the kernel loads the lowest segment at USER_BASE and then applies
 * the .rel.dyn / .rel.plt relocations found through PT_DYNAMIC.
 * The program headers are part of the first LOAD segment (FILEHDR PHDRS),
 * which is what ld requires for a PIE. */

PHDRS
{
    load    PT_LOAD    FILEHDR PHDRS FLAGS(7);
    dynamic PT_DYNAMIC FLAGS(6);
}

SECTIONS
{
    . = SIZEOF_HEADERS;

    .text     : { *(.text) *(.text.*) }             :load
    .rodata   : { *(.rodata) *(.rodata.*) }         :load
    .dynsym   : { *(.dynsym) }                      :load
    .dynstr   : { *(.dynstr) }                      :load
    .hash     : { *(.hash) }                        :load
    .dynamic  : { *(.dynamic) }                     :load :dynamic
    .rel.dyn  : { *(.rel.dyn) *(.rel.*) }           :load
    .got      : { *(.got) *(.got.plt) }             :load
    .data     : { *(.data) *(.data.*) }             :load
    .bss      : { *(COMMON) *(.bss) *(.bss.*) }     :load

    /DISCARD/ : { *(.eh_frame) *(.eh_frame_hdr) *(.comment) *(.note*)
                 *(.interp) }
}
