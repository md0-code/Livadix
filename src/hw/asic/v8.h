/* Livadix — the V8 family: V8 (Macintosh LC, LC II), Eagle (Classic II) and
 * Spice (Color Classic), the ASIC beside the 68020/68030 that decodes the
 * memory map, holds the VIA2 registers and drives the built-in video.
 *
 * Sources: Macintosh Color Classic Developer Note (Apple, 1993): chapter
 * 2, "Spice Custom IC" (memory mapping, VIA1 and a second set of VIA
 * registers "like those in the RBV custom IC", video addressing and
 * timing, SWIM2, sound timing, the push buttons), Figure 2-1 (address
 * lines A31 and A27-A0 into the ASIC), Figures 2-2 and 2-3 (the 24-bit
 * and 32-bit maps: RAM from 0, ROM from $40A0 0000 / $A0 0000, I/O from
 * $50F0 0000 / $F0 0000, the expansion slot at $FE00 0000 / $E0 0000,
 * VRAM at $50F4 0000, VDAC at $50F2 4000, the VIA2 registers at
 * $50F2 6000), "RAM Addresses" and Figure 1-3 (built-in RAM after the
 * SIMMs' bank, its top 2 MB always at $80 0000-$9F FFFF), "Video RAM"
 * (256 or 512 KB), Table 1-1 (depths by VRAM size), Table 2-1 (512 x 384
 * at 60.15 Hz from C15M); chapter 4 ("the Macintosh LC II's ROM with
 * changes"). Guide to the Macintosh Family Hardware, 2nd ed., chapter 4,
 * for the VIA2 registers the RBV defines (rbv.h).
 *
 * Observed from the ROMs (LC 350EACF0, LC II
 * 35C28F5F, Classic II 3193670E, Color Classic ECD99DC0):
 *   - the decoder table of decoder kind 7 (LC ROM offset $3AE4; Classic
 *     II $4C7A6): ROM $00A0 0000, VIA1 $50F0 0000, SCC $50F0 4000, SCSI
 *     $50F1 0000, pseudo-DMA $50F1 2000 and $50F0 6000, sound $50F1 4000,
 *     SWIM $50F1 6000, the Classic II's register at $50F1 8000, the VDAC
 *     $50F2 4000 and these registers $50F2 6000;
 *   - the ASIC decodes A31 and A27-A0 only: the ROM runs at $00A0 xxxx
 *     from reset (24-bit) and at $40A0 xxxx later, and the LC II's ROM
 *     reaches the top of RAM at $409F FFxx; the 68020 LC starts in the
 *     24-bit decode (A31-A24 ignored, the map of mc_v8_map24) and its
 *     _SwapMMUMode switches it with port B bit 3 (clear 24-bit, set
 *     32-bit; ROM 350EACF0 at $40A0 3F42, $40A0 3F5A);
 *   - register $01, the RAM configuration (the ROMs' RAM sizing, LC ROM
 *     $40A4 67DA-$40A4 67FE, $40A4 A5BC, $40A4 A8A4; LC II ROM
 *     $40A4 A5CC-$40A4 A632; Classic II $40A4 A94C, $40A4 AC34): bits 7-6
 *     the SIMMs' bank, 00 none, 01 2 MB, 10 4 MB, 11 8 MB, the built-in
 *     RAM after it (the LC ROM ends in $24, $64, $A4, $E4 for 0, 2, 4 and
 *     8 MB of SIMMs; it probes with $C4, the LC II's with $00 and $C4;
 *     the Macintosh TV's ROM EAF1678D codes 00 none, 01 1 MB, 11 4 MB,
 *     its table at $A042C, mc_v8_set_simm_sizes);
 *     bit 5, set in the LC's and Classic II's final values and clear in
 *     the LC II's, kept†; bits 2-1 from the LC's probe of the built-in
 *     RAM at $80 0000 ($06 a 256 KB wrap, $04 none, $02 no RAM), kept†;
 *     bits 4, 3 and 0 kept†;
 *   - the VDAC: address +0, data +1 (red, green, blue), control +2 (the
 *     ROM writes $08); a pixel of d bits addresses the CLUT entry with
 *     its bits on top and the low 8 - d bits set (1 bit: $7F white, $FF
 *     black, ROM offset $4BE94);
 *   - register $10: the depth (bits 2-0: 1, 2, 4, 8, 16 bits; the LC's
 *     driver writes $10, $13 and $32 for 1 and 8 bits at 512 x 384 and 4
 *     bits at 640 x 480), the sense lines (5-3) and video off (6), as the
 *     RBV's monitor register; the screen's rows 1024 bytes apart;
 *   - the Classic II's register at $50F1 8000: six bits written and read
 *     back by its POST (ROM $40A4 8D0A-$40A4 8D2A, the table at $48D3A),
 *     kept†; its 512 x 342 screen in RAM at $9F 9A80 (its ProductInfo's
 *     video record, ROM offset $3D54).
 *
 * The VIA2 registers (port B $00, slot flags $02, flags $03, slot enables
 * $12, enables $13), the 60.15 Hz tick and the video's VBL are those of
 * the RBV (rbv.c): mc_v8 holds an mc_rbv for them, as the Developer Note
 * describes ("a set of registers like those in the RBV").
 *
 * Livadix's own code. */
#ifndef MC_V8_H
#define MC_V8_H

#include "../hw.h"
#include "rbv.h"

#include <stdint.h>

typedef enum mc_v8_kind { MC_V8_V8, MC_V8_EAGLE, MC_V8_SPICE } mc_v8_kind;

enum {
    MC_V8_RAMCFG = 0x01,        /* the RAM configuration register */
    MC_V8_VRAM = 0x00F40000u,   /* VRAM's offset in the 16 MB I/O decode */
    MC_V8_CLASSIC_SCREEN = 0x009F9A80u
};

typedef struct mc_v8 {
    mc_dev dev;
    mc_v8_kind kind;
    mc_rbv rbv;                 /* the VIA2 registers, the tick, the VBL, the sense lines */
    uint8_t ramcfg;
    uint8_t simm_mb[4];         /* the SIMMs' bank of each RAMCFG bits 7-6 code, in MB */
    uint8_t eagle;              /* the Classic II's register at $50F1 8000 */
    void (*remap)(void *ctx);   /* the RAM configuration changed */
    void *ctx;
    /* The VDAC. */
    uint8_t dac_addr, dac_rgb, dac_ctl;
    uint8_t clut[256][3];
    uint32_t palette[256];
    uint32_t view_palette[256];
} mc_v8;

extern const mc_dev_ops mc_v8_ops;         /* the registers at $50F2 6000 */
extern const mc_dev_ops mc_v8_vdac_ops;    /* the VDAC at $50F2 4000 */
extern const mc_dev_ops mc_v8_eagle_ops;   /* the Classic II's register at $50F1 8000 */

/* `sense` the monitor's code (rbv.h, MC_RBV_SENSE_*); remap(ctx) is called
 * when the RAM configuration changes. */
void mc_v8_init(mc_v8 *v, const mc_hw_host *host, const char *name, mc_v8_kind kind, uint8_t sense,
                void (*remap)(void *ctx), void *ctx);

/* Where the RAM configuration puts the built-in RAM: the SIMMs' bank size
 * (0, 2, 4 or 8 MB; on the Macintosh TV 0, 1, 2 or 4 MB, mc_v8_set_simm_sizes),
 * the built-in RAM from there. */
uint32_t mc_v8_simm_bank(const mc_v8 *v);
/* The SIMMs' bank sizes of RAMCFG codes 0-3, in MB (default 0, 2, 4, 8). */
void mc_v8_set_simm_sizes(mc_v8 *v, const uint8_t mb[4]);

/* The 24-bit decode of the 68020 LC (hw_machine.h, map24). */
extern const uint32_t mc_v8_map24[16];

/* The picture: from `vram` (V8, Spice) or the built-in RAM's frame buffer
 * (Eagle: `screen` points at it). 0 when the video is off. */
typedef struct mc_v8_view {
    uint32_t width, height, depth, row_bytes;
    const uint8_t *pixels;
    const uint32_t *palette;
} mc_v8_view;
int mc_v8_view_get(mc_v8 *v, const uint8_t *vram, uint32_t vram_size, const uint8_t *screen, mc_v8_view *view);
/* The deepest depth the built-in video shows on a monitor with `vram`
 * bytes of VRAM. */
uint32_t mc_v8_max_depth(mc_v8_kind kind, uint8_t sense, uint32_t vram);

#endif
