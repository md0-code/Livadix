/* Livadix - LocalTalk Packet Control assist for the PCI SCC.
 * CHRP I/O Device Reference (Apple/IBM/Motorola, 1996), 9.8.2-9.8.4
 * and 9.10.3. Arming a channel monitors TxD for sixteen mark bits,
 * then disables the line driver and latches Detect until Start clears.
 * SCC bit timing comes from its programmed clock (Zilog SCC UM ch.3).
 * Original 6400 LocalTalk HAL accesses StartA/StartB/DetectAB at
 * O'Hare +$13090/+$130A0/+$130B0. The board maps this three-register
 * block there and routes each detect output to its transmit DBDMA s5.
 * Livadix's original implementation, without guest or ROM patches. */
#ifndef MC_LTPC_H
#define MC_LTPC_H
#include "../../scc.h"

enum { MC_LTPC_START_A = 0, MC_LTPC_START_B = 0x10, MC_LTPC_DETECT = 0x20 };
enum { MC_LTPC_LINE_DETECT_A, MC_LTPC_LINE_DETECT_B };

typedef struct mc_ltpc {
    mc_dev dev;
    mc_scc *scc;
    uint8_t start, detect;
    uint64_t due[2], period[2];
} mc_ltpc;

void mc_ltpc_init(mc_ltpc *, const mc_hw_host *, mc_scc *);
/* Notify after the SCC's registers or transmit buffers change. */
void mc_ltpc_changed(mc_ltpc *, unsigned channel);
/* The transmit line driver is disabled after this channel's abort. */
int mc_ltpc_tristated(const mc_ltpc *, unsigned channel);
extern const mc_dev_ops mc_ltpc_ops;
#endif
