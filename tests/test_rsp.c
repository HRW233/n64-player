/* test_rsp.c — M2 test suite: SP DMA, the RSP scalar core, OSTask dispatch.
 *
 * The last test is the one that matters: a task is built in RDRAM the way
 * libultra builds one, its ucode is hand-assembled here, and running it must
 * produce a specific display list in DMEM. Nothing about it needs a cartridge,
 * and the assertion is on the bytes, not on "it did not crash".
 */
#include "n64.h"

#include <stdio.h>
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
    return ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | ((uint32_t)rd << 11) |
           ((uint32_t)sa << 6) | (uint32_t)fn;
}
static uint32_t enc_i(int op, int rs, int rt, int imm) {
    return ((uint32_t)op << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | ((uint32_t)imm & 0xFFFFu);
}

/* ------------------------------------------------------------------ SP DMA */

static void test_sp_dma(void) {
    printf("SP: RD_LEN moves RDRAM into DMEM, WR_LEN moves it back\n");
    Bus b;
    bus_init(&b, NULL, 0);

    bus_write32(&b, 0x00100000u, 0xDEADBEEFu);
    bus_write32(&b, 0x00100004u, 0x0BADC0DEu);

    bus_write32(&b, 0x04040000u, 0xA4000000u); /* SP_MEM_ADDR: DMEM 0 */
    bus_write32(&b, 0x04040004u, 0x00100000u); /* SP_DRAM_ADDR */
    bus_write32(&b, 0x04040008u, 0x7u);        /* SP_RD_LEN: 8 bytes, RDRAM -> DMEM */
    CHECK_EQ(bus_read32(&b, 0x04000000u), 0xDEADBEEFu);
    CHECK_EQ(bus_read32(&b, 0x04000004u), 0x0BADC0DEu);

    bus_write32(&b, 0x04040000u, 0xA4000000u);
    bus_write32(&b, 0x04040004u, 0x00100010u);
    bus_write32(&b, 0x0404000Cu, 0x7u);        /* SP_WR_LEN: 8 bytes, DMEM -> RDRAM */
    CHECK_EQ(bus_read32(&b, 0x00100010u), 0xDEADBEEFu);
    CHECK_EQ(bus_read32(&b, 0x00100014u), 0x0BADC0DEu);
    CHECK_EQ(b.sp.dmas, 2);
}

static void test_sp_status_and_pc(void) {
    printf("SP: halt/broke bits and SP_PC behave like registers\n");
    Bus b;
    bus_init(&b, NULL, 0);
    bus_write32(&b, 0x04040010u, 0x1u);        /* set halt */
    CHECK_EQ(bus_read32(&b, 0x04040010u) & 1u, 1u);
    bus_write32(&b, 0x04040010u, 0x2u);        /* clear halt */
    CHECK_EQ(bus_read32(&b, 0x04040010u) & 1u, 0u);
    bus_write32(&b, 0x04040010u, 0x4u);        /* set broke */
    CHECK_EQ(bus_read32(&b, 0x04040010u) & 2u, 2u);

    bus_write32(&b, 0x04080000u, 0x04001040u); /* SP_PC */
    CHECK_EQ(bus_read32(&b, 0x04080000u), 0x04001040u);
}

/* ------------------------------------------------------------------ RSP */

static void load_ucode(Bus *b, const uint32_t *p, int n) {
    bus_init(b, NULL, 0);
    for (int i = 0; i < n; i++) bus_write32(b, 0x04001000u + (uint32_t)i * 4u, p[i]);
}

static void test_rsp_alu(void) {
    printf("RSP: mult/mflo through DMEM, then a branch without delay slots\n");
    uint32_t p[] = {
        enc_i(0x09, 0, 1, 5),          /* addiu $1, $0, 5 */
        enc_i(0x09, 0, 2, 7),          /* addiu $2, $0, 7 */
        enc_r(1, 2, 0, 0, 0x18),       /* mult  $1, $2 */
        enc_r(0, 0, 3, 0, 0x12),       /* mflo  $3 */
        enc_i(0x2B, 0, 3, 0x10),       /* sw    $3, 0x10($0) */
        enc_i(0x23, 0, 4, 0x10),       /* lw    $4, 0x10($0) */
        enc_i(0x04, 4, 3, 0x02),       /* beq   $4, $3, +8 bytes (imm is in words) */
        enc_i(0x09, 0, 5, 99),         /* skipped: the RSP has no delay slot */
        enc_i(0x09, 0, 6, 1),          /* skipped */
        0x0000000Du,                   /* break */
    };
    Bus b;
    load_ucode(&b, p, (int)(sizeof p / sizeof p[0]));
    Rsp r;
    rsp_reset(&r, 0x04001000u);
    rsp_run(&r, &b, 64);

    CHECK_EQ(r.gpr[3], 35);
    CHECK_EQ(r.gpr[4], 35);
    CHECK_EQ(bus_read32(&b, 0x04000010u), 35);
    CHECK_EQ(r.gpr[5], 0);
    CHECK_EQ(r.gpr[6], 0);
    CHECK_EQ(r.halted, 1);
    CHECK(!strcmp(r.halt_reason, "break"));
    CHECK_EQ(r.steps, 7); /* seven retired; the break is the eighth and halts */
}

static void test_rsp_faults(void) {
    printf("RSP: vector ops and bad addresses halt with a reason\n");
    uint32_t vec[] = {0x4A000000u}; /* COP2 */
    Bus b;
    load_ucode(&b, vec, 1);
    Rsp r;
    rsp_reset(&r, 0x04001000u);
    rsp_run(&r, &b, 8);
    CHECK_EQ(r.halted, 1);
    CHECK(strstr(r.halt_reason, "COP2") != NULL);

    /* 0xA5000000 masks to physical 0x05000000, which is past the RSP's 8 KiB:
     * 0xA0000000 would not do, since kseg1 aliases it onto physical 0 (DMEM). */
    uint32_t far_load[] = {
        enc_i(0x0F, 0, 1, 0xA500),     /* lui $1, 0xA500: nothing there */
        enc_i(0x23, 1, 2, 0x000),      /* lw  $2, 0($1) */
    };
    load_ucode(&b, far_load, 2);
    rsp_reset(&r, 0x04001000u);
    rsp_run(&r, &b, 8);
    CHECK_EQ(r.halted, 1);
    CHECK(strstr(r.halt_reason, "LW outside DMEM") != NULL);
}

/* ------------------------------------------------------- synthetic task */

/* A ucode that turns the three words in DMEM 0x000 into a display list at
 * DMEM 0x100 and DMAs it to the task's output address:
 *
 *   0x100  the task's first data word, unchanged
 *   0x104  the second data word, OR'd with G_TRI1 (0xBF000000)
 *   0x108  G_ENDDL (0xB8000000)
 */
static void build_ucode(uint32_t *p) {
    p[0]  = enc_i(0x23, 0, 8, 0x000);      /* lw    $t0, 0($0) */
    p[1]  = enc_i(0x23, 0, 9, 0x004);      /* lw    $t1, 4($0) */
    p[2]  = enc_i(0x2B, 0, 8, 0x100);      /* sw    $t0, 0x100($0) */
    p[3]  = enc_i(0x0F, 0, 10, 0xBF00);    /* lui   $t2, 0xBF00 */
    p[4]  = enc_r(9, 10, 9, 0, 0x25);      /* or    $t1, $t1, $t2 */
    p[5]  = enc_i(0x2B, 0, 9, 0x104);      /* sw    $t1, 0x104($0) */
    p[6]  = enc_i(0x0F, 0, 11, 0xB800);    /* lui   $t3, 0xB800 */
    p[7]  = enc_i(0x2B, 0, 11, 0x108);     /* sw    $t3, 0x108($0) */
    p[8]  = enc_i(0x0F, 0, 12, 0xA404);    /* lui   $t4, 0xA404: SP registers */
    p[9]  = enc_i(0x09, 0, 13, 0x100);     /* addiu $t5, $0, 0x100 */
    p[10] = enc_i(0x2B, 12, 13, 0x00);     /* sw    $t5, 0($t4): SP_MEM_ADDR */
    p[11] = enc_i(0x23, 0, 14, 0x008);     /* lw    $t6, 8($0): output address */
    p[12] = enc_i(0x2B, 12, 14, 0x04);     /* sw    $t6, 4($t4): SP_DRAM_ADDR */
    p[13] = enc_i(0x09, 0, 15, 11);        /* addiu $t7, $0, 11: twelve bytes */
    p[14] = enc_i(0x2B, 12, 15, 0x0C);     /* sw    $t7, 12($t4): SP_WR_LEN */
    p[15] = 0x0000000Du;                   /* break */
}

static void test_ostask_display_list(void) {
    printf("task: ucode DMA'd into IMEM builds a display list in DMEM\n");
    uint32_t ucode[16];
    build_ucode(ucode);

    Bus b;
    bus_init(&b, NULL, 0);
    for (int i = 0; i < 16; i++)
        bus_write32(&b, 0x00110000u + (uint32_t)i * 4u, ucode[i]); /* ucode in RDRAM */
    bus_write32(&b, 0x00120000u, 0xFA3F2F1Fu);                     /* SETPRIMCOLOR-ish */
    bus_write32(&b, 0x00120004u, 0x00000112u);                     /* vertex indices */
    bus_write32(&b, 0x00120008u, 0x00130000u);                     /* where to DMA the list */

    OsTask t;
    memset(&t, 0, sizeof t);
    t.type = 1;
    t.ucode = 0x00110000u;
    t.ucode_data = 0x00121000u;
    t.output_buff = 0x00140000u;
    t.output_buff_size = 0x40;
    t.data_ptr = 0x00120000u;
    t.data_size = 12;

    OsTaskResult res;
    char err[128];
    int rc = ostask_run(&b, &t, &res, err, sizeof err);

    CHECK_EQ(rc, 0);
    if (rc != 0) printf("  ostask_run: %s\n", err);
    CHECK_EQ(res.halted, 1);
    CHECK(!strcmp(res.reason, "break"));

    /* the ucode really was loaded by DMA, not left on the CPU side */
    CHECK_EQ(bus_read32(&b, 0x04001000u), ucode[0]);
    CHECK_EQ(bus_read32(&b, 0x0400103Cu), ucode[15]);

    /* the display list, in DMEM where the ucode built it */
    CHECK_EQ(bus_read32(&b, 0x04000100u), 0xFA3F2F1Fu);
    CHECK_EQ(bus_read32(&b, 0x04000104u), 0xBF000112u);
    CHECK_EQ(bus_read32(&b, 0x04000108u), 0xB8000000u);

    /* and the copy the ucode DMA'd out to RDRAM itself */
    CHECK_EQ(bus_read32(&b, 0x00130000u), 0xFA3F2F1Fu);
    CHECK_EQ(bus_read32(&b, 0x00130004u), 0xBF000112u);
    CHECK_EQ(bus_read32(&b, 0x00130008u), 0xB8000000u);

    /* ucode, data, and the output write at least: nothing else touched RDRAM */
    CHECK(b.sp.dmas >= 3);
    CHECK_EQ(bus_read32(&b, 0x0013000Cu), 0u); /* the DMA stopped after 12 bytes */
}

static void test_ostask_rejects_empty(void) {
    printf("task: a task with no ucode is rejected, not run\n");
    Bus b;
    bus_init(&b, NULL, 0);
    OsTask t;
    memset(&t, 0, sizeof t);
    OsTaskResult res;
    char err[128];
    CHECK_EQ(ostask_run(&b, &t, &res, err, sizeof err), -1);
    CHECK(strstr(err, "ucode") != NULL);
}

int main(void) {
    printf("n64-player M2 tests\n\n");
    test_sp_dma();
    test_sp_status_and_pc();
    test_rsp_alu();
    test_rsp_faults();
    test_ostask_display_list();
    test_ostask_rejects_empty();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
