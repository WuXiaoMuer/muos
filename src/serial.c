#include "serial.h"
#include "io.h"

#define COM1_PORT 0x3F8

static int serial_initialized = 0;

void serial_init(void) {
    outb(COM1_PORT + 1, 0x00);    /* Disable interrupts */
    outb(COM1_PORT + 3, 0x80);    /* Enable DLAB */
    outb(COM1_PORT + 0, 0x03);    /* 38400 baud (lo) */
    outb(COM1_PORT + 1, 0x00);    /* 38400 baud (hi) */
    outb(COM1_PORT + 3, 0x03);    /* 8N1 */
    outb(COM1_PORT + 2, 0xC7);    /* Enable FIFO */
    outb(COM1_PORT + 4, 0x0B);    /* IRQs, RTS/DSR */
    serial_initialized = 1;
}

static int serial_is_transmit_empty(void) {
    return inb(COM1_PORT + 5) & 0x20;
}

void serial_putchar(char c) {
    if (!serial_initialized) return;
    while (!serial_is_transmit_empty()) {
        __asm__ volatile ("pause");
    }
    outb(COM1_PORT, (uint8_t)c);
}

void serial_write(const char* str) {
    for (int i = 0; str[i] != '\0'; i++) {
        serial_putchar(str[i]);
    }
}
