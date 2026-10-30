#ifndef MUOS_MM_H
#define MUOS_MM_H
#include "types.h"
#include "multiboot.h"
#include "user.h"

#define PAGE_SIZE       4096
#define PAGE_SHIFT      12
#define PAGE_MASK       0xFFFFF000

/* Higher-half layout: kernel virtual = physical + KERNEL_VBASE; the image
 * is linked at KERNEL_BASE and loaded at KERNEL_PHYS_BASE. */
#define KERNEL_VBASE      0xC0000000u
#define KERNEL_BASE       0xC0100000u
#define KERNEL_PHYS_BASE  0x00100000u

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

void mm_init(const multiboot_info_t* mb, uint32_t kernel_end);
void mm_enable_paging(void);

void*  mm_alloc_page(void);
void   mm_free_page(void* addr);
uint32_t mm_get_total_pages(void);
uint32_t mm_get_free_pages(void);
uint32_t mm_get_used_pages(void);
uint32_t mm_get_mmap_regions(void);
uint32_t mm_get_mem_upper_kb(void);

void*  mm_alloc_pages(uint32_t count);
void   mm_free_pages(void* addr, uint32_t count);

/* Page mapping */
bool_t mm_map_page(uint32_t phys, uint32_t virt, uint32_t flags);
void mm_unmap_page(uint32_t virt);
uint32_t mm_virt_to_phys(uint32_t virt);

/* Reserve a physical range (page-rounded) so the allocator never hands
 * it out. Fails if any page in the range is already in use. Used by the
 * user-mode sandbox, whose virtual addresses are identity-mapped.
 * Returns true on success. */
bool_t mm_reserve_range(uint32_t start, uint32_t end);

/* Mark an already identity-mapped range as user-accessible (U/S bit on
 * both the PDE and every PTE). Reloads CR3. */
bool_t mm_user_map_range(uint32_t start, uint32_t end);

/* Get page directory physical address */
uint32_t mm_get_page_directory(void);

/* Per-process user address spaces. Each process owns an 8MB physical
 * block mapped at USER_BASE through a private page directory. The kernel
 * identity map remains shared for every other range, so the physical
 * window [0x08000000,0x08C00000) is reserved for user mappings only. */
#define MM_USER_WINDOW_START 0x08000000u
/* Reserve physical frames shadowed by the configured user window (rounded
 * up to a 4MB PDE boundary). */
#define MM_USER_WINDOW_END   ((USER_END + 0x3FFFFFu) & ~0x3FFFFFu)

/* The last page of the user window is left unmapped by the block and holds
 * a per-process sigreturn trampoline instead (read-only for user code). */
uint32_t mm_create_user_pd(uint32_t block_phys, uint32_t tramp_phys);
/* Clone a user page directory (kernel PDEs shared, user PTEs copied into
 * fresh page tables) so COW'd remaps are preserved; gives the child its
 * own sigreturn trampoline page. */
uint32_t mm_clone_user_pd(uint32_t src_pd_phys, uint32_t tramp_phys);
void     mm_free_user_pd(uint32_t pd_phys);
void     mm_switch_pd(uint32_t pd_phys);
uint32_t mm_get_current_pd(void);

/* Copy-on-write support for the user window. */
void     mm_page_ref_inc(uint32_t phys);
uint32_t mm_page_ref(uint32_t phys);
/* Translate a user virtual address through a specific page directory. */
uint32_t mm_user_translate(uint32_t pd_phys, uint32_t va);
/* Make the page containing `va` writable in `pd_phys`, copying it first
 * when it is shared (frame refcount > 1). Returns false when not mapped. */
bool_t   mm_user_make_writable(uint32_t pd_phys, uint32_t va);
/* Clear the writable bit on every user page (fork marks both sides). */
void     mm_user_write_protect(uint32_t pd_phys);
/* Drop one reference from every user page this directory maps. */
void     mm_user_release_pages(uint32_t pd_phys);
/* Privatize every still-shared user page (used by execve). */
bool_t   mm_user_detach_pages(uint32_t pd_phys);

#endif
