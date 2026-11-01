#ifndef MUOS_KHEAP_H
#define MUOS_KHEAP_H
#include "types.h"

/* First-fit kernel heap on top of the physical page allocator.
 *  - 16-byte aligned allocations, 16-byte block headers
 *  - NOT reentrant: callers in IRQ context must wrap with cli/sti
 *  - starts at 1MB and grows in 256KB chunks on demand up to
 *    KHEAP_MAX_BYTES; kmalloc returns NULL beyond that or when RAM runs out
 *  - the heap window is CPU-only memory: never use it for DMA buffers */

#define KHEAP_MAX_BYTES (16u * 1024u * 1024u)

void  kheap_init(void);
bool_t kheap_ready(void);
void* kmalloc(uint32_t size);
void  kfree(void* ptr);
void* kcalloc(uint32_t num, uint32_t size);

#endif
