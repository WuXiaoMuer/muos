#include "isr.h"
#include "vga.h"

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

void isr_handler(registers_t* regs) {
    /* Exceptions are fatal: print state and halt */

    /* Default: print exception info */
    vga_setcolor(vga_entry_color(VGA_WHITE, VGA_RED));
    vga_print("\n[KERNEL PANIC] Exception: ");
    vga_print(exception_names[regs->int_no]);
    vga_print(" (");
    vga_print_dec(regs->int_no);
    vga_print(")\n");
    vga_print("  EIP: "); vga_print_hex(regs->eip);
    vga_print("  CS: ");  vga_print_hex(regs->cs);
    vga_print("  EFLAGS: "); vga_print_hex(regs->eflags);
    vga_print("\n  EAX: "); vga_print_hex(regs->eax);
    vga_print("  EBX: "); vga_print_hex(regs->ebx);
    vga_print("\n  ECX: "); vga_print_hex(regs->ecx);
    vga_print("  EDX: "); vga_print_hex(regs->edx);
    vga_print("\n  ESI: "); vga_print_hex(regs->esi);
    vga_print("  EDI: "); vga_print_hex(regs->edi);
    vga_print("\n  Error code: "); vga_print_hex(regs->err_code);
    vga_print("\n\nSystem halted.\n");

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
