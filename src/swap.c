/* swap.c - page swap on the tail of the primary ATA disk.
 *
 * The area starts at a fixed LBA well past the MuFS region and occupies
 * roughly half of the remaining disk (capped at 32MB). Slot indices are
 * tracked in a RAM bitmap; only live processes reference slots, so losing
 * the bitmap on reboot is harmless. The kernel reaches frames through the
 * identity map, so a slot copy is a plain ata_read/write_bytes. */
#include "swap.h"
#include "ata.h"
#include "mm.h"
#include "user.h"
#include "string.h"
#include "serial.h"

#define SWAP_RESERVED_LBA    4096u      /* keep the first 2MB for MuFS */
#define SWAP_MAX_SECTORS     65536u     /* 32MB */
#define SWAP_SECTORS_PER_SLOT 8u        /* 4KB page */
#define SWAP_MAX_SLOTS       (SWAP_MAX_SECTORS / SWAP_SECTORS_PER_SLOT)

static bool_t   enabled;
static uint32_t base_lba;
static uint32_t slots;
static uint32_t used;
static uint32_t hint;
static uint8_t  slot_map[(SWAP_MAX_SLOTS + 7) / 8];

static bool_t slot_used(uint32_t s) {
    return s < slots && (slot_map[s / 8] & (uint8_t)(1u << (s % 8)));
}

static void slot_set(uint32_t s, bool_t v) {
    if (s >= slots) return;
    uint8_t bit = (uint8_t)(1u << (s % 8));
    if (v) slot_map[s / 8] |= bit;
    else   slot_map[s / 8] &= (uint8_t)~bit;
}

static bool_t slot_alloc(uint32_t* out) {
    if (!enabled || !slots) return false;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t start = pass ? 0 : hint;
        uint32_t stop  = pass ? hint : slots;
        for (uint32_t s = start; s < stop; s++) {
            if (!slot_used(s)) {
                slot_set(s, true);
                used++;
                hint = s + 1;
                if (hint >= slots) hint = 0;
                *out = s;
                return true;
            }
        }
    }
    return false;
}

void swap_free_slot(uint32_t slot) {
    if (!slot_used(slot)) return;
    slot_set(slot, false);
    if (used) used--;
}

bool_t swap_alloc_and_write(uint32_t phys, uint32_t* slot_out) {
    if (!enabled || !phys) return false;
    uint32_t s;
    if (!slot_alloc(&s)) return false;
    uint64_t off = ((uint64_t)base_lba + (uint64_t)s * SWAP_SECTORS_PER_SLOT) * 512u;
    if (ata_write_bytes((uint32_t)off, (const void*)phys, PAGE_SIZE) != (int)PAGE_SIZE) {
        swap_free_slot(s);
        return false;
    }
    *slot_out = s;
    return true;
}

bool_t swap_read_and_free(uint32_t slot, uint32_t phys) {
    if (!enabled || !phys || !slot_used(slot)) return false;
    uint64_t off = ((uint64_t)base_lba + (uint64_t)slot * SWAP_SECTORS_PER_SLOT) * 512u;
    if (ata_read_bytes((uint32_t)off, (void*)phys, PAGE_SIZE) != (int)PAGE_SIZE)
        return false;
    swap_free_slot(slot);
    return true;
}

void swap_init(void) {
    enabled = false;
    base_lba = 0;
    slots = 0;
    used = 0;
    hint = 0;

    if (!ata_present()) return;
    uint32_t total = ata_sector_count();
    if (total < SWAP_RESERVED_LBA + 4096u) return;   /* need >= 2MB free */

    uint32_t free_lba = total - SWAP_RESERVED_LBA;
    uint32_t want = free_lba / 2;
    if (want > SWAP_MAX_SECTORS) want = SWAP_MAX_SECTORS;
    if (want < 2048u) return;

    base_lba = total - want;
    slots = want / SWAP_SECTORS_PER_SLOT;
    if (slots > SWAP_MAX_SLOTS) slots = SWAP_MAX_SLOTS;

    for (uint32_t i = 0; i < sizeof(slot_map); i++) slot_map[i] = 0;
    enabled = true;

    serial_write("[swap] enabled: ");
    { char b[12]; u32_to_dec(b, slots); serial_write(b); }
    serial_write(" pages at LBA ");
    { char b[12]; u32_to_dec(b, base_lba); serial_write(b); }
    serial_write("\n");
}

bool_t swap_enabled(void) { return enabled; }
uint32_t swap_slot_count(void) { return slots; }
uint32_t swap_used_slots(void) { return used; }

/* Write a pattern into a scratch user page, evict it, and fault it back. */
bool_t swap_selftest(void) {
    if (!enabled) return false;

    uint32_t tramp = (uint32_t)mm_alloc_page();
    if (!tramp) return false;
    memset((void*)tramp, 0, PAGE_SIZE);
    uint32_t pd = mm_create_user_pd(tramp);
    if (!pd) { mm_free_page((void*)tramp); return false; }

    bool_t ok = false;
    uint32_t va = USER_BASE;
    uint32_t before = mm_get_free_pages();

    serial_write("[swap] selftest start\n");
    if (!mm_user_fault(pd, va, true)) { serial_write("[swap] t1 fail\n"); goto out; }
    uint32_t phys = mm_user_translate(pd, va);
    if (!phys) { serial_write("[swap] t2 fail\n"); goto out; }
    uint8_t* p = (uint8_t*)phys;
    for (uint32_t i = 0; i < PAGE_SIZE; i++) p[i] = (uint8_t)((i * 7u + 3u) & 0xFF);

    if (!mm_user_swap_out(pd, va)) { serial_write("[swap] t3 fail\n"); goto out; }
    if (mm_user_translate(pd, va) != 0) { serial_write("[swap] t4 fail\n"); goto out; }
    if (mm_get_free_pages() != before - 1) { serial_write("[swap] t5 fail\n"); goto out; }

    if (!mm_user_fault(pd, va, false)) { serial_write("[swap] t6 fail\n"); goto out; }
    phys = mm_user_translate(pd, va);
    if (!phys) { serial_write("[swap] t7 fail\n"); goto out; }
    for (uint32_t i = 0; i < PAGE_SIZE; i++) {
        if (((uint8_t*)phys)[i] != (uint8_t)((i * 7u + 3u) & 0xFF)) {
            serial_write("[swap] t8 data mismatch\n");
            goto out;
        }
    }
    if (mm_get_free_pages() != before - 2) { serial_write("[swap] t9 count\n"); goto out; }
    if (swap_used_slots() != 0) { serial_write("[swap] t10 slot\n"); goto out; }

    ok = true;
out:
    mm_user_reset_pages(pd);
    mm_free_user_pd(pd);
    mm_free_page((void*)tramp);
    if (!ok) serial_write("[swap] selftest FAILED\n");
    return ok;
}
