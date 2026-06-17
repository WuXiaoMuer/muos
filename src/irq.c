#include "irq.h"
#include "pic.h"
#include "io.h"

static irq_handler_t irq_handlers[16];

void irq_register_handler(uint8_t irq, irq_handler_t handler) {
    if (irq < 16) {
        irq_handlers[irq] = handler;
        pic_unmask(irq);
    }
}

void irq_unregister_handler(uint8_t irq) {
    if (irq < 16) {
        irq_handlers[irq] = NULL;
        pic_mask(irq);
    }
}

/* Called from assembly irq_common stub */
void irq_handler(registers_t* regs) {
    uint8_t irq = regs->int_no - 0x20;

    /* Handle spurious IRQ7/IRQ15 */
    if (irq == 7) {
        uint8_t isr = inb(0x20) & 0x80; /* Check master PIC ISR bit 7 */
        if (!isr) {
            return; /* Spurious - don't send EOI to master */
        }
    }
    if (irq == 15) {
        uint8_t isr = inb(0xA0) & 0x80; /* Check slave PIC ISR bit 7 */
        if (!isr) {
            pic_send_eoi(7); /* Send EOI to master only */
            return;
        }
    }

    if (irq_handlers[irq] != NULL) {
        irq_handlers[irq](regs);
    }

    pic_send_eoi(irq);
}
