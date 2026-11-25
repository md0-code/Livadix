/* Livadix - ATI mach64 GT-B display controller.
 * ATI RRG-G02700 rev.0.10 (1996), ch.2 aperture mapping, ch.3 register
 * addresses, ch.4 CRTC, palette, PLL and 2D engine, ch.7 PCI configuration.
 * Apple 5500/6500 Developer Note ch.2 identifies the onboard ATI-264GT.
 * Original ROM 6E92FE08 identifies ATY,264GT-B and programs the native
 * driver through BAR0 +$7FFC00, with pixels in the big-endian aperture.
 * 2D context layout and draw side effects: ATI PRG888GX0-01 (1994),
 * chapter 2, pp.2-43 and 2-47/48.
 * GT-B's separate register BAR follows the later mach64 mapping also
 * documented in ATI RRG-G03300 rev.2.01x, section 2.4.1.
 */
#ifndef MC_MACH64_H
#define MC_MACH64_H
#include "../hw.h"
#include "../ppc/chips/pci.h"
#include <stdint.h>

enum { MC_MACH64_LINE_IRQ };
#define MC_MACH64_APERTURE 0x1000000u
#define MC_MACH64_REGISTERS 0x800u

typedef struct mc_mach64 {
    mc_dev dev;
    mc_pci_config cfg;
    uint8_t *vram, *scanout;
    uint32_t vram_size, scanout_size;
    uint32_t reg[MC_MACH64_REGISTERS / 4];
    uint8_t pll[64];
    uint8_t palette_rgb[256][3];
    uint32_t palette[256];
    uint8_t read_index, write_index, read_component, write_component, dac_mask;
    uint8_t vblank, irq, monitor_code;
    int8_t monitor_ext;
    uint64_t frame_ticks, frame_start, next_vblank, next_vline, draws;
    uint64_t host_data;
    unsigned host_bits, host_column, host_row, host_width, host_height;
    int host_active, host_line;
    uint32_t masked_palette[256];
} mc_mach64;

typedef struct mc_mach64_view {
    uint32_t width, height, depth, row_bytes, native_depth, native_row_bytes;
    const uint8_t *pixels;
    const uint32_t *palette;
} mc_mach64_view;

int mc_mach64_init(mc_mach64 *, const mc_hw_host *, uint32_t vram_size);
void mc_mach64_free(mc_mach64 *);
void mc_mach64_reset(mc_mach64 *);
void mc_mach64_set_monitor(mc_mach64 *, unsigned code, int extended);
mc_pci_function mc_mach64_function(mc_mach64 *);
int mc_mach64_view_get(mc_mach64 *, mc_mach64_view *);
extern const mc_dev_ops mc_mach64_ops;
#endif
