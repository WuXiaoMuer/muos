/* linker_so.ld - ET_DYN shared objects (libmu.so and ld-mu.so).
 *
 * The file headers and program headers are part of the first LOAD segment
 * (FILEHDR PHDRS), so the kernel / user-mode loader can read the headers at
 * the object base. Link addresses start at 0; the loader picks a bias. */

ENTRY(_start)

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
    .gnu.hash : { *(.gnu.hash) }                    :load
    .dynamic  : { *(.dynamic) }                     :load :dynamic
    .rel.dyn  : { *(.rel.dyn) }                     :load
    .rel.plt  : { *(.rel.plt) }                     :load
    .plt      : { *(.plt) *(.plt.*) }               :load
    .got      : { *(.got) *(.got.plt) }             :load
    .data     : { *(.data) *(.data.*) }             :load
    .bss      : { *(COMMON) *(.bss) *(.bss.*) }     :load

    /DISCARD/ : { *(.eh_frame) *(.eh_frame_hdr) *(.comment) *(.note*)
                 *(.interp) }
}
