/* OPL2 / OPL3 (YM3812 / YMF262) FM synthesizer + timers.
 *
 * Output is generated directly at the host rate (default 22050 Hz) in lock-step with the emulated
 * machine's virtual time: every register write first renders samples up to "now", so the sound
 * stays aligned with the game no matter how fast the host actually runs.
 *
 * Operator maths (log-sine / exp tables, envelope units, key scaling, feedback, 4-op algorithms)
 * follow the documented behaviour of the YMF262; envelopes use simple time-based rate tables
 * instead of the chip's bit-exact counter. */
#include "machine.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#define RING_FRAMES 16384            /* power of two */

enum { ST_OFF = 0, ST_ATTACK, ST_DECAY, ST_SUSTAIN, ST_RELEASE };

typedef struct {
    uint8_t am, vib, egt, ksr, mult, ksl, tl, ar, dr, sl, rr, wf;
    uint32_t phase;
    int32_t env;                      /* attenuation, 16.16 fixed point, 0 .. 511<<16 */
    uint8_t state, key;
    int out, prout;
} Op;

typedef struct {
    uint16_t fnum; uint8_t block, kon, fb, cnt, cha, chb;
    uint8_t is4op;                    /* this is the first channel of an active 4-op pair */
    uint8_t second;                   /* this is the second channel of an active 4-op pair */
} Chan;

struct OPL {
    int rate;
    opl_clock_fn now_ns; void *clk_ctx;
    /* registers kept for timers & misc */
    uint8_t regs[512];
    uint8_t idx[2];
    uint8_t status;
    uint64_t t1_start, t2_start; int t1_run, t2_run;
    /* synthesis */
    Op op[36]; Chan ch[18];
    uint8_t newmode, nts, dam, dvb, rhy, conn4;
    uint32_t vibpos_acc, trem_acc; int vibpos, trempos;
    uint64_t gen_samples;
    /* ring */
    int16_t ring[RING_FRAMES * 2]; uint32_t rd, wr;
    /* derived tables */
    double chip_ratio;                 /* 49716 / rate */
    uint32_t ratio_q16;
    int32_t dec_step[64];              /* per-sample decay step (16.16) by effective rate */
    int32_t att_alpha[64];             /* per-sample attack coefficient (0.16) */
};

/* ------------------------------------------------------------------ tables */
static uint16_t logsin[256], exptab[256];
static int tables_ready;
static const uint8_t mult_x2[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
static const uint8_t kslrom[16] = {0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64};
static const uint8_t kslshift[4] = {8, 1, 2, 0};
static const int8_t slot_of[32] = {0, 1, 2, 3, 4, 5, -1, -1, 6, 7, 8, 9, 10, 11, -1, -1, 12, 13, 14, 15, 16, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

static void init_tables(void) {
    if (tables_ready) return;
    for (int i = 0; i < 256; i++) {
        logsin[i] = (uint16_t)lround(-log2(sin((i + 0.5) * M_PI / 512.0)) * 256.0);
        exptab[i] = (uint16_t)lround(pow(2.0, (255 - i) / 256.0) * 1024.0);
    }
    tables_ready = 1;
}

static inline int exp_out(int level) {
    if (level > 0x1FFF) level = 0x1FFF;
    return (exptab[level & 0xFF] << 1) >> (level >> 8);
}

/* waveform: returns signed amplitude for a 10-bit phase and an attenuation (in 1/256 octave units) */
static int wave(int wf, int ph, int att) {
    ph &= 0x3FF;
    int neg = 0, lv;
    switch (wf) {
    case 0: neg = ph & 0x200; lv = logsin[(ph & 0x100) ? ((ph & 0xFF) ^ 0xFF) : (ph & 0xFF)]; break;
    case 1: if (ph & 0x200) return 0; lv = logsin[(ph & 0x100) ? ((ph & 0xFF) ^ 0xFF) : (ph & 0xFF)]; break;
    case 2: lv = logsin[(ph & 0x100) ? ((ph & 0xFF) ^ 0xFF) : (ph & 0xFF)]; break;
    case 3: if (ph & 0x100) return 0; lv = logsin[ph & 0xFF]; break;
    case 4: if (ph & 0x200) return 0; neg = (ph & 0x300) == 0x100;
            lv = logsin[(ph & 0x80) ? (((ph ^ 0xFF) << 1) & 0xFF) : ((ph << 1) & 0xFF)]; break;
    case 5: if (ph & 0x200) return 0;
            lv = logsin[(ph & 0x80) ? (((ph ^ 0xFF) << 1) & 0xFF) : ((ph << 1) & 0xFF)]; break;
    case 6: neg = ph & 0x200; lv = 0; break;
    default: { int p = ph; if (p & 0x200) { neg = 1; p = (p & 0x1FF) ^ 0x1FF; } lv = p << 3; break; }
    }
    int v = exp_out(lv + att);
    return neg ? -v : v;
}

/* ------------------------------------------------------------------ derived rate tables */
static void build_rates(OPL *o) {
    double fs = o->rate;
    for (int r = 0; r < 64; r++) {
        if (r < 4) { o->dec_step[r] = 0; o->att_alpha[r] = 0; continue; }
        double f = pow(2.0, (r - 4) / 4.0);
        double dec_s = 39.28 / f;                          /* seconds for 0 -> 96 dB */
        double att_s = 2.82624 / f;
        o->dec_step[r] = (int32_t)((512.0 * 65536.0) / (dec_s * fs));
        if (o->dec_step[r] < 1) o->dec_step[r] = 1;
        double tau = att_s / 6.24;
        o->att_alpha[r] = (int32_t)((1.0 - exp(-1.0 / (tau * fs))) * 65536.0);
        if (o->att_alpha[r] > 65535) o->att_alpha[r] = 65535;
    }
}

/* ------------------------------------------------------------------ channel / operator helpers */
static inline int ch_op(int ch, int which) {            /* operator index of channel's op `which` (0/1) */
    int bank = ch / 9, n = ch % 9, grp = n / 3, pos = n % 3;
    return bank * 18 + grp * 6 + pos + which * 3;
}

static int eff_rate(const Op *p, const Chan *c, int nts, int reg_rate) {
    if (!reg_rate) return 0;
    int ksv = (c->block << 1) | ((c->fnum >> (nts ? 8 : 9)) & 1);
    int rate = (reg_rate << 2) + (ksv >> (p->ksr ? 0 : 2));
    return rate > 63 ? 63 : rate;
}

static int ksl_att(const Op *p, const Chan *c) {
    int v = (kslrom[c->fnum >> 6] << 2) - ((8 - c->block) << 5);
    if (v < 0) v = 0;
    return v >> kslshift[p->ksl];
}

static void key_on(OPL *o, int ch) {
    Chan *c = &o->ch[ch];
    for (int w = 0; w < 2; w++) {
        Op *p = &o->op[ch_op(ch, w)];
        p->key = 1; p->state = ST_ATTACK; p->phase = 0;
        if (eff_rate(p, c, o->nts, p->ar) >= 60) p->env = 0;
    }
}
static void key_off(OPL *o, int ch) {
    for (int w = 0; w < 2; w++) {
        Op *p = &o->op[ch_op(ch, w)];
        p->key = 0; if (p->state != ST_OFF) p->state = ST_RELEASE;
    }
}

static void env_step(OPL *o, Op *p, const Chan *c) {
    int sl = p->sl == 15 ? 31 : p->sl;
    switch (p->state) {
    case ST_ATTACK: {
        int r = eff_rate(p, c, o->nts, p->ar);
        if (r >= 60) { p->env = 0; p->state = ST_DECAY; break; }
        if (r) { int64_t d = ((int64_t)p->env * o->att_alpha[r]) >> 16; p->env -= (int32_t)(d < 4096 ? 4096 : d); }
        if (p->env <= (1 << 16)) { p->env = 0; p->state = ST_DECAY; }
        break; }
    case ST_DECAY: {
        if ((p->env >> 20) >= sl) { p->state = ST_SUSTAIN; break; }
        p->env += o->dec_step[eff_rate(p, c, o->nts, p->dr)];
        break; }
    case ST_SUSTAIN:
        if (!p->egt) p->env += o->dec_step[eff_rate(p, c, o->nts, p->rr)];
        break;
    case ST_RELEASE:
        p->env += o->dec_step[eff_rate(p, c, o->nts, p->rr)];
        break;
    default: break;
    }
    if (p->env >= (511 << 16)) { p->env = 511 << 16; if (p->state == ST_RELEASE || p->state == ST_SUSTAIN) p->state = p->key ? p->state : ST_OFF; }
}

/* ------------------------------------------------------------------ per-sample synthesis */
typedef struct { int vib_range_shift; int trem; } Lfo;

static inline void phase_adv(OPL *o, Op *p, const Chan *c, int vibrato_pos) {
    int fnum = c->fnum;
    if (p->vib) {
        int range = (fnum >> 7) & 7;
        if (!(vibrato_pos & 3)) range = 0; else if (vibrato_pos & 1) range >>= 1;
        range >>= (o->dvb ? 0 : 1);
        if (vibrato_pos & 4) range = -range;
        fnum += range;
    }
    uint64_t base = (((uint64_t)fnum << c->block) >> 1) * mult_x2[p->mult] >> 1;
    p->phase += (uint32_t)((base * o->ratio_q16 * 8192ull) >> 16);
}

static inline int op_eval(OPL *o, Op *p, const Chan *c, int mod, int trem) {
    int e = (p->env >> 16) + (p->tl << 2) + ksl_att(p, c) + (p->am ? trem : 0);
    if (e > 0x1FF) e = 0x1FF;
    int wf = o->newmode ? p->wf : (p->wf & 3);
    return wave(wf, (int)(p->phase >> 22) + mod, e << 3);
}

static void gen_sample(OPL *o, int *outl, int *outr) {
    /* LFOs advance in chip-rate units */
    o->vibpos_acc += (uint32_t)(o->chip_ratio * 256.0);
    while (o->vibpos_acc >= 1024u * 256u) { o->vibpos_acc -= 1024u * 256u; o->vibpos = (o->vibpos + 1) & 7; }
    o->trem_acc += (uint32_t)(o->chip_ratio * 256.0);
    while (o->trem_acc >= 64u * 256u) { o->trem_acc -= 64u * 256u; o->trempos = (o->trempos + 1) % 210; }
    int tshift = o->dam ? 2 : 4;
    int trem = (o->trempos < 105 ? o->trempos : 210 - o->trempos) >> tshift;

    int L = 0, R = 0;
    int nch = o->newmode ? 18 : 9;
    /* envelopes + phases first (every operator advances even if silent) */
    for (int ch = 0; ch < nch; ch++) {
        Chan *c = o->ch[ch].second ? &o->ch[ch - 3] : &o->ch[ch];     /* 4-op: all operators follow the first channel */
        for (int w = 0; w < 2; w++) {
            Op *p = &o->op[ch_op(ch, w)];
            env_step(o, p, c);
            phase_adv(o, p, c, o->vibpos);
        }
    }
    for (int ch = 0; ch < nch; ch++) {
        Chan *c = &o->ch[ch];
        if (c->second) continue;                         /* handled with its first channel */
        Op *a = &o->op[ch_op(ch, 0)], *b = &o->op[ch_op(ch, 1)];
        int fbm = c->fb ? (a->out + a->prout) >> (9 - c->fb) : 0;
        int out;
        if (c->is4op) {
            Chan *c2 = &o->ch[ch + 3];
            Op *c3 = &o->op[ch_op(ch + 3, 0)], *d = &o->op[ch_op(ch + 3, 1)];
            int alg = (c->cnt << 1) | c2->cnt; c2 = c;                      /* operators of the pair use the first channel's pitch */
            a->prout = a->out; a->out = op_eval(o, a, c, fbm, trem);
            switch (alg) {
            case 0: b->out = op_eval(o, b, c, a->out, trem); c3->out = op_eval(o, c3, c2, b->out, trem);
                    d->out = op_eval(o, d, c2, c3->out, trem); out = d->out; break;
            case 1: b->out = op_eval(o, b, c, a->out, trem); c3->out = op_eval(o, c3, c2, 0, trem);
                    d->out = op_eval(o, d, c2, c3->out, trem); out = b->out + d->out; break;
            case 2: b->out = op_eval(o, b, c, 0, trem); c3->out = op_eval(o, c3, c2, b->out, trem);
                    d->out = op_eval(o, d, c2, c3->out, trem); out = a->out + d->out; break;
            default: b->out = op_eval(o, b, c, 0, trem); c3->out = op_eval(o, c3, c2, b->out, trem);
                    d->out = op_eval(o, d, c2, 0, trem); out = a->out + c3->out + d->out; break;
            }
            /* stereo routing of a 4-op voice follows the first channel */
        } else {
            a->prout = a->out; a->out = op_eval(o, a, c, fbm, trem);
            if (c->cnt) { b->out = op_eval(o, b, c, 0, trem); out = a->out + b->out; }
            else { b->out = op_eval(o, b, c, a->out, trem); out = b->out; }
        }
        if (!o->newmode) { L += out; R += out; }
        else { if (c->cha) L += out; if (c->chb) R += out; }
    }
    *outl = L; *outr = R;
}

/* ------------------------------------------------------------------ ring + timing */
static uint64_t now_samples(OPL *o) {
    if (!o->now_ns) return o->gen_samples;
    return o->now_ns(o->clk_ctx) * (uint64_t)o->rate / 1000000000ull;
}

static void clamp_push(OPL *o, int l, int r) {
    l *= 2; r *= 2;
    if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
    uint32_t w = o->wr & (RING_FRAMES - 1);
    o->ring[w * 2] = (int16_t)l; o->ring[w * 2 + 1] = (int16_t)r;
    o->wr++;
    if (o->wr - o->rd > RING_FRAMES) o->rd = o->wr - RING_FRAMES;     /* overrun: drop oldest */
}

void opl_sync(OPL *o) {
    uint64_t target = now_samples(o);
    int guard = 0;
    while (o->gen_samples < target && guard++ < RING_FRAMES) {
        int l, r; gen_sample(o, &l, &r); clamp_push(o, l, r); o->gen_samples++;
    }
    if (o->gen_samples < target) o->gen_samples = target;                /* far behind: skip ahead */
}

OPL *opl_new(int rate) {
    init_tables();
    OPL *o = calloc(1, sizeof(OPL));
    o->rate = rate; o->chip_ratio = 49716.0 / rate; o->ratio_q16 = (uint32_t)(o->chip_ratio * 65536.0);
    build_rates(o);
    for (int i = 0; i < 36; i++) { o->op[i].env = 511 << 16; o->op[i].state = ST_OFF; }
    for (int i = 0; i < 18; i++) { o->ch[i].cha = o->ch[i].chb = 1; }
    return o;
}
void opl_free(OPL *o) { free(o); }
void opl_attach(OPL *o, opl_clock_fn now_ns, void *ctx) { o->now_ns = now_ns; o->clk_ctx = ctx; }

/* frames available / pull */
uint32_t opl_available(OPL *o) { return o->wr - o->rd; }
void opl_discard(OPL *o, uint32_t frames) { uint32_t a = o->wr - o->rd; o->rd += frames > a ? a : frames; }
void opl_mix(OPL *o, int16_t *out, int frames) {
    for (int i = 0; i < frames; i++) {
        if (o->rd == o->wr) { out[i * 2] = out[i * 2 + 1] = 0; continue; }
        uint32_t r = o->rd & (RING_FRAMES - 1);
        out[i * 2] = o->ring[r * 2]; out[i * 2 + 1] = o->ring[r * 2 + 1];
        o->rd++;
    }
}

/* ------------------------------------------------------------------ timers (driver detection) */
static uint64_t usec(OPL *o) { return o->now_ns ? o->now_ns(o->clk_ctx) / 1000ull : 0; }

static void timer_check(OPL *o) {
    uint64_t now = usec(o);
    if (o->t1_run && !(o->regs[4] & 0x40)) {
        uint64_t per = (uint64_t)(256 - o->regs[2]) * 80;
        if (now - o->t1_start >= per) o->status |= 0xC0;
    }
    if (o->t2_run && !(o->regs[4] & 0x20)) {
        uint64_t per = (uint64_t)(256 - o->regs[3]) * 320;
        if (now - o->t2_start >= per) o->status |= 0xA0;
    }
}

/* ------------------------------------------------------------------ register writes */
static void update_4op(OPL *o) {
    for (int i = 0; i < 18; i++) o->ch[i].is4op = o->ch[i].second = 0;
    if (!o->newmode) return;
    static const int pairs[6] = {0, 1, 2, 9, 10, 11};
    for (int k = 0; k < 6; k++) if ((o->conn4 >> k) & 1) { o->ch[pairs[k]].is4op = 1; o->ch[pairs[k] + 3].second = 1; }
}

static void write_reg(OPL *o, int bank, int reg, uint8_t v) {
    int full = bank * 256 + reg;
    o->regs[full] = v;
    if (bank == 0 && reg == 4) {
        if (v & 0x80) { o->status = 0; }
        else {
            if (v & 1) { if (!o->t1_run) { o->t1_start = usec(o); o->t1_run = 1; } } else o->t1_run = 0;
            if (v & 2) { if (!o->t2_run) { o->t2_start = usec(o); o->t2_run = 1; } } else o->t2_run = 0;
        }
        return;
    }
    if (bank == 1 && reg == 5) { o->newmode = v & 1; update_4op(o); return; }
    if (bank == 1 && reg == 4) { o->conn4 = v & 0x3F; update_4op(o); return; }
    if (bank == 0 && reg == 8) { o->nts = (v >> 6) & 1; return; }
    if (reg == 0xBD && bank == 0) {
        o->dam = (v >> 7) & 1; o->dvb = (v >> 6) & 1; o->rhy = v;
        return;
    }
    int grp = reg & 0xE0;
    if ((grp == 0x20 || grp == 0x40 || grp == 0x60 || grp == 0x80 || grp == 0xE0) && (reg & 0x1F) < 0x16) {
        int s = slot_of[reg & 0x1F];
        if (s < 0) return;
        Op *p = &o->op[bank * 18 + s];
        switch (grp) {
        case 0x20: p->am = (v >> 7) & 1; p->vib = (v >> 6) & 1; p->egt = (v >> 5) & 1; p->ksr = (v >> 4) & 1; p->mult = v & 15; break;
        case 0x40: p->ksl = v >> 6; p->tl = v & 0x3F; break;
        case 0x60: p->ar = v >> 4; p->dr = v & 15; break;
        case 0x80: p->sl = v >> 4; p->rr = v & 15; break;
        case 0xE0: p->wf = v & 7; break;
        }
        return;
    }
    if (reg >= 0xA0 && reg <= 0xA8) { int ch = bank * 9 + (reg - 0xA0); o->ch[ch].fnum = (o->ch[ch].fnum & 0x300) | v; return; }
    if (reg >= 0xB0 && reg <= 0xB8) {
        int ch = bank * 9 + (reg - 0xB0); Chan *c = &o->ch[ch];
        c->fnum = (c->fnum & 0xFF) | ((v & 3) << 8); c->block = (v >> 2) & 7;
        int kon = (v >> 5) & 1;
        if (c->second) { c->kon = kon; return; }                         /* 4-op: first channel's key bit rules */
        if (kon && !c->kon) { key_on(o, ch); if (c->is4op) key_on(o, ch + 3); }
        else if (!kon && c->kon) { key_off(o, ch); if (c->is4op) key_off(o, ch + 3); }
        c->kon = kon;
        return;
    }
    if (reg >= 0xC0 && reg <= 0xC8) {
        int ch = bank * 9 + (reg - 0xC0); Chan *c = &o->ch[ch];
        c->fb = (v >> 1) & 7; c->cnt = v & 1;
        c->cha = (v >> 4) & 1; c->chb = (v >> 5) & 1;
        return;
    }
}

long opl_dbg_writes; int opl_dbg_log;
void opl_write(OPL *o, uint16_t port, uint8_t v) {
    int hi = (port & 2) ? 1 : 0;
    if (opl_dbg_log && (port & 1) && opl_dbg_writes < opl_dbg_log) fprintf(stderr, "[opl] %s reg %02X = %02X  (t=%llu)\n", hi ? "B2" : "B1", o->idx[hi], v, (unsigned long long)usec(o));
    if (port & 1) opl_dbg_writes++;
    if (!(port & 1)) { o->idx[hi] = v; return; }
    opl_sync(o);
    int bank = hi;
    int reg = o->idx[hi];
    if (!o->newmode && hi == 1 && reg != 5) { /* second bank is only reachable after OPL3 enable, but allow writing 0x105 */ }
    write_reg(o, bank, reg, v);
}

uint8_t opl_read(OPL *o, uint16_t port) {
    if (port & 1) return 0xFF;
    timer_check(o);
    return o->status;
}
