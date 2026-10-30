/* wm.c - a small compositing window manager for the VBE framebuffer.
 *
 * Each window owns a 32-bit backing buffer (BODY_W x BODY_H). The compositor
 * paints the desktop, windows (z-order), taskbar and mouse cursor into a
 * full-screen back buffer, then fb_blit32() presents it (double buffering).
 * Events: PS/2 mouse move/click/drag/close, keyboard text into the focused
 * window, ESC to leave. Text is rendered as 8x16 by doubling the 8x8 glyph
 * rows (the same font used by the text console).
 */
#include "wm.h"
#include "fb.h"
#include "mouse.h"
#include "keyboard.h"
#include "pit.h"
#include "vga.h"
#include "mm.h"
#include "font8x8.h"
#include "string.h"

#define WIN_COUNT 3
#define WIN_W     380
#define WIN_H     260
#define TITLE_H   22
#define BODY_W    (WIN_W - 2)
#define BODY_H    (WIN_H - TITLE_H - 1)
#define TASKBAR_H 28
#define TEXT_MAX  1024
#define GLYPH_W   8
#define GLYPH_H   16

typedef struct {
    int      x, y;
    bool_t   open;
    char     title[24];
    char     text[TEXT_MAX];
    int      tlen;
} wm_window_t;

static uint32_t* scr;                 /* full-screen back buffer */
static uint32_t* wbuf[WIN_COUNT];     /* per-window body buffers */
static int       SW, SH;
static int       order[WIN_COUNT];    /* z-order, last = top */
static wm_window_t wins[WIN_COUNT];
static uint16_t  saved_text[80 * 25];

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void buf_fill(uint32_t* b, int w, int h, uint32_t c) {
    for (int i = 0; i < w * h; i++) b[i] = c;
}

static void buf_rect(uint32_t* b, int w, int h, int x, int y,
                     int rw, int rh, uint32_t c) {
    for (int yy = 0; yy < rh; yy++) {
        int py = y + yy;
        if (py < 0 || py >= h) continue;
        for (int xx = 0; xx < rw; xx++) {
            int px = x + xx;
            if (px < 0 || px >= w) continue;
            b[py * w + px] = c;
        }
    }
}

/* 8x16 text: each 8x8 glyph row is drawn twice. */
static void buf_text(uint32_t* b, int w, int h, int x, int y,
                     const char* s, uint32_t fg, uint32_t bg) {
    for (; *s; s++, x += GLYPH_W) {
        unsigned char ch = (unsigned char)*s;
        const unsigned char* glyph =
            (ch >= 0x20 && ch < 0x7F) ? font8x8_basic[ch - 0x20]
                                      : font8x8_basic[0];
        for (int gy = 0; gy < 8; gy++) {
            uint32_t bits = glyph[gy];
            for (int dy = 0; dy < 2; dy++) {
                int py = y + gy * 2 + dy;
                if (py < 0 || py >= h) continue;
                for (int gx = 0; gx < 8; gx++) {
                    int px = x + gx;
                    if (px < 0 || px >= w) continue;
                    b[py * w + px] = (bits & (1u << gx)) ? fg : bg;
                }
            }
        }
    }
}

static void body_render(int i) {
    wm_window_t* win = &wins[i];
    uint32_t* b = wbuf[i];
    buf_fill(b, BODY_W, BODY_H, rgb(16, 20, 24));

    const int cols = BODY_W / GLYPH_W;
    const int rows = BODY_H / GLYPH_H;
    int line_no = 0;
    char buf[64];
    int bp = 0;
    for (int k = 0; k < win->tlen && line_no < rows; k++) {
        char ch = win->text[k];
        if (ch == '\n' || bp >= cols) {
            buf[bp < 63 ? bp : 63] = '\0';
            buf_text(b, BODY_W, BODY_H, 8, 8 + line_no * GLYPH_H,
                     buf, rgb(225, 225, 225), rgb(16, 20, 24));
            line_no++;
            bp = 0;
            if (ch == '\n') continue;
        }
        if (line_no >= rows) break;
        buf[bp < 63 ? bp : 63] = ch;
        bp++;
    }
    buf[bp < 63 ? bp : 63] = '\0';
    buf_text(b, BODY_W, BODY_H, 8, 8 + line_no * GLYPH_H,
             buf, rgb(225, 225, 225), rgb(16, 20, 24));

    /* Caret */
    int cy = 8 + line_no * GLYPH_H;
    if (cy + GLYPH_H < BODY_H) {
        buf_rect(b, BODY_W, BODY_H, 8 + bp * GLYPH_W, cy,
                 GLYPH_W, GLYPH_H, rgb(120, 200, 120));
    }
}

/* 10x14 arrow cursor. */
static const char cursor_mask[14][11] = {
    "1         ", "11        ", "111       ", "1111      ",
    "11111     ", "111111    ", "1111111   ", "11111111  ",
    "111111111 ", "1111111111", "1111111   ", "111111    ",
    "1111      ", "11        "
};

static void draw_cursor(int mx, int my) {
    for (int y = 0; y < 14; y++) {
        for (int x = 0; x < 10; x++) {
            int px = mx + x, py = my + y;
            if (px < 0 || px >= SW || py < 0 || py >= SH) continue;
            if (cursor_mask[y][x] == '1') scr[py * SW + px] = rgb(255,255,255);
        }
    }
}

static void compose(int mx, int my) {
    /* Desktop gradient. */
    for (int y = 0; y < SH; y++) {
        uint8_t r = (uint8_t)(24 + y * 24 / (SH ? SH : 1));
        uint8_t g = (uint8_t)(40 + y * 30 / (SH ? SH : 1));
        uint8_t b = (uint8_t)(80 + y * 60 / (SH ? SH : 1));
        uint32_t c = rgb(r, g, b);
        for (int x = 0; x < SW; x++) scr[y * SW + x] = c;
    }

    for (int k = 0; k < WIN_COUNT; k++) {
        int i = order[k];
        wm_window_t* win = &wins[i];
        if (!win->open) continue;

        /* Shadow */
        buf_rect(scr, SW, SH, win->x + 5, win->y + 5, WIN_W, WIN_H,
                 rgb(10, 12, 18));
        /* Border + title bar */
        buf_rect(scr, SW, SH, win->x, win->y, WIN_W, WIN_H, rgb(40, 44, 52));
        uint32_t tb = (k == WIN_COUNT - 1) ? rgb(42, 111, 219) : rgb(60, 64, 72);
        buf_rect(scr, SW, SH, win->x + 1, win->y + 1, WIN_W - 2, TITLE_H - 1, tb);
        /* Close box */
        buf_rect(scr, SW, SH, win->x + WIN_W - 22, win->y + 5, 16, 12,
                 rgb(190, 60, 60));

        char t[26];
        int n = 0;
        for (const char* p = win->title; *p && n < 25; p++) t[n++] = *p;
        t[n] = '\0';
        buf_text(scr, SW, SH, win->x + 8, win->y + 3, t,
                 rgb(245, 245, 245), tb);
        buf_text(scr, SW, SH, win->x + WIN_W - 19, win->y + 2, "x",
                 rgb(255, 240, 240), rgb(190, 60, 60));

        /* Body: copy the window's backing buffer into the screen image. */
        for (int y = 0; y < BODY_H; y++) {
            int py = win->y + TITLE_H + y;
            if (py < 0 || py >= SH) continue;
            for (int x = 0; x < BODY_W; x++) {
                int px = win->x + 1 + x;
                if (px < 0 || px >= SW) continue;
                scr[py * SW + px] = wbuf[i][y * BODY_W + x];
            }
        }
    }

    /* Taskbar */
    int ty = SH - TASKBAR_H;
    if (ty > 0) {
        buf_rect(scr, SW, SH, 0, ty, SW, TASKBAR_H, rgb(18, 22, 30));
        buf_text(scr, SW, SH, 10, ty + 6,
                 "MuOS WM  drag title / click close / type text / ESC exits",
                 rgb(200, 210, 225), rgb(18, 22, 30));
    }

    draw_cursor(mx, my);
    fb_blit32(scr, (uint32_t)SW, (uint32_t)SH);
}

/* Hit test from top of the z-order. */
static int hit(int mx, int my) {
    for (int k = WIN_COUNT - 1; k >= 0; k--) {
        int i = order[k];
        wm_window_t* win = &wins[i];
        if (!win->open) continue;
        if (mx >= win->x && mx < win->x + WIN_W &&
            my >= win->y && my < win->y + WIN_H)
            return i;
    }
    return -1;
}

static void focus(int i) {
    int k = 0;
    for (; k < WIN_COUNT; k++) if (order[k] == i) break;
    if (k >= WIN_COUNT) return;
    for (int j = k; j + 1 < WIN_COUNT; j++) order[j] = order[j + 1];
    order[WIN_COUNT - 1] = i;
}

void wm_run(void) {
    if (!fb_active()) {
        vga_print("wm needs VBE: boot muos.iso (ISO/VBE path)\n");
        return;
    }
    SW = (int)fb_width();
    SH = (int)fb_height();
    if (SW <= 0 || SH <= 0 || SW > 1024 || SH > 768) return;

    uint32_t scr_pages = ((uint32_t)SW * (uint32_t)SH * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
    scr = (uint32_t*)mm_alloc_pages(scr_pages);
    if (!scr) { vga_print("wm: out of memory for screen buffer\n"); return; }
    uint32_t body_pages = ((uint32_t)BODY_W * BODY_H * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
    bool_t ok = true;
    for (int i = 0; i < WIN_COUNT; i++) {
        wbuf[i] = (uint32_t*)mm_alloc_pages(body_pages);
        if (!wbuf[i]) { ok = false; break; }
    }
    if (!ok) {
        for (int i = 0; i < WIN_COUNT; i++)
            if (wbuf[i]) mm_free_pages(wbuf[i], body_pages);
        mm_free_pages(scr, scr_pages);
        vga_print("wm: out of memory for window buffers\n");
        return;
    }

    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 80; c++)
            saved_text[r * 80 + c] = vga_get_cell((uint16_t)r, (uint16_t)c);
    vga_hide_cursor();

    memset(wins, 0, sizeof(wins));
    wins[0].x = 30;  wins[0].y = 40;
    wins[1].x = SW - WIN_W - 20; wins[1].y = 90;
    wins[2].x = (SW - WIN_W) / 2; wins[2].y = SH - WIN_H - TASKBAR_H - 10;
    for (int i = 0; i < WIN_COUNT; i++) {
        wins[i].open = true;
        order[i] = i;
        if (wins[i].x < 0) wins[i].x = 0;
        if (wins[i].x > SW - WIN_W) wins[i].x = SW - WIN_W;
        if (wins[i].y < TITLE_H) wins[i].y = TITLE_H;
        if (wins[i].y > SH - WIN_H - TASKBAR_H)
            wins[i].y = SH - WIN_H - TASKBAR_H;
    }
    wins[0].tlen = 0;
    { const char* s = "Terminal\nType here with the keyboard.\nESC leaves the window manager.\n";
      for (int i = 0; s[i] && i < TEXT_MAX; i++) { wins[0].text[i] = s[i]; wins[0].tlen++; } }
    { const char* s = "System\nMuOS 7 window manager\n- per-window back buffers\n- compositor + double buffer\n- mouse drag / focus / close\n";
      wins[1].tlen = 0;
      for (int i = 0; s[i] && i < TEXT_MAX; i++) { wins[1].text[i] = s[i]; wins[1].tlen++; } }
    wins[2].tlen = 0;
    strncpy(wins[0].title, "Terminal", 23);
    strncpy(wins[1].title, "System", 23);
    strncpy(wins[2].title, "Editor", 23);

    int focus_i = -1;
    for (int i = 0; i < WIN_COUNT; i++) body_render(i);

    int mx = SW / 2, my = SH / 2;
    int drag_i = -1, drag_dx = 0, drag_dy = 0;
    int prev_left = 0;
    uint32_t last = pit_get_ticks();
    bool_t running = true;

    while (running) {
        while (pit_get_ticks() == last) __asm__ volatile ("sti; hlt; cli");
        last = pit_get_ticks();

        int px = 0, py = 0;
        mouse_get_pixels(&px, &py);
        mx = px * SW / 640;
        my = py * SH / 400;
        if (mx < 0) mx = 0;
        if (mx >= SW) mx = SW - 1;
        if (my < 0) my = 0;
        if (my >= SH) my = SH - 1;

        mouse_state_t ms = mouse_get_state();
        int left = ms.btn_left ? 1 : 0;

        if (left && !prev_left) {
            int i = hit(mx, my);
            if (i >= 0) {
                wm_window_t* win = &wins[i];
                int in_title = (my >= win->y && my < win->y + TITLE_H);
                int in_close = (mx >= win->x + WIN_W - 22 &&
                                mx < win->x + WIN_W - 4 &&
                                my >= win->y + 4 && my < win->y + 18);
                focus(i);
                focus_i = i;
                if (in_close) {
                    win->open = false;
                } else if (in_title) {
                    drag_i = i;
                    drag_dx = win->x - mx;
                    drag_dy = win->y - my;
                }
            } else {
                focus_i = -1;
            }
        }
        if (!left) drag_i = -1;
        if (left && drag_i >= 0) {
            wm_window_t* win = &wins[drag_i];
            win->x = mx + drag_dx;
            win->y = my + drag_dy;
            if (win->x < 0) win->x = 0;
            if (win->y < TITLE_H) win->y = TITLE_H;
            if (win->x > SW - WIN_W) win->x = SW - WIN_W;
            if (win->y > SH - WIN_H - TASKBAR_H) win->y = SH - WIN_H - TASKBAR_H;
        }
        prev_left = left;

        while (keyboard_haschar()) {
            char c = keyboard_getchar();
            if (c == 27) { running = false; break; }
            if (focus_i < 0 || !wins[focus_i].open) continue;
            wm_window_t* win = &wins[focus_i];
            if (c == 8) {
                if (win->tlen > 0) win->tlen--;
            } else if (c == '\r') {
                if (win->tlen < TEXT_MAX - 1) win->text[win->tlen++] = '\n';
            } else if (c >= 32 && c < 127) {
                if (win->tlen < TEXT_MAX - 1) win->text[win->tlen++] = c;
            }
            body_render(focus_i);
        }

        compose(mx, my);
    }

    for (int i = 0; i < WIN_COUNT; i++) mm_free_pages(wbuf[i], body_pages);
    mm_free_pages(scr, scr_pages);
    scr = NULL;

    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 80; c++)
            vga_put_cell((uint16_t)r, (uint16_t)c, saved_text[r * 80 + c]);
    vga_show_cursor();
    vga_repaint();
}
