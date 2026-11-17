/* NONOXP (elf AI5WIN, 2003) engine core — portable C, no platform dependencies */
#ifndef AI5_H
#define AI5_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- platform layer (implemented per target: headless / 3DS) ---- */
void *plat_read_file(const char *name, size_t *len);   /* game dir file, malloc'd */
void  plat_log(const char *fmt, ...);
/* one frame: present the 640x480 RGB screen, pump input, sleep to ~60fps. returns false when the app should quit */
bool  plat_frame(const uint8_t *rgb, int w, int h);
void  plat_poll(void);     /* pump input only (called from tight script polling loops) */
void  plat_cursor(int *x, int *y, bool set);   /* pointer position in 640x480 space (mouse / touch); set=true warps it */
bool  plat_key(int key);   /* [EXE 0x409430] 0=confirm(Enter/LMB) 1=cancel(Esc/RMB) 2=up 3=down 4=left 5=right 6=skip(Shift) 7.. */

/* ---- archives ---- */
typedef struct { char name[21]; uint32_t off, size; } arc_entry;
typedef struct { char path[64]; int n; arc_entry *e; } arc_t;
bool  arc_open(arc_t *a, const char *path);
void *arc_load(arc_t *a, const char *name, size_t *len);          /* raw bytes */
void *arc_load_lzss(arc_t *a, const char *name, size_t *len);     /* LZSS-decoded */
size_t lzss_decode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap);
/* platform partial read (so archives are never loaded whole) */
bool plat_read_at(const char *name, uint32_t off, void *dst, uint32_t len);
uint32_t plat_file_size(const char *name);
int plat_list(const char *dir, char names[][24], int max);   /* names of the files in a game-folder subdirectory */

/* ---- VM memory layout (matches original offsets) ---- */
#define MEM_SIZE     0x200000
#define OFF_FLAG     0x0080      /* u8  flags[4096]            */
#define OFF_V16      0x1084      /* u16 var16[26]              */
#define OFF_SYS16    0x10B8      /* u16 sysvar16[]             */
#define OFF_V32      0x10EC      /* u32 var32[26]              */
#define OFF_SYS32    0x1154      /* u32 sysvar32[]             */
#define OFF_M32      0x2000      /* u32 m32[]                  */

typedef struct { const uint8_t *code; uint32_t ip; } vm_proc;   /* code = owning MES buffer */
#define MAX_PROCS 256
#define MAX_PARAMS 32
typedef struct { int type; uint32_t val; const char *str; } vm_param;  /* type 1=str 2=int */

typedef struct vm {
    uint8_t *mem;
    const uint8_t *mes; uint32_t mes_len;  /* current script buffer (decoded) */
    uint32_t base, ip;                     /* code start, ip relative to code start */
    const uint32_t *labels; uint32_t nlabels;
    vm_proc procs[MAX_PROCS], lib[MAX_PROCS];
    vm_proc procstack[16][MAX_PROCS]; int procsp;
    vm_param params[MAX_PARAMS]; int nparams;
    uint32_t msgid;
    char mes_name[32], goto_name[32]; bool want_goto;
    bool quit; uint64_t steps; int depth;   /* depth = nesting of run_block, like the original's +0x260 counter */
    arc_t arc_mes, arc_data, arc_bg, arc_music, arc_voice, arc_sound;   /* arc_sound = SOUND.ARC (compressed music/effects, optional) */
    uint8_t *libbuf;
    uint32_t menu_cur, menu_n[256];          /* [EXE] 1B selects+clears menu, MENUI appends an item */
    uint32_t menu_item[256][16], menu_id[256][16];
    const uint8_t *menu_mes[256]; uint32_t menu_run;   /* script buffer the items live in; menu being run */
} vm_t;
void vm_call_proc(vm_t *vm, uint32_t no);

static inline uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static inline uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void wr16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
#define FLAG(vm,i)  ((vm)->mem[OFF_FLAG + ((i) & 0xFFF)])
#define V16(vm,i)   rd16((vm)->mem + OFF_V16 + 2*(i))
#define SYS16(vm,i) rd16((vm)->mem + OFF_SYS16 + 2*(i))
#define V32(vm,i)   rd32((vm)->mem + OFF_V32 + 4*(i))
#define SYS32(vm,i) rd32((vm)->mem + OFF_SYS32 + 4*(i))
#define M32(vm,i)   rd32((vm)->mem + OFF_M32 + 4*(i))
#define SET_V16(vm,i,v)   wr16((vm)->mem + OFF_V16 + 2*(i), v)
#define SET_SYS16(vm,i,v) wr16((vm)->mem + OFF_SYS16 + 2*(i), v)
#define SET_V32(vm,i,v)   wr32((vm)->mem + OFF_V32 + 4*(i), v)
#define SET_SYS32(vm,i,v) wr32((vm)->mem + OFF_SYS32 + 4*(i), v)
#define SET_M32(vm,i,v)   wr32((vm)->mem + OFF_M32 + 4*(i), v)

bool vm_init(vm_t *vm);
bool vm_load_mes(vm_t *vm, const char *name);
void vm_run(vm_t *vm);                 /* run current script until END at depth 0 / quit */
uint32_t vm_expr(vm_t *vm);

/* ---- save files (FLAGnn in the original's SAVEDAT folder). Returns false if missing / failed. ---- */
bool plat_save_read(const char *name, void *buf, size_t len);
bool plat_save_write(const char *name, const void *buf, size_t len);
void plat_datetime(int out[6]);
uint32_t plat_ms(void);          /* millisecond clock (timeGetTime equivalent) */   /* year, month, day, hour, minute, second (local) */

/* ---- sound. slots: 0-1 music, 2-5 effects, 6-7 voice. data (malloc'd) passes to the platform, which frees it.
   data formats: "AI5S" header + Ogg Vorbis (SOUND.ARC), plain Ogg Vorbis (VOICE.ARC), or RIFF/WAVE PCM16 (MUSIC.ARC). ---- */
enum { SND_MUSIC = 0, SND_SE = 2, SND_VOICE = 6, SND_SLOTS = 8 };
void plat_snd_play(int slot, void *data, size_t len, int volume /* 0..255 */);
void plat_snd_stop(int slot);
void plat_snd_fade(int slot, int ms);          /* fade out, then stop */
bool plat_snd_playing(int slot);

/* ---- views for small-screen platforms (3DS): scene without the text windows, and the window text as strings ---- */
typedef struct {
    int nlines; struct { char sjis[132]; int item; } line[16];   /* item = choice index, or -1 for dialogue */
    bool dialogue_visible, menu_visible; int hover;               /* hover = highlighted choice, -1 if none */
    int loop;                                                     /* event loop (MENUS) being run: 100 = dialogue/choices, other = full-screen menus, -1 none */
    bool backlog;                                                 /* message log viewer is open */
} ui_text;
const uint8_t *sys_scene_rgb(void);
const uint8_t *sys_scene_depth(void);     /* 640x480 layer map for stereoscopic 3D (0 backdrop, 1 sprite), NULL = draw flat */         /* 640x480 RGB888 scene (surface 0 with shake applied), no text windows */
const uint8_t *sys_bottom_rgb(void);       /* 320x240 RGB the engine wants on the bottom screen (title menu, confirm dialogs), else NULL */
const uint8_t *sys_live_rgb(void);         /* the live 640x480 screen (shown on the bottom screen during full-screen menus) */
const ui_text *sys_ui_text(vm_t *vm);
const uint8_t *sys_font(uint32_t *cells);
int sys_font_flags(void);                  /* bit 0: Korean glyph remap font (full-width space drawn half width on the small screen) */   /* font16.bin glyph data: 128 bytes per 16x16 4bpp cell */
uint32_t sys_glyph_index(const uint8_t **p, int *half);   /* decode one SJIS char, advance *p */

/* hooks implemented in sys.c */
void sys_call(vm_t *vm, uint32_t fn);          /* SYS: params in vm->params */
void sys_util(vm_t *vm);                       /* UTIL */
void sys_text(vm_t *vm, const char *sjis, bool zenkaku);
void sys_menu_define(vm_t *vm, uint32_t body_ip);
void sys_menu_exec(vm_t *vm);
void sys_op1b(vm_t *vm);
void sys_line(vm_t *vm, int arg);
void sys_msgid(vm_t *vm, uint32_t id);
#endif
