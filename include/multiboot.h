#ifndef MUOS_MULTIBOOT_H
#define MUOS_MULTIBOOT_H
#include "types.h"

/*
 * Multiboot1 (0.6.96) information structures - only the fields MuOS
 * consumes, laid out at the exact spec offsets. mmap_length/mmap_addr
 * live at offsets 44/48, after syms[4]; a truncated definition that
 * skips syms misreads them.
 */
#define MULTIBOOT_MAGIC      0x2BADB002
#define MULTIBOOT_FLAG_MEM   0x00000001  /* mem_lower/mem_upper valid   */
#define MULTIBOOT_FLAG_MMAP  0x00000040  /* mmap_length/mmap_addr valid */

typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];        /* offsets 28-43: keep mmap fields aligned */
    uint32_t mmap_length;    /* offset 44 */
    uint32_t mmap_addr;      /* offset 48 */
} __attribute__((packed)) multiboot_info_t;

/* One BIOS E820-style region. Iterate with:
 *   p += entry->size + 4   (size excludes the size field itself) */
typedef struct {
    uint32_t size;
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;           /* 1 = usable RAM */
} __attribute__((packed)) multiboot_mmap_entry_t;

#endif
