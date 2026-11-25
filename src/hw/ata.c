/* Livadix — the ATA controller and its devices (ata.h). Written from ATA-2
 * (ANSI X3.279-1996) and the ATA-3 draft (X3T13/2008D†), ATAPI from SFF-8020i
 * (ATAPI CD-ROM, Rev 1.2), the ATA Manager's view from Apple's ATA Device
 * Software Guide. Section numbers cite ATA-2. */
#include "ata.h"

#include <stdio.h>
#include <string.h>

/* Phases of the command in progress. */
enum {
    P_IDLE,
    P_CMD,          /* BSY: the command is accepted, its action is due */
    P_RD_LOAD,      /* BSY: the next block of a read is being fetched */
    P_RD_DRQ,       /* DRQ: the host reads a block */
    P_WR_DRQ,       /* DRQ: the host writes a block */
    P_WR_COMMIT,    /* BSY: a block written by the host is being committed */
    P_VERIFY,       /* BSY: READ VERIFY */
    P_RST_HOLD,     /* SRST asserted: BSY until it is released */
    P_RST_WAIT,     /* SRST released: the devices reset */
    P_PKT_WAIT,     /* BSY: PACKET accepted, the request for the command packet is due */
    P_PKT_CMD,      /* DRQ: the host writes the 12-byte command packet */
    P_PKT_EXEC,     /* BSY: the packet command executes */
    P_PKT_IN,       /* DRQ: the host reads a chunk of the data phase */
    P_PKT_OUT,      /* DRQ: the host writes a chunk of the data phase */
    P_PKT_NEXT,     /* BSY: between chunks */
    P_PKT_COMMIT    /* BSY: an out chunk was written */
};

#define ST_IDLE (MC_ATA_ST_DRDY | MC_ATA_ST_DSC)

static uint32_t min32(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* --- time and the interrupt pin ---------------------------------------- */

static void set_pin(mc_ata *a) {
    int pin = a->intrq && !(a->devctl & MC_ATA_DC_NIEN);
    if (pin != a->pin) {
        a->pin = pin;
        mc_dev_line(&a->dev, MC_ATA_LINE_INTRQ, pin);
    }
}
static void raise_irq(mc_ata *a) { a->intrq = 1; set_pin(a); }
static void clear_irq(mc_ata *a) { a->intrq = 0; set_pin(a); }
static void arm(mc_ata *a, uint64_t delay) {
    a->deadline = mc_dev_now(&a->dev) + delay;
    mc_dev_schedule(&a->dev, a->deadline);
}
static void disarm(mc_ata *a) {
    if (a->deadline != MC_NEVER) {
        a->deadline = MC_NEVER;
        mc_dev_schedule(&a->dev, MC_NEVER);
    }
}

static mc_ata_unit *cur(mc_ata *a) { return &a->unit[a->cur]; }
static mc_ata_unit *selected(mc_ata *a) { return &a->unit[a->selected]; }

/* --- geometry and addressing --------------------------------------------- */

static uint32_t total_sectors(const mc_ata_unit *u) {
    uint64_t n = u->media ? mc_vdisk_blocks(u->media) : 0;
    return n > 0x0FFFFFFFu ? 0x0FFFFFFFu : (uint32_t)n;       /* 28-bit LBA */
}

/* The default translation: 16 heads, 63 sectors a track, at most 16383
 * cylinders (IDENTIFY words 1, 3, 6; Livadix's notional geometry). */
static void set_default_geometry(mc_ata_unit *u) {
    uint32_t total = total_sectors(u);
    u->heads = 16;
    u->sectors = 63;
    u->cylinders = total / (16 * 63);
    if (u->cylinders > 16383) u->cylinders = 16383;
    if (u->cylinders < 1) u->cylinders = 1;
    u->cur_cylinders = u->cylinders;
    u->cur_heads = u->heads;
    u->cur_sectors = u->sectors;
}

/* The sector the task file addresses (LBA: drive/head bit 6, 7.2.8; CHS
 * otherwise). Returns 0 when it is not addressable (IDNF). */
static int get_address(mc_ata *a, const mc_ata_unit *u, uint32_t *lba) {
    uint32_t total = total_sectors(u);
    if (a->drive & 0x40) {
        *lba = (uint32_t)(a->drive & 0x0F) << 24 | (uint32_t)u->cyl_hi << 16 | (uint32_t)u->cyl_lo << 8 | u->sector;
        return *lba < total;
    }
    uint32_t cyl = (uint32_t)u->cyl_hi << 8 | u->cyl_lo, head = a->drive & 0x0F, sec = u->sector;
    if (head >= u->cur_heads || sec < 1 || sec > u->cur_sectors || cyl >= u->cur_cylinders) return 0;
    *lba = (cyl * u->cur_heads + head) * u->cur_sectors + (sec - 1);
    return *lba < total;
}

/* The task file shows sector `lba` (the last one transferred, or the one in error). */
static void set_address(mc_ata *a, mc_ata_unit *u, uint32_t lba) {
    if (a->drive & 0x40) {
        u->sector = (uint8_t)lba;
        u->cyl_lo = (uint8_t)(lba >> 8);
        u->cyl_hi = (uint8_t)(lba >> 16);
        a->drive = (uint8_t)((a->drive & 0xF0) | ((lba >> 24) & 0x0F));
    } else {
        uint32_t spt = u->cur_sectors, heads = u->cur_heads;
        uint32_t cyl = lba / (spt * heads);
        u->sector = (uint8_t)(lba % spt + 1);
        a->drive = (uint8_t)((a->drive & 0xF0) | ((lba / spt) % heads));
        u->cyl_lo = (uint8_t)cyl;
        u->cyl_hi = (uint8_t)(cyl >> 8);
    }
}

/* --- IDENTIFY ---------------------------------------------------------------- */

static void put_word(uint8_t *b, unsigned w, uint16_t v) { b[2 * w] = (uint8_t)v; b[2 * w + 1] = (uint8_t)(v >> 8); }
/* An ASCII field of `words` words, space padded, first character in the high byte of each word (ATA-2 8.12). */
static void put_string(uint8_t *b, unsigned first, unsigned words, const char *s) {
    for (unsigned i = 0; i < words * 2; i++) {
        char c = i < strlen(s) ? s[i] : ' ';
        b[2 * first + (i ^ 1)] = (uint8_t)c;
    }
}

/* The integrity word 255 (ATA-3†): signature A5h in the low byte, the two's
 * complement of the byte sum of words 0..254 and the signature in the high byte. */
static void put_integrity(uint8_t *b) {
    unsigned sum = 0xA5;
    for (unsigned i = 0; i < 510; i++) sum += b[i];
    b[510] = 0xA5;
    b[511] = (uint8_t)(0u - sum);
}

/* IDENTIFY DEVICE (ATA-2 8.12, Table 17). Word 0 bit 6 fixed disk; 1, 3, 6
 * default CHS; 10-19 serial; 22 ECC bytes; 23-26 firmware; 27-46 model; 47
 * maximum multiple; 49 bit 9 LBA; 51 PIO cycle timing class; 53 bit 0 words
 * 54-58 valid; 54-58 current CHS and capacity; 59 multiple setting; 60-61
 * LBA capacity; 64 PIO modes 3 and 4; 67, 68 PIO cycle times 120 ns; 80
 * major version ATA-1 to ATA-3; 82 bits 3 power management and 5 write cache,
 * 83 bit 12 FLUSH CACHE (ATA-3†); 85 bit 5 write cache enabled. */
static void identify_disk(const mc_ata_unit *u, uint8_t *b) {
    memset(b, 0, 512);
    uint32_t total = total_sectors(u);
    put_word(b, 0, 0x0040);
    put_word(b, 1, (uint16_t)u->cylinders);
    put_word(b, 3, (uint16_t)u->heads);
    put_word(b, 6, (uint16_t)u->sectors);
    put_string(b, 10, 10, u->serial);
    put_word(b, 22, 4);
    put_string(b, 23, 4, "1.0");
    put_string(b, 27, 20, "LIVADIX VIRTUAL DISK");
    put_word(b, 47, MC_ATA_MAX_MULTIPLE);
    put_word(b, 49, 0x0200);
    put_word(b, 51, 0x0200);
    put_word(b, 53, 0x0003);
    put_word(b, 54, (uint16_t)u->cur_cylinders);
    put_word(b, 55, (uint16_t)u->cur_heads);
    put_word(b, 56, (uint16_t)u->cur_sectors);
    uint32_t cur = u->cur_cylinders * u->cur_heads * u->cur_sectors;
    put_word(b, 57, (uint16_t)cur);
    put_word(b, 58, (uint16_t)(cur >> 16));
    put_word(b, 59, u->multiple ? (uint16_t)(0x0100 | u->multiple) : 0);
    put_word(b, 60, (uint16_t)total);
    put_word(b, 61, (uint16_t)(total >> 16));
    put_word(b, 64, 0x0003);
    put_word(b, 67, 120);
    put_word(b, 68, 120);
    put_word(b, 80, 0x000E);
    put_word(b, 82, 0x4028);
    put_word(b, 83, 0x5000);
    put_word(b, 84, 0x4000);
    put_word(b, 85, (uint16_t)(0x0008 | (u->write_cache ? 0x0020 : 0)));
    put_word(b, 86, 0x1000);
    put_word(b, 87, 0x4000);
    put_integrity(b);
}

/* IDENTIFY PACKET DEVICE (SFF-8020i 8.x): word 0 ATAPI protocol (bits 15..14
 * = 10), CD-ROM device type 5 (bits 12..8), removable (bit 7), microprocessor
 * DRQ (bits 6..5 = 00), 12-byte packets (bits 1..0 = 00); 49 bit 9 LBA; 51
 * PIO class; 53 bit 1; 64 PIO modes 3, 4. */
static void identify_packet(const mc_ata_unit *u, uint8_t *b) {
    memset(b, 0, 512);
    put_word(b, 0, 0x8580);
    put_string(b, 10, 10, u->serial);
    put_string(b, 23, 4, "1.0");
    put_string(b, 27, 20, "LIVADIX VIRTUAL CD-ROM");
    put_word(b, 49, 0x0200);
    put_word(b, 51, 0x0200);
    put_word(b, 53, 0x0002);
    put_word(b, 64, 0x0003);
    put_word(b, 67, 120);
    put_word(b, 68, 120);
    put_word(b, 80, 0x000E);
    put_integrity(b);
}

/* --- completion -------------------------------------------------------------- */

static void finish_ok(mc_ata *a, int irq) {
    mc_ata_unit *u = cur(a);
    u->status = ST_IDLE;
    a->phase = P_IDLE;
    if (irq) raise_irq(a);
}

static void finish_error(mc_ata *a, uint8_t error) {
    mc_ata_unit *u = cur(a);
    u->error = error;
    u->status = ST_IDLE | MC_ATA_ST_ERR;
    a->phase = P_IDLE;
    raise_irq(a);
}

/* The device signature after a reset or diagnostic (ATA-2 9.9, SFF-8020i):
 * count 1, sector 1, cylinder 0000h for ATA, EB14h for ATAPI. */
static void signature(mc_ata_unit *u) {
    u->count = 1;
    u->sector = 1;
    u->cyl_lo = u->kind == MC_ATA_PACKET ? 0x14 : 0x00;
    u->cyl_hi = u->kind == MC_ATA_PACKET ? 0xEB : 0x00;
}

static void defaults(mc_ata_unit *u) {
    u->cur_cylinders = u->cylinders;
    u->cur_heads = u->heads;
    u->cur_sectors = u->sectors;
    u->multiple = 0;
    u->write_cache = 1;
    u->look_ahead = 1;
    u->pio_mode = 0;
    u->power = 0xFF;
    u->sleeping = 0;
}

static void unit_reset(mc_ata_unit *u) {
    defaults(u);
    signature(u);
    u->error = 0x01;            /* diagnostic code: no error detected */
    u->status = u->kind == MC_ATA_PACKET ? 0x00 : ST_IDLE;
    if (u->kind == MC_ATA_PACKET) {
        u->scsi.target.reset(&u->scsi.target);
        u->scsi.block = 2048;
    }
}

static void reset_complete(mc_ata *a) {
    for (unsigned i = 0; i < 2; i++)
        if (a->unit[i].kind != MC_ATA_NONE) unit_reset(&a->unit[i]);
    a->drive = 0xA0;
    a->selected = 0;
    a->phase = P_IDLE;
    clear_irq(a);
}

/* --- reads and writes ------------------------------------------------------ */

static void read_load(mc_ata *a) {
    mc_ata_unit *u = cur(a);
    uint32_t total = total_sectors(u);
    uint32_t n = min32(a->block_sectors, a->sectors_left);
    if (a->lba >= total) {
        set_address(a, u, (uint32_t)a->lba);       /* the sector in error is the first one beyond the end */
        finish_error(a, MC_ATA_ER_IDNF);
        return;
    }
    if (a->lba + n > total) n = total - (uint32_t)a->lba;
    if (!mc_vdisk_read(u->media, a->lba, n, a->buffer)) {
        set_address(a, u, (uint32_t)a->lba);
        finish_error(a, MC_ATA_ER_UNC);
        return;
    }
    a->len = n * MC_ATA_SECTOR_BYTES;
    a->pos = 0;
    set_address(a, u, (uint32_t)a->lba + n - 1);
    u->count = (uint8_t)(a->sectors_left - n);
    u->status = ST_IDLE | MC_ATA_ST_DRQ;
    a->phase = P_RD_DRQ;
    raise_irq(a);
}

static void read_block_done(mc_ata *a) {
    mc_ata_unit *u = cur(a);
    uint32_t n = a->len / MC_ATA_SECTOR_BYTES;
    if (a->cmd == 0xEC || a->cmd == 0xA1 || a->cmd == 0xE4) n = a->sectors_left;      /* one block, no addressing */
    a->lba += n;
    a->sectors_left -= n;
    if (!a->sectors_left) {
        finish_ok(a, 0);
        return;
    }
    u->status = MC_ATA_ST_BSY;
    a->phase = P_RD_LOAD;
    arm(a, MC_ATA_T_SECTOR);
}

static void write_request(mc_ata *a) {
    mc_ata_unit *u = cur(a);
    uint32_t n = min32(a->block_sectors, a->sectors_left);
    a->len = a->cmd == 0xE8 ? MC_ATA_SECTOR_BYTES : n * MC_ATA_SECTOR_BYTES;
    a->pos = 0;
    u->status = ST_IDLE | MC_ATA_ST_DRQ;
    a->phase = P_WR_DRQ;
}

static void write_commit(mc_ata *a) {
    mc_ata_unit *u = cur(a);
    if (a->cmd == 0xE8) {
        memcpy(a->sector_buffer, a->buffer, MC_ATA_SECTOR_BYTES);
        finish_ok(a, 1);
        return;
    }
    uint32_t total = total_sectors(u);
    uint32_t n = a->len / MC_ATA_SECTOR_BYTES;
    if (a->lba >= total || a->lba + n > total) {
        set_address(a, u, a->lba >= total ? (uint32_t)a->lba : total);
        finish_error(a, MC_ATA_ER_IDNF);
        return;
    }
    if (!mc_vdisk_write(u->media, a->lba, n, a->buffer)) {
        set_address(a, u, (uint32_t)a->lba);
        u->error = MC_ATA_ER_ABRT;
        u->status = ST_IDLE | MC_ATA_ST_DWF | MC_ATA_ST_ERR;
        a->phase = P_IDLE;
        raise_irq(a);
        return;
    }
    set_address(a, u, (uint32_t)a->lba + n - 1);
    a->lba += n;
    a->sectors_left -= n;
    u->count = (uint8_t)a->sectors_left;
    raise_irq(a);
    if (!a->sectors_left) {
        finish_ok(a, 0);
        return;
    }
    write_request(a);
}

/* --- ATAPI --------------------------------------------------------------------- */

static void packet_finish(mc_ata *a, uint8_t scsi_status) {
    mc_ata_unit *u = cur(a);
    u->count = MC_ATAPI_COD | MC_ATAPI_IO;
    if (scsi_status != MC_SCSI_GOOD) {
        u->error = (uint8_t)(u->scsi.key << 4);
        if (u->scsi.key == MC_SENSE_ILLEGAL_REQUEST) u->error |= MC_ATA_ER_ABRT;
        u->status = ST_IDLE | MC_ATA_ST_ERR;
    } else {
        u->status = ST_IDLE;
    }
    a->phase = P_IDLE;
    raise_irq(a);
}

/* A data transfer the target cut short: its status (CHECK CONDITION when it gave none). */
static void packet_fail(mc_ata *a) {
    mc_ata_unit *u = cur(a);
    uint8_t st = u->scsi.target.finish(&u->scsi.target);
    packet_finish(a, st ? st : MC_SCSI_CHECK_CONDITION);
}

/* The next bytes of the data phase from the target into `stage`; 0 on a target error. */
static int stage_fetch(mc_ata *a, mc_ata_unit *u) {
    uint32_t left = a->pkt_total - a->pkt_fetched;
    uint32_t n = min32(left, sizeof a->stage);
    if (u->scsi.mode != MC_UNIT_RESPONSE && n > 512) n &= ~511u;
    if (!n) return 0;
    if (u->scsi.target.data_in(&u->scsi.target, a->stage, a->pkt_fetched, n)) return 0;
    a->pkt_fetched += n;
    a->stage_pos = 0;
    a->stage_len = n;
    return 1;
}

static uint32_t chunk_size(const mc_ata *a) {
    return min32(a->pkt_limit, a->pkt_total - a->pkt_done);
}

static void packet_present(mc_ata *a) {
    mc_ata_unit *u = cur(a);
    uint32_t chunk = chunk_size(a);
    if (a->pkt_dir == MC_SCSI_XFER_IN) {
        uint32_t got = 0;
        while (got < chunk) {
            if (a->stage_pos == a->stage_len && !stage_fetch(a, u)) {
                packet_fail(a);
                return;
            }
            uint32_t k = min32(a->stage_len - a->stage_pos, chunk - got);
            memcpy(a->buffer + got, a->stage + a->stage_pos, k);
            a->stage_pos += k;
            got += k;
        }
        u->count = MC_ATAPI_IO;
        a->phase = P_PKT_IN;
    } else {
        u->count = 0;
        a->phase = P_PKT_OUT;
    }
    u->cyl_lo = (uint8_t)chunk;
    u->cyl_hi = (uint8_t)(chunk >> 8);
    a->len = chunk;
    a->pos = 0;
    u->status = ST_IDLE | MC_ATA_ST_DRQ;
    raise_irq(a);
}

static void packet_execute(mc_ata *a) {
    mc_ata_unit *u = cur(a);
    int dir = MC_SCSI_XFER_NONE;
    uint32_t bytes = 0;
    uint8_t st = u->scsi.target.command(&u->scsi.target, 0, a->packet, 12, &dir, &bytes);
    a->stage_pos = a->stage_len = 0;
    a->pkt_total = bytes;
    a->pkt_done = a->pkt_fetched = 0;
    a->pkt_dir = dir;
    if (st != MC_SCSI_GOOD) {
        packet_finish(a, st);
        return;
    }
    if (dir == MC_SCSI_XFER_NONE || !bytes) {
        packet_finish(a, u->scsi.target.finish(&u->scsi.target));
        return;
    }
    packet_present(a);
}

static void packet_chunk_done(mc_ata *a) {
    mc_ata_unit *u = cur(a);
    a->pkt_done += a->len;
    if (a->pkt_done < a->pkt_total) {
        u->status = MC_ATA_ST_BSY;
        a->phase = P_PKT_NEXT;
        arm(a, MC_ATA_T_SECTOR);
        return;
    }
    packet_finish(a, u->scsi.target.finish(&u->scsi.target));
}

/* --- commands --------------------------------------------------------------------- */

static void abort_command(mc_ata *a) {
    mc_ata_unit *u = cur(a);
    if (u->kind == MC_ATA_PACKET) {     /* an unsupported command leaves the signature (SFF-8020i) */
        u->count = 1;
        u->sector = 1;
        u->cyl_lo = 0x14;
        u->cyl_hi = 0xEB;
    }
    finish_error(a, MC_ATA_ER_ABRT);
}

static int media_command(uint8_t cmd) {
    return cmd == 0x20 || cmd == 0x21 || cmd == 0x30 || cmd == 0x31 || cmd == 0x40 || cmd == 0x41 || cmd == 0xC4 ||
           cmd == 0xC5 || (cmd >= 0x70 && cmd <= 0x7F);
}

static int set_features(mc_ata *a, mc_ata_unit *u) {
    switch (a->features) {
    case 0x02: u->write_cache = 1; return 1;
    case 0x82: u->write_cache = 0; return 1;
    case 0x55: u->look_ahead = 0; return 1;
    case 0xAA: u->look_ahead = 1; return 1;
    case 0x33: case 0x99: case 0x66: case 0xCC: case 0x77: case 0xBB: return 1;  /* retries, defaults after reset, ECC bytes */
    case 0x03: {        /* transfer mode in the sector count register: PIO default 00h/01h or flow control 08h + mode 0..4 */
        uint8_t m = u->count;
        if (m == 0x00 || m == 0x01) { u->pio_mode = 0; return 1; }
        if ((m & 0xF8) == 0x08 && (m & 7) <= 4) { u->pio_mode = m & 7; return 1; }
        return 0;       /* DMA modes and the rest */
    }
    default: return 0;
    }
}

static void start_ata(mc_ata *a, mc_ata_unit *u) {
    uint8_t cmd = a->cmd;
    uint32_t count = u->count ? u->count : 256;
    uint32_t lba;
    if (media_command(cmd)) u->power = 0xFF;
    switch (cmd) {
    case 0x20: case 0x21: case 0xC4:
        if (cmd == 0xC4 && !u->multiple) { abort_command(a); return; }
        if (!get_address(a, u, &lba)) { finish_error(a, MC_ATA_ER_IDNF); return; }
        a->lba = lba;
        a->sectors_left = count;
        a->block_sectors = cmd == 0xC4 ? u->multiple : 1;
        u->status = MC_ATA_ST_BSY;
        a->phase = P_RD_LOAD;
        arm(a, MC_ATA_T_FIRST);
        return;
    case 0x30: case 0x31: case 0xC5:
        if (cmd == 0xC5 && !u->multiple) { abort_command(a); return; }
        if (!get_address(a, u, &lba)) { finish_error(a, MC_ATA_ER_IDNF); return; }
        if (mc_vdisk_readonly(u->media, lba, count)) { abort_command(a); return; }
        a->lba = lba;
        a->sectors_left = count;
        a->block_sectors = cmd == 0xC5 ? u->multiple : 1;
        write_request(a);
        return;
    case 0x40: case 0x41:
        if (!get_address(a, u, &lba)) { finish_error(a, MC_ATA_ER_IDNF); return; }
        a->lba = lba;
        a->sectors_left = count;
        u->status = MC_ATA_ST_BSY;
        a->phase = P_VERIFY;
        arm(a, MC_ATA_T_FIRST);
        return;
    case 0x91: {        /* INITIALIZE DEVICE PARAMETERS (9.? ATA-2 8.?): heads from drive/head, sectors from the count */
        uint32_t heads = (a->drive & 0x0F) + 1u, spt = u->count;
        if (heads > 16 || spt < 1 || spt > 255) { abort_command(a); return; }
        u->cur_heads = heads;
        u->cur_sectors = spt;
        uint32_t cyl = total_sectors(u) / (heads * spt);
        u->cur_cylinders = cyl > 16383 ? 16383 : (cyl ? cyl : 1);
        finish_ok(a, 1);
        return;
    }
    case 0xC6: {        /* SET MULTIPLE MODE: a power of two up to the maximum, 0 disables */
        uint8_t n = u->count;
        if (n > MC_ATA_MAX_MULTIPLE || (n & (n - 1))) { abort_command(a); return; }
        u->multiple = n;
        finish_ok(a, 1);
        return;
    }
    case 0xEC:
        identify_disk(u, a->buffer);
        goto data_in_block;
    case 0xE4:
        memcpy(a->buffer, a->sector_buffer, MC_ATA_SECTOR_BYTES);
    data_in_block:
        a->sectors_left = 1;
        a->block_sectors = 1;
        a->len = 512;
        a->pos = 0;
        u->status = ST_IDLE | MC_ATA_ST_DRQ;
        a->phase = P_RD_DRQ;
        raise_irq(a);
        return;
    case 0xE8:
        a->sectors_left = 1;
        a->block_sectors = 1;
        write_request(a);
        return;
    case 0xEF:
        if (set_features(a, u)) finish_ok(a, 1); else abort_command(a);
        return;
    case 0xE0: case 0xE2: u->power = 0x00; finish_ok(a, 1); return;     /* STANDBY (IMMEDIATE) */
    case 0xE1: case 0xE3: u->power = 0x80; finish_ok(a, 1); return;     /* IDLE (IMMEDIATE) */
    case 0xE5:          /* CHECK POWER MODE: 00h standby, 80h idle, FFh active or idle */
        u->count = u->power;
        finish_ok(a, 1);
        return;
    case 0xE6:          /* SLEEP: nothing answers until a reset */
        finish_ok(a, 1);
        u->sleeping = 1;
        return;
    case 0xE7:          /* FLUSH CACHE: the writes are already with the host system */
        finish_ok(a, 1);
        return;
    case 0x90:          /* EXECUTE DEVICE DIAGNOSTIC: the signature, code 01h */
        signature(u);
        u->error = 0x01;
        u->status = ST_IDLE;
        a->phase = P_IDLE;
        raise_irq(a);
        return;
    default:
        if (cmd >= 0x10 && cmd <= 0x1F) {       /* RECALIBRATE: cylinder 0 */
            u->cyl_lo = u->cyl_hi = 0;
            finish_ok(a, 1);
            return;
        }
        if (cmd >= 0x70 && cmd <= 0x7F) {       /* SEEK: the address must exist */
            if (!get_address(a, u, &lba)) { finish_error(a, MC_ATA_ER_IDNF); return; }
            finish_ok(a, 1);
            return;
        }
        abort_command(a);
        return;
    }
}

static void start_packet_unit(mc_ata *a, mc_ata_unit *u) {
    uint8_t cmd = a->cmd;
    switch (cmd) {
    case 0xA0:          /* PACKET: the byte count limit in the cylinder registers (SFF-8020i) */
        if (a->features & 1) { abort_command(a); return; }      /* DMA */
        a->pkt_limit = (uint32_t)u->cyl_hi << 8 | u->cyl_lo;
        a->pkt_limit &= ~1u;
        if (!a->pkt_limit) a->pkt_limit = 0xFFFE;
        a->phase = P_PKT_WAIT;
        arm(a, MC_ATA_T_PACKET);
        return;
    case 0xA1:
        identify_packet(u, a->buffer);
        a->sectors_left = 1;
        a->block_sectors = 1;
        a->len = 512;
        a->pos = 0;
        u->status = ST_IDLE | MC_ATA_ST_DRQ;
        a->phase = P_RD_DRQ;
        raise_irq(a);
        return;
    case 0x08:          /* DEVICE RESET: the signature, BSY cleared, no interrupt† */
        unit_reset(u);
        u->status = ST_IDLE;
        a->phase = P_IDLE;
        return;
    case 0x90:
        signature(u);
        u->error = 0x01;
        u->status = ST_IDLE;
        a->phase = P_IDLE;
        raise_irq(a);
        return;
    case 0xEF:
        if (set_features(a, u)) finish_ok(a, 1); else abort_command(a);
        return;
    case 0xE0: case 0xE2: u->power = 0x00; finish_ok(a, 1); return;
    case 0xE1: case 0xE3: u->power = 0x80; finish_ok(a, 1); return;
    case 0xE5: u->count = u->power; finish_ok(a, 1); return;
    default:
        abort_command(a);
        return;
    }
}

static void run_event(mc_ata *a) {
    mc_ata_unit *u = cur(a);
    a->deadline = MC_NEVER;
    switch (a->phase) {
    case P_CMD:
        if (u->kind == MC_ATA_PACKET) start_packet_unit(a, u); else start_ata(a, u);
        break;
    case P_RD_LOAD: read_load(a); break;
    case P_WR_COMMIT: write_commit(a); break;
    case P_VERIFY: {
        uint32_t total = total_sectors(u), n = a->sectors_left;
        if (a->lba + n > total) {       /* the first sector beyond the end is the one in error */
            uint32_t good = a->lba >= total ? 0 : total - (uint32_t)a->lba;
            set_address(a, u, (uint32_t)a->lba + good);
            u->count = (uint8_t)(n - good);
            finish_error(a, MC_ATA_ER_IDNF);
            break;
        }
        set_address(a, u, (uint32_t)a->lba + n - 1);
        u->count = 0;
        finish_ok(a, 1);
        break;
    }
    case P_RST_WAIT: reset_complete(a); break;
    case P_PKT_WAIT:
        u->count = MC_ATAPI_COD;        /* the command packet is wanted */
        u->status = ST_IDLE | MC_ATA_ST_DRQ;
        a->phase = P_PKT_CMD;
        a->len = 12;
        a->pos = 0;
        break;
    case P_PKT_EXEC: packet_execute(a); break;
    case P_PKT_NEXT: packet_present(a); break;
    case P_PKT_COMMIT: {
        uint32_t chunk = a->len;
        if (u->scsi.target.data_out(&u->scsi.target, a->buffer, a->pkt_done, chunk)) {
            packet_fail(a);
            break;
        }
        packet_chunk_done(a);
        break;
    }
    default:
        break;
    }
}

static void catch_up(mc_ata *a) {
    if (a->deadline != MC_NEVER && mc_dev_now(&a->dev) >= a->deadline) {
        mc_dev_schedule(&a->dev, MC_NEVER);
        run_event(a);
    }
}

static void event(void *dev, uint64_t now) {
    mc_ata *a = dev;
    (void)now;
    if (a->deadline != MC_NEVER && mc_dev_now(&a->dev) >= a->deadline) run_event(a);
}

/* --- the data register ------------------------------------------------------------ */

uint16_t mc_ata_data_read16(mc_ata *a) {
    if (a->phase != P_RD_DRQ && a->phase != P_PKT_IN) return 0;
    uint16_t w = 0;
    if (a->pos < a->len) w = a->buffer[a->pos];
    if (a->pos + 1 < a->len) w |= (uint16_t)a->buffer[a->pos + 1] << 8;
    a->pos += 2;
    if (a->pos >= a->len) {
        a->pos = a->len;
        if (a->phase == P_RD_DRQ) read_block_done(a); else packet_chunk_done(a);
    }
    return w;
}

void mc_ata_data_write16(mc_ata *a, uint16_t word) {
    if (a->phase != P_WR_DRQ && a->phase != P_PKT_CMD && a->phase != P_PKT_OUT) return;
    uint8_t *dst = a->phase == P_PKT_CMD ? a->packet : a->buffer;
    if (a->pos < a->len) dst[a->pos] = (uint8_t)word;
    if (a->pos + 1 < a->len) dst[a->pos + 1] = (uint8_t)(word >> 8);
    a->pos += 2;
    if (a->pos < a->len) return;
    a->pos = a->len;
    mc_ata_unit *u = cur(a);
    u->status = MC_ATA_ST_BSY;
    if (a->phase == P_WR_DRQ) {
        a->phase = P_WR_COMMIT;
        arm(a, MC_ATA_T_WRITE);
    } else if (a->phase == P_PKT_CMD) {
        a->phase = P_PKT_EXEC;
        arm(a, MC_ATA_T_EXEC);
    } else {
        a->phase = P_PKT_COMMIT;
        arm(a, MC_ATA_T_SECTOR);
    }
}

/* --- registers ---------------------------------------------------------------------- */

static void srst(mc_ata *a, int assert_) {
    if (assert_) {
        disarm(a);
        a->phase = P_RST_HOLD;
        a->pos = a->len = 0;
        for (unsigned i = 0; i < 2; i++)
            if (a->unit[i].kind != MC_ATA_NONE) a->unit[i].status = MC_ATA_ST_BSY;
        clear_irq(a);
    } else if (a->phase == P_RST_HOLD) {
        a->phase = P_RST_WAIT;
        arm(a, MC_ATA_T_RESET);
    }
}

static void command_write(mc_ata *a, uint8_t cmd) {
    mc_ata_unit *u = selected(a);
    if (u->kind == MC_ATA_NONE) return;
    if (cmd == 0x08 && u->kind == MC_ATA_PACKET && a->phase != P_RST_HOLD && a->phase != P_RST_WAIT) {
        disarm(a);          /* DEVICE RESET is taken in any state */
        a->phase = P_IDLE;
    }
    if (a->phase != P_IDLE) return;     /* a command while BSY or DRQ is ignored (7.2.13) */
    if (u->sleeping && cmd != 0x08 && cmd != 0x90) return;
    a->cur = a->selected;
    a->cmd = cmd;
    u->error = 0;
    clear_irq(a);
    u->status = MC_ATA_ST_BSY;
    a->phase = P_CMD;
    arm(a, MC_ATA_T_CMD + (media_command(cmd) && u->power == 0 ? MC_ATA_T_SPINUP : 0));
}

static void task_write(mc_ata *a, unsigned reg, uint8_t v) {
    if (a->phase != P_IDLE) return;     /* the command block is not writable while BSY or DRQ */
    for (unsigned i = 0; i < 2; i++) {  /* both devices receive the writes */
        mc_ata_unit *u = &a->unit[i];
        switch (reg) {
        case MC_ATA_COUNT: u->count = v; break;
        case MC_ATA_SECTOR: u->sector = v; break;
        case MC_ATA_CYL_LO: u->cyl_lo = v; break;
        case MC_ATA_CYL_HI: u->cyl_hi = v; break;
        default: break;
        }
    }
}

uint32_t mc_ata_reg_read(mc_ata *a, unsigned reg, unsigned size) {
    catch_up(a);
    mc_ata_unit *u = selected(a);
    if (reg == MC_ATA_DATA) {
        uint16_t w = mc_ata_data_read16(a);
        if (size == 4) {
            uint16_t w2 = mc_ata_data_read16(a);
            if (a->lane_swap) { w = (uint16_t)(w << 8 | w >> 8); w2 = (uint16_t)(w2 << 8 | w2 >> 8); }
            return (uint32_t)w << 16 | w2;
        }
        if (a->lane_swap) w = (uint16_t)(w << 8 | w >> 8);
        return size == 1 ? (w & 0xFF) : w;
    }
    if (reg == MC_ATA_DRIVE_ADDRESS) {      /* ATA-2 7.2.1: nWTG, nHS3..0, nDS1, nDS0 (active low); bit 7 high */
        return 0x80u | 0x40u | ((~(a->drive & 0x0Fu) & 0x0Fu) << 2) | (a->selected ? 0x01u : 0x02u);
    }
    if (u->kind == MC_ATA_NONE) return 0;
    switch (reg) {
    case MC_ATA_ERROR: return u->error;
    case MC_ATA_COUNT: return u->count;
    case MC_ATA_SECTOR: return u->sector;
    case MC_ATA_CYL_LO: return u->cyl_lo;
    case MC_ATA_CYL_HI: return u->cyl_hi;
    case MC_ATA_DRIVE: return a->drive;
    case MC_ATA_STATUS: clear_irq(a); return u->status;
    case MC_ATA_ALT_STATUS: return u->status;
    default: return 0;
    }
}

void mc_ata_reg_write(mc_ata *a, unsigned reg, unsigned size, uint32_t value) {
    catch_up(a);
    if (reg == MC_ATA_DATA) {
        if (size == 4) {
            uint16_t hi = (uint16_t)(value >> 16), lo = (uint16_t)value;
            if (a->lane_swap) { hi = (uint16_t)(hi << 8 | hi >> 8); lo = (uint16_t)(lo << 8 | lo >> 8); }
            mc_ata_data_write16(a, hi);
            mc_ata_data_write16(a, lo);
        } else {
            uint16_t w = (uint16_t)value;
            if (a->lane_swap && size == 2) w = (uint16_t)(w << 8 | w >> 8);
            mc_ata_data_write16(a, w);
        }
        return;
    }
    uint8_t v = (uint8_t)value;
    switch (reg) {
    case MC_ATA_FEATURES: if (a->phase == P_IDLE) a->features = v; break;
    case MC_ATA_COUNT: case MC_ATA_SECTOR: case MC_ATA_CYL_LO: case MC_ATA_CYL_HI: task_write(a, reg, v); break;
    case MC_ATA_DRIVE:
        if (a->phase == P_IDLE) {
            a->drive = v | 0xA0;
            a->selected = (v >> 4) & 1;
        }
        break;
    case MC_ATA_COMMAND: command_write(a, v); break;
    case MC_ATA_DEV_CONTROL: {
        uint8_t old = a->devctl;
        a->devctl = v & (MC_ATA_DC_NIEN | MC_ATA_DC_SRST);
        if ((v & MC_ATA_DC_SRST) && !(old & MC_ATA_DC_SRST)) srst(a, 1);
        else if (!(v & MC_ATA_DC_SRST) && (old & MC_ATA_DC_SRST)) srst(a, 0);
        set_pin(a);
        break;
    }
    default: break;
    }
}

/* --- the chip ---------------------------------------------------------------------------- */

static uint32_t ops_read(void *dev, uint32_t offset, unsigned size) {
    mc_ata *a = dev;
    return mc_ata_reg_read(a, (offset >> a->reg_shift) & 15, size);
}
static void ops_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_ata *a = dev;
    mc_ata_reg_write(a, (offset >> a->reg_shift) & 15, size, value);
}
static void ops_reset(void *dev) { mc_ata_reset(dev); }

/* Register reads that change nothing until the chip's next event or a write:
 * the alternate status, the byte registers, and the status register while
 * INTRQ is not asserted; the data register never. */
static int ops_stable(void *dev, uint32_t offset, unsigned size) {
    mc_ata *a = dev;
    (void)size;
    unsigned reg = (offset >> a->reg_shift) & 15;
    if (a->phase == P_RD_DRQ || a->phase == P_PKT_IN) return reg != MC_ATA_DATA && (reg != MC_ATA_STATUS || !a->intrq);
    if (reg == MC_ATA_DATA) return 0;
    return reg != MC_ATA_STATUS || !a->intrq;
}

const mc_dev_ops mc_ata_ops = {ops_read, ops_write, event, ops_reset, ops_stable};

void mc_ata_init(mc_ata *a, const mc_hw_host *host, unsigned reg_shift) {
    memset(a, 0, sizeof *a);
    a->dev.ops = &mc_ata_ops;
    a->dev.host = host;
    a->dev.name = "ata";
    a->reg_shift = reg_shift;
    a->lane_swap = 1;
    a->deadline = MC_NEVER;
    a->drive = 0xA0;
}

void mc_ata_reset(mc_ata *a) {
    disarm(a);
    a->devctl = 0;
    a->features = 0;
    a->pos = a->len = 0;
    a->drive = 0xA0;
    a->selected = 0;
    clear_irq(a);
    for (unsigned i = 0; i < 2; i++)
        if (a->unit[i].kind != MC_ATA_NONE) a->unit[i].status = MC_ATA_ST_BSY;
    a->phase = P_RST_WAIT;
    arm(a, MC_ATA_T_RESET);
}

static void set_serial(mc_ata_unit *u, unsigned dev, const char *prefix) {
    snprintf(u->serial, sizeof u->serial, "%s%08u", prefix, dev + 1);
}

void mc_ata_attach_disk(mc_ata *a, unsigned dev, mc_vdisk *media) {
    mc_ata_unit *u = &a->unit[dev & 1];
    memset(u, 0, sizeof *u);
    if (!media) return;
    u->kind = MC_ATA_DISK;
    u->media = media;
    set_default_geometry(u);
    defaults(u);
    signature(u);
    u->error = 0x01;
    u->status = ST_IDLE;
    set_serial(u, dev & 1, "LVDXHD");
}

static void cd_ejected(void *ctx, mc_scsi_unit *unit, mc_vdisk *media) {
    mc_ata *a = ctx;
    unsigned dev = unit == &a->unit[1].scsi ? 1 : 0;
    if (a->on_eject) a->on_eject(a->eject_ctx, dev, media);
}

void mc_ata_attach_cd(mc_ata *a, unsigned dev) {
    mc_ata_unit *u = &a->unit[dev & 1];
    memset(u, 0, sizeof *u);
    u->kind = MC_ATA_PACKET;
    mc_scsi_cd_init(&u->scsi);
    u->scsi.block = 2048;       /* ATAPI CD-ROMs read 2048-byte sectors (SFF-8020i) */
    u->scsi.read_cd = 1;        /* and take READ CD (SFF-8020i 10.8.14) */
    mc_scsi_unit_on_eject(&u->scsi, cd_ejected, a);
    defaults(u);
    signature(u);
    u->error = 0x01;
    u->status = 0x00;
    set_serial(u, dev & 1, "LVDXCD");
}

int mc_ata_cd_insert(mc_ata *a, unsigned dev, mc_vdisk *media) {
    mc_ata_unit *u = &a->unit[dev & 1];
    if (u->kind != MC_ATA_PACKET) return 0;
    int r = mc_scsi_cd_insert(&u->scsi, media);
    if (r) u->scsi.block = 2048;
    return r;
}

mc_vdisk *mc_ata_cd_remove(mc_ata *a, unsigned dev) {
    mc_ata_unit *u = &a->unit[dev & 1];
    return u->kind == MC_ATA_PACKET ? mc_scsi_unit_remove(&u->scsi) : NULL;
}

void mc_ata_on_eject(mc_ata *a, mc_ata_eject_fn fn, void *ctx) {
    a->on_eject = fn;
    a->eject_ctx = ctx;
}
