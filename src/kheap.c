#include "kheap.h"
#include "mm.h"
#include "string.h"

/* Grow-on-demand kernel heap.
 *
 * The heap lives in its own virtual window (not the RAM identity alias) so
 * that fresh physical frames can be mapped contiguously as it grows. These
 * addresses are CPU-only: never hand a kheap pointer to a DMA device.
 *
 * Initial capacity is 1MB; exhaustion appends 256KB chunks until the 16MB
 * ceiling, then kmalloc returns NULL as before. */
#define KHEAP_PAGES       256
#define KHEAP_GROW_PAGES  64
#define KHEAP_MAX_PAGES   (KHEAP_MAX_BYTES / PAGE_SIZE)   /* 16MB ceiling */

/* Live-block marker in every header; a mismatch means corruption or
 * "pointer was never returned by kmalloc". */
#define KHEAP_MAGIC 0x4B48454Fu  /* 'KHEO' */

/* Block header is 16 bytes so payloads stay 16-byte aligned as long as
 * heap_start is page-aligned and every payload size is rounded to 16. */
typedef struct {
    uint32_t magic;
    uint32_t size;   /* payload size, multiple of 16 */
    uint32_t used;
    uint32_t pad;
} kheap_block_t;

#define KHEAP_HEADER   ((uint32_t)sizeof(kheap_block_t))
#define ALIGN16(x)     (((x) + 15u) & ~15u)

static uint8_t* heap_start = NULL;
static uint32_t heap_size  = 0;
static uint32_t heap_pages = 0;

/* Pick a free 16MB window above the RAM alias and below the top of the
 * address space, skipping anything already mapped (RAM alias, E1000 MMIO). */
static uint32_t kheap_va_base(void) {
    uint32_t cand = 0xD0000000u;
    while (cand < 0xFF000000u && mm_virt_to_phys(cand) != 0) cand += 0x400000u;
    if (cand >= 0xE0000000u && cand < 0xE0020000u) cand = 0xE0020000u;
    return cand;
}

/* Map `pages` fresh frames at the current heap end. Returns the number of
 * pages actually mapped (0 on failure); the caller extends heap_size. */
static uint32_t kheap_map(uint32_t pages) {
    uint32_t done = 0;
    while (done < pages) {
        void* frame = mm_alloc_page();
        if (!frame) break;
        uint32_t virt = (uint32_t)heap_start + heap_size + done * PAGE_SIZE;
        if (!mm_map_page((uint32_t)frame, virt, PAGE_KERNEL)) {
            mm_free_page(frame);
            break;
        }
        done++;
    }
    return done;
}

/* Find the last block header; NULL if the heap is corrupt. */
static kheap_block_t* kheap_last(uint32_t end_offset) {
    uint8_t* end = heap_start + end_offset;
    uint8_t* p = heap_start;
    kheap_block_t* last = NULL;
    while (p + KHEAP_HEADER <= end) {
        kheap_block_t* b = (kheap_block_t*)p;
        if (b->magic != KHEAP_MAGIC) return NULL;
        last = b;
        p = (uint8_t*)b + KHEAP_HEADER + b->size;
    }
    return (p == end) ? last : NULL;
}

/* Append pages to the heap and extend (or create) the trailing free block. */
static bool_t kheap_grow(void) {
    if (!heap_start || heap_pages + 1 >= KHEAP_MAX_PAGES) return false;

    /* Locate the trailing block before committing any pages. */
    kheap_block_t* last = kheap_last(heap_size);
    if (!last) return false;             /* corrupt heap */

    uint32_t want = KHEAP_GROW_PAGES;
    if (heap_pages + want > KHEAP_MAX_PAGES) want = KHEAP_MAX_PAGES - heap_pages;
    uint32_t got = kheap_map(want);
    if (!got) return false;

    uint32_t add = got * PAGE_SIZE;
    uint32_t old_size = heap_size;
    heap_size += add;
    heap_pages += got;

    if (!last->used) {
        last->size += add;               /* coalesce with the trailing free block */
    } else {
        kheap_block_t* nb = (kheap_block_t*)(heap_start + old_size);
        nb->magic = KHEAP_MAGIC;
        nb->size  = add - KHEAP_HEADER;
        nb->used  = 0;
        nb->pad   = 0;
    }
    return true;
}

void kheap_init(void) {
    heap_start = (uint8_t*)kheap_va_base();
    heap_size  = 0;
    heap_pages = 0;

    uint32_t got = kheap_map(KHEAP_PAGES);
    if (!got) { heap_start = NULL; return; }  /* stays disabled */

    heap_size  = got * PAGE_SIZE;
    heap_pages = got;

    /* One big free block spanning the whole heap */
    kheap_block_t* b = (kheap_block_t*)heap_start;
    b->magic = KHEAP_MAGIC;
    b->size  = heap_size - KHEAP_HEADER;   /* multiple of 16 */
    b->used  = 0;
    b->pad   = 0;
}

bool_t kheap_ready(void) {
    return heap_start != NULL;
}

void* kmalloc(uint32_t size) {
    if (!heap_start) return NULL;
    if (size == 0) size = 1;
    uint32_t need = ALIGN16(size);
    if (need < size) return NULL;  /* size too close to 2^32 */
    if (need > KHEAP_MAX_BYTES - KHEAP_HEADER) return NULL;  /* never fits */

    for (;;) {
        uint8_t* heap_end = heap_start + heap_size;
        uint8_t* p = heap_start;
        while (p + KHEAP_HEADER <= heap_end) {
            kheap_block_t* b = (kheap_block_t*)p;
            if (b->magic != KHEAP_MAGIC) return NULL;  /* heap corrupted */
            if (!b->used && b->size >= need) {
                /* Split off the tail when the remainder can hold a header
                 * plus a minimal (16-byte) payload */
                if (b->size >= need + KHEAP_HEADER + 16) {
                    kheap_block_t* nb = (kheap_block_t*)(p + KHEAP_HEADER + need);
                    nb->magic = KHEAP_MAGIC;
                    nb->size  = b->size - need - KHEAP_HEADER;
                    nb->used  = 0;
                    nb->pad   = 0;
                    b->size   = need;
                }
                b->used = 1;
                return (void*)(p + KHEAP_HEADER);
            }
            p += KHEAP_HEADER + b->size;
        }
        /* Exhausted: append more pages and retry; stops at the cap. */
        if (!kheap_grow()) return NULL;
    }
}

void kfree(void* ptr) {
    if (!ptr || !heap_start) return;

    uint8_t* p = (uint8_t*)ptr;
    if (p < heap_start + KHEAP_HEADER || p >= heap_start + heap_size) return;
    kheap_block_t* b = (kheap_block_t*)(p - KHEAP_HEADER);
    if (b->magic != KHEAP_MAGIC) return;  /* not a heap block */
    if (!b->used) return;                 /* double free — ignore */
    b->used = 0;

    /* Coalesce forward while the next block is free */
    uint8_t* heap_end = heap_start + heap_size;
    uint8_t* next = (uint8_t*)b + KHEAP_HEADER + b->size;
    while (next + KHEAP_HEADER <= heap_end) {
        kheap_block_t* nb = (kheap_block_t*)next;
        if (nb->magic != KHEAP_MAGIC || nb->used) break;
        b->size += KHEAP_HEADER + nb->size;
        next += KHEAP_HEADER + nb->size;
    }

    /* Coalesce backward: find the previous block by walking from the
     * start. O(n) but the heap holds only a handful of blocks. */
    kheap_block_t* cur = (kheap_block_t*)heap_start;
    while ((uint8_t*)cur + KHEAP_HEADER <= heap_end && cur < b) {
        if (cur->magic != KHEAP_MAGIC) break;
        uint8_t* nxt = (uint8_t*)cur + KHEAP_HEADER + cur->size;
        if (nxt == (uint8_t*)b) {
            if (!cur->used) cur->size += KHEAP_HEADER + b->size;
            break;
        }
        cur = (kheap_block_t*)nxt;
    }
}

void* kcalloc(uint32_t num, uint32_t size) {
    uint64_t total = (uint64_t)num * (uint64_t)size;
    if (total > 0xFFFFFFFFull) return NULL;
    void* p = kmalloc((uint32_t)total);
    if (p) memset(p, 0, (uint32_t)total);
    return p;
}
