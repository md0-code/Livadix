/* Helpers shared by the SCSI tests: synthesized images under work/ and a
 * whole command run directly over an mc_scsi_bus (arbitration, selection,
 * command, data, status, message) as SCSI-2 5.1 sequences it. */
#ifndef MC_SCSI_UTIL_H
#define MC_SCSI_UTIL_H

#include "hw_test.h"
#include "scsi_bus.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* A file of `blocks` 512-byte blocks (`sector` 512 or 2048 sized image)
 * where every block starts with its number and a tag byte. */
static inline const char *scsi_make_image(const char *suite, const char *name, uint32_t bytes, uint8_t tag) {
    const char *path = hw_work_path(suite, name);
    FILE *f = fopen(path, "wb");
    if (!f) return path;
    uint8_t block[512];
    for (uint32_t b = 0; b < bytes / 512; b++) {
        for (unsigned i = 0; i < 512; i++) block[i] = (uint8_t)(i ^ tag);
        block[0] = (uint8_t)(b >> 24); block[1] = (uint8_t)(b >> 16); block[2] = (uint8_t)(b >> 8); block[3] = (uint8_t)b;
        block[4] = tag;
        fwrite(block, 1, 512, f);
    }
    fclose(f);
    return path;
}

typedef struct scsi_result {
    uint8_t status, message;
    uint32_t moved;
    int ok;         /* selection succeeded and the target ended with COMMAND COMPLETE */
} scsi_result;

/* Runs one command on `id` from initiator 7: data in goes to `in` (up to
 * `in_size`), data out comes from `out` (`out_size` bytes). */
static inline scsi_result scsi_run(mc_scsi_bus *bus, int id, const uint8_t *cdb, unsigned cdb_len, uint8_t *in,
                                   uint32_t in_size, const uint8_t *out, uint32_t out_size) {
    scsi_result r = {0xFF, 0xFF, 0, 0};
    if (!mc_scsi_bus_arbitrate(bus, 7) || !mc_scsi_bus_select(bus, id, 1)) return r;
    mc_scsi_bus_set_atn(bus, 0);
    mc_scsi_bus_out(bus, 0x80);         /* IDENTIFY, LUN 0 */
    for (unsigned i = 0; i < cdb_len && mc_scsi_bus_phase(bus) == MC_SCSI_COMMAND; i++) mc_scsi_bus_out(bus, cdb[i]);
    while (mc_scsi_bus_phase(bus) == MC_SCSI_DATA_IN) {
        uint8_t b = mc_scsi_bus_in(bus, 0);
        if (r.moved < in_size) in[r.moved] = b;
        r.moved++;
    }
    while (mc_scsi_bus_phase(bus) == MC_SCSI_DATA_OUT) {
        mc_scsi_bus_out(bus, r.moved < out_size ? out[r.moved] : 0);
        r.moved++;
    }
    if (mc_scsi_bus_phase(bus) != MC_SCSI_STATUS) {
        mc_scsi_bus_release(bus);       /* leave the bus free for the next test */
        return r;
    }
    r.status = mc_scsi_bus_in(bus, 0);
    if (mc_scsi_bus_phase(bus) != MC_SCSI_MSG_IN) return r;
    r.message = mc_scsi_bus_in(bus, 0);
    r.ok = mc_scsi_bus_phase(bus) == MC_SCSI_BUS_FREE && r.message == 0;
    return r;
}

#endif
