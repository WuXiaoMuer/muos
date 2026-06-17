#include "mm.h"
#include "vga.h"

/* External symbol defined in linker script */
extern uint32_t _kernel_end;

/* Bitmap for physical page frames */
static uint32_t* bitmap = NULL;
static uint32_t  total_pages = 0;
static uint32_t  free_pages = 0;
static uint32_t  bitmap_size_pages = 0;

/* Page directory (must be page-aligned) */
static uint32_t* page_directory __attribute__((aligned(PAGE_SIZE))) = NULL;
static uint32_t  page_dir_phys = 0;

/* First page table (maps first 4MB - identity) */
static uint32_t* page_table_0 __attribute__((aligned(PAGE_SIZE))) = NULL;

static void bitmap_set(uint32_t frame) {
    uint32_t idx = frame / 32;
    uint32_t bit = frame % 32;
    bitmap[idx] |= (1 << bit);
}

static void bitmap_clear(uint32_t frame) {
    uint32_t idx = frame / 32;
    uint32_t bit = frame % 32;
    bitmap[idx] &= ~(1 << bit);
}

static bool_t bitmap_test(uint32_t frame) {
    uint32_t idx = frame / 32;
    uint32_t bit = frame % 32;
    return (bitmap[idx] & (1 << bit)) != 0;
}

static uint32_t bitmap_find_first_free(void) {
    for (uint32_t i = 0; i < total_pages / 32; i++) {
        if (bitmap[i] != 0xFFFFFFFF) {
            for (uint32_t j = 0; j < 32; j++) {
                if (!(bitmap[i] & (1 << j))) {
                    uint32_t frame = i * 32 + j;
                    if (frame < total_pages) {
                        return frame;
                    }
                }
            }
        }
    }
    return (uint32_t)-1;
}

void mm_init(uint32_t mem_upper_kb, uint32_t kernel_end) {
    /* Calculate total physical pages */
    uint32_t total_mem_kb = mem_upper_kb + 1024; /* mem_upper + first 1MB */
    total_pages = total_mem_kb * 1024 / PAGE_SIZE;

    /* Place bitmap after kernel */
    bitmap_size_pages = (total_pages / 8 + PAGE_SIZE - 1) / PAGE_SIZE;
    bitmap = (uint32_t*)kernel_end;

    /* Reserve kernel area (0 to kernel_end + bitmap) in bitmap */
    uint32_t reserved_pages = (kernel_end + bitmap_size_pages * PAGE_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Initialize bitmap: all free */
    for (uint32_t i = 0; i < total_pages / 32; i++) {
        bitmap[i] = 0;
    }

    /* Mark reserved pages as used */
    for (uint32_t i = 0; i < reserved_pages; i++) {
        bitmap_set(i);
    }

    free_pages = total_pages - reserved_pages;

    /* Allocate page directory and first page table */
    page_directory = (uint32_t*)mm_alloc_page();
    if (!page_directory) return;
    page_dir_phys = (uint32_t)page_directory;

    page_table_0 = (uint32_t*)mm_alloc_page();
    if (!page_table_0) return;

    /* Clear page directory and page table */
    for (uint32_t i = 0; i < 1024; i++) {
        page_directory[i] = 0;
        page_table_0[i] = 0;
    }

    /* Identity map first 4MB */
    for (uint32_t i = 0; i < 1024; i++) {
        page_table_0[i] = (i * PAGE_SIZE) | PAGE_KERNEL;
    }

    /* Map first 4MB in page directory */
    page_directory[0] = (uint32_t)page_table_0 | PAGE_KERNEL;

    /* Also map the kernel at 3GB+ (higher half) for future use */
    /* page_directory[768] = (uint32_t)page_table_0 | PAGE_KERNEL; */
}

void mm_enable_paging(void) {
    /* Load page directory */
    __asm__ volatile ("mov %0, %%cr3" : : "r"(page_dir_phys));

    /* Enable paging and write-protect */
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;  /* Set PG bit */
    cr0 |= 0x00010000;  /* Set WP bit */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
}

void* mm_alloc_page(void) {
    uint32_t frame = bitmap_find_first_free();
    if (frame == (uint32_t)-1) return NULL;

    bitmap_set(frame);
    free_pages--;
    return (void*)(frame * PAGE_SIZE);
}

void mm_free_page(void* addr) {
    uint32_t frame = (uint32_t)addr / PAGE_SIZE;
    if (frame < total_pages) {
        if (bitmap_test(frame)) {
            bitmap_clear(frame);
            free_pages++;
        }
    }
}

uint32_t mm_get_total_pages(void) { return total_pages; }
uint32_t mm_get_free_pages(void)  { return free_pages; }
uint32_t mm_get_used_pages(void)  { return total_pages - free_pages; }

void* mm_alloc_pages(uint32_t count) {
    /* Find contiguous free pages */
    for (uint32_t start = 0; start < total_pages - count; start++) {
        bool_t found = true;
        for (uint32_t i = 0; i < count; i++) {
            if (bitmap_test(start + i)) {
                found = false;
                start += i; /* Skip ahead */
                break;
            }
        }
        if (found) {
            for (uint32_t i = 0; i < count; i++) {
                bitmap_set(start + i);
            }
            free_pages -= count;
            return (void*)(start * PAGE_SIZE);
        }
    }
    return NULL;
}

void mm_free_pages(void* addr, uint32_t count) {
    uint32_t frame = (uint32_t)addr / PAGE_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        if (bitmap_test(frame + i)) {
            bitmap_clear(frame + i);
            free_pages++;
        }
    }
}

void mm_map_page(uint32_t phys, uint32_t virt, uint32_t flags) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    /* Get or create page table */
    uint32_t* pt;
    if (!(page_directory[pd_index] & PDE_PRESENT)) {
        pt = (uint32_t*)mm_alloc_page();
        if (!pt) return;
        for (uint32_t i = 0; i < 1024; i++) pt[i] = 0;
        page_directory[pd_index] = (uint32_t)pt | PAGE_KERNEL;
    } else {
        pt = (uint32_t*)(page_directory[pd_index] & PAGE_MASK);
    }

    pt[pt_index] = (phys & PAGE_MASK) | (flags & 0xFFF) | PTE_PRESENT;

    /* Flush TLB entry */
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

void mm_unmap_page(uint32_t virt) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (page_directory[pd_index] & PDE_PRESENT) {
        uint32_t* pt = (uint32_t*)(page_directory[pd_index] & PAGE_MASK);
        pt[pt_index] = 0;
        __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    }
}

uint32_t mm_virt_to_phys(uint32_t virt) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (!(page_directory[pd_index] & PDE_PRESENT)) return 0;

    uint32_t* pt = (uint32_t*)(page_directory[pd_index] & PAGE_MASK);
    if (!(pt[pt_index] & PTE_PRESENT)) return 0;

    return (pt[pt_index] & PAGE_MASK) + (virt & 0xFFF);
}

uint32_t mm_get_page_directory(void) {
    return page_dir_phys;
}
