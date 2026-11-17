# n64-player

An N64 emulator core in portable C11, built against a written milestone plan
rather than by guessing at subsystems. No dependencies beyond `-lm`.

**Status: M0, M1 and M2.** `make test` runs 150 checks across three binaries with
0 failures, compiled with `-Wall -Wextra` and no warnings. Every check drives the
emulator with hand-encoded code — no cartridge data is used or stored here.

## Drop a cartridge in

```sh
make
./n64player your-dump.z64
```

or, equivalently:

```sh
make run ROM=your-dump.z64
```

The boot path executes the cart's **own IPL3** out of DMEM, where IPL1 would have
put it, and lets it drive PI DMA and jump to the entry point. What this tree
cannot supply is IPL1 itself and the CIC seed block it writes to DMEM
`0x00-0x3F`, so a cart whose bootcode checks that seed stops there. The tool
reports which of those happened, and where execution stopped, rather than
freezing silently:

```
CIC            6102
boot           cart IPL3 at DMEM 0xA4000040
               PIF seed block (DMEM 0x00-0x3F) zeroed: IPL1 is not here
...
entry point    0x80000400 after 41204 instructions
stopped        unimplemented opcode 0x4a (COP2 vector)
```

`--skip-ipl3` bypasses the boot path and starts at the header entry point, which
is useful for running game code when the bootcode will not pass its seed check.

## What works

| Unit | File |
|---|---|
| Big-endian RDRAM, cart window, PI registers and DMA | `src/bus.c` |
| SP registers, IMEM/DMEM, DMA in both directions | `src/bus.c` |
| ROM loading with z64/v64/n64 byte-order normalisation | `src/rom.c` |
| R4300i integer core: delay slots, exceptions, unaligned access | `src/cpu.c` |
| COP1 FPU: arithmetic, converts, 16 compares, `bc1f`/`bc1t` | `src/cpu.c` |
| TLB: `TLBWI`/`TLBWR`/`TLBP`/`TLBR`, 4 KiB translation, refill vector | `src/cpu.c` |
| RSP scalar core: 32-bit MIPS, no delay slots, `break` halts | `src/rsp.c` |
| `OSTask` dispatch: ucode to IMEM, data to DMEM, run to break | `src/rsp.c` |
| Boot: cart IPL3 from DMEM, or `--skip-ipl3` | `src/boot.c` |
| CLI: header dump, tracing, register dump | `src/main.c` |

## What it cannot do

- **No frame has ever been rasterized.** The RDP is M3. A task's display list
  now lands in RDRAM, but nothing consumes it.
- **No COP2 vector instructions.** The RSP's scalar half runs; its vector unit
  does not exist, so real gfx or audio ucode stops at its first `vmov`.
- **No audio, controller or VI.** M3 and M4.
- **No boot ROM.** `0x1FC00000` reads as zero, and the CIC seed block is zeroed.

`docs/emulator-plan.md` records the plan, each milestone's exit criteria, and the
known simplifications (4 KiB pages, no ASID compare, synchronous PI and SP DMA,
`Count` per instruction, compare-bit-only FCR31).

## Build and test

```sh
make          # builds ./n64player
make test     # builds and runs all three test binaries
```

## Layout

```
src/n64.h      shared types: Bus, Cpu, Sp, Rsp, OsTask, Rom
src/bus.c      address map, RDRAM, PI and SP DMA, device stubs
src/cpu.c      decode, translate, COP0/COP1, exceptions, disassembler
src/rsp.c      RSP scalar core and OSTask dispatch
src/boot.c     boot path and CIC identification
src/rom.c      ROM validation and byte-order normalisation
src/main.c     n64player CLI
tests/         test_cpu (M0/M1), test_rsp (M2), test_boot (synthetic cart)
docs/          milestone plan and the GitHub Pages site
```

## Licence

No licence file yet. Until one is added, the default is all rights reserved;
say which licence you want and it goes in.
