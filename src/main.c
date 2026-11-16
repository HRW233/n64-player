/* main.c — n64info: inspect a cart, optionally run its first instructions.
 *
 * This is the M0 harness. It deliberately does NOT claim to boot a game: the
 * IPL3 step here is an approximation (copy 1 MiB from ROM offset 0x1000 to
 * 0x80000400, then jump to the header entry point). Real bootcode needs the CIC
 * handshake and PI DMA, which is milestone M1.
 */
#include "n64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0) {
    printf("usage: %s <rom.n64|rom.z64|rom.v64> [options]\n\n", argv0);
    printf("  --dump              print the parsed header and exit\n");
    printf("  --run N             execute at most N instructions after the IPL3 approximation\n");
    printf("  --trace             print each instruction while running (implies --run if unset)\n");
    printf("  --regs              print the register file at the end\n\n");
    printf("Scope: integer core, COP1 FPU, TLB, PI DMA. The CIC handshake and the boot\n");
    printf("ROM are not shipped, so a commercial ROM stops before its first instruction.\n");
}

static void dump_regs(const Cpu *c) {
    static const char *const n[32] = {"r0", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1",
                                      "t2", "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3",
                                      "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp",
                                      "fp", "ra"};
    for (int i = 0; i < 32; i += 2)
        printf("  %-3s %016llx   %-3s %016llx\n", n[i], (unsigned long long)c->gpr[i], n[i + 1],
               (unsigned long long)c->gpr[i + 1]);
    printf("  pc %08x  hi %016llx  lo %016llx\n", c->pc, (unsigned long long)c->hi, (unsigned long long)c->lo);
    printf("  status %08x  cause %08x  epc %08x\n", c->cop0[12], c->cop0[13], c->cop0[14]);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    const char *path = argv[1];
    long run = 0;
    int trace = 0, regs = 0;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--run") && i + 1 < argc) {
            run = strtol(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--trace")) {
            trace = 1;
            if (!run) run = 1000;
        } else if (!strcmp(argv[i], "--regs")) {
            regs = 1;
        } else if (!strcmp(argv[i], "--dump")) {
            run = 0;
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
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
    printf("%-14s %c (%s)\n", "region", rom.region, rom.region == 'E' ? "PAL" : rom.region == 'J' ? "NTSC-J" : rom.region == 'P' ? "NTSC-U" : "unset");

    if (run <= 0) {
        rom_free(&rom);
        return 0;
    }

    Bus bus;
    bus_init(&bus, rom.data, rom.size);

    /* IPL3 approximation: the real bootcode copies 1 MiB from 0x1000 to 0x80000400. */
    if (rom.size > 0x1000) {
        uint32_t n = rom.size - 0x1000;
        if (n > 0x100000u) n = 0x100000u;
        memcpy(bus.rdram + 0x400, rom.data + 0x1000, n);
    }

    Cpu cpu;
    cpu_reset(&cpu, rom.entry);

    long i;
    for (i = 0; i < run; i++) {
        if (trace) {
            uint32_t ins = bus_read32(&bus, cpu.pc);
            char text[64];
            cpu_describe(cpu.pc, ins, text, sizeof text);
            printf("%08llx  %08x  %s\n", (unsigned long long)(cpu.steps), ins, text);
        }
        if (!cpu_step(&cpu, &bus)) break;
    }
    printf("\nran %ld instructions, %llu steps total\n", i, (unsigned long long)cpu.steps);
    if (cpu.halted) printf("halted: %s\n", cpu.halt_reason);
    if (bus.unmapped_reads || bus.unmapped_writes)
        printf("unmapped: %llu reads, %llu writes\n", (unsigned long long)bus.unmapped_reads,
               (unsigned long long)bus.unmapped_writes);
    if (regs || cpu.halted) dump_regs(&cpu);

    rom_free(&rom);
    return 0;
}
