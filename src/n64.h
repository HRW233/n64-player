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

/* ---------------------------------------------------------------- bus */

typedef struct {
    uint32_t dram_addr, cart_addr, rd_len, wr_len;
    uint32_t status;   /* bit 0: busy, bit 3: interrupt */
    uint64_t dmas;
} Pi;

typedef struct {
    uint8_t rdram[RDRAM_SIZE];
    const uint8_t *cart;      /* normalised big-endian ROM image, at 0x10000000 */
    uint32_t cart_size;
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

#endif /* N64_H */
