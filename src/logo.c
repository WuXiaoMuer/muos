/* logo.c - 3D text-mode wordmark and the boot/`logo` screen.
 *
 * The face glyphs are drawn in a vertical cyan gradient and repeated two
 * columns right / one row down in dark grey, which reads as an extruded
 * block. Only ASCII is used so the VGA text path and the VBE framebuffer
 * renderer (font8x8, 0x20..0x7E) show the same thing. */
#include "logo.h"
#include "vga.h"
#include "string.h"

const char* const muos_logo[LOGO_ROWS] = {
    "#     # #   #   #####   ##### ",
    "##   ## #   #  #     # #      ",
    "# # # # #   #  #     #  ##### ",
    "#  #  # #   #  #     #       #",
    "#     #  ###    #####   ##### "
};

/* Panel geometry (80x25 cell grid). */
#define PANEL_X 18
#define PANEL_Y 6
#define PANEL_W 44
#define PANEL_H 12
#define LOGO_X  24
#define LOGO_Y  7

static const enum vga_color logo_grad[LOGO_ROWS] = {
    VGA_WHITE, VGA_LIGHT_CYAN, VGA_LIGHT_CYAN, VGA_CYAN, VGA_LIGHT_BLUE
};

static void put_cell(int row, int col, char ch, enum vga_color fg) {
    if (row < 0 || row >= VGA_HEIGHT || col < 0 || col >= VGA_WIDTH) return;
    vga_put_cell((uint16_t)row, (uint16_t)col,
                 vga_entry((unsigned char)ch, vga_entry_color(fg, VGA_BLACK)));
}

static void text_at(int row, int col, const char* s, enum vga_color fg) {
    for (int i = 0; s[i]; i++) put_cell(row, col + i, s[i], fg);
}

static void text_centered(int row, const char* s, enum vga_color fg) {
    text_at(row, (VGA_WIDTH - (int)strlen(s)) / 2, s, fg);
}

void logo_print_3d(int row, int col) {
    /* Shadow first, then the face on top, so overlaps resolve correctly. */
    for (int i = 0; i < LOGO_ROWS; i++) {
        for (int j = 0; muos_logo[i][j]; j++) {
            if (muos_logo[i][j] != ' ')
                put_cell(row + 1 + i, col + 2 + j, '#', VGA_DARK_GREY);
        }
    }
    for (int i = 0; i < LOGO_ROWS; i++) {
        for (int j = 0; muos_logo[i][j]; j++) {
            if (muos_logo[i][j] != ' ')
                put_cell(row + i, col + j, muos_logo[i][j], logo_grad[i]);
        }
    }
}

void logo_show(void) {
    vga_clear();

    /* Frame: cyan top/bottom, blue sides, '+' corners. */
    for (int c = PANEL_X; c < PANEL_X + PANEL_W; c++) {
        put_cell(PANEL_Y, c, '-', VGA_LIGHT_CYAN);
        put_cell(PANEL_Y + PANEL_H - 1, c, '-', VGA_LIGHT_CYAN);
    }
    for (int r = PANEL_Y + 1; r < PANEL_Y + PANEL_H - 1; r++) {
        put_cell(r, PANEL_X, '|', VGA_LIGHT_BLUE);
        put_cell(r, PANEL_X + PANEL_W - 1, '|', VGA_LIGHT_BLUE);
    }
    put_cell(PANEL_Y, PANEL_X, '+', VGA_LIGHT_CYAN);
    put_cell(PANEL_Y, PANEL_X + PANEL_W - 1, '+', VGA_LIGHT_CYAN);
    put_cell(PANEL_Y + PANEL_H - 1, PANEL_X, '+', VGA_LIGHT_CYAN);
    put_cell(PANEL_Y + PANEL_H - 1, PANEL_X + PANEL_W - 1, '+', VGA_LIGHT_CYAN);

    logo_print_3d(LOGO_Y, LOGO_X);

    text_centered(13, "MuOS " MUOS_VERSION "  -  x86 32-bit Microkernel", VGA_YELLOW);
    text_centered(14, "Ring3 + Linux i386 ABI", VGA_LIGHT_CYAN);
    text_centered(19, "Type 'help' for commands, 'desktop' for the GUI.", VGA_LIGHT_GREY);
    text_centered(20, "Tab completes commands  |  help <cmd> shows details", VGA_DARK_GREY);

    vga_setcolor(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
    vga_set_cursor(21, 0);
}
