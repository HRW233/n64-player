/* rom.c — load a .n64/.z64/.v64 cart image and normalise it to big-endian.
 *
 * The three common on-disk orders differ only in how the bytes of each word are
 * arranged, so the whole image can be reordered once at load time and the rest
 * of the emulator never sees the difference:
 *   .z64  80 37 12 40  big-endian, as the console sees it
 *   .v64  37 80 40 12  byte-swapped in 16-bit units
 *   .n64  40 12 37 80  little-endian 32-bit words
 */
#include "n64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *rom_order_name(int order) {
    switch (order) {
    case ROM_ORDER_Z64: return "z64 (big-endian)";
    case ROM_ORDER_V64: return "v64 (byteswapped)";
    case ROM_ORDER_N64: return "n64 (little-endian)";
    default: return "unknown";
    }
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int rom_load(const char *path, Rom *out, char *err, size_t errcap) {
    memset(out, 0, sizeof *out);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errcap, "cannot open %s", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        snprintf(err, errcap, "cannot seek %s", path);
        fclose(f);
        return -1;
    }
    long n = ftell(f);
    rewind(f);
    if (n < 0x1000) {
        snprintf(err, errcap, "%s is %ld bytes: smaller than a header block", path, n);
        fclose(f);
        return -1;
    }
    if (n > (long)ROM_MAX) {
        snprintf(err, errcap, "%s is %ld bytes: larger than %u", path, n, ROM_MAX);
        fclose(f);
        return -1;
    }
    uint8_t *raw = malloc((size_t)n);
    if (!raw) {
        snprintf(err, errcap, "out of memory reading %s", path);
        fclose(f);
        return -1;
    }
    if (fread(raw, 1, (size_t)n, f) != (size_t)n) {
        snprintf(err, errcap, "short read on %s", path);
        free(raw);
        fclose(f);
        return -1;
    }
    fclose(f);

    uint8_t *data = malloc((size_t)n);
    if (!data) {
        snprintf(err, errcap, "out of memory converting %s", path);
        free(raw);
        return -1;
    }

    uint32_t magic = be32(raw);
    if (magic == 0x80371240u) {
        out->order = ROM_ORDER_Z64;
        memcpy(data, raw, (size_t)n);
    } else if (magic == 0x37804012u) {
        out->order = ROM_ORDER_V64;
        for (long i = 0; i + 1 < n; i += 2) {
            data[i] = raw[i + 1];
            data[i + 1] = raw[i];
        }
        if (n & 1) data[n - 1] = raw[n - 1];
    } else if (magic == 0x40123780u) {
        out->order = ROM_ORDER_N64;
        for (long i = 0; i + 3 < n; i += 4) {
            data[i] = raw[i + 3];
            data[i + 1] = raw[i + 2];
            data[i + 2] = raw[i + 1];
            data[i + 3] = raw[i];
        }
    } else {
        snprintf(err, errcap, "%s: unknown magic %08x (expected z64, v64 or n64 order)", path, magic);
        free(raw);
        free(data);
        return -1;
    }
    free(raw);

    out->data = data;
    out->size = (uint32_t)n;
    out->entry = be32(data + 0x08);
    out->crc1 = be32(data + 0x10);
    out->crc2 = be32(data + 0x14);
    memcpy(out->title, data + 0x20, 20);
    out->title[20] = '\0';
    for (int i = 19; i >= 0 && (out->title[i] == ' ' || out->title[i] == '\n'); i--) out->title[i] = '\0';
    out->region = (char)data[0x3E];
    if (out->region < 'A' || out->region > 'Z') out->region = ' ';
    return 0;
}

void rom_free(Rom *r) {
    free(r->data);
    memset(r, 0, sizeof *r);
}
