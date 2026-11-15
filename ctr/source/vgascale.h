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
/* The engine draws its message window as a separate strip below the picture (rows 353..439 of the 640x480 screen;
 * the picture itself is rows 40..335).  vgascale_window_up() is 1 while that strip's frame is on screen. */
int vgascale_window_up(Machine *m);
/* Picture only (no message strip), 2x2 box-filtered and scaled uniformly to the 400-pixel width of the top screen,
 * centred vertically with black bars. */
void vgascale_render_picture(Machine *m, uint16_t *fb);
#endif
