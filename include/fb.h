#ifndef MUOS_FB_H
#define MUOS_FB_H
#include "types.h"

/* VBE linear framebuffer brought up by the El Torito bootloader (boot_cd.s).
 * The bootloader leaves a descriptor at VBE_INFO_ADDR; the kernel maps the
 * framebuffer only on that path (`-kernel` boots keep VGA text mode). */

#define VBE_INFO_ADDR 0x00006F00u
#define VBE_INFO_MAGIC 0x31454256u   /* 'V','B','E','1' in memory order */

typedef struct {
    uint32_t magic;
    uint32_t phys;       /* linear framebuffer base */
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
    uint32_t mode;
} vbe_info_t;

bool_t fb_pending(void);        /* bootloader left a usable VBE descriptor */
bool_t fb_enable(void);         /* map the framebuffer; true when active */
bool_t fb_active(void);

void fb_clear(uint32_t rgb);
void fb_put_cell(uint16_t row, uint16_t col, uint16_t entry);
void fb_set_cursor(uint16_t row, uint16_t col);

/* Pixel-level helpers for a graphical UI prototype. */
void     fb_pixel(int x, int y, uint32_t rgb);
uint32_t fb_get_pixel(int x, int y);
void     fb_fill_rect(int x, int y, int w, int h, uint32_t rgb);
void     fb_text(int x, int y, const char* s, uint32_t fg, uint32_t bg);
uint32_t fb_palette(uint8_t index);
uint32_t fb_width(void);
uint32_t fb_height(void);
uint32_t fb_bpp(void);

#endif
