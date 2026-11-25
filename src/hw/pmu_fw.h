/* Livadix — the PMU at firmware level: Apple's M50753 firmware (pmu-pb140.bin,
 * pmu-pb160.bin) on the M50753 model, wired as the PowerBook boards wire it
 * (see pmu_fw.c). Internal to pmu.c, which dispatches to it. */
#ifndef MC_PMU_FW_H
#define MC_PMU_FW_H

#include "adb_wire.h"
#include "m50753.h"
#include "pmu.h"

/* The M50753's oscillator: the frequency at which the firmware's own
 * second (91 periods of timer X, prescaler $FF and timer $0B, ratio 1/n)
 * is one second: 91 x 16 x 255 x 11 Hz, within the part's 4 MHz +-10%
 * rating (data sheet, "Recommended operating conditions") †. phi is a
 * quarter of it (Figure 15, note 1). */
#define MC_PMU_FW_XIN 4084080u
#define MC_PMU_FW_PHI (MC_PMU_FW_XIN / 4u)
/* Asleep, the firmware's loop is paced by the PMU's own speed: some 130
 * cycles a pass and 60 passes to its second (its $25 reload while not
 * awake), which a 32,768 Hz clock gives (phi 8,192 Hz). The board runs it
 * from that clock while the sleep loop's plane bits are set (P0 bits 0, 1,
 * 2 and 4, which wake clears) and the firmware is in that loop (its RAM $00
 * bit 6, set on the way in and cleared on the way out) †. */
#define MC_PMU_FW_SLOW_PHI (32768u / 4u)

struct mc_pmu_fw {
    mc_m50753 chip;
    mc_adb_wire wire;
    mc_pmu *pmu;
    uint64_t base_c15m, base_cycles;     /* chip cycle base_cycles was at guest time base_c15m */
    uint32_t phi;                         /* the chip's clock: MC_PMU_FW_PHI or the sleep clock */
    uint64_t last_edge;                   /* guest time of the last /REQ or /ACK change */
    uint64_t wake_release;                /* when the key line (P1.1) returns low (0: not pulsed) */
    uint64_t key_release;                 /* when the power key (IN.4) is released (0: not pressed) */
    uint8_t wake_hold;                    /* 2 a key to press, 1 a key held until the firmware wakes */
    uint8_t key_hold;                     /* the power key held until the firmware's debounce has it */
    int contrast_reported;
};

mc_pmu_fw *mc_pmu_fw_new(mc_pmu *p, const uint8_t *firmware);
void mc_pmu_fw_free(mc_pmu_fw *f);
void mc_pmu_fw_reset(mc_pmu_fw *f);
void mc_pmu_fw_event(mc_pmu_fw *f, uint64_t now);
void mc_pmu_fw_req(mc_pmu_fw *f, int level);
void mc_pmu_fw_port(mc_pmu_fw *f, uint8_t value);
int mc_pmu_fw_wake(mc_pmu_fw *f);
/* The Portable's reset button: INT2 and P3.0 low while pressed. */
void mc_pmu_fw_reset_switch(mc_pmu_fw *f, int pressed);
/* MC_PMU_PORTABLE: the 60.15 Hz on INT1. */
void mc_pmu_fw_tick(mc_pmu_fw *f, int level);
/* Runs the chip up to guest time `now`. */
void mc_pmu_fw_catch_up(mc_pmu_fw *f, uint64_t now);
/* Parameter RAM is the firmware's RAM $30-$AF; the PB 160 firmware's clock
 * is $30-$33 (big-endian seconds, the first four bytes of it). The
 * Portable's: $2F-$AE, its clock $25-$28 (pmu_fw.c). */
void mc_pmu_fw_pram_get(mc_pmu_fw *f, uint8_t *out);
void mc_pmu_fw_pram_put(mc_pmu_fw *f, const uint8_t *in);
uint32_t mc_pmu_fw_seconds(mc_pmu_fw *f);
void mc_pmu_fw_set_seconds(mc_pmu_fw *f, uint32_t seconds);

#endif
