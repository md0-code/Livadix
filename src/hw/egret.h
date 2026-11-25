/* Livadix — Egret: the link to the host and Egret's own pseudo-commands (the
 * rest is adbctl.h).
 *
 * Wiring (the VIA1 port B lines as the IIsi and Quadra 605 ROMs' Egret
 * driver drives them and Egret firmware 341S0851 answers):
 *   VIA1 PB5  SysSes, system session (host output, high asserted)
 *   VIA1 PB4  ViaFull, the shift register holds a byte / was read (host
 *             output, high asserted)
 *   VIA1 PB3  XcvrSes, transceiver session (Egret output, low asserted)
 *   VIA1 CB1  the shift clock (Egret drives it), CB2 the data
 *   VIA1 SR   mode 7 to send, mode 3 to receive (external clock)
 * The same lines on the IIsi, LC, LC II, Classic II, LC III, IIvx/IIvi and
 * Quadra 900/950 (the ROMs share the driver)†.
 *
 * The handshake: the host asserts SysSes; for each byte it loads the shift
 * register and asserts ViaFull, Egret clocks the byte in, the host negates
 * ViaFull; it ends the packet by negating SysSes. To send, Egret asserts
 * XcvrSes and clocks 8 idle bits (the host's interrupt); the host asserts
 * SysSes (dropping it first if it was starting a packet of its own); for
 * each byte Egret waits for ViaFull negated, clocks the byte out, and
 * waits for the host to assert and negate ViaFull; Egret negates XcvrSes
 * before the last byte, and the host ends by negating SysSes. */
#ifndef MC_EGRET_H
#define MC_EGRET_H

#include "adbctl.h"

#endif
