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
    if (a < RDRAM_SIZE || in_window(a, 0x80000000u, RDRAM_SIZE) || in_window(a, 0xA0000000u, RDRAM_SIZE))
        return rdram_byte(b, a);
    if (b->cart && in_window(a, 0x10000000u, b->cart_size)) return b->cart[a - 0x10000000u];
    if (in_window(a, 0x04600000u, 0x40u)) return (uint8_t)(bus_pi_read(b, a - 0x04600000u) >> (8 * (3 - (a & 3))));
    if (in_window(a, 0xBFC00000u, 0x800u)) return 0;    /* IPL3 area: the bootcode itself is not shipped */
    if (in_window(a, 0x04000000u, 0x4000u)) return 0;   /* SP registers */
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
    if (in_window(a, 0x04600000u, 0x40u)) return bus_pi_read(b, a - 0x04600000u);
    return ((uint32_t)bus_read8(b, a) << 24) | ((uint32_t)bus_read8(b, a + 1) << 16) |
           ((uint32_t)bus_read8(b, a + 2) << 8) | bus_read8(b, a + 3);
}

uint64_t bus_read64(const Bus *b, uint32_t a) {
    return ((uint64_t)bus_read32(b, a) << 32) | bus_read32(b, a + 4);
}

void bus_write8(Bus *b, uint32_t a, uint8_t v) {
    if (a < RDRAM_SIZE || in_window(a, 0x80000000u, RDRAM_SIZE) || in_window(a, 0xA0000000u, RDRAM_SIZE)) {
        b->rdram[(a < RDRAM_SIZE) ? a : (a & (RDRAM_SIZE - 1))] = v;
        return;
    }
    if (in_window(a, 0x04000000u, 0x4000u)) return;   /* SP: M2 */
    if (in_window(a, 0x04400000u, 0x40u)) return;     /* VI: M3 */
    if (in_window(a, 0x04800000u, 0x40u)) return;     /* SI: M4 */
    if (in_window(a, 0x1FC00000u, 0x80000u)) return;  /* PIF: needs the boot ROM */
    b->unmapped_writes++;
}

void bus_write16(Bus *b, uint32_t a, uint16_t v) {
    bus_write8(b, a, (uint8_t)(v >> 8));
    bus_write8(b, a + 1, (uint8_t)v);
}

void bus_write32(Bus *b, uint32_t a, uint32_t v) {
    if (in_window(a, 0x04600000u, 0x40u)) {
        bus_pi_write(b, a - 0x04600000u, v);
        return;
    }
    bus_write8(b, a, (uint8_t)(v >> 24));
    bus_write8(b, a + 1, (uint8_t)(v >> 16));
    bus_write8(b, a + 2, (uint8_t)(v >> 8));
    bus_write8(b, a + 3, (uint8_t)v);
}

void bus_write64(Bus *b, uint32_t a, uint64_t v) {
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
    uint32_t dram = b->pi.dram_addr & ~1u;
    uint32_t cart = b->pi.cart_addr & ~1u;

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
