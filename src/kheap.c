#include "kheap.h"
#include "mm.h"
#include "string.h"

/* Heap capacity in pages: 256 pages = 1MB, plenty for the few dozen
 * small kernel objects MuOS allocates today. */
#define KHEAP_PAGES 256

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

void kheap_init(void) {
    uint8_t* mem = (uint8_t*)mm_alloc_pages(KHEAP_PAGES);
    if (!mem) return;  /* stays disabled; kmalloc returns NULL */

    heap_start = mem;
    heap_size  = KHEAP_PAGES * PAGE_SIZE;

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
    return NULL;  /* exhausted (see kheap.h: no grow yet) */
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
