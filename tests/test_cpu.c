/* test_cpu.c — M0 test suite.
 *
 * Every test builds its own instruction stream as encoded words and runs it, so
 * the whole suite is verifiable without a cartridge: no ROM, no bootcode, no
 * copyrighted data anywhere. This is the harness the later milestones keep
 * adding to (M1 handoff, M2 RSP tasks, M3 display lists).
 */
#include "n64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        checks++;                                                         \
        if (!(cond)) {                                                    \
            failures++;                                                   \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                    \
    do {                                                                  \
        checks++;                                                         \
        unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); \
        if (_a != _b) {                                                   \
            failures++;                                                   \
            printf("  FAIL %s:%d: %s == %s (got %#llx, want %#llx)\n",    \
                   __FILE__, __LINE__, #a, #b, _a, _b);                   \
        }                                                                 \
    } while (0)

static uint32_t enc_r(int rs, int rt, int rd, int sa, int fn) {
    return ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | ((uint32_t)rd << 11) | ((uint32_t)sa << 6) | (uint32_t)fn;
}
static uint32_t enc_i(int op, int rs, int rt, int imm) {
    return ((uint32_t)op << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | ((uint32_t)imm & 0xFFFFu);
}
static uint32_t enc_j(int op, uint32_t target) {
    return ((uint32_t)op << 26) | ((target >> 2) & 0x03FFFFFFu);
}
static uint32_t beq(int rs, int rt, int32_t off_bytes) {
    return enc_i(0x04, rs, rt, off_bytes / 4);
}
static uint32_t enc_fpu(int fmt, int ft, int fs, int fd, int fn) {
    return (0x11u << 26) | ((uint32_t)fmt << 21) | ((uint32_t)ft << 16) | ((uint32_t)fs << 11) |
           ((uint32_t)fd << 6) | (uint32_t)fn;
}
static uint32_t enc_cop1_move(int sub, int rt, int fs) {
    return (0x11u << 26) | ((uint32_t)sub << 21) | ((uint32_t)rt << 16) | ((uint32_t)fs << 11);
}
static uint32_t enc_mtc0(int rt, int rd) {
    return (0x10u << 26) | (0x04u << 21) | ((uint32_t)rt << 16) | ((uint32_t)rd << 11);
}
static uint32_t enc_cop0_co(int fn) { return (0x10u << 26) | (0x10u << 21) | (uint32_t)fn; }

static Bus bus;
static Cpu cpu;

static void load_program(uint32_t addr, const uint32_t *ins, int n) {
    bus_init(&bus, NULL, 0);
    for (int i = 0; i < n; i++) bus_write32(&bus, addr + (uint32_t)i * 4, ins[i]);
    cpu_reset(&cpu, addr);
}

static void run(int n) {
    for (int i = 0; i < n; i++)
        if (!cpu_step(&cpu, &bus)) return;
}

/* ------------------------------------------------------------------ tests */

static void test_lui_ori(void) {
    printf("lui/ori compose a constant\n");
    uint32_t p[] = {enc_i(0x0F, 0, 1, 0x1234), enc_i(0x0D, 1, 1, 0x5678)};
    load_program(0x80000000u, p, 2);
    run(2);
    CHECK_EQ(cpu.gpr[1], 0x0000000012345678ull);
}

static void test_sign_extension(void) {
    printf("32-bit results sign-extend into the 64-bit register\n");
    uint32_t p[] = {enc_i(0x09, 0, 2, -1)};
    load_program(0x80000000u, p, 1);
    run(1);
    CHECK_EQ(cpu.gpr[2], 0xFFFFFFFFFFFFFFFFull);
}

static void test_shifts(void) {
    printf("sll/srl/sra use the low 32 bits and sign-extend\n");
    uint32_t p[] = {
        enc_i(0x0F, 0, 1, 0x8000),      /* lui  r1, 0x8000 */
        enc_r(0, 1, 2, 4, 0x03),        /* sra  r2, r1, 4   (rt is the source) */
        enc_r(0, 1, 3, 4, 0x02),        /* srl  r3, r1, 4 */
        enc_r(0, 1, 4, 4, 0x00),        /* sll  r4, r1, 4 */
    };
    load_program(0x80000000u, p, 4);
    run(4);
    CHECK_EQ(cpu.gpr[2], 0xFFFFFFFFF8000000ull);
    CHECK_EQ(cpu.gpr[3], 0x0000000008000000ull);
    CHECK_EQ(cpu.gpr[4], 0x0000000000000000ull);
}

static void test_branch_delay_slot(void) {
    printf("beq executes its delay slot and skips the fall-through\n");
    uint32_t p[] = {
        enc_i(0x09, 0, 1, 1),           /* 0x00 addiu r1, r0, 1 */
        beq(1, 1, 0x08),                /* 0x04 beq r1, r1, +0x08 -> 0x80000010 */
        enc_i(0x09, 0, 2, 7),           /* 0x08 delay: addiu r2, r0, 7 */
        enc_i(0x09, 0, 3, 9),           /* 0x0C skipped */
        enc_i(0x09, 0, 5, 3),           /* 0x10 target */
    };
    load_program(0x80000000u, p, 5);
    run(4);
    CHECK_EQ(cpu.gpr[2], 7);
    CHECK_EQ(cpu.gpr[3], 0);
    CHECK_EQ(cpu.gpr[5], 3);
}

static void test_jal_jr(void) {
    printf("jal links pc+8, jr returns through its delay slot\n");
    uint32_t p[] = {
        enc_j(0x03, 0x80000010u),       /* 0x00 jal 0x80000010 */
        enc_i(0x09, 0, 2, 1),           /* 0x04 delay: addiu r2, r0, 1 */
        enc_i(0x09, 0, 5, 5),           /* 0x08 return lands here */
        enc_i(0x09, 0, 6, 6),           /* 0x0C */
        enc_r(31, 0, 0, 0, 0x08),       /* 0x10 jr ra */
        enc_i(0x09, 0, 4, 2),           /* 0x14 delay: addiu r4, r0, 2 */
    };
    load_program(0x80000000u, p, 6);
    run(6);
    CHECK_EQ(cpu.gpr[31], 0x80000008u);
    CHECK_EQ(cpu.gpr[2], 1);
    CHECK_EQ(cpu.gpr[4], 2);
    CHECK_EQ(cpu.gpr[5], 5);
    CHECK_EQ(cpu.gpr[6], 6);
}

static void test_memory_big_endian(void) {
    printf("stores and loads are big-endian in RDRAM\n");
    uint32_t q[] = {
        enc_i(0x0F, 0, 1, 0x8000),
        enc_i(0x09, 1, 1, 0x2000),
        enc_i(0x0F, 0, 2, 0x1234),
        enc_i(0x0D, 2, 2, 0x5678),
        enc_i(0x2B, 1, 2, 0),           /* sw r2, 0(r1) */
        enc_i(0x20, 1, 3, 0),           /* lb r3, 0(r1) */
        enc_i(0x24, 1, 4, 0),           /* lbu r4, 0(r1) */
        enc_i(0x21, 1, 5, 2),           /* lh r5, 2(r1) */
        enc_i(0x23, 1, 6, 0),           /* lw r6, 0(r1) */
        enc_i(0x09, 0, 7, -1),          /* addiu r7, r0, -1 */
        enc_i(0x28, 1, 7, 4),           /* sb r7, 4(r1) */
        enc_i(0x20, 1, 8, 4),           /* lb r8, 4(r1) */
        enc_i(0x24, 1, 9, 4),           /* lbu r9, 4(r1) */
    };
    load_program(0x80000000u, q, (int)(sizeof q / sizeof q[0]));
    run((int)(sizeof q / sizeof q[0]));
    CHECK_EQ(bus.rdram[0x2000], 0x12);
    CHECK_EQ(bus.rdram[0x2001], 0x34);
    CHECK_EQ(bus.rdram[0x2002], 0x56);
    CHECK_EQ(bus.rdram[0x2003], 0x78);
    CHECK_EQ(cpu.gpr[3], 0x12);
    CHECK_EQ(cpu.gpr[4], 0x12);
    CHECK_EQ(cpu.gpr[5], 0x5678);
    CHECK_EQ(cpu.gpr[6], 0x12345678u);
    CHECK_EQ(bus.rdram[0x2004], 0xFF);
    CHECK_EQ(cpu.gpr[8], 0xFFFFFFFFFFFFFFFFull); /* lb sign-extends */
    CHECK_EQ(cpu.gpr[9], 0xFF);                  /* lbu does not */
}

static void test_unaligned(void) {
    printf("lwl/lwr and swl/swr reassemble unaligned words\n");
    bus_init(&bus, NULL, 0);
    bus_write32(&bus, 0x80003000u, 0x11223344u);
    bus_write32(&bus, 0x80003004u, 0x55667788u);
    uint32_t p[] = {
        enc_i(0x0F, 0, 1, 0x8000),
        enc_i(0x09, 1, 1, 0x3000),      /* r1 = 0x80003000 */
        enc_i(0x22, 1, 8, 1),           /* lwl r8, 1(r1) */
        enc_i(0x26, 1, 8, 4),           /* lwr r8, 4(r1) */
        enc_i(0x2B, 1, 0, 8),           /* sw  r0, 8(r1): clear the next word */
        enc_i(0x0F, 0, 9, 0xAABB),
        enc_i(0x0D, 9, 9, 0xCCDD),      /* r9 = 0xAABBCCDD */
        enc_i(0x2A, 1, 9, 1),           /* swl r9, 1(r1) */
        enc_i(0x2E, 1, 9, 4),           /* swr r9, 4(r1) */
    };
    for (uint32_t i = 0; i < 9; i++) bus_write32(&bus, 0x80000000u + i * 4, p[i]);
    cpu_reset(&cpu, 0x80000000u);
    run(9);
    CHECK_EQ(cpu.gpr[8], 0x22334455u);
    CHECK_EQ(bus_read8(&bus, 0x80003001u), 0xAA);
    CHECK_EQ(bus_read8(&bus, 0x80003002u), 0xBB);
    CHECK_EQ(bus_read8(&bus, 0x80003003u), 0xCC);
    CHECK_EQ(bus_read8(&bus, 0x80003004u), 0xDD);
    CHECK_EQ(bus_read8(&bus, 0x80003005u), 0x66); /* untouched byte of the next word */
}

static void test_mult_div(void) {
    printf("mult/divu-by-zero/div use lo and hi correctly\n");
    uint32_t m[] = {enc_i(0x09, 0, 1, 7), enc_i(0x09, 0, 2, 3), enc_r(1, 2, 0, 0, 0x18)};
    load_program(0x80000000u, m, 3);
    run(3);
    CHECK_EQ(cpu.lo, 21);
    CHECK_EQ(cpu.hi, 0);

    uint32_t d0[] = {enc_i(0x09, 0, 3, 5), enc_r(3, 0, 0, 0, 0x1B)};
    load_program(0x80000000u, d0, 2);
    run(2);
    CHECK_EQ(cpu.lo, 0xFFFFFFFFu);
    CHECK_EQ(cpu.hi, 5);

    uint32_t d1[] = {enc_i(0x09, 0, 4, -7), enc_i(0x09, 0, 5, 2), enc_r(4, 5, 0, 0, 0x1A)};
    load_program(0x80000000u, d1, 3);
    run(3);
    CHECK_EQ(cpu.lo, 0xFFFFFFFDu); /* -3 */
    CHECK_EQ(cpu.hi, 0xFFFFFFFFu); /* -1 */
}

static void test_slt(void) {
    printf("slt is signed, sltu is unsigned\n");
    uint32_t p[] = {
        enc_i(0x09, 0, 1, -1),
        enc_i(0x09, 0, 2, 1),
        enc_r(1, 2, 3, 0, 0x2A),
        enc_r(1, 2, 4, 0, 0x2B),
    };
    load_program(0x80000000u, p, 4);
    run(4);
    CHECK_EQ(cpu.gpr[3], 1);
    CHECK_EQ(cpu.gpr[4], 0);
}

static void test_overflow_exception(void) {
    printf("add overflow raises an exception and sets EPC\n");
    uint32_t p[] = {
        enc_i(0x0F, 0, 1, 0x7FFF),
        enc_i(0x0D, 1, 1, 0xFFFF),      /* r1 = 0x7FFFFFFF */
        enc_i(0x09, 0, 2, 1),
        enc_r(1, 2, 3, 0, 0x20),        /* add r3, r1, r2 -> overflow */
    };
    load_program(0x80000000u, p, 4);
    run(4);
    CHECK_EQ(cpu.pc, 0x80000180u);
    CHECK_EQ(cpu.cop0[14], 0x8000000Cu);                /* EPC = the add */
    CHECK_EQ((cpu.cop0[13] >> 2) & 0x1F, 12);           /* ExcCode = OV */
    CHECK_EQ(cpu.gpr[3], 0);
}

static void test_bus_alias(void) {
    printf("RDRAM is visible through kseg0 and kseg1\n");
    bus_init(&bus, NULL, 0);
    bus_write32(&bus, 0x80000000u, 0xCAFEBABEu);
    CHECK_EQ(bus_read32(&bus, 0xA0000000u), 0xCAFEBABEu);
    CHECK_EQ(bus_read32(&bus, 0x00000000u), 0xCAFEBABEu);
    CHECK_EQ(bus.rdram[0], 0xCA);
    CHECK_EQ(bus_is_rdram(0x80200000u), 1);
    CHECK_EQ(bus_is_rdram(0x00800000u), 0);
}

static void write_synthetic_rom(const char *path, int order) {
    uint8_t z64[0x1000];
    memset(z64, 0, sizeof z64);
    z64[0] = 0x80; z64[1] = 0x37; z64[2] = 0x12; z64[3] = 0x40;   /* .z64 magic */
    z64[0x08] = 0x80; z64[0x09] = 0x00; z64[0x0A] = 0x04; z64[0x0B] = 0x00;
    z64[0x10] = 0xDE; z64[0x11] = 0xAD; z64[0x12] = 0xBE; z64[0x13] = 0xEF;
    z64[0x14] = 0x12; z64[0x15] = 0x34; z64[0x16] = 0x56; z64[0x17] = 0x78;
    memcpy(z64 + 0x20, "TEST ZELDA          ", 20);
    z64[0x3E] = 'E';

    uint8_t out[0x1000];
    if (order == ROM_ORDER_Z64) {
        memcpy(out, z64, sizeof out);
    } else if (order == ROM_ORDER_V64) {
        for (int i = 0; i < (int)sizeof out; i += 2) {
            out[i] = z64[i + 1];
            out[i + 1] = z64[i];
        }
    } else {
        for (int i = 0; i < (int)sizeof out; i += 4) {
            out[i] = z64[i + 3];
            out[i + 1] = z64[i + 2];
            out[i + 2] = z64[i + 1];
            out[i + 3] = z64[i];
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("  FAIL cannot write %s\n", path);
        failures++;
        return;
    }
    fwrite(out, 1, sizeof out, f);
    fclose(f);
}

static void test_rom_orders(void) {
    printf("rom loader normalises z64/v64/n64 byte orders\n");
    const char *paths[3] = {"build/order.z64", "build/order.v64", "build/order.n64"};
    for (int i = 0; i < 3; i++) {
        write_synthetic_rom(paths[i], i);
        Rom rom;
        char err[128];
        int rc = rom_load(paths[i], &rom, err, sizeof err);
        CHECK_EQ(rc, 0);
        if (rc != 0) {
            printf("  %s: %s\n", paths[i], err);
            continue;
        }
        CHECK_EQ(rom.order, i);
        CHECK_EQ(rom.entry, 0x80000400u);
        CHECK_EQ(rom.crc1, 0xDEADBEEFu);
        CHECK_EQ(rom.crc2, 0x12345678u);
        CHECK(!strcmp(rom.title, "TEST ZELDA"));
        CHECK_EQ(rom.region, 'E');
        /* the cart window sees the normalised image, magic first */
        Bus b;
        bus_init(&b, rom.data, rom.size);
        CHECK_EQ(bus_read32(&b, 0x10000000u), 0x80371240u);
        rom_free(&rom);
    }
}

static void test_fpu_double(void) {
    printf("COP1: cvt.d.s then add.d, read back with dmfc1\n");
    uint32_t p[] = {
        enc_i(0x0F, 0, 1, 0x3FC0),                  /* lui   r1, 0x3FC0: 1.5f */
        enc_cop1_move(0x04, 1, 0),                  /* mtc1  r1, f0 */
        enc_fpu(16, 0, 0, 0, 0x21),                 /* cvt.d.s f0, f0 */
        enc_fpu(17, 0, 0, 2, 0x00),                 /* add.d f2, f0, f0 */
        enc_cop1_move(0x01, 2, 2),                  /* dmfc1 r2, f2 */
    };
    load_program(0x80000000u, p, (int)(sizeof p / sizeof p[0]));
    run(5);
    CHECK_EQ(cpu.gpr[2], 0x4008000000000000ull); /* 3.0 as an IEEE double */
}

static void test_fpu_compare_branch(void) {
    printf("COP1: cvt.d.w, c.lt.d, bc1t with a delay slot\n");
    uint32_t p[] = {
        enc_i(0x09, 0, 1, -1),                      /* 0x00 addiu r1, r0, -1 */
        enc_cop1_move(0x04, 1, 0),                  /* 0x04 mtc1  r1, f0 */
        enc_fpu(20, 0, 0, 0, 0x21),                 /* 0x08 cvt.d.w f0, f0 */
        enc_i(0x09, 0, 2, 1),                       /* 0x0C addiu r2, r0, 1 */
        enc_cop1_move(0x04, 2, 1),                  /* 0x10 mtc1  r2, f1 */
        enc_fpu(20, 0, 1, 1, 0x21),                 /* 0x14 cvt.d.w f1, f1 */
        enc_fpu(17, 1, 0, 0, 0x3C),                 /* 0x18 c.lt.d f0, f1 */
        enc_i(0x08 + 0x03, 0, 0, 0),                /* placeholder, replaced below */
        enc_i(0x09, 0, 3, 1),                       /* 0x20 delay: addiu r3, r0, 1 */
        enc_i(0x09, 0, 4, 9),                       /* 0x24 skipped */
        enc_i(0x09, 0, 5, 3),                       /* 0x28 target */
    };
    /* bc1t +0x08 from 0x1C lands on 0x28. enc_i(0x0B,...) is not BC1, so build it
     * directly: OP=0x11, sub=0x08, bit 0 of ft set for "true". */
    p[7] = (0x11u << 26) | (0x08u << 21) | (0x01u << 16) | ((uint32_t)(0x08 / 4) & 0xFFFFu);
    load_program(0x80000000u, p, (int)(sizeof p / sizeof p[0]));
    run(10); /* through the delay slot to the branch target */
    CHECK_EQ(cpu.gpr[3], 1); /* delay slot ran */
    CHECK_EQ(cpu.gpr[4], 0); /* fall-through skipped */
    CHECK_EQ(cpu.gpr[5], 3); /* branch target ran */
}

static void test_fpu_convert_back(void) {
    printf("COP1: cvt.s.w, add.s, cvt.w.s, mfc1\n");
    uint32_t p[] = {
        enc_i(0x09, 0, 1, 3),
        enc_cop1_move(0x04, 1, 0),      /* mtc1 r1, f0 */
        enc_fpu(20, 0, 0, 0, 0x20),     /* cvt.s.w f0, f0 */
        enc_fpu(16, 0, 0, 2, 0x00),     /* add.s f2, f0, f0 */
        enc_fpu(16, 0, 2, 2, 0x24),     /* cvt.w.s f2, f2 */
        enc_cop1_move(0x00, 3, 2),      /* mfc1 r3, f2 */
    };
    load_program(0x80000000u, p, (int)(sizeof p / sizeof p[0]));
    run(6);
    CHECK_EQ(cpu.gpr[3], 6);
}

static void test_pi_dma(void) {
    printf("PI: WR_LEN copies cart bytes into RDRAM\n");
    static uint8_t cart[64];
    for (int i = 0; i < 64; i++) cart[i] = (uint8_t)(0x10 + i);
    Bus b;
    bus_init(&b, cart, sizeof cart);
    bus_write32(&b, 0x04600000u, 0x00001000u); /* DRAM_ADDR */
    bus_write32(&b, 0x04600004u, 0x00000010u); /* CART_ADDR */
    bus_write32(&b, 0x0460000Cu, 0x0000000Fu); /* WR_LEN: 16 bytes */
    CHECK_EQ(b.pi.dmas, 1);
    CHECK_EQ(bus_read8(&b, 0x1000u), 0x20); /* cart[0x10], since cart[i] = 0x10 + i */
    CHECK_EQ(bus_read8(&b, 0x100Fu), 0x2F); /* cart[0x1F] */
    CHECK_EQ(bus_read32(&b, 0x0460000Cu), 0x0000000Fu);
}

static void test_tlb_translation(void) {
    printf("TLB: TLBWI maps a page and lw goes through it\n");
    uint32_t p[] = {
        enc_mtc0(0, 10),                  /* EntryHi = 0: vpn 0 */
        enc_i(0x0F, 0, 1, 0x0004),
        enc_i(0x0D, 1, 1, 0x0002),        /* EntryLo = pfn 0x40, V */
        enc_mtc0(1, 2),                   /* EntryLo0 */
        enc_mtc0(1, 3),                   /* EntryLo1 */
        enc_mtc0(0, 0),                   /* Index 0 */
        enc_cop0_co(0x02),                /* tlbwi */
        enc_i(0x23, 0, 5, 0),             /* lw r5, 0(r0) */
    };
    load_program(0x80000000u, p, (int)(sizeof p / sizeof p[0]));
    bus_write32(&bus, 0x00040000u, 0xCAFEF00Du); /* after load_program, which re-inits the bus */
    run((int)(sizeof p / sizeof p[0]));
    CHECK_EQ((uint32_t)cpu.gpr[5], 0xCAFEF00Du);
    CHECK_EQ(cpu.tlb_hits, 1);
    CHECK_EQ(cpu.tlb_misses, 0);
}

static void test_tlb_miss(void) {
    printf("TLB: an unmapped kuseg load takes the refill vector\n");
    uint32_t p[] = {
        enc_i(0x0F, 0, 1, 0x0000),
        enc_i(0x09, 1, 1, 0x1000),        /* r1 = 0x1000 */
        enc_i(0x23, 1, 5, 0),             /* lw r5, 0(r1) */
    };
    load_program(0x80000000u, p, 3);
    run(3);
    CHECK_EQ(cpu.pc, 0x80000000u);                 /* refill, not the general vector */
    CHECK_EQ((cpu.cop0[13] >> 2) & 0x1F, 2);       /* ExcCode = TLB miss */
    CHECK_EQ(cpu.cop0[14], 0x80000008u);           /* EPC = the lw */
    CHECK_EQ(cpu.tlb_misses, 1);
}

static void test_count(void) {
    printf("COP0 Count advances once per instruction\n");
    uint32_t p[] = {enc_i(0x09, 0, 1, 1), enc_i(0x09, 0, 1, 2), enc_i(0x09, 0, 1, 3)};
    load_program(0x80000000u, p, 3);
    run(3);
    CHECK_EQ(cpu.cop0[9], 3);
}

static void test_describe(void) {
    printf("disassembler names the 64-bit accesses\n");
    char buf[64];
    /* rs=1 is $at and rt=2 is $v0 in the ABI names the disassembler prints */
    cpu_describe(0x80000000u, enc_i(0x30, 1, 2, 8), buf, sizeof buf);
    CHECK(!strcmp(buf, "ll v0, 8(at)"));
    cpu_describe(0x80000000u, enc_i(0x37, 1, 2, 8), buf, sizeof buf);
    CHECK(!strcmp(buf, "ld v0, 8(at)"));
    cpu_describe(0x80000000u, enc_i(0x3F, 1, 2, 8), buf, sizeof buf);
    CHECK(!strcmp(buf, "sd v0, 8(at)"));
    cpu_describe(0x80000000u, enc_i(0x2B, 1, 2, 8), buf, sizeof buf);
    CHECK(!strcmp(buf, "sw v0, 8(at)"));
}

int main(void) {
    printf("n64-player M0+M1 tests\n\n");
    test_lui_ori();
    test_sign_extension();
    test_shifts();
    test_branch_delay_slot();
    test_jal_jr();
    test_memory_big_endian();
    test_unaligned();
    test_mult_div();
    test_slt();
    test_overflow_exception();
    test_bus_alias();
    test_rom_orders();
    test_fpu_double();
    test_fpu_compare_branch();
    test_fpu_convert_back();
    test_pi_dma();
    test_tlb_translation();
    test_tlb_miss();
    test_count();
    test_describe();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
