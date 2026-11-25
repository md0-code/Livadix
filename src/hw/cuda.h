/* Livadix — Cuda: the link to the host and Cuda's own pseudo-commands (the
 * rest is adbctl.h).
 *
 * Wiring (Macintosh Quadra 840AV Developer Note, "Cuda"; the VIA1 port B
 * lines as the Quadra 605 ROM's Cuda driver drives them and Cuda firmware
 * 341S0788 answers):
 *   VIA1 PB5  TIP, transaction in progress (host output, low asserted)
 *   VIA1 PB4  BYTEACK (host output, toggled once per byte; high at rest)
 *   VIA1 PB3  TREQ, transaction request (Cuda output, low asserted)
 *   VIA1 CB1  the shift clock (Cuda drives it), CB2 the data
 *   VIA1 SR   mode 7 (shift out, external clock) to send, mode 3 (shift
 *             in, external clock) to receive
 * The same lines on every Cuda machine the ROMs of docs/NOTES.md cover:
 * Color Classic, the LC 520/550 family and TV, LC 475/575/Quadra 605,
 * 630/580, 660AV/840AV.
 *
 * The handshake: the host loads the shift register, then asserts TIP for
 * the first byte (BYTEACK high) and toggles BYTEACK for each further one;
 * Cuda clocks each byte in; the host ends the packet by negating TIP with
 * BYTEACK high (negating TIP with BYTEACK low asks Cuda to resynchronise:
 * it asserts TREQ, clocks 8 idle bits, waits for BYTEACK high, negates
 * TREQ and clocks 8 more). To send, Cuda asserts TREQ and clocks 8 idle
 * bits (the host's interrupt); the host asserts TIP; Cuda sends a byte each
 * time BYTEACK differs from the level it last saw, negates TREQ before the
 * last one, and after the host negates TIP clocks 8 idle bits. A host that
 * had TIP asserted when Cuda asked (a collision) gives up its packet;
 * Cuda clocks again once TIP is negated. */
#ifndef MC_CUDA_H
#define MC_CUDA_H

#include "adbctl.h"

#endif
