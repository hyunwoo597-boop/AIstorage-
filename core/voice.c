/* Voice for the Korean DOS release: plays the Windows version's voice clips (VOICE.ARC) for the lines that have one.
 *
 * The Korean scene scripts live in SAGWA.DAT (96 files back to back, a directory at the end), the uncensored versions of some of them in ADULT.DAT (same format).  When the engine reads one
 * of them we note which scene it is and where in memory it went.  The engine's script loop fetches every item at
 * "mov al,[si] / cmp al,0 / jz .. / cmp al,1 / jz .. / cmp al,4" - a breakpoint there sees SI; when SI is the first
 * character of a line listed in voice_map.h (built by tools/voice/), that line's clip is requested.  The front end
 * takes the request with voice_take() and plays it. */
#include "voice.h"
#include "voice_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAXSCN 128
struct Voice {
    int nscn; char name[MAXSCN][12]; uint32_t start[MAXSCN], size[MAXSCN]; int8_t map[MAXSCN]; uint8_t arc[MAXSCN];  /* arc: 1 SAGWA.DAT, 2 ADULT.DAT */  /* map: index into VM_SCENES or -1 */
    int cur;                    /* VM_SCENES index of the scene in the script buffer, -1 none */
    uint16_t seg, off;          /* where it was loaded */
    int last;                   /* VM_LINES index last requested (a line is announced once per arrival) */
    const char *pending;
    unsigned fired;
};

static int sagwa_dir(Voice *v, const char *path, int arcid, const char *prefix) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long fs = ftell(f);
    long tail = fs - 4, lo = tail - 8192; if (lo < 0) lo = 0;
    uint8_t *b = malloc((size_t)(tail - lo)); if (!b) { fclose(f); return 0; }
    fseek(f, lo, SEEK_SET); if (fread(b, 1, (size_t)(tail - lo), f) != (size_t)(tail - lo)) { free(b); fclose(f); return 0; }
    fclose(f);
    int len = (int)(tail - lo);
    for (int i = 0; i < len; i++) b[i] ^= 0xAA;
    /* find the start p so that the entries (len, name, u16 size) end exactly at the tail and their sizes add up to p */
    for (int p = 0; p < len; p++) {
        int q = p, n = v->nscn; uint32_t sum = 0, ok = 1; size_t pl = strlen(prefix);
        while (q < len && ok) {
            int l = b[q]; if (l < 1 || l > 10 || q + 3 + l > len) { ok = 0; break; }
            for (int k = 0; k < l; k++) { uint8_t ch = b[q + 1 + k]; if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) ok = 0; }
            if (!ok || n >= MAXSCN) { ok = 0; break; }
            uint32_t sz = b[q + 1 + l] | b[q + 2 + l] << 8;
            if (pl + (size_t)l >= sizeof v->name[0]) { ok = 0; break; }
            memcpy(v->name[n], prefix, pl);
            for (int k = 0; k < l; k++) v->name[n][pl + k] = (char)(b[q + 1 + k] - 32 * (b[q + 1 + k] >= 'a'));
            v->name[n][pl + l] = 0; v->start[n] = sum; v->size[n] = sz; v->arc[n] = (uint8_t)arcid; sum += sz; n++; q += 3 + l;
        }
        if (ok && q == len && n > v->nscn && (long)sum == lo + p) { v->nscn = n; free(b); return 1; }
    }
    free(b); return 0;
}

static int v_open(Machine *m, const char *dosname) {
    (void)m; const char *s = strrchr(dosname, '\\'); s = s ? s + 1 : dosname;
    const char *c = strrchr(s, ':'); if (c) s = c + 1;
    return strcasecmp(s, "SAGWA.DAT") == 0 ? 1 : strcasecmp(s, "ADULT.DAT") == 0 ? 2 : 0;
}
static void v_read(Machine *m, int tag, uint32_t pos, uint32_t n, uint16_t seg, uint16_t off) {
    Voice *v = m->voice; if (!v || n < 64) return;
    if (getenv("VDBG")) fprintf(stderr, "[voice] read pos=%u n=%u -> %04X:%04X\n", (unsigned)pos, (unsigned)n, (unsigned)seg, (unsigned)off);
    /* the uncensored scenes (ADULT.DAT) replace scenes of the same name: a read that is not exactly one known scene forgets the current one */
    v->cur = -1; v->last = -1;
    for (int i = 0; i < v->nscn; i++)
        if (v->arc[i] == tag && v->start[i] == pos && v->size[i] == n) { if (getenv("VDBG")) fprintf(stderr, "[voice] scene %s -> %d at %04X:%04X\n", v->name[i], v->map[i], seg, off); v->cur = v->map[i]; v->seg = seg; v->off = off; v->last = -1; return; }
}
static int fetch_cb(CPU *c, int idx) {
    (void)idx; Voice *v = c->m->voice;
    if (!v || v->cur < 0 || c->seg[DS] != v->seg) return 0;
    uint16_t o = (uint16_t)(c->r[SI] - v->off);
    int lo = 0, hi = VM_NLINES - 1;      /* VM_LINES is sorted by (scene, off) */
    while (lo <= hi) {
        int mid = (lo + hi) / 2; const VoiceLine *l = &VM_LINES[mid];
        int d = l->scene != v->cur ? (int)l->scene - v->cur : (int)l->off - (int)o;
        if (d == 0) { if (mid != v->last) { v->last = mid; v->pending = l->voice; v->fired++; } return 0; }
        if (d < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}
/* mov al,[si] ; cmp al,0 ; jz +2E ; cmp al,1 ; jz +2E ; cmp al,4 ; jz */
static const uint8_t SIG_FETCH[] = { 0x8A, 0x04, 0x3C, 0x00, 0x74, 0x2E, 0x3C, 0x01, 0x74, 0x2E, 0x3C, 0x04, 0x74 };
static void v_on_load(Machine *m, uint16_t load_seg) {
    CPU *c = &m->cpu; uint32_t base = LIN(load_seg, 0);
    if (!m->voice) return;
    for (uint32_t o = 0; o < 0xF000; o++) {
        if (m->mem[base + o] != 0x8A || memcmp(m->mem + base + o, SIG_FETCH, sizeof SIG_FETCH)) continue;
        if (c->nbp < 8) { c->bp[c->nbp] = base + o; c->bp_fn[c->nbp] = fetch_cb; c->nbp++; }
        if (getenv("VDBG")) fprintf(stderr, "[voice] fetch hook at %04X:%04X\n", (unsigned)load_seg, (unsigned)o);
        break;
    }
}

int voice_init(Machine *m) {
    Voice *v = calloc(1, sizeof *v); if (!v) return 0;
    char p[300]; snprintf(p, sizeof p, "%s/SAGWA.DAT", m->gamedir);
    if (!sagwa_dir(v, p, 1, "")) { snprintf(p, sizeof p, "%s/sagwa.dat", m->gamedir); if (!sagwa_dir(v, p, 1, "")) { free(v); return 0; } }
    snprintf(p, sizeof p, "%s/ADULT.DAT", m->gamedir);
    if (!sagwa_dir(v, p, 2, "A_")) { snprintf(p, sizeof p, "%s/adult.dat", m->gamedir); sagwa_dir(v, p, 2, "A_"); }
    for (int i = 0; i < v->nscn; i++) {
        v->map[i] = -1;
        for (int k = 0; k < VM_NSCENES; k++) if (!strcmp(v->name[i], VM_SCENES[k])) v->map[i] = (int8_t)k;
    }
    v->cur = -1; v->last = -1;
    m->voice = v; m->on_open = v_open; m->on_read = v_read; m->on_load3 = v_on_load;
    return 1;
}
void voice_free(Machine *m) { free(m->voice); m->voice = NULL; }
const char *voice_take(Machine *m) { Voice *v = m->voice; if (!v || !v->pending) return NULL; const char *r = v->pending; v->pending = NULL; return r; }
unsigned voice_fired(Machine *m) { return m->voice ? m->voice->fired : 0; }
