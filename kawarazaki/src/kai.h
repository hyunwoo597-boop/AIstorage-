/* 河原崎家の一族 (elf, Windows "AI.exe") - portable engine. Platform interface + engine API. */
#ifndef KAI_H
#define KAI_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
/* ---- platform ---- */
bool  plat_read_at(const char *name, uint32_t off, void *dst, uint32_t len);   /* game folder file */
uint32_t plat_file_size(const char *name);
void  plat_log(const char *fmt, ...);
bool  plat_frame(const uint8_t *rgb, int w, int h);   /* present 640x480 RGB888; false = quit */
void  plat_poll(void);
bool  plat_key(int key);          /* 0 confirm 1 cancel 2 up 3 down 4 left 5 right 6 skip */
void  plat_cursor(int *x, int *y, bool set);
uint32_t plat_ms(void);
bool  plat_save_read(const char *name, void *buf, size_t len);
bool  plat_save_write(const char *name, const void *buf, size_t len);
enum { SND_BGM = 0, SND_SE = 1, SND_VOICE = 4, SND_SLOTS = 6 };   /* 0 bgm, 1-3 effects, 4 voice, 5 spare */
void  plat_snd_play(int slot, void *data, size_t len, int volume, bool loop);   /* WAV PCM or Ogg; platform frees data */
void  plat_snd_stop(int slot);
void  plat_snd_fade(int slot, int ms);
bool  plat_snd_playing(int slot);
/* ---- engine ---- */
bool  k_init(void);
void  k_run(const char *first_script);
const uint8_t *k_screen(void);       /* 640x480 RGB888 being shown */
#endif
