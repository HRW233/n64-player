/* rsp.c — the RSP scalar core and OSTask dispatch (milestone M2).
 *
 * The RSP is a 32-bit MIPS (an R4000 derivative) with its own 4 KiB IMEM and
 * 4 KiB DMEM, no caches, no FPU, no TLB and no exceptions: a bad instruction
 * simply stops the SP, which is why every failure here halts with a name
 * instead of guessing. Branches take effect immediately — the RSP has no delay
 * slots. Vector (COP2) instructions are milestone M3 and stop here.
 *
 * A task is only DMA plus a PC. The CPU writes SP_MEM_ADDR, SP_DRAM_ADDR and
 * then SP_RD_LEN/SP_WR_LEN, and bus.c moves the bytes; this file has to put the
 * ucode where the hardware would and start the PC at its first instruction.
 */
#include "n64.h"

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

void rsp_halt(Rsp *r, const char *why) {
    r->halted = 1;
    snprintf(r->halt_reason, sizeof r->halt_reason, "%s", why);
}

/* Format into a local buffer, then halt: passing r->halt_reason straight back
 * into rsp_halt makes the compiler (correctly) suspicious about overlap. */
static void rsp_haltf(Rsp *r, const char *fmt, unsigned arg) {
    char buf[96];
    snprintf(buf, sizeof buf, fmt, arg);
    rsp_halt(r, buf);
}

void rsp_reset(Rsp *r, uint32_t pc) {
    memset(r, 0, sizeof *r);
    r->pc = pc;
}

static void set(Rsp *r, unsigned d, uint32_t v) {
    if (d) r->gpr[d] = v;
}

/* ------------------------------------------------------------ memory */

/* RSP addresses arrive as 0xA4000000 aliases, as 0x04000000 physical addresses,
 * or as the bare 0x0000-0x1FFF offsets ucode uses for its own memory. */
static int sp_map(uint32_t a, uint32_t *off) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (p >= 0x04000000u) p -= 0x04000000u;
    if (p < 0x2000u) {
        *off = p;
        return 1;
    }
    return 0;
}

static int sp_reg(uint32_t a, uint32_t *off) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (p >= 0x04040000u && p < 0x04040020u) {
        *off = p - 0x04040000u;
        return 1;
    }
    return 0;
}

static int sp_rd8(Bus *b, uint32_t a, uint8_t *out) {
    uint32_t off;
    if (sp_map(a, &off)) {
        *out = off < 0x1000u ? b->sp.dmem[off] : b->sp.imem[off - 0x1000u];
        return 1;
    }
    if (sp_reg(a, &off)) {
        *out = (uint8_t)(bus_sp_read(b, off) >> (8 * (3 - (a & 3))));
        return 1;
    }
    return 0;
}

static int sp_wr8(Bus *b, uint32_t a, uint8_t v) {
    uint32_t off;
    if (sp_map(a, &off)) {
        if (off < 0x1000u) b->sp.dmem[off] = v;
        else b->sp.imem[off - 0x1000u] = v;
        return 1;
    }
    return sp_reg(a, &off); /* byte writes into registers are dropped */
}

static int sp_rd16(Bus *b, uint32_t a, uint16_t *out) {
    uint8_t hi, lo;
    if (!sp_rd8(b, a, &hi) || !sp_rd8(b, a + 1, &lo)) return 0;
    *out = (uint16_t)((hi << 8) | lo);
    return 1;
}

static int sp_wr16(Bus *b, uint32_t a, uint16_t v) {
    return sp_wr8(b, a, (uint8_t)(v >> 8)) && sp_wr8(b, a + 1, (uint8_t)v);
}

static int sp_rd32(Bus *b, uint32_t a, uint32_t *out) {
    uint32_t off;
    if (sp_reg(a, &off)) {
        *out = bus_sp_read(b, off);
        return 1;
    }
    uint8_t b0, b1, b2, b3;
    if (!sp_rd8(b, a, &b0) || !sp_rd8(b, a + 1, &b1) || !sp_rd8(b, a + 2, &b2) || !sp_rd8(b, a + 3, &b3))
        return 0;
    *out = ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | b3;
    return 1;
}

static int sp_wr32(Bus *b, uint32_t a, uint32_t v) {
    uint32_t off;
    if (sp_reg(a, &off)) {
        bus_sp_write(b, off, v);
        return 1;
    }
    return sp_wr8(b, a, (uint8_t)(v >> 24)) && sp_wr8(b, a + 1, (uint8_t)(v >> 16)) &&
           sp_wr8(b, a + 2, (uint8_t)(v >> 8)) && sp_wr8(b, a + 3, (uint8_t)v);
}

/* LWL/LWR merge the bytes around a word, as on the CPU, but within DMEM only. */
static void op_lwl(Rsp *r, Bus *b, uint32_t a, unsigned rt) {
    uint32_t aligned = a & ~3u, n = a & 3u;
    uint32_t v = 0;
    for (uint32_t j = 0; j + n < 4; j++) {
        uint8_t byte;
        if (!sp_rd8(b, aligned + n + j, &byte)) { rsp_halt(r, "LWL outside DMEM"); return; }
        v = (v << 8) | byte;
    }
    uint32_t mask = n ? ((1u << (8 * n)) - 1u) : 0u;
    set(r, rt, (v << (8 * n)) | (r->gpr[rt] & mask));
}

static void op_lwr(Rsp *r, Bus *b, uint32_t a, unsigned rt) {
    uint32_t aligned = a & ~3u, n = a & 3u;
    uint32_t v = 0;
    for (uint32_t j = 0; j <= n; j++) {
        uint8_t byte;
        if (!sp_rd8(b, aligned + j, &byte)) { rsp_halt(r, "LWR outside DMEM"); return; }
        v = (v << 8) | byte;
    }
    uint32_t keep = ~((1u << (8 * (n + 1))) - 1u);
    set(r, rt, (r->gpr[rt] & keep) | v);
}

static void op_swl(Rsp *r, Bus *b, uint32_t a, unsigned rt) {
    uint32_t aligned = a & ~3u, n = a & 3u, v = r->gpr[rt];
    for (uint32_t i = n; i < 4; i++)
        if (!sp_wr8(b, aligned + i, (uint8_t)(v >> (8 * (3 - (i - n)))))) { rsp_halt(r, "SWL outside DMEM"); return; }
}

static void op_swr(Rsp *r, Bus *b, uint32_t a, unsigned rt) {
    uint32_t aligned = a & ~3u, n = a & 3u, v = r->gpr[rt];
    for (uint32_t i = 0; i <= n; i++)
        if (!sp_wr8(b, aligned + i, (uint8_t)(v >> (8 * (n - i))))) { rsp_halt(r, "SWR outside DMEM"); return; }
}

/* ------------------------------------------------------------ core */

int rsp_step(Rsp *r, Bus *b) {
    if (r->halted) return 0;

    uint32_t pc = r->pc;
    uint32_t ins;
    uint32_t phys = pc & 0x1FFFFFFFu;
    if (phys < 0x04001000u || phys >= 0x04002000u) {
        rsp_halt(r, "pc left IMEM");
        return 0;
    }
    if (!sp_rd32(b, pc, &ins)) {
        rsp_halt(r, "instruction fetch failed");
        return 0;
    }

    uint32_t next = pc + 4;
    uint32_t s = r->gpr[RS], t = r->gpr[RT];
    uint32_t target = ((pc + 4) & 0xF0000000u) | ((ins & 0x03FFFFFFu) << 2);

    switch (OP) {
    case 0x00:
        switch (FN) {
        case 0x00: set(r, RD, t << SA); break;                        /* SLL */
        case 0x02: set(r, RD, t >> SA); break;                        /* SRL */
        case 0x03: set(r, RD, (uint32_t)((int32_t)t >> SA)); break;   /* SRA */
        case 0x04: set(r, RD, t << (s & 31u)); break;                 /* SLLV */
        case 0x06: set(r, RD, t >> (s & 31u)); break;                 /* SRLV */
        case 0x07: set(r, RD, (uint32_t)((int32_t)t >> (s & 31u))); break; /* SRAV */
        case 0x08: next = s & 0x1FFFFFFFu; break;                     /* JR */
        case 0x09: set(r, RD, pc + 4); next = s & 0x1FFFFFFFu; break; /* JALR */
        case 0x0C: rsp_halt(r, "syscall (the RSP has no exceptions)"); return 0;
        case 0x0D: rsp_halt(r, "break"); return 0;                    /* BREAK */
        case 0x0F: break;                                             /* SYNC */
        case 0x10: set(r, RD, r->hi); break;                          /* MFHI */
        case 0x11: r->hi = s; break;                                  /* MTHI */
        case 0x12: set(r, RD, r->lo); break;                          /* MFLO */
        case 0x13: r->lo = s; break;                                  /* MTLO */
        case 0x18: {                                                  /* MULT */
            int64_t p = (int64_t)(int32_t)s * (int64_t)(int32_t)t;
            r->lo = (uint32_t)p;
            r->hi = (uint32_t)((uint64_t)p >> 32);
            break;
        }
        case 0x19: {                                                  /* MULTU */
            uint64_t p = (uint64_t)s * (uint64_t)t;
            r->lo = (uint32_t)p;
            r->hi = (uint32_t)(p >> 32);
            break;
        }
        case 0x1A: {                                                  /* DIV */
            if (t == 0) { r->lo = (s & 0x80000000u) ? 1u : 0xFFFFFFFFu; r->hi = s; }
            else if (s == 0x80000000u && t == 0xFFFFFFFFu) { r->lo = 0x80000000u; r->hi = 0; }
            else { r->lo = (uint32_t)((int32_t)s / (int32_t)t); r->hi = (uint32_t)((int32_t)s % (int32_t)t); }
            break;
        }
        case 0x1B: {                                                  /* DIVU */
            if (t == 0) { r->lo = 0xFFFFFFFFu; r->hi = s; }
            else { r->lo = s / t; r->hi = s % t; }
            break;
        }
        case 0x20: case 0x21: set(r, RD, s + t); break;               /* ADD / ADDU (no trap) */
        case 0x22: case 0x23: set(r, RD, s - t); break;               /* SUB / SUBU */
        case 0x24: set(r, RD, s & t); break;
        case 0x25: set(r, RD, s | t); break;
        case 0x26: set(r, RD, s ^ t); break;
        case 0x27: set(r, RD, ~(s | t)); break;
        case 0x2A: set(r, RD, (int32_t)s < (int32_t)t ? 1u : 0u); break;
        case 0x2B: set(r, RD, s < t ? 1u : 0u); break;
        default:
            rsp_haltf(r, "unimplemented SPECIAL funct 0x%02x", FN);
            return 0;
        }
        break;

    case 0x01: /* REGIMM */
        switch (RT) {
        case 0x00: if ((int32_t)s < 0) next = pc + 4 + (uint32_t)(SIMM << 2); break;
        case 0x01: if ((int32_t)s >= 0) next = pc + 4 + (uint32_t)(SIMM << 2); break;
        case 0x10: set(r, 31, pc + 4); if ((int32_t)s < 0) next = pc + 4 + (uint32_t)(SIMM << 2); break;
        case 0x11: set(r, 31, pc + 4); if ((int32_t)s >= 0) next = pc + 4 + (uint32_t)(SIMM << 2); break;
        default: rsp_halt(r, "unimplemented REGIMM variant"); return 0;
        }
        break;

    case 0x02: next = target; break;                                  /* J */
    case 0x03: set(r, 31, pc + 4); next = target; break;               /* JAL */
    case 0x04: if (s == t) next = pc + 4 + (uint32_t)(SIMM << 2); break;
    case 0x05: if (s != t) next = pc + 4 + (uint32_t)(SIMM << 2); break;
    case 0x06: if ((int32_t)s <= 0) next = pc + 4 + (uint32_t)(SIMM << 2); break;
    case 0x07: if ((int32_t)s > 0) next = pc + 4 + (uint32_t)(SIMM << 2); break;

    case 0x08: set(r, RT, s + (uint32_t)SIMM); break;                  /* ADDI */
    case 0x09: set(r, RT, s + (uint32_t)SIMM); break;                  /* ADDIU */
    case 0x0A: set(r, RT, (int32_t)s < SIMM ? 1u : 0u); break;          /* SLTI */
    case 0x0B: set(r, RT, s < (uint32_t)SIMM ? 1u : 0u); break;         /* SLTIU */
    case 0x0C: set(r, RT, s & IMM); break;
    case 0x0D: set(r, RT, s | IMM); break;
    case 0x0E: set(r, RT, s ^ IMM); break;
    case 0x0F: set(r, RT, (uint32_t)IMM << 16); break;

    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2E: {
        uint32_t a = s + (uint32_t)SIMM;
        uint32_t v;
        uint16_t h;
        uint8_t byte;
        switch (OP) {
        case 0x20: if (!sp_rd8(b, a, &byte)) { rsp_halt(r, "LB outside DMEM"); return 0; }
                   set(r, RT, (uint32_t)(int32_t)(int8_t)byte); break;
        case 0x21: if (!sp_rd16(b, a, &h)) { rsp_halt(r, "LH outside DMEM"); return 0; }
                   set(r, RT, (uint32_t)(int32_t)(int16_t)h); break;
        case 0x22: op_lwl(r, b, a, RT); break;
        case 0x23: if (!sp_rd32(b, a, &v)) { rsp_halt(r, "LW outside DMEM"); return 0; }
                   set(r, RT, v); break;
        case 0x24: if (!sp_rd8(b, a, &byte)) { rsp_halt(r, "LBU outside DMEM"); return 0; }
                   set(r, RT, byte); break;
        case 0x25: if (!sp_rd16(b, a, &h)) { rsp_halt(r, "LHU outside DMEM"); return 0; }
                   set(r, RT, h); break;
        case 0x26: op_lwr(r, b, a, RT); break;
        case 0x28: if (!sp_wr8(b, a, (uint8_t)t)) { rsp_halt(r, "SB outside DMEM"); return 0; }
                   break;
        case 0x29: if (!sp_wr16(b, a, (uint16_t)t)) { rsp_halt(r, "SH outside DMEM"); return 0; }
                   break;
        case 0x2A: op_swl(r, b, a, RT); break;
        case 0x2B: if (!sp_wr32(b, a, t)) { rsp_halt(r, "SW outside DMEM"); return 0; }
                   break;
        case 0x2E: op_swr(r, b, a, RT); break;
        }
        break;
    }

    case 0x12: rsp_halt(r, "COP2 vector op (milestone M3)"); return 0;
    default:
        rsp_haltf(r, "unimplemented opcode 0x%02x", OP);
        return 0;
    }

    if (r->halted) return 0;
    r->pc = next;
    r->steps++;
    r->gpr[0] = 0;
    return 1;
}

uint64_t rsp_run(Rsp *r, Bus *b, uint64_t max_steps) {
    uint64_t n = 0;
    while (n < max_steps && !r->halted) {
        if (!rsp_step(r, b)) break;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------ OSTask */

int ostask_load(const Bus *b, uint32_t addr, OsTask *out) {
    memset(out, 0, sizeof *out);
    out->type = bus_read32(b, addr + 0x00);
    out->flags = bus_read32(b, addr + 0x04);
    out->ucode = bus_read32(b, addr + 0x08);
    out->ucode_data = bus_read32(b, addr + 0x0C);
    out->dram_stack = bus_read32(b, addr + 0x10);
    out->output_buff = bus_read32(b, addr + 0x18);
    out->output_buff_size = bus_read32(b, addr + 0x1C);
    out->data_ptr = bus_read32(b, addr + 0x20);
    out->data_size = bus_read32(b, addr + 0x24);
    return (out->ucode && out->data_ptr) ? 0 : -1;
}

/* libultra's loading order, which is also what the SP registers force: a full
 * IMEM of ucode, 0x800 bytes of ucode_data at DMEM 0x800, then the task data at
 * DMEM 0. The ucode DMAs its own output back; nothing here guesses at it. */
#define UCODE_SIZE 0x1000u
#define UCODE_DATA_SIZE 0x800u

int ostask_run(Bus *b, const OsTask *t, OsTaskResult *res, char *err, size_t cap) {
    memset(res, 0, sizeof *res);
    if (!t->ucode || !t->data_ptr) {
        snprintf(err, cap, "task has no ucode or data pointer");
        return -1;
    }

    uint32_t data_len = t->data_size ? t->data_size : SP_MEM_SIZE;
    if (data_len > SP_MEM_SIZE) data_len = SP_MEM_SIZE;

    bus_sp_write(b, 0x00, 0x04001000u);
    bus_sp_write(b, 0x04, t->ucode);
    bus_sp_write(b, 0x08, UCODE_SIZE - 1u);      /* RDRAM -> IMEM */

    if (t->ucode_data) {
        bus_sp_write(b, 0x00, 0x04000800u);
        bus_sp_write(b, 0x04, t->ucode_data);
        bus_sp_write(b, 0x08, UCODE_DATA_SIZE - 1u);
    }

    bus_sp_write(b, 0x00, 0x04000000u);
    bus_sp_write(b, 0x04, t->data_ptr);
    bus_sp_write(b, 0x08, data_len - 1u);

    Rsp rsp;
    rsp_reset(&rsp, 0x04001000u);
    bus_sp_write(b, 0x10, 0x0002u);              /* clear halt before starting */
    b->sp.pc = rsp.pc;

    res->steps = rsp_run(&rsp, b, 1u << 20);
    res->pc = rsp.pc;
    snprintf(res->reason, sizeof res->reason, "%s", rsp.halted ? rsp.halt_reason : "ran out of steps");
    b->sp.pc = rsp.pc;

    if (!rsp.halted) {
        res->halted = 0;
        snprintf(err, cap, "ucode ran %llu instructions without halting", (unsigned long long)res->steps);
        return -1;
    }
    res->halted = strcmp(rsp.halt_reason, "break") == 0;
    if (!res->halted) {
        snprintf(err, cap, "ucode stopped: %s", rsp.halt_reason);
        return -1;
    }
    return 0;
}
