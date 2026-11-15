#include "3ds.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
void stub_input(int scan, u32 *held, touchPosition *tp, circlePosition *cp) {
    *held = 0; memset(tp, 0, sizeof *tp); memset(cp, 0, sizeof *cp);
    if (getenv("STUB_MODE") && atoi(getenv("STUB_MODE")) == 3) return;
    if (getenv("STUB_MODE") && atoi(getenv("STUB_MODE")) == 5) { if (scan >= 850 && scan < 960) *held |= KEY_SELECT; return; }
    if (getenv("STUB_MODE") && atoi(getenv("STUB_MODE")) == 8) {      /* open menu, go to "Sound test", run it, resume */
        if (scan >= 880 && scan < 884) *held |= KEY_START;
        for (int i = 0; i < 9; i++) if (scan >= 900 + 4 * i && scan < 902 + 4 * i) *held |= KEY_DDOWN;
        if (scan >= 940 && scan < 942) *held |= KEY_A;
        if (scan >= 1000 && scan < 1003) *held |= KEY_START;
        return; }
    if (getenv("STUB_MODE") && atoi(getenv("STUB_MODE")) == 4) {      /* open menu, move to item 2, change it, dump while open */
        if (scan >= 880 && scan < 884) *held |= KEY_START;
        if (scan >= 920 && scan < 923) *held |= KEY_DDOWN;
        if (scan >= 940 && scan < 943) *held |= KEY_A;
        if (scan >= 960 && scan < 963) *held |= KEY_DDOWN;
        if (scan >= 980 && scan < 983) *held |= KEY_A;
        if (scan >= 1050 && scan < 1053) *held |= KEY_START;
        return; }
    if (getenv("STUB_MODE") && atoi(getenv("STUB_MODE")) == 7) {      /* like 1, but wiggle the menu cursor while the choice window of the scene at ~29560 is up */
        int lo = getenv("STUB_CHOICE_LO") ? atoi(getenv("STUB_CHOICE_LO")) : 29560, hi = getenv("STUB_CHOICE_HI") ? atoi(getenv("STUB_CHOICE_HI")) : 29705;
        if (scan >= lo && scan < hi) {
            int ph = (scan - lo) % 40;
            if (getenv("STUB_CHOICE_PAD")) { cp->dx = 0; cp->dy = ph < 20 ? -150 : 150; }      /* circle pad up/down = mouse over the choices */
            else { if (ph < 3) *held |= KEY_DDOWN; else if (ph >= 20 && ph < 23) *held |= KEY_DUP; }   /* D-pad = arrow keys */
            return; }
        if (scan >= 300 && scan % 150 < 4) *held |= KEY_X; return; }
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
