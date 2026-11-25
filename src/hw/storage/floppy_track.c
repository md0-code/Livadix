/* Floppy track synthesis and decoding (floppy_track.h).
 *
 * Sources: SWIM Chip User's Reference (Apple, rev. 1.5, January 1988),
 * "GCR Encoding", "GCR Track Format", "MFM Encoding", "MFM Track Format" and
 * "CRC Generation/Detection"; Apple 800K Double-Sided Drive ERS (699-0452-A,
 * September 1986), 3.4 "Rotational speed" (the five zone speeds); Inside
 * Macintosh III, "The Disk Interface" (TACH: 60 pulses per revolution). */
#include "floppy_track.h"

#include <string.h>

#define C15M_HZ 15667200u

/* SWIM Chip User's Reference, "GCR Encoding": the 64 disk bytes that carry
 * 6-bit values (most significant bit set, at least two adjacent ones below
 * it, at most one pair of adjacent zeros, never $D5 or $AA), in ascending
 * order of the value they encode. */
const uint8_t mc_gcr_encode6[64] = {
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6, 0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
    0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
    0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF};

int mc_gcr_decode6(uint8_t b) {
    static int8_t table[256];
    static int ready;
    if (!ready) {
        memset(table, -1, sizeof table);
        for (int i = 0; i < 64; i++) table[mc_gcr_encode6[i]] = (int8_t)i;
        ready = 1;
    }
    return table[b];
}

mc_floppy_format mc_floppy_format_of(uint64_t sectors) {
    switch (sectors) {
    case 800: return MC_FLOPPY_GCR400;
    case 1600: return MC_FLOPPY_GCR800;
    case 1440: return MC_FLOPPY_MFM720;
    case 2880: return MC_FLOPPY_MFM1440;
    default: return MC_FLOPPY_NONE;     /* 1680 KiB (3360 sectors) among them */
    }
}
int mc_floppy_is_gcr(mc_floppy_format f) { return f == MC_FLOPPY_GCR400 || f == MC_FLOPPY_GCR800; }
unsigned mc_floppy_sides(mc_floppy_format f) { return f == MC_FLOPPY_NONE ? 0 : f == MC_FLOPPY_GCR400 ? 1 : 2; }

/* SWIM Chip User's Reference, "GCR Track Format": tracks 0-15 hold 12
 * sectors, 16-31 11, 32-47 10, 48-63 9, 64-79 8. MFM ("MFM Track Format"):
 * 9 sectors per side on 720K disks, 18 on 1440K disks. */
unsigned mc_floppy_sectors(mc_floppy_format f, unsigned track) {
    if (track >= MC_FLOPPY_TRACKS) return 0;
    switch (f) {
    case MC_FLOPPY_GCR400: case MC_FLOPPY_GCR800: return 12 - track / 16;
    case MC_FLOPPY_MFM720: return 9;
    case MC_FLOPPY_MFM1440: return 18;
    default: return 0;
    }
}

/* 800K Double-Sided Drive ERS 3.4: track 00-15 394 rpm, 16-31 429, 32-47
 * 472, 48-63 525, 64-79 590. MFM disks spin at a constant 300 rpm. */
unsigned mc_floppy_gcr_rpm(unsigned track) {
    static const unsigned rpm[5] = {394, 429, 472, 525, 590};
    return rpm[(track < MC_FLOPPY_TRACKS ? track : MC_FLOPPY_TRACKS - 1) / 16];
}
unsigned mc_floppy_rpm(int gcr, unsigned track) { return gcr ? mc_floppy_gcr_rpm(track) : 300; }

/* GCR: 2-microsecond cells of 32 FCLK periods at 15.6672 MHz, 8 cells a byte
 * (SWIM Chip User's Reference, "GCR Encoding"): 256 C15M periods, 61,200
 * bytes/s. MFM ("MFM Encoding"): a 2-microsecond data cell on 1440K disks
 * (500 kbit/s, 62,500 bytes/s), twice that on 720K disks. */
uint32_t mc_floppy_bytes_per_second(int gcr, int high_density) {
    if (gcr) return C15M_HZ / 256u;
    return high_density ? 62500u : 31250u;
}
uint32_t mc_floppy_track_length(int gcr, int high_density, unsigned track) {
    return (uint32_t)((uint64_t)mc_floppy_bytes_per_second(gcr, high_density) * 60u / mc_floppy_rpm(gcr, track));
}

int64_t mc_floppy_first_sector(mc_floppy_format f, unsigned track, unsigned side) {
    unsigned sides = mc_floppy_sides(f);
    if (track >= MC_FLOPPY_TRACKS || side >= sides) return -1;
    int64_t lba = 0;
    for (unsigned t = 0; t < track; t++) lba += (int64_t)mc_floppy_sectors(f, t) * sides;
    return lba + (int64_t)side * mc_floppy_sectors(f, track);
}

/* ---- GCR ---- */

/* SWIM Chip User's Reference, "GCR Track Format": the data bytes are
 * mangled with three running checksums. The decoding algorithm given there:
 * per group of three bytes, rotate checksum C left (its bit 7 into the
 * carry), A ^= C, A into checksum A with carry, B ^= checksum A, B into
 * checksum B with carry, C ^= checksum B, C into checksum C with carry. The
 * encoder is its exact inverse. Each group of three bytes becomes four
 * nibbles: the top two bits of A, B, C, then the low six bits of each; the
 * 524 bytes end with a group of two (699 nibbles), and the three checksum
 * bytes follow as one more group. */
void mc_gcr_encode_data(const uint8_t in[524], uint8_t out[MC_GCR_DATA_NIBBLES]) {
    unsigned ca = 0, cb = 0, cc = 0, n = 0;
    for (unsigned i = 0; i < 524; i += 3) {
        int has_c = i + 2 < 524;
        unsigned a = in[i], b = in[i + 1], c = has_c ? in[i + 2] : 0;
        unsigned carry = cc >> 7;
        cc = ((cc << 1) | carry) & 0xFF;
        unsigned ea = a ^ cc;
        unsigned s = ca + a + carry; ca = s & 0xFF; carry = s >> 8;
        unsigned eb = b ^ ca;
        s = cb + b + carry; cb = s & 0xFF; carry = s >> 8;
        unsigned ec = 0;
        if (has_c) { ec = c ^ cb; s = cc + c + carry; cc = s & 0xFF; }
        out[n++] = mc_gcr_encode6[((ea >> 6) << 4) | ((eb >> 6) << 2) | (ec >> 6)];
        out[n++] = mc_gcr_encode6[ea & 0x3F];
        out[n++] = mc_gcr_encode6[eb & 0x3F];
        if (has_c) out[n++] = mc_gcr_encode6[ec & 0x3F];
    }
    out[n++] = mc_gcr_encode6[((ca >> 6) << 4) | ((cb >> 6) << 2) | (cc >> 6)];
    out[n++] = mc_gcr_encode6[ca & 0x3F];
    out[n++] = mc_gcr_encode6[cb & 0x3F];
    out[n++] = mc_gcr_encode6[cc & 0x3F];
}

int mc_gcr_decode_data(const uint8_t in[MC_GCR_DATA_NIBBLES], uint8_t out[524]) {
    int v[MC_GCR_DATA_NIBBLES];
    for (unsigned i = 0; i < MC_GCR_DATA_NIBBLES; i++)
        if ((v[i] = mc_gcr_decode6(in[i])) < 0) return 0;
    unsigned ca = 0, cb = 0, cc = 0, n = 0;
    for (unsigned i = 0; i < 524; i += 3) {
        int has_c = i + 2 < 524;
        unsigned top = (unsigned)v[n++];
        unsigned a = ((top >> 4) & 3) << 6 | (unsigned)v[n++];
        unsigned b = ((top >> 2) & 3) << 6 | (unsigned)v[n++];
        unsigned c = has_c ? ((top & 3) << 6 | (unsigned)v[n++]) : 0;
        unsigned carry = cc >> 7;
        cc = ((cc << 1) | carry) & 0xFF;
        a ^= cc;
        unsigned s = ca + a + carry; ca = s & 0xFF; carry = s >> 8;
        b ^= ca;
        s = cb + b + carry; cb = s & 0xFF; carry = s >> 8;
        out[i] = (uint8_t)a; out[i + 1] = (uint8_t)b;
        if (has_c) { c ^= cb; s = cc + c + carry; cc = s & 0xFF; out[i + 2] = (uint8_t)c; }
    }
    unsigned top = (unsigned)v[n++];
    unsigned ka = ((top >> 4) & 3) << 6 | (unsigned)v[n++];
    unsigned kb = ((top >> 2) & 3) << 6 | (unsigned)v[n++];
    unsigned kc = (top & 3) << 6 | (unsigned)v[n++];
    return ka == ca && kb == cb && kc == cc;
}

/* SWIM Chip User's Reference, "GCR Track Format": the address field holds
 * track, sector, side, format and checksum as nibbles; track and side form
 * one word with the side in bit 11 and the track in bits 0-10, so the
 * "side" value carries track bits 6-10 in its bits 0-4. The checksum is the
 * exclusive-or of the other four (Livadix's reading of the field layout). */
void mc_gcr_address(unsigned track, unsigned side, unsigned sector, unsigned format, uint8_t out[5]) {
    out[0] = (uint8_t)(track & 0x3F);
    out[1] = (uint8_t)(sector & 0x3F);
    out[2] = (uint8_t)(((track >> 6) & 0x1F) | ((side & 1) << 5));
    out[3] = (uint8_t)(format & 0x3F);
    out[4] = (uint8_t)(out[0] ^ out[1] ^ out[2] ^ out[3]);
}

/* Format byte: $22 for two-sided 800K disks, $02 for one-sided 400K disks
 * (bit 5 the sides, the low bits the 2:1 interleave). */
static unsigned gcr_format_byte(mc_floppy_format f) { return f == MC_FLOPPY_GCR800 ? 0x22 : 0x02; }

/* Bytes of one GCR sector without its gap: address mark (3), address
 * nibbles (5), bit slip (2), self-sync (6), data mark (3), sector nibble,
 * 703 data nibbles, bit slip (2). */
#define GCR_SECTOR_BYTES (3 + 5 + 2 + 6 + 3 + 1 + MC_GCR_DATA_NIBBLES + 2)

static void put(mc_floppy_track *t, uint32_t *at, uint8_t value, uint8_t mark) {
    if (*at < t->length) { t->data[*at] = value; t->mark[*at] = mark; }
    (*at)++;
}

static int build_gcr(mc_floppy_format f, unsigned track, unsigned side, const uint8_t *data, const uint8_t *tags, mc_floppy_track *out) {
    unsigned n = mc_floppy_sectors(f, track);
    out->length = mc_floppy_track_length(1, 0, track);
    memset(out->data, 0xFF, out->length);
    memset(out->mark, 0, out->length);
    /* 2:1 interleave: logical sector s at physical slot 2s mod n, the next
     * free slot when taken. */
    int slot_sector[12];
    for (unsigned i = 0; i < 12; i++) slot_sector[i] = -1;
    for (unsigned s = 0; s < n; s++) {
        unsigned p = (2 * s) % n;
        while (slot_sector[p] >= 0) p = (p + 1) % n;
        slot_sector[p] = (int)s;
    }
    uint32_t gap = (out->length - n * GCR_SECTOR_BYTES) / n, at = 0;
    for (unsigned p = 0; p < n; p++) {
        unsigned s = (unsigned)slot_sector[p];
        at += gap;                                      /* self-sync gap ($FF) */
        uint8_t adr[5];
        mc_gcr_address(track, side, s, gcr_format_byte(f), adr);
        put(out, &at, 0xD5, 0); put(out, &at, 0xAA, 0); put(out, &at, 0x96, 0);
        for (int i = 0; i < 5; i++) put(out, &at, mc_gcr_encode6[adr[i]], 0);
        put(out, &at, 0xDE, 0); put(out, &at, 0xAA, 0);
        at += 6;                                        /* self-sync */
        put(out, &at, 0xD5, 0); put(out, &at, 0xAA, 0); put(out, &at, 0xAD, 0);
        put(out, &at, mc_gcr_encode6[s & 0x3F], 0);
        uint8_t raw[524], nib[MC_GCR_DATA_NIBBLES];
        if (tags) memcpy(raw, tags + s * 12, 12); else memset(raw, 0, 12);
        memcpy(raw + 12, data + s * 512, 512);
        mc_gcr_encode_data(raw, nib);
        for (unsigned i = 0; i < MC_GCR_DATA_NIBBLES; i++) put(out, &at, nib[i], 0);
        put(out, &at, 0xDE, 0); put(out, &at, 0xAA, 0);
    }
    return 1;
}

/* ---- MFM ---- */

/* SWIM Chip User's Reference, "CRC Generation/Detection": the CRC starts at
 * all ones; per data bit, xorBit = CRC[15] ^ DATA[7], CRC[4] and CRC[11] take
 * xorBit, then CRC rotates left one bit. */
uint16_t mc_mfm_crc(uint16_t crc, const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        unsigned d = data[i];
        for (int b = 0; b < 8; b++) {
            unsigned x = ((crc >> 15) ^ (d >> 7)) & 1;
            if (x) crc ^= (1u << 4) | (1u << 11);
            crc = (uint16_t)((crc << 1) | x);
            d = (d << 1) & 0xFF;
        }
    }
    return crc;
}

/* SWIM Chip User's Reference, "MFM Track Format": gap to the index mark (80
 * x $4E), sync (12 x $00), index mark ($C2 $C2 $C2 marks, $FC), gap (50 x $4E);
 * per sector a sync field, the address mark ($A1 x 3 marks, $FE), track,
 * side, sector (from 1), size ($02), CRC, a 22-byte gap, sync, the data mark
 * ($A1 x 3 marks, $FB), 512 bytes, CRC and the inter-sector gap (84 bytes on
 * 720K disks, 101 on 1440K disks); $4E to the end of the track. */
static int build_mfm(mc_floppy_format f, unsigned track, unsigned side, const uint8_t *data, mc_floppy_track *out) {
    int hd = f == MC_FLOPPY_MFM1440;
    unsigned n = mc_floppy_sectors(f, track);
    out->length = mc_floppy_track_length(0, hd, track);
    memset(out->data, 0x4E, out->length);
    memset(out->mark, 0, out->length);
    uint32_t at = 80;
    for (int i = 0; i < 12; i++) put(out, &at, 0x00, 0);
    for (int i = 0; i < 3; i++) put(out, &at, 0xC2, 1);
    put(out, &at, 0xFC, 0);
    at += 50;
    for (unsigned s = 0; s < n; s++) {
        for (int i = 0; i < 12; i++) put(out, &at, 0x00, 0);
        uint8_t id[10] = {0xA1, 0xA1, 0xA1, 0xFE, (uint8_t)track, (uint8_t)side, (uint8_t)(s + 1), 2, 0, 0};
        uint16_t crc = mc_mfm_crc(0xFFFF, id, 8);
        id[8] = (uint8_t)(crc >> 8); id[9] = (uint8_t)crc;
        for (int i = 0; i < 10; i++) put(out, &at, id[i], i < 3);
        at += 22;
        for (int i = 0; i < 12; i++) put(out, &at, 0x00, 0);
        static const uint8_t dam[4] = {0xA1, 0xA1, 0xA1, 0xFB};
        crc = mc_mfm_crc(0xFFFF, dam, 4);
        crc = mc_mfm_crc(crc, data + s * 512, 512);
        for (int i = 0; i < 4; i++) put(out, &at, dam[i], i < 3);
        for (int i = 0; i < 512; i++) put(out, &at, data[s * 512 + i], 0);
        put(out, &at, (uint8_t)(crc >> 8), 0); put(out, &at, (uint8_t)crc, 0);
        at += hd ? 101 : 84;
    }
    return 1;
}

int mc_floppy_build_track(mc_floppy_format f, unsigned track, unsigned side, const uint8_t *data, const uint8_t *tags, mc_floppy_track *out) {
    if (track >= MC_FLOPPY_TRACKS || side >= mc_floppy_sides(f)) return 0;
    if (mc_floppy_is_gcr(f)) return build_gcr(f, track, side, data, tags, out);
    return build_mfm(f, track, side, data, out);
}

/* ---- Decoding ---- */

/* The track twice over a margin, so fields that cross the index read as
 * one run. */
#define WRAP 1200u
static uint8_t at_data(const mc_floppy_track *t, uint32_t i) { return t->data[i % t->length]; }
static uint8_t at_mark(const mc_floppy_track *t, uint32_t i) { return t->mark[i % t->length]; }

static unsigned decode_gcr(mc_floppy_format f, unsigned track, unsigned side, const mc_floppy_track *t, mc_floppy_sector_fn fn, void *ctx) {
    unsigned n = mc_floppy_sectors(f, track), found = 0;
    uint32_t seen = 0, len = t->length;
    for (uint32_t i = 0; i < len; i++) {
        if (at_data(t, i) != 0xD5 || at_data(t, i + 1) != 0xAA || at_data(t, i + 2) != 0x96) continue;
        int v[5];
        int ok = 1;
        for (int k = 0; k < 5; k++) if ((v[k] = mc_gcr_decode6(at_data(t, i + 3 + k))) < 0) ok = 0;
        if (!ok || (v[0] ^ v[1] ^ v[2] ^ v[3]) != v[4]) continue;
        unsigned atrack = (unsigned)v[0] | (((unsigned)v[2] & 0x1F) << 6), aside = ((unsigned)v[2] >> 5) & 1, sector = (unsigned)v[1];
        if (atrack != track || aside != side || sector >= n || (seen >> sector & 1)) continue;
        /* The first data field after this address field (and before the
         * next one) that decodes with a good checksum. */
        for (uint32_t j = i + 8; j < i + 8 + WRAP; j++) {
            if (at_data(t, j) == 0xD5 && at_data(t, j + 1) == 0xAA && at_data(t, j + 2) == 0x96) break;
            if (at_data(t, j) != 0xD5 || at_data(t, j + 1) != 0xAA || at_data(t, j + 2) != 0xAD) continue;
            if (mc_gcr_decode6(at_data(t, j + 3)) != (int)sector) continue;
            uint8_t nib[MC_GCR_DATA_NIBBLES], raw[524];
            for (unsigned k = 0; k < MC_GCR_DATA_NIBBLES; k++) nib[k] = at_data(t, j + 4 + k);
            if (!mc_gcr_decode_data(nib, raw)) continue;
            fn(ctx, sector, raw + 12, raw);
            seen |= 1u << sector; found++;
            break;
        }
    }
    return found;
}

/* The CRC after a run of `count` mark bytes starting at `first`. */
static uint16_t crc_of_marks(const mc_floppy_track *t, uint32_t first, uint32_t count) {
    uint16_t crc = 0xFFFF;
    for (uint32_t k = 0; k < count; k++) { uint8_t b = at_data(t, first + k); crc = mc_mfm_crc(crc, &b, 1); }
    return crc;
}

static unsigned decode_mfm(mc_floppy_format f, unsigned track, unsigned side, const mc_floppy_track *t, mc_floppy_sector_fn fn, void *ctx) {
    unsigned n = mc_floppy_sectors(f, track), found = 0;
    uint32_t seen = 0, len = t->length;
    for (uint32_t i = 0; i < len; i++) {
        /* A run of $A1 marks starting at i, followed by the ID mark $FE. */
        if (!at_mark(t, i) || at_data(t, i) != 0xA1 || (at_mark(t, i + len - 1) && at_data(t, i + len - 1) == 0xA1)) continue;
        uint32_t run = 0;
        while (run < 8 && at_mark(t, i + run) && at_data(t, i + run) == 0xA1) run++;
        uint32_t p = i + run;
        if (at_data(t, p) != 0xFE) continue;
        uint16_t crc = crc_of_marks(t, i, run);
        for (uint32_t k = 0; k < 7; k++) { uint8_t b = at_data(t, p + k); crc = mc_mfm_crc(crc, &b, 1); }
        if (crc) continue;
        unsigned c = at_data(t, p + 1), h = at_data(t, p + 2), r = at_data(t, p + 3), size = at_data(t, p + 4);
        if (c != track || h != side || r < 1 || r > n || size != 2 || (seen >> (r - 1) & 1)) continue;
        /* The first data field after this ID (and before the next one)
         * whose CRC checks out: $A1 marks, then $FB (or $F8, deleted data). */
        for (uint32_t j = p + 7; j < p + 7 + WRAP; j++) {
            if (!at_mark(t, j) || at_data(t, j) != 0xA1) continue;
            uint32_t drun = 0;
            while (drun < 8 && at_mark(t, j + drun) && at_data(t, j + drun) == 0xA1) drun++;
            uint32_t q = j + drun;
            uint8_t dm = at_data(t, q);
            if (dm == 0xFE) break;
            if (dm != 0xFB && dm != 0xF8) { j = q - 1; continue; }
            uint8_t sec[514];
            for (uint32_t k = 0; k < 514; k++) sec[k] = at_data(t, q + 1 + k);
            uint16_t dcrc = crc_of_marks(t, j, drun);
            dcrc = mc_mfm_crc(dcrc, &dm, 1);
            dcrc = mc_mfm_crc(dcrc, sec, 514);
            if (dcrc == 0) { fn(ctx, r - 1, sec, NULL); seen |= 1u << (r - 1); found++; break; }
            j = q - 1;
        }
    }
    return found;
}

unsigned mc_floppy_decode_track(mc_floppy_format f, unsigned track, unsigned side, const mc_floppy_track *t, mc_floppy_sector_fn fn, void *ctx) {
    if (!t->length || track >= MC_FLOPPY_TRACKS || side >= mc_floppy_sides(f)) return 0;
    return mc_floppy_is_gcr(f) ? decode_gcr(f, track, side, t, fn, ctx) : decode_mfm(f, track, side, t, fn, ctx);
}
