#ifndef MUOS_PIC_H
#define MUOS_PIC_H
#include "types.h"

#define PIC1            0x20    /* Master PIC */
#define PIC2            0xA0    /* Slave PIC */
#define PIC1_COMMAND    PIC1
#define PIC1_DATA       (PIC1 + 1)
#define PIC2_COMMAND    PIC2
#define PIC2_DATA       (PIC2 + 1)

#define PIC_EOI         0x20    /* End-of-interrupt command */

/* Remap PIC: master IRQ0-7 -> INT 0x20-0x27, slave IRQ8-15 -> INT 0x28-0x2F */
#define IRQ0            0x20
#define IRQ1            0x21
#define IRQ2            0x22
#define IRQ3            0x23
#define IRQ4            0x24
#define IRQ5            0x25
#define IRQ6            0x26
#define IRQ7            0x27
#define IRQ8            0x28
#define IRQ9            0x29
#define IRQ10           0x2A
#define IRQ11           0x2B
#define IRQ12           0x2C
#define IRQ13           0x2D
#define IRQ14           0x2E
#define IRQ15           0x2F

void pic_init(void);
void pic_send_eoi(uint8_t irq);
void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);
void pic_disable(void);

#endif
