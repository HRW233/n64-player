/* bus.c — the memory map the R4300i sees, in physical addresses.
 *
 * CPU code translates a virtual address first (see cpu.c translate()), so the
 * windows here are the physical ones: RDRAM from 0, the cart at 0x10000000, and
 * the register blocks. RDRAM is big-endian because the console is: a word
 * written to 0x00000000 lands as 12 34 56 78 in memory.
 *
 * The PI block is real enough to load data: writing WR_LEN (cart -> RDRAM, what
 * libultra calls OS_READ) or RD_LEN (RDRAM -> cart) performs the transfer
 * immediately and clears the busy bit. Real hardware takes ~2 us
 * per 64 bytes, which matters for timing-sensitive bootcode; M1 finishes the
 * transfer synchronously and M5 puts it on a timeline.
 */
#include "n64.h"

#include <string.h>

static inline int in_window(uint32_t a, uint32_t base, uint32_t len) {
    return (uint32_t)(a - base) < len;
}

/* kseg0/kseg1 are unmapped windows on the physical map, and bootcode runs from
 * both: IPL3 fetches instructions from 0xA4000040 (the kseg1 alias of DMEM).
 * Accepting either form here keeps the CPU free of address juggling on fetch
 * and makes traces print the address the program actually used. */
static inline uint32_t to_phys(uint32_t a) {
    if (a >= 0xA0000000u) return a - 0xA0000000u;
    if (a >= 0x80000000u) return a - 0x80000000u;
    return a;
}

void bus_init(Bus *b, const uint8_t *cart, uint32_t cart_size) {
    memset(b, 0, sizeof *b);
    b->cart = cart;
    b->cart_size = cart_size;
}

int bus_is_rdram(uint32_t a) {
    return a < RDRAM_SIZE || in_window(a, 0x80000000u, RDRAM_SIZE) || in_window(a, 0xA0000000u, RDRAM_SIZE);
}

static uint8_t rdram_byte(const Bus *b, uint32_t a) {
    if (a < RDRAM_SIZE) return b->rdram[a];
    if (in_window(a, 0x80000000u, RDRAM_SIZE)) return b->rdram[a - 0x80000000u];
    return b->rdram[a - 0xA0000000u];
}

uint8_t bus_read8(const Bus *b, uint32_t a) {
    a = to_phys(a);
    if (a < RDRAM_SIZE || in_window(a, 0x80000000u, RDRAM_SIZE) || in_window(a, 0xA0000000u, RDRAM_SIZE))
        return rdram_byte(b, a);
    if (b->cart && in_window(a, 0x10000000u, b->cart_size)) return b->cart[a - 0x10000000u];
    if (in_window(a, 0x04600000u, 0x40u)) return (uint8_t)(bus_pi_read(b, a - 0x04600000u) >> (8 * (3 - (a & 3))));
    if (in_window(a, 0x04000000u, 0x1000u)) return b->sp.dmem[a - 0x04000000u];
    if (in_window(a, 0x04001000u, 0x1000u)) return b->sp.imem[a - 0x04001000u];
    if (in_window(a, 0x04040000u, 0x20u)) return (uint8_t)(bus_sp_read(b, a - 0x04040000u) >> (8 * (3 - (a & 3))));
    if (in_window(a, 0x04080000u, 4u)) return (uint8_t)(b->sp.pc >> (8 * (3 - (a & 3))));
    if (in_window(a, 0xBFC00000u, 0x800u)) return 0;    /* IPL3 area: the bootcode itself is not shipped */
    if (in_window(a, 0x04400000u, 0x40u)) return 0;     /* VI registers */
    if (in_window(a, 0x04800000u, 0x40u)) return 0;     /* SI registers */
    if (in_window(a, 0x1FC00000u, 0x80000u)) return 0;  /* PIF ROM: needs the boot ROM (not shipped) */
    ((Bus *)b)->unmapped_reads++;
    return 0;
}

uint16_t bus_read16(const Bus *b, uint32_t a) {
    return (uint16_t)((bus_read8(b, a) << 8) | bus_read8(b, a + 1));
}

uint32_t bus_read32(const Bus *b, uint32_t a) {
    a = to_phys(a);
    if (in_window(a, 0x04600000u, 0x40u)) return bus_pi_read(b, a - 0x04600000u);
    if (in_window(a, 0x04040000u, 0x20u)) return bus_sp_read(b, a - 0x04040000u);
    if (in_window(a, 0x04080000u, 4u)) return b->sp.pc;
    return ((uint32_t)bus_read8(b, a) << 24) | ((uint32_t)bus_read8(b, a + 1) << 16) |
           ((uint32_t)bus_read8(b, a + 2) << 8) | bus_read8(b, a + 3);
}

uint64_t bus_read64(const Bus *b, uint32_t a) {
    a = to_phys(a);
    return ((uint64_t)bus_read32(b, a) << 32) | bus_read32(b, a + 4);
}

void bus_write8(Bus *b, uint32_t a, uint8_t v) {
    a = to_phys(a);
    if (a < RDRAM_SIZE || in_window(a, 0x80000000u, RDRAM_SIZE) || in_window(a, 0xA0000000u, RDRAM_SIZE)) {
        b->rdram[(a < RDRAM_SIZE) ? a : (a & (RDRAM_SIZE - 1))] = v;
        return;
    }
    if (in_window(a, 0x04000000u, 0x1000u)) { b->sp.dmem[a - 0x04000000u] = v; return; }
    if (in_window(a, 0x04001000u, 0x1000u)) { b->sp.imem[a - 0x04001000u] = v; return; }
    if (in_window(a, 0x04040000u, 0x20u)) return;      /* SP registers are 32-bit */
    if (in_window(a, 0x04400000u, 0x40u)) return;     /* VI: M3 */
    if (in_window(a, 0x04800000u, 0x40u)) return;     /* SI: M4 */
    if (in_window(a, 0x1FC00000u, 0x80000u)) return;  /* PIF: needs the boot ROM */
    b->unmapped_writes++;
}

void bus_write16(Bus *b, uint32_t a, uint16_t v) {
    a = to_phys(a);
    bus_write8(b, a, (uint8_t)(v >> 8));
    bus_write8(b, a + 1, (uint8_t)v);
}

void bus_write32(Bus *b, uint32_t a, uint32_t v) {
    a = to_phys(a);
    if (in_window(a, 0x04600000u, 0x40u)) {
        bus_pi_write(b, a - 0x04600000u, v);
        return;
    }
    if (in_window(a, 0x04040000u, 0x20u)) {
        bus_sp_write(b, a - 0x04040000u, v);
        return;
    }
    if (in_window(a, 0x04080000u, 4u)) {
        b->sp.pc = v;   /* SP_PC */
        return;
    }
    bus_write8(b, a, (uint8_t)(v >> 24));
    bus_write8(b, a + 1, (uint8_t)(v >> 16));
    bus_write8(b, a + 2, (uint8_t)(v >> 8));
    bus_write8(b, a + 3, (uint8_t)v);
}

void bus_write64(Bus *b, uint32_t a, uint64_t v) {
    a = to_phys(a);
    bus_write32(b, a, (uint32_t)(v >> 32));
    bus_write32(b, a + 4, (uint32_t)v);
}

/* ------------------------------------------------------------ PI / DMA */

uint32_t bus_pi_read(const Bus *b, uint32_t off) {
    switch (off & 0x3Fu) {
    case 0x00: return b->pi.dram_addr;
    case 0x04: return b->pi.cart_addr;
    case 0x08: return b->pi.rd_len;
    case 0x0C: return b->pi.wr_len;
    case 0x10: return b->pi.status;
    default: return 0;
    }
}

static void pi_dma(Bus *b, int to_dram, uint32_t len_reg) {
    uint32_t len = (len_reg & 0xFFFFFFu) + 1u;
    /* The registers hold whatever the caller wrote, and bootcode writes kseg1
     * aliases: 0xA0000400 and 0x00000400 mean the same place. */
    uint32_t dram = b->pi.dram_addr & 0x1FFFFFFFu;
    /* PI_CART_ADDR is a physical address, so bootcode writes 0x10001000 or its
     * kseg1 alias 0xB0001000 — not a bare offset into the image. Anything below
     * the cart window underflows and transfers nothing. */
    uint32_t cart = (b->pi.cart_addr & 0x1FFFFFFFu) - 0x10000000u;

    b->pi.status |= 1u; /* busy */
    for (uint32_t i = 0; i < len; i++) {
        uint8_t byte = 0;
        if (b->cart && cart + i < b->cart_size) byte = b->cart[cart + i];
        if (to_dram) b->rdram[(dram + i) & (RDRAM_SIZE - 1)] = byte;
    }
    b->pi.dmas++;
    b->pi.status &= ~1u;
}

void bus_pi_write(Bus *b, uint32_t off, uint32_t v) {
    switch (off & 0x3Fu) {
    case 0x00: b->pi.dram_addr = v; break;
    case 0x04: b->pi.cart_addr = v; break;
    /* libultra's OS_READ (cart -> RDRAM) writes PI_WR_LEN, and OS_WRITE
     * (RDRAM -> cart) writes PI_RD_LEN: the names describe the PI bus, not
     * the direction the game sees. Getting these two swapped boots nothing. */
    case 0x08: b->pi.rd_len = v; pi_dma(b, 0, v); break;   /* RDRAM -> cart */
    case 0x0C: b->pi.wr_len = v; pi_dma(b, 1, v); break;   /* cart -> RDRAM */
    case 0x10: b->pi.status &= ~(v & 0x7u); break;          /* clear interrupt bits */
    default: break;
    }
}

/* ------------------------------------------------------------ SP / DMA */

uint32_t bus_sp_read(const Bus *b, uint32_t off) {
    switch (off & 0x1Fu) {
    case 0x00: return b->sp.mem_addr;
    case 0x04: return b->sp.dram_addr;
    case 0x08: return b->sp.rd_len;
    case 0x0C: return b->sp.wr_len;
    case 0x10: return b->sp.status;
    case 0x14: return 0;   /* SP_DMA_FULL: nothing queues, so never full */
    case 0x18: return 0;   /* SP_DMA_BUSY: transfers finish inside the write */
    case 0x1C: return b->sp.semaphore;
    default: return 0;
    }
}

/* RSP address to a byte of IMEM/DMEM. Accepts 0xA4000000, 0x04000000 and the
 * bare 0x0000-0x1FFF offsets that ucode uses for its own memory. */
static uint8_t *sp_mem(Bus *b, uint32_t addr) {
    uint32_t p = addr & 0x1FFFFFFFu;
    if (p >= 0x04000000u) p -= 0x04000000u;
    if (p < 0x1000u) return &b->sp.dmem[p];
    if (p < 0x2000u) return &b->sp.imem[p - 0x1000u];
    return NULL;
}

static void sp_dma(Bus *b, int from_dram, uint32_t len_reg) {
    uint32_t len = (len_reg & 0xFFFu) + 1u;
    uint32_t dram = b->sp.dram_addr & 0x1FFFFFFu;

    b->sp.status |= 4u; /* dma busy */
    for (uint32_t i = 0; i < len; i++) {
        uint8_t *m = sp_mem(b, b->sp.mem_addr + i);
        if (!m) continue;
        uint32_t d = (dram + i) & (RDRAM_SIZE - 1u);
        if (from_dram) *m = b->rdram[d];
        else b->rdram[d] = *m;
    }
    b->sp.dmas++;
    b->sp.status &= ~4u;
}

void bus_sp_write(Bus *b, uint32_t off, uint32_t v) {
    switch (off & 0x1Fu) {
    case 0x00: b->sp.mem_addr = v; break;
    case 0x04: b->sp.dram_addr = v; break;
    /* Same naming trap as the PI block: RD_LEN reads RDRAM into SP memory,
     * WR_LEN writes SP memory out to RDRAM. */
    case 0x08: b->sp.rd_len = v; sp_dma(b, 1, v); break;
    case 0x0C: b->sp.wr_len = v; sp_dma(b, 0, v); break;
    case 0x10: /* SP_STATUS: the set/clear pairs below are the subset ucode uses */
        if (v & 0x0001u) b->sp.status |= 1u;    /* halt */
        if (v & 0x0002u) b->sp.status &= ~1u;   /* clear halt */
        if (v & 0x0004u) b->sp.status |= 2u;    /* broke */
        if (v & 0x0008u) b->sp.status &= ~2u;   /* clear broke */
        if (v & 0x0010u) b->sp.status |= 8u;    /* interrupt */
        if (v & 0x0020u) b->sp.status &= ~8u;   /* clear interrupt */
        if (v & 0x0040u) b->sp.status |= 0x20u; /* single step */
        if (v & 0x0080u) b->sp.status &= ~0x20u;
        break;
    case 0x1C: b->sp.semaphore = v & 1u; break;
    default: break;
    }
}
