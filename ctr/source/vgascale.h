#ifndef VGASCALE_H
#define VGASCALE_H
#include <stdint.h>
#include "../../core/machine.h"

/* Render the emulated 640x480x16 planar VGA screen, box-filtered down by exactly 2x2 (-> 320x240),
 * straight into a 3DS RGB565 framebuffer (column-major, 240 pixels per column, y flipped):
 * pixel (x,y) of the 320x240 picture is stored at fb[(x0 + x) * 240 + (239 - y)]. */
void vgascale_render(Machine *m, uint16_t *fb, int x0);
/* Zoomed view: the 640x400 game area stretched over the whole 400x240 top screen (fb = column-major 400x240 RGB565). */
void vgascale_render_zoom(Machine *m, uint16_t *fb);
#endif
