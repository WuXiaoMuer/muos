#ifndef MUOS_VGA_H
#define MUOS_VGA_H
#include "types.h"

#define VGA_WIDTH   80
#define VGA_HEIGHT  25

enum vga_color {
    VGA_BLACK         = 0,
    VGA_BLUE          = 1,
    VGA_GREEN         = 2,
    VGA_CYAN          = 3,
    VGA_RED           = 4,
    VGA_MAGENTA       = 5,
    VGA_BROWN         = 6,
    VGA_LIGHT_GREY    = 7,
    VGA_DARK_GREY     = 8,
    VGA_LIGHT_BLUE    = 9,
    VGA_LIGHT_GREEN   = 10,
    VGA_LIGHT_CYAN    = 11,
    VGA_LIGHT_RED     = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW        = 14,
    VGA_WHITE         = 15,
};

uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg);
uint16_t vga_entry(unsigned char c, uint8_t color);

void vga_init(void);
void vga_clear(void);
void vga_setcolor(uint8_t color);
uint8_t vga_getcolor(void);
void vga_putchar(char c);
void vga_write(const char* data, size_t size);
void vga_print(const char* str);
void vga_print_hex(uint32_t n);
void vga_print_dec(uint32_t n);
void vga_scroll(void);
void vga_update_cursor(void);
void vga_set_cursor(uint16_t row, uint16_t col);
void vga_move_cursor_left(void);
void vga_move_cursor_right(void);
uint16_t vga_get_cursor_row(void);
uint16_t vga_get_cursor_col(void);

#endif
