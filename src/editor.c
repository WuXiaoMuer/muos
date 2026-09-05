/* editor.c - MuOS Text Editor */
#include "editor.h"
#include "vga.h"
#include "keyboard.h"
#include "fs.h"
#include "string.h"

#define EDIT_COLS 79
#define EDIT_ROWS 22

static char lines[128][EDIT_COLS+1];  /* 128 lines × 79 chars each */
static int  line_count, cursor_x, cursor_y, scroll_top;
static char filename[32];
static int  modified;

static void drow(int y, const char* s, uint8_t c) {
    int i;
    for(i = 0; s[i]; i++) ((volatile uint16_t*)0xB8000)[y*80+i] = vga_entry((unsigned char)s[i], c);
    for(; i < 79; i++) ((volatile uint16_t*)0xB8000)[y*80+i] = vga_entry(' ', c);
}

static void draw_status(void) {
    uint8_t cs = vga_entry_color(VGA_BLACK, VGA_LIGHT_GREEN);
    char buf[80]; int p = 0;
    const char* ns = filename[0] ? filename : "[New File]";
    for(const char* s = ns; *s; s++) buf[p++] = *s;
    buf[p++] = ' '; buf[p++] = modified ? '*' : ' ';
    buf[p++] = ' '; buf[p++] = 'L'; buf[p++] = ':';
    { int n = cursor_y+1; if(!n)buf[p++]='0'; else{int d[4],c=0;while(n){d[c++]=n%10;n/=10;}while(c)buf[p++]='0'+d[--c];} }
    buf[p++] = ' '; buf[p++] = 'C'; buf[p++] = ':';
    { int n = cursor_x+1; if(!n)buf[p++]='0'; else{int d[4],c=0;while(n){d[c++]=n%10;n/=10;}while(c)buf[p++]='0'+d[--c];} }
    buf[p++] = ' '; buf[p] = 0;
    for(int i = 0; i < 79; i++) {
        if(i < p) ((volatile uint16_t*)0xB8000)[i] = vga_entry((unsigned char)buf[i], cs);
        else ((volatile uint16_t*)0xB8000)[i] = vga_entry(' ', cs);
    }
}

static void draw_help(void) {
    uint8_t ch = vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY);
    const char* s = "^S:Save  ^R:Reload  ESC:Exit  Arrows:Move  Tab:Indent";
    for(int i = 0; i < 79; i++) {
        if(s[i]) ((volatile uint16_t*)0xB8000)[23*80+i] = vga_entry((unsigned char)s[i], ch);
        else ((volatile uint16_t*)0xB8000)[23*80+i] = vga_entry(' ', ch);
    }
}

static void draw_content(void) {
    for(int y = 0; y < EDIT_ROWS; y++) {
        int ln = scroll_top + y;
        if(ln < line_count) drow(y+1, lines[ln], vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        else drow(y+1, "", vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
    }
}

static void draw_cursor(void) {
    int vrow = cursor_y - scroll_top + 1;
    if(vrow < 1 || vrow > EDIT_ROWS) return;
    /* Highlight cursor position */
    uint16_t* vm = (uint16_t*)0xB8000;
    int idx = vrow*80 + cursor_x;
    uint16_t orig = vm[idx];
    uint8_t ch = orig & 0xFF;
    uint8_t fg = (orig >> 8) ^ 0x77;
    if(!ch) ch = ' ';
    vm[idx] = vga_entry(ch, fg);
}

static void scroll_to_cursor(void) {
    if(cursor_y < scroll_top) scroll_top = cursor_y;
    if(cursor_y >= scroll_top + EDIT_ROWS) scroll_top = cursor_y - EDIT_ROWS + 1;
    if(scroll_top < 0) scroll_top = 0;
}

static void ins_at_cursor(char c) {
    char* ln = lines[cursor_y];
    int len = (int)strlen(ln);
    if(len >= EDIT_COLS) return;
    for(int i = len; i > cursor_x; i--) ln[i] = ln[i-1];
    ln[cursor_x] = c; ln[len+1] = '\0';
    cursor_x++; modified = 1;
}

static void del_at_cursor(void) {
    char* ln = lines[cursor_y];
    int len = (int)strlen(ln);
    if(cursor_x >= len) return;
    for(int i = cursor_x; i < len; i++) ln[i] = ln[i+1];
    modified = 1;
}

static void bksp_at_cursor(void) {
    if(cursor_x > 0) { cursor_x--; del_at_cursor(); }
    else if(cursor_y > 0) {
        /* Join with previous line */
        char* prev = lines[cursor_y-1];
        char* cur  = lines[cursor_y];
        int plen = (int)strlen(prev);
        int clen = (int)strlen(cur);
        if(plen + clen <= EDIT_COLS) {
            for(int i = 0; i <= clen; i++) prev[plen+i] = cur[i];
            /* Shift lines up */
            for(int i = cursor_y; i < line_count-1; i++) {
                char* dst = lines[i]; char* src = lines[i+1];
                int j = 0; while(src[j]) { dst[j] = src[j]; j++; } dst[j] = '\0';
            }
            line_count--;
            cursor_y--; cursor_x = plen;
            modified = 1;
        }
    }
}

static void save_file(void) {
    if(!filename[0]) return;
    int fd = fs_open(filename);
    if(fd < 0) fd = fs_create(filename);
    if(fd < 0) return;
    /* Build buffer (silently truncated at 1023 bytes; fs cap is 1KB) */
    char buf[1024]; int bp = 0;
    for(int i = 0; i < line_count && bp < 1023; i++) {
        for(char* p = lines[i]; *p && bp < 1023; p++) buf[bp++] = *p;
        if(bp < 1023) buf[bp++] = '\n';
    }
    buf[bp] = 0;
    fs_write(fd, buf, bp);
    modified = 0;
}

static void load_file(const char* fn) {
    int fd = fs_open(fn);
    if(fd < 0) return;
    char buf[1024];
    int len = fs_read(fd, buf, 1023);
    buf[len] = '\0';

    /* Parse into lines */
    line_count = 0; cursor_x = 0; cursor_y = 0;
    int lx = 0;
    for(int i = 0; i < len && line_count < 128; i++) {
        if(buf[i] == '\n') {
            lines[line_count][lx] = '\0';
            line_count++;
            lx = 0;
        } else if(lx < EDIT_COLS) {
            lines[line_count][lx++] = buf[i];
        }
    }
    if(lx > 0 && line_count < 128) {
        lines[line_count][lx] = '\0';
        line_count++;
    }
    if(line_count == 0) {
        line_count = 1;
        lines[0][0] = '\0';
    }
}

void editor_run(const char* fn) {
    /* Init lines */
    for(int i = 0; i < 128; i++) lines[i][0] = '\0';
    line_count = 1; cursor_x = 0; cursor_y = 0; scroll_top = 0; modified = 0;
    filename[0] = '\0';
    if(fn && fn[0]) {
        int i;
        for(i = 0; fn[i] && i < 31; i++) filename[i] = fn[i];
        filename[i] = '\0';
        load_file(fn);
    }

    vga_clear();
    draw_status();
    draw_help();
    draw_content();

    for(;;) {
        scroll_to_cursor();
        draw_content();
        draw_cursor();

        char c = keyboard_getchar();
        /* Erase cursor before processing */
        if(cursor_y - scroll_top + 1 >= 1 && cursor_y - scroll_top + 1 <= EDIT_ROWS) {
            int idx = (cursor_y-scroll_top+1)*80 + cursor_x;
            char ch = lines[cursor_y][cursor_x];
            if(!ch) ch = ' ';
            ((volatile uint16_t*)0xB8000)[idx] = vga_entry(ch, vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        }

        if(c == 0x13) { /* Ctrl+S */ save_file(); draw_status(); continue; }
        if(c == 0x12) { /* Ctrl+R: reload from disk (needs a filename) */
            if(filename[0]) load_file(filename);
            draw_content(); draw_status(); continue; }

        switch((unsigned char)c) {
        case KEY_ESC: vga_clear(); return;
        case '\n': case '\r':
            if(line_count < 128) {
                /* Split line at cursor */
                char* ln = lines[cursor_y];
                int len = 0; while(ln[len]) len++;
                for(int i = line_count; i > cursor_y+1; i--) {
                    char* dst = lines[i]; char* src = lines[i-1];
                    int j = 0; while(src[j]){dst[j]=src[j];j++;}dst[j]='\0';
                }
                int rem = len - cursor_x;
                for(int i = 0; i <= rem; i++) lines[cursor_y+1][i] = ln[cursor_x+i];
                ln[cursor_x] = '\0';
                line_count++; cursor_y++; cursor_x = 0;
                modified = 1;
            }
            break;
        case '\b': bksp_at_cursor(); break;
        case KEY_UP:    if(cursor_y > 0) { cursor_y--; int ln = 0; while(lines[cursor_y][ln]) ln++; if(cursor_x > ln) cursor_x = ln; } break;
        case KEY_DOWN:  if(cursor_y < line_count-1) { cursor_y++; int ln = 0; while(lines[cursor_y][ln]) ln++; if(cursor_x > ln) cursor_x = ln; } break;
        case KEY_LEFT:  if(cursor_x > 0) cursor_x--; else if(cursor_y > 0) { cursor_y--; int ln = 0; while(lines[cursor_y][ln]) ln++; cursor_x = ln; } break;
        case KEY_RIGHT: if(lines[cursor_y][cursor_x]) cursor_x++; else if(cursor_y < line_count-1) { cursor_y++; cursor_x = 0; } break;
        case KEY_HOME: cursor_x = 0; break;
        case KEY_END:  { int ln = 0; while(lines[cursor_y][ln]) ln++; cursor_x = ln; } break;
        case KEY_DELETE: del_at_cursor(); break;
        case '\t': for(int i = 0; i < 4; i++) ins_at_cursor(' '); break;
        default: if(c >= ' ' && c < 127) ins_at_cursor(c); break;
        }
        draw_status();
    }
}
