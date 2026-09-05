#include "mouse.h"
/*
 * TODO(ROADMAP): fake driver — no PS/2 controller init, no IRQ12.
 * mouse_get_state() reports a fixed center position with no buttons
 * so the desktop UI has data to show. A real PS/2 mouse driver is a
 * ROADMAP item; enabling it must not disturb 8042 keyboard timing.
 */
static volatile int16_t mx = 160, my = 100;
static volatile int8_t  btn_l, btn_r, btn_m;

void mouse_init(void){ mx=160;my=100;btn_l=btn_r=btn_m=0; }

mouse_state_t mouse_get_state(void){
    mouse_state_t s;
    s.x=mx;s.y=my;s.btn_left=btn_l;s.btn_right=btn_r;s.btn_middle=btn_m;
    return s;
}
