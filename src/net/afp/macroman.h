/* Livadix — Macintosh names and Windows names.
 *
 * Mac Roman to Unicode by Apple's mapping table (ROMAN.TXT), with $DB the
 * currency sign as on the Systems before Mac OS 8.5 (the euro sign maps to
 * it as well). The characters a Windows name cannot hold go to the private
 * use code points Services for Macintosh used (docs/NOTES.md,
 * "Sharing"):
 *   $01-$1F -> U+F001-U+F01F;  "  U+F020;  *  U+F021;  /  U+F022;
 *   <  U+F023;  >  U+F024;  ?  U+F025;  \  U+F026;  |  U+F027;
 *   a trailing space U+F028, a trailing period U+F029.
 * Names are UTF-16 code units (wchar_t on Windows).
 *
 * Livadix's own code. */
#ifndef MC_MACROMAN_H
#define MC_MACROMAN_H

#include <stddef.h>
#include <stdint.h>

#define MC_MAC_NAME_MAX 31

uint16_t mc_macroman_to_unicode(uint8_t c);
/* 1 and the Mac Roman byte when u has one. */
int mc_unicode_to_macroman(uint16_t u, uint8_t *c);

/* A Macintosh name (Mac Roman, n bytes) as a Windows name: the units
 * written (at most capacity - 1, NUL-terminated), 0 when the name is empty
 * or holds NUL or ':'. */
size_t mc_mac_to_host_name(const uint8_t *mac, size_t n, uint16_t *out, size_t capacity);
/* A Windows name as a Macintosh name: the bytes written (out holds 255);
 * *exact is 0 when a character has no Mac Roman form (written as '_') or
 * the name is longer than 31 bytes (not cut). */
size_t mc_host_to_mac_name(const uint16_t *name, uint8_t *out, int *exact);
/* Mac Roman case folding for comparisons (Inside AppleTalk, appendix D,
 * Table D-2). */
uint8_t mc_macroman_upper(uint8_t c);
int mc_mac_name_equal(const uint8_t *a, size_t an, const uint8_t *b, size_t bn);

#endif
