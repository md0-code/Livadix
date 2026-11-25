/* SWIM floppy disk controller and Apple 3.5-inch drives (swim.h).
 *
 * Sources:
 *   SWIM Chip User's Reference (Apple, rev. 1.5, January 1988) [SWIM]:
 *     "Using the IWM Hardware" (latches, state table), "IWM Register Set"
 *     (status, handshake, mode, the ISM switch), "ISM Register Set" (data,
 *     mark, CRC/IWM configuration, parameter RAM, phase, setup, mode/status,
 *     error, handshake), "IWM State/ISM Register Mapping".
 *   SWIM specification 343S0061-A (1988): two-byte FIFO, CRC preset before
 *     the mark byte.
 *   Apple 800K Double-Sided Drive ERS 699-0452-A (1986) [ERS]: 3.1.1 Table 1
 *     (status on the RD line by SEL, CA2, CA1, CA0), 3.1.2 and 3.2.3 (commands
 *     latched at the rising edge of LSTRB), 3.2.2 (/ENBL high presets the
 *     latches: /MOTORON high, /DIRTN zero), 3.2.4 (status meanings), 3.4
 *     (zone speeds).
 *   Inside Macintosh III, "The Disk Interface" [IM3]: step takes about
 *     12 ms, TACH 60 pulses a revolution, the drive registers.
 *   Apple SuperDrive register names (Apple's drive command and status codes
 *     sheet): SEL=0 CA2-0=101 MFM-capable drive, SEL=1 101 MFM mode on,
 *     SEL=1 111 low-density media, SEL=1 CA1-0=01 command GCR/MFM mode,
 *     SEL=1 00 command reset eject latch; TACH reads the index in MFM mode. */
#include "swim.h"

#include <stdio.h>
#include <string.h>

enum { L_MOTOR = 0x10, L_DRIVESEL = 0x20, L_L6 = 0x40, L_L7 = 0x80 };
/* ISM mode register [SWIM, "ISM Register Set", MODE]. */
enum { M_CLEAR = 0x01, M_DRIVE1 = 0x02, M_DRIVE2 = 0x04, M_ACTION = 0x08, M_WRITE = 0x10,
       M_HDSEL = 0x20, M_ISM = 0x40, M_MOTOR = 0x80 };
/* ISM error register [SWIM, ERROR]: bit 0 the processor is not keeping up
 * (write underrun, read overrun), bit 1 a mark byte read from the data
 * register, bit 2 the processor is faster than the FIFO. */
enum { E_SLOW = 0x01, E_MARK = 0x02, E_FAST = 0x04 };
enum { X_NONE, X_IWM_WRITE, X_ISM_READ, X_ISM_WRITE };
#define F_MARK 1
#define F_CRC 2
#define NO_STREAM UINT64_MAX

/* [IM3] a step completes in about 12 ms. */
#define STEP_TIME ((uint64_t)MC_C15M_HZ * 12u / 1000u)
/* /STEP goes high again once the drive has counted the step: 72 us at most
 * after the falling edge ([ERS] 3.2.4.2, 3.4.3.1 T2); the move itself shows
 * on /READY (3.4.3.3). */
#define STEP_ACK ((uint64_t)MC_C15M_HZ * 72u / 1000000u)
/* [SWIM] mode bit 2 clear: the enable stays active 2^23 FCLK periods after
 * MotorOn goes off. */
#define IWM_TIMER (1ull << 23)

static uint64_t now_of(mc_swim *s) { return mc_dev_now(&s->dev); }

/* ---- Drive and media ---- */

static int spinning(const mc_swim_drive *d) { return d->motor && d->inserted; }
static int drive_gcr(const mc_swim_drive *d) { return !d->mfm_mode; }
static int media_hd(const mc_swim_drive *d) { return d->inserted && d->format == MC_FLOPPY_MFM1440; }
static int superdrive(const mc_swim_drive *d) { return d->type == MC_DRIVE_SUPERDRIVE; }
/* Sides the drive has: a single-sided 400K drive has head 0 only. */
static unsigned drive_sides(const mc_swim_drive *d) { return d->type == MC_DRIVE_400K ? 1 : 2; }
static uint32_t drive_bps(const mc_swim_drive *d) { return mc_floppy_bytes_per_second(drive_gcr(d), media_hd(d)); }
static uint32_t track_len(const mc_swim_drive *d) { return mc_floppy_track_length(drive_gcr(d), media_hd(d), d->track); }
/* Bytes that have passed the head since time 0. */
static uint64_t abs_byte(const mc_swim_drive *d, uint64_t t) { return t * drive_bps(d) / MC_C15M_HZ; }
/* C15M periods per byte, rounded up. */
static uint64_t byte_time(const mc_swim_drive *d) { return (MC_C15M_HZ + drive_bps(d) - 1) / drive_bps(d); }

typedef struct commit_ctx { mc_swim_drive *d; int64_t first; } commit_ctx;
static void commit_sector(void *ctx, unsigned index, const uint8_t *data, const uint8_t *tags) {
    commit_ctx *c = ctx;
    uint64_t lba = (uint64_t)c->first + index;
    uint8_t old[512], old_tags[12];
    if (!mc_disk_read(c->d->disk, lba, 1, old) || memcmp(old, data, 512)) mc_disk_write(c->d->disk, lba, 1, data);
    if (tags && mc_disk_has_tags(c->d->disk) && (!mc_disk_read_tags(c->d->disk, lba, 1, old_tags) || memcmp(old_tags, tags, 12)))
        mc_disk_write_tags(c->d->disk, lba, 1, tags);
}

/* Decodes the cached track and writes the sectors that changed. */
static void commit(mc_swim_drive *d) {
    if (!d->dirty) return;
    d->dirty = 0;
    if (!d->cache_valid || d->cache_blank || !d->disk || d->protect) return;
    commit_ctx c = {d, mc_floppy_first_sector(d->format, d->cache_track, d->cache_side)};
    if (c.first < 0) return;
    mc_floppy_decode_track(d->format, d->cache_track, d->cache_side, &d->cache, commit_sector, &c);
}

static void invalidate(mc_swim_drive *d) { commit(d); d->cache_valid = 0; }

/* Makes the cache hold the track side under the head. */
static void ensure_track(mc_swim_drive *d, unsigned side) {
    int readable = d->inserted && d->format != MC_FLOPPY_NONE && mc_floppy_is_gcr(d->format) == drive_gcr(d) &&
                   side < mc_floppy_sides(d->format) && side < drive_sides(d);
    if (d->cache_valid && d->cache_track == d->track && d->cache_side == side && d->cache_blank == !readable) return;
    invalidate(d);
    d->cache_track = d->track; d->cache_side = (uint8_t)side; d->cache_blank = (uint8_t)!readable; d->cache_valid = 1;
    if (readable) {
        unsigned n = mc_floppy_sectors(d->format, d->track);
        int64_t first = mc_floppy_first_sector(d->format, d->track, side);
        uint8_t data[18 * 512], tags[12 * 12];
        if (first < 0 || !mc_disk_read(d->disk, (uint64_t)first, n, data)) memset(data, 0, sizeof data);
        int has_tags = mc_floppy_is_gcr(d->format) && mc_disk_has_tags(d->disk) && mc_disk_read_tags(d->disk, (uint64_t)first, n, tags);
        mc_floppy_build_track(d->format, d->track, side, data, has_tags ? tags : NULL, &d->cache);
    } else {
        /* No flux the drive's mode can read: no byte with its top bit set
         * and no mark. */
        d->cache.length = track_len(d);
        memset(d->cache.data, 0, d->cache.length);
        memset(d->cache.mark, 0, d->cache.length);
    }
}

static unsigned current_side(const mc_swim *s) {
    if (s->ism && (s->setup & 0x01)) return (s->ism_mode & M_HDSEL) ? 1 : 0;   /* [SWIM] Setup bit 0: HDSEL output */
    return s->headsel;
}

/* ---- Enables ---- */

static int iwm_enable_active(mc_swim *s) {
    return (s->latch & L_MOTOR) || (!(s->iwm_mode & 0x04) && now_of(s) < s->timer_end);
}

/* The drive whose /ENBL is active, or -1. */
static int selected(mc_swim *s) {
    int which;
    if (s->ism) {
        if (!(s->ism_mode & M_MOTOR)) return -1;
        which = (s->ism_mode & M_DRIVE1) ? 0 : (s->ism_mode & M_DRIVE2) ? 1 : -1;
    } else {
        which = iwm_enable_active(s) ? ((s->latch & L_DRIVESEL) ? 1 : 0) : -1;
    }
    if (which == 0 && s->upper) return -1;
    return which >= 0 && (unsigned)which < s->drives ? which : -1;
}

static void stop_xfer(mc_swim *s);

/* [ERS] 3.2.2: a drive whose /ENBL goes high presets its latches: motor off,
 * direction zero. */
static void motor_set(mc_swim *s,unsigned drive,int on) {
    mc_swim_drive *d=&s->drive[drive];
    if(d->motor!=(uint8_t)on)mc_dev_drive(&s->dev,drive,MC_DRIVE_MOTOR,now_of(s),(uint32_t)on,0);
    d->motor=(uint8_t)on;
}

static void sync_enable(mc_swim *s) {
    int sel = selected(s);
    if (s->xfer != X_NONE && s->xfer_drive >= 0 && s->xfer_drive != sel) stop_xfer(s);
    for (unsigned i = 0; i < s->drives; i++) {
        mc_swim_drive *d = &s->drive[i];
        if (d->enabled && (int)i != sel) { motor_set(s,i,0); d->dirtn = 0; }
        d->enabled = (int)i == sel;
    }
}

/* The drive's RD line: status selected by SEL, CA2, CA1, CA0 [ERS] Table 1;
 * a floating line reads 1. */
static int rd_line(mc_swim *s) {
    int sel = selected(s);
    if (sel < 0) return 1;
    mc_swim_drive *d = &s->drive[sel];
    uint64_t t = now_of(s);
    unsigned addr = (unsigned)s->headsel << 3 | ((s->phases >> 2) & 1) << 2 | ((s->phases >> 1) & 1) << 1 | (s->phases & 1);
    switch (addr) {
    case 0x0: return d->dirtn;                          /* /DIRTN */
    case 0x1: return t < d->step_ack ? 0 : 1;           /* /STEP: low until the drive took the step [ERS] 3.2.4.2 */
    case 0x2: return d->motor ? 0 : 1;                  /* /MOTORON */
    case 0x3: return d->eject_latch;                    /* EJECT latch */
    case 0x4: case 0xC: return 1;                       /* RDDATA head 0 / head 1 */
    case 0x5: return superdrive(d) ? 1 : 0;             /* MFM-capable drive: only the SuperDrive [Apple drive sheet] */
    case 0x6: return d->type == MC_DRIVE_400K ? 0 : 1;  /* /SINGLE SIDE: low for the 400K drive [ERS] 3.2.4.6 */
    case 0x7: return 0;                                 /* /DRVIN: drive connected */
    case 0x8: return d->inserted ? 0 : 1;               /* /CSTIN */
    case 0x9: return d->inserted && !d->protect ? 1 : 0;/* /WRTPRT: low when locked or empty */
    case 0xA: return d->track == 0 ? 0 : 1;             /* /TK0 */
    case 0xB: {                                         /* /TACH (GCR) or index (MFM) */
        if (!spinning(d)) return 1;
        if (s->speed && d->type == MC_DRIVE_400K) mc_swim_set_speed(s, s->speed(s->speed_ctx));
        if (d->pwm_speed && !d->mfm_mode) {
            /* The motor at the speed control's rate. */
            const uint64_t rev = 16ull * 60u * MC_C15M_HZ;
            const uint64_t phase = (d->rot_phase + (t - d->rot_time) * d->pwm_speed) % rev;
            return (int)((phase * 120u / rev) & 1u);
        }
        uint64_t len = track_len(d), b = t * drive_bps(d);
        uint64_t pos = (b / MC_C15M_HZ) % len, f = pos * MC_C15M_HZ + b % MC_C15M_HZ, whole = len * MC_C15M_HZ;
        if (d->mfm_mode) return f * 50 < whole ? 1 : 0;    /* index pulse: the first 2% of the turn */
        return (int)((f * 120 / whole) & 1);               /* 60 pulses a revolution [IM3] */
    }
    case 0xD: return d->mfm_mode;                       /* MFM mode on; GCR-only drives stay in GCR (0) */
    case 0xE: return d->inserted && d->motor && t >= d->step_done ? 0 : 1;   /* /READY */
    default:                                            /* REVISED (800K drive 1, 400K drive 0 [ERS] 3.2.4.13) / low-density media */
        if (!superdrive(d)) return d->type == MC_DRIVE_800K ? 1 : 0;
        return d->inserted && d->format == MC_FLOPPY_MFM1440 ? 0 : 1;
    }
}

/* [ERS] 3.1.2, 3.2.3: at the rising edge of LSTRB the level of CA2 goes into
 * the latch addressed by SEL, CA1, CA0. */
static void drive_command(mc_swim *s) {
    int sel = selected(s);
    if (sel < 0) return;
    mc_swim_drive *d = &s->drive[sel];
    unsigned value = (s->phases >> 2) & 1, addr = (unsigned)s->headsel << 2 | ((s->phases >> 1) & 1) << 1 | (s->phases & 1);
    switch (addr) {
    case 0: d->dirtn = (uint8_t)value; break;
    case 1:
        if (!value) {
            const unsigned before=d->track;
            unsigned to = d->dirtn ? (d->track ? d->track - 1u : 0u) : (d->track < MC_FLOPPY_TRACKS - 1 ? d->track + 1u : d->track);
            if (to != d->track) {
                if (s->xfer != X_NONE && s->xfer_drive == sel) stop_xfer(s);
                invalidate(d);
                d->track = (uint8_t)to;
            }
            mc_dev_drive(&s->dev,(unsigned)sel,MC_DRIVE_STEP,now_of(s),!d->dirtn,before==d->track);
            d->step_done = now_of(s) + STEP_TIME;
            d->step_ack = now_of(s) + STEP_ACK;
        }
        break;
    case 2: motor_set(s,(unsigned)sel,!value); break;
    case 3:
        if (value && d->inserted) {
            if (s->xfer != X_NONE && s->xfer_drive == sel) stop_xfer(s);
            invalidate(d);
            d->inserted = 0; d->ejected = 1; motor_set(s,(unsigned)sel,0);
        }
        break;
    case 4: if (value) d->eject_latch = 0; break;
    case 5:
        if (superdrive(d) && d->mfm_mode != !value) {
            if (s->xfer != X_NONE && s->xfer_drive == sel) stop_xfer(s);
            invalidate(d);
            d->mfm_mode = (uint8_t)!value;
        }
        break;
    default: break;
    }
}

void mc_swim_set_speed(mc_swim *s, uint32_t rpm16) {
    const uint64_t now = now_of(s), rev = 16ull * 60u * MC_C15M_HZ;
    for (unsigned i = 0; i < s->drives; i++) {
        mc_swim_drive *d = &s->drive[i];
        if (d->type != MC_DRIVE_400K || d->pwm_speed == rpm16) continue;
        d->rot_phase = (d->rot_phase + (now - d->rot_time) * d->pwm_speed) % rev;
        d->rot_time = now;
        d->pwm_speed = rpm16;
    }
}

void mc_swim_speed_source(mc_swim *s, uint32_t (*speed)(void *ctx), void *ctx) {
    s->speed = speed;
    s->speed_ctx = ctx;
}

static void set_phases(mc_swim *s, uint8_t phases) {
    uint8_t old = s->phases;
    s->phases = phases & 0x0F;
    if (!(old & 0x08) && (s->phases & 0x08)) drive_command(s);
}

/* ---- Writing ---- */

static void crc_byte(mc_swim *s, uint8_t value, int mark) {
    if (mark && !s->crc_prev_mark) s->crc = 0xFFFF;     /* preset before the first mark byte */
    s->crc = mc_mfm_crc(s->crc, &value, 1);
    s->crc_prev_mark = (uint8_t)(mark != 0);
}

/* One byte onto the track at the write position. A locked disk records
 * nothing (/WRTGATE has no effect, [ERS] 3.2.5). */
static void emit(mc_swim *s, uint8_t value, int mark) {
    if (s->xfer_drive < 0) return;
    mc_swim_drive *d = &s->drive[s->xfer_drive];
    if (!d->inserted || !d->motor || d->protect) return;
    ensure_track(d, s->xfer_side);
    d->cache.data[s->wr_pos] = value;
    d->cache.mark[s->wr_pos] = (uint8_t)(mark != 0);
    s->wr_pos = (s->wr_pos + 1) % d->cache.length;
    d->dirty = 1;
}

static void start_write(mc_swim *s) {
    int sel = selected(s);
    s->xfer_drive = sel >= 0 && spinning(&s->drive[sel]) ? sel : -1;
    s->xfer_side = (uint8_t)current_side(s);
    s->wr_pos = 0;
    if (s->xfer_drive >= 0) {
        mc_swim_drive *d = &s->drive[s->xfer_drive];
        ensure_track(d, s->xfer_side);
        s->wr_pos = (uint32_t)(abs_byte(d, now_of(s)) % d->cache.length);
    }
    s->crc = 0xFFFF; s->crc_prev_mark = 0;
}

static uint64_t xfer_byte_time(mc_swim *s) {
    return s->xfer_drive >= 0 ? byte_time(&s->drive[s->xfer_drive]) : 256u;
}

/* [SWIM] IWM handshake: the shift register takes the buffered byte when it
 * has sent the last one; an empty buffer then is an underrun. */
static void iwm_write_update(mc_swim *s) {
    uint64_t t = now_of(s), bt = xfer_byte_time(s);
    while (!s->wr_underrun && t >= s->wr_shift_end) {
        if (!s->wr_buffered) { s->wr_underrun = 1; break; }
        emit(s, s->wr_byte, 0);
        s->wr_buffered = 0;
        s->wr_shift_end += bt;
    }
}

static void ism_write_update(mc_swim *s);

static void stop_xfer(mc_swim *s) {
    uint8_t kind = s->xfer;
    if (kind == X_NONE) return;
    if (kind == X_IWM_WRITE) {
        iwm_write_update(s);
        if (s->wr_buffered && !s->wr_underrun) emit(s, s->wr_byte, 0);
        s->wr_buffered = 0; s->wr_underrun = 0;
    } else if (kind == X_ISM_WRITE) {
        ism_write_update(s);
    } else if (kind == X_ISM_READ && s->rd_frozen == NO_STREAM) {
        s->rd_frozen = s->xfer_drive >= 0 ? abs_byte(&s->drive[s->xfer_drive], now_of(s)) : 0;
    }
    s->xfer = X_NONE;
    if (s->xfer_drive >= 0) commit(&s->drive[s->xfer_drive]);
}

static void iwm_write_data(mc_swim *s, uint8_t value) {
    if (s->xfer != X_IWM_WRITE) {
        stop_xfer(s);
        s->xfer = X_IWM_WRITE;
        start_write(s);
        s->wr_underrun = 0; s->wr_buffered = 0;
        emit(s, value, 0);
        s->wr_shift_end = now_of(s) + xfer_byte_time(s);
        return;
    }
    iwm_write_update(s);
    if (s->wr_underrun) return;
    s->wr_byte = value;     /* the last byte written before the load is the one sent */
    s->wr_buffered = 1;
}

static void set_error(mc_swim *s, uint8_t bit) {
    if (!s->error) s->error = bit;      /* [SWIM] ERROR: once one bit is set no other is */
}

/* The ISM FIFO drains one byte per byte time; a CRC entry sends the two CRC
 * bytes; an empty FIFO when a byte is due is an underrun, which clears
 * ACTION [SWIM, MODE bit 3]. */
static void ism_write_update(mc_swim *s) {
    uint64_t t = now_of(s), bt = xfer_byte_time(s);
    while (s->xfer == X_ISM_WRITE && t >= s->wr_next) {
        if (!s->fifo_n) {
            set_error(s, E_SLOW);
            s->ism_mode &= (uint8_t)~M_ACTION;
            s->xfer = X_NONE;
            if (s->xfer_drive >= 0) commit(&s->drive[s->xfer_drive]);
            break;
        }
        uint8_t v = s->fifo[0], f = s->fifo_flags[0];
        s->fifo[0] = s->fifo[1]; s->fifo_flags[0] = s->fifo_flags[1]; s->fifo_n--;
        if (f & F_CRC) {
            uint16_t crc = s->crc;
            emit(s, (uint8_t)(crc >> 8), 0);
            emit(s, (uint8_t)crc, 0);
            s->crc_prev_mark = 0;
            s->wr_next += 2 * bt;
        } else {
            crc_byte(s, v, f & F_MARK);
            emit(s, v, f & F_MARK);
            s->wr_next += bt;
        }
    }
}

static void fifo_push(mc_swim *s, uint8_t value, uint8_t flags) {
    if (s->xfer == X_ISM_WRITE) ism_write_update(s);
    if (s->fifo_n >= 2) { set_error(s, E_FAST); return; }
    s->fifo[s->fifo_n] = value; s->fifo_flags[s->fifo_n] = flags; s->fifo_n++;
}

/* ---- ISM reading ---- */

static void start_read(mc_swim *s) {
    int sel = selected(s);
    s->xfer_drive = sel >= 0 && spinning(&s->drive[sel]) ? sel : -1;
    s->xfer_side = (uint8_t)current_side(s);
    s->rd_first = NO_STREAM; s->rd_taken = 0; s->rd_frozen = NO_STREAM;
    s->crc = 0xFFFF; s->crc_prev_mark = 0;
    if (s->xfer_drive < 0) return;
    mc_swim_drive *d = &s->drive[s->xfer_drive];
    ensure_track(d, s->xfer_side);
    uint64_t a0 = abs_byte(d, now_of(s));
    uint32_t len = d->cache.length, p0 = (uint32_t)(a0 % len);
    if (s->setup & 0x04) {              /* GCR mode: the bytes as they come */
        s->rd_first = a0 + 1; s->rd_pos0 = (p0 + 1) % len;
        return;
    }
    /* [SWIM] MODE note: the first byte returned is a mark byte; the search
     * is done by the chip. */
    for (uint32_t k = 1; k <= len; k++)
        if (d->cache.mark[(p0 + k) % len]) { s->rd_first = a0 + k; s->rd_pos0 = (p0 + k) % len; return; }
}

/* Bytes in the read FIFO (at most two; more is an overrun that loses the
 * oldest). */
static unsigned rd_count(mc_swim *s) {
    if (s->rd_first == NO_STREAM || s->xfer_drive < 0) return 0;
    uint64_t a = s->rd_frozen != NO_STREAM ? s->rd_frozen : abs_byte(&s->drive[s->xfer_drive], now_of(s));
    uint64_t produced = a >= s->rd_first ? a - s->rd_first + 1 : 0;
    if (produced - s->rd_taken > 2) { set_error(s, E_SLOW); s->rd_taken = produced - 2; }
    return (unsigned)(produced - s->rd_taken);
}

static void rd_head(mc_swim *s, uint8_t *value, uint8_t *mark) {
    mc_swim_drive *d = &s->drive[s->xfer_drive];
    ensure_track(d, s->xfer_side);
    uint32_t p = (uint32_t)((s->rd_pos0 + s->rd_taken) % d->cache.length);
    *value = d->cache.data[p]; *mark = d->cache.mark[p];
}

static uint8_t ism_read_fifo(mc_swim *s, int mark_register) {
    /* ACTION clear: the data register reads the CORRECTION bytes (no
     * correction here); bytes left in the FIFO read from MARK. */
    if (!(s->ism_mode & M_ACTION) && (!mark_register || s->rd_frozen == NO_STREAM)) return 0;
    if (s->ism_mode & M_WRITE) return 0;
    if (!rd_count(s)) { set_error(s, E_FAST); return 0; }
    uint8_t v, m;
    rd_head(s, &v, &m);
    if (m && !mark_register) set_error(s, E_MARK);
    crc_byte(s, v, m);
    s->rd_taken++;
    return v;
}

static uint8_t ism_handshake(mc_swim *s) {
    uint8_t h = 0;
    if (s->ism_mode & M_WRITE) {
        if (s->xfer == X_ISM_WRITE) ism_write_update(s);
        if (s->fifo_n < 2) h |= 0x80;
        if (s->fifo_n == 0) h |= 0x40;
        if (s->crc) h |= 0x02;
    } else {
        unsigned n = rd_count(s);
        if (n >= 1) h |= 0x80;
        if (n >= 2) h |= 0x40;
        uint16_t crc = s->crc;
        if (n) {
            uint8_t v, m;
            rd_head(s, &v, &m);
            if (m) h |= 0x01;
            if (m && !s->crc_prev_mark) crc = 0xFFFF;
            crc = mc_mfm_crc(crc, &v, 1);
        }
        if (crc) h |= 0x02;
    }
    int rd = rd_line(s);
    if (rd) h |= 0x04 | 0x08;           /* RDDATA and SENSE inputs: the drive's RD line */
    if (s->ism_mode & M_MOTOR) h |= 0x10;
    if (s->error) h |= 0x20;
    return h;
}

static void ism_set_mode(mc_swim *s, uint8_t mode) {
    uint8_t old = s->ism_mode;
    s->ism_mode = mode;
    if (mode & M_CLEAR) {               /* clear the FIFO and restart the CRC */
        s->fifo_n = 0;
        if (s->xfer == X_ISM_READ) s->rd_taken += rd_count(s);
        else { s->rd_first = NO_STREAM; s->rd_frozen = NO_STREAM; }
        s->crc = 0xFFFF; s->crc_prev_mark = 0;
    }
    if ((old & M_ACTION) && !(mode & M_ACTION)) stop_xfer(s);
    if (!(mode & M_ISM)) {              /* back to the IWM register set */
        stop_xfer(s);
        s->ism = 0;
    }
    sync_enable(s);
    if (s->ism && !(old & M_ACTION) && (mode & M_ACTION)) {
        stop_xfer(s);
        if (mode & M_WRITE) {
            s->xfer = X_ISM_WRITE;
            start_write(s);
            s->wr_next = now_of(s);
            ism_write_update(s);
        } else {
            s->xfer = X_ISM_READ;
            start_read(s);
        }
    }
}

static uint8_t ism_access(mc_swim *s, unsigned reg, int write, uint8_t value) {
    if (s->xfer == X_ISM_WRITE) ism_write_update(s);    /* bring the FIFO and ACTION up to now */
    if (write) {
        switch (reg) {
        case 0: fifo_push(s, value, 0); break;
        case 1: fifo_push(s, value, F_MARK); break;
        case 2: if (s->ism_mode & M_ACTION) fifo_push(s, 0, F_CRC); else s->iwm_config = value; break;
        case 3: s->param[s->param_index++ & 15] = value; break;
        case 4: {                       /* PHASE: bits 4-7 directions, 0-3 levels */
            s->phase_dir = (uint8_t)(value >> 4);
            uint8_t levels = (uint8_t)((s->phases & ~s->phase_dir) | (value & s->phase_dir));
            set_phases(s, levels);
            break;
        }
        case 5: s->setup = value; break;
        case 6: s->param_index = 0; ism_set_mode(s, (uint8_t)(s->ism_mode & ~value)); break;
        case 7: ism_set_mode(s, (uint8_t)(s->ism_mode | value)); break;
        default: break;
        }
        return 0;
    }
    switch (reg) {
    case 8: return ism_read_fifo(s, 0);
    case 9: return ism_read_fifo(s, 1);
    case 10: { uint8_t e = s->error; s->error = 0; return e; }
    case 11: {
        /* Bytes $C (LATE/NORMAL) and $E (EARLY/NORMAL) share one NORMAL
         * write-precompensation value (SWIM Chip User's Reference,
         * "Parameter RAM"), held in byte $C: byte $E reads its own EARLY
         * with $C's NORMAL (the Quadra 700 ROM's ISM test expects $EC from
         * $E after writing $CC and $EE there, table at ROM offset $48662). */
        const unsigned i = s->param_index++ & 15;
        return i == 14 ? (uint8_t)((s->param[14] & 0xF0) | (s->param[12] & 0x0F)) : s->param[i];
    }
    case 12: return (uint8_t)(s->phase_dir << 4 | s->phases);
    case 13: return s->setup;
    case 14: return s->ism_mode;
    case 15: return ism_handshake(s);
    default: return 0xFF;
    }
}

/* ---- IWM ---- */

static uint8_t iwm_read_data(mc_swim *s) {
    int sel = selected(s);
    if (sel < 0 || !spinning(&s->drive[sel])) return 0;
    mc_swim_drive *d = &s->drive[sel];
    ensure_track(d, current_side(s));
    uint64_t a = abs_byte(d, now_of(s));
    /* Asynchronous mode: a byte read is cleared from the register; the next
     * one appears when the disk delivers it. */
    if (d->last_read == a + 1) return 0;
    d->last_read = a + 1;
    return d->cache.data[a % d->cache.length];
}

static uint8_t iwm_access(mc_swim *s, unsigned reg, int write, uint8_t value) {
    unsigned latch = reg >> 1, on = reg & 1;
    if (latch < 4) {
        set_phases(s, (uint8_t)(on ? (s->phases | (1u << latch)) : (s->phases & ~(1u << latch))));
    } else {
        uint8_t bit = (uint8_t)(1u << latch), before = s->latch;
        s->latch = on ? (uint8_t)(s->latch | bit) : (uint8_t)(s->latch & ~bit);
        if (bit == L_MOTOR && (before & L_MOTOR) && !on) s->timer_end = now_of(s) + IWM_TIMER;
        if (bit == L_DRIVESEL && (s->iwm_config & 0x80) && !(s->latch & L_MOTOR)) s->timer_end = 0;   /* [SWIM] IWM config bit 7 */
    }
    sync_enable(s);
    int motor = iwm_enable_active(s), l6 = (s->latch & L_L6) != 0, l7 = (s->latch & L_L7) != 0;
    if (s->xfer == X_IWM_WRITE && (!l7 || !motor)) stop_xfer(s);
    if (write) {
        if (on && l7 && l6) {
            if (!motor) {               /* Set Mode [110] */
                s->iwm_mode = value;
                s->mode_seq = (uint8_t)(((s->mode_seq << 1) | ((value >> 6) & 1)) & 0x0F);
                if (s->mode_count < 4) s->mode_count++;
                if (s->chip != MC_CHIP_IWM && s->mode_count >= 4 && s->mode_seq == 0x0B) {   /* bit 6 = 1, 0, 1, 1: the IWM has no ISM */
                    s->ism = 1;
                    s->ism_mode |= M_ISM;
                    s->mode_seq = 0; s->mode_count = 0;
                    sync_enable(s);
                }
            } else {                    /* Write Data [111] */
                iwm_write_data(s, value);
            }
        }
        return 0;
    }
    if (!l7 && !l6) return motor ? iwm_read_data(s) : 0xFF;      /* Read Data [001] / All Ones [000] */
    if (!l7 && l6)                                               /* Read Status [01x] */
        return (uint8_t)((s->iwm_mode & 0x1F) | (motor ? 0x20 : 0) | (rd_line(s) ? 0x80 : 0));
    if (l7 && !l6) {                                             /* Read Write-Handshake [10x] */
        if (s->xfer == X_IWM_WRITE) iwm_write_update(s);
        uint8_t h = 0x3F;
        if (!(s->xfer == X_IWM_WRITE && s->wr_buffered)) h |= 0x80;
        if (!s->wr_underrun) h |= 0x40;
        return h;
    }
    return 0xFF;
}

/* ---- Interface ---- */

uint8_t mc_swim_access(mc_swim *s, unsigned reg, int write, uint8_t value) {
    reg &= 15;
    return s->ism ? ism_access(s, reg, write, value) : iwm_access(s, reg, write, value);
}

void mc_swim_set_headsel(mc_swim *s, int level) { s->headsel = level ? 1 : 0; }
void mc_swim_set_upper(mc_swim *s, int upper) {
    s->upper = upper ? 1 : 0;
    sync_enable(s);
}

static uint32_t swim_read(void *dev, uint32_t offset, unsigned size) {
    (void)size;
    return mc_swim_access(dev, (offset >> 9) & 15, 0, 0);
}
static void swim_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)size;
    mc_swim_access(dev, (offset >> 9) & 15, 1, (uint8_t)value);
}
static void swim_event(void *dev, uint64_t now) { (void)dev; (void)now; }

/* [SWIM] reset: IWM selected, latches and mode cleared; ISM mode, setup and
 * error zero, phases outputs at zero ($F0). Media and head positions stay. */
static void swim_reset(void *dev) {
    mc_swim *s = dev;
    stop_xfer(s);
    s->ism = 0; s->phases = 0; s->latch = 0; s->iwm_mode = 0; s->mode_seq = 0; s->mode_count = 0; s->timer_end = 0;
    s->ism_mode = 0; s->setup = 0; s->phase_dir = 0x0F; s->error = 0; s->param_index = 0; s->iwm_config = 0;
    s->fifo_n = 0; s->rd_first = NO_STREAM; s->rd_frozen = NO_STREAM; s->rd_taken = 0;
    s->wr_buffered = 0; s->wr_underrun = 0;
    for (unsigned i = 0; i < MC_SWIM_DRIVES; i++) {
        mc_swim_drive *d = &s->drive[i];
        commit(d);
        motor_set(s,i,0); d->dirtn = 0; d->enabled = 0; d->step_done = 0; d->mfm_mode = 0; d->eject_latch = 0;
    }
}

const mc_dev_ops mc_swim_ops = {swim_read, swim_write, swim_event, swim_reset};

void mc_swim_init(mc_swim *s, const mc_hw_host *host, unsigned drives) {
    memset(s, 0, sizeof *s);
    s->dev.ops = &mc_swim_ops;
    s->dev.host = host;
    s->dev.name = "SWIM";
    s->drives = drives > MC_SWIM_DRIVES ? MC_SWIM_DRIVES : drives;
    s->xfer_drive = -1;
    for (unsigned i = 0; i < s->drives; i++) s->drive[i].fitted = 1;
    swim_reset(s);
}

void mc_swim2_init(mc_swim *s, const mc_hw_host *host, unsigned drives) {
    mc_swim_init(s, host, drives);
    s->chip = MC_CHIP_SWIM2;
    s->dev.name = "SWIM2";
}

void mc_iwm_init(mc_swim *s, const mc_hw_host *host, unsigned drives, mc_drive_type type) {
    mc_swim_init(s, host, drives);
    s->chip = MC_CHIP_IWM;
    s->dev.name = "IWM";
    for (unsigned i = 0; i < MC_SWIM_DRIVES; i++) s->drive[i].type = type == MC_DRIVE_400K ? MC_DRIVE_400K : MC_DRIVE_800K;
}

int mc_swim_insert(mc_swim *s, unsigned drive, mc_disk *disk, int write_protect, char *error, size_t capacity) {
    const char *problem = NULL;
    mc_floppy_format f = disk ? mc_floppy_format_of(mc_disk_sectors(disk)) : MC_FLOPPY_NONE;
    if (drive >= s->drives) problem = "No such floppy drive";
    else if (!disk) problem = "No disk image";
    else if (mc_disk_sector_size(disk) != 512) problem = "Floppy images have 512-byte sectors";
    else if (mc_disk_sectors(disk) == 3360) problem = "1680 KiB floppy images are not supported";
    else if (f != MC_FLOPPY_NONE && !mc_floppy_is_gcr(f) && !superdrive(&s->drive[drive])) problem = "This drive reads GCR 400 and 800 KiB disks only";
    else if (f == MC_FLOPPY_NONE) problem = "Floppy images must hold 400, 720, 800 or 1440 KiB";
    if (problem) {
        if (capacity) snprintf(error, capacity, "%s", problem);
        return 0;
    }
    mc_swim_drive *d = &s->drive[drive];
    if (s->xfer != X_NONE && s->xfer_drive == (int)drive) stop_xfer(s);
    invalidate(d);
    d->disk = disk; d->format = f; d->inserted = 1; d->ejected = 0;
    d->protect = (uint8_t)(write_protect || mc_disk_readonly(disk));
    d->last_read = 0;
    return 1;
}

mc_disk *mc_swim_media(const mc_swim *s, unsigned drive, int *ejected) {
    if (drive >= s->drives) { if (ejected) *ejected = 0; return NULL; }
    if (ejected) *ejected = s->drive[drive].ejected;
    return s->drive[drive].disk;
}

mc_disk *mc_swim_remove(mc_swim *s, unsigned drive) {
    if (drive >= s->drives) return NULL;
    mc_swim_drive *d = &s->drive[drive];
    if (s->xfer != X_NONE && s->xfer_drive == (int)drive) stop_xfer(s);
    invalidate(d);
    mc_disk *disk = d->disk;
    motor_set(s,drive,0);
    d->disk = NULL; d->format = MC_FLOPPY_NONE; d->inserted = 0; d->ejected = 0;
    return disk;
}

void mc_swim_flush(mc_swim *s) {
    if (s->xfer == X_IWM_WRITE) iwm_write_update(s);
    else if (s->xfer == X_ISM_WRITE) ism_write_update(s);
    for (unsigned i = 0; i < s->drives; i++) commit(&s->drive[i]);
}
