#ifndef MUOS_SYSCALL_H
#define MUOS_SYSCALL_H
#include "isr.h"

/* int 0x80 entry stub (isr_stubs.s), DPL=3 in the IDT. */
extern void syscall_entry(void);

/* Assembly stub calls this with the full interrupt frame; return values
 * are written back into regs->eax by the handler itself. */
void syscall_handler(registers_t* regs);

#endif
