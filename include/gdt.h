#ifndef MUOS_GDT_H
#define MUOS_GDT_H
#include "types.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS         0x28
#define GDT_TLS         0x30   /* slot 6: per-process TLS data segment */

void gdt_init(void);

/* Install/refresh the TSS descriptor in slot 5 (after the GDT is loaded;
 * the descriptor table itself lives in memory and stays live). */
void gdt_set_tss(uint32_t base, uint32_t limit);

/* Install/refresh the DPL3 data descriptor used for i386 TLS (%gs).
 * flags follows the Linux user_desc bitfield layout. */
void gdt_set_user_tls(uint32_t base, uint32_t limit, uint32_t flags);
void gdt_get_user_tls(uint32_t* base, uint32_t* limit, uint32_t* flags);

#endif
