/* linker_dyn.ld - dynamically linked PIE program for MuOS.
 *
 * Like linker_pie.ld but keeps .interp (so the linker emits PT_INTERP and
 * DT_NEEDED) and gives .rel.dyn / .rel.plt their own sections, which the
 * user-mode dynamic linker walks for RELATIVE and symbol relocations. */

ENTRY(_start)

PHDRS
{
    interp  PT_INTERP;
    load    PT_LOAD    FILEHDR PHDRS FLAGS(7);
    dynamic PT_DYNAMIC FLAGS(6);
}

SECTIONS
{
    . = SIZEOF_HEADERS;

    .interp   : { *(.interp) }                      :load :interp
    .text     : { *(.text) *(.text.*) }             :load
    .rodata   : { *(.rodata) *(.rodata.*) }         :load
    .dynsym   : { *(.dynsym) }                      :load
    .dynstr   : { *(.dynstr) }                      :load
    .hash     : { *(.hash) }                        :load
    .dynamic  : { *(.dynamic) }                     :load :dynamic
    .rel.dyn  : { *(.rel.dyn) }                     :load
    .rel.plt  : { *(.rel.plt) }                     :load
    .plt      : { *(.plt) *(.plt.*) }               :load
    .got      : { *(.got) *(.got.plt) }             :load
    .data     : { *(.data) *(.data.*) }             :load
    .bss      : { *(COMMON) *(.bss) *(.bss.*) }     :load

    /DISCARD/ : { *(.eh_frame) *(.eh_frame_hdr) *(.comment) *(.note*)
                 *(.gnu.hash) }
}
