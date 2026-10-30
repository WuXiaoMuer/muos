#ifndef MUOS_MOUSE_H
#define MUOS_MOUSE_H
#include "types.h"

typedef struct {
    int16_t x, y;          /* text cell coordinates: 0..79, 0..24 */
    int8_t  btn_left, btn_right, btn_middle;
} mouse_state_t;

/* Configures the 8042 auxiliary port and enables PS/2 mouse reporting.
 * IRQ routing lives in keyboard.c (shared 8042 drain). */
bool_t mouse_init(void);

mouse_state_t mouse_get_state(void);
/* Raw 640x400 virtual-pixel position (used by the graphical UI). */
void mouse_get_pixels(int* x, int* y);

/* Called from the shared 8042 IRQ/poll path for auxiliary-port bytes. */
void mouse_handle_byte(uint8_t data);

#endif
