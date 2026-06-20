#include "mouse.h"
/* Mouse state — PS/2 init disabled to preserve 8042 keyboard stability */
static volatile int16_t mx = 160, my = 100;
static volatile int8_t  btn_l, btn_r, btn_m;

void mouse_init(void){ mx=160;my=100;btn_l=btn_r=btn_m=0; }

mouse_state_t mouse_get_state(void){
    mouse_state_t s;
    s.x=mx;s.y=my;s.btn_left=btn_l;s.btn_right=btn_r;s.btn_middle=btn_m;
    return s;
}

void mouse_wait_click(void){ while(!btn_l)__asm__("hlt"); while(btn_l)__asm__("hlt"); }
