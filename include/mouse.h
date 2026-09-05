#ifndef MUOS_MOUSE_H
#define MUOS_MOUSE_H
#include "types.h"

typedef struct {
    int16_t x, y;
    int8_t  btn_left, btn_right, btn_middle;
} mouse_state_t;

void mouse_init(void);
mouse_state_t mouse_get_state(void);

#endif
