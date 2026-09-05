#ifndef MUOS_KHEAP_H
#define MUOS_KHEAP_H
#include "types.h"

/* First-fit kernel heap on top of the physical page allocator.
 *  - 16-byte aligned allocations, 8/16-byte block headers
 *  - NOT reentrant: callers in IRQ context must wrap with cli/sti
 *  - fixed size; kmalloc returns NULL when exhausted
 *    (grow-on-demand is a ROADMAP item, needs VM primitives first) */

void  kheap_init(void);
bool_t kheap_ready(void);
void* kmalloc(uint32_t size);
void  kfree(void* ptr);
void* kcalloc(uint32_t num, uint32_t size);

#endif
