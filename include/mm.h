#ifndef MUOS_MM_H
#define MUOS_MM_H
#include "types.h"

#define PAGE_SIZE       4096
#define PAGE_SHIFT      12
#define PAGE_MASK       0xFFFFF000

/* Page directory/table entry flags */
#define PDE_PRESENT     (1 << 0)
#define PDE_WRITABLE    (1 << 1)
#define PDE_USER        (1 << 2)
#define PDE_PWT         (1 << 3)  /* Write-through */
#define PDE_PCD         (1 << 4)  /* Cache disable */
#define PDE_ACCESSED    (1 << 5)
#define PDE_DIRTY       (1 << 6)
#define PDE_4MB         (1 << 7)  /* 4MB page */
#define PDE_GLOBAL      (1 << 8)

#define PTE_PRESENT     PDE_PRESENT
#define PTE_WRITABLE    PDE_WRITABLE
#define PTE_USER        PDE_USER

/* Default kernel page flags */
#define PAGE_KERNEL     (PDE_PRESENT | PDE_WRITABLE)
#define PAGE_KERNEL_RO  (PDE_PRESENT)

void mm_init(uint32_t mem_upper_kb, uint32_t kernel_end);
void mm_enable_paging(void);

void*  mm_alloc_page(void);
void   mm_free_page(void* addr);
uint32_t mm_get_total_pages(void);
uint32_t mm_get_free_pages(void);
uint32_t mm_get_used_pages(void);

void*  mm_alloc_pages(uint32_t count);
void   mm_free_pages(void* addr, uint32_t count);

/* Page mapping */
void mm_map_page(uint32_t phys, uint32_t virt, uint32_t flags);
void mm_unmap_page(uint32_t virt);
uint32_t mm_virt_to_phys(uint32_t virt);

/* Get page directory physical address */
uint32_t mm_get_page_directory(void);

#endif
