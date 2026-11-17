# N64 player — milestone plan

A cartridge-accurate enough N64 emulator to take a `.n64`/`.z64` dumped from your
own console and run it. Written down before the code because the failure mode of
this kind of project is a CPU core that passes its unit tests and cannot boot
anything, followed by months of guessing which of the seven subsystems is wrong.

**Status: M0, M1 and M2 are implemented and tested** — 150 checks across three
binaries, 0 failures. M1's exit criterion is still blocked on the boot ROM and
the CIC seed, neither of which this tree ships. M3 onwards are not started.

## What M0 is (in this tree)

| Piece | File | State |
|---|---|---|
| Big-endian RDRAM, cart window, register stubs | `src/bus.c` | done |
| ROM load, byte-order normalisation (z64/v64/n64), header parse | `src/rom.c` | done |
| R4300i integer core, delay slots, exceptions, unaligned access | `src/cpu.c` | done |
| Trace disassembler | `src/cpu.c` (`cpu_describe`) | partial |
| COP1: single/double arithmetic, converts, compares, BC1F/T | `src/cpu.c` (`fpu_arith`) | done |
| TLB: TLBWI/TLBWR/TLBP/TLBR, 4 KiB translation, refill vector | `src/cpu.c` (`translate`, `op_cop0`) | done |
| PI registers and a synchronous DMA | `src/bus.c` (`bus_pi_write`) | done |
| SP registers, IMEM/DMEM, DMA both ways | `src/bus.c` (`bus_sp_write`) | done |
| RSP scalar core, 32-bit, no delay slots | `src/rsp.c` | done |
| `OSTask` load and dispatch | `src/rsp.c` (`ostask_run`) | done |
| Boot path: the cart's IPL3 from DMEM, or `--skip-ipl3` | `src/boot.c` | done |
| Instruction-stream test suite (no ROM needed) | `tests/test_cpu.c` | done |
| SP/RSP/task test suite | `tests/test_rsp.c` | done |
| Boot-path test with a synthetic cart | `tests/test_boot.c` | done |

Exit criteria met: `make test` passes; branches execute their delay slots; 32-bit
results sign-extend; RDRAM stores are big-endian; `lwl`/`lwr` and `swl`/`swr`
reassemble unaligned words; `mult`/`div`/`divu` land in `hi`/`lo` by the
architectural rules including divide-by-zero; an `add` overflow raises exception
code 12 with the right EPC; all three ROM byte orders normalise to the same image.

Verified in M1 as well: `cvt.d.s`/`add.d`/`dmfc1` round-trip a double,
`c.lt.d` plus `bc1t` branches through its delay slot, `cvt.s.w`/`add.s`/`cvt.w.s`
returns an integer, `PI_WR_LEN` copies cart bytes into RDRAM, `TLBWI` maps a page
that `lw` then reads through, an unmapped kuseg load takes the refill vector at
`0x80000000` with `ExcCode` 2, and `Count` advances once per instruction.

Still halting with a named reason, rather than guessing: COP2 (RSP),
`LDL`/`LDR`/`SDL`/`SDR`, LL/SC reservations. The boot path runs the cart's own
IPL3 out of DMEM; what it cannot supply is IPL1 and the CIC seed block, so a cart
that checks its seed stops there and says so.

Known simplifications written down so they are not mistaken for accuracy:
4 KiB pages only (no page-mask pairs), no ASID comparison, synchronous PI
completion, `Count` counted per instruction, and only the compare bit of FCR31
(the FPU exception model is absent).

## M1 — boot to RDRAM

The first real milestone, and the one that decides whether the rest is worth
building. Exit criteria: a commercial cart's bootcode runs past its first
megabyte and jumps into its own entry point with a plausible stack.

Done in this tree: COP1 (single and double arithmetic, converts, compares,
`mfc1`/`mtc1`, rounding modes), the TLB (`TLBWI`/`TLBWR`/`TLBP`/`TLBR` with
4 KiB pages and the refill vector), PI DMA (`PI_DRAM_ADDR`, `PI_CART_ADDR`,
`PI_WR_LEN`, `PI_STATUS`), and exceptions through the BEV/EXL vectors.

Still blocking the exit criterion, because it needs input this repository must
not contain:

- **The PIF ROM (IPL1).** Nintendo's, and not shippable, so `0x1FC00000` reads as
  zero and the boot path stops there.
- **The CIC handshake.** The 6101/6102/6103/6105/6106 variants differ in the
  bootcode CRC they check and in how their bootcode is encrypted, so CIC
  identification (from `crc1`/`crc2` in the header) has to select the right path.
  The algorithm is implementable; the key material is not ours to include.
- **IPL3 itself.** Done in M2: the cart's bootcode is copied into DMEM where
  IPL1 would have put it and executes there, driving PI DMA for real. Its
  checksum write-back to `0x80000318` and its seed check remain unverified
  without a cartridge, and the seed cannot pass with IPL1 absent.
- **A real `Count`.** A cycle counter at 93.75 MHz, not one per instruction.

Verification: the bootcode's own progress is the test. A ROM that reaches its
entry point prints through its own debug path; no ROM-specific hacks.

## M2 — RSP and task dispatch

- Scalar RSP core (SP is a MIPS R4000 variant without caches, 4 KiB IMEM/DMEM).
- `OSTask` dispatch: `osSpTaskStartGo`, SP registers, SP DMA to/from RDRAM.
- The DP command list handoff (`D_ENDDL`), so RDP work reaches M3.
- First test: a known task produces a known display list in DMEM, checked by
  diffing against a recorded expectation rather than "it didn't crash".

Done: the scalar core (integer ALU, multiplies, loads/stores, branches, `break`
with no delay slots), SP DMA in both directions, the SP register block including
`SP_PC` and the halt/broke bits, and `ostask_run` — ucode to IMEM, ucode_data to
DMEM 0x800, data to DMEM 0, run until the ucode breaks.

Still open in M2, and honestly listed rather than implied by the word "done":

- **COP2 vector instructions.** The scalar half of the RSP runs ucode; the vector
  unit does not exist, so any real gfx or audio ucode stops at its first `vmov`.
  That is the largest remaining piece of the RSP and it belongs with M3.
- **The `D_ENDDL` handoff.** A task's display list now lands in RDRAM, but nothing
  consumes it: the RDP is M3.
- **Interrupts.** `SP_STATUS` interrupt bits are stored; no interrupt is ever
  raised, because there is no scheduler until M4.

## M3 — RDP and first frame

- Rasterizer: triangle setup, span walking, the three-cycle and one-cycle modes,
  scissor/coverage, z-buffer and dithering.
- Texture pipeline: RGBA16/32, IA4/8/16, I4/8, CI4/8 with TLUT, `gDPLoadTextureBlock`
  variants and tiles; miptile and mipmap LOD selection.
- Combine modes: the two-cycle blender is the part that makes OoT/MM look right;
  it needs the 8-channel mux implemented as a table evaluator, not per-game cases.
- VI scanout: `VI_CTRL` (pixel size, type, interlace), timing registers, the
  status/current-line pair. First exit criterion: a screenshot of the boot logo
  that matches a reference frame.

Then the parts that turn frames into a program you can navigate:

- PIF controller reads and the joybus protocol; controller pak as a stub.
- Audio: the RSP audio ucode (`aspMain`, `n_aspMain`) plus AI DMA at 48 kHz.
- Save types: SRAM, FlashRAM, EEPROM 4k/16k, autodetected from the save type or
  chosen explicitly.
- Timing: VI at 60/50 Hz, AI interrupts, `osViSwapBuffer` pacing, frameskip.

## M4 — correctness harness

- Frame-hash regression: render N frames from a save state and compare hashes so
  a fix in the blender cannot silently break another scene.
- Save states: full machine serialisation (RDRAM, RSP DMEM/IMEM, RDP state,
  devices), versioned.
- Determinism: fixed instruction/cycle accounting so a state replays identically.

## M5 — speed

Only after correctness, and only with measurements:

- Block-level translation cache for the R4300i (decode once per basic block).
- RSP JIT or ucode-specific fast paths for the audio and gfx microcodes.
- Optional threaded RDP replay, which is the same trick the TotK port uses and
  has the same hazard: one frame of latency for overlapped work.

## Non-goals

- No ROM distribution of any kind: the emulator never ships a game, a key or a
  bootcode, and no test in this tree uses copyrighted data.
- No accuracy claims beyond what `make test` and the frame-hash suite prove.
- 64DD and the Transfer Pak stay stubs until a game is actually blocked on them.

## Risks, in the order they will bite

1. **CIC variants.** Getting the 6105 bootcode wrong looks exactly like "the CPU
   core is fine but nothing runs". Detect it early with `crc1`/`crc2`.
2. **Combine mode.** The RDP blender has more cases than any other unit; without
   a table-driven implementation it becomes dozens of per-game patches.
3. **Timing.** Audio pitch and VI pacing both come from cycle counts; a free-running
   `Count` makes them drift apart slowly and is hard to spot in a short test.
4. **No hardware oracle.** Every milestone is verified against recordings made
   from your own cartridge and console or a known-good reference implementation —
   never against "the game looks right today".
