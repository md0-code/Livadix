/* Livadix — DES (FIPS PUB 46-3), one 64-bit block, for AFP's "Randnum
 * Exchange" (the password as the key, the server's random number as the
 * block). The key's parity bits are ignored.
 *
 * Livadix's own code. */
#ifndef MC_DES_H
#define MC_DES_H

#include <stdint.h>

void mc_des_encrypt(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]);

#endif
