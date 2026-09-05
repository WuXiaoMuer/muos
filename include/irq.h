#ifndef MUOS_IRQ_H
#define MUOS_IRQ_H
#include "types.h"
#include "isr.h"

typedef void (*irq_handler_t)(registers_t* regs);

void irq_register_handler(uint8_t irq, irq_handler_t handler);

/* Assembly stubs */
extern void irq0(void);   extern void irq1(void);
extern void irq2(void);   extern void irq3(void);
extern void irq4(void);   extern void irq5(void);
extern void irq6(void);   extern void irq7(void);
extern void irq8(void);   extern void irq9(void);
extern void irq10(void);  extern void irq11(void);
extern void irq12(void);  extern void irq13(void);
extern void irq14(void);  extern void irq15(void);

#endif
