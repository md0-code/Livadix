/* Livadix — New Age (see newage.h). Livadix's own code. */
#include "newage.h"

#include <stdio.h>
#include <string.h>

#define REG_MSR 0x101u          /* read: main status; write: data-rate select */
#define REG_FIFO 0x141u

/* Main status register (82077AA data sheet, "Main Status Register"). */
#define MSR_RQM 0x80u           /* ready for a byte */
#define MSR_DIO 0x40u           /* the byte goes to the processor */
#define MSR_CB 0x10u            /* command in progress */

/* Status registers 0 and 1 (82077AA, "Status Register 0", "Status Register 1"). */
#define ST0_INVALID 0x80u
#define ST0_ABNORMAL 0x40u
#define ST0_SEEK_END 0x20u
#define ST0_NOT_READY 0x08u
#define ST0_CHANGE 0xC0u        /* Apple: a disk inserted or (bit 1) removed */
#define ST1_NOT_WRITABLE 0x02u
#define ST1_NO_DATA 0x04u
#define ST1_MISSING_AM 0x01u

/* A motion or data command's time: 1 ms of guest time (C15M periods). */
#define EXEC_TIME 15667u

static void update_irq(mc_newage *n);

static void set_drq(mc_newage *n, int on) {
    if (n->drq == on) return;
    n->drq = (uint8_t)on;
    mc_dev_line(&n->dev, MC_NEWAGE_LINE_DRQ, on);
}

static int idle(const mc_newage *n) { return !n->cmd_n && n->res_i >= n->res_n && !n->busy && !n->seeking; }

static int change_pending(const mc_newage *n) {
    for (unsigned d = 0; d < n->drives; d++)
        if (n->drive[d].change) return 1;
    return 0;
}

static void update_irq(mc_newage *n) {
    const int on = n->result_irq || n->int_pending || (change_pending(n) && idle(n));
    if (n->irq == on) return;
    n->irq = (uint8_t)on;
    mc_dev_line(&n->dev, MC_NEWAGE_LINE_IRQ, on);
}

static void reset(mc_newage *n) {
    n->cmd_n = n->cmd_len = 0;
    n->res_n = n->res_i = 0;
    n->int_pending = 0;
    n->result_irq = 0;
    n->busy = n->seeking = 0;
    n->xfer = 0;
    n->buf_n = n->buf_i = 0;
    set_drq(n, 0);
    mc_dev_schedule(&n->dev, MC_NEVER);
    update_irq(n);
}

/* The bytes a command takes, its first included (82077AA, "Command Set");
 * Apple's commands take their drive byte. */
static unsigned command_length(uint8_t c) {
    switch (c & 0x1Fu) {
    case 0x02: case 0x05: case 0x06: case 0x09: case 0x0C: case 0x11: case 0x16: case 0x19: case 0x1D: return 9;
    case 0x03: return 3;
    case 0x04: case 0x07: case 0x0A: case 0x12: case 0x1A: case 0x1B: case 0x1C: return 2;
    case 0x08: case 0x0E: case 0x10: case 0x14: case 0x18: return 1;
    case 0x0D: return 6;
    case 0x0F: return 3;
    case 0x13: return 4;
    default: return 2;
    }
}

static void result(mc_newage *n, const uint8_t *bytes, unsigned count) {
    memcpy(n->res, bytes, count);
    n->res_n = count;
    n->res_i = 0;
}

/* The disk a drive can read now (NULL: not fitted, empty or ejected). */
static mc_newage_drive *loaded(mc_newage *n, unsigned d) {
    if (d >= n->drives) return NULL;
    mc_newage_drive *v = &n->drive[d];
    return v->disk && !v->ejected ? v : NULL;
}

/* A data command's end: seven result bytes and the interrupt. */
static void end_data(mc_newage *n, uint8_t st0, uint8_t st1, uint8_t c, uint8_t h, uint8_t r, uint8_t sz) {
    const uint8_t res[7] = {st0, st1, 0, c, h, r, sz};
    n->busy = 0;
    n->xfer = 0;
    set_drq(n, 0);
    result(n, res, 7);
    n->result_irq = 1;
    update_irq(n);
}

/* A motion command's end: the status SENSE INTERRUPT STATUS reports. */
static void end_motion(mc_newage *n, uint8_t st0, uint8_t pcn) {
    n->seeking = 0;
    n->st0 = st0;
    n->pcn = pcn;
    n->int_pending = 1;
    update_irq(n);
}

/* The image sector of track side sector `r` as the command numbers it
 * (GCR from 0, MFM from 1); -1 when the disk has none. */
static int64_t sector_of(const mc_newage_drive *v, unsigned c, unsigned h, unsigned r) {
    const int gcr = mc_floppy_is_gcr(v->format);
    const unsigned first = gcr ? 0 : 1, count = mc_floppy_sectors(v->format, c);
    const int64_t base = mc_floppy_first_sector(v->format, c, h);
    if (base < 0 || r < first || r - first >= count) return -1;
    return base + (r - first);
}

/* Starts a data command's transfer (or ends it at once on an error). */
static void start_data(mc_newage *n) {
    const uint8_t *k = n->exec, op = (uint8_t)(k[0] & 0x1Fu);
    const unsigned d = k[1] & 1u, h = (k[1] >> 2) & 1u;
    const int mfm = (k[0] & 0x40u) != 0;
    mc_newage_drive *v = loaded(n, d);
    const uint8_t st0 = (uint8_t)((h << 2) | d);
    if (!v) { end_data(n, (uint8_t)(st0 | ST0_ABNORMAL | ST0_NOT_READY), 0, 0, 0, 0, 0); return; }
    const int gcr = mc_floppy_is_gcr(v->format);
    if (mfm == gcr || h >= mc_floppy_sides(v->format)) {
        end_data(n, (uint8_t)(st0 | ST0_ABNORMAL), ST1_MISSING_AM, v->pcn, (uint8_t)h, 0, 0);
        return;
    }
    if (op == 0x0A) {           /* READ ID: the sector coming round */
        const unsigned count = mc_floppy_sectors(v->format, v->pcn);
        const unsigned r = count ? v->turn++ % count : 0;
        const uint8_t sz = gcr ? (uint8_t)(v->format == MC_FLOPPY_GCR800 ? 0x22 : 0x02) : 2;
        end_data(n, st0, 0, v->pcn, (uint8_t)h, (uint8_t)(r + (gcr ? 0 : 1)), sz);
        return;
    }
    if ((op == 0x05 || op == 0x0D) && v->protect) {
        end_data(n, (uint8_t)(st0 | ST0_ABNORMAL), ST1_NOT_WRITABLE, v->pcn, (uint8_t)h, 0, 0);
        return;
    }
    if (op == 0x0D) {           /* FORMAT: four ID bytes a sector come first */
        n->buf_n = 4u * k[3];
        if (n->buf_n > sizeof n->buf) n->buf_n = sizeof n->buf;
        n->buf_i = 0;
        n->xfer = 3;
        set_drq(n, 1);
        return;
    }
    /* READ DATA, WRITE DATA: sectors R to EOT of cylinder C, head H. */
    const unsigned c = k[2], r = k[4], eot = k[6];
    const int64_t first = sector_of(v, c, h, r), last = sector_of(v, c, h, eot);
    if (first < 0 || last < first) {
        end_data(n, (uint8_t)(st0 | ST0_ABNORMAL), ST1_NO_DATA, (uint8_t)c, (uint8_t)h, (uint8_t)r, k[5]);
        return;
    }
    n->buf_n = (uint32_t)(last - first + 1) * 512u;
    n->buf_i = 0;
    if (op == 0x06) {
        if (!mc_disk_read(v->disk, (uint64_t)first, (uint32_t)(last - first + 1), n->buf)) {
            end_data(n, (uint8_t)(st0 | ST0_ABNORMAL), ST1_NO_DATA, (uint8_t)c, (uint8_t)h, (uint8_t)r, k[5]);
            return;
        }
        n->xfer = 1;
    } else {
        n->xfer = 2;
    }
    set_drq(n, 1);
}

/* The transfer moved its last byte. */
static void finish_data(mc_newage *n) {
    const uint8_t *k = n->exec, op = (uint8_t)(k[0] & 0x1Fu);
    const unsigned d = k[1] & 1u, h = (k[1] >> 2) & 1u;
    const uint8_t st0 = (uint8_t)((h << 2) | d);
    mc_newage_drive *v = loaded(n, d);
    if (!v) { end_data(n, (uint8_t)(st0 | ST0_ABNORMAL | ST0_NOT_READY), 0, 0, 0, 0, 0); return; }
    if (op == 0x0D) {           /* the track side filled with the filler byte */
        const unsigned count = mc_floppy_sectors(v->format, v->pcn);
        const int64_t base = mc_floppy_first_sector(v->format, v->pcn, h);
        uint8_t fill[512];
        memset(fill, k[5], sizeof fill);
        for (unsigned i = 0; base >= 0 && i < count; i++) mc_disk_write(v->disk, (uint64_t)base + i, 1, fill);
        end_data(n, st0, 0, v->pcn, (uint8_t)h, 0, k[2]);
        return;
    }
    if (op == 0x05) {
        const int64_t first = sector_of(v, k[2], h, k[4]);
        if (first < 0 || !mc_disk_write(v->disk, (uint64_t)first, n->buf_n / 512u, n->buf)) {
            end_data(n, (uint8_t)(st0 | ST0_ABNORMAL), ST1_NO_DATA, k[2], (uint8_t)h, k[4], k[5]);
            return;
        }
    }
    end_data(n, st0, 0, k[2], (uint8_t)h, (uint8_t)(k[6] + 1u), k[5]);
}

int mc_newage_dma_read(void *dev, uint8_t *byte) {
    mc_newage *n = dev;
    if (n->xfer != 1 || n->buf_i >= n->buf_n) return 0;
    *byte = n->buf[n->buf_i++];
    if (n->buf_i == n->buf_n) finish_data(n);
    return 1;
}

int mc_newage_dma_write(void *dev, uint8_t byte) {
    mc_newage *n = dev;
    if ((n->xfer != 2 && n->xfer != 3) || n->buf_i >= n->buf_n) return 0;
    n->buf[n->buf_i++] = byte;
    if (n->buf_i == n->buf_n) finish_data(n);
    return 1;
}

static void motor_set(mc_newage *n,unsigned drive,int on) {
    mc_newage_drive *d=&n->drive[drive];
    if(d->motor!=(uint8_t)on)mc_dev_drive(&n->dev,drive,MC_DRIVE_MOTOR,mc_dev_now(&n->dev),(uint32_t)on,0);
    d->motor=(uint8_t)on;
}

/* The command's time is up. */
static void newage_event(void *dev, uint64_t now) {
    mc_newage *n = dev;
    (void)now;
    const uint8_t *k = n->exec, d = (uint8_t)(k[1] & 1u);
    mc_newage_drive *v = d < n->drives ? &n->drive[d] : NULL;
    switch (k[0] & 0x1Fu) {
    case 0x07: case 0x0F:       /* RECALIBRATE, SEEK */
        if (!v) { end_motion(n, (uint8_t)(ST0_ABNORMAL | ST0_SEEK_END | ST0_NOT_READY | d), 0); break; }
        const unsigned before=v->pcn;
        v->pcn = (k[0] & 0x1Fu) == 0x07 ? 0 : (uint8_t)(k[2] < MC_FLOPPY_TRACKS ? k[2] : MC_FLOPPY_TRACKS - 1);
        if(v->pcn!=before)mc_dev_drive(&n->dev,d,MC_DRIVE_STEP,now,v->pcn>before,0);
        end_motion(n, (uint8_t)(ST0_SEEK_END | d), v->pcn);
        break;
    case 0x12:                  /* $52: eject */
        if (v && v->disk) v->ejected = 1;
        if (v) motor_set(n,d,0);
        end_motion(n, d, v ? v->pcn : 0);
        break;
    case 0x1A:                  /* $9A: motor on */
        if (v) motor_set(n,d,1);
        end_motion(n, d, v ? v->pcn : 0);
        break;
    case 0x1B:                  /* $9B */
        if (v) motor_set(n,d,0);
        end_motion(n, d, v ? v->pcn : 0);
        break;
    case 0x1C:                  /* $1C, $5C: GCR or MFM mode */
        end_motion(n, d, v ? v->pcn : 0);
        break;
    default:
        start_data(n);
        break;
    }
}

static void execute(mc_newage *n) {
    const uint8_t c = n->cmd[0], drive = n->cmd_len > 1 ? (uint8_t)(n->cmd[1] & 3u) : 0;
    switch (c & 0x1Fu) {
    case 0x04: {            /* SENSE DRIVE STATUS */
        uint8_t st3 = 0xFF;
        if ((drive & 1u) < n->drives && drive < 2) {
            const mc_newage_drive *v = &n->drive[drive];
            st3 = (uint8_t)(0x2Cu | drive | (v->pcn ? 0 : 0x10u));
            if (v->disk && !v->ejected) {
                if (!v->protect) st3 |= 0x40u;
                st3 |= v->format == MC_FLOPPY_MFM1440 ? 0x02u : 0x80u;
            }
        }
        result(n, &st3, 1);
        break;
    }
    case 0x08:              /* SENSE INTERRUPT STATUS */
        if (n->int_pending) {
            const uint8_t r[2] = {n->st0, n->pcn};
            n->int_pending = 0;
            result(n, r, (n->st0 & ST0_SEEK_END) ? 2 : 1);
        } else {
            uint8_t r = ST0_INVALID;
            for (unsigned d = 0; d < n->drives && !n->seeking; d++)
                if (n->drive[d].change) {
                    r = (uint8_t)(ST0_CHANGE | d | (n->drive[d].change == 2 ? 2u : 0));
                    n->drive[d].change = 0;
                    break;
                }
            result(n, &r, 1);
        }
        break;
    case 0x10: {            /* VERSION */
        const uint8_t r = 0x90;
        result(n, &r, 1);
        break;
    }
    case 0x07: case 0x0F:   /* RECALIBRATE, SEEK */
        memcpy(n->exec, n->cmd, sizeof n->exec);
        n->seeking = 1;
        mc_dev_schedule(&n->dev, mc_dev_now(&n->dev) + EXEC_TIME);
        break;
    case 0x1A: case 0x1B: case 0x12:
        if (c & 0x80u || ((c & 0x1Fu) == 0x12 && (c & 0x40u))) {   /* $9A, $9B, $52: an interrupt */
            memcpy(n->exec, n->cmd, sizeof n->exec);
            n->seeking = 1;
            mc_dev_schedule(&n->dev, mc_dev_now(&n->dev) + EXEC_TIME);
        } else if (c == 0x1A && drive < n->drives) {
            motor_set(n,drive,0);
        }
        break;                  /* $1A, $1B, $12, $32: no result */
    case 0x1C:                  /* $1C, $5C: an interrupt */
        memcpy(n->exec, n->cmd, sizeof n->exec);
        n->seeking = 1;
        mc_dev_schedule(&n->dev, mc_dev_now(&n->dev) + EXEC_TIME);
        break;
    case 0x05: case 0x06: case 0x0A: case 0x0D:
        memcpy(n->exec, n->cmd, sizeof n->exec);
        n->busy = 1;
        mc_dev_schedule(&n->dev, mc_dev_now(&n->dev) + EXEC_TIME);
        break;
    case 0x02: case 0x09: case 0x0C: case 0x11: case 0x16: case 0x19: case 0x1D: {
        const uint8_t r[7] = {(uint8_t)(ST0_ABNORMAL | (drive & 3u)), ST1_MISSING_AM, 0, 0, 0, 0, 2};
        result(n, r, 7);
        n->result_irq = 1;
        break;
    }
    case 0x03: case 0x13:
        break;              /* no result phase */
    default:
        if (command_length(c) == 2) break;    /* Apple's drive commands† */
        {
            const uint8_t r = ST0_INVALID;
            result(n, &r, 1);
        }
        break;
    }
    update_irq(n);
}

static uint8_t status(const mc_newage *n) {
    if (n->res_i < n->res_n) return MSR_RQM | MSR_DIO | MSR_CB;
    if (n->busy) return MSR_CB;
    return (uint8_t)(MSR_RQM | (n->cmd_n ? MSR_CB : 0) | (n->seeking ? 1u << (n->exec[1] & 1u) : 0));
}

static uint32_t newage_read(void *dev, uint32_t offset, unsigned size) {
    mc_newage *n = dev;
    switch (offset % MC_NEWAGE_SIZE) {
    case REG_MSR: return status(n);
    case REG_FIFO:
        if (n->res_i < n->res_n) {
            const uint8_t v = n->res[n->res_i++];
            n->result_irq = 0;
            if (n->res_i == n->res_n) n->res_i = n->res_n = 0;
            update_irq(n);
            return v;
        }
        return 0;
    default:
        /* $080-$0FF: the serial DMA status the DMA serial driver reads
         * (newage.h): bit 30 of the longword at $090, done. */
        if ((offset % MC_NEWAGE_SIZE) >= 0x80u && (offset % MC_NEWAGE_SIZE) < 0x100u)
            return size == 4 ? 0x40000000u : (size == 2 && !(offset & 2u)) ? 0x4000u : ((offset & 3u) == 0 && size == 1) ? 0x40u : 0;
        return 0;
    }
}

static void newage_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_newage *n = dev;
    (void)size;
    switch (offset % MC_NEWAGE_SIZE) {
    case REG_MSR:
        n->dsr = (uint8_t)(value & 0x7Fu);
        if (value & 0x80u) reset(n);
        break;
    case REG_FIFO:
        if (n->busy) break;                 /* executing: not taking bytes */
        n->res_i = n->res_n = 0;            /* a new command drops what was not read */
        if (!n->cmd_n) n->cmd_len = command_length((uint8_t)value);
        n->cmd[n->cmd_n++] = (uint8_t)value;
        if (n->cmd_n >= n->cmd_len) {
            n->cmd_n = 0;
            execute(n);
        } else {
            update_irq(n);
        }
        break;
    default: break;
    }
}

static void newage_reset(void *dev) {
    mc_newage *n = dev;
    reset(n);
    for (unsigned d = 0; d < MC_NEWAGE_DRIVES; d++) {
        n->drive[d].pcn = 0;
        motor_set(n,d,0);
        if (n->drive[d].disk && !n->drive[d].ejected) n->drive[d].change = 1;
    }
    update_irq(n);
}

const mc_dev_ops mc_newage_ops = {newage_read, newage_write, newage_event, newage_reset};

void mc_newage_init(mc_newage *n, const mc_hw_host *host, const char *name) {
    memset(n, 0, sizeof *n);
    n->dev.ops = &mc_newage_ops;
    n->dev.host = host;
    n->dev.name = name;
}

void mc_newage_fit(mc_newage *n, unsigned drives) {
    n->drives = (uint8_t)(drives > MC_NEWAGE_DRIVES ? MC_NEWAGE_DRIVES : drives);
}

int mc_newage_insert(mc_newage *n, unsigned drive, mc_disk *disk, int write_protect, char *error, size_t capacity) {
    const char *problem = NULL;
    const mc_floppy_format f = disk ? mc_floppy_format_of(mc_disk_sectors(disk)) : MC_FLOPPY_NONE;
    if (drive >= n->drives) problem = "No such floppy drive";
    else if (!disk) problem = "No disk image";
    else if (mc_disk_sector_size(disk) != 512) problem = "Floppy images have 512-byte sectors";
    else if (mc_disk_sectors(disk) == 3360) problem = "1680 KiB floppy images are not supported";
    else if (f == MC_FLOPPY_NONE) problem = "Floppy images must hold 400, 720, 800 or 1440 KiB";
    if (problem) {
        if (capacity) snprintf(error, capacity, "%s", problem);
        return 0;
    }
    mc_newage_drive *v = &n->drive[drive];
    v->disk = disk;
    v->format = f;
    v->protect = (uint8_t)(write_protect || mc_disk_readonly(disk));
    v->ejected = 0;
    v->change = 1;
    v->turn = 0;
    update_irq(n);
    return 1;
}

mc_disk *mc_newage_media(const mc_newage *n, unsigned drive, int *ejected) {
    if (drive >= n->drives) { if (ejected) *ejected = 0; return NULL; }
    if (ejected) *ejected = n->drive[drive].ejected;
    return n->drive[drive].disk;
}

mc_disk *mc_newage_remove(mc_newage *n, unsigned drive) {
    if (drive >= n->drives) return NULL;
    mc_newage_drive *v = &n->drive[drive];
    mc_disk *disk = v->disk;
    if (disk && !v->ejected) v->change = 2;
    v->disk = NULL;
    v->format = MC_FLOPPY_NONE;
    v->ejected = 0;
    motor_set(n,drive,0);
    if (n->xfer && (n->exec[1] & 1u) == drive) end_data(n, (uint8_t)(ST0_ABNORMAL | ST0_NOT_READY | drive), 0, 0, 0, 0, 0);
    update_irq(n);
    return disk;
}
