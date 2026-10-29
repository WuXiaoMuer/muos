#include "elf.h"
#include "string.h"

/* Loader errors (internal; surfaced as a single message by user.c) */
#define ELF_OK          0
#define ELF_ERR_HEADER  (-1)
#define ELF_ERR_FORMAT  (-2)
#define ELF_ERR_RANGE   (-3)
#define ELF_ERR_NOLOAD  (-4)

static bool_t is_power_of_two(uint32_t v) {
    return v && !(v & (v - 1));
}

int elf32_validate(const void* image, uint32_t image_size,
                   uint32_t load_base, uint32_t load_limit,
                   uint32_t* entry_out, uint32_t* end_out) {
    if (!image || image_size < sizeof(elf32_ehdr_t)) return ELF_ERR_HEADER;

    elf32_ehdr_t eh;
    memcpy(&eh, image, sizeof(eh));

    if (memcmp(eh.e_ident, "\x7f" "ELF", 4) != 0) return ELF_ERR_HEADER;
    if (eh.e_ident[4] != ELF_CLASS_32)                return ELF_ERR_FORMAT;
    if (eh.e_ident[5] != ELF_DATA_LSB)                return ELF_ERR_FORMAT;
    if (eh.e_ident[6] != ELF_VERSION_CUR)             return ELF_ERR_FORMAT;
    if (eh.e_type != ELF_TYPE_EXEC)                   return ELF_ERR_FORMAT;
    if (eh.e_machine != ELF_MACHINE_386)              return ELF_ERR_FORMAT;
    if (eh.e_ehsize < sizeof(elf32_ehdr_t))           return ELF_ERR_FORMAT;
    if (eh.e_phentsize < sizeof(elf32_phdr_t) ||
        eh.e_phentsize == 0 || eh.e_phnum == 0 ||
        eh.e_phnum > 128)                             return ELF_ERR_FORMAT;
    if (eh.e_phoff > image_size)                      return ELF_ERR_FORMAT;
    uint32_t table_bytes = (uint32_t)eh.e_phnum * eh.e_phentsize;
    if (table_bytes > image_size - eh.e_phoff)        return ELF_ERR_FORMAT;

    uint32_t max_end = load_base;
    uint32_t loads = 0;
    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        elf32_phdr_t ph;
        memcpy(&ph, (const uint8_t*)image + eh.e_phoff + (uint32_t)i * eh.e_phentsize,
               sizeof(ph));
        if (ph.p_type != ELF_PT_LOAD) continue;
        if (ph.p_filesz > ph.p_memsz)                 return ELF_ERR_FORMAT;
        if (ph.p_offset > image_size ||
            ph.p_filesz > image_size - ph.p_offset)   return ELF_ERR_RANGE;
        if (ph.p_vaddr < load_base || ph.p_vaddr >= load_limit ||
            ph.p_memsz > load_limit - ph.p_vaddr)      return ELF_ERR_RANGE;
        if (ph.p_align && !is_power_of_two(ph.p_align)) return ELF_ERR_FORMAT;
        uint32_t seg_end = ph.p_vaddr + ph.p_memsz;
        if (seg_end > max_end) max_end = seg_end;
        loads++;
    }
    if (loads == 0)                                   return ELF_ERR_NOLOAD;
    if (eh.e_entry < load_base || eh.e_entry >= load_limit) return ELF_ERR_RANGE;

    if (entry_out) *entry_out = eh.e_entry;
    if (end_out)   *end_out = max_end;
    return ELF_OK;
}

int elf32_load(const void* image, uint32_t image_size,
               uint32_t load_base, uint32_t load_limit,
               uint32_t* entry_out, uint32_t* end_out) {
    if (!image || image_size < sizeof(elf32_ehdr_t)) return ELF_ERR_HEADER;

    /* The blob is embedded at arbitrary alignment, so copy the header
     * instead of casting the pointer (strict-alignment safety). */
    elf32_ehdr_t eh;
    memcpy(&eh, image, sizeof(eh));

    if (memcmp(eh.e_ident, "\x7f" "ELF", 4) != 0) return ELF_ERR_HEADER;
    if (eh.e_ident[4] != ELF_CLASS_32)                return ELF_ERR_FORMAT;
    if (eh.e_ident[5] != ELF_DATA_LSB)                return ELF_ERR_FORMAT;
    if (eh.e_ident[6] != ELF_VERSION_CUR)             return ELF_ERR_FORMAT;
    if (eh.e_type != ELF_TYPE_EXEC)                   return ELF_ERR_FORMAT;
    if (eh.e_machine != ELF_MACHINE_386)              return ELF_ERR_FORMAT;
    if (eh.e_ehsize < sizeof(elf32_ehdr_t))           return ELF_ERR_FORMAT;
    if (eh.e_phentsize < sizeof(elf32_phdr_t) ||
        eh.e_phentsize == 0 || eh.e_phnum == 0 ||
        eh.e_phnum > 128)                             return ELF_ERR_FORMAT;

    /* phoff + phnum * phentsize, overflow checked */
    if (eh.e_phoff > image_size)                      return ELF_ERR_FORMAT;
    uint32_t table_bytes = (uint32_t)eh.e_phnum * eh.e_phentsize;
    if (table_bytes > image_size - eh.e_phoff)        return ELF_ERR_FORMAT;

    uint32_t max_end = load_base;
    uint32_t loads = 0;

    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        elf32_phdr_t ph;
        memcpy(&ph, (const uint8_t*)image + eh.e_phoff + (uint32_t)i * eh.e_phentsize,
               sizeof(ph));
        if (ph.p_type != ELF_PT_LOAD) continue;

        if (ph.p_filesz > ph.p_memsz)                 return ELF_ERR_FORMAT;
        if (ph.p_offset > image_size ||
            ph.p_filesz > image_size - ph.p_offset)   return ELF_ERR_RANGE;

        /* Segment must lie entirely inside the sandbox. Check the base
         * before subtracting, otherwise the unsigned range test wraps. */
        if (ph.p_vaddr < load_base || ph.p_vaddr >= load_limit ||
            ph.p_memsz > load_limit - ph.p_vaddr)      return ELF_ERR_RANGE;

        if (ph.p_align && !is_power_of_two(ph.p_align)) return ELF_ERR_FORMAT;

        /* dst == p_virtual address: identity-mapped sandbox memory */
        memcpy((void*)ph.p_vaddr, (const uint8_t*)image + ph.p_offset, ph.p_filesz);
        if (ph.p_memsz > ph.p_filesz) {
            memset((void*)(ph.p_vaddr + ph.p_filesz), 0,
                   ph.p_memsz - ph.p_filesz);          /* bss */
        }

        uint32_t seg_end = ph.p_vaddr + ph.p_memsz;
        if (seg_end > max_end) max_end = seg_end;
        loads++;
    }

    if (loads == 0)                                   return ELF_ERR_NOLOAD;
    if (eh.e_entry < load_base || eh.e_entry >= load_limit) return ELF_ERR_RANGE;

    *entry_out = eh.e_entry;
    *end_out   = max_end;
    return ELF_OK;
}
