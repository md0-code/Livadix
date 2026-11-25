/* Livadix — SWIM III and its SuperDrives (swim3.h).
 *
 * Livadix's own code. */
#include "swim3.h"

#include <stdio.h>
#include <string.h>

#define MS(n) ((uint64_t)MC_C15M_HZ * (n) / 1000u)
#define STEP_TIME MS(3)
#define SECTOR_TIME MS(2)
#define SECTOR_GCR (1u + MC_GCR_DATA_NIBBLES)

enum { R_ERROR = 2, R_PARAM = 3, R_PHASE = 4, R_SETUP = 5, R_MODE_CLR = 6, R_MODE_SET = 7, R_INTR = 8, R_STEPS = 9,
       R_TRACK = 10, R_SECTOR = 11, R_FORMAT = 12, R_FIRST = 13, R_COUNT = 14, R_INTEN = 15 };
enum { M_DRIVE1 = 0x02, M_DRIVE2 = 0x04, M_GO = 0x08, M_WRITE = 0x10, M_SEL = 0x20, M_SEEK = 0x80 };
enum { OP_NONE, OP_SEEK, OP_HEADER, OP_READ, OP_WRITE };

static void update_irq(mc_swim3 *s) {
    const uint8_t irq = (s->mode & 0x01u) && (s->intr & s->reg[R_INTEN]) != 0;
    if (irq != s->irq) {
        s->irq = irq;
        mc_dev_line(&s->dev, MC_SWIM3_LINE_IRQ, irq);
    }
}

static void set_drq(mc_swim3 *s, int drq) {
    if ((uint8_t)drq != s->drq) {
        s->drq = (uint8_t)drq;
        mc_dev_line(&s->dev, MC_SWIM3_LINE_DRQ, drq);
    }
}

static int selected(const mc_swim3 *s) {
    if ((s->mode & M_DRIVE1) && s->drives > 0) return 0;
    if ((s->mode & M_DRIVE2) && s->drives > 1) return 1;
    return -1;
}

/* --- the drive's lines ([ERS] Table 1; swim.c) ------------------------------------ */

static int rd_line(const mc_swim3 *s) {
    const int sel = selected(s);
    if (sel < 0) return 1;
    const mc_swim3_drive *d = &s->drive[sel];
    const uint8_t ph = s->reg[R_PHASE];
    const unsigned addr = (s->mode & M_SEL ? 8u : 0u) | (ph & 7u);
    const int inserted = d->disk && !d->ejected;
    switch (addr) {
    case 0x0: return d->dirtn;                          /* /DIRTN */
    case 0x1: return 1;                                 /* /STEP: the step taken */
    case 0x2: return d->motor ? 0 : 1;                  /* /MOTORON */
    case 0x3: return d->eject_latch;                    /* EJECT */
    case 0x4: case 0xC: return 1;                       /* RDDATA */
    case 0x5: return 1;                                 /* MFM-capable (a SuperDrive) */
    case 0x6: return 1;                                 /* /SINGLE SIDE: two sides */
    case 0x7: return 0;                                 /* /DRVIN */
    case 0x8: return inserted ? 0 : 1;                  /* /CSTIN */
    case 0x9: return inserted && !d->protect ? 1 : 0;   /* /WRTPRT */
    case 0xA: return d->track == 0 ? 0 : 1;             /* /TK0 */
    case 0xB: return 1;                                 /* /TACH, index: not timed† */
    case 0xD: return d->mfm;                            /* MFM mode */
    case 0xE: return inserted && d->motor ? 0 : 1;      /* /READY */
    default: return inserted && d->format == MC_FLOPPY_MFM1440 ? 0 : 1;   /* low-density media */
    }
}

/* LSTRB's rising edge: CA2 into the latch SEL, CA1, CA0 address. */
static void motor_set(mc_swim3 *s,unsigned drive,int on) {
    mc_swim3_drive *d=&s->drive[drive];
    if(d->motor!=(uint8_t)on)mc_dev_drive(&s->dev,drive,MC_DRIVE_MOTOR,mc_dev_now(&s->dev),(uint32_t)on,0);
    d->motor=(uint8_t)on;
}
static void head_step(mc_swim3 *s,unsigned drive) {
    mc_swim3_drive *d=&s->drive[drive];const unsigned before=d->track;
    if(d->dirtn){if(d->track)d->track--;}
    else if(d->track<MC_FLOPPY_TRACKS-1)d->track++;
    mc_dev_drive(&s->dev,drive,MC_DRIVE_STEP,mc_dev_now(&s->dev),!d->dirtn,before==d->track);
}

static void drive_command(mc_swim3 *s) {
    const int sel = selected(s);
    if (sel < 0) return;
    mc_swim3_drive *d = &s->drive[sel];
    const uint8_t ph = s->reg[R_PHASE];
    const unsigned value = (ph >> 2) & 1u, addr = (s->mode & M_SEL ? 4u : 0u) | (ph & 3u);
    switch (addr) {
    case 0: d->dirtn = (uint8_t)value; break;
    case 1:
        if (!value) {
            head_step(s,(unsigned)sel);
        }
        break;
    case 2: motor_set(s,(unsigned)sel,!value); break;
    case 3:
        if (value && d->disk && !d->ejected) {
            d->ejected = 1;
            motor_set(s,(unsigned)sel,0);
        }
        break;
    case 4: if (value) d->eject_latch = 0; break;
    case 5: d->mfm = (uint8_t)!value; break;
    default: break;
    }
}

/* --- operations ------------------------------------------------------------------- */

static int gcr(const mc_swim3 *s) { return (s->reg[R_SETUP] & 0x04u) != 0; }

static int64_t sector_lba(const mc_swim3_drive *d, unsigned track, unsigned side, unsigned sector, int gcr_numbers) {
    const int64_t first = mc_floppy_first_sector(d->format, track, side);
    if (first < 0) return -1;
    const unsigned n = mc_floppy_sectors(d->format, track);
    const unsigned index = gcr_numbers ? sector : sector - 1u;
    if ((!gcr_numbers && sector == 0) || index >= n) return -1;
    return first + index;
}

static void finish(mc_swim3 *s, uint8_t intr) {
    s->op = OP_NONE;
    s->mode &= (uint8_t)(0xFFu & ~(M_GO | M_SEEK));
    s->intr |= intr;
    set_drq(s, 0);
    update_irq(s);
    mc_dev_schedule(&s->dev, MC_NEVER);
}

static void start(mc_swim3 *s) {
    const int sel = selected(s);
    if (s->op != OP_NONE) return;
    if (s->mode & M_SEEK) {
        s->op = OP_SEEK;
        s->op_drive = sel;
        s->due = mc_dev_now(&s->dev) + STEP_TIME;
        mc_dev_schedule(&s->dev, s->due);
        return;
    }
    if (!(s->mode & M_GO)) return;
    s->op_drive = sel;
    mc_swim3_drive *d = sel >= 0 ? &s->drive[sel] : NULL;
    /* A disk written in the other encoding than the setup's gives no
     * address field (the driver then tries the other one). */
    const int ready = d && d->disk && !d->ejected && mc_floppy_is_gcr(d->format) == gcr(s);
    const unsigned count = s->reg[R_COUNT];
    s->due = mc_dev_now(&s->dev) + SECTOR_TIME * (count ? count : 1u);
    if (!count) {
        s->op = OP_HEADER;
        mc_dev_schedule(&s->dev, ready ? s->due : MC_NEVER);
        return;
    }
    if (!ready) {
        s->op = OP_HEADER;      /* no disk: nothing comes round† */
        return;
    }
    const unsigned side = (s->mode & M_SEL) ? 1u : 0u;
    s->buf_n = 0;
    s->buf_i = 0;
    s->nsec = 0;
    for (unsigned i = 0; i < count && s->buf_n + SECTOR_GCR <= sizeof s->buf; i++) {
        const unsigned sector = s->reg[R_FIRST] + i;
        const int64_t lba = sector_lba(d, d->track, side, sector, gcr(s));
        if (lba < 0) break;
        s->nsec++;
        if (s->mode & M_WRITE) continue;
        uint8_t raw[524];
        memset(raw, 0, 12);
        if (!mc_disk_read(d->disk, (uint64_t)lba, 1, raw + 12)) break;
        if (gcr(s)) {
            /* The sector number and the data field's 703 nibbles as 6-bit
             * values (swim3.h). */
            uint8_t nib[MC_GCR_DATA_NIBBLES];
            mc_gcr_encode_data(raw, nib);
            s->buf[s->buf_n++] = (uint8_t)(sector & 0x3Fu);
            for (unsigned k = 0; k < MC_GCR_DATA_NIBBLES; k++) s->buf[s->buf_n++] = (uint8_t)mc_gcr_decode6(nib[k]);
        } else {
            memcpy(s->buf + s->buf_n, raw + 12, 512);
            s->buf_n += 512u;
        }
    }
    if (!s->nsec) {
        s->error |= 0x01u;
        finish(s, 0x01u);
        return;
    }
    s->op = (s->mode & M_WRITE) ? OP_WRITE : OP_READ;
    s->wn = s->raw_left = 0;
    s->esc = s->raw_pending = s->wend = 0;
    set_drq(s, 1);
    if (s->op == OP_WRITE) mc_dev_schedule(&s->dev, s->due);
}

/* The next data field of a write's stream from `*at` (swim3.h): MFM, the
 * 512 bytes after the data mark ($99 $FB, then $99 $0F); 0 when there is
 * none; GCR, the data field after its mark. */
static int write_field(mc_swim3 *s, uint32_t *at, uint8_t data[512]) {
    for (uint32_t i = *at; i + 1 < s->wn; i++) {
        if (gcr(s)) {
            /* GCR: $D5 $AA, then 6-bit values: the mark's third byte ($AD:
             * $0B), the sector number, the 703 values of the data field. */
            if (s->wstream[i] != 0xD5u || s->wstream[i + 1] != 0xAAu || i + 4u + MC_GCR_DATA_NIBBLES > s->wn ||
                s->wstream[i + 2] != (uint8_t)mc_gcr_decode6(0xADu))
                continue;
            uint8_t nib[MC_GCR_DATA_NIBBLES], raw[524];
            for (unsigned k = 0; k < MC_GCR_DATA_NIBBLES; k++) nib[k] = mc_gcr_encode6[s->wstream[i + 4 + k] & 0x3Fu];
            *at = i + 4u + MC_GCR_DATA_NIBBLES;
            if (!mc_gcr_decode_data(nib, raw)) return 0;
            memcpy(data, raw + 12, 512);
            return 1;
        }
        if (s->wstream[i] != 0x99u || s->wstream[i + 1] != 0xFBu) continue;
        uint32_t k = i + 2;
        if (k + 1 < s->wn && s->wstream[k] == 0x99u && s->wstream[k + 1] == 0x0Fu) k += 2;
        if (k + 512u > s->wn) return 0;
        memcpy(data, s->wstream + k, 512);
        *at = k + 512u;
        return 1;
    }
    return 0;
}

static void complete(mc_swim3 *s) {
    if (s->op == OP_SEEK) {
        if (s->op_drive >= 0) {
            mc_swim3_drive *d = &s->drive[s->op_drive];
            if(s->reg[R_STEPS])head_step(s,(unsigned)s->op_drive);
            s->reg[R_TRACK] = d->track;
            if(s->reg[R_STEPS]>1) {
                s->reg[R_STEPS]--;s->due+=STEP_TIME;mc_dev_schedule(&s->dev,s->due);return;
            }
        }
        s->reg[R_STEPS] = 0;
        finish(s, MC_SWIM3_INT_SEEK);
        return;
    }
    if (s->op == OP_HEADER) {
        mc_swim3_drive *d = &s->drive[s->op_drive];
        /* The header's track (bit 7 its side), sector and format byte: the
         * MFM size code (2: 512 bytes) or the GCR format byte (sides and
         * interleave: $22 two sides, $02 one; IM II "Disk Driver"). */
        s->reg[R_TRACK] = (uint8_t)(d->track | ((s->mode & M_SEL) ? 0x80u : 0));
        s->reg[R_SECTOR] = (uint8_t)(d->next_sector + (gcr(s) ? 0u : 1u));
        const unsigned n = mc_floppy_sectors(d->format, d->track);
        d->next_sector = (uint8_t)(n ? (d->next_sector + 1u) % n : 0);
        s->reg[R_FORMAT] = gcr(s) ? (d->format == MC_FLOPPY_GCR400 ? 0x02u : 0x22u) : 0x02u;
        finish(s, MC_SWIM3_INT_HEADER);
        return;
    }
    if (s->op == OP_WRITE && s->op_drive >= 0) {
        mc_swim3_drive *d = &s->drive[s->op_drive];
        const unsigned side = (s->mode & M_SEL) ? 1u : 0u;
        uint8_t data[512];
        uint32_t at = 0;
        for (unsigned i = 0; i < s->nsec; i++) {
            const int64_t lba = sector_lba(d, d->track, side, s->reg[R_FIRST] + i, gcr(s));
            if (lba < 0 || d->protect || !write_field(s, &at, data) || !mc_disk_write(d->disk, (uint64_t)lba, 1, data)) s->error |= 0x01u;
        }
    }
    s->reg[R_SECTOR] = (uint8_t)(s->reg[R_FIRST] + s->nsec);
    s->reg[R_COUNT] = 0;
    finish(s, MC_SWIM3_INT_DONE);
}

static void swim3_event(void *dev, uint64_t now) {
    mc_swim3 *s = dev;
    if (s->op == OP_NONE || now < s->due) return;
    if (s->op == OP_READ && s->buf_i < s->buf_n) return;   /* waits for the DMA */
    complete(s);
}

int mc_swim3_dma_read(void *dev, uint8_t *byte) {
    mc_swim3 *s = dev;
    if (s->op != OP_READ || s->buf_i >= s->buf_n) return 0;
    *byte = s->buf[s->buf_i++];
    if (s->buf_i == s->buf_n) {
        set_drq(s, 0);
        mc_dev_schedule(&s->dev, s->due);
    }
    return 1;
}

/* A write's stream (swim3.h): kept until its end code ($99 $08); an MFM
 * data field's 512 bytes are taken as they come. */
int mc_swim3_dma_write(void *dev, uint8_t byte) {
    mc_swim3 *s = dev;
    if (s->op != OP_WRITE || s->wend || s->wn >= sizeof s->wstream) return 0;
    s->wstream[s->wn++] = byte;
    if (s->raw_left) {
        s->raw_left--;
    } else if (s->esc) {
        s->esc = 0;
        if (byte == 0x08u) {
            s->wend = 1;
            set_drq(s, 0);
        } else if (byte == 0xFBu && !gcr(s)) s->raw_pending = 1;
        else if (byte == 0x0Fu && s->raw_pending) {
            s->raw_pending = 0;
            s->raw_left = 512;
        }
    } else if (byte == 0x99u) s->esc = 1;
    else if (s->raw_pending) {
        s->raw_pending = 0;
        s->raw_left = 511;
    }
    return 1;
}

/* --- registers -------------------------------------------------------------------- */

static uint8_t reg_read(mc_swim3 *s, unsigned r) {
    switch (r) {
    case R_ERROR: {
        const uint8_t e = s->error;
        s->error = 0;
        return e;
    }
    case R_MODE_CLR: return s->mode;
    case R_MODE_SET: return (uint8_t)(rd_line(s) ? 0x04u : 0);
    case R_INTR: {
        const uint8_t f = s->intr;
        s->intr = 0;
        update_irq(s);
        return f;
    }
    default: return s->reg[r];
    }
}

static void reg_write(mc_swim3 *s, unsigned r, uint8_t v) {
    switch (r) {
    case R_PHASE: {
        const uint8_t was = s->reg[R_PHASE];
        s->reg[R_PHASE] = v;
        if ((v & 0x08u) && !(was & 0x08u)) drive_command(s);
        break;
    }
    case R_MODE_CLR:
        s->mode &= (uint8_t)(0xFFu & ~v);
        if (!(s->mode & (M_GO | M_SEEK)) && s->op != OP_NONE && s->op != OP_SEEK) {
            s->op = OP_NONE;
            set_drq(s, 0);
            mc_dev_schedule(&s->dev, MC_NEVER);
        }
        update_irq(s);
        break;
    case R_MODE_SET:
        s->mode |= v;
        if (v & (M_GO | M_SEEK)) start(s);
        update_irq(s);
        break;
    case R_INTR: s->intr &= (uint8_t)(0xFFu & ~v); break;
    default: s->reg[r] = v; break;
    }
    update_irq(s);
}

static uint32_t swim3_read(void *dev, uint32_t offset, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | reg_read(dev, ((offset + i) >> ((mc_swim3 *)dev)->shift) & 15u);
    return v;
}

static void swim3_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    for (unsigned i = 0; i < size; i++) reg_write(dev, ((offset + i) >> ((mc_swim3 *)dev)->shift) & 15u, (uint8_t)(value >> (8 * (size - 1 - i))));
}

static void swim3_reset(void *dev) {
    mc_swim3 *s = dev;
    memset(s->reg, 0, sizeof s->reg);
    s->mode = s->intr = s->error = 0;
    /* The interrupt mask resets to all sources: the floppy driver's first
     * operations after an insertion wait for interrupts it has not
     * unmasked (Livadix's reading†). */
    s->reg[R_INTEN] = 0;
    s->op = OP_NONE;
    set_drq(s, 0);
    update_irq(s);
}

const mc_dev_ops mc_swim3_ops = {swim3_read, swim3_write, swim3_event, swim3_reset, NULL};

/* --- setup and media -------------------------------------------------------------- */

void mc_swim3_init(mc_swim3 *s, const mc_hw_host *host, const char *name, unsigned drives) {
    memset(s, 0, sizeof *s);
    s->dev.ops = &mc_swim3_ops;
    s->dev.host = host;
    s->dev.name = name;
    s->drives = drives > MC_SWIM3_DRIVES ? MC_SWIM3_DRIVES : drives;
    s->shift = 9;
    for (unsigned i = 0; i < MC_SWIM3_DRIVES; i++) s->drive[i].eject_latch = 0;
}

void mc_swim3_set_stride(mc_swim3 *s, unsigned shift) { s->shift = shift; }

int mc_swim3_insert(mc_swim3 *s, unsigned drive, mc_disk *disk, int write_protect, char *error, size_t capacity) {
    const char *problem = NULL;
    const mc_floppy_format f = disk ? mc_floppy_format_of(mc_disk_sectors(disk)) : MC_FLOPPY_NONE;
    if (drive >= s->drives) problem = "No such floppy drive";
    else if (!disk) problem = "No disk image";
    else if (mc_disk_sector_size(disk) != 512) problem = "Floppy images have 512-byte sectors";
    else if (mc_disk_sectors(disk) == 3360) problem = "1680 KiB floppy images are not supported";
    else if (f == MC_FLOPPY_NONE) problem = "Floppy images must hold 400, 720, 800 or 1440 KiB";
    if (problem) {
        if (capacity) snprintf(error, capacity, "%s", problem);
        return 0;
    }
    mc_swim3_drive *d = &s->drive[drive];
    d->disk = disk;
    d->format = f;
    d->protect = (uint8_t)(write_protect || mc_disk_readonly(disk));
    d->ejected = 0;
    d->eject_latch = 1;     /* a disk went in: the latch reads 1 until reset† */
    d->next_sector = 0;
    return 1;
}

mc_disk *mc_swim3_media(const mc_swim3 *s, unsigned drive, int *ejected) {
    if (drive >= s->drives) {
        if (ejected) *ejected = 0;
        return NULL;
    }
    if (ejected) *ejected = s->drive[drive].ejected;
    return s->drive[drive].disk;
}

mc_disk *mc_swim3_remove(mc_swim3 *s, unsigned drive) {
    if (drive >= s->drives) return NULL;
    mc_swim3_drive *d = &s->drive[drive];
    mc_disk *disk = d->disk;
    d->disk = NULL;
    d->format = MC_FLOPPY_NONE;
    d->ejected = 0;
    motor_set(s,drive,0);
    return disk;
}
