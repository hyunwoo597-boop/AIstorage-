#ifndef VOICE_H
#define VOICE_H
#include "machine.h"
/* Voice clips for the Korean DOS release (see voice.c). */
typedef struct Voice Voice;
int  voice_init(Machine *m);          /* after machine_new: 1 if SAGWA.DAT's scene directory was found (voice hooks installed) */
void voice_free(Machine *m);
const char *voice_take(Machine *m);   /* name of a clip to start now (e.g. "S01_01", no extension), or NULL; each request is returned once */
unsigned voice_fired(Machine *m);     /* number of requests so far (diagnostics) */
#endif
