#ifndef MUOS_MOUSE_H
#define MUOS_MOUSE_H
#include "types.h"

#define MOUSE_IRQ   12
#define MOUSE_PORT  0x60
#define MOUSE_STATUS 0x64

/* Mouse packet */
typedef struct {
    int16_t x, y;       /* Position (0-639, 0-199 in text coords) */
    int8_t  btn_left;
    int8_t  btn_right;
    int8_t  btn_middle;
} mouse_state_t;

void mouse_init(void);
mouse_state_t mouse_get_state(void);
void mouse_wait_click(void);  /* Block until left click */

#endif
