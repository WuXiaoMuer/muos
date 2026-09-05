#include "test.h"
#include "types.h"
#include "vga.h"
#include "serial.h"
#include "mm.h"
#include "kheap.h"
#include "fs.h"
#include "keyboard.h"
#include "pit.h"
#include "task.h"
#include "string.h"

/* ANSI-ish helpers */
#define OK  "[PASS] "
#define FAIL "[FAIL] "

static int passed = 0;
static int failed = 0;

static void tpass(const char* name) {
    passed++;
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print(OK);
    vga_print(name);
    vga_putchar('\n');
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
    serial_write(OK); serial_write(name); serial_write("\n");
}

static void tfail(const char* name) {
    failed++;
    vga_setcolor(vga_entry_color(VGA_LIGHT_RED, VGA_BLACK));
    vga_print(FAIL);
    vga_print(name);
    vga_putchar('\n');
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
    serial_write(FAIL); serial_write(name); serial_write("\n");
}

#define CHECK(cond, name) do { if (cond) tpass(name); else tfail(name); } while (0)

/* ---------- Memory manager tests ---------- */
static void test_mm(void) {
    uint32_t total = mm_get_total_pages();
    uint32_t free0 = mm_get_free_pages();
    uint32_t used0 = mm_get_used_pages();

    void* p = mm_alloc_page();
    CHECK(p != NULL, "mm_alloc_page returns non-NULL");
    CHECK(mm_get_used_pages() == used0 + 1, "mm used count increments by 1");
    CHECK(mm_get_free_pages() == free0 - 1, "mm free count decrements by 1");

    mm_free_page(p);
    CHECK(mm_get_used_pages() == used0, "mm used count restored after free");
    CHECK(mm_get_free_pages() == free0, "mm free count restored after free");

    void* p4 = mm_alloc_pages(4);
    CHECK(p4 != NULL, "mm_alloc_pages(4) returns non-NULL");
    CHECK(mm_get_used_pages() == used0 + 4, "mm used count increments by 4");
    CHECK(mm_get_free_pages() == free0 - 4, "mm free count decrements by 4");

    mm_free_pages(p4, 4);
    CHECK(mm_get_used_pages() == used0, "mm used count restored after multi free");
    CHECK(mm_get_free_pages() == free0, "mm free count restored after multi free");
    CHECK(total == free0 + used0, "mm total = free + used");
}

/* ---------- Filesystem tests ---------- */
static void test_fs(void) {
    int n0 = fs_count();

    /* Open non-existent */
    CHECK(fs_open("__nonexistent__") == -1, "fs_open missing file returns -1");

    /* Create */
    int fd = fs_create("__test_a");
    CHECK(fd >= 0, "fs_create returns valid fd");
    CHECK(fs_count() == n0 + 1, "fs_count increments after create");

    /* Duplicate create */
    CHECK(fs_create("__test_a") == -1, "fs_create duplicate returns -1");

    /* Write & read */
    const char* data = "Hello MuOS";
    int len = 0; while (data[len]) len++;
    int w = fs_write(fd, data, len);
    CHECK(w == len, "fs_write returns written length");
    CHECK((int)fs_size(fs_open("__test_a")) == len, "fs_size matches written length");

    char buf[64] = {0};
    int r = fs_read(fd, buf, sizeof(buf) - 1);
    CHECK(r == len, "fs_read returns length");

    int same = 1;
    for (int i = 0; i < len; i++) if (buf[i] != data[i]) same = 0;
    CHECK(same, "fs_read data matches written data");

    /* Delete */
    CHECK(fs_delete("__test_a") == 0, "fs_delete succeeds");
    CHECK(fs_count() == n0, "fs_count restored after delete");
    CHECK(fs_open("__test_a") == -1, "deleted file no longer openable");
}

/* ---------- VGA/text-mode tests ---------- */
static void test_vga(void) {
    uint8_t color = vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_setcolor(color);

    int ok_set = 0, ok_adv = 0, ok_nl = 0;

    /* set_cursor test */
    vga_set_cursor(5, 0);
    ok_set = (vga_get_cursor_row() == 5 && vga_get_cursor_col() == 0);

    /* advance col test */
    vga_set_cursor(5, 0);
    vga_putchar('A');
    ok_adv = (vga_get_cursor_col() == 1);

    /* newline test */
    vga_set_cursor(5, 0);
    vga_putchar('\n');
    ok_nl = (vga_get_cursor_row() == 6 && vga_get_cursor_col() == 0);

    if (ok_set) tpass("vga_set_cursor"); else tfail("vga_set_cursor");
    if (ok_adv) tpass("vga_putchar advances col"); else tfail("vga_putchar advances col");
    if (ok_nl) tpass("vga_putchar newline advances row and resets col"); else tfail("vga_putchar newline advances row and resets col");
}

/* ---------- Keyboard tests ---------- */
static void test_keyboard(void) {
    keyboard_flush();
    CHECK(keyboard_haschar() == 0, "keyboard_flush empties buffer");
}

/* ---------- PIT tests ---------- */
static void test_pit(void) {
    uint32_t t1 = pit_get_ticks();
    for (volatile int i = 0; i < 1000; i++) __asm__ volatile("nop");
    uint32_t t2 = pit_get_ticks();
    CHECK(t2 >= t1, "pit ticks are monotonic");
}

/* ---------- Task info tests ---------- */
static void test_task_info(void) {
    CHECK(task_get_count() >= 1, "at least one task exists");
    task_t* t = task_get_current();
    CHECK(t != NULL, "task_get_current returns non-NULL");
    CHECK(t->pid > 0, "current task pid > 0");
    CHECK(t->name[0] != '\0', "current task has a name");
}

/* ---------- Kernel heap tests ---------- */
static void test_heap(void) {
    if (!kheap_ready()) {
        tfail("kernel heap initialized");
        return;
    }
    tpass("kernel heap initialized");

    void* a = kmalloc(1);
    CHECK(a != NULL, "kmalloc(1) returns non-NULL");
    CHECK(((uint32_t)a & 15) == 0, "kmalloc payload is 16-byte aligned");
    kfree(a);

    void* b = kmalloc(17);
    CHECK(b != NULL && ((uint32_t)b & 15) == 0, "kmalloc(17) non-NULL and aligned");
    if (b) {
        for (int i = 0; i < 17; i++) ((uint8_t*)b)[i] = 0xA5;  /* sentinel */
        kfree(b);
    }

    void* c = kcalloc(8, 8);
    CHECK(c != NULL, "kcalloc(8,8) non-NULL");
    if (c) {
        bool_t zeroed = true;
        for (int i = 0; i < 64; i++) if (((uint8_t*)c)[i]) zeroed = false;
        CHECK(zeroed, "kcalloc memory is zeroed");
        kfree(c);
    }

    /* Churn: 100 small allocations then all freed */
    void* blocks[100];
    bool_t all = true;
    for (int i = 0; i < 100; i++) {
        blocks[i] = kmalloc(24);
        if (!blocks[i]) all = false;
    }
    for (int i = 0; i < 100; i++) kfree(blocks[i]);
    CHECK(all, "100 small allocations succeed and free");

    /* Double free and out-of-range free must be ignored safely */
    void* d = kmalloc(32);
    kfree(d);
    kfree(d);
    kfree((void*)0x1);
    void* e = kmalloc(32);
    CHECK(e != NULL, "heap usable after double-free attempt");
    kfree(e);

    CHECK(kmalloc(2u * 1024 * 1024) == NULL, "oversized kmalloc returns NULL");
}

/* ---------- Paging beyond 4MB ---------- */
static void test_paging_high(void) {
    if (mm_get_total_pages() < 1024) {
        tpass("paging high address test skipped (RAM < 4MB)");
        return;
    }
    /* First-fit hands out frames in ascending order, so allocating
     * ~4MB worth must push the allocator past the old 4MB mapping
     * limit. Writing the highest frame proves the identity map works
     * there (any mapping gap would page-fault instantly). */
    void* pages[1100];
    uint32_t n = 0;
    uint32_t hi = 0;
    uint32_t free0 = mm_get_free_pages();
    for (; n < 1100; n++) {
        pages[n] = mm_alloc_page();
        if (!pages[n]) break;
        if ((uint32_t)pages[n] > hi) hi = (uint32_t)pages[n];
    }
    CHECK(hi >= 0x400000, "identity map reaches frames above 4MB");
    if (hi >= 0x400000) {
        volatile uint32_t* p = (volatile uint32_t*)hi;
        *p = 0xA5A5A5A5;
        CHECK(*p == 0xA5A5A5A5, "write/read frame above 4MB");
        CHECK(mm_virt_to_phys(hi) == hi, "virt_to_phys identity above 4MB");
    }
    for (uint32_t i = 0; i < n; i++) mm_free_page(pages[i]);
    CHECK(mm_get_free_pages() == free0, "free count restored after test");
}

/* ---------- Memory map coverage ---------- */
static void test_mmap(void) {
    CHECK(mm_get_free_pages() > 0, "free frames available");
    /* The bitmap must cover at least what the loader reported
     * (mem_upper + first MB), with one page of rounding slack. */
    uint64_t covered = (uint64_t)mm_get_total_pages() * PAGE_SIZE + PAGE_SIZE;
    uint64_t reported = ((uint64_t)mm_get_mem_upper_kb() + 1024) * 1024;
    CHECK(covered >= reported, "bitmap covers reported RAM");
}

int tests_run(void) {
    passed = 0; failed = 0;

    vga_setcolor(vga_entry_color(VGA_YELLOW, VGA_BLACK));
    vga_print("\n--- MuOS Self-Test Suite ---\n");
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
    serial_write("--- MuOS Self-Test Suite ---\n");

    /* Disable interrupts during tests so the scheduler does not
     * switch away while we are checking shared kernel state. */
    __asm__ volatile ("cli");

    test_mm();
    test_heap();
    test_paging_high();
    test_mmap();
    test_fs();
    test_vga();
    test_keyboard();
    test_pit();
    test_task_info();

    __asm__ volatile ("sti");

    vga_setcolor(vga_entry_color(VGA_YELLOW, VGA_BLACK));
    vga_print("--- Results: ");
    vga_print_dec(passed);
    vga_print(" passed, ");
    vga_print_dec(failed);
    vga_print(" failed ---\n\n");
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));

    serial_write("--- Results: ");
    { char nb[12]; u32_to_dec(nb, (uint32_t)passed); serial_write(nb); }
    serial_write(" passed, ");
    { char nb[12]; u32_to_dec(nb, (uint32_t)failed); serial_write(nb); }
    serial_write(" failed ---\n\n");

    return failed;
}
