#ifndef TEXTCAP_H
#define TEXTCAP_H
#include <stdint.h>
#include "machine.h"

/* Capture of the engine's glyph blits (text drawn on the VGA screen) so the dialogue
 * can be re-laid out in large type on a second screen. */
void textcap_init(Machine *m);                 /* install: hooks the program loader (INT 21h/4B01) */
void textcap_free(Machine *m);
uint32_t textcap_gen(Machine *m);              /* increments whenever the captured text changes */
int  textcap_active(Machine *m);               /* 1 if the engine's glyph routine was found and hooked */
int  textcap_unsettled(Machine *m);            /* 1 while some glyphs are hidden/new because VRAM was caught mid-redraw: render again soon */
/* Draw the captured text, large, into an RGB888 buffer (w*h*3 bytes). Returns number of glyphs drawn. */
int  textcap_render(Machine *m, uint8_t *rgb, int w, int h);
/* Largest magnification the bottom-screen layout may use (default 1.5; the engine's glyphs are 16 px). */
void textcap_set_max_scale(float s);
/* Touch on a choice menu: after textcap_render, 1 if the layout contains a highlighted line (a menu) and the bottom-screen y coordinate ty is on one of its
 * lines; then (*gx, *gy) is a point on the engine's 640x480 screen inside the first character of that line's logical line (where a mouse click selects it). */
int textcap_hit(Machine *m, int tx, int ty, int *gx, int *gy);
int textcap_menu_active(void);      /* 1 while a menu with a highlighted entry is shown on the bottom screen (as of the last textcap_render) */
#endif
