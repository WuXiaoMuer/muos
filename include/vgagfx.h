#ifndef MUOS_VGAGFX_H
#define MUOS_VGAGFX_H
#include "types.h"

#define GFX_WIDTH   320
#define GFX_HEIGHT  200
#define GFX_COLORS  256

/* Framebuffer at 0xA0000 in mode 13h */
#define GFX_FB      ((uint8_t*)0xA0000)

/* Colors (VGA palette indices) */
#define GFX_BLACK        0
#define GFX_BLUE         1
#define GFX_GREEN        2
#define GFX_CYAN         3
#define GFX_RED          4
#define GFX_MAGENTA      5
#define GFX_BROWN        6
#define GFX_LGRAY        7
#define GFX_DGRAY        8
#define GFX_LBLUE        9
#define GFX_LGREEN       10
#define GFX_LCYAN        11
#define GFX_LRED          12
#define GFX_LMAGENTA     13
#define GFX_YELLOW        14
#define GFX_WHITE         15

void gfx_init(void);
void gfx_clear(uint8_t color);
void gfx_set_pixel(uint16_t x, uint16_t y, uint8_t color);
void gfx_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color);
void gfx_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color);
void gfx_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint8_t color);
void gfx_draw_char(uint16_t x, uint16_t y, char c, uint8_t fg, uint8_t bg);
void gfx_draw_text(uint16_t x, uint16_t y, const char* str, uint8_t fg, uint8_t bg);
void gfx_switch_to(void);   /* Enter graphics mode */
void gfx_switch_back(void); /* Return to text mode */

#endif
