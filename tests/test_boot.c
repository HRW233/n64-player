/* test_boot.c — the boot path, using a synthetic cartridge.
 *
 * The cart is written here: a header with 6102's CRCs, an IPL3 at offset 0x40
 * that does what the real one does first (PI DMA the cart into RDRAM) and then
 * jumps to the entry point, and four instructions of "game" at offset 0x1000.
 *
 * This does not prove a commercial cart boots. It proves the machinery a real
 * IPL3 needs is wired the way the hardware is: the bootcode is loaded into DMEM,
 * executed from its kseg1 alias, allowed to drive PI DMA with kseg1 register and
 * cart addresses, and its jump to 0x80000400 lands on the copied data.
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

static uint32_t enc_i(int op, int rs, int rt, int imm) {
    return ((uint32_t)op << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16) |
           ((uint32_t)imm & 0xFFFFu);
}

#define IPL3_WORDS 12
static uint32_t IPL3[IPL3_WORDS];

/* The first dozen instructions of a bootcode: copy the cart in, jump to the
 * entry point. Written with the CPU's delay slots in mind. Filled at runtime
 * because the encoders are functions, not constant expressions. */
static void build_ipl3(void) {
    uint32_t p[IPL3_WORDS] = {
        0x3C08A460u,                    /* lui  $8, 0xA460  PI registers (kseg1) */
        enc_i(0x0D, 0, 9, 0x0400),      /* ori  $9, $0, 0x0400  DRAM_ADDR */
        0xAD090000u,                    /* sw   $9, 0($8) */
        0x3C0AB000u,                    /* lui  $10, 0xB000  the cart through kseg1 */
        enc_i(0x0D, 10, 10, 0x1000),    /* ori  $10, $10, 0x1000 */
        0xAD0A0004u,                    /* sw   $10, 4($8) */
        enc_i(0x0D, 0, 11, 0x0FFF),     /* ori  $11, $0, 0x0FFF  len-1 = 4 KiB */
        0xAD0B000Cu,                    /* sw   $11, 12($8)  WR_LEN: cart -> RDRAM */
        0x3C0C8000u,                    /* lui  $12, 0x8000 */
        enc_i(0x0D, 12, 12, 0x0400),    /* ori  $12, $12, 0x0400  entry point */
        0x01800008u,                    /* jr   $12 */
        0x00000000u,                    /* nop  (delay slot) */
    };
    memcpy(IPL3, p, sizeof p);
}

static void write_synthetic_cart(const char *path) {
    uint8_t rom[0x2000];
    memset(rom, 0, sizeof rom);

    rom[0] = 0x80; rom[1] = 0x37; rom[2] = 0x12; rom[3] = 0x40;      /* .z64 magic */
    rom[0x08] = 0x80; rom[0x09] = 0x00; rom[0x0A] = 0x04; rom[0x0B] = 0x00;
    rom[0x10] = 0x90; rom[0x11] = 0xBB; rom[0x12] = 0x6C; rom[0x13] = 0xB5; /* 6102 */
    rom[0x14] = 0x0B; rom[0x15] = 0x05; rom[0x16] = 0x0E; rom[0x17] = 0xE0;
    memcpy(rom + 0x20, "SYNTHETIC BOOT      ", 20);
    rom[0x3E] = 'E';

    for (int i = 0; i < IPL3_WORDS; i++) {
        uint32_t w = IPL3[i];
        rom[0x40 + i * 4 + 0] = (uint8_t)(w >> 24);
        rom[0x40 + i * 4 + 1] = (uint8_t)(w >> 16);
        rom[0x40 + i * 4 + 2] = (uint8_t)(w >> 8);
        rom[0x40 + i * 4 + 3] = (uint8_t)w;
    }

    const uint32_t game[] = {
        enc_i(0x09, 0, 1, 42),      /* addiu $1, $0, 42 */
        0x0000000Du,                /* break */
    };
    for (int i = 0; i < 2; i++) {
        uint32_t w = game[i];
        rom[0x1000 + i * 4 + 0] = (uint8_t)(w >> 24);
        rom[0x1000 + i * 4 + 1] = (uint8_t)(w >> 16);
        rom[0x1000 + i * 4 + 2] = (uint8_t)(w >> 8);
        rom[0x1000 + i * 4 + 3] = (uint8_t)w;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("  FAIL cannot write %s\n", path);
        failures++;
        return;
    }
    fwrite(rom, 1, sizeof rom, f);
    fclose(f);
}

static void test_cic_names(void) {
    printf("boot: CIC variants are named from the header CRCs, or refused\n");
    CHECK(cic_name(0x90BB6CB5u, 0x0B050EE0u) && !strcmp(cic_name(0x90BB6CB5u, 0x0B050EE0u), "6102"));
    CHECK(cic_name(0x6170A4A1u, 0x90BB6CB5u) && !strcmp(cic_name(0x6170A4A1u, 0x90BB6CB5u), "6101"));
    CHECK(cic_name(0x12345678u, 0x9ABCDEF0u) == NULL);
}

static void test_ipl3_boot(void) {
    printf("boot: IPL3 in DMEM drives PI DMA and jumps to the entry point\n");
    write_synthetic_cart("build/boot.z64");

    Rom rom;
    char err[128];
    if (rom_load("build/boot.z64", &rom, err, sizeof err) != 0) {
        printf("  FAIL rom_load: %s\n", err);
        failures++;
        return;
    }

    Bus bus;
    Cpu cpu;
    BootInfo info;
    CHECK_EQ(boot_cart(&bus, &cpu, &rom, 0, &info), 0);
    CHECK_EQ(info.ran_ipl3, 1);
    CHECK(info.cic && !strcmp(info.cic, "6102"));
    CHECK_EQ(cpu.pc, 0xA4000040u);
    CHECK_EQ(bus_read32(&bus, 0x04000040u), IPL3[0]);   /* bootcode is in DMEM */
    CHECK_EQ(bus_read32(&bus, 0x04000000u), 0u);        /* seed block is zeroed */

    int reached = 0;
    for (int i = 0; i < 64 && !cpu.halted; i++) {
        if (cpu.pc == rom.entry) reached = 1;
        cpu_step(&cpu, &bus);
    }
    if (cpu.pc == rom.entry) reached = 1;

    CHECK_EQ(bus.pi.dmas, 1);
    CHECK_EQ(bus_read32(&bus, 0x00000400u), enc_i(0x09, 0, 1, 42)); /* cart 0x1000 -> RDRAM 0x400 */
    CHECK(reached);
    CHECK_EQ(cpu.gpr[1], 42);
    CHECK_EQ(cpu.halted, 1);
    CHECK(strstr(cpu.halt_reason, "BREAK") != NULL);
    rom_free(&rom);
}

static void test_skip_ipl3(void) {
    printf("boot: --skip-ipl3 preloads the entry point instead\n");
    write_synthetic_cart("build/boot2.z64");
    Rom rom;
    char err[128];
    if (rom_load("build/boot2.z64", &rom, err, sizeof err) != 0) {
        CHECK(0);
        return;
    }
    Bus bus;
    Cpu cpu;
    BootInfo info;
    CHECK_EQ(boot_cart(&bus, &cpu, &rom, 1, &info), 0);
    CHECK_EQ(info.ran_ipl3, 0);
    CHECK_EQ(info.load_pc, 0x80000400u);
    CHECK_EQ(cpu.pc, 0x80000400u);
    CHECK_EQ(bus_read32(&bus, 0x80000400u), enc_i(0x09, 0, 1, 42));
    cpu_step(&cpu, &bus);
    CHECK_EQ(cpu.gpr[1], 42);
    rom_free(&rom);
}

static void test_tiny_image(void) {
    printf("boot: an image too small to hold IPL3 is refused, not guessed at\n");
    FILE *f = fopen("build/tiny.z64", "wb");
    uint8_t rom[0x200];
    memset(rom, 0, sizeof rom);
    rom[0] = 0x80; rom[1] = 0x37; rom[2] = 0x12; rom[3] = 0x40;
    if (f) {
        fwrite(rom, 1, sizeof rom, f);
        fclose(f);
    }
    Rom r;
    char err[128];
    CHECK_EQ(rom_load("build/tiny.z64", &r, err, sizeof err), -1); /* rejected at load */
}

int main(void) {
    printf("n64-player boot tests\n\n");
    build_ipl3();
    test_cic_names();
    test_ipl3_boot();
    test_skip_ipl3();
    test_tiny_image();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
