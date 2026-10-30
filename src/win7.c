/* win7.c - MuOS 7 Desktop (Windows 7 style text-mode GUI)
 *
 * Layout: row 0 = title bar, rows 1..23 = desktop, row 24 = taskbar.
 * Windows are drawn in z-order (active window last), then taskbar, then
 * the start menu. The desktop idles in a HLT loop so the RTC clock and
 * memory display stay live instead of freezing until the next key. */
#include "win7.h"
#include "vga.h"
#include "keyboard.h"
#include "pit.h"
#include "rtc.h"
#include "mm.h"
#include "task.h"
#include "fs.h"
#include "editor.h"
#include "mouse.h"
#include "string.h"
#include "logo.h"

#define MAX_WIN    8
#define SCR_W      80
#define SCR_H      25
#define TASKBAR_Y  24
#define DESK_BOTTOM 23

#define MENU_X     0
#define MENU_Y     16
#define MENU_W     24
#define MENU_H     8

typedef struct {
    int x, y, w, h, id;
    const char* t;
    uint8_t bc;
    int open, maximized;
    int old_x, old_y, old_w, old_h;
} Win;

static Win wins[MAX_WIN];
static int nwin, active, start_menu, start_sel;

/* Draw order: zorder[0] is the bottom-most window. */
static int zorder[MAX_WIN];
static int zcount;

static int last_minimized = -1;

/* Software mouse cursor (text-mode block) */
static int      mouse_cx = 40, mouse_cy = 22;
static bool_t   mouse_visible = false;
static uint16_t mouse_saved;
static int      mouse_btn = 0;
static int      drag_win = -1, drag_off_x, drag_off_y;
static bool_t   desktop_exit = false;

/* Start-menu actions: 0/-2/5/6/1 are window indices, -2 = editor, -1 = exit */
static const int menu_actions[6] = { 0, -2, 5, 6, 1, -1 };

/* Calculator window state (id 6) */
static int  calc_acc, calc_cur;
static char calc_op;
static int  calc_fresh;

/* ── Low-level text helpers (all clipped to the 80x25 screen) ── */
static void put_cell(int x, int y, uint16_t v) {
    if (x < 0 || x >= SCR_W || y < 0 || y >= SCR_H) return;
    vga_put_cell((uint16_t)y, (uint16_t)x, v);
}

static void fd(int y, int x1, int x2, uint8_t c, char ch) {
    if (y < 0 || y >= SCR_H) return;
    if (x1 < 0) x1 = 0;
    if (x2 >= SCR_W) x2 = SCR_W - 1;
    uint16_t v = vga_entry(ch, c);
    for (int x = x1; x <= x2; x++)
        vga_put_cell((uint16_t)y, (uint16_t)x, v);
}

static void ft(int x, int y, const char* s, uint8_t c) {
    if (y < 0 || y >= SCR_H) return;
    while (*s) {
        if (x >= 0 && x < SCR_W)
            vga_put_cell((uint16_t)y, (uint16_t)x,
                         vga_entry((unsigned char)*s, c));
        x++; s++;
    }
}

/* Print at most max characters (window-internal clipping). */
static void ftn(int x, int y, int max, const char* s, uint8_t c) {
    if (y < 0 || y >= SCR_H || max <= 0) return;
    for (int i = 0; i < max && s[i]; i++) {
        if (x + i >= 0 && x + i < SCR_W)
            vga_put_cell((uint16_t)y, (uint16_t)(x + i),
                         vga_entry((unsigned char)s[i], c));
    }
}

/* ── Win7 colors ───────────────────────────────────── */
#define CLR_TASKBAR  vga_entry_color(VGA_WHITE, VGA_BLUE)
#define CLR_DESKTOP  vga_entry_color(VGA_LIGHT_GREY, VGA_DARK_GREY)
#define CLR_START    vga_entry_color(VGA_WHITE, VGA_CYAN)
#define CLR_TRAY     vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY)

static int append_int(char* b, int p, int v) {
    if (v < 0) b[p++] = '-';
    char tmp[12];
    uint32_t n = v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    uint32_t len = u32_to_dec(tmp, n);
    for (uint32_t i = 0; i < len; i++) b[p++] = tmp[i];
    return p;
}

/* ═══════════════════ Taskbar ═══════════════════ */
static void format_clock(char* tb) {
    rtc_time_t t;
    if (rtc_read_time(&t)) {
        tb[0] = (char)('0' + t.hour / 10);
        tb[1] = (char)('0' + t.hour % 10);
        tb[2] = ':';
        tb[3] = (char)('0' + t.minute / 10);
        tb[4] = (char)('0' + t.minute % 10);
        tb[5] = ':';
        tb[6] = (char)('0' + t.second / 10);
        tb[7] = (char)('0' + t.second % 10);
        tb[8] = '\0';
    } else {
        /* No usable RTC: show uptime as UP:MM so it is not mistaken for time */
        uint32_t s = pit_get_ticks() / 100;
        int m = (int)((s / 60) % 60);
        tb[0] = 'U'; tb[1] = 'P'; tb[2] = ':';
        tb[3] = (char)('0' + m / 10);
        tb[4] = (char)('0' + m % 10);
        tb[5] = '\0';
    }
}

static void draw_tray(void) {
    fd(TASKBAR_Y, 64, 79, CLR_TRAY, ' ');

    uint32_t total_mb = mm_get_total_pages() * 4 / 1024;
    uint32_t free_mb  = mm_get_free_pages() * 4 / 1024;
    uint32_t used_mb  = total_mb - free_mb;
    char b[16];
    int p = 0;
    b[p++] = 'M'; b[p++] = ':';
    p += (int)u32_to_dec(b + p, used_mb);
    b[p++] = '/';
    p += (int)u32_to_dec(b + p, total_mb);
    b[p] = '\0';
    ftn(64, TASKBAR_Y, 8, b, CLR_TRAY);

    char tb[10];
    format_clock(tb);
    ft(72, TASKBAR_Y, tb, CLR_TRAY);
}

static void draw_taskbar(void) {
    fd(TASKBAR_Y, 0, 7, CLR_START, ' ');
    ft(1, TASKBAR_Y, "[Start]", CLR_START);

    fd(TASKBAR_Y, 8, 63, CLR_TASKBAR, ' ');

    /* Compact tabs: up to five 10-column buttons before the tray. */
    int tx = 9;
    for (int i = 0; i < nwin; i++) {
        if (!wins[i].open) continue;
        if (tx + 10 > 64) break;

        uint8_t tc = (i == active)
            ? vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY)
            : vga_entry_color(VGA_WHITE, VGA_BLUE);

        char buf[11];
        int p = 0;
        const char* ns = wins[i].t;
        while (*ns && p < 10) buf[p++] = *ns++;
        while (p < 10) buf[p++] = ' ';
        buf[p] = '\0';
        ft(tx, TASKBAR_Y, buf, tc);
        tx += 11;
    }

    draw_tray();
}

/* ═══════════════════ Desktop ═══════════════════ */
static void draw_desktop(void) {
    for (int y = 1; y <= DESK_BOTTOM; y++)
        fd(y, 0, SCR_W - 1, CLR_DESKTOP, ' ');

    fd(0, 0, SCR_W - 1, CLR_TASKBAR, ' ');
    ft(3, 0, "MuOS 7  -  x86 Microkernel Desktop", CLR_TASKBAR);

    /* Desktop icons live left of the initial windows. Start at row 14:
     * the Terminal window's bottom shadow occupies row 13. */
    uint8_t ic = vga_entry_color(VGA_WHITE, VGA_DARK_GREY);
    ft(2, 14, "[1] Computer",   ic);
    ft(2, 15, "[2] Terminal",   vga_entry_color(VGA_YELLOW, VGA_DARK_GREY));
    ft(2, 16, "[3] Editor",     vga_entry_color(VGA_LIGHT_GREEN, VGA_DARK_GREY));
    ft(2, 17, "[4] Memory",     vga_entry_color(VGA_LIGHT_CYAN, VGA_DARK_GREY));
    ft(2, 18, "[5] Tasks",      vga_entry_color(VGA_LIGHT_MAGENTA, VGA_DARK_GREY));
    ft(2, 19, "[6] Calculator", vga_entry_color(VGA_LIGHT_RED, VGA_DARK_GREY));
    ft(2, 20, "[7] About",      vga_entry_color(VGA_WHITE, VGA_DARK_GREY));

    ft(2, 21, "MuOS 7 Ultimate", vga_entry_color(VGA_LIGHT_BLUE, VGA_DARK_GREY));
    ft(2, 22, "F1-F7 windows | ESC shell", vga_entry_color(VGA_LIGHT_GREY, VGA_DARK_GREY));
}

/* ═══════════════════ Window frame ═══════════════════ */
static void draw_win(Win* w, int focus) {
    int x = w->x, y = w->y, ww = w->w, hh = w->h;
    uint8_t tc = focus
        ? vga_entry_color(VGA_WHITE, VGA_BLUE)
        : vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY);
    uint8_t bc = w->bc;

    fd(y, x, x + ww - 1, tc, ' ');
    ftn(x + 1, y, ww - 8, w->t, tc);

    /* Window controls: _ [ ] X (only when there is room) */
    if (ww >= 12) {
        int cx = x + ww - 6;
        put_cell(cx,     y, vga_entry('_', vga_entry_color(VGA_BLACK, tc & 0x0F)));
        put_cell(cx + 1, y, vga_entry(0xFE, vga_entry_color(VGA_BLACK, tc & 0x0F)));
        put_cell(cx + 2, y, vga_entry('X', vga_entry_color(VGA_LIGHT_RED, tc & 0x0F)));
    }

    for (int r = y + 1; r < y + hh; r++) {
        put_cell(x, r, vga_entry(0xB3, bc));
        put_cell(x + ww - 1, r, vga_entry(0xB3, bc));
        fd(r, x + 1, x + ww - 2, bc, ' ');
    }

    int by = y + hh;
    if (by < SCR_H) {
        fd(by, x + 1, x + ww - 2, bc, 0xC4);
        put_cell(x, by, vga_entry(0xC0, bc));
        put_cell(x + ww - 1, by, vga_entry(0xD9, bc));
    }

    /* Drop shadow, clipped to the desktop (never over the taskbar). */
    if (x + ww < SCR_W) {
        for (int r = y + 1; r <= by && r <= DESK_BOTTOM; r++)
            put_cell(x + ww, r, vga_entry(0xB0, vga_entry_color(VGA_BLACK, VGA_BLACK)));
    }
    if (by + 1 <= DESK_BOTTOM) {
        int sx2 = x + ww + 1;
        if (sx2 >= SCR_W) sx2 = SCR_W - 1;
        fd(by + 1, x + 1, sx2, vga_entry_color(VGA_BLACK, VGA_BLACK), 0xB0);
    }
}

/* ═══════════════════ Window content ═══════════════════ */
static void draw_content(Win* w) {
    int wx = w->x, wy = w->y;
    switch (w->id) {
    case 1: /* Terminal */
        ft(wx + 2, wy + 1, "$ muos> _", vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
        ft(wx + 2, wy + 3, "Available commands:", vga_entry_color(VGA_YELLOW, VGA_BLACK));
        ft(wx + 2, wy + 4, "help clear echo mem", vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        ft(wx + 2, wy + 5, "tasks time ps logo", vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        ft(wx + 2, wy + 6, "touch ls cat rm write", vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        ft(wx + 2, wy + 7, "run exec fault edit", vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        ft(wx + 2, wy + 8, "calc version gui", vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        break;

    case 2: /* Computer / System info */
        { uint32_t tot_mb = mm_get_total_pages() * 4 / 1024;
          uint32_t fr_mb  = mm_get_free_pages() * 4 / 1024;
          uint32_t us_mb  = tot_mb - fr_mb;
          char b[16];
          ft(wx + 2, wy + 1, "=== System Information ===", vga_entry_color(VGA_GREEN, VGA_BLACK));
          ft(wx + 2, wy + 2, "OS:     MuOS 7 Ultimate", vga_entry_color(VGA_GREEN, VGA_BLACK));
          ft(wx + 2, wy + 3, "Arch:   x86 32-bit", vga_entry_color(VGA_GREEN, VGA_BLACK));
          ft(wx + 2, wy + 4, "RAM:    ", vga_entry_color(VGA_GREEN, VGA_BLACK));
          u32_to_dec(b, tot_mb);
          ft(wx + 10, wy + 4, b, vga_entry_color(VGA_GREEN, VGA_BLACK));
          ft(wx + 10 + (int)strlen(b) + 1, wy + 4, "MB", vga_entry_color(VGA_GREEN, VGA_BLACK));
          ft(wx + 2, wy + 5, "Used:   ", vga_entry_color(VGA_LIGHT_RED, VGA_BLACK));
          u32_to_dec(b, us_mb); ft(wx + 10, wy + 5, b, vga_entry_color(VGA_LIGHT_RED, VGA_BLACK));
          ft(wx + 10 + (int)strlen(b) + 1, wy + 5, "MB", vga_entry_color(VGA_LIGHT_RED, VGA_BLACK));
          ft(wx + 2, wy + 6, "Free:   ", vga_entry_color(VGA_GREEN, VGA_BLACK));
          u32_to_dec(b, fr_mb); ft(wx + 10, wy + 6, b, vga_entry_color(VGA_GREEN, VGA_BLACK));
          ft(wx + 10 + (int)strlen(b) + 1, wy + 6, "MB", vga_entry_color(VGA_GREEN, VGA_BLACK));
          /* Usage bar */
          int bar = 24, f = tot_mb ? (int)(us_mb * bar / tot_mb) : 0;
          ft(wx + 2, wy + 8, "[", vga_entry_color(VGA_GREEN, VGA_BLACK));
          for (int i = 0; i < f; i++)
              ft(wx + 3 + i, wy + 8, "=", vga_entry_color(VGA_LIGHT_RED, VGA_BLACK));
          for (int i = f; i < bar; i++)
              ft(wx + 3 + i, wy + 8, " ", vga_entry_color(VGA_BLACK, VGA_BLACK));
          ft(wx + 3 + bar, wy + 8, "]", vga_entry_color(VGA_GREEN, VGA_BLACK));
        } break;

    case 3: /* Tasks */
        ft(wx + 2, wy + 1, "PID  Name          State", vga_entry_color(VGA_YELLOW, VGA_BLACK));
        ft(wx + 2, wy + 2, "---  ------------  -----", vga_entry_color(VGA_YELLOW, VGA_BLACK));
        { task_t* t = task_get_current();
          if (t) {
              int yy = wy + 3;
              task_t* cur = t;
              do {
                  char b[40];
                  int p = 0;
                  char tb[12]; u32_to_dec(tb, cur->pid);
                  int tl = (int)strlen(tb);
                  while (tl < 5) { b[p++] = ' '; tl++; }
                  for (int i = 0; tb[i]; i++) b[p++] = tb[i];
                  b[p++] = ' ';
                  for (const char* s = cur->name; *s && p < 20; s++) b[p++] = *s;
                  while (p < 20) b[p++] = ' ';
                  const char* st = cur->state == TASK_RUNNING ? "Running" : "Ready  ";
                  while (*st && p < 38) b[p++] = *st++;
                  b[p] = '\0';
                  ftn(wx + 2, yy++, w->w - 4, b, vga_entry_color(VGA_CYAN, VGA_BLACK));
                  cur = cur->next;
              } while (cur && cur != t && yy < wy + w->h - 1);
          }
        } break;

    case 4: /* Help */
        ft(wx + 2, wy + 1, "    MuOS 7 Help", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        ft(wx + 2, wy + 2, "Mouse   Click / drag / X", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        ft(wx + 2, wy + 3, "TAB     Switch window", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        ft(wx + 2, wy + 4, "W/S     Minimize/Restore", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        ft(wx + 2, wy + 5, "M       Maximize window", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        ft(wx + 2, wy + 6, "Arrows  Move window", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        ft(wx + 2, wy + 7, "DEL     Close window", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        ft(wx + 2, wy + 8, "SPACE   Start menu", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        ft(wx + 2, wy + 9, "F1-F7   Open window", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        break;

    case 5: /* About / Logo */
        for (int i = 0; i < 5; i++)
            ft(wx + 2, wy + 1 + i, muos_logo[i], vga_entry_color(VGA_WHITE, VGA_BLUE));
        ft(wx + 2, wy + 7, "MuOS 7 Ultimate Edition", vga_entry_color(VGA_LIGHT_CYAN, VGA_BLUE));
        ft(wx + 2, wy + 8, "x86 32-bit Microkernel", vga_entry_color(VGA_LIGHT_GREY, VGA_BLUE));
        ft(wx + 2, wy + 9, "Built: " __DATE__, vga_entry_color(VGA_LIGHT_GREY, VGA_BLUE));
        break;

    case 6: { /* Calculator (interactive while focused) */
        ft(wx + 2, wy + 1, "=== Calculator ===", vga_entry_color(VGA_YELLOW, VGA_BLACK));
        char b[32];
        int p = 0;
        if (calc_op) {
            p = append_int(b, p, calc_acc);
            b[p++] = ' '; b[p++] = calc_op; b[p++] = ' ';
            p = append_int(b, p, calc_cur);
        } else {
            p = append_int(b, p, calc_cur);
        }
        b[p] = '\0';
        ftn(wx + 2, wy + 3, w->w - 4, b, vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
        ft(wx + 2, wy + 5, "Digits + - * / = C", vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        ft(wx + 2, wy + 6, "while this window is active", vga_entry_color(VGA_DARK_GREY, VGA_BLACK));
        break;
    }

    case 7: /* File Browser */
        ft(wx + 2, wy + 1, "=== File Browser ===", vga_entry_color(VGA_YELLOW, VGA_BLACK));
        { int n = fs_count();
          char b[8]; u32_to_dec(b, n);
          ft(wx + 2, wy + 2, "Files: ", vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
          ft(wx + 9, wy + 2, b, vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
          int yy = wy + 3;
          for (int i = 0; i < n && yy < wy + w->h - 1; i++) {
              const char* name = fs_name(i);
              uint32_t sz = fs_size(i);
              ftn(wx + 2, yy, 17, name, vga_entry_color(VGA_CYAN, VGA_BLACK));
              u32_to_dec(b, sz);
              ftn(wx + 20, yy, 5, b, vga_entry_color(VGA_DARK_GREY, VGA_BLACK));
              ft(wx + 20 + 5, yy, "B", vga_entry_color(VGA_DARK_GREY, VGA_BLACK));
              yy++;
          }
          if (n == 0)
              ft(wx + 2, yy, "(empty)", vga_entry_color(VGA_DARK_GREY, VGA_BLACK));
        } break;
    }
}

/* ═══════════════════ Start menu ═══════════════════ */
static void draw_start_menu(void) {
    int sx = MENU_X, sy = MENU_Y, sw = MENU_W, sh = MENU_H;
    uint8_t mc = vga_entry_color(VGA_BLACK, VGA_WHITE);
    uint8_t hc = vga_entry_color(VGA_WHITE, VGA_BLUE);

    for (int y = sy; y < sy + sh; y++)
        fd(y, sx, sx + sw - 1, mc, ' ');

    for (int y = sy; y < sy + sh; y++) {
        put_cell(sx, y, vga_entry(0xB3, vga_entry_color(VGA_WHITE, VGA_BLUE)));
        put_cell(sx + sw - 1, y, vga_entry(0xB3, vga_entry_color(VGA_WHITE, VGA_BLUE)));
    }

    fd(sy, sx + 1, sx + sw - 2, vga_entry_color(VGA_WHITE, VGA_BLUE), ' ');
    ft(sx + 1, sy, "  MuOS 7  -  User", vga_entry_color(VGA_WHITE, VGA_BLUE));

    const char* items[] = {
        "  > Terminal",
        "  > Text Editor",
        "  > Calculator",
        "  > File Browser",
        "  > System Info",
        "  > Exit Desktop"
    };
    for (int i = 0; i < 6; i++) {
        uint8_t c = (i == start_sel) ? hc : mc;
        fd(sy + 1 + i, sx + 1, sx + sw - 2, c, ' ');
        ft(sx + 2, sy + 1 + i, items[i], c);
    }
    fd(sy + 7, sx + 1, sx + sw - 2, mc, 0xC4);
}

/* ═══════════════════ Window management ═══════════════════ */
static void raise_win(int idx) {
    int pos = -1;
    for (int i = 0; i < zcount; i++)
        if (zorder[i] == idx) { pos = i; break; }
    if (pos < 0) return;
    for (int i = pos; i < zcount - 1; i++) zorder[i] = zorder[i + 1];
    zorder[zcount - 1] = idx;
}

static void add_win(int x, int y, int w, int h, int id, const char* t, uint8_t bc) {
    if (nwin >= MAX_WIN) return;
    wins[nwin].x = x; wins[nwin].y = y;
    wins[nwin].w = w; wins[nwin].h = h;
    wins[nwin].id = id; wins[nwin].t = t;
    wins[nwin].bc = bc; wins[nwin].open = 1;
    wins[nwin].maximized = 0;
    wins[nwin].old_x = x; wins[nwin].old_y = y;
    wins[nwin].old_w = w; wins[nwin].old_h = h;
    zorder[zcount++] = nwin;
    active = nwin;
    nwin++;
}

static void mv(Win* w, int dx, int dy) {
    if (w->maximized) return;
    w->x += dx; w->y += dy;
    if (w->x < 1) w->x = 1;
    if (w->y < 1) w->y = 1;
    if (w->x + w->w > SCR_W - 1) w->x = SCR_W - 1 - w->w;
    if (w->y + w->h > DESK_BOTTOM) w->y = DESK_BOTTOM - w->h;
}

static void toggle_maximize(Win* w) {
    if (w->maximized) {
        w->x = w->old_x; w->y = w->old_y;
        w->w = w->old_w; w->h = w->old_h;
        w->maximized = 0;
    } else {
        w->old_x = w->x; w->old_y = w->y;
        w->old_w = w->w; w->old_h = w->h;
        w->x = 1; w->y = 1; w->w = SCR_W - 2; w->h = DESK_BOTTOM - 1;
        w->maximized = 1;
    }
}

/* Mouse cursor save/restore: every full redraw hides it first, then
 * draws it again on top so it never leaves stale cells behind. */
static void mouse_show(void) {
    if (mouse_visible) return;
    if (mouse_cx < 0 || mouse_cx >= SCR_W || mouse_cy < 0 || mouse_cy >= SCR_H) return;
    uint16_t orig = vga_get_cell((uint16_t)mouse_cy, (uint16_t)mouse_cx);
    mouse_saved = orig;
    /* Highlight the cell by inverting its colors instead of replacing the
     * character, so the text under the pointer stays readable. */
    uint8_t ch = (uint8_t)(orig & 0xFF);
    if (ch == 0) ch = ' ';
    uint8_t attr = (uint8_t)((orig >> 8) ^ 0x77);
    vga_put_cell((uint16_t)mouse_cy, (uint16_t)mouse_cx, vga_entry(ch, attr));
    mouse_visible = true;
}

static void mouse_hide(void) {
    if (!mouse_visible) return;
    if (mouse_cx >= 0 && mouse_cx < SCR_W && mouse_cy >= 0 && mouse_cy < SCR_H) {
        vga_put_cell((uint16_t)mouse_cy, (uint16_t)mouse_cx, mouse_saved);
    }
    mouse_visible = false;
}

static void redraw_all(void) {
    mouse_hide();
    draw_desktop();
    for (int z = 0; z < zcount; z++) {
        int i = zorder[z];
        if (wins[i].open) {
            draw_win(&wins[i], i == active);
            draw_content(&wins[i]);
        }
    }
    draw_taskbar();
    if (start_menu) draw_start_menu();
    mouse_show();
}

/* ═══════════════════ Calculator input ═══════════════════ */
static bool_t calc_key(char c) {
    if (c >= '0' && c <= '9') {
        if (calc_fresh) { calc_cur = 0; calc_fresh = 0; }
        if (calc_cur < 100000000) calc_cur = calc_cur * 10 + (c - '0');
        return true;
    }

    if (c == '+' || c == '-' || c == '*' || c == '/') {
        if (calc_op) {
            switch (calc_op) {
                case '+': calc_acc += calc_cur; break;
                case '-': calc_acc -= calc_cur; break;
                case '*': calc_acc *= calc_cur; break;
                case '/': calc_acc = calc_cur ? calc_acc / calc_cur : 0; break;
            }
        } else {
            calc_acc = calc_cur;
        }
        calc_op = c;
        calc_cur = 0;
        calc_fresh = 1;
        return true;
    }

    if (c == '=' || c == '\n' || c == '\r') {
        if (calc_op) {
            switch (calc_op) {
                case '+': calc_acc += calc_cur; break;
                case '-': calc_acc -= calc_cur; break;
                case '*': calc_acc *= calc_cur; break;
                case '/': calc_acc = calc_cur ? calc_acc / calc_cur : 0; break;
            }
        } else {
            calc_acc = calc_cur;
        }
        calc_cur = calc_acc;
        calc_op = 0;
        calc_fresh = 1;
        return true;
    }

    if (c == 'c' || c == 'C') {
        calc_acc = calc_cur = 0;
        calc_op = 0;
        calc_fresh = 1;
        return true;
    }
    return false;
}

static void calc_reset(void) {
    calc_acc = calc_cur = 0;
    calc_op = 0;
    calc_fresh = 1;
}

/* Open a window and bring it to the front. */
static void focus_win(int i) {
    if (i < 0 || i >= nwin) return;
    wins[i].open = 1;
    active = i;
    raise_win(i);
}

static void activate_action(int action) {
    start_menu = 0;
    if (action == -1) {
        desktop_exit = true;
        return;
    }
    if (action == -2) {
        editor_run("notes.txt");
        vga_hide_cursor();
        redraw_all();
        return;
    }
    if (action >= 0 && action < nwin) {
        focus_win(action);
        redraw_all();
    }
}

/* Left-click routing for the desktop, taskbar, menu, icons and windows. */
static void mouse_click(int x, int y) {
    if (start_menu) {
        if (x >= MENU_X && x < MENU_X + MENU_W &&
            y >= MENU_Y && y < MENU_Y + MENU_H) {
            int item = y - (MENU_Y + 1);
            if (item >= 0 && item < 6) {
                activate_action(menu_actions[item]);
                return;
            }
        }
        start_menu = 0;
        redraw_all();
        return;
    }

    if (y == TASKBAR_Y) {
        if (x <= 7) {
            start_menu = 1;
            start_sel = 0;
            redraw_all();
            return;
        }
        if (x >= 9 && x < 64) {
            int tx = 9;
            for (int i = 0; i < nwin; i++) {
                if (!wins[i].open) continue;
                if (x >= tx && x < tx + 10) {
                    focus_win(i);
                    redraw_all();
                    return;
                }
                tx += 11;
            }
        }
        return;
    }

    /* Desktop icons (left column rows 14..20) */
    if (x >= 2 && x <= 18 && y >= 14 && y <= 20) {
        static const int icon_actions[7] = { 1, 0, -2, 1, 2, 5, 4 };
        activate_action(icon_actions[y - 14]);
        return;
    }

    /* Topmost window under the pointer */
    for (int z = zcount - 1; z >= 0; z--) {
        int i = zorder[z];
        Win* w = &wins[i];
        if (!w->open) continue;
        if (x < w->x || x >= w->x + w->w ||
            y < w->y || y >= w->y + w->h) continue;

        if (y == w->y) {
            int close_x = w->x + w->w - 4;   /* X cell */
            if (x >= close_x) {
                w->open = 0;
                int found = -1;
                for (int j = 0; j < nwin; j++)
                    if (wins[j].open) { found = j; break; }
                active = found;
                if (found >= 0) raise_win(found);
                redraw_all();
                return;
            }
            if (x == close_x - 1) {          /* maximize */
                toggle_maximize(w);
                raise_win(i);
                redraw_all();
                return;
            }
            if (x == close_x - 2) {          /* minimize */
                last_minimized = i;
                w->open = 0;
                int found = -1;
                for (int j = 0; j < nwin; j++)
                    if (wins[j].open) { found = j; break; }
                active = found;
                if (found >= 0) raise_win(found);
                redraw_all();
                return;
            }
            focus_win(i);
            if (!w->maximized) {
                drag_win = i;
                drag_off_x = x - w->x;
                drag_off_y = y - w->y;
            }
            redraw_all();
            return;
        }

        focus_win(i);
        redraw_all();
        return;
    }
}

static void mouse_drag(int x, int y) {
    if (drag_win < 0) return;
    Win* w = &wins[drag_win];
    if (w->maximized || !w->open) return;

    int nx = x - drag_off_x;
    int ny = y - drag_off_y;
    if (nx < 1) nx = 1;
    if (ny < 1) ny = 1;
    if (nx + w->w > SCR_W - 1) nx = SCR_W - 1 - w->w;
    if (ny + w->h > DESK_BOTTOM) ny = DESK_BOTTOM - w->h;
    if (nx != w->x || ny != w->y) {
        w->x = nx;
        w->y = ny;
        redraw_all();
    }
}

static void mouse_poll(void) {
    mouse_state_t ms = mouse_get_state();

    if (ms.x != mouse_cx || ms.y != mouse_cy) {
        mouse_hide();
        mouse_cx = ms.x;
        mouse_cy = ms.y;
        mouse_show();
    }

    int left = ms.btn_left;
    if (left && !mouse_btn) {
        mouse_btn = 1;
        mouse_click(mouse_cx, mouse_cy);
    } else if (!left && mouse_btn) {
        mouse_btn = 0;
        drag_win = -1;
    } else if (left && mouse_btn && drag_win >= 0) {
        mouse_drag(mouse_cx, mouse_cy);
    }
}

/* ═══════════════════ Main loop ═══════════════════ */
void win7_run(void) {
    keyboard_flush();
    vga_clear();
    vga_hide_cursor();

    nwin = 0; zcount = 0; active = -1;
    start_menu = 0; start_sel = 0;
    last_minimized = -1;
    calc_reset();
    mouse_visible = false;
    mouse_btn = 0;
    drag_win = -1;
    desktop_exit = false;
    { mouse_state_t ms = mouse_get_state();
      mouse_cx = ms.x; mouse_cy = ms.y; }

    /* Initial layout: icons stay visible in the left column, windows sit
     * on the right. The remaining windows start minimized. */
    add_win(2, 2, 36, 10, 1, "Terminal",      vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
    add_win(40, 2, 36, 10, 2, "Computer",     vga_entry_color(VGA_GREEN, VGA_BLACK));
    add_win(40, 13, 36, 8,  3, "Tasks",       vga_entry_color(VGA_CYAN, VGA_BLACK));
    add_win(40, 13, 36, 10, 4, "Help",        vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
    add_win(20, 2, 40, 10, 5, "About MuOS 7", vga_entry_color(VGA_WHITE, VGA_BLUE));
    add_win(20, 13, 36, 8, 6, "Calculator",   vga_entry_color(VGA_LIGHT_RED, VGA_BLACK));
    add_win(40, 2, 36, 8,  7, "Files",        vga_entry_color(VGA_YELLOW, VGA_BLACK));

    wins[3].open = 0;  /* Help */
    wins[4].open = 0;  /* About */
    wins[5].open = 0;  /* Calculator */
    wins[6].open = 0;  /* Files */
    active = 0;
    raise_win(0);
    redraw_all();

    uint32_t last_tray = pit_get_ticks();

    while (1) {
        /* Idle until a key arrives; keep RTC/memory in the tray fresh.
         * HLT sleeps until the next IRQ (PIT at 100Hz). IF is on here. */
        while (!keyboard_haschar()) {
            mouse_poll();
            if (desktop_exit) break;

            uint32_t now = pit_get_ticks();
            if (now - last_tray >= 100) {
                last_tray = now;
                mouse_hide();
                draw_tray();
                mouse_show();
            }
            __asm__ volatile ("sti; hlt");
        }

        if (desktop_exit) {
            mouse_visible = false;
            vga_show_cursor();
            vga_clear();
            return;
        }

        char c = keyboard_getchar();

        /* Start menu mode */
        if (start_menu) {
            switch ((unsigned char)c) {
            case KEY_UP:
                if (start_sel > 0) start_sel--;
                redraw_all();
                break;
            case KEY_DOWN:
                if (start_sel < 5) start_sel++;
                redraw_all();
                break;
            case '\n': case '\r': {
                activate_action(menu_actions[start_sel]);
                if (desktop_exit) {
                    mouse_visible = false;
                    vga_show_cursor();
                    vga_clear();
                    return;
                }
                break;
            }
            case KEY_ESC: case ' ':
                start_menu = 0;
                redraw_all();
                break;
            }
            continue;
        }

        /* Calculator consumes its keys while focused, before globals. */
        if (active >= 0 && wins[active].open && wins[active].id == 6 && calc_key(c)) {
            redraw_all();
            continue;
        }

        /* Move-accumulator: drain same-direction repeats, redraw once. */
        {
            unsigned char uc = (unsigned char)c;
            if (uc == KEY_UP || uc == KEY_DOWN || uc == KEY_LEFT || uc == KEY_RIGHT) {
                int dx = 0, dy = 0;
                if (uc == KEY_UP)    dy = -1;
                if (uc == KEY_DOWN)  dy =  1;
                if (uc == KEY_LEFT)  dx = -1;
                if (uc == KEY_RIGHT) dx =  1;
                for (int i = 0; i < 32; i++) {
                    if (!keyboard_haschar()) break;
                    unsigned char n = (unsigned char)keyboard_peek();
                    if (n != uc) break;
                    keyboard_getchar();
                    if (active >= 0) mv(&wins[active], dx, dy);
                }
                if (active >= 0) mv(&wins[active], dx, dy);
                redraw_all();
                continue;
            }
        }

        switch ((unsigned char)c) {
        case KEY_ESC:
            mouse_visible = false;
            vga_show_cursor();
            vga_clear();
            return;

        case ' ':
            start_menu = 1;
            start_sel = 0;
            redraw_all();
            break;

        case '\t': {
            if (nwin > 0) {
                int old = active;
                do {
                    active = (active + 1) % nwin;
                } while (!wins[active].open && active != old);
                if (wins[active].open) raise_win(active);
                redraw_all();
            }
        } break;

        case 'w': case 'W':
            if (active >= 0 && wins[active].open) {
                last_minimized = active;
                wins[active].open = 0;
                int found = -1;
                for (int i = 0; i < nwin; i++)
                    if (wins[i].open) { found = i; break; }
                active = found;
                if (found >= 0) raise_win(found);
                redraw_all();
            }
            break;

        case 's': case 'S': {
            int found = -1;
            if (last_minimized >= 0 && !wins[last_minimized].open)
                found = last_minimized;
            else {
                for (int i = 0; i < nwin; i++)
                    if (!wins[i].open) { found = i; break; }
            }
            if (found >= 0) {
                focus_win(found);
                last_minimized = -1;
                redraw_all();
            }
        } break;

        case 'm': case 'M':
            if (active >= 0 && wins[active].open) {
                toggle_maximize(&wins[active]);
                raise_win(active);
                redraw_all();
            }
            break;

        case KEY_DELETE:
            if (active >= 0) {
                wins[active].open = 0;
                int found = -1;
                for (int i = 0; i < nwin; i++)
                    if (wins[i].open) { found = i; break; }
                active = found;
                if (found >= 0) raise_win(found);
                redraw_all();
            }
            break;

        case KEY_F1: case KEY_F2: case KEY_F3:
        case KEY_F4: case KEY_F5: case KEY_F6:
        case KEY_F7: {
            int id = (unsigned char)c - KEY_F1;
            if (id >= 0 && id < nwin) {
                focus_win(id);
                redraw_all();
            }
        } break;
        }
    }
}
