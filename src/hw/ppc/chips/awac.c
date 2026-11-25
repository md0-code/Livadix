/* Livadix — the AWAC sound codec (awac.h).
 *
 * Livadix's own code. */
#include "awac.h"

#include <string.h>

/* Gain for attenuation steps of 1.5 dB, 0-15: round(32768 x 10^(-1.5 n / 20)). */
static const uint32_t att_gain[16] = {32768, 27554, 23197, 19530, 16443, 13843, 11654, 9811,
                                      8260, 6954, 5855, 4929, 4150, 3494, 2941, 2476};

static void update_gain(mc_awac *a) {
    const uint16_t r = a->reg[a->out_reg];
    a->gain[0] = att_gain[(r >> 6) & 15u];
    a->gain[1] = att_gain[r & 15u];
}

void mc_awac_init(mc_awac *a, const mc_hw_host *host, const char *name) {
    memset(a, 0, sizeof *a);
    a->dev.ops = &mc_awac_ops;
    a->dev.host = host;
    a->dev.name = name;
    a->out_reg = 4;
    update_gain(a);
}

void mc_awac_control(mc_awac *a, uint16_t word) {
    a->reg[(word >> 12) & 7u] = word & 0xFFFu;
    update_gain(a);
}

uint8_t mc_awac_status(const mc_awac *a, unsigned index) { return a->status[index & 3u]; }

void mc_awac_set_status(mc_awac *a, unsigned index, uint8_t value) { a->status[index & 3u] = value; }

void mc_awac_output(const mc_awac *a, int16_t *stereo, size_t frames) {
    for (size_t i = 0; i < frames; i++)
        for (unsigned c = 0; c < 2; c++) stereo[2 * i + c] = (int16_t)((int32_t)stereo[2 * i + c] * (int32_t)a->gain[c] / 32768);
}

static void awac_reset(void *dev) {
    mc_awac *a = dev;
    memset(a->reg, 0, sizeof a->reg);
    update_gain(a);
}

const mc_dev_ops mc_awac_ops = {NULL, NULL, NULL, awac_reset, NULL};
