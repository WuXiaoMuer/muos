#include "mouse.h"
#include "io.h"

/* PS/2 controller (8042) ports */
#define PS2_STATUS 0x64
#define PS2_DATA   0x60
#define PS2_CMD    0x64

/* Virtual screen pixels: 640x400 so cells are 8x16. */
#define MOUSE_MAX_X 639
#define MOUSE_MAX_Y 399

static volatile int px = 320, py = 352;   /* cell 40,22: empty desktop strip */
static volatile int8_t btn_l, btn_r, btn_m;

static uint8_t packet[3];
static int pidx;

static void ps2_wait_write(void) {
    for (int i = 0; i < 100000; i++)
        if (!(inb(PS2_STATUS) & 0x02)) return;
}

static void ps2_wait_read(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & 0x01) return;
}

static uint8_t ps2_read(void) {
    ps2_wait_read();
    return inb(PS2_DATA);
}

static void ps2_write_cmd(uint8_t c) {
    ps2_wait_write();
    outb(PS2_CMD, c);
}

static void ps2_write_data(uint8_t d) {
    ps2_wait_write();
    outb(PS2_DATA, d);
}

/* Send a byte to the auxiliary device and consume its ACK. */
static void mouse_cmd(uint8_t c) {
    ps2_write_cmd(0xD4);
    ps2_write_data(c);
    (void)ps2_read();
}

bool_t mouse_init(void) {
    /* Start clean: drain any pending keyboard/mouse bytes. */
    while (inb(PS2_STATUS) & 0x01) (void)inb(PS2_DATA);
    pidx = 0;
    btn_l = btn_r = btn_m = 0;
    px = 320; py = 352;

    /* Enable the auxiliary (mouse) port. */
    ps2_write_cmd(0xA8);

    /* Read-modify-write the controller command byte: enable the aux
     * interrupt (bit1) and make sure the aux clock is not disabled. */
    ps2_write_cmd(0x20);
    uint8_t cmd = ps2_read();
    cmd |= 0x02;    /* IRQ12 on */
    cmd |= 0x01;    /* keyboard IRQ stays on */
    cmd &= (uint8_t)~0x20;
    ps2_write_cmd(0x60);
    ps2_write_data(cmd);

    /* Mouse: defaults, then enable data reporting. */
    mouse_cmd(0xF6);
    mouse_cmd(0xF4);

    return true;
}

void mouse_handle_byte(uint8_t data) {
    /* First packet byte must have bit3 set (sync). */
    if (pidx == 0 && !(data & 0x08)) return;

    packet[pidx++] = data;
    if (pidx < 3) return;
    pidx = 0;

    uint8_t flags = packet[0];

    /* dx/dy are 9-bit two's-complement; bits 6/7 signal overflow. */
    int dx = (int)packet[1] - ((flags & 0x10) ? 256 : 0);
    int dy = (int)packet[2] - ((flags & 0x20) ? 256 : 0);

    if (!(flags & 0xC0)) {
        /* PS/2 +y is up, screen y grows down. Scale 2x for reachability
         * on a 640x400 virtual screen. */
        px += dx * 2;
        py -= dy * 2;
        if (px < 0) px = 0;
        if (px > MOUSE_MAX_X) px = MOUSE_MAX_X;
        if (py < 0) py = 0;
        if (py > MOUSE_MAX_Y) py = MOUSE_MAX_Y;
    }

    btn_l = (flags & 0x01) ? 1 : 0;
    btn_r = (flags & 0x02) ? 1 : 0;
    btn_m = (flags & 0x04) ? 1 : 0;
}

mouse_state_t mouse_get_state(void) {
    mouse_state_t s;
    s.x = (int16_t)(px / 8);
    s.y = (int16_t)(py / 16);
    s.btn_left = btn_l;
    s.btn_right = btn_r;
    s.btn_middle = btn_m;
    return s;
}
