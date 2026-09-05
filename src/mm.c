#include "mm.h"
#include "multiboot.h"
#include "vga.h"

/* External symbol defined in linker script */
extern uint32_t _kernel_end;

/* 32-bit kernel: ignore RAM above this cap (262144 pages keeps the
 * bitmap and page-table set finite and every count in uint32 range) */
#define MM_MAX_RAM_MB 1024

/* Bitmap for physical page frames */
static uint32_t* bitmap = NULL;
static uint32_t  total_pages = 0;
static uint32_t  free_pages = 0;
static uint32_t  mmap_region_count = 0;
static uint32_t  reported_mem_upper_kb = 0;  /* loader-reported RAM above 1MB */

/* Page directory (must be page-aligned) */
static uint32_t* page_directory __attribute__((aligned(PAGE_SIZE))) = NULL;
static uint32_t  page_dir_phys = 0;

static void bitmap_set(uint32_t frame) {
    uint32_t idx = frame / 32;
    uint32_t bit = frame % 32;
    bitmap[idx] |= (1u << bit);
}

static void bitmap_clear(uint32_t frame) {
    uint32_t idx = frame / 32;
    uint32_t bit = frame % 32;
    bitmap[idx] &= ~(1u << bit);
}

static bool_t bitmap_test(uint32_t frame) {
    uint32_t idx = frame / 32;
    uint32_t bit = frame % 32;
    return (bitmap[idx] & (1u << bit)) != 0;
}

/* Number of 32-bit words the bitmap needs for total_pages frames */
static uint32_t bitmap_words(void) {
    return (total_pages + 31) / 32;
}

static uint32_t bitmap_find_first_free(void) {
    uint32_t words = bitmap_words();
    for (uint32_t i = 0; i < words; i++) {
        if (bitmap[i] != 0xFFFFFFFF) {
            for (uint32_t j = 0; j < 32; j++) {
                uint32_t frame = i * 32 + j;
                if (frame >= total_pages) return (uint32_t)-1;
                if (!(bitmap[i] & (1u << j))) {
                    return frame;
                }
            }
        }
    }
    return (uint32_t)-1;
}

void mm_init(const multiboot_info_t* mb, uint32_t kernel_end) {
    /* Place bitmap after kernel */
    bitmap = (uint32_t*)kernel_end;

    const uint64_t ram_cap = (uint64_t)MM_MAX_RAM_MB * 1024 * 1024;
    if (mb && (mb->flags & MULTIBOOT_FLAG_MEM)) reported_mem_upper_kb = mb->mem_upper;

    /* Pass 1: determine RAM top. Prefer the BIOS memory map (E820 via
     * multiboot); fall back to mem_upper when the loader provided none
     * (e.g. the ISO boot path passes no multiboot info). */
    uint64_t ram_top = 0;
    if (mb && (mb->flags & MULTIBOOT_FLAG_MMAP)) {
        uint32_t p = mb->mmap_addr;
        uint32_t end = mb->mmap_addr + mb->mmap_length;
        while (p + 4 <= end) {
            const multiboot_mmap_entry_t* e = (const multiboot_mmap_entry_t*)p;
            if (e->type == 1) {
                uint64_t e_end = e->base_addr + e->length;
                if (e_end > ram_cap) e_end = ram_cap;
                if (e->base_addr < ram_cap && e_end > ram_top) ram_top = e_end;
                mmap_region_count++;
            }
            p += e->size + 4;
        }
    }
    if (ram_top == 0) {
        uint32_t mem_upper_kb = 0;
        if (mb && (mb->flags & MULTIBOOT_FLAG_MEM)) mem_upper_kb = mb->mem_upper;
        if (mem_upper_kb == 0) mem_upper_kb = 32768;  /* sane default: 32MB */
        ram_top = ((uint64_t)mem_upper_kb + 1024) * 1024;  /* + first MB */
        if (ram_top > ram_cap) ram_top = ram_cap;
    }

    total_pages = (uint32_t)(ram_top / PAGE_SIZE);

    /* Reserve kernel area (0 to kernel_end + bitmap) in bitmap */
    uint32_t bitmap_bytes = bitmap_words() * 4;
    uint32_t reserved_pages = (kernel_end + bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Initialize bitmap: everything used, then free only the frames
     * that the memory map reports as usable RAM. This naturally keeps
     * the sub-1MB legacy area, BIOS/EBDA holes and mmap holes reserved. */
    for (uint32_t i = 0; i < bitmap_words(); i++) {
        bitmap[i] = 0xFFFFFFFF;
    }

    free_pages = 0;
    if (mb && (mb->flags & MULTIBOOT_FLAG_MMAP)) {
        uint32_t p = mb->mmap_addr;
        uint32_t end = mb->mmap_addr + mb->mmap_length;
        while (p + 4 <= end) {
            const multiboot_mmap_entry_t* e = (const multiboot_mmap_entry_t*)p;
            if (e->type == 1) {
                uint64_t e_end = e->base_addr + e->length;
                if (e_end > ram_cap) e_end = ram_cap;
                uint32_t first = (uint32_t)((e->base_addr + PAGE_SIZE - 1) / PAGE_SIZE);
                uint32_t last  = (uint32_t)(e_end / PAGE_SIZE);
                if (first < reserved_pages) first = reserved_pages;
                if (last > total_pages) last = total_pages;
                for (uint32_t f = first; f < last; f++) {
                    bitmap_clear(f);
                    free_pages++;
                }
            }
            p += e->size + 4;
        }
    } else {
        /* No memory map: treat everything above the reserved area as usable */
        for (uint32_t f = reserved_pages; f < total_pages; f++) {
            bitmap_clear(f);
            free_pages++;
        }
    }

    /* Allocate page directory */
    page_directory = (uint32_t*)mm_alloc_page();
    if (!page_directory) return;
    page_dir_phys = (uint32_t)page_directory;
    for (uint32_t i = 0; i < 1024; i++) page_directory[i] = 0;

    /* Identity-map ALL RAM below ram_top. Runs with paging still off,
     * so touching physical addresses directly is safe. Page tables are
     * allocated from the bitmap allocator itself: frames are handed out
     * in ascending order starting right after the kernel, so every new
     * page table lies inside an already-mapped range (the first tables
     * cover the low 4MB where all subsequent tables are allocated) and
     * the bootstrap needs no temporary mappings. This closes the old
     * allocator/paging mismatch where frames above 4MB were handed out
     * while still unmapped. */
    for (uint32_t pfn = 0; pfn < total_pages; pfn++) {
        uint32_t pde = pfn >> 10;
        uint32_t* pt;
        if (!(page_directory[pde] & PDE_PRESENT)) {
            pt = (uint32_t*)mm_alloc_page();
            if (!pt) return;  /* ran out of frames while bootstrapping */
            for (uint32_t i = 0; i < 1024; i++) pt[i] = 0;
            page_directory[pde] = (uint32_t)pt | PAGE_KERNEL;
        } else {
            pt = (uint32_t*)(page_directory[pde] & PAGE_MASK);
        }
        pt[pfn & 0x3FF] = (pfn * PAGE_SIZE) | PAGE_KERNEL;
    }

    /* Higher-half kernel mapping (3GB+) and user-space layout are
     * deferred until the Ring3/ABI work (see ROADMAP.md). */
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
uint32_t mm_get_mmap_regions(void) { return mmap_region_count; }
uint32_t mm_get_mem_upper_kb(void) { return reported_mem_upper_kb; }

void* mm_alloc_pages(uint32_t count) {
    if (count == 0 || count > total_pages || free_pages < count) return NULL;
    /* Find contiguous free pages */
    for (uint32_t start = 0; start + count <= total_pages; start++) {
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
    if (count == 0) return;
    if ((uint32_t)addr & (PAGE_SIZE - 1)) return;  /* must be page-aligned */
    if (frame >= total_pages || frame + count > total_pages) return;
    for (uint32_t i = 0; i < count; i++) {
        if (bitmap_test(frame + i)) {
            bitmap_clear(frame + i);
            free_pages++;
        }
    }
}

bool_t mm_map_page(uint32_t phys, uint32_t virt, uint32_t flags) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    /* Get or create page table */
    uint32_t* pt;
    if (!(page_directory[pd_index] & PDE_PRESENT)) {
        pt = (uint32_t*)mm_alloc_page();
        if (!pt) return false;
        for (uint32_t i = 0; i < 1024; i++) pt[i] = 0;
        page_directory[pd_index] = (uint32_t)pt | PAGE_KERNEL;
    } else {
        pt = (uint32_t*)(page_directory[pd_index] & PAGE_MASK);
    }

    if (pt[pt_index] & PTE_PRESENT) return false;  /* already mapped */

    pt[pt_index] = (phys & PAGE_MASK) | (flags & 0xFFF) | PTE_PRESENT;

    /* Flush TLB entry */
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    return true;
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
