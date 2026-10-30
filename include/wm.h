#ifndef MUOS_WM_H
#define MUOS_WM_H
#include "types.h"

/* Framebuffer window manager: per-window 32-bit backing buffers, a
 * compositor into a double-buffered screen image, PS/2 mouse dragging/
 * focus/close and a keyboard text area. Requires VBE (ISO boot path). */
void wm_run(void);

#endif
