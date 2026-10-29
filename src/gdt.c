#include "gdt.h"

static struct gdt_entry gdt[7];
static struct gdt_ptr   gdt_ptr;

/* User TLS descriptor state (slot 6), for set/get_thread_area. */
static uint32_t tls_base = 0;
static uint32_t tls_limit = 0;
static uint32_t tls_flags = 0;

static void gdt_set_gate(int32_t num, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_mid    = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[num].access      = access;
}

void gdt_init(void) {
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (uint32_t)&gdt;

    /* Null descriptor */
    gdt_set_gate(0, 0, 0, 0, 0);

    /* Kernel Code: base=0, limit=4GB, DPL=0, executable, readable */
    gdt_set_gate(1, 0, 0xFFFFFFFF,
                 0x9A, /* Present, Ring0, Code, Exec/Read */
                 0xCF  /* 4K granularity, 32-bit */
    );

    /* Kernel Data: base=0, limit=4GB, DPL=0, writable */
    gdt_set_gate(2, 0, 0xFFFFFFFF,
                 0x92, /* Present, Ring0, Data, Read/Write */
                 0xCF  /* 4K granularity, 32-bit */
    );

    /* User Code: base=0, limit=4GB, DPL=3, executable, readable */
    gdt_set_gate(3, 0, 0xFFFFFFFF,
                 0xFA, /* Present, Ring3, Code, Exec/Read */
                 0xCF
    );

    /* User Data: base=0, limit=4GB, DPL=3, writable */
    gdt_set_gate(4, 0, 0xFFFFFFFF,
                 0xF2, /* Present, Ring3, Data, Read/Write */
                 0xCF
    );

    /* Slot 5 stays empty until tss_init() fills it and issues `ltr`.
     * The GDT limit already covers it, so no reload of lgdt is needed. */

    /* Load GDT and reload segment registers */
    __asm__ volatile (
        "lgdt %0\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        "ljmp $0x08, $1f\n\t"
        "1:\n\t"
        :
        : "m"(gdt_ptr)
        : "eax"
    );
}

void gdt_set_tss(uint32_t base, uint32_t limit) {
    /* 0x89 = present, DPL0, 32-bit TSS (available); byte-granular limit */
    gdt_set_gate(5, base, limit, 0x89, 0x00);
}

void gdt_set_user_tls(uint32_t base, uint32_t limit, uint32_t flags) {
    tls_base = base;
    tls_limit = limit & 0xFFFFF;
    tls_flags = flags;

    /* user_desc bits: 0 seg_32bit, 3 read_exec_only, 4 limit_in_pages,
     * 5 seg_not_present. TLS is a DPL3 writable data segment. */
    uint8_t access = 0xF2;                         /* present, DPL3, data RW */
    if (flags & 0x08) access = 0xF0;               /* read-only data */
    if (flags & 0x20) access &= (uint8_t)~0x80;    /* not present */
    uint8_t gran = 0x40;                           /* D=1 (32-bit) */
    if (flags & 0x10) gran |= 0x80;                /* G=1 (4KB granularity) */

    gdt_set_gate(6, tls_base, tls_limit, access, gran);
}

void gdt_get_user_tls(uint32_t* base, uint32_t* limit, uint32_t* flags) {
    if (base)  *base  = tls_base;
    if (limit) *limit = tls_limit;
    if (flags) *flags = tls_flags;
}
