/* boot.c — take a cartridge to its entry point, or say exactly why it stopped.
 *
 * There are two paths, and the difference between them is the difference
 * between an emulator and a demo:
 *
 *   default   IPL1 (the PIF ROM) normally copies 0xFC0 bytes of the cart's IPL3
 *             from offset 0x40 into DMEM at offset 0x40, leaving 0x40 bytes at
 *             the bottom of DMEM for the CIC seed and version block, then starts
 *             the CPU at 0xA4000040. Without IPL1 we can do the copy and start
 *             the CPU, but not fill the seed block: it is zeroed, and any IPL3
 *             that checks it will notice.
 *
 *   skip_ipl3 The M0 approximation: copy the first megabyte of cart data to
 *             0x80000400 and jump to the header's entry point. Not a boot, but
 *             it runs the game's own code without a boot path.
 *
 * Both facts are reconstructed from published documentation. There is no
 * cartridge in this tree to check them against, so they are stated here rather
 * than buried in code that would look more authoritative than it is.
 */
#include "n64.h"

#include <stdio.h>
#include <string.h>

#define IPL3_CART_OFFSET 0x40u
#define IPL3_LENGTH 0xFC0u
#define IPL3_DMEM_OFFSET 0x40u
#define IPL3_ENTRY 0xA4000040u

/* Only the two variants whose header CRCs are published consistently are
 * listed. Anything else reports as unrecognised instead of being guessed at,
 * because a wrong CIC name sends the reader of the output in the wrong
 * direction. 6103 and 6105 share their CRCs with each other, and 6105's
 * bootcode is encrypted, which is a separate problem this tree does not solve.
 */
static const struct {
    uint32_t crc1, crc2;
    const char *name;
} CIC_TABLE[] = {
    {0x6170A4A1u, 0x90BB6CB5u, "6101"},
    {0x90BB6CB5u, 0x0B050EE0u, "6102"},
};

const char *cic_name(uint32_t crc1, uint32_t crc2) {
    for (size_t i = 0; i < sizeof CIC_TABLE / sizeof CIC_TABLE[0]; i++)
        if (CIC_TABLE[i].crc1 == crc1 && CIC_TABLE[i].crc2 == crc2) return CIC_TABLE[i].name;
    return NULL;
}

int boot_cart(Bus *b, Cpu *c, const Rom *rom, int skip_ipl3, BootInfo *info) {
    memset(info, 0, sizeof *info);
    bus_init(b, rom->data, rom->size);
    info->cic = cic_name(rom->crc1, rom->crc2);

    if (skip_ipl3 || rom->size < IPL3_CART_OFFSET + IPL3_LENGTH) {
        uint32_t n = rom->size > 0x1000u ? rom->size - 0x1000u : 0;
        if (n > 0x100000u) n = 0x100000u;
        memcpy(b->rdram + 0x400, rom->data + 0x1000, n);
        cpu_reset(c, rom->entry);
        info->ran_ipl3 = 0;
        info->load_pc = rom->entry;
        return skip_ipl3 ? 0 : -1;
    }

    /* DMEM 0x00-0x3F is the CIC seed block PIF would have written. It is already
     * zero from bus_init; saying so out loud is the point. */
    memset(b->sp.dmem, 0, IPL3_DMEM_OFFSET);
    memcpy(b->sp.dmem + IPL3_DMEM_OFFSET, rom->data + IPL3_CART_OFFSET, IPL3_LENGTH);

    cpu_reset(c, IPL3_ENTRY);
    info->ran_ipl3 = 1;
    info->load_pc = IPL3_ENTRY;
    return 0;
}
