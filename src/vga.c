#include "vga.h"
#include "io.h"

static uint16_t* const VGA_BUFFER = (uint16_t*)0xB8000;
static uint8_t    vga_color;
static volatile uint16_t   vga_row;
static volatile uint16_t   vga_col;

uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return (bg << 4) | fg;
}

uint16_t vga_entry(unsigned char c, uint8_t color) {
    return ((uint16_t)color << 8) | (uint16_t)c;
}

void vga_init(void) {
    vga_row = 0;
    vga_col = 0;
    vga_color = vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
}

void vga_setcolor(uint8_t color) {
    vga_color = color;
}

uint8_t vga_getcolor(void) {
    return vga_color;
}

void vga_clear(void) {
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_BUFFER[i] = vga_entry(' ', vga_color);
    }
    vga_row = 0;
    vga_col = 0;
    vga_update_cursor();
}

void vga_scroll(void) {
    if (vga_row >= VGA_HEIGHT) {
        /* Move all lines up by one */
        for (size_t i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++) {
            VGA_BUFFER[i] = VGA_BUFFER[i + VGA_WIDTH];
        }
        /* Clear the last line */
        for (size_t i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++) {
            VGA_BUFFER[i] = vga_entry(' ', vga_color);
        }
        vga_row = VGA_HEIGHT - 1;
    }
}

void vga_update_cursor(void) {
    uint16_t pos = vga_row * VGA_WIDTH + vga_col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void vga_putchar(char c) {
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (c == '\r') {
        vga_col = 0;
    } else if (c == '\t') {
        vga_col = (vga_col + 4) & ~3;
        if (vga_col >= VGA_WIDTH) {
            vga_col = 0;
            vga_row++;
        }
    } else if (c == '\b') {
        if (vga_col > 0) {
            vga_col--;
            VGA_BUFFER[vga_row * VGA_WIDTH + vga_col] = vga_entry(' ', vga_color);
        }
    } else {
        VGA_BUFFER[vga_row * VGA_WIDTH + vga_col] = vga_entry((unsigned char)c, vga_color);
        vga_col++;
    }

    if (vga_col >= VGA_WIDTH) {
        vga_col = 0;
        vga_row++;
    }

    vga_scroll();
    vga_update_cursor();
}

void vga_write(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        vga_putchar(data[i]);
    }
}

void vga_print(const char* str) {
    for (size_t i = 0; str[i] != '\0'; i++) {
        vga_putchar(str[i]);
    }
}

void vga_print_hex(uint32_t n) {
    char buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    int pos = 10;
    buf[pos--] = '\0';

    for (int i = 0; i < 8; i++) {
        uint8_t nibble = (n >> (i * 4)) & 0xF;
        buf[pos--] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
    }
    vga_print(buf);
}

void vga_print_dec(uint32_t n) {
    if (n == 0) {
        vga_putchar('0');
        return;
    }
    char buf[11];
    int pos = 10;
    buf[pos--] = '\0';
    uint32_t val = n;
    while (val > 0 && pos >= 0) {
        buf[pos--] = '0' + (val % 10);
        val /= 10;
    }
    vga_print(&buf[pos + 1]);
}

uint16_t vga_get_cursor_row(void) { return vga_row; }
uint16_t vga_get_cursor_col(void) { return vga_col; }

void vga_set_cursor(uint16_t row, uint16_t col) {
    vga_row = row;
    vga_col = col;
    vga_update_cursor();
}

void vga_move_cursor_left(void) {
    if (vga_col > 0) vga_col--;
    vga_update_cursor();
}

void vga_move_cursor_right(void) {
    vga_col++;
    vga_update_cursor();
}
