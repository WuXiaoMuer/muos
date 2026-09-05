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
#include "multiboot.h"
#include "kheap.h"
#include "task.h"
#include "shell.h"
#include "test.h"
#include "string.h"

/* Linker symbol - end of kernel image */
extern uint32_t _kernel_end;

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
    vga_print("             MuOS Kernel v0.3\n");
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
    const multiboot_info_t* mb = NULL;
    if (magic == MULTIBOOT_MAGIC && mb_info_addr != 0) {
        mb = (const multiboot_info_t*)mb_info_addr;
    }

    /* Align kernel_end to page boundary */
    uint32_t kernel_end = ((uint32_t)&_kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    mm_init(mb, kernel_end);
    vga_print("[OK] Physical memory: "); vga_print_dec(mm_get_total_pages() * 4);
    vga_print(" KB total, "); vga_print_dec(mm_get_free_pages() * 4);
    vga_print(" KB free\n");
    if (mm_get_mmap_regions() > 0) {
        vga_print("[OK] Memory map: "); vga_print_dec(mm_get_mmap_regions());
        vga_print(" usable region(s)\n");
    } else {
        vga_print("[WARN] No multiboot memory map, using mem_upper fallback\n");
    }
    serial_write("[OK] Physical memory manager initialized\n");

    /* 5. Enable paging */
    mm_enable_paging();
    vga_print("[OK] Paging enabled (identity map)\n");
    serial_write("[OK] Paging enabled\n");

    /* 5b. Kernel heap (needs paging: heap pages live anywhere in RAM) */
    kheap_init();
    if (kheap_ready()) {
        vga_print("[OK] Kernel heap (1MB)\n");
        serial_write("[OK] Kernel heap initialized\n");
    } else {
        vga_print("[WARN] Kernel heap unavailable\n");
        serial_write("[WARN] Kernel heap unavailable\n");
    }

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
     * allocated stack) was unreliable — the physical frames handed out
     * above 4MB were not mapped by the page tables, so touching the
     * stack page faulted. For a single-task system, calling shell_run()
     * from the kernel stack works fine; real multitasking returns with
     * the user-mode milestone (see ROADMAP.md). */

    /* Rename the registered task to "shell" so the running entity
     * still appears with the right name in `tasks` / `ps`. */
    if (task_get_current()) {
        task_t* t = task_get_current();
        strncpy(t->name, "shell", TASK_NAME_MAX - 1);
        t->name[TASK_NAME_MAX - 1] = '\0';
    }

    /* The shell runs as a normal C call from the boot task entry
     * (kernel stack), not as a context-switched task. The scheduler
     * and task structs are kept around for `tasks` / `ps` / `task_*`
     * introspection but no actual switching happens. */
    __asm__ volatile ("cld; sti");
    shell_run();
    for (;;) { __asm__ volatile ("hlt"); }
}
