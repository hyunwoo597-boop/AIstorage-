#include "3ds.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
void stub_input(int scan, u32 *held, touchPosition *tp, circlePosition *cp) {
    *held = 0; memset(tp, 0, sizeof *tp); memset(cp, 0, sizeof *cp);
    if (getenv("STUB_MODE") && atoi(getenv("STUB_MODE")) == 3) return;
    if (getenv("STUB_MODE") && atoi(getenv("STUB_MODE")) == 5) { if (scan >= 850 && scan < 960) *held |= KEY_SELECT; return; }
    if (getenv("STUB_MODE") && atoi(getenv("STUB_MODE")) == 4) {      /* open menu, move to item 2, change it, dump while open */
        if (scan >= 880 && scan < 884) *held |= KEY_START;
        if (scan >= 920 && scan < 923) *held |= KEY_DDOWN;
        if (scan >= 940 && scan < 943) *held |= KEY_A;
        if (scan >= 960 && scan < 963) *held |= KEY_DDOWN;
        if (scan >= 980 && scan < 983) *held |= KEY_A;
        if (scan >= 1050 && scan < 1053) *held |= KEY_START;
        return; }
    if (getenv("STUB_MODE")) { if (scan >= 300 && scan % 150 < 4) *held |= KEY_X; return; }
    if (scan >= 100 && scan < 110) cp->dx = 140, cp->dy = -60;            /* circle pad: right/down */
    if (scan >= 400 && scan < 404) *held |= KEY_A;                       /* click */
    if (scan >= 500 && scan < 506) { *held |= KEY_TOUCH; tp->px = 100 + (scan - 500) * 5; tp->py = 100; }  /* drag */
    if (scan >= 600 && scan < 603) { *held |= KEY_TOUCH; tp->px = 150; tp->py = 150; }                      /* tap */
    if (scan >= 700 && scan < 703) *held |= KEY_START;                    /* open menu */
    if (scan == 720 || scan == 722) *held |= KEY_DDOWN;                   /* move selection */
    if (scan >= 730 && scan < 733) *held |= KEY_A;                        /* change CPU mode */
    if (scan >= 750 && scan < 753) *held |= KEY_START;                    /* resume */
}
