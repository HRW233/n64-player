/* n64.h — shared types for the N64 player.
 *
 * M0: bus, cart, R4300i integer core.
 * M1 (this tree): COP1 FPU, TLB + address translation, PI DMA, exception vectors.
 */
#ifndef N64_H
#define N64_H

#include <stddef.h>
#include <stdint.h>

#define RDRAM_SIZE (4u * 1024u * 1024u) /* 4 MiB; expansion pak is still to do */
#define ROM_MAX (64u * 1024u * 1024u)
#define TLB_ENTRIES 32
#define SP_MEM_SIZE 0x1000u

/* ---------------------------------------------------------------- bus */

typedef struct {
    uint32_t dram_addr, cart_addr, rd_len, wr_len;
    uint32_t status;   /* bit 0: busy, bit 3: interrupt */
    uint64_t dmas;
} Pi;

/* RSP IMEM at 0x04001000 and DMEM at 0x04000000, both big-endian like RDRAM.
 * Transfers complete immediately; there is no DMA queue (see bus.c). */
typedef struct {
    uint8_t imem[SP_MEM_SIZE];
    uint8_t dmem[SP_MEM_SIZE];
    uint32_t mem_addr, dram_addr, rd_len, wr_len;
    uint32_t status;    /* bit 0 halted, bit 1 broke, bit 2 dma busy, bit 3 io full */
    uint32_t pc;        /* SP_PC: 0x04001000 + offset into IMEM */
    uint32_t semaphore;
    uint64_t dmas;
} Sp;

typedef struct {
    uint8_t rdram[RDRAM_SIZE];
    const uint8_t *cart;      /* normalised big-endian ROM image, at 0x10000000 */
    uint32_t cart_size;
    Sp sp;                    /* 0x04000000: DMEM, IMEM and the SP registers */
    Pi pi;                    /* 0x04600000: real registers, DMA executes at once */
    uint64_t unmapped_reads, unmapped_writes;
} Bus;

void bus_init(Bus *b, const uint8_t *cart, uint32_t cart_size);
uint8_t bus_read8(const Bus *b, uint32_t paddr);
uint16_t bus_read16(const Bus *b, uint32_t paddr);
uint32_t bus_read32(const Bus *b, uint32_t paddr);
uint64_t bus_read64(const Bus *b, uint32_t paddr);
void bus_write8(Bus *b, uint32_t paddr, uint8_t v);
void bus_write16(Bus *b, uint32_t paddr, uint16_t v);
void bus_write32(Bus *b, uint32_t paddr, uint32_t v);
void bus_write64(Bus *b, uint32_t paddr, uint64_t v);
/* True when paddr lies in RDRAM through any of its three windows. */
int bus_is_rdram(uint32_t paddr);
/* PI block, offset from 0x04600000. A write to WR_LEN/RD_LEN starts the transfer. */
uint32_t bus_pi_read(const Bus *b, uint32_t off);
void bus_pi_write(Bus *b, uint32_t off, uint32_t v);
/* SP block, offset from 0x04040000. A write to RD_LEN/WR_LEN starts a transfer. */
uint32_t bus_sp_read(const Bus *b, uint32_t off);
void bus_sp_write(Bus *b, uint32_t off, uint32_t v);

/* ---------------------------------------------------------------- cpu */

typedef struct {
    uint64_t gpr[32];   /* r0 is kept at zero by the core */
    uint64_t hi, lo;
    uint64_t fpr[32];   /* COP1: doubles in full, singles in the low 32 bits */
    uint32_t fcr31;     /* COP1 control: bits 0-1 rounding mode, bit 23 compare */
    uint32_t pc;
    uint32_t branch_target; /* destination of the branch whose delay slot is next */
    uint8_t delay;          /* the instruction at pc is a delay slot */
    uint32_t cop0[32];
    uint32_t tlb_ehi[TLB_ENTRIES], tlb_elo0[TLB_ENTRIES], tlb_elo1[TLB_ENTRIES];
    uint32_t tlb_pagemask[TLB_ENTRIES];
    uint8_t tlb_valid[TLB_ENTRIES];
    uint32_t tlb_random;    /* COP0 Random: counts down 31..8 */
    uint64_t tlb_hits, tlb_misses;
    uint8_t override_pending; /* an exception/ERET decided the next pc */
    uint32_t override_target;
    uint64_t steps;
    int halted;
    char halt_reason[96];
} Cpu;

void cpu_reset(Cpu *c, uint32_t entry);
/* Execute one instruction. Returns 0 when the core halted. */
int cpu_step(Cpu *c, Bus *b);
void cpu_halt(Cpu *c, const char *why);
/* Debug text for one instruction, e.g. "addiu a0, a0, 4". */
void cpu_describe(uint32_t pc, uint32_t ins, char *buf, size_t cap);

/* ---------------------------------------------------------------- rsp */

/* The scalar half of the RSP: a 32-bit MIPS with no delays slots, no COP1 and
 * no exceptions. Vector (COP2) instructions halt with a named reason. */
typedef struct {
    uint32_t gpr[32];
    uint32_t pc;        /* an address inside IMEM, normally 0x04001000 + offset */
    uint32_t hi, lo;
    int halted;
    char halt_reason[96];
    uint64_t steps;
} Rsp;

void rsp_reset(Rsp *r, uint32_t pc);
/* Execute one instruction. Returns 0 when the RSP halted. */
int rsp_step(Rsp *r, Bus *b);
void rsp_halt(Rsp *r, const char *why);
/* Run until the RSP halts or max_steps pass. Returns instructions executed. */
uint64_t rsp_run(Rsp *r, Bus *b, uint64_t max_steps);

/* ---------------------------------------------------------------- task */

/* OSTask as libultra lays it out in RDRAM. Only the fields the dispatch uses. */
typedef struct {
    uint32_t type, flags, ucode, ucode_data, dram_stack,
             output_buff, output_buff_size, data_ptr, data_size;
} OsTask;

typedef struct {
    int halted;         /* the ucode ended with a break */
    uint64_t steps;
    uint32_t pc;
    char reason[128];
} OsTaskResult;

int ostask_load(const Bus *b, uint32_t addr, OsTask *out);
/* Load the ucode and its data with SP DMA, run it, and report how it stopped.
 * Returns 0 when the ucode ran to a break, -1 on a bad task or RSP fault. */
int ostask_run(Bus *b, const OsTask *t, OsTaskResult *res, char *err, size_t cap);

/* ---------------------------------------------------------------- rom */

enum { ROM_ORDER_Z64 = 0, ROM_ORDER_V64, ROM_ORDER_N64 };

typedef struct {
    uint8_t *data;       /* normalised to big-endian (.z64) layout */
    uint32_t size;
    int order;           /* as found on disk */
    uint32_t entry;      /* header 0x08: RDRAM address IPL3 jumps to */
    uint32_t crc1, crc2; /* header 0x10, 0x14: IPL3 bootcode CRCs */
    char title[21];
    char region;         /* header 0x3E: 'E', 'J', 'P', or ' ' */
} Rom;

int rom_load(const char *path, Rom *out, char *err, size_t errcap);
void rom_free(Rom *r);
const char *rom_order_name(int order);

/* ---------------------------------------------------------------- boot */

typedef struct {
    int ran_ipl3;        /* 1 when the cart's own bootcode is executing */
    const char *cic;     /* NULL when the header CRCs match nothing known */
    uint32_t load_pc;
} BootInfo;

/* Set up the bus and CPU for a cartridge: either run the cart's own IPL3 out of
 * DMEM, or (skip_ipl3) preload the first megabyte and start at the entry point.
 * Returns -1 when the image is too small to contain IPL3. */
int boot_cart(Bus *b, Cpu *c, const Rom *rom, int skip_ipl3, BootInfo *info);
const char *cic_name(uint32_t crc1, uint32_t crc2);

#endif /* N64_H */
