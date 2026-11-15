#ifndef X86_H
#define X86_H
#include <stdint.h>

typedef struct Machine Machine;

enum { AX, CX, DX, BX, SP, BP, SI, DI };
enum { ES, CS, SS, DS, FS, GS };

typedef struct CPU {
    uint16_t r[8];
    uint16_t rh[8];       /* upper 16 bits of E?? registers (386) */
    uint16_t seg[6];
    uint16_t ip;
    uint8_t cf, pf, af, zf, sf, tf, iff, df, of;
    uint8_t halted;
    uint8_t inhibit;      /* interrupt shadow after STI / MOV SS */
    int8_t  segov;        /* segment override or -1 */
    uint8_t osz32, asz32; /* 0x66 / 0x67 prefixes for the current instruction */
    uint8_t mod, reg, rm;
    uint8_t ea_seg;
    uint16_t ea_off;
    uint32_t ea32;
    Machine *m;
    /* simple breakpoint hooks (linear addresses) */
    uint32_t bp[8];
    int nbp;
    int (*bp_cb)(struct CPU *, int idx);
} CPU;

#define R8(c, i) (((i) & 4) ? ((uint8_t *)&(c)->r[(i) & 3])[1] : ((uint8_t *)&(c)->r[(i)])[0])
#define SETR8(c, i, v) do { if ((i) & 4) ((uint8_t *)&(c)->r[(i) & 3])[1] = (v); else ((uint8_t *)&(c)->r[(i)])[0] = (v); } while (0)

void cpu_reset(CPU *c, Machine *m);
int  cpu_step(CPU *c);                 /* returns number of "instructions" consumed */
void cpu_int(CPU *c, int n);           /* push frame and jump through IVT */
void cpu_iret(CPU *c);                 /* pop ip, cs, flags */
uint16_t cpu_get_flags(CPU *c);
void cpu_set_flags(CPU *c, uint16_t f);
#define LIN(seg, off) ((((uint32_t)(seg) << 4) + (uint16_t)(off)) & 0xFFFFF)
#endif
