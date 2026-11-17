/* main.c — n64player: point it at a cartridge and it boots it.
 *
 *   ./n64player your-dump.z64
 *
 * It runs the cart's own IPL3 out of DMEM, reports where execution got to, and
 * stops with a named reason rather than a silent freeze. What it cannot supply
 * is IPL1 (the PIF ROM) and the CIC seed block it writes, so a cart that checks
 * its seed will not get through; --skip-ipl3 bypasses the boot path entirely and
 * starts at the header entry point.
 */
#include "n64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_STEPS 1000000ull

static void usage(const char *argv0) {
    printf("usage: %s <rom.z64|rom.n64|rom.v64> [options]\n\n", argv0);
    printf("  --dump              print the parsed header and exit\n");
    printf("  --skip-ipl3         start at the header entry point instead of running IPL3\n");
    printf("  --run N             execute at most N instructions (default %llu)\n",
           (unsigned long long)DEFAULT_STEPS);
    printf("  --trace             print each instruction while running\n");
    printf("  --regs              print the register file at the end\n\n");
    printf("The CPU, FPU, TLB, PI DMA, SP DMA and the RSP scalar core all work.\n");
    printf("IPL1 and the CIC seed block are not in this tree, so the boot path can\n");
    printf("reach the entry point but cannot pass a cart's own seed check.\n");
}

static void dump_regs(const Cpu *c) {
    static const char *const n[32] = {"r0", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1",
                                      "t2", "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3",
                                      "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp",
                                      "fp", "ra"};
    for (int i = 0; i < 32; i += 2)
        printf("  %-3s %016llx   %-3s %016llx\n", n[i], (unsigned long long)c->gpr[i], n[i + 1],
               (unsigned long long)c->gpr[i + 1]);
    printf("  pc %08x  hi %016llx  lo %016llx\n", c->pc, (unsigned long long)c->hi,
           (unsigned long long)c->lo);
    printf("  status %08x  cause %08x  epc %08x\n", c->cop0[12], c->cop0[13], c->cop0[14]);
}

int main(int argc, char **argv) {
    const char *path = NULL;
    uint64_t limit = DEFAULT_STEPS;
    int trace = 0, regs = 0, dump = 0, skip_ipl3 = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--run") && i + 1 < argc) {
            limit = strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--trace")) {
            trace = 1;
        } else if (!strcmp(argv[i], "--regs")) {
            regs = 1;
        } else if (!strcmp(argv[i], "--dump")) {
            dump = 1;
        } else if (!strcmp(argv[i], "--skip-ipl3")) {
            skip_ipl3 = 1;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        } else if (!path) {
            path = argv[i];
        } else {
            fprintf(stderr, "more than one ROM given\n");
            return 2;
        }
    }
    if (!path) {
        usage(argv[0]);
        return 2;
    }

    Rom rom;
    char err[256];
    if (rom_load(path, &rom, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }

    printf("%-14s %s\n", "file", path);
    printf("%-14s %u bytes\n", "size", rom.size);
    printf("%-14s %s\n", "byte order", rom_order_name(rom.order));
    printf("%-14s %08x\n", "entry", rom.entry);
    printf("%-14s \"%s\"\n", "title", rom.title);
    printf("%-14s %08x %08x\n", "bootcode CRC", rom.crc1, rom.crc2);

    if (dump) {
        rom_free(&rom);
        return 0;
    }

    Bus bus;
    Cpu cpu;
    BootInfo boot;
    int rc = boot_cart(&bus, &cpu, &rom, skip_ipl3, &boot);
    if (rc != 0 && !skip_ipl3) {
        fprintf(stderr, "error: %s is too small to contain IPL3 (0xFC0 bytes at 0x40)\n", path);
        rom_free(&rom);
        return 1;
    }
    if (rc != 0) {
        fprintf(stderr, "error: cannot set up the boot path\n");
        rom_free(&rom);
        return 1;
    }

    printf("%-14s %s\n", "CIC",
           boot.cic ? boot.cic : "unrecognised (header CRCs match no known variant)");
    if (boot.ran_ipl3) {
        printf("%-14s %s\n", "boot", "cart IPL3 at DMEM 0xA4000040");
        printf("%-14s %s\n", "", "PIF seed block (DMEM 0x00-0x3F) zeroed: IPL1 is not here");
    } else {
        printf("%-14s %s\n", "boot", "entry point preloaded (--skip-ipl3)");
    }

    uint64_t i;
    int reached = 0;
    uint64_t reached_at = 0;
    for (i = 0; i < limit; i++) {
        if (!reached && cpu.pc == rom.entry) {
            reached = 1;
            reached_at = i;
        }
        if (trace) {
            uint32_t ins = bus_read32(&bus, cpu.pc & 0x1FFFFFFFu);
            char text[64];
            cpu_describe(cpu.pc, ins, text, sizeof text);
            printf("%08llx  %08x  %s\n", (unsigned long long)cpu.steps, ins, text);
        }
        if (!cpu_step(&cpu, &bus)) break;
    }
    if (!reached && cpu.pc == rom.entry) {
        reached = 1;
        reached_at = cpu.steps;
    }

    printf("\n");
    printf("%-14s %llu\n", "ran", (unsigned long long)cpu.steps);
    if (reached)
        printf("%-14s 0x%08x after %llu instructions\n", "entry point", rom.entry,
               (unsigned long long)reached_at);
    else
        printf("%-14s not reached\n", "entry point");
    printf("%-14s %s\n", "stopped", cpu.halted ? cpu.halt_reason : "step limit reached");
    if (boot.ran_ipl3 && !reached)
        printf("%-14s %s\n", "", "IPL3 did not get through: see the seed note above");
    if (bus.unmapped_reads || bus.unmapped_writes)
        printf("%-14s %llu reads, %llu writes\n", "unmapped",
               (unsigned long long)bus.unmapped_reads, (unsigned long long)bus.unmapped_writes);
    printf("%-14s %llu IPI", "devices", (unsigned long long)bus.pi.dmas);
    printf("  %llu SP\n", (unsigned long long)bus.sp.dmas);
    if (regs) dump_regs(&cpu);

    rom_free(&rom);
    return 0;
}
