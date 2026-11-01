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
#include "logo.h"

#define WIN_COUNT 4
#define WIDGET_WIN 3
#define WIN_W     380
#define WIN_H     260
#define TITLE_H   22
#define BODY_W    (WIN_W - 2)
#define BODY_H    (WIN_H - TITLE_H - 1)
#define TASKBAR_H 28
#define TEXT_MAX  1024
#define GLYPH_W   8
#define GLYPH_H   16

/* Widgets window geometry (body-local pixels). */
#define W_BTN_X   10
#define W_BTN_W   110
#define W_BTN_H   22
#define W_BTN1_Y  62
#define W_BTN2_Y  90
#define W_IN_X    10
#define W_IN_Y    130
#define W_IN_W    220
#define W_IN_H    24

/* List control (body-local). */
#define LIST_X    242
#define LIST_Y    62
#define LIST_W    126
#define LIST_H    (LIST_ROWS * LIST_ROW_H)
#define LIST_ROWS 6
#define LIST_ROW_H 18
#define LIST_SB   8
#define LIST_COUNT 10
#define MENU_X    242
#define MENU_Y    8
#define MENU_W    126
#define MENU_H    20
#define MENU_POP_Y 32
#define MENU_POP_H (24 + MENU_COUNT * MENU_ITEM_H)
#define MENU_ITEM_H 18
#define MENU_COUNT 3

typedef struct {
    int      x, y;
    bool_t   open;
    char     title[24];
    char     text[TEXT_MAX];
    int      tlen;
    int      scroll;        /* first visible text line */
    char     input[64];     /* Widgets input box */
    int      ilen;
    int      list_sel;      /* list control selection */
    int      list_scroll;   /* first visible list row */
    bool_t   menu_open;     /* dropdown menu expanded */
} wm_window_t;

/* Clipping stack: every buf_rect/buf_text call is limited to the current
 * rectangle. Nested controls push their viewport and pop afterwards. */
#define CLIP_MAX 8
typedef struct { int x0, y0, x1, y1; } clip_rect_t;
static clip_rect_t clip_stack[CLIP_MAX];
static int clip_depth;
static int clip_x0, clip_y0, clip_x1, clip_y1;

static void clip_reset(int w, int h) {
    clip_depth = 0;
    clip_x0 = 0; clip_y0 = 0; clip_x1 = w; clip_y1 = h;
}

static void clip_push(int x, int y, int w, int h) {
    if (clip_depth >= CLIP_MAX) return;
    int x0 = x > clip_x0 ? x : clip_x0;
    int y0 = y > clip_y0 ? y : clip_y0;
    int x1 = (x + w) < clip_x1 ? (x + w) : clip_x1;
    int y1 = (y + h) < clip_y1 ? (y + h) : clip_y1;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    clip_stack[clip_depth].x0 = clip_x0;
    clip_stack[clip_depth].y0 = clip_y0;
    clip_stack[clip_depth].x1 = clip_x1;
    clip_stack[clip_depth].y1 = clip_y1;
    clip_depth++;
    clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1;
}

static void clip_pop(void) {
    if (clip_depth <= 0) return;
    clip_depth--;
    clip_x0 = clip_stack[clip_depth].x0;
    clip_y0 = clip_stack[clip_depth].y0;
    clip_x1 = clip_stack[clip_depth].x1;
    clip_y1 = clip_stack[clip_depth].y1;
}

static uint32_t* scr;                 /* full-screen back buffer */
static uint32_t* wbuf[WIN_COUNT];     /* per-window body buffers */
static int       SW, SH;
static void body_render(int i);
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
        if (py < 0 || py >= h || py < clip_y0 || py >= clip_y1) continue;
        for (int xx = 0; xx < rw; xx++) {
            int px = x + xx;
            if (px < 0 || px >= w || px < clip_x0 || px >= clip_x1) continue;
            b[py * w + px] = c;
        }
    }
}

/* 8x8 glyph scaled by `scale`; used for the larger menu heading. */
static void buf_text_s(uint32_t* b, int w, int h, int x, int y,
                       const char* s, uint32_t fg, uint32_t bg, int scale) {
    if (scale < 1) scale = 1;
    for (; *s; s++, x += 8 * scale) {
        unsigned char ch = (unsigned char)*s;
        const unsigned char* glyph =
            (ch >= 0x20 && ch < 0x7F) ? font8x8_basic[ch - 0x20]
                                      : font8x8_basic[0];
        for (int gy = 0; gy < 8; gy++) {
            uint32_t bits = glyph[gy];
            for (int sy = 0; sy < scale; sy++) {
                int py = y + gy * scale + sy;
                if (py < 0 || py >= h || py < clip_y0 || py >= clip_y1) continue;
                for (int gx = 0; gx < 8; gx++) {
                    int px0 = x + gx * scale;
                    for (int sx = 0; sx < scale; sx++) {
                        int px = px0 + sx;
                        if (px < 0 || px >= w || px < clip_x0 || px >= clip_x1)
                            continue;
                        b[py * w + px] = (bits & (1u << gx)) ? fg : bg;
                    }
                }
            }
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
                if (py < 0 || py >= h || py < clip_y0 || py >= clip_y1) continue;
                for (int gx = 0; gx < 8; gx++) {
                    int px = x + gx;
                    if (px < 0 || px >= w || px < clip_x0 || px >= clip_x1) continue;
                    b[py * w + px] = (bits & (1u << gx)) ? fg : bg;
                }
            }
        }
    }
}

static int win_total_lines(wm_window_t* win) {
    int cols = (BODY_W - 18) / GLYPH_W;
    int lines = 1, col = 0;
    for (int k = 0; k < win->tlen; k++) {
        char ch = win->text[k];
        if (ch == '\n') { lines++; col = 0; continue; }
        if (col >= cols) { lines++; col = 0; }
        col++;
    }
    return lines;
}

static void log_append(wm_window_t* win, const char* s) {
    for (; *s && win->tlen < TEXT_MAX - 1; s++) win->text[win->tlen++] = *s;
}

/* Text window: wrapped lines with a vertical scrollbar on the right. */
static void body_render_text(int i) {
    wm_window_t* win = &wins[i];
    uint32_t* b = wbuf[i];
    buf_fill(b, BODY_W, BODY_H, rgb(16, 20, 24));

    const int cols = (BODY_W - 18) / GLYPH_W;
    const int rows = BODY_H / GLYPH_H;
    int total = win_total_lines(win);
    int maxs = total > rows ? total - rows : 0;
    if (win->scroll > maxs) win->scroll = maxs;
    if (win->scroll < 0) win->scroll = 0;

    char line[64];
    int bp = 0, line_no = 0, drawn = 0;
    int caret_line = 0, caret_col = 0;
    for (int k = 0; k <= win->tlen; k++) {
        char ch = (k < win->tlen) ? win->text[k] : '\n';
        if (k == win->tlen) { caret_line = line_no; caret_col = bp; }
        if (ch == '\n' || bp >= cols) {
            if (line_no >= win->scroll && drawn < rows) {
                line[bp < 63 ? bp : 63] = '\0';
                buf_text(b, BODY_W, BODY_H, 8, 8 + drawn * GLYPH_H,
                         line, rgb(225, 225, 225), rgb(16, 20, 24));
                drawn++;
            }
            line_no++;
            bp = 0;
            if (ch == '\n') continue;
        }
        line[bp < 63 ? bp : 63] = ch;
        bp++;
    }

    int cy = 8 + (caret_line - win->scroll) * GLYPH_H;
    if (caret_line >= win->scroll && caret_line < win->scroll + rows &&
        cy + GLYPH_H < BODY_H) {
        buf_rect(b, BODY_W, BODY_H, 8 + caret_col * GLYPH_W, cy,
                 GLYPH_W, GLYPH_H, rgb(120, 200, 120));
    }

    int track_x = BODY_W - 10, track_y = 4, track_h = BODY_H - 8;
    buf_rect(b, BODY_W, BODY_H, track_x, track_y, 6, track_h, rgb(34, 38, 46));
    if (total > 0) {
        int thumb_h = track_h * rows / total;
        if (thumb_h < 16) thumb_h = 16;
        if (thumb_h > track_h) thumb_h = track_h;
        int span = track_h - thumb_h;
        int thumb_y = track_y + (maxs ? span * win->scroll / maxs : 0);
        buf_rect(b, BODY_W, BODY_H, track_x, thumb_y, 6, thumb_h,
                 rgb(112, 128, 152));
    }
}

static void draw_button(uint32_t* b, int x, int y, const char* label) {
    buf_rect(b, BODY_W, BODY_H, x, y, W_BTN_W, W_BTN_H, rgb(42, 111, 219));
    buf_rect(b, BODY_W, BODY_H, x, y, W_BTN_W, 2, rgb(96, 160, 240));
    buf_rect(b, BODY_W, BODY_H, x, y + W_BTN_H - 2, W_BTN_W, 2, rgb(26, 70, 145));
    buf_text(b, BODY_W, BODY_H, x + 8, y + 3, label,
             rgb(245, 248, 255), rgb(42, 111, 219));
}

static const char* list_items[LIST_COUNT] = {
    "Alpha", "Bravo", "Charlie", "Delta", "Echo",
    "Foxtrot", "Golf", "Hotel", "India", "Juliet"
};

static const char* menu_items[MENU_COUNT] = {
    "Say Hello", "Clear Log", "About"
};

static int menu_item_at(int bx, int by) {
    if (bx < MENU_X || bx >= MENU_X + MENU_W) return -1;
    int y = by - (MENU_POP_Y + 24);
    if (y < 0 || y >= MENU_COUNT * MENU_ITEM_H) return -1;
    return y / MENU_ITEM_H;
}

static void draw_menu(wm_window_t* win, uint32_t* b) {
    buf_rect(b, BODY_W, BODY_H, MENU_X, MENU_Y, MENU_W, MENU_H,
             rgb(42, 111, 219));
    buf_rect(b, BODY_W, BODY_H, MENU_X, MENU_Y, MENU_W, 2,
             rgb(96, 160, 240));
    buf_text(b, BODY_W, BODY_H, MENU_X + 8, MENU_Y + 2, "Menu",
             rgb(245, 248, 255), rgb(42, 111, 219));
    if (!win->menu_open) return;

    clip_push(MENU_X, MENU_POP_Y, MENU_W, MENU_POP_H);
    buf_rect(b, BODY_W, BODY_H, MENU_X, MENU_POP_Y, MENU_W, MENU_POP_H,
             rgb(84, 94, 112));
    buf_rect(b, BODY_W, BODY_H, MENU_X + 1, MENU_POP_Y + 1, MENU_W - 2,
             MENU_POP_H - 2, rgb(28, 32, 42));
    buf_text_s(b, BODY_W, BODY_H, MENU_X + 8, MENU_POP_Y + 4, "Menu",
               rgb(232, 242, 255), rgb(28, 32, 42), 2);
    for (int r = 0; r < MENU_COUNT; r++) {
        int y = MENU_POP_Y + 24 + r * MENU_ITEM_H;
        buf_rect(b, BODY_W, BODY_H, MENU_X + 1, y, MENU_W - 2,
                 MENU_ITEM_H - 1, rgb(38, 44, 58));
        buf_text(b, BODY_W, BODY_H, MENU_X + 8, y + 2, menu_items[r],
                 rgb(220, 228, 240), rgb(38, 44, 58));
    }
    clip_pop();
}

static void menu_choose(wm_window_t* win, int mi) {
    if (mi == 0) {
        log_append(&wins[0], "Hello from menu!\n");
        wins[0].scroll = TEXT_MAX;
        body_render(0);
    } else if (mi == 1) {
        wins[0].tlen = 0;
        wins[0].scroll = 0;
        body_render(0);
    } else {
        log_append(&wins[0], "Menu: font scale + dropdown\n");
        wins[0].scroll = TEXT_MAX;
        body_render(0);
    }
    win->menu_open = false;
}

static int list_max_scroll(wm_window_t* win) {
    (void)win;
    return LIST_COUNT > LIST_ROWS ? LIST_COUNT - LIST_ROWS : 0;
}

static void list_clamp(wm_window_t* win) {
    int maxs = list_max_scroll(win);
    if (win->list_scroll > maxs) win->list_scroll = maxs;
    if (win->list_scroll < 0) win->list_scroll = 0;
    if (win->list_sel < 0) win->list_sel = 0;
    if (win->list_sel >= LIST_COUNT) win->list_sel = LIST_COUNT - 1;
    if (win->list_sel < win->list_scroll)
        win->list_scroll = win->list_sel;
    if (win->list_sel >= win->list_scroll + LIST_ROWS)
        win->list_scroll = win->list_sel - LIST_ROWS + 1;
}

static void draw_list(wm_window_t* win, uint32_t* b) {
    list_clamp(win);
    clip_push(LIST_X, LIST_Y, LIST_W, LIST_H);
    buf_rect(b, BODY_W, BODY_H, LIST_X, LIST_Y, LIST_W, LIST_H,
             rgb(72, 82, 98));
    buf_rect(b, BODY_W, BODY_H, LIST_X + 1, LIST_Y + 1, LIST_W - 2,
             LIST_H - 2, rgb(18, 22, 30));

    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = win->list_scroll + r;
        if (idx >= LIST_COUNT) break;
        int y = LIST_Y + 1 + r * LIST_ROW_H;
        uint32_t bg = (idx == win->list_sel) ? rgb(42, 111, 219)
                                             : rgb(26, 32, 42);
        buf_rect(b, BODY_W, BODY_H, LIST_X + 1, y, LIST_W - 2 - LIST_SB,
                 LIST_ROW_H - 1, bg);
        buf_text(b, BODY_W, BODY_H, LIST_X + 6, y + 2, list_items[idx],
                 idx == win->list_sel ? rgb(245, 248, 255) : rgb(200, 210, 225),
                 bg);
    }

    if (LIST_COUNT > LIST_ROWS) {
        int track_x = LIST_X + LIST_W - LIST_SB + 1;
        buf_rect(b, BODY_W, BODY_H, track_x, LIST_Y + 1, LIST_SB - 2,
                 LIST_H - 2, rgb(34, 38, 46));
        int span = LIST_H - 2;
        int thumb_h = span * LIST_ROWS / LIST_COUNT;
        if (thumb_h < 14) thumb_h = 14;
        int maxs = list_max_scroll(win);
        int thumb_y = LIST_Y + 1 +
                      (maxs ? (span - thumb_h) * win->list_scroll / maxs : 0);
        buf_rect(b, BODY_W, BODY_H, track_x, thumb_y, LIST_SB - 2, thumb_h,
                 rgb(112, 128, 152));
    }
    clip_pop();

    char st[40];
    int p = 0;
    const char* pre = "sel: ";
    while (*pre && p < 39) st[p++] = *pre++;
    const char* s = list_items[win->list_sel];
    while (*s && p < 39) st[p++] = *s++;
    st[p] = '\0';
    buf_text(b, BODY_W, BODY_H, LIST_X, LIST_Y + LIST_H + 8, st,
             rgb(160, 190, 220), rgb(16, 20, 24));
}

/* Update the list scrollbar from a screen-space mouse y. */
static void list_scroll_drag(wm_window_t* win, int my) {
    int maxs = list_max_scroll(win);
    if (!maxs) { win->list_scroll = 0; return; }
    int top = win->y + TITLE_H + 1 + LIST_Y + 1;
    int span = LIST_H - 2;
    int thumb_h = span * LIST_ROWS / LIST_COUNT;
    if (thumb_h < 14) thumb_h = 14;
    int y = my - top - thumb_h / 2;
    if (y < 0) y = 0;
    if (y > span - thumb_h) y = span - thumb_h;
    win->list_scroll = (span - thumb_h) ? y * maxs / (span - thumb_h) : 0;
}

/* Widgets window: two buttons, an input box and a list control. */
static void body_render_widgets(int i) {
    wm_window_t* win = &wins[i];
    uint32_t* b = wbuf[i];
    buf_fill(b, BODY_W, BODY_H, rgb(16, 20, 24));

    buf_text(b, BODY_W, BODY_H, 10, 8, "Widgets demo",
             rgb(230, 230, 230), rgb(16, 20, 24));
    buf_text(b, BODY_W, BODY_H, 10, 32, "Type below, Enter sends to Terminal.",
             rgb(160, 175, 190), rgb(16, 20, 24));

    draw_button(b, W_BTN_X, W_BTN1_Y, "Say Hello");
    draw_button(b, W_BTN_X, W_BTN2_Y, "Clear Log");

    buf_rect(b, BODY_W, BODY_H, W_IN_X - 2, W_IN_Y - 2,
             W_IN_W + 4, W_IN_H + 4, rgb(96, 106, 126));
    buf_rect(b, BODY_W, BODY_H, W_IN_X, W_IN_Y, W_IN_W, W_IN_H,
             rgb(10, 12, 16));
    buf_text(b, BODY_W, BODY_H, W_IN_X + 6, W_IN_Y + 4, win->input,
             rgb(225, 235, 245), rgb(10, 12, 16));
    if (win->ilen < 31) {
        buf_rect(b, BODY_W, BODY_H, W_IN_X + 6 + win->ilen * GLYPH_W,
                 W_IN_Y + 4, 6, GLYPH_H, rgb(120, 200, 120));
    }

    draw_list(win, b);
    draw_menu(win, b);
}

static void body_render(int i) {
    clip_reset(BODY_W, BODY_H);
    if (i == WIDGET_WIN) body_render_widgets(i);
    else body_render_text(i);
}

static int widget_button_at(int bx, int by) {
    if (bx >= W_BTN_X && bx < W_BTN_X + W_BTN_W) {
        if (by >= W_BTN1_Y && by < W_BTN1_Y + W_BTN_H) return 1;
        if (by >= W_BTN2_Y && by < W_BTN2_Y + W_BTN_H) return 2;
    }
    return 0;
}

/* Map a mouse y coordinate inside the window body to a scroll offset. */
static void scroll_drag(wm_window_t* win, int my) {
    int total = win_total_lines(win);
    int rows = BODY_H / GLYPH_H;
    if (total <= rows) { win->scroll = 0; return; }
    int track_top = win->y + TITLE_H + 1 + 4;
    int track_h = BODY_H - 8;
    int thumb_h = track_h * rows / total;
    if (thumb_h < 16) thumb_h = 16;
    int span = track_h - thumb_h;
    int y = my - track_top - thumb_h / 2;
    if (y < 0) y = 0;
    if (y > span) y = span;
    win->scroll = span ? y * (total - rows) / span : 0;
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
    clip_reset(SW, SH);
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

        /* Body: copy the window's backing buffer into the screen image,
         * clipped to the window body rectangle. */
        clip_push(win->x + 1, win->y + TITLE_H + 1, BODY_W, BODY_H);
        for (int y = 0; y < BODY_H; y++) {
            int py = win->y + TITLE_H + y;
            if (py < 0 || py >= SH) continue;
            for (int x = 0; x < BODY_W; x++) {
                int px = win->x + 1 + x;
                if (px < 0 || px >= SW) continue;
                scr[py * SW + px] = wbuf[i][y * BODY_W + x];
            }
        }
        clip_pop();
    }

    /* Taskbar */
    int ty = SH - TASKBAR_H;
    if (ty > 0) {
        buf_rect(scr, SW, SH, 0, ty, SW, TASKBAR_H, rgb(18, 22, 30));
        clip_push(4, ty + 4, SW - 8, TASKBAR_H - 8);
        buf_text(scr, SW, SH, 10, ty + 6,
                 "MuOS WM  drag / scrollbar / list / buttons / type / ESC exits",
                 rgb(200, 210, 225), rgb(18, 22, 30));
        clip_pop();
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
    wins[3].x = 30; wins[3].y = SH - WIN_H - TASKBAR_H - 10;
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
    { const char* s = "System\nMuOS " MUOS_VERSION " window manager\n- per-window back buffers\n- compositor + double buffer\n- mouse drag / focus / close\n";
      wins[1].tlen = 0;
      for (int i = 0; s[i] && i < TEXT_MAX; i++) { wins[1].text[i] = s[i]; wins[1].tlen++; } }
    wins[2].tlen = 0;
    wins[3].tlen = 0;
    wins[3].input[0] = '\0';
    wins[3].list_sel = 0;
    wins[3].list_scroll = 0;
    strncpy(wins[0].title, "Terminal", 23);
    strncpy(wins[1].title, "System", 23);
    strncpy(wins[2].title, "Editor", 23);
    strncpy(wins[3].title, "Widgets", 23);

    int focus_i = -1;
    for (int i = 0; i < WIN_COUNT; i++) body_render(i);

    int mx = SW / 2, my = SH / 2;
    int drag_i = -1, drag_dx = 0, drag_dy = 0;
    int scroll_i = -1;
    int list_scroll_i = -1;
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
                } else if (i == WIDGET_WIN) {
                    int bx = mx - (win->x + 1);
                    int by = my - (win->y + TITLE_H + 1);
                    int btn = widget_button_at(bx, by);
                    if (bx >= MENU_X && bx < MENU_X + MENU_W &&
                        by >= MENU_Y && by < MENU_Y + MENU_H) {
                        win->menu_open = !win->menu_open;
                    } else if (win->menu_open) {
                        int mi = menu_item_at(bx, by);
                        if (mi >= 0) menu_choose(win, mi);
                        else win->menu_open = false;
                    } else if (btn == 1) {
                        log_append(&wins[0], "Hello from Widgets!\n");
                        wins[0].scroll = TEXT_MAX;      /* follow tail */
                        body_render(0);
                    } else if (btn == 2) {
                        wins[0].tlen = 0;
                        wins[0].scroll = 0;
                        body_render(0);
                    } else if (bx >= LIST_X && bx < LIST_X + LIST_W &&
                               by >= LIST_Y && by < LIST_Y + LIST_H) {
                        if (bx >= LIST_X + LIST_W - LIST_SB) {
                            list_scroll_i = i;
                            list_scroll_drag(win, my);
                        } else {
                            int row = (by - LIST_Y - 1) / LIST_ROW_H;
                            if (row >= 0 && row < LIST_ROWS) {
                                int idx = win->list_scroll + row;
                                if (idx >= 0 && idx < LIST_COUNT)
                                    win->list_sel = idx;
                            }
                        }
                        body_render(i);
                    }
                    body_render(i);
                } else if (mx >= win->x + WIN_W - 12 &&
                           my >= win->y + TITLE_H + 1 &&
                           my < win->y + TITLE_H + 1 + BODY_H) {
                    scroll_i = i;
                    scroll_drag(win, my);
                    body_render(i);
                }
            } else {
                focus_i = -1;
            }
        }
        if (!left) { drag_i = -1; scroll_i = -1; list_scroll_i = -1; }
        if (left && scroll_i >= 0) {
            scroll_drag(&wins[scroll_i], my);
            body_render(scroll_i);
        }
        if (left && list_scroll_i >= 0) {
            list_scroll_drag(&wins[list_scroll_i], my);
            body_render(list_scroll_i);
        }
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
            if (focus_i == WIDGET_WIN) {
                if (c == 8) {
                    if (win->ilen > 0) win->ilen--;
                } else if (c == '\r' || c == '\n') {
                    if (win->ilen > 0) {
                        log_append(&wins[0], win->input);
                        log_append(&wins[0], "\n");
                    }
                    win->ilen = 0;
                    wins[0].scroll = TEXT_MAX;      /* Terminal follows tail */
                    body_render(0);
                } else if (c >= 32 && c < 127) {
                    if (win->ilen < 63) win->input[win->ilen++] = c;
                }
                win->input[win->ilen] = '\0';
            } else {
                if (c == 8) {
                    if (win->tlen > 0) win->tlen--;
                } else if (c == '\r' || c == '\n') {
                    if (win->tlen < TEXT_MAX - 1) win->text[win->tlen++] = '\n';
                } else if (c >= 32 && c < 127) {
                    if (win->tlen < TEXT_MAX - 1) win->text[win->tlen++] = c;
                }
                win->scroll = TEXT_MAX;             /* follow tail while typing */
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
