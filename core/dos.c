/* Minimal DOS / BIOS / EMS / mouse high-level emulation. */
#include "machine.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#define STUB_BASE 0x0100
#define IRET_STUB 0x00FF
#define LOL_OFF   0x0300
#define EMS_PAGES 128
#define FRAME_SEG 0xE000
#define XMS_STUB  0x0090
#define XMS_KB    3072

extern void vga_palette16(Machine *m, uint8_t rgb[16][3]);

static void logf_(Machine *m, const char *fmt, ...) { (void)m; (void)fmt; }

/* ------------------------------------------------------------ helpers */
static inline CPU *C(Machine *m) { return &m->cpu; }
static inline uint16_t rd16(Machine *m, uint16_t seg, uint16_t off) { return mem_rw(m, LIN(seg, off)); }
static inline void wr16(Machine *m, uint16_t seg, uint16_t off, uint16_t v) { mem_ww(m, LIN(seg, off), v); }
static inline uint8_t rd8(Machine *m, uint16_t seg, uint16_t off) { return mem_rb(m, LIN(seg, off)); }
static inline void wr8(Machine *m, uint16_t seg, uint16_t off, uint8_t v) { mem_wb(m, LIN(seg, off), v); }

static void read_asciiz(Machine *m, uint16_t seg, uint16_t off, char *out, int max) {
    int i = 0; for (; i < max - 1; i++) { uint8_t b = rd8(m, seg, off + i); if (!b) break; out[i] = (char)b; } out[i] = 0;
}

/* finish an HLE'd interrupt: IRET, optionally forcing CF (cf<0: leave) */
static void hret(Machine *m, int cf) {
    CPU *c = C(m); cpu_iret(c); if (cf >= 0) c->cf = cf;
}
static void dos_err(Machine *m, int code) { C(m)->r[AX] = code; hret(m, 1); }

/* --------------------------------------------------------------- paths */
static int ci_lookup(const char *dir, const char *name, char *out, size_t outsz) {
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *e; int found = 0;
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, name)) { snprintf(out, outsz, "%s", e->d_name); found = 1; break; }
    }
    closedir(d); return found;
}
/* resolve DOS path to host path (case-insensitive); returns 1 if it exists, 0 otherwise (host path still filled for creation) */
static int resolve_path(Machine *m, const char *dos, char *host, size_t hs) {
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s", dos);
    char *p = tmp;
    if (p[0] && p[1] == ':') p += 2;
    for (char *q = p; *q; q++) if (*q == '\\') *q = '/';
    snprintf(host, hs, "%s", m->gamedir);
    int exists = 1;
    char *save = NULL; char *tok = strtok_r(p, "/", &save);
    while (tok) {
        if (!strcmp(tok, ".")) { tok = strtok_r(NULL, "/", &save); continue; }
        if (!strcmp(tok, "..")) { tok = strtok_r(NULL, "/", &save); continue; }
        char real[260];
        if (exists && ci_lookup(host, tok, real, sizeof real)) snprintf(host + strlen(host), hs - strlen(host), "/%s", real);
        else { exists = 0; snprintf(host + strlen(host), hs - strlen(host), "/%s", tok); }
        tok = strtok_r(NULL, "/", &save);
    }
    return exists;
}

/* ----------------------------------------------------------- MCB heap */
#define MCB_TYPE(s) rd8(m, s, 0)
#define MCB_OWNER(s) rd16(m, s, 1)
#define MCB_SIZE(s) rd16(m, s, 3)
static void mcb_set(Machine *m, uint16_t s, char t, uint16_t owner, uint16_t size) { wr8(m, s, 0, t); wr16(m, s, 1, owner); wr16(m, s, 3, size); }
static void mcb_merge(Machine *m) {
    uint16_t s = m->mcb_first;
    while (MCB_TYPE(s) == 'M') {
        uint16_t n = s + MCB_SIZE(s) + 1;
        if (MCB_OWNER(s) == 0 && MCB_OWNER(n) == 0) { mcb_set(m, s, MCB_TYPE(n), 0, MCB_SIZE(s) + MCB_SIZE(n) + 1); continue; }
        s = n;
    }
}
static uint16_t mcb_largest(Machine *m) {
    uint16_t s = m->mcb_first, best = 0;
    for (;;) { if (MCB_OWNER(s) == 0 && MCB_SIZE(s) > best) best = MCB_SIZE(s); if (MCB_TYPE(s) == 'Z') break; s += MCB_SIZE(s) + 1; }
    return best;
}
/* returns segment of data block, 0 on failure */
static uint16_t mcb_alloc(Machine *m, uint16_t paras, uint16_t owner) {
    uint16_t s = m->mcb_first;
    for (;;) {
        if (MCB_OWNER(s) == 0 && MCB_SIZE(s) >= paras) {
            uint16_t sz = MCB_SIZE(s); char t = MCB_TYPE(s);
            if (sz > paras + 1) {
                mcb_set(m, s, 'M', owner, paras);
                mcb_set(m, s + paras + 1, t, 0, sz - paras - 1);
            } else mcb_set(m, s, t, owner, sz);
            return s + 1;
        }
        if (MCB_TYPE(s) == 'Z') break;
        s += MCB_SIZE(s) + 1;
    }
    return 0;
}
static void mcb_free(Machine *m, uint16_t seg) { wr16(m, seg - 1, 1, 0); mcb_merge(m); }
static int mcb_resize(Machine *m, uint16_t seg, uint16_t paras, uint16_t *maxp) {
    uint16_t s = seg - 1; uint16_t sz = MCB_SIZE(s);
    if (paras <= sz) {
        if (sz > paras + 1) { char t = MCB_TYPE(s); uint16_t o = MCB_OWNER(s); mcb_set(m, s, 'M', o, paras); mcb_set(m, s + paras + 1, t, 0, sz - paras - 1); mcb_merge(m); }
        return 1;
    }
    if (MCB_TYPE(s) == 'M') {
        uint16_t n = s + sz + 1;
        if (MCB_OWNER(n) == 0 && sz + 1 + MCB_SIZE(n) >= paras) {
            uint16_t total = sz + 1 + MCB_SIZE(n); char t = MCB_TYPE(n); uint16_t o = MCB_OWNER(s);
            if (total > paras + 1) { mcb_set(m, s, 'M', o, paras); mcb_set(m, s + paras + 1, t, 0, total - paras - 1); }
            else mcb_set(m, s, t, o, total);
            return 1;
        }
        if (maxp) *maxp = (MCB_OWNER(n) == 0) ? sz + 1 + MCB_SIZE(n) : sz;
    } else if (maxp) *maxp = sz;
    return 0;
}
static void mcb_free_owner(Machine *m, uint16_t owner) {
    uint16_t s = m->mcb_first;
    for (;;) { if (MCB_OWNER(s) == owner) wr16(m, s, 1, 0); if (MCB_TYPE(s) == 'Z') break; s += MCB_SIZE(s) + 1; }
    mcb_merge(m);
}

/* ------------------------------------------------------------- init */
void dos_init(Machine *m) {
    memset(m->mem, 0, sizeof(m->mem));
    /* IVT -> default IRET stub */
    m->mem[0xF0000 + IRET_STUB] = 0xCF;
    for (int i = 0; i < 256; i++) { mem_ww(m, i * 4, IRET_STUB); mem_ww(m, i * 4 + 2, HLE_SEG); }
    static const uint8_t hle_ints[] = {0x08, 0x09, 0x10, 0x15, 0x16, 0x1A, 0x20, 0x21, 0x29, 0x2F, 0x33, 0x67};
    for (unsigned i = 0; i < sizeof hle_ints; i++) {
        uint8_t n = hle_ints[i]; uint32_t off = STUB_BASE + n * 4;
        m->mem[0xF0000 + off] = 0xF1; m->mem[0xF0000 + off + 1] = n; m->mem[0xF0000 + off + 2] = 0xCF;
        mem_ww(m, n * 4, off); mem_ww(m, n * 4 + 2, HLE_SEG);
    }
    /* XMS entry stub: F000:0090 = HLE trap E0, then RETF */
    m->mem[0xF0000 + XMS_STUB] = 0xF1; m->mem[0xF0000 + XMS_STUB + 1] = 0xE0; m->mem[0xF0000 + XMS_STUB + 2] = 0xCB;
    m->xms_total_kb = XMS_KB; m->xms_used_kb = 0;
    /* EMM device-header signature seen at <INT 67h segment>:000A */
    memcpy(m->mem + 0xF0000 + 0x0A, "EMMXXXX0", 8);
    /* BDA */
    mem_ww(m, 0x413, 640); mem_ww(m, 0x410, 0x0021); m->mem[0x449] = 3; mem_ww(m, 0x44A, 80); mem_ww(m, 0x463, 0x3D4);
    /* DOS list of lists */
    m->mem[0xF0000 + LOL_OFF - 2] = 0x00; m->mem[0xF0000 + LOL_OFF - 1] = 0x01;
    /* heap: one big free block from 0x0100 to 0xA000 */
    m->mcb_first = 0x0100;
    mcb_set(m, 0x0100, 'Z', 0, 0xA000 - 0x0100 - 1);
    /* EMS */
    m->ems_total_pages = EMS_PAGES;
    m->ems = calloc(EMS_PAGES, 16384);
    m->ems_bytes = EMS_PAGES * 16384;
    for (int i = 0; i < 256; i++) m->ems_alloc[i] = 0;
    m->ems_next_page = 0;
    m->cur_psp = 0;
    for (int i = 0; i < MAX_FILES; i++) m->files[i].fp = NULL;
    m->files[0].dev = m->files[1].dev = m->files[2].dev = m->files[3].dev = m->files[4].dev = 2;
    m->mouse_visible = 0;
    m->icount = 0; m->ic_base = 0; m->vt_base_ns = 0;
}

/* ------------------------------------------------------- program loader */
static uint16_t build_env(Machine *m, const char *progpath, uint16_t owner) {
    uint16_t seg = mcb_alloc(m, 0x20, owner); if (!seg) return 0;
    const char *env = "COMSPEC=C:\\COMMAND.COM\0PATH=C:\\\0PROMPT=$P$G\0";
    int n = 0; static const char e1[] = "COMSPEC=C:\\COMMAND.COM", e2[] = "PATH=C:\\", e3[] = "PROMPT=$P$G";
    (void)env;
    const char *list[] = {e1, e2, e3};
    for (int i = 0; i < 3; i++) { for (const char *p = list[i]; *p; p++) wr8(m, seg, n++, *p); wr8(m, seg, n++, 0); }
    wr8(m, seg, n++, 0); wr16(m, seg, n, 1); n += 2;
    for (const char *p = progpath; *p; p++) wr8(m, seg, n++, *p);
    wr8(m, seg, n++, 0);
    return seg;
}

static void build_psp(Machine *m, uint16_t psp, uint16_t top, uint16_t parent, uint16_t env, const char *args) {
    for (int i = 0; i < 256; i++) wr8(m, psp, i, 0);
    wr8(m, psp, 0, 0xCD); wr8(m, psp, 1, 0x20);
    wr16(m, psp, 2, top);
    wr8(m, psp, 5, 0x9A);
    wr16(m, psp, 0x0A, mem_rw(m, 0x22 * 4)); wr16(m, psp, 0x0C, mem_rw(m, 0x22 * 4 + 2));
    wr16(m, psp, 0x16, parent);
    for (int i = 0; i < 20; i++) wr8(m, psp, 0x18 + i, 0xFF);
    for (int i = 0; i < 5; i++) wr8(m, psp, 0x18 + i, i);
    wr16(m, psp, 0x2C, env);
    wr16(m, psp, 0x32, 20); wr16(m, psp, 0x34, 0x18); wr16(m, psp, 0x36, psp);
    wr8(m, psp, 0x50, 0xCD); wr8(m, psp, 0x51, 0x21); wr8(m, psp, 0x52, 0xCB);
    int al = (int)strlen(args); if (al > 125) al = 125;
    wr8(m, psp, 0x80, al ? al + 1 : 0);
    if (al) { wr8(m, psp, 0x81, ' '); for (int i = 0; i < al; i++) wr8(m, psp, 0x82 + i, args[i]); wr8(m, psp, 0x82 + al, 0x0D); }
    else wr8(m, psp, 0x81, 0x0D);
}

typedef struct { uint16_t psp, cs, ip, ss, sp; int is_exe; } LoadInfo;

/* Load a program image + build PSP. Does not touch the CPU. */
static int load_prog(Machine *m, const char *name, const char *args, LoadInfo *li) {
    char host[300];
    if (!resolve_path(m, name, host, sizeof host)) { fprintf(stderr, "[dos] cannot find %s\n", name); return -1; }
    FILE *f = fopen(host, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz); if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return -1; }
    fclose(f);
    uint16_t parent = m->cur_psp;
    char dpath[128]; snprintf(dpath, sizeof dpath, "C:\\%s", name);
    for (char *p = dpath; *p; p++) *p = toupper((unsigned char)*p);
    uint16_t owner_tmp = 0xFFFE;
    uint16_t env = build_env(m, dpath, owner_tmp);
    uint16_t maxp = mcb_largest(m);
    uint16_t psp = mcb_alloc(m, maxp, owner_tmp);
    if (!psp) { free(buf); return -1; }
    uint16_t top = psp + maxp;
    /* re-own blocks by new psp */
    wr16(m, psp - 1, 1, psp); if (env) wr16(m, env - 1, 1, psp);
    build_psp(m, psp, top, parent, env, args ? args : "");
    int is_exe = sz >= 2 && buf[0] == 'M' && buf[1] == 'Z';
    li->psp = psp; li->is_exe = is_exe;
    if (!is_exe) {
        for (long i = 0; i < sz; i++) mem_wb(m, LIN(psp, 0x100 + i), buf[i]);
        li->cs = li->ss = psp; li->ip = 0x100; li->sp = 0xFFFE; wr16(m, psp, 0xFFFE, 0);
    } else {
        uint16_t lastpage = buf[2] | (buf[3] << 8), pages = buf[4] | (buf[5] << 8), nreloc = buf[6] | (buf[7] << 8);
        uint16_t hdr = buf[8] | (buf[9] << 8);
        uint16_t ss = buf[0xE] | (buf[0xF] << 8), sp = buf[0x10] | (buf[0x11] << 8), ip = buf[0x14] | (buf[0x15] << 8), cs = buf[0x16] | (buf[0x17] << 8);
        uint16_t roff = buf[0x18] | (buf[0x19] << 8);
        long imgsz = (long)pages * 512 - (lastpage ? 512 - lastpage : 0) - (long)hdr * 16;
        uint16_t load = psp + 0x10;
        for (long i = 0; i < imgsz; i++) mem_wb(m, LIN(load, 0) + i, buf[hdr * 16 + i]);
        for (int i = 0; i < nreloc; i++) {
            uint16_t ro = buf[roff + i * 4] | (buf[roff + i * 4 + 1] << 8), rs = buf[roff + i * 4 + 2] | (buf[roff + i * 4 + 3] << 8);
            uint32_t a = LIN(load + rs, ro); mem_ww(m, a, mem_rw(m, a) + load);
        }
        li->cs = cs + load; li->ip = ip; li->ss = ss + load; li->sp = sp;
    }
    free(buf);
    return 0;
}

/* INT 21h AX=4B03: load overlay at given segment */
static int load_overlay(Machine *m, const char *name, uint16_t seg, uint16_t reloc) {
    char host[300];
    if (!resolve_path(m, name, host, sizeof host)) return -1;
    FILE *f = fopen(host, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz); if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return -1; }
    fclose(f);
    if (sz >= 2 && buf[0] == 'M' && buf[1] == 'Z') {
        uint16_t lastpage = buf[2] | (buf[3] << 8), pages = buf[4] | (buf[5] << 8), nreloc = buf[6] | (buf[7] << 8);
        uint16_t hdr = buf[8] | (buf[9] << 8), roff = buf[0x18] | (buf[0x19] << 8);
        long imgsz = (long)pages * 512 - (lastpage ? 512 - lastpage : 0) - (long)hdr * 16;
        for (long i = 0; i < imgsz; i++) mem_wb(m, LIN(seg, 0) + i, buf[hdr * 16 + i]);
        for (int i = 0; i < nreloc; i++) {
            uint16_t ro = buf[roff + i * 4] | (buf[roff + i * 4 + 1] << 8), rs = buf[roff + i * 4 + 2] | (buf[roff + i * 4 + 3] << 8);
            uint32_t a = LIN(seg + rs, ro); mem_ww(m, a, mem_rw(m, a) + reloc);
        }
    } else for (long i = 0; i < sz; i++) mem_wb(m, LIN(seg, 0) + i, buf[i]);
    free(buf); return 0;
}

int dos_exec(Machine *m, const char *name, const char *args) {
    LoadInfo li;
    if (load_prog(m, name, args, &li)) return -1;
    CPU *c = C(m);
    memset(c->r, 0, sizeof c->r); memset(c->rh, 0, sizeof c->rh);
    c->seg[DS] = c->seg[ES] = li.psp; c->seg[CS] = li.cs; c->ip = li.ip; c->seg[SS] = li.ss; c->r[SP] = li.sp;
    c->iff = 1; c->df = 0; c->halted = 0; c->inhibit = 0;
    m->cur_psp = li.psp; m->exited = 0; m->tsr_kept = 0; m->exit_psp = li.psp; m->dta_seg = li.psp; m->dta_off = 0x80;
    return 0;
}

/* ------------------------------------------------------------- files */
/* ------------------------------------------------------------- RAM cache for the big read-only data files
 * The game streams its pictures / scripts from several 1 MB archives with many small reads and seeks.  On a 3DS every
 * such read is a round trip to the SD card (tens of milliseconds for one picture), which freezes the whole emulator -
 * and with it the sound.  Files opened read-only are therefore loaded into RAM once and served from there. */
#define CACHE_MIN_FILE   (48u * 1024)
#define CACHE_MAX_FILE   (6u * 1024 * 1024)
#define CACHE_MAX_TOTAL  (14u * 1024 * 1024)
#define CACHE_SLOTS      24
static struct { char path[300]; uint8_t *data; uint32_t size; } fcache[CACHE_SLOTS];
static uint32_t fcache_total;
static void cache_drop(const char *host) {
    for (int i = 0; i < CACHE_SLOTS; i++)
        if (fcache[i].data && !strcmp(fcache[i].path, host)) { /* not freed: an open handle may still point into it */ fcache_total -= fcache[i].size; memset(&fcache[i], 0, sizeof fcache[i]); }
}
static int cache_lookup(const char *host, const uint8_t **data, uint32_t *size) {
    for (int i = 0; i < CACHE_SLOTS; i++)
        if (fcache[i].data && !strcmp(fcache[i].path, host)) { *data = fcache[i].data; *size = fcache[i].size; return 1; }
    int slot = -1;
    for (int i = 0; i < CACHE_SLOTS; i++) if (!fcache[i].data) { slot = i; break; }
    if (slot < 0 || strlen(host) >= sizeof fcache[0].path) return 0;
    FILE *f = fopen(host, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < (long)CACHE_MIN_FILE || sz > (long)CACHE_MAX_FILE || fcache_total + (uint32_t)sz > CACHE_MAX_TOTAL) { fclose(f); return 0; }
    uint8_t *buf = malloc((size_t)sz); if (!buf) { fclose(f); return 0; }
    size_t got = 0;
    while (got < (size_t)sz) { size_t c = (size_t)sz - got; if (c > 131072) c = 131072; size_t r = fread(buf + got, 1, c, f); if (!r) break; got += r; }
    fclose(f);
    if (got != (size_t)sz) { free(buf); return 0; }
    snprintf(fcache[slot].path, sizeof fcache[slot].path, "%s", host);
    fcache[slot].data = buf; fcache[slot].size = (uint32_t)sz; fcache_total += (uint32_t)sz;
    *data = buf; *size = (uint32_t)sz; return 1;
}
void dos_cache_preload(Machine *m) {
    DIR *d = opendir(m->gamedir); if (!d) return;
    struct dirent *e; char host[600];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(host, sizeof host, "%s/%s", m->gamedir, e->d_name);
        const uint8_t *p; uint32_t sz; cache_lookup(host, &p, &sz);
    }
    closedir(d);
}
static int alloc_handle(Machine *m) { for (int i = 5; i < MAX_FILES; i++) if (!m->files[i].fp && !m->files[i].dev) return i; return -1; }

static void dos_terminate(Machine *m, int code, int keep_paras) {
    uint16_t psp = m->cur_psp;
    if (keep_paras >= 0) {
        mcb_resize(m, psp, keep_paras < 1 ? 1 : keep_paras, NULL);
        m->tsr_kept = 1;
    } else {
        mcb_free_owner(m, psp);
    }
    for (int i = 5; i < MAX_FILES; i++) if ((m->files[i].fp || m->files[i].dev == 3) && m->files[i].psp == psp) { if (m->files[i].fp) fclose(m->files[i].fp); m->files[i].fp = NULL; m->files[i].mem = NULL; m->files[i].dev = 0; }
    m->exit_code = code; m->exit_psp = psp; m->exited = 1;
    m->cur_psp = rd16(m, psp, 0x16);
}

void machine_con_putc(Machine *m, uint8_t ch) {
    if (m->verbose) fputc(ch, stderr);
    if (ch == '\r') return;
    if (m->con_len >= (int)sizeof(m->con) - 1) { memmove(m->con, m->con + 128, m->con_len - 128); m->con_len -= 128; }
    m->con[m->con_len++] = (char)ch; m->con[m->con_len] = 0;
}

double (*dos_now_ms)(void); double dos_io_max_ms; int dos_io_max_ah; unsigned dos_io_slow;
static void int21(Machine *m);
static void int21_timed(Machine *m) {
    if (!dos_now_ms) { int21(m); return; }
    int ah = R8(C(m), 4); double t0 = dos_now_ms();
    int21(m);
    double dt = dos_now_ms() - t0;
    if (dt > dos_io_max_ms) { dos_io_max_ms = dt; dos_io_max_ah = ah; }
    if (dt > 50.0) dos_io_slow++;
}
static void int21(Machine *m) {
    CPU *c = C(m); uint8_t ah = R8(c, 4), al = R8(c, 0);
    char name[300], host[600];
    if (m->verbose > 1) fprintf(stderr, "[int21] AH=%02X AL=%02X BX=%04X CX=%04X DX=%04X DS=%04X\n", ah, al, c->r[BX], c->r[CX], c->r[DX], c->seg[DS]);
    switch (ah) {
    case 0x00: dos_terminate(m, 0, -1); return;
    case 0x02: case 0x06: machine_con_putc(m, R8(c, 2)); hret(m, -1); return;
    case 0x01: case 0x07: case 0x08: {
        if (m->bk_h == m->bk_t) { machine_skip(m, m->ips / 400); c->ip -= 2; return; }
        uint16_t k = m->bios_kbuf[m->bk_h]; m->bk_h = (m->bk_h + 1) & 15; SETR8(c, 0, k & 0xFF); hret(m, -1); return; }
    case 0x09: { for (int i = 0; i < 500; i++) { uint8_t b = rd8(m, c->seg[DS], c->r[DX] + i); if (b == '$') break; machine_con_putc(m, b); } hret(m, -1); return; }
    case 0x0A: wr8(m, c->seg[DS], c->r[DX] + 1, 0); hret(m, -1); return;
    case 0x0B: machine_poll(m, m->bk_h != m->bk_t); SETR8(c, 0, m->bk_h != m->bk_t ? 0xFF : 0); hret(m, -1); return;
    case 0x0C: if (al == 0x0B) SETR8(c, 0, 0); hret(m, -1); return;
    case 0x0D: hret(m, -1); return;
    case 0x0E: SETR8(c, 0, 5); hret(m, -1); return;
    case 0x19: SETR8(c, 0, 2); hret(m, -1); return;
    case 0x1A: m->dta_seg = c->seg[DS]; m->dta_off = c->r[DX]; hret(m, -1); return;
    case 0x2F: c->seg[ES] = m->dta_seg; c->r[BX] = m->dta_off; hret(m, -1); return;
    case 0x25: mem_ww(m, al * 4, c->r[DX]); mem_ww(m, al * 4 + 2, c->seg[DS]); hret(m, -1); return;
    case 0x35: c->r[BX] = mem_rw(m, al * 4); c->seg[ES] = mem_rw(m, al * 4 + 2); hret(m, -1); return;
    case 0x2A: { time_t t = time(NULL); struct tm *tm = localtime(&t); c->r[CX] = tm->tm_year + 1900; SETR8(c, 6, tm->tm_mon + 1); SETR8(c, 2, tm->tm_mday); SETR8(c, 0, tm->tm_wday); hret(m, -1); return; }
    case 0x2C: { time_t t = time(NULL); struct tm *tm = localtime(&t); SETR8(c, 5, tm->tm_hour); SETR8(c, 1, tm->tm_min); SETR8(c, 6, tm->tm_sec); SETR8(c, 2, 0); hret(m, -1); return; }
    case 0x30: c->r[AX] = 0x0005; c->r[BX] = 0; c->r[CX] = 0; hret(m, -1); return;
    case 0x31: dos_terminate(m, al, c->r[DX]); return;
    case 0x33: if (al == 0 || al == 1) SETR8(c, 2, 0); else if (al == 5) SETR8(c, 2, 3); else if (al == 6) { c->r[BX] = 0x0005; c->r[DX] = 0; } hret(m, -1); return;
    case 0x34: c->seg[ES] = HLE_SEG; c->r[BX] = LOL_OFF + 0x20; hret(m, -1); return;
    case 0x36: c->r[AX] = 4; c->r[BX] = 20000; c->r[CX] = 512; c->r[DX] = 30000; hret(m, 0); return;
    case 0x3B: hret(m, 0); return;
    case 0x47: wr8(m, c->seg[DS], c->r[SI], 0); hret(m, 0); return;
    case 0x3C: case 0x3D: {
        read_asciiz(m, c->seg[DS], c->r[DX], name, sizeof name);
        if (!strcasecmp(name, "EMMXXXX0")) { int h = alloc_handle(m); if (h < 0) { dos_err(m, 4); return; } m->files[h].dev = 1; m->files[h].psp = m->cur_psp; c->r[AX] = h; hret(m, 0); return; }
        int ex = resolve_path(m, name, host, sizeof host);
        FILE *f;
        if (ah == 0x3D && ex && (al & 3) == 0) {          /* read-only: serve big data files from the RAM cache */
            const uint8_t *cd; uint32_t cs;
            if (cache_lookup(host, &cd, &cs)) {
                int h = alloc_handle(m); if (h < 0) { dos_err(m, 4); return; }
                if (m->verbose) fprintf(stderr, "[dos] cached open %s (%u bytes)\n", host, (unsigned)cs);
                m->files[h].fp = NULL; m->files[h].dev = 3; m->files[h].mem = cd; m->files[h].msize = cs; m->files[h].mpos = 0; m->files[h].psp = m->cur_psp;
                c->r[AX] = h; hret(m, 0); return;
            }
        } else cache_drop(host);                           /* the file may be modified: forget any cached copy */
        if (ah == 0x3C) f = fopen(host, "w+b");
        else f = ex ? fopen(host, (al & 3) == 0 ? "rb" : "r+b") : NULL;
        if (!f && ah == 0x3D && ex && (al & 3) != 0) f = fopen(host, "rb");
        if (!f) { dos_err(m, ex ? 5 : 2); return; }
        int h = alloc_handle(m); if (h < 0) { fclose(f); dos_err(m, 4); return; }
        m->files[h].fp = f; m->files[h].dev = 0; m->files[h].psp = m->cur_psp;
        c->r[AX] = h; hret(m, 0); return; }
    case 0x3E: { int h = c->r[BX]; if (h < 0 || h >= MAX_FILES) { dos_err(m, 6); return; }
        if (m->files[h].fp) { fclose(m->files[h].fp); m->files[h].fp = NULL; }
        if (m->files[h].dev == 3) m->files[h].mem = NULL;
        if (h >= 5) m->files[h].dev = 0;
        hret(m, 0); return; }
    case 0x3F: { int h = c->r[BX]; uint16_t n = c->r[CX];
        if (h < 0 || h >= MAX_FILES || (!m->files[h].fp && !m->files[h].dev)) { dos_err(m, 6); return; }
        if (m->files[h].dev == 3) {
            DosFile *df = &m->files[h]; uint32_t left = df->msize > df->mpos ? df->msize - df->mpos : 0, got = n < left ? n : left;
            uint16_t o = c->r[DX]; const uint8_t *src = df->mem + df->mpos;
            for (uint32_t i = 0; i < got; i++) mem_wb(m, LIN(c->seg[DS], (uint16_t)(o + i)), src[i]);
            df->mpos += got; c->r[AX] = (uint16_t)got; hret(m, 0); return;
        }
        if (!m->files[h].fp) { c->r[AX] = 0; hret(m, 0); return; }
        static uint8_t tmp[65536]; size_t got = fread(tmp, 1, n, m->files[h].fp);
        uint32_t a = LIN(c->seg[DS], c->r[DX]); uint16_t o = c->r[DX];
        for (size_t i = 0; i < got; i++) mem_wb(m, LIN(c->seg[DS], (uint16_t)(o + i)), tmp[i]);
        (void)a; c->r[AX] = (uint16_t)got; hret(m, 0); return; }
    case 0x40: { int h = c->r[BX]; uint16_t n = c->r[CX];
        if (h < 0 || h >= MAX_FILES || (!m->files[h].fp && !m->files[h].dev)) { dos_err(m, 6); return; }
        if (!m->files[h].fp) { if (h == 1 || h == 2) for (int i = 0; i < n; i++) machine_con_putc(m, rd8(m, c->seg[DS], c->r[DX] + i)); c->r[AX] = n; hret(m, 0); return; }
        static uint8_t tmp[65536]; for (int i = 0; i < n; i++) tmp[i] = rd8(m, c->seg[DS], c->r[DX] + i);
        size_t w = n ? fwrite(tmp, 1, n, m->files[h].fp) : 0; fflush(m->files[h].fp);
        c->r[AX] = (uint16_t)w; hret(m, 0); return; }
    case 0x41: read_asciiz(m, c->seg[DS], c->r[DX], name, sizeof name); if (resolve_path(m, name, host, sizeof host)) cache_drop(host); if (resolve_path(m, name, host, sizeof host) && remove(host) == 0) hret(m, 0); else dos_err(m, 2); return;
    case 0x42: { int h = c->r[BX]; if (h < 0 || h >= MAX_FILES || (!m->files[h].fp && m->files[h].dev != 3)) { dos_err(m, 6); return; }
        long off = (int32_t)(((uint32_t)c->r[CX] << 16) | c->r[DX]); int wh = al == 0 ? SEEK_SET : al == 1 ? SEEK_CUR : SEEK_END;
        if (m->files[h].dev == 3) {
            DosFile *df = &m->files[h]; long base = wh == SEEK_SET ? 0 : wh == SEEK_CUR ? (long)df->mpos : (long)df->msize;
            long np = base + off; if (np < 0) np = 0; df->mpos = (uint32_t)np;
            c->r[AX] = np & 0xFFFF; c->r[DX] = (np >> 16) & 0xFFFF; hret(m, 0); return;
        }
        fseek(m->files[h].fp, off, wh); long p = ftell(m->files[h].fp); c->r[AX] = p & 0xFFFF; c->r[DX] = (p >> 16) & 0xFFFF; hret(m, 0); return; }
    case 0x43: read_asciiz(m, c->seg[DS], c->r[DX], name, sizeof name);
        if (!resolve_path(m, name, host, sizeof host)) { dos_err(m, 2); return; }
        if (al == 0) c->r[CX] = 0x20; hret(m, 0); return;
    case 0x44: {
        int h = c->r[BX];
        switch (al) {
        case 0: if (h < 0 || h >= MAX_FILES || (!m->files[h].fp && !m->files[h].dev)) { dos_err(m, 6); return; }
            c->r[DX] = m->files[h].dev == 1 ? 0x80C0 : m->files[h].dev == 2 ? 0x80D3 : 0x0002; hret(m, 0); return;
        case 6: c->r[AX] = 0x00FF; hret(m, 0); return;
        case 7: c->r[AX] = 0x00FF; hret(m, 0); return;
        case 8: c->r[AX] = 1; hret(m, 0); return;
        default: hret(m, 0); return; } }
    case 0x45: { int h = c->r[BX]; int n = alloc_handle(m); if (n < 0 || h < 0 || h >= MAX_FILES) { dos_err(m, 4); return; }
        m->files[n] = m->files[h]; m->files[n].psp = m->cur_psp; c->r[AX] = n; hret(m, 0); return; }
    case 0x48: { uint16_t req = c->r[BX]; uint16_t s = mcb_alloc(m, req, m->cur_psp);
        if (!s) { c->r[BX] = mcb_largest(m); dos_err(m, 8); return; } c->r[AX] = s; hret(m, 0); return; }
    case 0x49: mcb_free(m, c->seg[ES]); hret(m, 0); return;
    case 0x4A: { uint16_t mx = 0; if (mcb_resize(m, c->seg[ES], c->r[BX], &mx)) hret(m, 0); else { c->r[BX] = mx; dos_err(m, 8); } return; }
    case 0x4C: dos_terminate(m, al, -1); return;
    case 0x4D: c->r[AX] = m->exit_code & 0xFF; hret(m, 0); return;
    case 0x4E: case 0x4F: {
        /* very small FindFirst/FindNext supporting simple wildcards */
        static DIR *dir; static char pat[64];
        if (ah == 0x4E) {
            read_asciiz(m, c->seg[DS], c->r[DX], name, sizeof name);
            char *sl = strrchr(name, '\\'); const char *pt = sl ? sl + 1 : name; if (name[0] && name[1] == ':' && !sl) pt = name + 2;
            snprintf(pat, sizeof pat, "%s", pt);
            if (dir) closedir(dir);
            dir = opendir(m->gamedir);
        }
        if (!dir) { dos_err(m, 18); return; }
        struct dirent *e;
        while ((e = readdir(dir))) {
            const char *p = pat, *s = e->d_name; int ok = 1;
            /* simple 8.3 wildcard match */
            const char *star = NULL, *ss = s;
            while (*s) {
                if (*p == '*') { star = p++; ss = s; }
                else if (*p == '?' || tolower(*p) == tolower(*s)) { p++; s++; }
                else if (star) { p = star + 1; s = ++ss; }
                else { ok = 0; break; }
            }
            while (ok && *p == '*') p++;
            if (ok && *p) ok = 0;
            if (!ok || e->d_name[0] == '.') continue;
            uint16_t ds = m->dta_seg, dof = m->dta_off;
            wr8(m, ds, dof + 0x15, 0x20); wr16(m, ds, dof + 0x16, 0); wr16(m, ds, dof + 0x18, 0);
            char full[600]; snprintf(full, sizeof full, "%s/%s", m->gamedir, e->d_name); struct stat st; uint32_t size = 0;
            if (!stat(full, &st)) size = st.st_size;
            wr16(m, ds, dof + 0x1A, size & 0xFFFF); wr16(m, ds, dof + 0x1C, size >> 16);
            for (int i = 0; i < 13; i++) wr8(m, ds, dof + 0x1E + i, i < (int)strlen(e->d_name) ? toupper((unsigned char)e->d_name[i]) : 0);
            hret(m, 0); return;
        }
        closedir(dir); dir = NULL; dos_err(m, 18); return; }
    case 0x4B: {
        read_asciiz(m, c->seg[DS], c->r[DX], name, sizeof name);
        uint16_t bs = c->seg[ES], bo = c->r[BX];
        if (al == 1) {
            char args[130]; uint16_t cto = rd16(m, bs, bo + 2), cts = rd16(m, bs, bo + 4);
            int n = rd8(m, cts, cto), k = 0; if (n > 126) n = 126;
            for (int i = 0; i < n; i++) { uint8_t ch = rd8(m, cts, cto + 1 + i); if (ch == 0x0D) break; if (!(k == 0 && ch == ' ')) args[k++] = ch; }
            args[k] = 0;
            LoadInfo li;
            if (load_prog(m, name, args, &li)) { dos_err(m, 2); return; }
            wr16(m, bs, bo + 0x0E, li.sp); wr16(m, bs, bo + 0x10, li.ss); wr16(m, bs, bo + 0x12, li.ip); wr16(m, bs, bo + 0x14, li.cs);
            m->cur_psp = li.psp; if (m->on_load) m->on_load(m, li.psp + 0x10); if (m->on_load2) m->on_load2(m, li.psp + 0x10); hret(m, 0); return;
        } else if (al == 3) {
            if (load_overlay(m, name, rd16(m, bs, bo), rd16(m, bs, bo + 2))) { dos_err(m, 2); return; }
            hret(m, 0); return;
        }
        if (m->verbose) fprintf(stderr, "[dos] EXEC AL=%02X '%s' unsupported\n", al, name);
        dos_err(m, 1); return; }
    case 0x50: m->cur_psp = c->r[BX]; hret(m, -1); return;
    case 0x51: case 0x62: c->r[BX] = m->cur_psp; hret(m, -1); return;
    case 0x52: c->seg[ES] = HLE_SEG; c->r[BX] = LOL_OFF; hret(m, -1); return;
    case 0x56: hret(m, 0); return;
    case 0x57: if (al == 0) { c->r[CX] = 0; c->r[DX] = 0x21; } hret(m, 0); return;
    case 0x58: if (al == 0) c->r[AX] = 0; hret(m, 0); return;
    case 0x5D: hret(m, 0); return;
    case 0x63: dos_err(m, 1); return;
    default:
        if (m->verbose) {
            fprintf(stderr, "[dos] unhandled INT21 AH=%02X AL=%02X\n", ah, al);
            if (ah == 0x4B) { read_asciiz(m, c->seg[DS], c->r[DX], name, sizeof name); fprintf(stderr, "[dos]   exec '%s' blk=%04X:%04X\n", name, c->seg[ES], c->r[BX]);
                for (int i = 0; i < 14; i++) fprintf(stderr, " %02X", rd8(m, c->seg[ES], c->r[BX] + i)); fputc(10, stderr); }
        }
        m->stat_unhandled++; hret(m, 1); c->r[AX] = 1; return;
    }
}

/* ------------------------------------------------------------- BIOS */
static const uint8_t scan_to_ascii[0x40] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, 9,
    'q','w','e','r','t','y','u','i','o','p','[',']', 13, 0, 'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0,'\\','z','x','c','v',
    'b','n','m',',','.','/', 0,'*', 0, ' ', 0, 0, 0, 0, 0, 0};

static void bios_int9(Machine *m) {
    CPU *c = C(m);
    uint8_t sc = io_inb(m, 0x60);
    io_outb(m, 0x20, 0x20);
    int brk = sc & 0x80; uint8_t mk = sc & 0x7F;
    if (mk == 0x2A || mk == 0x36) m->shift_state = brk ? (m->shift_state & ~1) : (m->shift_state | 1);
    else if (!brk) {
        uint8_t as = mk < 0x40 ? scan_to_ascii[mk] : 0;
        int nt = (m->bk_t + 1) & 15;
        if (nt != m->bk_h) { m->bios_kbuf[m->bk_t] = (mk << 8) | as; m->bk_t = nt; }
    }
    (void)c;
    hret(m, -1);
}

static void bios_int8(Machine *m) {
    CPU *c = C(m);
    uint32_t t = mem_rw(m, 0x46C) | ((uint32_t)mem_rw(m, 0x46E) << 16); t++;
    mem_ww(m, 0x46C, t & 0xFFFF); mem_ww(m, 0x46E, t >> 16);
    io_outb(m, 0x20, 0x20);
    hret(m, -1);
    uint16_t o = mem_rw(m, 0x1C * 4), s = mem_rw(m, 0x1C * 4 + 2);
    if (!(s == HLE_SEG && o == IRET_STUB)) cpu_int(c, 0x1C);
}

static void bios_int16(Machine *m) {
    CPU *c = C(m); uint8_t ah = R8(c, 4);
    switch (ah) {
    case 0x00: case 0x10:
        if (m->bk_h == m->bk_t) { machine_skip(m, m->ips / 400); c->ip -= 2; return; }
        c->r[AX] = m->bios_kbuf[m->bk_h]; m->bk_h = (m->bk_h + 1) & 15; hret(m, -1); return;
    case 0x01: case 0x11:
        if (m->bk_h == m->bk_t) { machine_poll(m, 0); hret(m, -1); c->zf = 1; } else { machine_poll(m, 1); c->r[AX] = m->bios_kbuf[m->bk_h]; hret(m, -1); c->zf = 0; } return;
    case 0x02: case 0x12: SETR8(c, 0, m->shift_state); hret(m, -1); return;
    case 0x05: { int nt = (m->bk_t + 1) & 15; if (nt != m->bk_h) { m->bios_kbuf[m->bk_t] = c->r[CX]; m->bk_t = nt; } SETR8(c, 0, 0); hret(m, -1); return; }
    default: hret(m, -1); return;
    }
}

static void bios_int10(Machine *m) {
    CPU *c = C(m); uint8_t ah = R8(c, 4), al = R8(c, 0);
    switch (ah) {
    case 0x00: vga_set_mode(m, al); m->mem[0x449] = al & 0x7F; break;
    case 0x0E: machine_con_putc(m, al); break;
    case 0x0F: c->r[AX] = ((80) << 8) | m->mem[0x449]; SETR8(c, 7, 0); break;
    case 0x10: {
        VGA *v = &m->vga;
        switch (al) {
        case 0x00: v->attr[R8(c, 3) & 15] = R8(c, 7); v->dirty++; break;
        case 0x02: for (int i = 0; i < 16; i++) v->attr[i] = rd8(m, c->seg[ES], c->r[DX] + i); v->dirty++; break;
        case 0x10: v->dac[R8(c, 3)][0] = R8(c, 6) & 63; v->dac[R8(c, 3)][1] = R8(c, 5) & 63; v->dac[R8(c, 3)][2] = R8(c, 1) & 63; v->dirty++; break;
        case 0x12: for (int i = 0; i < c->r[CX]; i++) for (int k = 0; k < 3; k++) v->dac[(c->r[BX] + i) & 255][k] = rd8(m, c->seg[ES], c->r[DX] + i * 3 + k) & 63; v->dirty++; break;
        case 0x15: SETR8(c, 6, v->dac[R8(c, 3)][0]); SETR8(c, 5, v->dac[R8(c, 3)][1]); SETR8(c, 1, v->dac[R8(c, 3)][2]); break;
        case 0x17: for (int i = 0; i < c->r[CX]; i++) for (int k = 0; k < 3; k++) wr8(m, c->seg[ES], c->r[DX] + i * 3 + k, v->dac[(c->r[BX] + i) & 255][k]); break;
        } break; }
    case 0x12: if (R8(c, 3) == 0x10) { SETR8(c, 7, 0); SETR8(c, 3, 3); c->r[CX] = 0; } break;
    case 0x1A: if (al == 0) { SETR8(c, 0, 0x1A); SETR8(c, 3, 8); SETR8(c, 7, 0); } break;
    default: break;
    }
    hret(m, -1);
}

static void bios_int1a(Machine *m) {
    CPU *c = C(m); uint8_t ah = R8(c, 4);
    if (ah == 0) { c->r[DX] = mem_rw(m, 0x46C); c->r[CX] = mem_rw(m, 0x46E); SETR8(c, 0, 0); }
    hret(m, -1);
}

static void bios_int15(Machine *m) {
    CPU *c = C(m); uint8_t ah = R8(c, 4);
    if (ah == 0x88) { c->r[AX] = 0; hret(m, 0); return; }
    if (ah == 0x86 || ah == 0x90 || ah == 0x91) { hret(m, 0); return; }
    SETR8(c, 4, 0x86); hret(m, 1);
}

/* --------------------------------------------------------------- mouse */
static void mouse(Machine *m) {
    CPU *c = C(m); uint16_t ax = c->r[AX];
    if (m->verbose > 1) { static int mcnt; if (mcnt++ < 40) fprintf(stderr, "[mouse] AX=%04X CX=%04X DX=%04X present=%d\n", ax, c->r[CX], c->r[DX], m->mouse_present); }
    switch (ax) {
    case 0x00: if (m->mouse_present) { c->r[AX] = 0xFFFF; c->r[BX] = 2; } else c->r[AX] = 0;
        m->mouse_visible = 0; m->mouse_minx = 0; m->mouse_maxx = 639; m->mouse_miny = 0; m->mouse_maxy = 479; break;
    case 0x01: m->mouse_visible++; break;
    case 0x02: m->mouse_visible--; break;
    case 0x03: {
        /* a poll that sees no change is idle time: burn a slice of virtual time instead of spinning on it */
        static int lx = -1, ly = -1, lb = -1;
        machine_poll(m, !(m->mouse_x == lx && m->mouse_y == ly && m->mouse_btn == lb));
        lx = m->mouse_x; ly = m->mouse_y; lb = m->mouse_btn;
        c->r[BX] = m->mouse_btn; c->r[CX] = m->mouse_x; c->r[DX] = m->mouse_y; break; }
    case 0x04: m->mouse_x = c->r[CX]; m->mouse_y = c->r[DX]; break;
    case 0x07: m->mouse_minx = (int16_t)c->r[CX]; m->mouse_maxx = (int16_t)c->r[DX]; break;
    case 0x08: m->mouse_miny = (int16_t)c->r[CX]; m->mouse_maxy = (int16_t)c->r[DX]; break;
    case 0x0B: c->r[CX] = 0; c->r[DX] = 0; break;
    case 0x05: case 0x06: c->r[BX] = 0; c->r[CX] = m->mouse_x; c->r[DX] = m->mouse_y; c->r[AX] = m->mouse_btn; break;
    case 0x24: c->r[BX] = 0x0626; c->r[CX] = 0x0400; break;
    default: break;
    }
    hret(m, -1);
}

/* ----------------------------------------------------------------- EMS */
static int ems_free_pages(Machine *m) {
    int used = 0; for (int h = 0; h < 256; h++) if (m->ems_alloc[h]) used += m->ems_handle_pages[h];
    return m->ems_total_pages - used;
}
static int ems_find_base(Machine *m, int pages) {
    /* first fit among sorted allocations */
    int base = 0;
    for (;;) {
        int clash = -1;
        for (int h = 0; h < 256; h++) if (m->ems_alloc[h]) {
            int b = m->ems_handle_base[h], e = b + m->ems_handle_pages[h];
            if (base < e && base + pages > b) { clash = e; break; }
        }
        if (clash < 0) return base + pages <= m->ems_total_pages ? base : -1;
        base = clash;
    }
}
static void ems(Machine *m) {
    CPU *c = C(m); uint8_t ah = R8(c, 4), al = R8(c, 0);
    uint16_t in_bx = c->r[BX], in_dx = c->r[DX];
    switch (ah) {
    case 0x40: SETR8(c, 4, 0); break;
    case 0x41: c->r[BX] = FRAME_SEG; SETR8(c, 4, 0); break;
    case 0x42: c->r[BX] = ems_free_pages(m); c->r[DX] = m->ems_total_pages; SETR8(c, 4, 0); break;
    case 0x43: {
        int pages = c->r[BX], h = -1;
        if (pages > ems_free_pages(m)) { SETR8(c, 4, pages > m->ems_total_pages ? 0x87 : 0x88); break; }
        for (int i = 1; i < 256; i++) if (!m->ems_alloc[i]) { h = i; break; }
        if (h < 0) { SETR8(c, 4, 0x85); break; }
        int base = ems_find_base(m, pages); if (base < 0) { SETR8(c, 4, 0x88); break; }
        m->ems_alloc[h] = 1; m->ems_handle_pages[h] = pages; m->ems_handle_base[h] = base;
        c->r[DX] = h; SETR8(c, 4, 0); break; }
    case 0x44: {
        int phys = al, lp = c->r[BX], h = c->r[DX];
        if (phys > 3) { SETR8(c, 4, 0x8B); break; }
        if (lp == 0xFFFF) { m->ems_map[phys] = -1; SETR8(c, 4, 0); break; }
        if (h < 0 || h > 255 || !m->ems_alloc[h]) { SETR8(c, 4, 0x83); break; }
        if (lp >= m->ems_handle_pages[h]) { SETR8(c, 4, 0x8A); break; }
        m->ems_map[phys] = m->ems_handle_base[h] + lp; SETR8(c, 4, 0); break; }
    case 0x45: { int h = c->r[DX]; if (h < 0 || h > 255 || !m->ems_alloc[h]) { SETR8(c, 4, 0x83); break; }
        memset(m->ems + ((size_t)m->ems_handle_base[h] << 14), 0, (size_t)m->ems_handle_pages[h] << 14);
        m->ems_alloc[h] = 0; SETR8(c, 4, 0); break; }
    case 0x46: SETR8(c, 4, 0); SETR8(c, 0, 0x40); break;
    case 0x47: case 0x48: SETR8(c, 4, 0); break;
    case 0x4B: { int n = 0; for (int h = 0; h < 256; h++) n += m->ems_alloc[h]; c->r[BX] = n; SETR8(c, 4, 0); break; }
    case 0x4C: { int h = c->r[DX]; if (h > 255 || !m->ems_alloc[h]) { SETR8(c, 4, 0x83); break; } c->r[BX] = m->ems_handle_pages[h]; SETR8(c, 4, 0); break; }
    case 0x4D: { c->r[BX] = 0; SETR8(c, 4, 0); break; }
    case 0x4E: SETR8(c, 4, 0); break;
    case 0x51: { int h = c->r[DX]; if (h > 255 || !m->ems_alloc[h]) { SETR8(c, 4, 0x83); break; }
        int np = c->r[BX]; if (np <= m->ems_handle_pages[h]) { m->ems_handle_pages[h] = np; c->r[BX] = np; SETR8(c, 4, 0); } else SETR8(c, 4, 0x88); break; }
    case 0x53: SETR8(c, 4, 0); break;
    case 0x59: SETR8(c, 4, 0x84); break;
    default: if (m->verbose) fprintf(stderr, "[ems] unhandled AH=%02X\n", ah); SETR8(c, 4, 0x84); m->stat_unhandled++; break;
    }
    if (m->verbose) fprintf(stderr, "[ems] AH=%02X AL=%02X BX=%04X DX=%04X -> AH=%02X BX=%04X DX=%04X\n", ah, al, in_bx, in_dx, R8(c, 4), c->r[BX], c->r[DX]);
    hret(m, -1);
}


/* ---------------- XMS (HIMEM.SYS emulation) ---------------- */
static uint32_t xms_largest(Machine *m) { return m->xms_total_kb - m->xms_used_kb; }
static int xms_alloc(Machine *m, uint32_t kb) {
    if (kb > xms_largest(m)) return 0;
    for (int h = 1; h < 64; h++) if (!m->xms[h].p) {
        m->xms[h].p = calloc(kb ? kb : 1, 1024); if (!m->xms[h].p) return 0;
        m->xms[h].kb = kb; m->xms_used_kb += kb; return h;
    }
    return 0;
}
static int xms_valid(Machine *m, int h) { return h > 0 && h < 64 && m->xms[h].p; }
/* copy between (handle,offset) endpoints; handle 0 = real-mode seg:off in offset */
static int xms_move(Machine *m, uint32_t len, uint16_t sh, uint32_t so, uint16_t dh, uint32_t doff) {
    if ((sh && !xms_valid(m, sh)) || (dh && !xms_valid(m, dh))) return 0xA3 - (sh && !xms_valid(m, sh) ? 0 : 1);
    if (sh && (uint64_t)so + len > (uint64_t)m->xms[sh].kb * 1024) return 0xA7;
    if (dh && (uint64_t)doff + len > (uint64_t)m->xms[dh].kb * 1024) return 0xA7;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b;
        if (sh) b = m->xms[sh].p[so + i]; else b = mem_rb(m, LIN(so >> 16, (so & 0xFFFF)) + i);
        if (dh) m->xms[dh].p[doff + i] = b; else mem_wb(m, LIN(doff >> 16, (doff & 0xFFFF)) + i, b);
    }
    return 0;
}
static void xms(Machine *m) {
    CPU *c = C(m); uint8_t ah = R8(c, 4);
    if (m->verbose > 1) fprintf(stderr, "[xms] AH=%02X DX=%04X BX=%04X\n", ah, c->r[DX], c->r[BX]);
    int ok = 1; int err = 0;
    switch (ah) {
    case 0x00: c->r[AX] = 0x0300; c->r[BX] = 0x0301; c->r[DX] = 0; break;
    case 0x01: ok = 0; err = 0x90; break;                    /* no HMA */
    case 0x02: ok = 0; err = 0x93; break;
    case 0x03: case 0x04: case 0x05: case 0x06: break;       /* A20 control: always succeeds */
    case 0x07: c->r[AX] = 1; goto done_nob;
    case 0x08: { uint32_t f = xms_largest(m); c->r[AX] = f > 0xFFFF ? 0xFFFF : f; c->r[DX] = f > 0xFFFF ? 0xFFFF : f; if (!f) { ok = 0; err = 0xA0; } goto done_noflag; }
    case 0x09: { int h = xms_alloc(m, c->r[DX]); if (h) c->r[DX] = h; else { ok = 0; err = 0xA0; } break; }
    case 0x0A: { int h = c->r[DX]; if (!xms_valid(m, h)) { ok = 0; err = 0xA2; break; } free(m->xms[h].p); m->xms[h].p = NULL; m->xms_used_kb -= m->xms[h].kb; m->xms[h].kb = 0; break; }
    case 0x0B: {
        uint16_t ds = c->seg[DS], si = c->r[SI];
        uint32_t len = rd16(m, ds, si) | ((uint32_t)rd16(m, ds, si + 2) << 16);
        uint16_t sh = rd16(m, ds, si + 4); uint32_t so = rd16(m, ds, si + 6) | ((uint32_t)rd16(m, ds, si + 8) << 16);
        uint16_t dh = rd16(m, ds, si + 10); uint32_t dof = rd16(m, ds, si + 12) | ((uint32_t)rd16(m, ds, si + 14) << 16);
        int e = xms_move(m, len, sh, so, dh, dof); if (e) { ok = 0; err = e; }
        break; }
    case 0x0C: { int h = c->r[DX]; if (!xms_valid(m, h)) { ok = 0; err = 0xA2; break; } uint32_t a = 0x110000u + (uint32_t)h * 0x100000u; c->r[DX] = a >> 16; c->r[BX] = a & 0xFFFF; break; }
    case 0x0D: { if (!xms_valid(m, c->r[DX])) { ok = 0; err = 0xA2; } break; }
    case 0x0E: { int h = c->r[DX]; if (!xms_valid(m, h)) { ok = 0; err = 0xA2; break; } int fr = 0; for (int i = 1; i < 64; i++) if (!m->xms[i].p) fr++; SETR8(c, 7, 0); SETR8(c, 3, fr); c->r[DX] = m->xms[h].kb; break; }
    case 0x0F: { int h = c->r[DX]; if (!xms_valid(m, h)) { ok = 0; err = 0xA2; break; } uint32_t nk = c->r[BX];
        if (nk > m->xms[h].kb && nk - m->xms[h].kb > xms_largest(m)) { ok = 0; err = 0xA0; break; }
        uint8_t *np = calloc(nk ? nk : 1, 1024); memcpy(np, m->xms[h].p, 1024u * (nk < m->xms[h].kb ? nk : m->xms[h].kb)); free(m->xms[h].p);
        m->xms_used_kb = m->xms_used_kb - m->xms[h].kb + nk; m->xms[h].p = np; m->xms[h].kb = nk; break; }
    case 0x10: ok = 0; err = 0xB1; c->r[DX] = 0; break;     /* no UMBs */
    case 0x11: ok = 0; err = 0xB1; break;
    case 0x88: { uint32_t f = xms_largest(m); (c->r[AX]=f, c->rh[AX]=f>>16); SETR8(c, 3, 0); (c->r[CX]=(0x10FFFFu + m->xms_total_kb * 1024u), c->rh[CX]=(0x10FFFFu + m->xms_total_kb * 1024u)>>16); (c->r[DX]=f, c->rh[DX]=f>>16); goto done_noflag; }
    case 0x89: { uint32_t kb = ((uint32_t)c->rh[DX] << 16) | c->r[DX]; int h = xms_alloc(m, kb); if (h) c->r[DX] = h; else { ok = 0; err = 0xA0; } break; }
    case 0x8E: { int h = c->r[DX]; if (!xms_valid(m, h)) { ok = 0; err = 0xA2; break; } int fr = 0; for (int i = 1; i < 64; i++) if (!m->xms[i].p) fr++; SETR8(c, 7, 0); SETR8(c, 3, fr); (c->r[DX]=m->xms[h].kb, c->rh[DX]=m->xms[h].kb>>16); break; }
    default: if (m->verbose) fprintf(stderr, "[xms] unhandled AH=%02X\n", ah); ok = 0; err = 0x80; m->stat_unhandled++; break;
    }
    if (ok) c->r[AX] = (ah == 0x00 ? c->r[AX] : 1); else { c->r[AX] = 0; SETR8(c, 3, err); }
done_noflag:
done_nob:
    /* RETF */
    c->ip = rd16(m, c->seg[SS], c->r[SP]); c->r[SP] += 2;
    c->seg[CS] = rd16(m, c->seg[SS], c->r[SP]); c->r[SP] += 2;
}


static void int2f(Machine *m) {
    CPU *c = C(m);
    if (c->r[AX] == 0x4300) SETR8(c, 0, 0x80);                       /* XMS driver installed */
    else if (c->r[AX] == 0x4310) { c->seg[ES] = HLE_SEG; c->r[BX] = XMS_STUB; }
    hret(m, -1);
}

void hle_call(Machine *m, int id) {
    switch (id) {
    case 0x08: bios_int8(m); break;
    case 0x09: bios_int9(m); break;
    case 0x10: bios_int10(m); break;
    case 0x15: bios_int15(m); break;
    case 0x16: bios_int16(m); break;
    case 0x1A: bios_int1a(m); break;
    case 0x20: dos_terminate(m, 0, -1); break;
    case 0x21: int21_timed(m); break;
    case 0x29: machine_con_putc(m, R8(C(m), 0)); hret(m, -1); break;
    case 0x2F: int2f(m); break;
    case 0x33: mouse(m); break;
    case 0xE0: xms(m); break;
    case 0x67: ems(m); break;
    default: hret(m, -1); break;
    }
}
