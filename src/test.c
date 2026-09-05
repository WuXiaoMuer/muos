#include "test.h"
#include "types.h"
#include "vga.h"
#include "serial.h"
#include "mm.h"
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
