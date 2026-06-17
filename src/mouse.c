#include "mouse.h"
#include "io.h"
#include "irq.h"
#include "pic.h"

/* Mouse state */
static volatile int16_t mx = 160, my = 100;  /* Start centered */
static volatile int8_t  btn_l = 0, btn_r = 0, btn_m = 0;

/* PS/2 mouse packet decoder state machine (0-2) */
static volatile uint8_t mcycle = 0;
static uint8_t mbytes[3];

/* Wait for PS/2 controller input buffer to be empty */
static void mouse_wait_write(void) {
    int timeout = 100000;
    while ((inb(0x64) & 0x02) && --timeout) {
        __asm__ volatile ("pause");
    }
}

/* Wait for PS/2 controller output buffer to be full */
static void mouse_wait_read(void) {
    int timeout = 100000;
    while (!(inb(0x64) & 0x01) && --timeout) {
        __asm__ volatile ("pause");
    }
}

/* Send command to PS/2 controller (port 0x64) */
static void mouse_write_cmd(uint8_t cmd) {
    mouse_wait_write();
    outb(0x64, cmd);
}

/* Send data to PS/2 device (port 0x60) */
static void mouse_write_data(uint8_t data) {
    mouse_wait_write();
    outb(0x60, data);
}

/* Read data from PS/2 device */
static uint8_t mouse_read_data(void) {
    mouse_wait_read();
    return inb(0x60);
}

/* IRQ12 handler — mouse data available */
static void mouse_handler(registers_t* regs) {
    (void)regs;
    uint8_t status = inb(MOUSE_STATUS);
    if (!(status & 0x01)) return;  /* No data */
    if (!(status & 0x20)) return;  /* Not from mouse */

    uint8_t data = inb(MOUSE_PORT);
    switch (mcycle) {
    case 0:
        /* Byte 1: flags */
        if (!(data & 0x08)) return; /* Bit 3 must be 1 (always set) */
        mbytes[0] = data;
        mcycle = 1;
        break;
    case 1:
        /* Byte 2: X movement */
        mbytes[1] = data;
        mcycle = 2;
        break;
    case 2:
        /* Byte 3: Y movement */
        mbytes[2] = data;
        mcycle = 0;

        /* Decode packet */
        int8_t xm = (int8_t)mbytes[1];
        int8_t ym = (int8_t)mbytes[2];

        /* X/Y sign bits in byte 0 */
        if (mbytes[0] & 0x10) xm |= 0xFFFFFF00; /* Sign extend for 16-bit */
        /* Actually xm is already int8_t, just need to widen */
        int16_t dx = (int16_t)xm;
        int16_t dy = (int16_t)ym;

        /* Y is inverted in PS/2 protocol */
        mx += dx;
        my -= dy;

        /* Clamp to screen */
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx > 639) mx = 639;
        if (my > 199) my = 199;

        /* Button states */
        btn_l = (mbytes[0] & 0x01) ? 1 : 0;
        btn_r = (mbytes[0] & 0x02) ? 1 : 0;
        btn_m = (mbytes[0] & 0x04) ? 1 : 0;
        break;
    }
}

void mouse_init(void) {
    mx = 160; my = 100;
    btn_l = btn_r = btn_m = 0;
    mcycle = 0;

    /* 1. Enable auxiliary PS/2 port (mouse) */
    mouse_write_cmd(0xA8);

    /* 2. Enable IRQ12 for mouse */
    mouse_write_cmd(0x20);
    uint8_t cfg = mouse_read_data();
    cfg |= 0x02;   /* Enable IRQ12 */
    cfg &= ~0x20;  /* Disable mouse clock (will enable) */
    mouse_write_cmd(0x60);
    mouse_write_data(cfg);

    /* 3. Enable mouse data reporting */
    mouse_write_cmd(0xD4);  /* Next byte goes to mouse */
    mouse_write_data(0xF4);  /* Enable data reporting */
    mouse_read_data();       /* Read ACK (0xFA) */

    /* 4. Set sample rate to 100/sec */
    mouse_write_cmd(0xD4); mouse_write_data(0xF3); mouse_read_data();
    mouse_write_cmd(0xD4); mouse_write_data(100);  mouse_read_data();

    /* 5. Register IRQ12 handler */
    irq_register_handler(12, mouse_handler);
}

mouse_state_t mouse_get_state(void) {
    mouse_state_t s;
    s.x = mx; s.y = my;
    s.btn_left = btn_l;
    s.btn_right = btn_r;
    s.btn_middle = btn_m;
    return s;
}

void mouse_wait_click(void) {
    while (!btn_l) { __asm__ volatile ("hlt"); }
    while (btn_l)  { __asm__ volatile ("hlt"); }
}
