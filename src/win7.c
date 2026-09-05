/* win7.c - MuOS 7 Desktop (Windows 7 style) */
#include "win7.h"
#include "vga.h"
#include "keyboard.h"
#include "pit.h"
#include "mm.h"
#include "task.h"
#include "fs.h"
#include "string.h"
#include "logo.h"

#define MAX_WIN 8
typedef struct {
    int x, y, w, h, id;
    const char* t;
    uint8_t bc;
    int open, maximized;
    int old_x, old_y, old_w, old_h;
} Win;

static Win wins[MAX_WIN];
static int nwin, active, start_menu, start_sel;

/* ── Helpers ────────────────────────────────────────── */
static void fd(int y, int x1, int x2, uint8_t c, char ch) {
    uint16_t v = vga_entry(ch, c);
    for (int x = x1; x <= x2; x++)
        ((volatile uint16_t*)0xB8000)[y*80+x] = v;
}
static void ft(int x, int y, const char* s, uint8_t c) {
    while (*s) {
        ((volatile uint16_t*)0xB8000)[y*80+x] = vga_entry(*s, c);
        x++; s++;
    }
}

/* ── Win7 Colors ────────────────────────────────────── */
#define CLR_TASKBAR  vga_entry_color(VGA_WHITE, VGA_BLUE)
#define CLR_DESKTOP  vga_entry_color(VGA_LIGHT_GREY, VGA_DARK_GREY)
#define CLR_START    vga_entry_color(VGA_WHITE, VGA_CYAN)
#define CLR_TRAY     vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY)

/* ═══════════════════ Taskbar ═══════════════════ */
static void draw_taskbar(void) {
    int y = 23;
    /* Start button (glowing cyan) */
    fd(y, 0, 7, CLR_START, ' ');
    ft(1, y, "[Start]", CLR_START);
    /* Tab area */
    fd(y, 8, 63, CLR_TASKBAR, ' ');
    /* Window tabs */
    int tx = 9;
    for (int i = 0; i < nwin && tx < 62; i++) {
        if (!wins[i].open) continue;
        uint8_t tc = (i == active)
            ? vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY)
            : vga_entry_color(VGA_WHITE, VGA_BLUE);
        char buf[20];
        int p = 0;
        const char* ns = wins[i].t;
        while (*ns && p < 18) buf[p++] = *ns++;
        while (p < 18) buf[p++] = ' ';
        buf[p] = 0;
        ft(tx, y, buf, tc);
        tx += 19;
    }
    /* System tray */
    fd(y, 64, 79, CLR_TRAY, ' ');
    /* Memory */
    { char b[12]; b[0]='M'; b[1]=':';
      uint32_t fm = mm_get_free_pages()*4/1024;
      u32_to_dec(b+2, fm);
      int ml = 2 + (int)strlen(b + 2);
      b[ml++] = 'M'; b[ml] = 0;
      ft(64, y, b, vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
    }
    /* Clock */
    { uint32_t s = pit_get_ticks() / 100;
      int h = s / 3600, m = (s / 60) % 60;
      char tb[8];
      tb[0] = (h/10) ? (h/10)+'0' : ' ';
      tb[1] = h%10 + '0';
      tb[2] = ':';
      tb[3] = m/10 + '0';
      tb[4] = m%10 + '0';
      tb[5] = 0;
      ft(72, y, tb, vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
    }
}

/* ═══════════════════ Desktop ═══════════════════ */
static void draw_desktop(void) {
    /* Dark gradient-like background */
    for (int y = 1; y < 23; y++)
        fd(y, 0, 79, CLR_DESKTOP, ' ');
    /* Top title bar */
    fd(0, 0, 79, CLR_TASKBAR, ' ');
    ft(3, 0, "MuOS 7  -  x86 Microkernel Desktop", CLR_TASKBAR);
    /* Desktop icons (left column) */
    ft(2, 2, "[1] Computer",   vga_entry_color(VGA_WHITE, VGA_BLACK));
    ft(2, 3, "[2] Terminal",   vga_entry_color(VGA_YELLOW, VGA_BLACK));
    ft(2, 4, "[3] Editor",     vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
    ft(2, 5, "[4] Memory",     vga_entry_color(VGA_LIGHT_CYAN, VGA_BLACK));
    ft(2, 6, "[5] Tasks",      vga_entry_color(VGA_LIGHT_MAGENTA, VGA_BLACK));
    ft(2, 7, "[6] Calculator", vga_entry_color(VGA_LIGHT_RED, VGA_BLACK));
    ft(2, 8, "[7] About",      vga_entry_color(VGA_WHITE, VGA_BLACK));
    /* System info bottom-left */
    ft(2, 21, "MuOS 7 Ultimate", vga_entry_color(VGA_LIGHT_BLUE, VGA_BLACK));
    ft(2, 22, "256MB | x86 | GCC 15", vga_entry_color(VGA_DARK_GREY, VGA_BLACK));
}

/* ═══════════════════ Window Frame ═══════════════════ */
static void draw_win(Win* w, int focus) {
    uint16_t* vm = (uint16_t*)0xB8000;
    int x = w->x, y = w->y, ww = w->w, hh = w->h;
    uint8_t tc = focus
        ? vga_entry_color(VGA_WHITE, VGA_BLUE)
        : vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY);
    uint8_t bc = w->bc;
    /* Title bar with gradient feel */
    fd(y, x, x+ww-1, tc, ' ');
    ft(x+1, y, w->t, tc);
    /* Window controls: _ [] X */
    vm[y*80+x+ww-6] = vga_entry('_', vga_entry_color(VGA_BLACK, tc & 0x0F));
    vm[y*80+x+ww-5] = vga_entry(0xFE, vga_entry_color(VGA_BLACK, tc & 0x0F));
    vm[y*80+x+ww-4] = vga_entry('X', vga_entry_color(VGA_LIGHT_RED, tc & 0x0F));
    vm[y*80+x+ww-3] = vga_entry(' ', tc);
    vm[y*80+x+ww-2] = vga_entry(' ', tc);
    vm[y*80+x+ww-1] = vga_entry(' ', tc);
    /* Body + borders */
    for (int r = y+1; r < y+hh; r++) {
        vm[r*80+x] = vga_entry(0xB3, bc);
        vm[r*80+x+ww-1] = vga_entry(0xB3, bc);
        fd(r, x+1, x+ww-2, bc, ' ');
    }
    int by = y + hh;
    fd(by, x+1, x+ww-2, bc, 0xC4);
    vm[by*80+x] = vga_entry(0xC0, bc);
    vm[by*80+x+ww-1] = vga_entry(0xD9, bc);
    /* Drop shadow */
    for (int r = y+1; r <= by; r++)
        vm[r*80+x+ww] = vga_entry(0xB0, vga_entry_color(VGA_BLACK, VGA_BLACK));
    fd(by+1, x+1, x+ww+1, vga_entry_color(VGA_BLACK, VGA_BLACK), 0xB0);
}

/* ═══════════════════ Window Content ═══════════════════ */
static void draw_content(Win* w) {
    int wx = w->x, wy = w->y;
    switch (w->id) {
    case 1: /* Terminal */
        ft(wx+2, wy+1, "$ muos> _", vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
        ft(wx+2, wy+3, "Available commands:", vga_entry_color(VGA_YELLOW, VGA_BLACK));
        ft(wx+2, wy+4, "help clear echo mem", vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        ft(wx+2, wy+5, "tasks time ps logo", vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        ft(wx+2, wy+6, "touch ls cat rm write", vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        ft(wx+2, wy+7, "edit <file> win7 gui", vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        ft(wx+2, wy+8, "calc version reboot", vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        break;
    case 2: /* Computer / Memory */
        { uint32_t tot = mm_get_total_pages()*4;
          uint32_t fr = mm_get_free_pages()*4;
          uint32_t us = tot - fr;
          char b[16];
          ft(wx+2, wy+1, "=== System Information ===", vga_entry_color(VGA_GREEN, VGA_BLACK));
          ft(wx+2, wy+2, "OS:     MuOS 7 Ultimate", vga_entry_color(VGA_GREEN, VGA_BLACK));
          ft(wx+2, wy+3, "Arch:   x86 32-bit", vga_entry_color(VGA_GREEN, VGA_BLACK));
          ft(wx+2, wy+4, "RAM:    256 MB", vga_entry_color(VGA_GREEN, VGA_BLACK));
          ft(wx+2, wy+5, "Total:  ", vga_entry_color(VGA_GREEN, VGA_BLACK));
          u32_to_dec(b, tot); ft(wx+10, wy+5, b, vga_entry_color(VGA_GREEN, VGA_BLACK));
          ft(wx+10+2, wy+5, "KB", vga_entry_color(VGA_GREEN, VGA_BLACK));
          ft(wx+2, wy+6, "Used:   ", vga_entry_color(VGA_LIGHT_RED, VGA_BLACK));
          u32_to_dec(b, us); ft(wx+10, wy+6, b, vga_entry_color(VGA_LIGHT_RED, VGA_BLACK));
          ft(wx+2, wy+7, "Free:   ", vga_entry_color(VGA_GREEN, VGA_BLACK));
          u32_to_dec(b, fr); ft(wx+10, wy+7, b, vga_entry_color(VGA_GREEN, VGA_BLACK));
          /* Usage bar */
          int bar = 24, f = tot ? (int)((uint32_t)us*bar/tot) : 0;
          ft(wx+2, wy+8, "[", vga_entry_color(VGA_GREEN, VGA_BLACK));
          for (int i = 0; i < f; i++)
              ft(wx+3+i, wy+8, "=", vga_entry_color(VGA_LIGHT_RED, VGA_BLACK));
          for (int i = f; i < bar; i++)
              ft(wx+3+i, wy+8, " ", vga_entry_color(VGA_BLACK, VGA_BLACK));
          ft(wx+3+bar, wy+8, "]", vga_entry_color(VGA_GREEN, VGA_BLACK));
        } break;
    case 3: /* Tasks */
        ft(wx+2, wy+1, "PID  Name          State", vga_entry_color(VGA_YELLOW, VGA_BLACK));
        ft(wx+2, wy+2, "---  ------------  -----", vga_entry_color(VGA_YELLOW, VGA_BLACK));
        { task_t* t = task_get_current();
          if (t) {
              int yy = wy + 3;
              task_t* cur = t;
              do {
                  char b[40];
                  int p = 0;
                  { char tb[12]; u32_to_dec(tb, cur->pid);
                    int tl = 0; while (tb[tl]) tl++;
                    while (tl < 5) { b[p++] = ' '; tl++; }
                    for (int i = 0; tb[i]; i++) b[p++] = tb[i];
                    b[p++] = ' ';
                  }
                  for (const char* s = cur->name; *s && p < 20; s++) b[p++] = *s;
                  while (p < 20) b[p++] = ' ';
                  const char* st = cur->state == 1 ? "Running" : "Ready  ";
                  while (*st) b[p++] = *st++;
                  b[p] = 0;
                  ft(wx+2, yy++, b, vga_entry_color(VGA_CYAN, VGA_BLACK));
                  cur = cur->next;
              } while (cur && cur != t && yy < wy + w->h - 1);
          }
        } break;
    case 4: /* Help */
        ft(wx+2, wy+1, "    MuOS 7 Help", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        ft(wx+2, wy+2, "", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        ft(wx+2, wy+3, "TAB     Switch window", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        ft(wx+2, wy+4, "W/S     Minimize/Restore", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        ft(wx+2, wy+5, "M       Maximize window", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        ft(wx+2, wy+6, "Arrows  Move window", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        ft(wx+2, wy+7, "DEL     Close window", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        ft(wx+2, wy+8, "SPACE   Start menu", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        ft(wx+2, wy+9, "ESC     Exit desktop", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
        break;
    case 5: /* About / Logo */
        for (int i = 0; i < 5; i++)
            ft(wx+2, wy+1+i, muos_logo[i], vga_entry_color(VGA_WHITE, VGA_BLUE));
        ft(wx+2, wy+7, "MuOS 7 Ultimate Edition", vga_entry_color(VGA_LIGHT_CYAN, VGA_BLUE));
        ft(wx+2, wy+8, "x86 32-bit Microkernel", vga_entry_color(VGA_LIGHT_GREY, VGA_BLUE));
        ft(wx+2, wy+9, "Built: " __DATE__, vga_entry_color(VGA_LIGHT_GREY, VGA_BLUE));
        break;
    case 6: /* Calculator */
        ft(wx+2, wy+1, "=== Calculator ===", vga_entry_color(VGA_YELLOW, VGA_BLACK));
        ft(wx+2, wy+2, "Type 'calc' in terminal", vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        ft(wx+2, wy+3, "for interactive calc.", vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        ft(wx+2, wy+5, "Supports: + - * /", vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
        ft(wx+2, wy+6, "Example: calc 2+3", vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
        break;
    case 7: /* File Browser */
        ft(wx+2, wy+1, "=== File Browser ===", vga_entry_color(VGA_YELLOW, VGA_BLACK));
        { int n = fs_count();
          char b[8]; u32_to_dec(b, n);
          ft(wx+2, wy+2, "Files: ", vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
          ft(wx+9, wy+2, b, vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
          int yy = wy + 3;
          for (int i = 0; i < n && yy < wy + w->h - 1; i++) {
              const char* name = fs_name(i);
              uint32_t sz = fs_size(i);
              ft(wx+2, yy, name, vga_entry_color(VGA_CYAN, VGA_BLACK));
              u32_to_dec(b, sz);
              ft(wx+20, yy, b, vga_entry_color(VGA_DARK_GREY, VGA_BLACK));
              ft(wx+20+2, yy, "B", vga_entry_color(VGA_DARK_GREY, VGA_BLACK));
              yy++;
          }
          if (n == 0)
              ft(wx+2, yy, "(empty)", vga_entry_color(VGA_DARK_GREY, VGA_BLACK));
        } break;
    }
}

/* ═══════════════════ Start Menu ═══════════════════ */
static void draw_start_menu(void) {
    int sx = 0, sy = 16, sw = 24, sh = 8;
    uint8_t mc = vga_entry_color(VGA_BLACK, VGA_WHITE);
    uint8_t hc = vga_entry_color(VGA_WHITE, VGA_BLUE);
    /* Background */
    for (int y = sy; y < sy+sh; y++)
        fd(y, sx, sx+sw-1, mc, ' ');
    /* Borders */
    for (int y = sy; y < sy+sh; y++) {
        ((volatile uint16_t*)0xB8000)[y*80+sx] = vga_entry(0xB3, vga_entry_color(VGA_WHITE, VGA_BLUE));
        ((volatile uint16_t*)0xB8000)[y*80+sx+sw-1] = vga_entry(0xB3, vga_entry_color(VGA_WHITE, VGA_BLUE));
    }
    /* Header */
    fd(sy, sx+1, sx+sw-2, vga_entry_color(VGA_WHITE, VGA_BLUE), ' ');
    ft(sx+1, sy, "  MuOS 7  -  User", vga_entry_color(VGA_WHITE, VGA_BLUE));
    /* Menu items */
    const char* items[] = {
        "  > Terminal",
        "  > Text Editor",
        "  > Calculator",
        "  > File Browser",
        "  > System Info",
        "  > Shut Down"
    };
    for (int i = 0; i < 6; i++) {
        uint8_t c = (i == start_sel) ? hc : mc;
        fd(sy+1+i, sx+1, sx+sw-2, c, ' ');
        ft(sx+2, sy+1+i, items[i], c);
    }
    /* Separator */
    fd(sy+7, sx+1, sx+sw-2, mc, 0xC4);
}

/* ═══════════════════ Window Management ═══════════════════ */
static void add_win(int x, int y, int w, int h, int id, const char* t, uint8_t bc) {
    if (nwin >= MAX_WIN) return;
    wins[nwin].x = x; wins[nwin].y = y;
    wins[nwin].w = w; wins[nwin].h = h;
    wins[nwin].id = id; wins[nwin].t = t;
    wins[nwin].bc = bc; wins[nwin].open = 1;
    wins[nwin].maximized = 0;
    wins[nwin].old_x = x; wins[nwin].old_y = y;
    wins[nwin].old_w = w; wins[nwin].old_h = h;
    active = nwin;
    nwin++;
}

static void mv(Win* w, int dx, int dy) {
    if (w->maximized) return;
    w->x += dx; w->y += dy;
    if (w->x < 1) w->x = 1;
    if (w->y < 1) w->y = 1;
    if (w->x + w->w > 79) w->x = 79 - w->w;
    if (w->y + w->h > 22) w->y = 22 - w->h;
}

static void toggle_maximize(Win* w) {
    if (w->maximized) {
        w->x = w->old_x; w->y = w->old_y;
        w->w = w->old_w; w->h = w->old_h;
        w->maximized = 0;
    } else {
        w->old_x = w->x; w->old_y = w->y;
        w->old_w = w->w; w->old_h = w->h;
        w->x = 1; w->y = 1; w->w = 78; w->h = 21;
        w->maximized = 1;
    }
}

static void redraw_all(void) {
    draw_desktop();
    for (int i = 0; i < nwin; i++) {
        if (wins[i].open) {
            draw_win(&wins[i], i == active);
            draw_content(&wins[i]);
        }
    }
    draw_taskbar();
    if (start_menu) draw_start_menu();
}

/* ═══════════════════ Main Loop ═══════════════════ */
void win7_run(void) {
    keyboard_flush();
    nwin = 0; active = -1; start_menu = 0; start_sel = 0;

    /* Create windows — 3 open, rest minimized */
    add_win(2, 2, 36, 10, 1, "Terminal",      vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
    add_win(40, 2, 36, 10, 2, "Computer",     vga_entry_color(VGA_GREEN, VGA_BLACK));
    add_win(2, 13, 36, 8,  3, "Tasks",        vga_entry_color(VGA_CYAN, VGA_BLACK));
    add_win(40, 13, 36, 8, 4, "Help",         vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY));
    add_win(20, 2, 40, 10, 5, "About MuOS 7", vga_entry_color(VGA_WHITE, VGA_BLUE));
    add_win(20, 13, 36, 8, 6, "Calculator",   vga_entry_color(VGA_LIGHT_RED, VGA_BLACK));
    add_win(40, 2, 36, 8, 7, "Files",         vga_entry_color(VGA_YELLOW, VGA_BLACK));
    /* Minimize some */
    wins[3].open = 0;  /* Help */
    wins[4].open = 0;  /* About */
    wins[5].open = 0;  /* Calc */
    wins[6].open = 0;  /* Files */
    active = 0;
    redraw_all();

    while (1) {
        draw_taskbar();
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
                start_menu = 0;
                int targets[] = {0, 0, 5, 6, 1, -1};
                int tgt = targets[start_sel];
                if (tgt == -1) { vga_clear(); return; }
                if (tgt >= 0 && tgt < nwin) {
                    wins[tgt].open = 1;
                    active = tgt;
                }
                redraw_all();
                break;
            }
            case KEY_ESC: case ' ':
                start_menu = 0;
                redraw_all();
                break;
            }
            continue;
        }

        /* Move-accumulator: a long arrow-key hold would normally queue
         * 30+ redraws per second and lock the system. Drain same-direction
         * repeats and only redraw once. */
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

        /* Normal mode */
        switch ((unsigned char)c) {
        case KEY_ESC:
            vga_clear();
            return;
        case ' ':
            start_menu = 1;
            start_sel = 0;
            redraw_all();
            break;
        case '\t':
            if (nwin > 0) {
                int old = active;
                do {
                    active = (active + 1) % nwin;
                } while (!wins[active].open && active != old);
                redraw_all();
            }
            break;
        case 'w': case 'W':
            if (active >= 0 && wins[active].open) {
                wins[active].open = 0;
                for (int i = 0; i < nwin; i++)
                    if (wins[i].open) { active = i; break; }
                redraw_all();
            }
            break;
        case 's': case 'S': {
            int found = -1;
            for (int i = 0; i < nwin; i++)
                if (!wins[i].open) { found = i; break; }
            if (found >= 0) {
                wins[found].open = 1;
                active = found;
                redraw_all();
            }
        } break;
        case 'm': case 'M':
            if (active >= 0 && wins[active].open) {
                toggle_maximize(&wins[active]);
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
                redraw_all();
            }
            break;
        case KEY_F1: case KEY_F2: case KEY_F3:
        case KEY_F4: case KEY_F5: case KEY_F6:
        case KEY_F7: {
            int id = (unsigned char)c - KEY_F1;
            if (id >= 0 && id < nwin) {
                active = id;
                wins[id].open = 1;
                redraw_all();
            }
        } break;
        }
    }
}
