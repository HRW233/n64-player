# n64-player

An N64 emulator core in portable C11, built against a written milestone plan
rather than by guessing at subsystems. No dependencies beyond `-lm`.

**Status: M0 complete, and the testable half of M1.** `make test` runs 92 checks
with 0 failures, compiled with `-Wall -Wextra` and no warnings. Every check
drives the CPU with hand-encoded instruction streams — no cartridge is used, and
none is distributed here.

## What works

| Unit | File |
|---|---|
| Big-endian RDRAM, cart window, PI registers and DMA | `src/bus.c` |
| ROM loading with z64/v64/n64 byte-order normalisation | `src/rom.c` |
| R4300i integer core: delay slots, exceptions, unaligned access | `src/cpu.c` |
| COP1 FPU: arithmetic, converts, 16 compares, `bc1f`/`bc1t` | `src/cpu.c` |
| TLB: `TLBWI`/`TLBWR`/`TLBP`/`TLBR`, 4 KiB translation, refill vector | `src/cpu.c` |
| CLI: header dump, tracing, register dump | `src/main.c` |

## What it cannot do

It does not boot a commercial cart, and cannot be made to here: the PIF/IPL1
boot ROM is Nintendo's and is not shippable, and the 6105 CIC bootcode is
encrypted with key material that does not belong in a repository. `0x1FC00000`
reads as zero, so execution stops before the first cart instruction.

RSP, RDP, audio and controller support are milestones M2 onwards and are not
started. `docs/emulator-plan.md` records the full plan, the exit criteria for
each milestone, and the known simplifications (4 KiB pages, no ASID compare,
synchronous PI completion, `Count` per instruction, compare-bit-only FCR31).

## Build

```sh
make          # builds ./n64info
make test     # builds and runs build/test_cpu
```

```sh
./n64info --help
./n64info rom.z64 --dump
./n64info rom.z64 --run 1000 --trace --regs
```

## Layout

```
src/n64.h      shared types: Bus, Cpu, Rom
src/bus.c      address map, RDRAM, PI DMA, device stubs
src/cpu.c      decode, translate, COP0/COP1, exceptions, disassembler
src/rom.c      ROM validation and byte-order normalisation
src/main.c     n64info CLI
tests/         instruction-stream test suite, one binary
docs/          milestone plan and the GitHub Pages site
```

## Licence

No licence file yet. Until one is added, the default is all rights reserved;
say which licence you want and it goes in.
