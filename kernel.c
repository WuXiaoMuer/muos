/* kernel.c - 最小内核主程序
 * 作用：在 VGA 文本模式 (0xB8000) 上显示启动信息
 */

#include <stdint.h>

/* VGA 文本缓冲区位于物理地址 0xB8000 */
static volatile uint16_t* const VGA_BUFFER = (uint16_t*)0xB8000;

/* 默认配色：浅灰文字 (0x07)  on 黑色背景 (0x00) */
#define VGA_COLOR_DEFAULT 0x07

/* 当前光标位置 */
static uint16_t cursor_pos = 0;

/* 清屏 */
void clear_screen(void) {
    for (int i = 0; i < 80 * 25; i++) {
        VGA_BUFFER[i] = (VGA_COLOR_DEFAULT << 8) | ' ';
    }
    cursor_pos = 0;
}

/* 打印单个字符 */
void putchar(char c) {
    if (c == '\n') {
        cursor_pos = (cursor_pos / 80 + 1) * 80;
    } else {
        VGA_BUFFER[cursor_pos] = (VGA_COLOR_DEFAULT << 8) | (uint8_t)c;
        cursor_pos++;
    }
}

/* 打印字符串 */
void print(const char* str) {
    int i = 0;
    while (str[i]) {
        putchar(str[i]);
        i++;
    }
}

/* 内核入口函数 */
void kernel_main(void) {
    clear_screen();
    print("============================\n");
    print("      MuOS Booted!\n");
    print("  Hello from Windows dev!\n");
    print("============================\n");

    /* 阻塞，防止内核返回 */
    while (1) {
        __asm__ volatile ("hlt");
    }
}
