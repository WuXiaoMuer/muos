/* fb.c - linear framebuffer text console on top of VBE.
 *
 * The bootloader picks an 800x600 (or larger) 16/24/32-bpp linear mode and
 * leaves a vbe_info_t at VBE_INFO_ADDR. Text is still an 80x25 cell grid:
 * each 8x8 glyph is drawn twice vertically (8x16), so the 80x25 grid covers
 * 640x400 pixels and is centred in the mode. The VGA text console in vga.c
 * feeds this renderer when it is active.
 */
#include "fb.h"
#include "font8x8.h"
#include "mm.h"
#include "string.h"

static vbe_info_t info;
static bool_t active;
static uint8_t* fb;
static uint32_t origin_x, origin_y;   /* top-left of the 640x400 text area */
static uint16_t cursor_row = 0xFFFF, cursor_col = 0xFFFF;

/* Standard VGA 16-colour palette as 0x00RRGGBB. */
static const uint32_t palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

bool_t fb_pending(void) {
    const vbe_info_t* v = (const vbe_info_t*)VBE_INFO_ADDR;
    return v->magic == VBE_INFO_MAGIC && v->phys && v->width && v->height &&
           v->pitch && (v->bpp == 16 || v->bpp == 24 || v->bpp == 32);
}

static void put_pixel(uint32_t x, uint32_t y, uint32_t rgb) {
    uint8_t* p = fb + y * info.pitch + x * (info.bpp / 8);
    if (info.bpp == 32) {
        *(volatile uint32_t*)p = rgb;
    } else if (info.bpp == 24) {
        p[0] = rgb & 0xFF;
        p[1] = (rgb >> 8) & 0xFF;
        p[2] = (rgb >> 16) & 0xFF;
    } else {                 /* 16 bpp: 5-6-5 */
        uint16_t v = (uint16_t)(((rgb >> 19) & 0x1F) << 11 |
                                ((rgb >> 10) & 0x3F) << 5 |
                                ((rgb >> 3) & 0x1F));
        *(volatile uint16_t*)p = v;
    }
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t rgb) {
    for (uint32_t j = 0; j < h; j++) {
        for (uint32_t i = 0; i < w; i++) {
            if (x + i < info.width && y + j < info.height)
                put_pixel(x + i, y + j, rgb);
        }
    }
}

void fb_pixel(int x, int y, uint32_t rgb) {
    if (!active || x < 0 || y < 0 ||
        (uint32_t)x >= info.width || (uint32_t)y >= info.height) return;
    put_pixel((uint32_t)x, (uint32_t)y, rgb);
}

uint32_t fb_get_pixel(int x, int y) {
    if (!active || x < 0 || y < 0 ||
        (uint32_t)x >= info.width || (uint32_t)y >= info.height) return 0;
    uint8_t* p = fb + (uint32_t)y * info.pitch + (uint32_t)x * (info.bpp / 8);
    if (info.bpp == 32) return *(volatile uint32_t*)p;
    if (info.bpp == 24)
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    uint16_t v = *(volatile uint16_t*)p;
    uint32_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
    return ((r << 3) << 16) | ((g << 2) << 8) | ((b << 3));
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (!active || w <= 0 || h <= 0 || x >= (int)info.width || y >= (int)info.height)
        return;
    uint32_t x0 = x < 0 ? 0 : (uint32_t)x;
    uint32_t y0 = y < 0 ? 0 : (uint32_t)y;
    uint32_t w0 = (uint32_t)(x + w) > info.width ? info.width - x0 : (uint32_t)w;
    uint32_t h0 = (uint32_t)(y + h) > info.height ? info.height - y0 : (uint32_t)h;
    fill_rect(x0, y0, w0, h0, rgb);
}

uint32_t fb_palette(uint8_t index) { return palette[index & 0x0F]; }

/* 8x8 text at arbitrary pixel coordinates (used by the GUI prototype). */
void fb_text(int x, int y, const char* s, uint32_t fg, uint32_t bg) {
    if (!active) return;
    for (; *s; s++, x += 8) {
        unsigned char ch = (unsigned char)*s;
        const unsigned char* glyph =
            (ch >= 0x20 && ch < 0x7F) ? font8x8_basic[ch - 0x20]
                                      : font8x8_basic[0];
        for (int gy = 0; gy < 8; gy++) {
            uint32_t bits = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                fb_pixel(x + gx, y + gy,
                         (bits & (1u << gx)) ? fg : bg);
            }
        }
    }
}

bool_t fb_enable(void) {
    if (active) return true;
    if (!fb_pending()) return false;
    const vbe_info_t* v = (const vbe_info_t*)VBE_INFO_ADDR;
    memcpy(&info, v, sizeof(info));

    uint32_t bytes = info.pitch * info.height;
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t i = 0; i < pages; i++) {
        /* Identity-map the video memory (uncached) so both the CPU and any
         * scanout alias see the same bytes. */
        mm_map_page(info.phys + i * PAGE_SIZE, info.phys + i * PAGE_SIZE,
                    PAGE_KERNEL | 0x10 /* PCD */);
    }
    fb = (uint8_t*)info.phys;
    origin_x = (info.width > 640) ? (info.width - 640) / 2 : 0;
    origin_y = (info.height > 400) ? (info.height - 400) / 2 : 0;
    active = true;
    fb_clear(0x000000);
    return true;
}

bool_t fb_active(void) { return active; }
uint32_t fb_width(void) { return info.width; }
uint32_t fb_height(void) { return info.height; }
uint32_t fb_bpp(void) { return info.bpp; }

bool_t fb_blit32(const uint32_t* src, uint32_t w, uint32_t h) {
    if (!active || !src) return false;
    if (w > info.width || h > info.height) return false;
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t* s = src + y * w;
        uint8_t* d = fb + y * info.pitch;
        if (info.bpp == 32) {
            memcpy(d, s, w * 4);
        } else {
            for (uint32_t x = 0; x < w; x++) {
                uint32_t rgbv = s[x];
                if (info.bpp == 24) {
                    d[x * 3 + 0] = (uint8_t)(rgbv & 0xFF);
                    d[x * 3 + 1] = (uint8_t)((rgbv >> 8) & 0xFF);
                    d[x * 3 + 2] = (uint8_t)((rgbv >> 16) & 0xFF);
                } else {
                    uint16_t v = (uint16_t)(((rgbv >> 19) & 0x1F) << 11 |
                                            ((rgbv >> 10) & 0x3F) << 5 |
                                            ((rgbv >> 3) & 0x1F));
                    ((uint16_t*)d)[x] = v;
                }
            }
        }
    }
    return true;
}

void fb_clear(uint32_t rgb) {
    if (!active) return;
    fill_rect(0, 0, info.width, info.height, rgb);
}

void fb_put_cell(uint16_t row, uint16_t col, uint16_t entry) {
    if (!active) return;
    unsigned char ch = (unsigned char)(entry & 0xFF);
    uint8_t attr = (uint8_t)(entry >> 8);
    uint32_t fg = palette[attr & 0x0F];
    uint32_t bg = palette[(attr >> 4) & 0x0F];
    const unsigned char* glyph =
        (ch >= 0x20 && ch < 0x7F) ? font8x8_basic[ch - 0x20]
                                  : font8x8_basic[0];

    uint32_t x0 = origin_x + (uint32_t)col * 8;
    uint32_t y0 = origin_y + (uint32_t)row * 16;
    for (uint32_t gy = 0; gy < 8; gy++) {
        uint32_t bits = glyph[gy];
        for (uint32_t dy = 0; dy < 2; dy++) {
            uint32_t y = y0 + gy * 2 + dy;
            for (uint32_t gx = 0; gx < 8; gx++) {
                uint32_t color = (bits & (1u << gx)) ? fg : bg;
                put_pixel(x0 + gx, y, color);
            }
        }
    }
}

void fb_set_cursor(uint16_t row, uint16_t col) {
    if (!active) return;
    /* Erase the old underline marker by redrawing happens in vga.c; here we
     * simply place a short white bar under the new cell. */
    cursor_row = row;
    cursor_col = col;
    uint32_t x0 = origin_x + (uint32_t)col * 8;
    uint32_t y0 = origin_y + (uint32_t)row * 16 + 14;
    fill_rect(x0, y0, 8, 2, 0xFFFFFF);
}
