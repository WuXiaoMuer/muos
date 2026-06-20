#ifndef MUOS_KEYBOARD_H
#define MUOS_KEYBOARD_H
#include "types.h"

#define KEYBOARD_DATA_PORT  0x60
#define KEYBOARD_STATUS_PORT 0x64
#define KEYBOARD_BUFFER_SIZE 256

/* Special key codes */
#define KEY_ENTER       '\n'
#define KEY_BACKSPACE   '\b'
#define KEY_TAB         '\t'
#define KEY_ESC         0x1B

#define KEY_UP          0x80
#define KEY_DOWN        0x81
#define KEY_LEFT        0x82
#define KEY_RIGHT       0x83
#define KEY_HOME        0x84
#define KEY_END         0x85
#define KEY_PAGEUP      0x86
#define KEY_PAGEDOWN    0x87
#define KEY_DELETE      0x88
#define KEY_F1          0x90
#define KEY_F2          0x91
#define KEY_F3          0x92
#define KEY_F4          0x93
#define KEY_F5          0x94
#define KEY_F6          0x95
#define KEY_F7          0x96
#define KEY_F8          0x97
#define KEY_F9          0x98
#define KEY_F10         0x99
#define KEY_F11         0x9A
#define KEY_F12         0x9B

#define KEY_LSHIFT      0xA0
#define KEY_RSHIFT      0xA1
#define KEY_LCTRL       0xA2
#define KEY_RCTRL       0xA3
#define KEY_LALT        0xA4
#define KEY_RALT        0xA5
#define KEY_CAPSLOCK    0xA6

void keyboard_init(void);
char keyboard_getchar(void);        /* Blocking read */
char keyboard_peek(void);           /* Non-destructive peek, 0 if empty */
bool_t keyboard_haschar(void);        /* Check if buffer has data */
void keyboard_flush(void);          /* Clear input buffer */

#endif
