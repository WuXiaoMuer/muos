#include "keyboard.h"
#include "io.h"
#include "isr.h"
#include "irq.h"
#include "pic.h"

/* Keyboard state tracking */
static bool_t lshift   = false;
static bool_t rshift   = false;
static bool_t lctrl    = false;
static bool_t rctrl    = false;
static bool_t lalt     = false;
static bool_t ralt     = false;
static bool_t capslock = false;
static bool_t extended = false;

/* Circular input buffer */
static volatile char    kbd_buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint16_t kbd_head = 0;
static volatile uint16_t kbd_tail = 0;

/* Scancode set 1 → ASCII (unshifted) */
static const char kbd_lower[128] = {
    0,    0x1B, '1',  '2',  '3',  '4',  '5',  '6',
    '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',
    'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',
    'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',
    'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',
    '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',
    'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',
    0,    ' ',  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0
};

/* Scancode set 1 → ASCII (shifted) */
static const char kbd_upper[128] = {
    0,    0x1B, '!',  '@',  '#',  '$',  '%',  '^',
    '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t',
    'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',
    'O',  'P',  '{',  '}',  '\n', 0,    'A',  'S',
    'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',
    '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',
    'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',
    0,    ' ',  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0
};

static void kbd_buf_put(char c) {
    uint16_t next = (kbd_head + 1) % KEYBOARD_BUFFER_SIZE;
    if (next != kbd_tail) {
        kbd_buffer[kbd_head] = c;
        __asm__ volatile ("" ::: "memory");
        kbd_head = next;
    }
}

/* Process a single scancode byte. Called from both IRQ and polling path. */
static void kbd_process_scancode(uint8_t scancode) {
    if (scancode == 0xE0) {
        extended = true;
        return;
    }

    if (scancode & 0x80) {
        uint8_t key = scancode & 0x7F;
        switch (key) {
            case 0x2A: lshift = false; break;
            case 0x36: rshift = false; break;
            case 0x1D: if (extended) rctrl = false; else lctrl = false; break;
            case 0x38: if (extended) ralt  = false; else lalt  = false; break;
        }
        extended = false;
        return;
    }

    switch (scancode) {
        case 0x2A: lshift = true;  break;
        case 0x36: rshift = true;  break;
        case 0x1D: if (extended) rctrl = true; else lctrl = true; break;
        case 0x38: if (extended) ralt  = true; else lalt  = true; break;
        case 0x3A: capslock = !capslock; break;

        case 0x48: if (extended) kbd_buf_put(KEY_UP);    break;
        case 0x50: if (extended) kbd_buf_put(KEY_DOWN);  break;
        case 0x4B: if (extended) kbd_buf_put(KEY_LEFT);  break;
        case 0x4D: if (extended) kbd_buf_put(KEY_RIGHT); break;
        case 0x47: if (extended) kbd_buf_put(KEY_HOME);     break;
        case 0x4F: if (extended) kbd_buf_put(KEY_END);      break;
        case 0x49: if (extended) kbd_buf_put(KEY_PAGEUP);   break;
        case 0x51: if (extended) kbd_buf_put(KEY_PAGEDOWN); break;
        case 0x53: if (extended) kbd_buf_put(KEY_DELETE);   break;

        case 0x3B: kbd_buf_put(KEY_F1);  break;
        case 0x3C: kbd_buf_put(KEY_F2);  break;
        case 0x3D: kbd_buf_put(KEY_F3);  break;
        case 0x3E: kbd_buf_put(KEY_F4);  break;
        case 0x3F: kbd_buf_put(KEY_F5);  break;
        case 0x40: kbd_buf_put(KEY_F6);  break;
        case 0x41: kbd_buf_put(KEY_F7);  break;
        case 0x42: kbd_buf_put(KEY_F8);  break;
        case 0x43: kbd_buf_put(KEY_F9);  break;
        case 0x44: kbd_buf_put(KEY_F10); break;
        case 0x57: kbd_buf_put(KEY_F11); break;
        case 0x58: kbd_buf_put(KEY_F12); break;

        default: {
            bool_t shifted = lshift || rshift;
            if (capslock) {
                char lower = kbd_lower[scancode];
                if (lower >= 'a' && lower <= 'z') shifted = !shifted;
            }
            char c = shifted ? kbd_upper[scancode] : kbd_lower[scancode];
            if (c != 0) kbd_buf_put(c);
            break;
        }
    }
    extended = false;
}

/* IRQ1 handler */
static void kbd_irq_handler(registers_t* regs) {
    (void)regs;
    uint8_t status = inb(KEYBOARD_STATUS_PORT);
    if (!(status & 0x01)) return;
    kbd_process_scancode(inb(KEYBOARD_DATA_PORT));
}

/* Minimal init: BIOS already set up the 8042. Just drain and register IRQ. */
void keyboard_init(void) {
    lshift = false; rshift = false;
    lctrl  = false; rctrl  = false;
    lalt   = false; ralt   = false;
    capslock = false;
    extended = false;
    kbd_head = 0;
    kbd_tail  = 0;

    /* Drain any stale data */
    while (inb(KEYBOARD_STATUS_PORT) & 0x01) {
        inb(KEYBOARD_DATA_PORT);
    }

    /* Register IRQ1 handler (unmasks IRQ1 on PIC) */
    irq_register_handler(1, kbd_irq_handler);
}

/* Blocking read — IRQ-driven with CLI-protected polling fallback */
char keyboard_getchar(void) {
    while (kbd_head == kbd_tail) {
        __asm__ volatile ("hlt");
        /* Poll as fallback (CLI protects against IRQ handler race) */
        if (kbd_head == kbd_tail) {
            uint8_t s = inb(KEYBOARD_STATUS_PORT);
            if (s & 0x01) {
                __asm__ volatile ("cli");
                kbd_process_scancode(inb(KEYBOARD_DATA_PORT));
                __asm__ volatile ("sti");
            }
        }
    }
    __asm__ volatile ("" ::: "memory");
    char c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KEYBOARD_BUFFER_SIZE;
    return c;
}

bool_t keyboard_haschar(void) {
    return (kbd_head != kbd_tail);
}

void keyboard_flush(void) {
    kbd_head = 0;
    kbd_tail  = 0;
}
