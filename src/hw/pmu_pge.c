/* Livadix — the PG&E at firmware level (see pmu_pge.h). Livadix's own code. */
#include "pmu_pge.h"


#include <stdlib.h>
#include <string.h>

#define ACTIVE_SLICE 64u            /* E cycles while a byte is under way */
#define IDLE_SLICE 2048u

/* The ADB's timing in E cycles (Guide to the Macintosh Family Hardware,
 * "Apple Desktop Bus": the 100 us bit cell, the 800 us attention, the 140-
 * 260 us stop-to-start time, the 3 ms reset)†. */
#define ADB_BIT (MC_PGE_E_HZ / 10000u)
#define ADB_COMMAND_TIME (17u * ADB_BIT)            /* attention, sync, eight bits, stop */
#define ADB_BYTE_TIME (8u * ADB_BIT)
#define ADB_GAP (2u * ADB_BIT)                      /* stop-to-start, then the start bit */
#define ADB_NO_ANSWER (3u * ADB_BIT)
#define INT_PULSE (MC_PGE_E_HZ / 20000u)           /* 50 us */
#define INT_AGAIN (MC_PGE_E_HZ / 1000u)            /* 1 ms with /REQ high */

enum { ADB_IDLE, ADB_SENDING, ADB_ANSWERING };

static uint64_t pge_time(const mc_pmu_pge *g) {
    return g->base_c15m + (g->cpu.cycles - g->base_cycles) * MC_C15M_HZ / MC_PGE_E_HZ;
}

/* A port's data register reads its outputs' latches and its inputs' pins;
 * $00-$03 have their directions at $04-$07, $20-$2A at the next address. */
static int is_port(uint16_t a) { return a <= 0x03 || (a >= 0x20 && a <= 0x2A && !(a & 1)); }
static uint8_t direction(const mc_pmu_pge *g, uint16_t a) { return a <= 0x03 ? g->reg[a + 4] : g->reg[a + 1]; }

static void int_pulse(mc_pmu_pge *g);

static void outputs(mc_pmu_pge *g) {
    mc_pmu *p = g->pmu;
    /* /ACK: $26 bit 6. */
    const uint8_t dir = g->reg[0x27], level = (dir & 0x40u) ? (uint8_t)((g->reg[0x26] >> 6) & 1u) : 1u;
    if (level != p->ack) {
        p->ack = level;
        mc_dev_line(&p->dev, MC_PMU_LINE_ACK, level);
    }
    /* The main power: $22 bit 1 while driven; $24 bit 3 reads it back. */
    if (g->reg[0x23] & 0x02u) {
        /* Off with RAM $91 holding $62 is a sleep: the program keeps it
         * through its way to sleep and clears it on its way to shut down
         * ($9DEA, $9E31), and the boot ROM takes a reset with it as a wake
         * ($FE28). */
        const uint8_t power = (g->reg[0x22] & 0x02u) ? MC_PMU_POWER_ON
                              : g->ram[0x91 - 0x40] == 0x62u ? MC_PMU_POWER_SLEEP : MC_PMU_POWER_OFF;
        if (power != p->power) {
            p->power = power;
            mc_dev_line(&p->dev, MC_PMU_LINE_POWER, power);
        }
    }
    /* $24 bit 3 reads the power back. Bit 6 is the AC adapter: Livadix's Duo
     * always runs on it†. The program takes the line, debounced ($99DA,
     * RAM $C6 bit 7, then $CB bit 5 by $99BE), as external power: its
     * start ($8141) then powers on without the battery check ($A1B6, which
     * on battery alone needs the battery's identification, RAM $C7 bit 3,
     * from its serial-number chip, absent here), and asleep or off it
     * leaves the boot ROM's stopped loop for its charging loop ($CA18 to
     * $800B, $C983, $C9A0), where a key, the power key passing that check
     * ($8730, $97AE), restarts it ($C9F0 to $8122). */
    const uint8_t on = p->power == MC_PMU_POWER_ON;
    g->pins[0x24] = (uint8_t)((g->pins[0x24] & ~0x48u) | (on ? 0x08u : 0) | 0x40u);
    /* $22 bit 1 released (an input: the program lets it go once the power
     * is off, $9FCA) reads the power line: low while off. Read high, as a
     * floating pin, the charging loop took the Duo for powered ($9C22: with
     * $03 bit 6 low, counted by $9C3C into the restart request, RAM $8D
     * bit 5) and woke it four seconds after it slept. */
    g->pins[0x22] = (uint8_t)((g->pins[0x22] & ~0x02u) | (on ? 0x02u : 0));
    /* /INT: $22 bit 2 driven low, held until the Mac's $78; VIA1's CB1
     * sees a pulse on its falling edge (CB1 is also the SPI clock). */
    const uint8_t irq = (g->reg[0x23] & 0x04u) ? (uint8_t)((g->reg[0x22] >> 2) & 1u) : 1u;
    if (irq != g->int_out) {
        g->int_out = irq;
        if (!irq) int_pulse(g);
    }
    /* The processor's reset: $20 bit 2 driven low. */
    const uint8_t reset = (g->reg[0x21] & 0x04u) && !(g->reg[0x20] & 0x04u);
    if (reset != p->reset) {
        p->reset = reset;
        mc_dev_line(&p->dev, MC_PMU_LINE_RESET, reset);
    }
}

static void int_pulse(mc_pmu_pge *g) {
    if (!g->pmu->int_level) return;
    g->pmu->int_level = 0;
    mc_dev_line(&g->pmu->dev, MC_PMU_LINE_INT, 0);
    g->int_left = INT_PULSE;
}

static void int_end(mc_pmu_pge *g) {
    g->int_left = 0;
    if (!g->pmu->int_level) {
        g->pmu->int_level = 1;
        mc_dev_line(&g->pmu->dev, MC_PMU_LINE_INT, 1);
    }
}

/* --- the ADB transceiver --------------------------------------------------------- */

/* The command is out: the bus outside (the Duo Dock's ADB port, not
 * emulated) never answers; the built-in keyboard and trackball are the
 * program's own (they answer from its RAM, at addresses $5C and $60). */
static void adb_transaction(mc_pmu_pge *g) {
    g->adb_in_n = g->adb_in_pos = 0;
    g->adb_left = ((g->adb_out[0] >> 2) & 3u) == 3u ? ADB_NO_ANSWER : ADB_BIT;
    g->adb_phase = ADB_ANSWERING;
}

static void adb_event(mc_pmu_pge *g) {
    if (g->adb_phase == ADB_SENDING) {
        g->reg[0x19] |= 0x80u;      /* the transmitter empty again */
    } else if (g->adb_phase == ADB_ANSWERING) {
        if (g->adb_in_pos < g->adb_in_n) {
            g->reg[0x1A] = g->adb_in[g->adb_in_pos++];
            g->reg[0x19] |= 0x08u;
            g->adb_left = ADB_BYTE_TIME;
            return;
        }
        g->reg[0x19] |= g->adb_in_n || ((g->adb_out[0] >> 2) & 3u) != 3u ? 0x40u : 0x20u;
        g->adb_phase = ADB_IDLE;
    }
}

static void adb_control(mc_pmu_pge *g, uint8_t old, uint8_t v) {
    if (!v) {
        g->adb_phase = ADB_IDLE;
        g->adb_left = 0;
        return;
    }
    if ((v & 0x80u) && !(old & 0x80u) && g->adb_phase == ADB_IDLE) {
        g->adb_phase = ADB_SENDING;
        g->adb_out_n = 0;
        g->reg[0x19] = (uint8_t)((g->reg[0x19] & ~0x78u) | 0x80u);
    } else if (!(v & 0x80u) && (old & 0x80u) && g->adb_phase == ADB_SENDING && g->adb_out_n) {
        g->reg[0x19] &= 0x7Fu;
        adb_transaction(g);
    }
}

static void adb_data(mc_pmu_pge *g, uint8_t v) {
    if (g->adb_phase != ADB_SENDING || g->adb_out_n >= sizeof g->adb_out) return;
    g->adb_out[g->adb_out_n++] = v;
    g->reg[0x19] &= 0x7Fu;
    g->adb_left = g->adb_out_n == 1 ? ADB_COMMAND_TIME : ADB_BYTE_TIME;
}

/* --- the keyboard and the trackball ---------------------------------------------------
 * The program scans the keyboard every tick ($90C3): one column at a time
 * driven low through its direction bit (port $02, directions $06), the
 * rows read on $00 (rows 0-7) and $01 bits 0-1 (rows 8-9); its table at
 * $92F2 gives each row's eight columns' ADB key codes. The modifiers read
 * on $01 bits 3-6 (Command, Control, Shift, Option; $91D4) and bit 7 (Caps
 * Lock), low when down. The table's empty places hold 0, as A's code does:
 * Livadix puts A at row 0, column 4 (by S, F and H in that column)†. The trackball ($8DF4): the counters $15 (X) and
 * $16 (Y), its button $14 bit 7 low. */

static const uint8_t KEYS[10][8] = {
    {0x30, 0x00, 0x35, 0x00, 0x00, 0x0C, 0x06, 0x00}, {0x0D, 0x07, 0x63, 0x61, 0x01, 0x13, 0x32, 0x12},
    {0x0F, 0x02, 0x00, 0x08, 0x00, 0x15, 0x0E, 0x14}, {0x10, 0x05, 0x62, 0x60, 0x03, 0x16, 0x11, 0x17},
    {0x22, 0x26, 0x09, 0x00, 0x04, 0x00, 0x20, 0x1A}, {0x7E, 0x25, 0x00, 0x0B, 0x28, 0x1C, 0x1F, 0x19},
    {0x23, 0x00, 0x00, 0x2D, 0x29, 0x1D, 0x21, 0x1B}, {0x1E, 0x27, 0x2E, 0x2B, 0x2F, 0x0A, 0x00, 0x00},
    {0x00, 0x00, 0x34, 0x00, 0x2C, 0x18, 0x2A, 0x33}, {0x3E, 0x3D, 0x31, 0x3B, 0x3C, 0x00, 0x24, 0x00},
};

static uint8_t matrix(const mc_pmu_pge *g, uint16_t a) {
    const uint8_t driven = (uint8_t)(g->reg[0x06] & ~g->reg[0x02]);
    uint16_t rows = 0;
    for (int c = 0; c < 8; c++)
        if (driven & (1u << c)) rows |= g->keys[c];
    /* The power key ($7E in the table at row 5, column 0) also reads on row
     * 5 with no column driven, as the program looks for it first ($934A,
     * from its power-key check $97AE). */
    if (!driven && (g->keys[0] & 0x20u)) rows |= 0x20u;
    if (a == 0x00) return (uint8_t)~rows;
    return (uint8_t)(~((rows >> 8) & 0x03u) & ~g->mods);
}

/* --- memory ------------------------------------------------------------------------ */

static uint8_t pge_read(void *ctx, uint16_t a) {
    mc_pmu_pge *g = ctx;
    if (a < 0x40) {
        if (is_port(a)) {
            const uint8_t d = direction(g, a);
            uint8_t pins = g->pins[a];
            if (a <= 0x01) pins = matrix(g, a);
            return (uint8_t)((g->reg[a] & d) | (pins & (uint8_t)~d));
        }
        switch (a) {
        case 0x0C: return g->spi_in;
        case 0x14: return (uint8_t)((g->reg[0x14] & 0x7Fu) | (g->buttons & 1u ? 0 : 0x80u));
        case 0x15: case 0x16: {
            /* The trackball's counters, cleared as they are read. */
            int16_t *n = a == 0x15 ? &g->dx : &g->dy;
            const int v = *n < -64 ? -64 : *n > 63 ? 63 : *n;
            *n = (int16_t)(*n - v);
            return (uint8_t)v;
        }
        case 0x1A: g->reg[0x19] &= 0xF7u; return g->reg[0x1A];
        case 0x1D: return g->ad[g->reg[0x1E] & 7u];
        case 0x1E: return (uint8_t)(g->reg[0x1E] | 0x80u);
        case 0x0F: return g->reg[0x0F];
        default: return g->reg[a];
        }
    }
    if (a == 0x0FF0) return 0;                  /* the COP register */
    if (a >= 0xFFF0 && a < 0xFFFE && !(g->reg[0x1C] & 0x40u)) return g->vectors[a - 0xFFF0];
    if (a >= MC_PGE_BOOT_BASE) return g->boot[a - MC_PGE_BOOT_BASE];
    return g->ram[a - 0x40];
}

static void pge_write(void *ctx, uint16_t a, uint8_t v) {
    mc_pmu_pge *g = ctx;
    if (a < 0x40) {
        const uint8_t old = g->reg[a];
        switch (a) {
        case 0x18: g->reg[a] = v; adb_control(g, old, v); return;
        case 0x19: g->reg[a] = (uint8_t)(old & (v | ~0x30u)); return;     /* bits 5, 4 cleared by a 0 */
        case 0x1A: adb_data(g, v); return;
        case 0x0F: {
            /* The key line remains asserted while a key is held. The
             * charging loop masks it during the matrix scan ($90C3) and
             * enables it again at $C9AC. A press during that scan must
             * still request the key interrupt when it is enabled. */
            uint16_t held = g->mods;
            for (unsigned c = 0; c < 8; c++) held |= g->keys[c];
            g->reg[a] = (uint8_t)((v & 0x7Fu) | ((v & 8u) && held ? 0x80u : 0));
            return;
        }
        default: break;
        }
        g->reg[a] = v;
        if (a == 0x0C && (g->reg[0x0A] & 0x50u) == 0x50u) {
            /* SPI master: eight clocks through VIA1's shift register at the
             * SPCR's rate (E/2, /4, /16, /32). */
            static const uint8_t div[4] = {2, 4, 16, 32};
            int_end(g);
            g->spi_in = g->pmu->shift ? g->pmu->shift(g->pmu->shift_ctx, v) : 0xFF;
            g->spi_left = 8u * div[g->reg[0x0A] & 3u];
            g->reg[0x0B] &= 0x7Fu;
        }
        if (a >= 0x20 && a <= 0x27) outputs(g);
        return;
    }
    /* $0FF0 (the COP reset) and the ROM take nothing; the download's
     * vectors are kept. */
    if (a >= 0xFFF0) g->vectors[a - 0xFFF0] = v;
    else if (a != 0x0FF0 && a < MC_PGE_BOOT_BASE) g->ram[a - 0x40] = v;
}

int mc_pmu_pge_key(mc_pmu_pge *g, uint8_t code, int down) {
    mc_pmu_pge_catch_up(g, mc_dev_now(&g->pmu->dev));
    /* The right-hand modifiers are the left-hand ones here. */
    static const uint8_t MODS[][2] = {{0x37, 0x08}, {0x36, 0x10}, {0x7D, 0x10}, {0x38, 0x20}, {0x7B, 0x20},
                                      {0x3A, 0x40}, {0x7C, 0x40}, {0x39, 0x80}};
    for (size_t i = 0; i < sizeof MODS / sizeof *MODS; i++)
        if (MODS[i][0] == code) {
            g->mods = (uint8_t)(down ? g->mods | MODS[i][1] : g->mods & ~MODS[i][1]);
            return 1;
        }
    for (int r = 0; r < 10; r++)
        for (int c = 0; c < 8; c++)
            if ((KEYS[r][c] == code || (code == 0x7F && KEYS[r][c] == 0x7E)) && (code || (r == 0 && c == 4))) {
                g->keys[c] = (uint16_t)(down ? g->keys[c] | 1u << r : g->keys[c] & ~(1u << r));
                if (down) g->reg[0x0F] |= 0x80u;
                return 1;
            }
    return 0;
}

void mc_pmu_pge_mouse(mc_pmu_pge *g, int dx, int dy, int button) {
    mc_pmu_pge_catch_up(g, mc_dev_now(&g->pmu->dev));
    g->dx = (int16_t)(g->dx + dx);
    g->dy = (int16_t)(g->dy + dy);
    g->buttons = (uint8_t)(button != 0);
}

mc_pmu_pge *mc_pmu_pge_new(mc_pmu *p, const uint8_t *boot) {
    mc_pmu_pge *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->pmu = p;
    memcpy(g->boot, boot, MC_PGE_BOOT_SIZE);
    mc_cpu6805_init(&g->cpu, g, pge_read, pge_write, 0xFFFE);
    g->cpu.pc_mask = 0xFFFF;
    return g;
}

void mc_pmu_pge_free(mc_pmu_pge *g) { free(g); }

void mc_pmu_pge_load(mc_pmu_pge *g, const uint8_t *image, size_t size) {
    for (size_t i = 0; i < size && MC_PGE_XRAM_BASE + i <= 0xFFFFu; i++) pge_write(g, (uint16_t)(MC_PGE_XRAM_BASE + i), image[i]);
    g->loaded = 1;
}

void mc_pmu_pge_reset(mc_pmu_pge *g) {
    const uint64_t now = mc_dev_now(&g->pmu->dev);
    uint8_t clock[4];
    memcpy(clock, g->reg + 0x38, 4);    /* the clock runs on */
    memset(g->reg, 0, sizeof g->reg);
    memcpy(g->reg + 0x38, clock, 4);
    /* A cold start's external RAM fails the boot ROM's checksum: no
     * program until the Mac downloads one. */
    if (!g->loaded) memset(g->ram + (MC_PGE_XRAM_BASE - 0x40), 0xFF, MC_PGE_BOOT_BASE - MC_PGE_XRAM_BASE);
    memset(g->pins, 0xFF, sizeof g->pins);
    g->pins[0x22] = (uint8_t)(g->pmu->req ? 0xFF : 0xBF);
    /* The battery (channels 0 and 1, summed), the charger (2-4)†. */
    static const uint8_t AD[8] = {0x80, 0x80, 0x40, 0x80, 0x80, 0x80, 0x80, 0x80};
    memcpy(g->ad, AD, sizeof g->ad);
    g->spi_left = 0;
    g->second_left = MC_PGE_E_HZ;
    g->tick_left = MC_PGE_E_HZ / MC_PGE_TICK_HZ;
    g->adb_phase = ADB_IDLE;
    g->adb_left = 0;
    g->int_out = 1;
    g->int_left = 0;
    g->again_left = 0;
    g->base_c15m = now;
    g->base_cycles = g->cpu.cycles;
    mc_cpu6805_reset(&g->cpu);
    outputs(g);
    mc_dev_schedule(&g->pmu->dev, now + 1);
}

/* E cycles pass: the SPI, the ADB, the tick and the second. */
static void advance(mc_pmu_pge *g, uint32_t cycles) {
    if (g->int_left) {
        if (g->int_left <= cycles) int_end(g);
        else g->int_left -= cycles;
    }
    if (g->again_left) {
        if (g->again_left <= cycles) {
            g->again_left = 0;
            if (!g->int_out) int_pulse(g);
        } else {
            g->again_left -= cycles;
        }
    }
    if (g->spi_left) {
        if (g->spi_left <= cycles) {
            g->spi_left = 0;
            g->reg[0x0B] |= 0x80u;
        } else {
            g->spi_left -= cycles;
        }
    }
    if (g->adb_left) {
        if (g->adb_left <= cycles) {
            g->adb_left = 0;
            adb_event(g);
        } else {
            g->adb_left -= cycles;
        }
    }
    if (g->tick_left <= cycles) {
        g->tick_left += MC_PGE_E_HZ / MC_PGE_TICK_HZ - cycles;
        g->reg[0x0D] |= 0x80u;
    } else {
        g->tick_left -= cycles;
    }
    if (g->second_left <= cycles) {
        g->second_left += MC_PGE_E_HZ - cycles;
        g->reg[0x0D] |= 0x40u;
        for (int i = 0x3B; i >= 0x38 && !++g->reg[i]; i--) {}
    } else {
        g->second_left -= cycles;
    }
}

static void interrupts(mc_pmu_pge *g) {
    if (g->cpu.cc & MC_6805_I) return;
    const int boot = (g->reg[0x1C] & 0x40u) != 0;
    uint16_t vector = 0;
    if (!boot && (g->reg[0x19] & g->reg[0x18] & 0xE8u)) vector = 0xFFF8;
    else if ((g->reg[0x0F] & 0x88u) == 0x88u) vector = 0xFFF0;
    else if ((g->reg[0x0D] & 0xA0u) == 0xA0u) vector = boot ? 0xFFF0 : 0xFFF6;
    else if ((g->reg[0x0D] & 0x50u) == 0x50u) vector = boot ? 0xFFF0 : 0xFFF4;
    if (vector) g->cpu.cycles += mc_cpu6805_interrupt(&g->cpu, vector);
}

void mc_pmu_pge_catch_up(mc_pmu_pge *g, uint64_t now) {
    while (pge_time(g) < now) {
        unsigned cycles = mc_cpu6805_step(&g->cpu);
        if (!cycles) {
            /* STOP or WAIT: nothing runs until an interrupt; time moves on
             * to the next event. */
            uint64_t skip = (now - pge_time(g)) * MC_PGE_E_HZ / MC_C15M_HZ + 1;
            if (skip > g->tick_left) skip = g->tick_left;
            if (skip > g->second_left) skip = g->second_left;
            if (g->adb_left && skip > g->adb_left) skip = g->adb_left;
            if (!skip) skip = 1;
            g->cpu.cycles += skip;
            cycles = (unsigned)skip;
        }
        advance(g, cycles);
        interrupts(g);
    }
}

void mc_pmu_pge_event(mc_pmu_pge *g, uint64_t now) {
    mc_pmu_pge_catch_up(g, now);
    const mc_pmu *p = g->pmu;
    const unsigned slice = (!p->req || !p->ack || g->spi_left) ? ACTIVE_SLICE : IDLE_SLICE;
    mc_dev_schedule(&g->pmu->dev, now + (uint64_t)slice * MC_C15M_HZ / MC_PGE_E_HZ);
}

void mc_pmu_pge_req(mc_pmu_pge *g, int level) {
    const uint64_t now = mc_dev_now(&g->pmu->dev);
    mc_pmu_pge_catch_up(g, now);
    g->pins[0x22] = (uint8_t)((g->pins[0x22] & ~0x40u) | (level ? 0x40u : 0));
    /* /INT still held when the Mac has been quiet a while: a new edge
     * (the Mac's exchanges clock CB1 too)†. */
    g->again_left = level ? INT_AGAIN : 0;
    mc_dev_schedule(&g->pmu->dev, now + (uint64_t)ACTIVE_SLICE * MC_C15M_HZ / MC_PGE_E_HZ);
}

void mc_pmu_pge_pram_get(mc_pmu_pge *g, uint8_t *out) {
    mc_pmu_pge_catch_up(g, mc_dev_now(&g->pmu->dev));
    memcpy(out, g->ram + (MC_PGE_PRAM - 0x40), 256);
}

void mc_pmu_pge_pram_put(mc_pmu_pge *g, const uint8_t *in) {
    mc_pmu_pge_catch_up(g, mc_dev_now(&g->pmu->dev));
    memcpy(g->ram + (MC_PGE_PRAM - 0x40), in, 256);
}

uint32_t mc_pmu_pge_seconds(mc_pmu_pge *g) {
    mc_pmu_pge_catch_up(g, mc_dev_now(&g->pmu->dev));
    return (uint32_t)g->reg[0x38] << 24 | (uint32_t)g->reg[0x39] << 16 | (uint32_t)g->reg[0x3A] << 8 | g->reg[0x3B];
}

void mc_pmu_pge_set_seconds(mc_pmu_pge *g, uint32_t s) {
    mc_pmu_pge_catch_up(g, mc_dev_now(&g->pmu->dev));
    for (int i = 0; i < 4; i++) g->reg[0x38 + i] = (uint8_t)(s >> (24 - 8 * i));
}
