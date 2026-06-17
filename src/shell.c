#include "shell.h"
#include "vga.h"
#include "keyboard.h"
#include "pit.h"
#include "mm.h"
#include "task.h"

#define CMD_MAX     64
#define HIST_MAX    256

static char cmd_buffer[CMD_MAX];
static int  cmd_pos = 0;

static void shell_prompt(void) {
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("muos> ");
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
}

static void shell_newline(void) {
    vga_putchar('\n');
}

static void shell_readline(void) {
    cmd_pos = 0;
    shell_prompt();

    for (;;) {
        char c = keyboard_getchar();

        if (c == '\n' || c == '\r') {
            cmd_buffer[cmd_pos] = '\0';
            shell_newline();
            break;
        } else if (c == '\b') {
            if (cmd_pos > 0) {
                cmd_pos--;
                vga_putchar('\b');
            }
        } else if (c == '\t') {
            /* Tab completion: just ignore for now */
        } else if (c >= ' ' && c < 127) {
            if (cmd_pos < CMD_MAX - 1) {
                cmd_buffer[cmd_pos++] = c;
                vga_putchar(c);
            }
        }
    }
}

static void cmd_help(void) {
    vga_print("Available commands:\n");
    vga_print("  help    - Show this help\n");
    vga_print("  clear   - Clear the screen\n");
    vga_print("  echo    - Print text to screen\n");
    vga_print("  mem     - Show memory statistics\n");
    vga_print("  tasks   - List running tasks\n");
    vga_print("  time    - Show system uptime\n");
    vga_print("  reboot  - Reboot the system\n");
    vga_print("  logo    - Show MuOS logo\n");
}

static void cmd_clear(void) {
    vga_clear();
}

static void cmd_echo(void) {
    /* Echo everything after "echo " */
    char* arg = cmd_buffer + 4;
    while (*arg == ' ') arg++;
    vga_print(arg);
    vga_putchar('\n');
}

static void cmd_mem(void) {
    vga_print("Memory Statistics:\n");
    vga_print("  Total pages: "); vga_print_dec(mm_get_total_pages());
    vga_print("\n  Used pages:  "); vga_print_dec(mm_get_used_pages());
    vga_print("\n  Free pages:  "); vga_print_dec(mm_get_free_pages());
    vga_print("\n  Total KB:    "); vga_print_dec(mm_get_total_pages() * 4);
    vga_print("\n  Free KB:     "); vga_print_dec(mm_get_free_pages() * 4);
    vga_putchar('\n');
}

static void cmd_tasks(void) {
    vga_print("Tasks: "); vga_print_dec(task_get_count()); vga_putchar('\n');
    task_list();
}

static void cmd_time(void) {
    uint32_t ticks = pit_get_ticks();
    uint32_t seconds = ticks / 100;
    uint32_t minutes = seconds / 60;
    uint32_t hours = minutes / 60;

    vga_print("Uptime: ");
    vga_print_dec(hours); vga_print("h ");
    vga_print_dec(minutes % 60); vga_print("m ");
    vga_print_dec(seconds % 60); vga_print("s (");
    vga_print_dec(ticks); vga_print(" ticks)\n");
}

static void cmd_reboot(void) {
    vga_print("Rebooting...\n");

    /* Wait a bit for the message to display */
    for (volatile int i = 0; i < 10000000; i++) { __asm__ volatile ("nop"); }

    /* Triple fault to reboot */
    /* Load a null IDT and trigger an interrupt */
    uint8_t zero_idt[6] = {0, 0, 0, 0, 0, 0};
    __asm__ volatile ("lidt %0" : : "m"(zero_idt));
    __asm__ volatile ("int $0");
}

static void cmd_logo(void) {
    vga_setcolor(vga_entry_color(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print("\n");
    vga_print("  __  __       ___  ____  \n");
    vga_print(" |  \\/  |_   _/ _ \\/ ___| \n");
    vga_print(" | |\\/| | | | | | | \\___ \\ \n");
    vga_print(" | |  | | |_| | |_| |___) |\n");
    vga_print(" |_|  |_|\\__,_|\\___/|____/ \n");
    vga_print("\n");
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
    vga_print("MuOS v0.2 - x86 32-bit Microkernel\n");
    vga_print("GDT / IDT / PIC / PIT / MMU / Multitasking\n\n");
}

static void shell_execute(void) {
    if (cmd_pos == 0) return;

    if (cmd_buffer[0] == 'h' && cmd_buffer[1] == 'e' && cmd_buffer[2] == 'l' &&
        cmd_buffer[3] == 'p' && (cmd_buffer[4] == '\0' || cmd_buffer[4] == ' ')) {
        cmd_help();
    } else if (cmd_buffer[0] == 'c' && cmd_buffer[1] == 'l' && cmd_buffer[2] == 'e' &&
               cmd_buffer[3] == 'a' && cmd_buffer[4] == 'r') {
        cmd_clear();
    } else if (cmd_buffer[0] == 'e' && cmd_buffer[1] == 'c' && cmd_buffer[2] == 'h' &&
               cmd_buffer[3] == 'o') {
        cmd_echo();
    } else if (cmd_buffer[0] == 'm' && cmd_buffer[1] == 'e' && cmd_buffer[2] == 'm') {
        cmd_mem();
    } else if (cmd_buffer[0] == 't' && cmd_buffer[1] == 'a' && cmd_buffer[2] == 's' &&
               cmd_buffer[3] == 'k' && cmd_buffer[4] == 's') {
        cmd_tasks();
    } else if (cmd_buffer[0] == 't' && cmd_buffer[1] == 'i' && cmd_buffer[2] == 'm' &&
               cmd_buffer[3] == 'e') {
        cmd_time();
    } else if (cmd_buffer[0] == 'r' && cmd_buffer[1] == 'e' && cmd_buffer[2] == 'b' &&
               cmd_buffer[3] == 'o' && cmd_buffer[4] == 'o' && cmd_buffer[5] == 't') {
        cmd_reboot();
    } else if (cmd_buffer[0] == 'l' && cmd_buffer[1] == 'o' && cmd_buffer[2] == 'g' &&
               cmd_buffer[3] == 'o') {
        cmd_logo();
    } else {
        vga_setcolor(vga_entry_color(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("Unknown command: ");
        vga_print(cmd_buffer);
        vga_putchar('\n');
        vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        vga_print("Type 'help' for available commands.\n");
    }
}

void shell_init(void) {
    /* Nothing to init - keyboard and VGA are already set up */
}

void shell_run(void) {
    vga_setcolor(vga_entry_color(VGA_YELLOW, VGA_BLACK));
    vga_print("\n  MuOS Shell v0.1 - Type 'help' for commands.\n\n");
    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));

    for (;;) {
        shell_readline();
        shell_execute();
    }
}
