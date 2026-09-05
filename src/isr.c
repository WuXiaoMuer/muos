#include "isr.h"
#include "vga.h"
#include "serial.h"

const char* exception_names[32] = {
    "Division By Zero",
    "Debug",
    "NMI",
    "Breakpoint",
    "Overflow",
    "Bound Range",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment",
    "Invalid TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection",
    "Page Fault",
    "Reserved",
    "x87 FPU Error",
    "Alignment Check",
    "Machine Check",
    "SIMD Exception",
    "Virtualization",
    "Control Protection",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection",
    "VMM Communication",
    "Security Exception",
    "Reserved"
};

/* Format uint32_t as "0xXXXXXXXX" into buf (min 11 bytes) */
static const char* hex32(uint32_t n, char* buf) {
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        uint8_t nibble = (n >> ((7 - i) * 4)) & 0xF;
        buf[2 + i] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
    }
    buf[10] = '\0';
    return buf;
}

void isr_handler(registers_t* regs) {
    /* Exceptions are fatal: print state (VGA + serial) and halt.
     * Serial copy keeps the dump in a file for post-mortem analysis. */
    char head[80];

    /* Page fault: CR2 holds the faulting address */
    uint32_t cr2 = 0;
    if (regs->int_no == 14) {
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    }

    vga_setcolor(vga_entry_color(VGA_WHITE, VGA_RED));
    vga_print("\n[KERNEL PANIC] Exception: ");
    vga_print(exception_names[regs->int_no]);
    vga_print(" (");
    vga_print_dec(regs->int_no);
    vga_print(")\n");
    vga_print("  EIP: "); vga_print_hex(regs->eip);
    vga_print("  CS: ");  vga_print_hex(regs->cs);
    vga_print("  EFLAGS: "); vga_print_hex(regs->eflags);
    if (regs->int_no == 14) {
        vga_print("\n  CR2: "); vga_print_hex(cr2);
    }
    vga_print("\n  EAX: "); vga_print_hex(regs->eax);
    vga_print("  EBX: "); vga_print_hex(regs->ebx);
    vga_print("\n  ECX: "); vga_print_hex(regs->ecx);
    vga_print("  EDX: "); vga_print_hex(regs->edx);
    vga_print("\n  ESI: "); vga_print_hex(regs->esi);
    vga_print("  EDI: "); vga_print_hex(regs->edi);
    vga_print("\n  Error code: "); vga_print_hex(regs->err_code);
    vga_print("\n  ESP: "); vga_print_hex(regs->esp);
    vga_print("\n\nSystem halted.\n");

    serial_write("\n[KERNEL PANIC] Exception: ");
    serial_write(exception_names[regs->int_no]);
    serial_write("\n  EIP=");  serial_write(hex32(regs->eip, head));
    serial_write(" CS=");      serial_write(hex32(regs->cs, head));
    serial_write(" EFLAGS=");  serial_write(hex32(regs->eflags, head));
    if (regs->int_no == 14) {
        serial_write(" CR2="); serial_write(hex32(cr2, head));
    }
    serial_write("\n  EAX=");  serial_write(hex32(regs->eax, head));
    serial_write(" EBX=");     serial_write(hex32(regs->ebx, head));
    serial_write(" ECX=");     serial_write(hex32(regs->ecx, head));
    serial_write(" EDX=");     serial_write(hex32(regs->edx, head));
    serial_write("\n  ESI=");  serial_write(hex32(regs->esi, head));
    serial_write(" EDI=");     serial_write(hex32(regs->edi, head));
    serial_write(" ESP=");     serial_write(hex32(regs->esp, head));
    serial_write(" ERR=");     serial_write(hex32(regs->err_code, head));
    serial_write("\n\nSystem halted.\n");

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
