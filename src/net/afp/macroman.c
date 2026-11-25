/* Livadix — Macintosh names and Windows names (macroman.h).
 *
 * Sources: Apple's Mac OS Roman to Unicode mapping table (ROMAN.TXT,
 * unicode.org, "Apple" vendor mappings; its notes give $DB as U+00A4
 * before Mac OS 8.5); Inside AppleTalk, 2nd ed., appendix D, Table D-2
 * (upper-case equivalents); Inside AppleTalk chapter 13, "Catalog node
 * names" (long names: at most 31 bytes, no NUL or colon); Microsoft's
 * naming rules for Windows files ("Naming Files, Paths, and Namespaces":
 * the reserved characters < > : " / \ | ? * and 1-31, no trailing space or
 * period). The private use code points for the reserved characters are
 * those of Services for Macintosh.
 *
 * Livadix's own code. */
#include "macroman.h"

static const uint16_t high[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
    0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
    0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
    0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
    0x00FF, 0x0178, 0x2044, 0x00A4, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

/* The reserved characters other than the controls, in the order of their
 * code points U+F020-U+F027. */
static const char reserved[] = "\"*/<>?\\|";

uint16_t mc_macroman_to_unicode(uint8_t c) { return c < 0x80 ? c : high[c - 0x80]; }

int mc_unicode_to_macroman(uint16_t u, uint8_t *c) {
    if (u < 0x80) { *c = (uint8_t)u; return 1; }
    if (u == 0x20AC) { *c = 0xDB; return 1; }       /* the euro sign took $DB's place in 8.5 */
    for (unsigned k = 0; k < 128; k++)
        if (high[k] == u) { *c = (uint8_t)(0x80 + k); return 1; }
    return 0;
}

size_t mc_mac_to_host_name(const uint8_t *mac, size_t n, uint16_t *out, size_t capacity) {
    if (!n || n + 1 > capacity) return 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t c = mac[i];
        uint16_t u;
        if (c == 0 || c == ':') return 0;
        if (c < 0x20) u = (uint16_t)(0xF000 + c);
        else if (c < 0x80) {
            u = c;
            for (unsigned k = 0; reserved[k]; k++)
                if ((uint8_t)reserved[k] == c) u = (uint16_t)(0xF020 + k);
        } else u = high[c - 0x80];
        out[i] = u;
    }
    if (out[n - 1] == ' ') out[n - 1] = 0xF028;
    else if (out[n - 1] == '.') out[n - 1] = 0xF029;
    out[n] = 0;
    /* "." and ".." are not names on Windows: their last period went private
       above, but ".." keeps its first; a name of periods alone keeps them
       all as private code points. */
    int dots = 1;
    for (size_t i = 0; i < n; i++)
        if (mac[i] != '.') dots = 0;
    if (dots)
        for (size_t i = 0; i < n; i++) out[i] = 0xF029;
    return n;
}

size_t mc_host_to_mac_name(const uint16_t *name, uint8_t *out, int *exact) {
    size_t n = 0;
    *exact = 1;
    for (; *name && n < 255; name++) {
        const uint16_t u = *name;
        uint8_t c;
        if (u >= 0xF001 && u <= 0xF01F) c = (uint8_t)(u - 0xF000);
        else if (u >= 0xF020 && u <= 0xF027) c = (uint8_t)reserved[u - 0xF020];
        else if (u == 0xF028) c = ' ';
        else if (u == 0xF029) c = '.';
        else if (u == ':' || !mc_unicode_to_macroman(u, &c)) {
            c = '_';
            *exact = 0;
        }
        out[n++] = c;
    }
    if (n > MC_MAC_NAME_MAX || *name) *exact = 0;
    /* The trailing forms stand only at the end (or for a name of periods):
       anywhere else the name would not come back the same. */
    const uint16_t *start = name - n;
    int all_dots = n > 0;
    for (size_t i = 0; i < n; i++)
        if (start[i] != 0xF029) all_dots = 0;
    for (size_t i = 0; i + 1 < n && !all_dots; i++)
        if (start[i] == 0xF028 || start[i] == 0xF029) *exact = 0;
    if (n && (start[n - 1] == ' ' || start[n - 1] == '.')) *exact = 0;
    return n;
}

uint8_t mc_macroman_upper(uint8_t c) {
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 32);
    switch (c) {
    case 0x88: return 0xCB;
    case 0x8A: return 0x80;
    case 0x8B: return 0xCC;
    case 0x8C: return 0x81;
    case 0x8D: return 0x82;
    case 0x8E: return 0x83;
    case 0x96: return 0x84;
    case 0x9A: return 0x85;
    case 0x9B: return 0xCD;
    case 0x9F: return 0x86;
    case 0xBE: return 0xAE;
    case 0xBF: return 0xAF;
    case 0xCF: return 0xCE;
    default: return c;
    }
}

int mc_mac_name_equal(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
    if (an != bn) return 0;
    for (size_t i = 0; i < an; i++)
        if (mc_macroman_upper(a[i]) != mc_macroman_upper(b[i])) return 0;
    return 1;
}
