#ifndef VOICEPLAY_H
#define VOICEPLAY_H
/* Voice clip player (VOICE.ARC from the Windows version, DSP channel 1). */
int  vp_init(const char *gamedir);   /* number of clips in VOICE.ARC (0: no voice file) - call after ndspInit */
int  vp_play(const char *name);      /* start a clip by name (no extension); stops the previous one */
void vp_pump(void);                  /* once per frame: decode and queue the next pieces */
void vp_stop(void);
int  vp_playing(void);
int  vp_take_finished(void);         /* 1 once after a clip has played to its end (not when stopped) */
void vp_set_volume(float v);
#endif
