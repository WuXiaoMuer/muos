#ifndef MUOS_TSS_H
#define MUOS_TSS_H
#include "types.h"

/* 32-bit Task State Segment (Intel SDM Vol.3 7.2.1).
 * MuOS is single-CPU and uses only ss0/esp0: the stack the CPU switches
 * to when an interrupt or INT n takes it from Ring3 to Ring0. */
typedef struct {
    uint32_t prev_tss;
    uint32_t esp0, ss0;
    uint32_t esp1, ss1;
    uint32_t esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

#define TSS_KERNEL_STACK_PAGES 2   /* 8KB Ring0 stack for user-mode entries */

/* Allocates the Ring0 entry stack, fills the TSS, installs the GDT
 * descriptor and executes `ltr`. Requires mm_init + paging first. */
bool_t   tss_init(void);
void     tss_set_esp0(uint32_t esp0);
uint32_t tss_get_kernel_stack_top(void);

#endif
