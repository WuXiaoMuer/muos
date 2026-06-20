/* kernel.c - MuOS Main Kernel (v0.2)
 * x86 32-bit microkernel with GDT/IDT/PIC/PIT/MMU/Multitasking/Shell
 */

#include "types.h"
#include "vga.h"
#include "serial.h"
#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "pic.h"
#include "irq.h"
#include "pit.h"
#include "keyboard.h"
#include "mouse.h"
#include "mm.h"
#include "task.h"
#include "shell.h"
#include "test.h"

/* Multiboot constants */
#define MULTIBOOT_MAGIC    0x2BADB002
#define MULTIBOOT_FLAG_MEM 0x1

/* Multiboot info structure (partial) */
typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    /* ... more fields ... */
} __attribute__((packed)) multiboot_info_t;

/* Linker symbol - end of kernel image */
extern uint32_t _kernel_end;
extern uint32_t task_count;  /* from task.c */

/* Timer IRQ handler */
static void timer_handler(registers_t* regs) {
    (void)regs;
    pit_tick();
    /* Single task — no need to switch context, and switching
     * actually broke the shell task's stack frame layout. */
}


/* Main kernel entry */
void kernel_main(uint32_t magic, uint32_t mb_info_addr) {
    serial_init();
    vga_init();
    vga_clear();

    /* Boot banner - VGA */
    vga_setcolor(vga_entry_color(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print("============================================\n");
    vga_print("             MuOS Kernel v0.2\n");
    vga_print("         x86 32-bit Microkernel\n");
    vga_print("============================================\n\n");
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));

    /* 1. GDT */
    gdt_init();
    vga_print("[OK] GDT initialized\n");
    serial_write("[OK] GDT initialized\n");

    /* 2. IDT */
    idt_init();
    vga_print("[OK] IDT initialized (256 entries)\n");
    serial_write("[OK] IDT initialized\n");

    /* 3. PIC */
    pic_init();
    pic_disable(); /* Mask all IRQs initially */
    vga_print("[OK] PIC remapped (master: 0x20, slave: 0x28)\n");
    serial_write("[OK] PIC remapped\n");

    /* 4. Memory management */
    uint32_t mem_upper_kb = 0;
    if (magic == MULTIBOOT_MAGIC && mb_info_addr != 0) {
        multiboot_info_t* mb_info = (multiboot_info_t*)mb_info_addr;
        if (mb_info->flags & MULTIBOOT_FLAG_MEM) {
            mem_upper_kb = mb_info->mem_upper;
        }
    }
    if (mem_upper_kb == 0) {
        mem_upper_kb = 32768; /* Default: 32 MB upper memory */
        vga_print("[WARN] No multiboot memory info, assuming 32MB\n");
        serial_write("[WARN] No multiboot memory info, assuming 32MB\n");
    }

    /* Align kernel_end to page boundary */
    uint32_t kernel_end = ((uint32_t)&_kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    mm_init(mem_upper_kb, kernel_end);
    vga_print("[OK] Physical memory: "); vga_print_dec(mm_get_total_pages() * 4);
    vga_print(" KB total, "); vga_print_dec(mm_get_free_pages() * 4);
    vga_print(" KB free\n");
    serial_write("[OK] Physical memory manager initialized\n");

    /* 5. Enable paging */
    mm_enable_paging();
    vga_print("[OK] Paging enabled (identity map)\n");
    serial_write("[OK] Paging enabled\n");

    /* 6. PIT timer */
    pit_init(100); /* 100 Hz */
    irq_register_handler(0, timer_handler);
    vga_print("[OK] PIT initialized at 100 Hz\n");
    serial_write("[OK] PIT initialized at 100 Hz\n");

    /* 7. Keyboard */
    keyboard_init();
    vga_print("[OK] Keyboard\n");

    /* 8. Mouse */
    mouse_init();
    vga_print("[OK] Mouse\n");

    /* 9. Multitasking */
    task_init();
    vga_print("[OK] Scheduler\n");
    serial_write("[OK] Scheduler started\n");

    /* 9c. Built-in self-test suite (runs before entering shell).
     * Register the kernel as a task first so the task tests pass. */
    task_register("kernel");
    tests_run();

    /* Boot animation */
    vga_setcolor(vga_entry_color(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print("\n  Starting MuOS 7");
    for (int i = 0; i < 6; i++) {
        for (volatile int d = 0; d < 2000000; d++) __asm__ volatile("nop");
        vga_putchar('.');
    }
    vga_print("\n\n");
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));

    /* 10. Enter shell directly on the kernel stack. The original
     * task-stack handoff (built a fake interrupt frame in a freshly
     * mm_alloc_page'd stack) was unreliable and the source of the
     * post-Starting splash crash. For a single-task system, calling
     * shell_run() from the kernel stack works fine. */
    need_reschedule = 0;

    /* Rename the registered task to "shell" so the running entity
     * still appears with the right name in `tasks` / `ps`. */
    if (task_get_current()) {
        task_t* t = task_get_current();
        for (int i = 0; i < TASK_NAME_MAX - 1 && "shell"[i]; i++) t->name[i] = "shell"[i];
        t->name[5] = '\0';
    }

    /* The shell runs as a normal C call from the boot task entry
     * (kernel stack), not as a context-switched task. The scheduler
     * and task structs are kept around for `tasks` / `ps` / `task_*`
     * introspection but no actual switching happens. */
    __asm__ volatile ("cld; sti");
    shell_run();
    for (;;) { __asm__ volatile ("hlt"); }
}
