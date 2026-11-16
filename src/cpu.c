/* cpu.c — the R4300i core.
 *
 * M0: integer ALU, shifts, multiplies/divides, branches with delay slots, jumps,
 *     unaligned loads/stores, COP0 register moves, synchronous exceptions.
 * M1: COP1 (single/double FPU, converts, compares, branches), TLB instructions
 *     and 4 KiB address translation, exceptions through the BEV/EXL vectors.
 *
 * Still absent, and halting loudly rather than guessing: COP2 (the RSP), the 64-bit
 * unaligned pairs (LDL/LDR/SDL/SDR), LL/SC reservations, and the FPU exception
 * model (flags in FCR31 are only the compare bit).
 *
 * Two conventions the architected behaviour depends on:
 *   1. 32-bit results are sign-extended into the 64-bit register (MIPS III).
 *   2. Branches execute their delay slot before transferring control.
 */
#include "n64.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define RS ((ins >> 21) & 31u)
#define RT ((ins >> 16) & 31u)
#define RD ((ins >> 11) & 31u)
#define SA ((ins >> 6) & 31u)
#define FN (ins & 63u)
#define IMM (ins & 0xFFFFu)
#define SIMM ((int32_t)(int16_t)(ins & 0xFFFFu))
#define OP (ins >> 26)
#define JUMP_TARGET (((pc + 4) & 0xF0000000u) | ((ins & 0x03FFFFFFu) << 2))

enum { EXC_TLBMISS = 2, EXC_OV = 12, EXC_SYS = 8, EXC_BP = 9, EXC_RI = 10 };

static void set32(Cpu *c, unsigned r, uint32_t v) {
    if (r) c->gpr[r] = (uint64_t)(int64_t)(int32_t)v; /* MIPS III: sign-extend 32-bit results */
}

static void set64(Cpu *c, unsigned r, uint64_t v) {
    if (r) c->gpr[r] = v;
}

void cpu_halt(Cpu *c, const char *why) {
    c->halted = 1;
    snprintf(c->halt_reason, sizeof c->halt_reason, "%s", why);
}

void cpu_reset(Cpu *c, uint32_t entry) {
    memset(c, 0, sizeof *c);
    c->pc = entry;
    /* Status: CU0 on, BEV off, which is the state game code runs in after IPL3.
     * A real reset has BEV set; the boot path that clears it arrives with the
     * boot ROM, which is deliberately not in this tree. */
    c->cop0[12] = 0x04000000u;
    c->tlb_random = 31;
}

static void branch(Cpu *c, uint32_t target) {
    c->branch_target = target;
    c->delay = 1;
}

/* vector == 0 means the general exception vector; a non-zero vector overrides
 * it (the TLB refill vector at 0x80000000). */
static void exception_at(Cpu *c, uint32_t code, uint32_t pc, uint32_t vector) {
    c->cop0[13] = (c->cop0[13] & ~0x7Cu) | (code << 2);
    c->cop0[14] = pc; /* EPC */
    uint32_t status = c->cop0[12];
    int bev = (int)((status >> 22) & 1u);
    c->cop0[12] = status | 2u; /* EXL */
    c->override_pending = 1;
    c->override_target = vector ? vector : (bev ? 0xBFC00200u : 0x80000180u);
    c->delay = 0;
}

#define exception(c, code, pc) exception_at((c), (code), (pc), 0)

static void unimplemented(Cpu *c, uint32_t pc, uint32_t ins, const char *what) {
    char buf[96];
    snprintf(buf, sizeof buf, "unimplemented %s at %08x (ins %08x)", what, pc, ins);
    cpu_halt(c, buf);
}

/* ------------------------------------------------------------ address map */

/* 4 KiB pages, no page-mask pairs, no ASID: enough for bootcode, which maps a
 * handful of pages with TLBWI before it enables them. Returns 0 after raising
 * the exception, so the caller performs no access. */
static int translate(Cpu *c, uint32_t va, uint32_t pc, uint32_t *pa) {
    if (va < 0x80000000u) {
        for (int i = 0; i < TLB_ENTRIES; i++) {
            if (!c->tlb_valid[i]) continue;
            if ((va & 0xFFFFF000u) != (c->tlb_ehi[i] & 0xFFFFF000u)) continue;
            uint32_t odd = (va >> 12) & 1u;
            uint32_t elo = odd ? c->tlb_elo1[i] : c->tlb_elo0[i];
            if (!(elo & 2u)) continue; /* V */
            *pa = (elo & 0x3FFFF000u) | (va & 0xFFFu);
            c->tlb_hits++;
            return 1;
        }
        c->tlb_misses++;
        int exl = (int)((c->cop0[12] >> 1) & 1u);
        exception_at(c, EXC_TLBMISS, pc, exl ? 0 : 0x80000000u);
        return 0;
    }
    if (va < 0xA0000000u) { /* kseg0 */
        *pa = va - 0x80000000u;
        return 1;
    }
    if (va < 0xC0000000u) { /* kseg1: uncached mirror */
        *pa = va - 0xA0000000u;
        return 1;
    }
    *pa = va; /* kseg2/3: direct for now */
    return 1;
}

/* ------------------------------------------------------------ unaligned */

/* LWL/LWR merge the bytes around a word into rt, SWL/SWR write halves back out.
 * Paired, they implement unaligned access: lwl rt,0(base) ; lwr rt,3(base). */
static void op_lwl(Cpu *c, Bus *b, uint32_t a, unsigned rt) {
    uint32_t aligned = a & ~3u, n = a & 3u;
    uint64_t v = 0;
    for (uint32_t j = 0; j + n < 4; j++) v = (v << 8) | bus_read8(b, aligned + n + j);
    uint64_t mask = n ? ((1ull << (8 * n)) - 1) : 0;
    set32(c, rt, (uint32_t)((v << (8 * n)) | (c->gpr[rt] & mask)));
}

static void op_lwr(Cpu *c, Bus *b, uint32_t a, unsigned rt) {
    uint32_t aligned = a & ~3u, n = a & 3u;
    uint64_t v = 0;
    for (uint32_t j = 0; j <= n; j++) v = (v << 8) | bus_read8(b, aligned + j);
    uint64_t keep = ~((1ull << (8 * (n + 1))) - 1);
    set32(c, rt, (uint32_t)((c->gpr[rt] & keep) | v));
}

static void op_swl(Cpu *c, Bus *b, uint32_t a, unsigned rt) {
    uint32_t aligned = a & ~3u, n = a & 3u;
    uint32_t v = (uint32_t)c->gpr[rt];
    for (uint32_t i = n; i < 4; i++) bus_write8(b, aligned + i, (uint8_t)(v >> (8 * (3 - (i - n)))));
}

static void op_swr(Cpu *c, Bus *b, uint32_t a, unsigned rt) {
    uint32_t aligned = a & ~3u, n = a & 3u;
    uint32_t v = (uint32_t)c->gpr[rt];
    for (uint32_t i = 0; i <= n; i++) bus_write8(b, aligned + i, (uint8_t)(v >> (8 * (n - i))));
}

/* ------------------------------------------------------------ COP1 (FPU) */

static double fpr_d(const Cpu *c, unsigned r) {
    double d;
    memcpy(&d, &c->fpr[r], sizeof d);
    return d;
}

static void fpr_set_d(Cpu *c, unsigned r, double d) {
    memcpy(&c->fpr[r], &d, sizeof d);
}

static float fpr_s(const Cpu *c, unsigned r) {
    uint32_t lo = (uint32_t)c->fpr[r];
    float f;
    memcpy(&f, &lo, sizeof f);
    return f;
}

static void fpr_set_s(Cpu *c, unsigned r, float f) {
    uint32_t lo;
    memcpy(&lo, &f, sizeof lo);
    c->fpr[r] = lo; /* upper half unused: MFC1 reads the low 32 bits */
}

/* Value of an FPR in the given format, as a double (formats W/L are integers). */
static double fpr_value(const Cpu *c, unsigned fmt, unsigned r) {
    switch (fmt) {
    case 16: return (double)fpr_s(c, r);
    case 17: return fpr_d(c, r);
    case 20: return (double)(int32_t)c->fpr[r];
    case 21: return (double)(int64_t)c->fpr[r];
    default: return 0.0;
    }
}

static int64_t round_to_int(double v, unsigned mode) {
    switch (mode & 3u) {
    case 0: return (int64_t)llround(v); /* round to nearest, ties away from zero */
    case 1: return (int64_t)v;          /* truncate */
    case 2: return (int64_t)ceil(v);
    default: return (int64_t)floor(v);
    }
}

static void fpu_compare(Cpu *c, unsigned fmt, unsigned fs, unsigned ft, unsigned fn) {
    double a = fpr_value(c, fmt, fs), b = fpr_value(c, fmt, ft);
    int an = isnan(a), bn = isnan(b), res;
    switch (fn) {
    case 48: res = 0; break;                              /* C.F */
    case 49: res = an || bn; break;                       /* C.UN */
    case 50: case 58: res = !an && !bn && a == b; break;  /* C.EQ / C.SEQ */
    case 51: res = an || bn || a == b; break;             /* C.UEQ */
    case 52: case 60: res = !an && !bn && a < b; break;   /* C.OLT / C.LT */
    case 53: res = an || bn || a < b; break;              /* C.ULT */
    case 54: case 62: res = !an && !bn && a <= b; break;  /* C.OLE / C.LE */
    case 55: res = an || bn || a <= b; break;             /* C.ULE */
    case 56: res = 0; break;                              /* C.SF */
    case 57: res = an || bn || a <= b; break;             /* C.NGLE */
    case 59: res = an || bn || a != b; break;             /* C.NGL */
    case 61: res = an || bn || !(a < b); break;           /* C.NGE */
    case 63: res = an || bn || !(a <= b); break;          /* C.NGT */
    default: res = 0; break;
    }
    if (res) c->fcr31 |= (1u << 23);
    else c->fcr31 &= ~(1u << 23);
}

static void fpu_arith(Cpu *c, uint32_t pc, uint32_t ins) {
    unsigned fmt = (ins >> 21) & 31u, ft = (ins >> 16) & 31u, fs = (ins >> 11) & 31u,
             fd = (ins >> 6) & 31u, fn = ins & 63u;

    if (fn >= 48) { /* compares, including the un-ordered variants */
        fpu_compare(c, fmt, fs, ft, fn);
        return;
    }
    if (fmt == 20 || fmt == 21) { /* W/L only take conversions */
        if (fn != 32 && fn != 33) { unimplemented(c, pc, ins, "FPU op on integer format"); return; }
    }

    double a = fpr_value(c, fmt, fs), b = fpr_value(c, fmt, ft);
    switch (fn) {
    case 0x00: /* ADD */
    case 0x01: /* SUB */
    case 0x02: /* MUL */
    case 0x03: /* DIV */
    case 0x04: { /* SQRT */
        double r;
        if (fn == 0x00) r = a + b;
        else if (fn == 0x01) r = a - b;
        else if (fn == 0x02) r = a * b;
        else if (fn == 0x03) r = a / b;
        else r = sqrt(a);
        if (fmt == 16) fpr_set_s(c, fd, (float)r);
        else if (fmt == 17) fpr_set_d(c, fd, r);
        else unimplemented(c, pc, ins, "FPU arithmetic format");
        break;
    }
    case 0x05: /* ABS */
        if (fmt == 16) fpr_set_s(c, fd, (float)fabs(a));
        else fpr_set_d(c, fd, fabs(a));
        break;
    case 0x06: /* MOV */
        if (fmt == 16) fpr_set_s(c, fd, (float)a);
        else fpr_set_d(c, fd, a);
        break;
    case 0x07: /* NEG */
        if (fmt == 16) fpr_set_s(c, fd, -a);
        else fpr_set_d(c, fd, -a);
        break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: { /* ROUND/TRUNC/CEIL/FLOOR .L */
        unsigned mode = c->fcr31 & 3u;
        if (fn == 0x08) mode = 0;
        else if (fn == 0x09) mode = 1;
        else if (fn == 0x0A) mode = 2;
        else mode = 3;
        c->fpr[fd] = (uint64_t)round_to_int(a, mode);
        break;
    }
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: { /* .W variants, sign-extended */
        unsigned mode = c->fcr31 & 3u;
        if (fn == 0x0C) mode = 0;
        else if (fn == 0x0D) mode = 1;
        else if (fn == 0x0E) mode = 2;
        else mode = 3;
        int32_t v = (int32_t)round_to_int(a, mode);
        c->fpr[fd] = (uint64_t)(int64_t)v;
        break;
    }
    case 0x20: /* CVT.S */
        fpr_set_s(c, fd, (float)a);
        break;
    case 0x21: /* CVT.D */
        fpr_set_d(c, fd, a);
        break;
    case 0x24: { /* CVT.W */
        int32_t v = (int32_t)round_to_int(a, c->fcr31 & 3u);
        c->fpr[fd] = (uint64_t)(int64_t)v;
        break;
    }
    case 0x25: /* CVT.L */
        c->fpr[fd] = (uint64_t)round_to_int(a, c->fcr31 & 3u);
        break;
    default: unimplemented(c, pc, ins, "FPU funct"); break;
    }
}

static void op_cop1(Cpu *c, uint32_t pc, uint32_t ins) {
    unsigned sub = (ins >> 21) & 31u, ft = (ins >> 16) & 31u, fs = (ins >> 11) & 31u;
    switch (sub) {
    case 0x00: set32(c, ft, (uint32_t)c->fpr[fs]); break;   /* MFC1 */
    case 0x01: set64(c, ft, c->fpr[fs]); break;              /* DMFC1 */
    case 0x02: set32(c, ft, c->fcr31); break;                /* CFC1 */
    case 0x04: c->fpr[fs] = (uint32_t)c->gpr[ft]; break;      /* MTC1 */
    case 0x05: c->fpr[fs] = c->gpr[ft]; break;                /* DMTC1 */
    case 0x06: c->fcr31 = (uint32_t)c->gpr[ft]; break;        /* CTC1 */
    case 0x08: { /* BC1F / BC1T and their likely forms: bit 0 of ft is the "true" flag */
        int taken = (int)((c->fcr31 >> 23) & 1u);
        if (!(ft & 1u)) taken = !taken;
        if (taken) branch(c, pc + 4 + (uint32_t)(SIMM << 2));
        break;
    }
    case 16: case 17: case 20: case 21:
        fpu_arith(c, pc, ins);
        break;
    default: unimplemented(c, pc, ins, "COP1 sub-op"); break;
    }
}

/* ------------------------------------------------------------ COP0 / TLB */

static void op_cop0(Cpu *c, uint32_t pc, uint32_t ins) {
    unsigned rs = (ins >> 21) & 31u, rt = (ins >> 16) & 31u, rd = (ins >> 11) & 31u;
    if (rs == 0x10) { /* CO: TLB operations and ERET, no delay slot */
        switch (ins & 63u) {
        case 0x01: { /* TLBR */
            uint32_t i = c->cop0[0] & 31u;
            c->cop0[10] = c->tlb_ehi[i];
            c->cop0[2] = c->tlb_elo0[i];
            c->cop0[3] = c->tlb_elo1[i];
            c->cop0[5] = c->tlb_pagemask[i];
            break;
        }
        case 0x02: { /* TLBWI */
            uint32_t i = c->cop0[0] & 31u;
            c->tlb_ehi[i] = c->cop0[10];
            c->tlb_elo0[i] = c->cop0[2];
            c->tlb_elo1[i] = c->cop0[3];
            c->tlb_pagemask[i] = c->cop0[5];
            c->tlb_valid[i] = 1;
            break;
        }
        case 0x06: { /* TLBWR: the random slot, which counts down */
            uint32_t i = c->tlb_random & 31u;
            c->tlb_ehi[i] = c->cop0[10];
            c->tlb_elo0[i] = c->cop0[2];
            c->tlb_elo1[i] = c->cop0[3];
            c->tlb_pagemask[i] = c->cop0[5];
            c->tlb_valid[i] = 1;
            c->tlb_random = (i <= 8) ? 31u : i - 1u;
            c->cop0[1] = c->tlb_random;
            break;
        }
        case 0x08: { /* TLBP */
            uint32_t want = c->cop0[10] & 0xFFFFF000u;
            c->cop0[0] = 0x80000000u;
            for (uint32_t i = 0; i < TLB_ENTRIES; i++)
                if (c->tlb_valid[i] && (c->tlb_ehi[i] & 0xFFFFF000u) == want) {
                    c->cop0[0] = i;
                    break;
                }
            break;
        }
        case 0x18: { /* ERET */
            int erl = (int)((c->cop0[12] >> 2) & 1u);
            uint32_t target = erl ? c->cop0[30] : c->cop0[14];
            if (erl) c->cop0[12] &= ~4u;
            else c->cop0[12] &= ~2u;
            c->override_pending = 1;
            c->override_target = target;
            c->delay = 0;
            break;
        }
        default: unimplemented(c, pc, ins, "COP0 CO operation"); break;
        }
        return;
    }
    switch (rs) {
    case 0x00: set64(c, rt, c->cop0[rd]); break;          /* MFC0 */
    case 0x04: c->cop0[rd] = (uint32_t)c->gpr[rt]; break;  /* MTC0 */
    default: unimplemented(c, pc, ins, "COP0 op"); break;
    }
}

/* ------------------------------------------------------------ SPECIAL */

static void op_special(Cpu *c, Bus *b, uint32_t pc, uint32_t ins) {
    uint64_t a = c->gpr[RS], d = c->gpr[RT];
    unsigned sh = (unsigned)(c->gpr[RS] & 31u);
    (void)b;
    switch (FN) {
    case 0x00: set32(c, RD, (uint32_t)d << SA); break;                        /* SLL */
    case 0x02: set32(c, RD, (uint32_t)d >> SA); break;                        /* SRL */
    case 0x03: set32(c, RD, (uint32_t)((int32_t)d >> SA)); break;             /* SRA */
    case 0x04: set32(c, RD, (uint32_t)d << sh); break;                        /* SLLV */
    case 0x06: set32(c, RD, (uint32_t)d >> sh); break;                        /* SRLV */
    case 0x07: set32(c, RD, (uint32_t)((int32_t)d >> sh)); break;             /* SRAV */
    case 0x08: branch(c, (uint32_t)a); break;                                 /* JR */
    case 0x09: set64(c, RD, pc + 8); branch(c, (uint32_t)a); break;            /* JALR */
    case 0x0C: exception(c, EXC_SYS, pc); break;                              /* SYSCALL */
    case 0x0D: cpu_halt(c, "BREAK instruction"); break;                       /* BREAK */
    case 0x0F: break;                                                         /* SYNC */
    case 0x10: set64(c, RD, c->hi); break;                                    /* MFHI */
    case 0x11: c->hi = a; break;                                              /* MTHI */
    case 0x12: set64(c, RD, c->lo); break;                                    /* MFLO */
    case 0x13: c->lo = a; break;                                              /* MTLO */
    case 0x14: set64(c, RD, d << sh); break;                                  /* DSLLV */
    case 0x16: set64(c, RD, d >> sh); break;                                  /* DSRLV */
    case 0x17: set64(c, RD, (uint64_t)((int64_t)d >> sh)); break;             /* DSRAV */
    case 0x18: {                                                              /* MULT */
        int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)d;
        c->lo = (uint32_t)p;
        c->hi = (uint32_t)((uint64_t)p >> 32);
        break;
    }
    case 0x19: {                                                              /* MULTU */
        uint64_t p = (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)d;
        c->lo = (uint32_t)p;
        c->hi = (uint32_t)(p >> 32);
        break;
    }
    case 0x1A: {                                                              /* DIV */
        int32_t x = (int32_t)a, y = (int32_t)d;
        if (y == 0) {
            c->lo = (uint32_t)(x >= 0 ? -1 : 1);
            c->hi = (uint32_t)x;
        } else if (x == INT32_MIN && y == -1) {
            c->lo = (uint32_t)INT32_MIN;
            c->hi = 0;
        } else {
            c->lo = (uint32_t)(x / y);
            c->hi = (uint32_t)(x % y);
        }
        break;
    }
    case 0x1B: {                                                              /* DIVU */
        uint32_t x = (uint32_t)a, y = (uint32_t)d;
        if (y == 0) {
            c->lo = 0xFFFFFFFFu;
            c->hi = x;
        } else {
            c->lo = x / y;
            c->hi = x % y;
        }
        break;
    }
    case 0x1C: case 0x1D: set64(c, RD, a + d); break;                         /* DADD / DADDU */
    case 0x1E: case 0x1F: set64(c, RD, a - d); break;                         /* DSUB / DSUBU */
    case 0x20: case 0x21: {                                                   /* ADD / ADDU */
        int64_t s = (int64_t)(int32_t)a + (int64_t)(int32_t)d;
        if (FN == 0x20 && (s > INT32_MAX || s < INT32_MIN)) { exception(c, EXC_OV, pc); break; }
        set32(c, RD, (uint32_t)s);
        break;
    }
    case 0x22: case 0x23: {                                                   /* SUB / SUBU */
        int64_t s = (int64_t)(int32_t)a - (int64_t)(int32_t)d;
        if (FN == 0x22 && (s > INT32_MAX || s < INT32_MIN)) { exception(c, EXC_OV, pc); break; }
        set32(c, RD, (uint32_t)s);
        break;
    }
    case 0x24: set32(c, RD, (uint32_t)(a & d)); break;                        /* AND */
    case 0x25: set32(c, RD, (uint32_t)(a | d)); break;                        /* OR */
    case 0x26: set32(c, RD, (uint32_t)(a ^ d)); break;                        /* XOR */
    case 0x27: set32(c, RD, ~(uint32_t)(a | d)); break;                       /* NOR */
    case 0x2A: set32(c, RD, (int32_t)a < (int32_t)d ? 1u : 0u); break;        /* SLT */
    case 0x2B: set32(c, RD, (uint32_t)a < (uint32_t)d ? 1u : 0u); break;      /* SLTU */
    case 0x2C: case 0x2D: set64(c, RD, a + d); break;
    case 0x2E: case 0x2F: set64(c, RD, a - d); break;
    case 0x38: set64(c, RD, d << SA); break;                                  /* DSLL */
    case 0x3A: set64(c, RD, d >> SA); break;                                  /* DSRL */
    case 0x3B: set64(c, RD, (uint64_t)((int64_t)d >> SA)); break;             /* DSRA */
    case 0x3C: set64(c, RD, d << (SA + 32)); break;                           /* DSLL32 */
    case 0x3E: set64(c, RD, d >> (SA + 32)); break;                           /* DSRL32 */
    case 0x3F: set64(c, RD, (uint64_t)((int64_t)d >> (SA + 32))); break;      /* DSRA32 */
    default: unimplemented(c, pc, ins, "SPECIAL funct"); break;
    }
}

/* ------------------------------------------------------------ dispatch */

static void op_regimm(Cpu *c, uint32_t pc, uint32_t ins) {
    int64_t v = (int64_t)c->gpr[RS];
    uint32_t target = pc + 4 + (uint32_t)(SIMM << 2);
    switch (RT) {
    case 0x00: if (v < 0) branch(c, target); break;                          /* BLTZ */
    case 0x01: if (v >= 0) branch(c, target); break;                         /* BGEZ */
    case 0x02: if (v < 0) branch(c, target); break;                          /* BLTZL */
    case 0x03: if (v >= 0) branch(c, target); break;                         /* BGEZL */
    case 0x10: set64(c, 31, pc + 8); if (v < 0) branch(c, target); break;     /* BLTZAL */
    case 0x11: set64(c, 31, pc + 8); if (v >= 0) branch(c, target); break;    /* BGEZAL */
    default: unimplemented(c, pc, ins, "REGIMM variant"); break;
    }
}

static void op_memory(Cpu *c, Bus *b, uint32_t pc, uint32_t ins) {
    uint32_t va = (uint32_t)c->gpr[RS] + (uint32_t)SIMM;
    uint32_t pa;
    if (!translate(c, va, pc, &pa)) return; /* exception already raised */

    switch (OP) {
    case 0x20: set32(c, RT, (uint32_t)(int32_t)(int8_t)bus_read8(b, pa)); break;      /* LB */
    case 0x21: set32(c, RT, (uint32_t)(int32_t)(int16_t)bus_read16(b, pa)); break;     /* LH */
    case 0x22: op_lwl(c, b, pa, RT); break;                                            /* LWL */
    case 0x23: set32(c, RT, bus_read32(b, pa)); break;                                 /* LW */
    case 0x24: set32(c, RT, bus_read8(b, pa)); break;                                  /* LBU */
    case 0x25: set32(c, RT, bus_read16(b, pa)); break;                                 /* LHU */
    case 0x26: op_lwr(c, b, pa, RT); break;                                            /* LWR */
    case 0x27: set64(c, RT, bus_read32(b, pa)); break;                                 /* LWU */
    case 0x28: bus_write8(b, pa, (uint8_t)c->gpr[RT]); break;                          /* SB */
    case 0x29: bus_write16(b, pa, (uint16_t)c->gpr[RT]); break;                        /* SH */
    case 0x2A: op_swl(c, b, pa, RT); break;                                            /* SWL */
    case 0x2B: bus_write32(b, pa, (uint32_t)c->gpr[RT]); break;                        /* SW */
    case 0x2E: op_swr(c, b, pa, RT); break;                                            /* SWR */
    case 0x30: set32(c, RT, bus_read32(b, pa)); break;                                 /* LL (no reservation) */
    case 0x34: set64(c, RT, bus_read64(b, pa)); break;                                 /* LLD */
    case 0x37: set64(c, RT, bus_read64(b, pa)); break;                                 /* LD */
    case 0x38: bus_write32(b, pa, (uint32_t)c->gpr[RT]); set64(c, RT, 1); break;        /* SC (always succeeds) */
    case 0x3C: bus_write64(b, pa, c->gpr[RT]); set64(c, RT, 1); break;                  /* SCD */
    case 0x3F: bus_write64(b, pa, c->gpr[RT]); break;                                  /* SD */
    case 0x2C: case 0x2D: unimplemented(c, pc, ins, "SDL/SDR"); break;
    case 0x31: case 0x35: unimplemented(c, pc, ins, "LWC1/LDC1"); break;
    case 0x39: case 0x3D: unimplemented(c, pc, ins, "SWC1/SDC1"); break;
    default: unimplemented(c, pc, ins, "load/store opcode"); break;
    }
}

static void execute(Cpu *c, Bus *b, uint32_t pc, uint32_t ins) {
    switch (OP) {
    case 0x00: op_special(c, b, pc, ins); break;
    case 0x01: op_regimm(c, pc, ins); break;
    case 0x02: branch(c, JUMP_TARGET); break;                            /* J */
    case 0x03: set64(c, 31, pc + 8); branch(c, JUMP_TARGET); break;       /* JAL */
    case 0x04: if (c->gpr[RS] == c->gpr[RT]) branch(c, pc + 4 + (uint32_t)(SIMM << 2)); break;  /* BEQ */
    case 0x05: if (c->gpr[RS] != c->gpr[RT]) branch(c, pc + 4 + (uint32_t)(SIMM << 2)); break;  /* BNE */
    case 0x06: if ((int64_t)c->gpr[RS] <= 0) branch(c, pc + 4 + (uint32_t)(SIMM << 2)); break;   /* BLEZ */
    case 0x07: if ((int64_t)c->gpr[RS] > 0) branch(c, pc + 4 + (uint32_t)(SIMM << 2)); break;    /* BGTZ */
    case 0x08: { /* ADDI */
        int64_t s = (int64_t)(int32_t)c->gpr[RS] + SIMM;
        if (s > INT32_MAX || s < INT32_MIN) { exception(c, EXC_OV, pc); break; }
        set32(c, RT, (uint32_t)s);
        break;
    }
    case 0x09: set32(c, RT, (uint32_t)((int32_t)c->gpr[RS] + SIMM)); break;                     /* ADDIU */
    case 0x0A: set32(c, RT, (int32_t)c->gpr[RS] < SIMM ? 1u : 0u); break;                       /* SLTI */
    case 0x0B: set32(c, RT, (uint32_t)c->gpr[RS] < (uint32_t)SIMM ? 1u : 0u); break;            /* SLTIU */
    case 0x0C: set32(c, RT, (uint32_t)c->gpr[RS] & IMM); break;                                  /* ANDI */
    case 0x0D: set32(c, RT, (uint32_t)c->gpr[RS] | IMM); break;                                  /* ORI */
    case 0x0E: set32(c, RT, (uint32_t)c->gpr[RS] ^ IMM); break;                                  /* XORI */
    case 0x0F: set32(c, RT, (uint32_t)IMM << 16); break;                                         /* LUI */
    case 0x10: op_cop0(c, pc, ins); break;
    case 0x11: op_cop1(c, pc, ins); break;
    case 0x12: unimplemented(c, pc, ins, "COP2 (RSP)"); break;
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E:
    case 0x2F: case 0x30: case 0x31: case 0x34: case 0x35: case 0x37: case 0x38:
    case 0x39: case 0x3C: case 0x3D: case 0x3F:
        op_memory(c, b, pc, ins);
        break;
    default: unimplemented(c, pc, ins, "opcode"); break;
    }
}

int cpu_step(Cpu *c, Bus *b) {
    if (c->halted) return 0;

    uint32_t cur = c->pc;
    int in_delay = c->delay;
    uint32_t take_to = c->branch_target;
    c->delay = 0;
    c->override_pending = 0;

    uint32_t ins = bus_read32(b, cur);
    execute(c, b, cur, ins);

    c->steps++;
    c->cop0[9]++; /* Count: one per instruction until the cycle table lands (M5) */
    c->gpr[0] = 0;

    if (c->halted) return 0;
    if (c->override_pending) {
        c->pc = c->override_target; /* exception or ERET decided it */
        c->override_pending = 0;
    } else {
        c->pc = in_delay ? take_to : cur + 4;
    }
    return 1;
}

/* ------------------------------------------------------------ disassembly */

static const char *const GPR_NAMES[32] = {
    "r0", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
};

static const char *g(unsigned r) { return GPR_NAMES[r & 31u]; }

static const char *fmt_char(unsigned f) {
    switch (f) {
    case 16: return "s";
    case 17: return "d";
    case 20: return "w";
    case 21: return "l";
    default: return "?";
    }
}

static void describe_fpu(uint32_t ins, char *op, size_t cap) {
    static const char *const fn_name[64] = {
        "add", "sub", "mul", "div", "sqrt", "abs", "mov", "neg",
        "round.l", "trunc.l", "ceil.l", "floor.l", "round.w", "trunc.w", "ceil.w", "floor.w",
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        "cvt.s", "cvt.d", NULL, NULL, "cvt.w", "cvt.l", NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        "c.f", "c.un", "c.eq", "c.ueq", "c.olt", "c.ult", "c.ole", "c.ule",
        "c.sf", "c.ngle", "c.seq", "c.ngl", "c.lt", "c.nge", "c.le", "c.ngt",
    };
    unsigned fmt = (ins >> 21) & 31u, ft = (ins >> 16) & 31u, fs = (ins >> 11) & 31u,
             fd = (ins >> 6) & 31u, fn = ins & 63u;
    if (fn >= 48) {
        snprintf(op, cap, "%s.%s f%u, f%u", fn_name[fn] ? fn_name[fn] : "c.?", fmt_char(fmt), fs, ft);
        return;
    }
    const char *nm = fn_name[fn];
    if (!nm) {
        snprintf(op, cap, ".word %08x", ins);
        return;
    }
    if (fn == 0x04 || fn == 0x05 || fn == 0x06 || fn == 0x07 || fn == 0x20 || fn == 0x21)
        snprintf(op, cap, "%s.%s f%u, f%u", nm, fmt_char(fmt), fd, fs);
    else
        snprintf(op, cap, "%s.%s f%u, f%u, f%u", nm, fmt_char(fmt), fd, fs, ft);
}

void cpu_describe(uint32_t pc, uint32_t ins, char *buf, size_t cap) {
    char op[96];
    int32_t off = (int32_t)(int16_t)(ins & 0xFFFFu);
    uint32_t target;
    switch (ins >> 26) {
    case 0x00:
        switch (ins & 63u) {
        case 0x00: snprintf(op, sizeof op, "sll %s, %s, %u", g((ins >> 11) & 31), g((ins >> 16) & 31), (ins >> 6) & 31); goto done;
        case 0x02: snprintf(op, sizeof op, "srl %s, %s, %u", g((ins >> 11) & 31), g((ins >> 16) & 31), (ins >> 6) & 31); goto done;
        case 0x03: snprintf(op, sizeof op, "sra %s, %s, %u", g((ins >> 11) & 31), g((ins >> 16) & 31), (ins >> 6) & 31); goto done;
        case 0x08: snprintf(op, sizeof op, "jr %s", g((ins >> 21) & 31)); goto done;
        case 0x09: snprintf(op, sizeof op, "jalr %s, %s", g((ins >> 11) & 31), g((ins >> 21) & 31)); goto done;
        case 0x0C: snprintf(op, sizeof op, "syscall"); goto done;
        case 0x0D: snprintf(op, sizeof op, "break"); goto done;
        case 0x0F: snprintf(op, sizeof op, "sync"); goto done;
        case 0x10: snprintf(op, sizeof op, "mfhi %s", g((ins >> 11) & 31)); goto done;
        case 0x12: snprintf(op, sizeof op, "mflo %s", g((ins >> 11) & 31)); goto done;
        case 0x18: snprintf(op, sizeof op, "mult %s, %s", g((ins >> 21) & 31), g((ins >> 16) & 31)); goto done;
        case 0x19: snprintf(op, sizeof op, "multu %s, %s", g((ins >> 21) & 31), g((ins >> 16) & 31)); goto done;
        case 0x1A: snprintf(op, sizeof op, "div %s, %s", g((ins >> 21) & 31), g((ins >> 16) & 31)); goto done;
        case 0x1B: snprintf(op, sizeof op, "divu %s, %s", g((ins >> 21) & 31), g((ins >> 16) & 31)); goto done;
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
        case 0x2A: case 0x2B: {
            static const char *const n[] = {"add", "addu", "sub", "subu", "and", "or", "xor", "nor", "slt", "sltu"};
            unsigned k = (ins & 63u) > 0x27u ? (ins & 63u) - 0x2Au + 8u : (ins & 63u) - 0x20u;
            snprintf(op, sizeof op, "%s %s, %s, %s", n[k], g((ins >> 11) & 31), g((ins >> 21) & 31), g((ins >> 16) & 31));
            goto done;
        }
        default: snprintf(op, sizeof op, ".word %08x", ins); goto done;
        }
    case 0x01:
        snprintf(op, sizeof op, "regimm %s, %d", g((ins >> 21) & 31), off);
        goto done;
    case 0x02: case 0x03:
        target = ((pc + 4) & 0xF0000000u) | ((ins & 0x03FFFFFFu) << 2);
        snprintf(op, sizeof op, "%s %08x", (ins >> 26) == 2 ? "j" : "jal", target);
        goto done;
    case 0x04: case 0x05: case 0x06: case 0x07: {
        static const char *const n[] = {"beq", "bne", "blez", "bgtz"};
        target = pc + 4 + (uint32_t)(off << 2);
        snprintf(op, sizeof op, "%s %s, %08x", n[(ins >> 26) - 4], g((ins >> 21) & 31), target);
        goto done;
    }
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: {
        static const char *const n[] = {"addi", "addiu", "slti", "sltiu", "andi", "ori", "xori", "lui"};
        unsigned k = (ins >> 26) - 8;
        if (k == 7) snprintf(op, sizeof op, "lui %s, %#x", g((ins >> 16) & 31), (unsigned)(ins & 0xFFFFu));
        else if (k >= 4) snprintf(op, sizeof op, "%s %s, %s, %#x", n[k], g((ins >> 16) & 31), g((ins >> 21) & 31), (unsigned)(ins & 0xFFFFu));
        else snprintf(op, sizeof op, "%s %s, %s, %d", n[k], g((ins >> 16) & 31), g((ins >> 21) & 31), off);
        goto done;
    }
    case 0x10: {
        unsigned rs = (ins >> 21) & 31u, rd = (ins >> 11) & 31u;
        if (rs == 0x10) {
            switch (ins & 63u) {
            case 0x01: snprintf(op, sizeof op, "tlbr"); break;
            case 0x02: snprintf(op, sizeof op, "tlbwi"); break;
            case 0x06: snprintf(op, sizeof op, "tlbwr"); break;
            case 0x08: snprintf(op, sizeof op, "tlbp"); break;
            case 0x18: snprintf(op, sizeof op, "eret"); break;
            default: snprintf(op, sizeof op, ".word %08x", ins); break;
            }
        } else {
            snprintf(op, sizeof op, "%s %s, c%u", rs == 0 ? "mfc0" : "mtc0", g((ins >> 16) & 31), rd);
        }
        goto done;
    }
    case 0x11: {
        unsigned sub = (ins >> 21) & 31u;
        switch (sub) {
        case 0x00: snprintf(op, sizeof op, "mfc1 %s, f%u", g((ins >> 16) & 31), (ins >> 11) & 31); break;
        case 0x01: snprintf(op, sizeof op, "dmfc1 %s, f%u", g((ins >> 16) & 31), (ins >> 11) & 31); break;
        case 0x04: snprintf(op, sizeof op, "mtc1 %s, f%u", g((ins >> 16) & 31), (ins >> 11) & 31); break;
        case 0x05: snprintf(op, sizeof op, "dmtc1 %s, f%u", g((ins >> 16) & 31), (ins >> 11) & 31); break;
        case 0x08: snprintf(op, sizeof op, "bc1%s %d", ((ins >> 16) & 1u) ? "t" : "f", off); break;
        case 16: case 17: case 20: case 21: describe_fpu(ins, op, sizeof op); break;
        default: snprintf(op, sizeof op, ".word %08x", ins); break;
        }
        goto done;
    }
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E:
    case 0x30: case 0x31: case 0x34: case 0x35: case 0x37: case 0x38: case 0x39: case 0x3C:
    case 0x3D: case 0x3F: {
        const char *nm = ".word";
        switch (ins >> 26) {
        case 0x20: nm = "lb"; break;   case 0x21: nm = "lh"; break;   case 0x22: nm = "lwl"; break;
        case 0x23: nm = "lw"; break;   case 0x24: nm = "lbu"; break;  case 0x25: nm = "lhu"; break;
        case 0x26: nm = "lwr"; break;  case 0x27: nm = "lwu"; break;  case 0x28: nm = "sb"; break;
        case 0x29: nm = "sh"; break;   case 0x2A: nm = "swl"; break;  case 0x2B: nm = "sw"; break;
        case 0x2C: nm = "sdl"; break;  case 0x2D: nm = "sdr"; break;  case 0x2E: nm = "swr"; break;
        /* the 64-bit and COP1 accesses are named explicitly: they are not
         * contiguous in the 32-bit list, and an index slip here made traces
         * read "cache" for ll. */
        case 0x30: nm = "ll"; break;   case 0x31: nm = "lwc1"; break;
        case 0x34: nm = "lld"; break;  case 0x35: nm = "ldc1"; break;
        case 0x37: nm = "ld"; break;   case 0x38: nm = "sc"; break;
        case 0x39: nm = "swc1"; break; case 0x3C: nm = "scd"; break;
        case 0x3D: nm = "sdc1"; break; case 0x3F: nm = "sd"; break;
        default: break;
        }
        snprintf(op, sizeof op, "%s %s, %d(%s)", nm, g((ins >> 16) & 31), off, g((ins >> 21) & 31));
        goto done;
    }
    default: snprintf(op, sizeof op, ".word %08x", ins); goto done;
    }
done:
    snprintf(buf, cap, "%s", op);
}
