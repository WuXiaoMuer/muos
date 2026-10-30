#ifndef MUOS_ELF_H
#define MUOS_ELF_H
#include "types.h"

/* Minimal ELF32 definitions needed by the loader: enough to load static
 * ET_EXEC images and position-independent ET_DYN (PIE) images, apply
 * their dynamic relocations, and report a PT_INTERP request. */

#define ELF_MAGIC        0x464C457Fu  /* 0x7F 'E' 'L' 'F' */
#define ELF_CLASS_32     1
#define ELF_DATA_LSB     1
#define ELF_VERSION_CUR  1
#define ELF_TYPE_EXEC    2
#define ELF_TYPE_DYN     3
#define ELF_MACHINE_386  3

#define ELF_PT_LOAD      1
#define ELF_PT_DYNAMIC   2
#define ELF_PT_INTERP    3

/* Dynamic table tags (i386) */
#define DT_NULL       0
#define DT_NEEDED     1
#define DT_PLTRELSZ   2
#define DT_HASH       4
#define DT_STRTAB     5
#define DT_SYMTAB     6
#define DT_RELA       7
#define DT_RELASZ     8
#define DT_STRSZ      10
#define DT_SYMENT     11
#define DT_REL        17
#define DT_RELSZ      18
#define DT_RELENT     19
#define DT_PLTREL     20
#define DT_JMPREL     23
#define DT_RELCOUNT   0x6FFFFFFAu

/* i386 relocation types */
#define R_386_NONE     0
#define R_386_32       1
#define R_386_COPY     5
#define R_386_GLOB_DAT 6
#define R_386_JMP_SLOT 7
#define R_386_RELATIVE 8
#define R_386_IRELATIVE 42

#define SHN_UNDEF      0
#define SHN_ABS        0xFFF1

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

typedef struct {
    uint32_t d_tag;
    uint32_t d_val;
} __attribute__((packed, aligned(4))) elf32_dyn_t;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
} __attribute__((packed, aligned(4))) elf32_rel_t;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} __attribute__((packed, aligned(4))) elf32_sym_t;

typedef struct {
    uint32_t entry;
    uint32_t image_end;
    uint32_t phdr;        /* loaded address of the program header table */
    uint32_t phent;
    uint32_t phnum;
    uint32_t base;        /* load bias (0 for ET_EXEC) */
    bool_t   is_dyn;
    bool_t   has_interp;
    bool_t   has_dynamic;
    char     interp[64];  /* PT_INTERP request path */
    uint32_t reloc_count;
} elf32_load_info_t;

/* Load an ET_EXEC/ET_DYN/EM_386 image. ET_EXEC segments are loaded at
 * their link-time addresses; ET_DYN images get a bias of
 * load_base - min(p_vaddr) and their RELATIVE/GLOB_DAT/JMP_SLOT/32
 * relocations are applied. [load_base, load_limit) is the sandbox.
 * Returns 0 on success, negative on error. */
int elf32_load_ex(const void* image, uint32_t image_size,
                  uint32_t load_base, uint32_t load_limit,
                  elf32_load_info_t* info);

/* Compatibility wrapper: entry + highest loaded byte. */
int elf32_load(const void* image, uint32_t image_size,
               uint32_t load_base, uint32_t load_limit,
               uint32_t* entry_out, uint32_t* end_out);

/* Execve loader: maps the segments and, for self-contained images, applies
 * the dynamic relocations. When the image carries a PT_INTERP the caller
 * must map and enter that interpreter, which relocates the program and its
 * DT_NEEDED shared libraries itself (see user/ldmu.c). */
int elf32_load_program(const void* image, uint32_t image_size,
                       uint32_t load_base, uint32_t load_limit,
                       elf32_load_info_t* info);

/* Header/segment range validation without writing memory (execve precheck). */
int elf32_validate(const void* image, uint32_t image_size,
                   uint32_t load_base, uint32_t load_limit,
                   uint32_t* entry_out, uint32_t* end_out);

#endif
