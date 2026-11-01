#ifndef MUOS_SWAP_H
#define MUOS_SWAP_H
#include "types.h"

/* Page-level swap on the tail of the primary ATA disk. One slot holds one
 * 4KB page. The area is only claimed when the disk carries no partition
 * table and no FAT32/ext2 volume, so it never overlaps a filesystem. */
void     swap_init(void);
bool_t   swap_enabled(void);
uint32_t swap_slot_count(void);
uint32_t swap_used_slots(void);

/* Copy the physical frame `phys` to a free slot (does not touch the PTE). */
bool_t swap_alloc_and_write(uint32_t phys, uint32_t* slot_out);
/* Read a slot into `phys` and release it on success. */
bool_t swap_read_and_free(uint32_t slot, uint32_t phys);
void   swap_free_slot(uint32_t slot);

/* Exercise write -> evict -> fault-in on a scratch user page directory. */
bool_t swap_selftest(void);
/* Multi-page stress: evict 600 pages across three scratch address spaces,
 * verify the slots are occupied, fail them back in and compare content. */
bool_t swap_stress_test(void);

#endif
