#ifndef MUOS_ELF_H
#define MUOS_ELF_H
#include "types.h"

/* Minimal ELF32 definitions needed by the loader: enough to validate and
 * load static ET_EXEC images built for i386 (Linux ABI layout). */

#define ELF_MAGIC        0x464C457Fu  /* 0x7F 'E' 'L' 'F' */
#define ELF_CLASS_32     1
#define ELF_DATA_LSB     1
#define ELF_VERSION_CUR  1
#define ELF_TYPE_EXEC    2
#define ELF_MACHINE_386  3
#define ELF_PT_LOAD      1

typedef struct {
    uint8_t  e_ident[16];   /* 0x7F 'E' 'L' 'F', class, data, version... */
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed, aligned(4))) elf32_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed, aligned(4))) elf32_phdr_t;

/* Load an ET_EXEC/EM_386 image at its link-time virtual addresses.
 * [load_base, load_limit) is the sandbox the image must fit in.
 * Returns 0 on success, negative on error; *entry_out and *end_out
 * receive e_entry and the highest loaded byte. */
int elf32_load(const void* image, uint32_t image_size,
               uint32_t load_base, uint32_t load_limit,
               uint32_t* entry_out, uint32_t* end_out);

/* Header/segment range validation without writing memory (execve precheck). */
int elf32_validate(const void* image, uint32_t image_size,
                   uint32_t load_base, uint32_t load_limit,
                   uint32_t* entry_out, uint32_t* end_out);

#endif
