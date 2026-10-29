#include "tss.h"
#include "gdt.h"
#include "mm.h"
#include "string.h"

static tss_t    tss;
static uint32_t kernel_stack_top = 0;

bool_t tss_init(void) {
    /* The Ring0 stack used by every Ring3 -> Ring0 transition
     * (hardware interrupts included). Identity mapped, like all RAM. */
    void* stack = mm_alloc_pages(TSS_KERNEL_STACK_PAGES);
    if (!stack) return false;
    kernel_stack_top = (uint32_t)stack + TSS_KERNEL_STACK_PAGES * PAGE_SIZE;

    memset(&tss, 0, sizeof(tss));
    tss.ss0        = GDT_KERNEL_DATA;
    tss.esp0       = kernel_stack_top;
    tss.iomap_base = (uint16_t)sizeof(tss);  /* no I/O bitmap: user `in/out` faults */

    gdt_set_tss((uint32_t)&tss, (uint32_t)sizeof(tss) - 1);

    __asm__ volatile ("ltr %0" : : "r"((uint16_t)GDT_TSS));
    return true;
}

void tss_set_esp0(uint32_t esp0) {
    tss.esp0 = esp0;
}

uint32_t tss_get_kernel_stack_top(void) {
    return kernel_stack_top;
}
