#include "vga.h"
#include "io.h"
#include "fb.h"
#include "string.h"

static uint16_t* const VGA_BUFFER = (uint16_t*)0xB8000;
static uint16_t  cells[VGA_WIDTH * VGA_HEIGHT];
static uint8_t   vga_color;
static volatile uint16_t vga_row;
static volatile uint16_t vga_col;
static uint8_t   cursor_scan_start = 0x0E;
static uint8_t   cursor_scan_end   = 0x0F;
/* The ISO bootloader may have switched to a VBE graphics mode. Until the
 * kernel has mapped the framebuffer, text writes go to the shadow grid only
 * (0xB8000 is not the visible console anymore). */
static bool_t    text_visible;
static bool_t    pending;
static uint16_t  cursor_prev_row = 0xFFFF, cursor_prev_col = 0xFFFF;

uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return (bg << 4) | fg;
}

uint16_t vga_entry(unsigned char c, uint8_t color) {
    return ((uint16_t)color << 8) | (uint16_t)c;
}

static void render_cell(uint16_t row, uint16_t col, uint16_t entry) {
    cells[row * VGA_WIDTH + col] = entry;
    if (fb_active()) {
        fb_put_cell(row, col, entry);
    } else if (text_visible) {
        VGA_BUFFER[row * VGA_WIDTH + col] = entry;
    }
}

void vga_put_cell(uint16_t row, uint16_t col, uint16_t entry) {
    if (row >= VGA_HEIGHT || col >= VGA_WIDTH) return;
    render_cell(row, col, entry);
}

uint16_t vga_get_cell(uint16_t row, uint16_t col) {
    if (row >= VGA_HEIGHT || col >= VGA_WIDTH) return 0;
    return cells[row * VGA_WIDTH + col];
}

void vga_init(void) {
    vga_row = 0;
    vga_col = 0;
    vga_color = vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);
    pending = fb_pending();
    text_visible = !pending;

    /* Remember the BIOS cursor shape so GUI/editor can hide it and
     * restore the text-mode cursor afterwards. */
    outb(0x3D4, 0x0A); cursor_scan_start = inb(0x3D5);
    outb(0x3D4, 0x0B); cursor_scan_end   = inb(0x3D5);

    vga_clear();
}

void vga_hide_cursor(void) {
    if (fb_active() || pending) return;
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);   /* bit5 = cursor disable */
}

void vga_show_cursor(void) {
    if (fb_active() || pending) return;
    outb(0x3D4, 0x0A);
    outb(0x3D5, cursor_scan_start);
    outb(0x3D4, 0x0B);
    outb(0x3D5, cursor_scan_end);
}

void vga_setcolor(uint8_t color) {
    vga_color = color;
}

void vga_clear(void) {
    uint16_t blank = vga_entry(' ', vga_color);
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) cells[i] = blank;
    if (fb_active()) {
        fb_clear(0x000000);
    } else if (text_visible) {
        for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) VGA_BUFFER[i] = blank;
    }
    vga_row = 0;
    vga_col = 0;
    cursor_prev_row = cursor_prev_col = 0xFFFF;
    vga_update_cursor();
}

/* Repaint the whole shadow grid (after the framebuffer becomes available). */
void vga_repaint(void) {
    if (!fb_active()) return;
    for (uint16_t r = 0; r < VGA_HEIGHT; r++) {
        for (uint16_t c = 0; c < VGA_WIDTH; c++)
            fb_put_cell(r, c, cells[r * VGA_WIDTH + c]);
    }
    cursor_prev_row = cursor_prev_col = 0xFFFF;
    vga_update_cursor();
}

void vga_scroll(void) {
    if (vga_row < VGA_HEIGHT) return;
    memcpy(cells, cells + VGA_WIDTH,
           (VGA_HEIGHT - 1) * VGA_WIDTH * sizeof(uint16_t));
    uint16_t blank = vga_entry(' ', vga_color);
    for (size_t i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++)
        cells[i] = blank;
    vga_row = VGA_HEIGHT - 1;

    if (fb_active()) {
        vga_repaint();
    } else if (text_visible) {
        for (size_t i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++)
            VGA_BUFFER[i] = cells[i];
        for (size_t i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++)
            VGA_BUFFER[i] = blank;
    }
}

void vga_update_cursor(void) {
    if (fb_active()) {
        if (cursor_prev_row != 0xFFFF)
            fb_put_cell(cursor_prev_row, cursor_prev_col,
                        cells[cursor_prev_row * VGA_WIDTH + cursor_prev_col]);
        cursor_prev_row = vga_row;
        cursor_prev_col = vga_col;
        fb_set_cursor((uint16_t)vga_row, (uint16_t)vga_col);
        return;
    }
    if (pending) return;
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
            render_cell((uint16_t)vga_row, (uint16_t)vga_col,
                        vga_entry(' ', vga_color));
        }
    } else {
        render_cell((uint16_t)vga_row, (uint16_t)vga_col,
                    vga_entry((unsigned char)c, vga_color));
        vga_col++;
    }

    if (vga_col >= VGA_WIDTH) {
        vga_col = 0;
        vga_row++;
    }

    vga_scroll();
    vga_update_cursor();
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
    if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    if (col >= VGA_WIDTH)  col = VGA_WIDTH - 1;
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
