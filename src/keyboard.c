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
    /* Enter critical section so the ring-buffer indices cannot be torn
     * by a concurrent polling read on another path. */
    __asm__ volatile ("cli" ::: "memory");
    if (scancode == 0xE0) {
        extended = true;
        __asm__ volatile ("sti" ::: "memory");
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
        __asm__ volatile ("sti" ::: "memory");
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
    __asm__ volatile ("sti" ::: "memory");
}

/* IRQ1 handler — pre-empts polling only briefly.
 * Drains the 8042, drops mouse data, and lets the polled
 * keyboard_getchar() in shell pick up the byte. */
static void kbd_irq_handler(registers_t* regs) {
    (void)regs;
    /* Drain ALL pending bytes in the 8042 buffer. A mouse IRQ
     * can otherwise queue up a 3-byte packet that the next
     * poll-loop iteration misreads as keyboard scancodes. */
    while (inb(KEYBOARD_STATUS_PORT) & 0x01) {
        uint8_t s = inb(KEYBOARD_STATUS_PORT);
        uint8_t data = inb(KEYBOARD_DATA_PORT);
        if (!(s & 0x20)) {
            __asm__ volatile ("cli" ::: "memory");
            kbd_process_scancode(data);
            __asm__ volatile ("sti" ::: "memory");
        }
        /* Mouse byte — discard */
    }
    pic_send_eoi(1);
}

/* Minimal init: drain and leave 8042 alone */
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

    /* Register IRQ1 — the handler just drains the 8042 and
     * forwards keyboard scancodes; mouse data is discarded. */
    irq_register_handler(1, kbd_irq_handler);
    pic_unmask(1);
}

/* Pure polling read — no CLI/STI needed (single task, PIT doesn't preempt) */
char keyboard_getchar(void) {
    while (kbd_head == kbd_tail) {
        /* Enable interrupts, halt until any IRQ, then re-disable.
         * The 16-bit head/tail read after wake is atomic on x86. */
        __asm__ volatile ("sti; hlt; cli" ::: "memory");
        /* Polling fallback: drain any pending byte ourselves so
         * the loop has a chance to make progress even if IRQs are
         * misrouted. */
        uint8_t s = inb(KEYBOARD_STATUS_PORT);
        if (s & 0x01) {
            uint8_t data = inb(KEYBOARD_DATA_PORT);
            if (!(s & 0x20)) {
                kbd_process_scancode(data);
            }
        }
    }
    char c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KEYBOARD_BUFFER_SIZE;
    __asm__ volatile ("sti" ::: "memory");
    return c;
}

bool_t keyboard_haschar(void) {
    __asm__ volatile ("cli" ::: "memory");
    int r = (kbd_head != kbd_tail);
    __asm__ volatile ("sti" ::: "memory");
    return r;
}

char keyboard_peek(void) {
    __asm__ volatile ("cli" ::: "memory");
    char c = (kbd_head == kbd_tail) ? 0 : kbd_buffer[kbd_tail];
    __asm__ volatile ("sti" ::: "memory");
    return c;
}

void keyboard_flush(void) {
    __asm__ volatile ("cli" ::: "memory");
    kbd_head = 0;
    kbd_tail  = 0;
    __asm__ volatile ("sti" ::: "memory");
}
