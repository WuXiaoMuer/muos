#include "gdt.h"

static struct gdt_entry gdt[5];
static struct gdt_ptr   gdt_ptr;

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
