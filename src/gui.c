/* gui.c - pixel-level desktop prototype on the VBE framebuffer.
 *
 * Draws a gradient desktop, two windows and a taskbar with the 8x8 font, then
 * tracks the PS/2 mouse with a save/restore cursor. ESC (or q) restores the
 * 80x25 text screen. This is a visual prototype, not a window manager: there
 * is no per-window framebuffer, clipping stack or widget toolkit.
 */
#include "gui.h"
#include "fb.h"
#include "mouse.h"
#include "keyboard.h"
#include "vga.h"
#include "string.h"

#define CUR_W 10
#define CUR_H 14

static uint16_t saved_text[VGA_WIDTH * VGA_HEIGHT];
static uint32_t cur_bg[CUR_W * CUR_H];
static int cur_x = -1, cur_y = -1;

/* Simple arrow cursor mask (1 = white pixel). */
static const char cursor_mask[CUR_H][CUR_W + 1] = {
    "1         ",
    "11        ",
    "111       ",
    "1111      ",
    "11111     ",
    "111111    ",
    "1111111   ",
    "11111111  ",
    "111111111 ",
    "1111111111",
    "1111111   ",
    "111111    ",
    "1111      ",
    "11        "
};

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void draw_desktop(uint32_t w, uint32_t h) {
    /* Vertical gradient, 4 scanlines per step to keep it cheap. */
    for (uint32_t y = 0; y < h; y += 4) {
        uint32_t t = y * 255 / h;
        uint32_t c = rgb(12 + t / 6, 24 + t / 3, 70 + t / 2);
        fb_fill_rect(0, (int)y, (int)w, 4, c);
    }
}

static void draw_window(int x, int y, int w, int h, const char* title,
                        uint32_t accent) {
    fb_fill_rect(x + 4, y + 4, w, h, rgb(8, 10, 20));          /* shadow */
    fb_fill_rect(x, y, w, h, rgb(236, 240, 248));              /* body */
    fb_fill_rect(x, y, w, 20, accent);                         /* title bar */
    fb_fill_rect(x, y, w, 1, rgb(255, 255, 255));              /* top edge */
    fb_fill_rect(x, y + h - 1, w, 1, rgb(40, 60, 90));         /* bottom edge */
    fb_text(x + 6, y + 6, title, rgb(255, 255, 255), accent);
    /* Close button at the right of the title bar. */
    fb_fill_rect(x + w - 18, y + 4, 14, 12, rgb(210, 60, 60));
    fb_text(x + w - 15, y + 6, "x", rgb(255, 255, 255), rgb(210, 60, 60));
}

static void draw_scene(uint32_t w, uint32_t h) {
    draw_desktop(w, h);

    /* Top status strip. */
    fb_fill_rect(0, 0, (int)w, 24, rgb(20, 28, 48));
    fb_text(8, 8, "MuOS Pixel Desktop - move the mouse, ESC or q to exit",
            rgb(180, 220, 255), rgb(20, 28, 48));

    draw_window(48, 60, 430, 250, "terminal", rgb(30, 90, 170));
    fb_text(60, 96, "muos> run net", rgb(30, 90, 30), rgb(236, 240, 248));
    fb_text(60, 112, "PING-OK  UDP-OK  NET-OK", rgb(20, 70, 20), rgb(236, 240, 248));
    fb_text(60, 136, "muos> run tcp", rgb(30, 90, 30), rgb(236, 240, 248));
    fb_text(60, 152, "TCP-OK", rgb(20, 70, 20), rgb(236, 240, 248));
    fb_text(60, 184, "This window is drawn with pixels,", rgb(60, 60, 60),
            rgb(236, 240, 248));
    fb_text(60, 200, "not by writing VGA text cells.", rgb(60, 60, 60),
            rgb(236, 240, 248));

    draw_window(520, 96, 240, 180, "system", rgb(150, 70, 30));
    fb_text(534, 128, "framebuffer:", rgb(60, 60, 60), rgb(236, 240, 248));
    fb_text(534, 144, "VBE linear mode", rgb(20, 70, 20), rgb(236, 240, 248));
    fb_text(534, 168, "font: 8x8 bitmap", rgb(60, 60, 60), rgb(236, 240, 248));
    fb_text(534, 184, "cursor: PS/2 mouse", rgb(60, 60, 60), rgb(236, 240, 248));
    fb_text(534, 208, "ISO boot only", rgb(160, 60, 30), rgb(236, 240, 248));

    /* Taskbar. */
    fb_fill_rect(0, (int)h - 34, (int)w, 34, rgb(24, 34, 56));
    fb_fill_rect(0, (int)h - 34, (int)w, 1, rgb(90, 120, 180));
    fb_fill_rect(8, (int)h - 28, 68, 22, rgb(40, 110, 180));
    fb_text(16, (int)h - 21, "MuOS", rgb(255, 255, 255), rgb(40, 110, 180));
    fb_text(92, (int)h - 21, "pixel UI prototype", rgb(160, 190, 230),
            rgb(24, 34, 56));
}

static void cursor_restore(void) {
    if (cur_x < 0) return;
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++)
            fb_pixel(cur_x + i, cur_y + j, cur_bg[j * CUR_W + i]);
}

static void cursor_draw(int x, int y) {
    cur_x = x;
    cur_y = y;
    for (int j = 0; j < CUR_H; j++) {
        for (int i = 0; i < CUR_W; i++) {
            cur_bg[j * CUR_W + i] = fb_get_pixel(x + i, y + j);
            if (cursor_mask[j][i] == '1')
                fb_pixel(x + i, y + j, rgb(255, 255, 255));
        }
    }
    /* Outline the arrow in dark grey for contrast. */
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++)
            if (cursor_mask[j][i] == '1' &&
                (i == 0 || i == CUR_W - 1 ||
                 (i + 1 < CUR_W && cursor_mask[j][i + 1] == ' ')))
                fb_pixel(x + i, y + j, rgb(20, 20, 20));
}

void gui_run(void) {
    if (!fb_active()) {
        vga_print("pixel UI needs the VBE framebuffer (boot muos.iso)\n");
        return;
    }
    for (uint16_t r = 0; r < VGA_HEIGHT; r++)
        for (uint16_t c = 0; c < VGA_WIDTH; c++)
            saved_text[r * VGA_WIDTH + c] = vga_get_cell(r, c);

    uint32_t w = fb_width(), h = fb_height();
    draw_scene(w, h);
    cur_x = -1;
    cur_y = -1;

    for (;;) {
        int mx = 0, my = 0;
        mouse_get_pixels(&mx, &my);
        int sx = mx * (int)w / 640;
        int sy = my * (int)h / 400;
        if (sx > (int)w - CUR_W) sx = (int)w - CUR_W;
        if (sy > (int)h - CUR_H) sy = (int)h - CUR_H;
        if (sx != cur_x || sy != cur_y) {
            cursor_restore();
            cursor_draw(sx, sy);
        }
        if (keyboard_haschar()) {
            char k = keyboard_getchar();
            if (k == 27 || k == 'q' || k == 'Q') break;
        }
        __asm__ volatile ("sti; hlt; cli");
    }

    cursor_restore();
    for (uint16_t r = 0; r < VGA_HEIGHT; r++)
        for (uint16_t c = 0; c < VGA_WIDTH; c++)
            vga_put_cell(r, c, saved_text[r * VGA_WIDTH + c]);
    vga_repaint();
}
