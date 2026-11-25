/* Livadix — a DBDMA channel (dbdma.h).
 *
 * Livadix's own code. */
#include "dbdma.h"

#include <string.h>

#define CHUNK 4096u
#define WRITABLE (MC_DBDMA_RUN | MC_DBDMA_PAUSE | MC_DBDMA_FLUSH | MC_DBDMA_WAKE | 0x00FFu)

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* Tables 330, 339, 341: (s7..s0 & mask) == (value & mask). */
static int condition(const mc_dbdma *ch, unsigned which) {
    const uint32_t sel = ch->select[which];
    const uint8_t mask = (uint8_t)(sel >> 16), value = (uint8_t)sel;
    return (uint8_t)(ch->status & mask) == (uint8_t)(value & mask);
}

/* Tables 338, 340, 342: never, if set, if clear, always. */
static int test(unsigned field, int c) {
    switch (field & 3u) {
    case 0: return 0;
    case 1: return c;
    case 2: return !c;
    default: return 1;
    }
}

static unsigned cmd_of(const mc_dbdma *ch) { return ch->word[0] >> 28; }
static unsigned req_of(const mc_dbdma *ch) { return ch->word[0] & 0xFFFFu; }

void mc_dbdma_reset(mc_dbdma *ch) {
    ch->status = 0;
    ch->status_read_valid = 0;
    ch->command = 0;
    memset(ch->select, 0, sizeof ch->select);
    ch->loaded = 0;
    ch->done = 0;
    ch->waiting = 0;
    ch->ended = 0;
    ch->stopped = 0;
}

void mc_dbdma_init(mc_dbdma *ch, const mc_dbdma_host *host, unsigned index, const mc_dbdma_port *port) {
    memset(ch, 0, sizeof *ch);
    ch->host = host;
    ch->index = index;
    if (port) ch->port = *port;
    mc_dbdma_reset(ch);
}

void mc_dbdma_set_port(mc_dbdma *ch, const mc_dbdma_port *port) {
    if (port) ch->port = *port;
    else memset(&ch->port, 0, sizeof ch->port);
}

static int mem(mc_dbdma *ch, uint32_t address, void *data, uint32_t length, int write) {
    return ch->host->dma && ch->host->dma(ch->host->ctx, address, data, length, write);
}

static void interrupt(mc_dbdma *ch) {
    if (ch->host->interrupt) ch->host->interrupt(ch->host->ctx, ch->index);
}

/* 15.6.2: the channel dies, tries to return status, interrupts. */
static void die(mc_dbdma *ch) {
    ch->status = (uint16_t)((ch->status | MC_DBDMA_DEAD) & ~MC_DBDMA_ACTIVE);
    if (ch->loaded) {
        uint8_t b[4];
        put32(b, (req_of(ch) - ch->done) | (uint32_t)(ch->status | MC_DBDMA_ACTIVE) << 16);
        mem(ch, ch->command + 12, b, 4, 1);
    }
    ch->loaded = 0;
    interrupt(ch);
}

static int write_status(mc_dbdma *ch, uint16_t extra) {
    uint8_t b[4];
    const unsigned cmd = cmd_of(ch);
    const uint32_t res = cmd <= MC_DBDMA_INPUT_LAST ? req_of(ch) - ch->done : 0;
    put32(b, res | (uint32_t)(ch->status | MC_DBDMA_ACTIVE | extra) << 16);
    return mem(ch, ch->command + 12, b, 4, 1);
}

/* 15.7.1: wait, then the branch, status, interrupt and next fetch.
 * 0 when the wait condition holds the command. */
static int complete(mc_dbdma *ch) {
    const unsigned cmd = cmd_of(ch);
    if (test(ch->word[0] >> 16, condition(ch, 2))) { ch->waiting = 1; return 0; }
    ch->waiting = 0;
    int taken = 0;
    if (cmd <= MC_DBDMA_INPUT_LAST || cmd == MC_DBDMA_NOP) {
        taken = test(ch->word[0] >> 18, condition(ch, 1));
        ch->status = (uint16_t)(taken ? ch->status | MC_DBDMA_BT : ch->status & ~MC_DBDMA_BT);
    }
    if (!write_status(ch, 0)) { die(ch); return 1; }
    const int irq = test(ch->word[0] >> 20, condition(ch, 0));
    ch->command = taken ? ch->word[2] & ~15u : ch->command + 16;
    ch->loaded = 0;
    if (irq) interrupt(ch);
    return 1;
}

static unsigned quad_size(unsigned req) { return req & 4u ? 4u : req & 2u ? 2u : 1u; }    /* Table 345 */

/* A quad's access in its key's space. */
static int quad(mc_dbdma *ch, uint32_t address, unsigned size, uint32_t *value, int write) {
    const unsigned key = (ch->word[0] >> 24) & 7u;
    address &= ~(size - 1u);
    if (key == MC_DBDMA_KEY_SYSTEM) {
        uint8_t b[4] = {0};
        if (write) { put32(b, *value); return mem(ch, address, b, size, 1); }
        if (!mem(ch, address, b, size, 0)) return 0;
        *value = le32(b);
        return 1;
    }
    if (key == MC_DBDMA_KEY_REGS) {
        if (write) mc_dbdma_write(ch, address & 0xFFu, size, *value);
        else *value = mc_dbdma_read(ch, address & 0xFFu, size);
        return 1;
    }
    if (key == MC_DBDMA_KEY_DEVICE && ch->port.reg) return ch->port.reg(ch->port.dev, address, size, value, write);
    return 0;
}

/* The transfer of an INPUT or OUTPUT command; 1 when it finished, 0 when
 * the port stalled it, -1 on a bus error. `*bytes` counts what moved. */
static int transfer(mc_dbdma *ch, uint32_t *bytes) {
    const unsigned cmd = cmd_of(ch);
    const uint32_t req = req_of(ch);
    uint8_t buf[CHUNK];
    while (ch->done < req && *bytes < CHUNK * 4u) {
        const uint32_t address = ch->word[1] + ch->done;
        uint32_t n = req - ch->done;
        if (n > CHUNK) n = CHUNK;
        if (cmd <= MC_DBDMA_OUTPUT_LAST) {
            if (!ch->port.write) return 0;
            if (!mem(ch, address, buf, n, 0)) return -1;
            uint32_t k = 0;
            while (k < n && ch->port.write(ch->port.dev, buf[k])) k++;
            ch->done += k;
            *bytes += k;
            ch->moved += k;
            if (k < n) return 0;
        } else {
            if (!ch->port.read) return 0;
            uint32_t k = 0;
            int r = MC_DBDMA_BYTE;
            while (k < n && (r = ch->port.read(ch->port.dev, &buf[k])) == MC_DBDMA_BYTE) k++;
            if (k && !mem(ch, address, buf, k, 1)) return -1;
            ch->done += k;
            *bytes += k;
            ch->moved += k;
            if (r == MC_DBDMA_END) { ch->ended = 1; return 1; }
            if (k < n) return 0;
        }
    }
    return ch->done >= req;
}

int mc_dbdma_busy(const mc_dbdma *ch) {
    return (ch->status & MC_DBDMA_ACTIVE) && !(ch->status & MC_DBDMA_DEAD) && !ch->waiting;
}

int mc_dbdma_run(mc_dbdma *ch, unsigned budget) {
    uint32_t bytes = 0;
    while (budget && mc_dbdma_busy(ch)) {
        if (!ch->loaded) {
            uint8_t b[16];
            ch->status &= (uint16_t)~MC_DBDMA_WAKE;
            if (!mem(ch, ch->command, b, 16, 0)) { die(ch); return 0; }
            for (int i = 0; i < 4; i++) ch->word[i] = le32(b + 4 * i);
            ch->loaded = 1;
            ch->done = 0;
            ch->ended = 0;
        }
        budget--;
        const unsigned cmd = cmd_of(ch);
        switch (cmd) {
        case MC_DBDMA_OUTPUT_MORE: case MC_DBDMA_OUTPUT_LAST:
        case MC_DBDMA_INPUT_MORE: case MC_DBDMA_INPUT_LAST: {
            const int r = transfer(ch, &bytes);
            if (r < 0) { die(ch); return 0; }
            if (!r) return bytes >= CHUNK * 4u;     /* the port stalled it, or the byte budget ran out */
            break;
        }
        case MC_DBDMA_STORE_QUAD: {
            uint32_t v = ch->word[2];
            if (!quad(ch, ch->word[1], quad_size(req_of(ch)), &v, 1)) { die(ch); return 0; }
            break;
        }
        case MC_DBDMA_LOAD_QUAD: {
            const unsigned size = quad_size(req_of(ch));
            uint32_t v = 0;
            uint8_t b[4];
            if (!quad(ch, ch->word[1], size, &v, 0)) { die(ch); return 0; }
            if (size < 4) v &= (1u << (8 * size)) - 1u;
            ch->word[2] = v;
            put32(b, v);
            if (!mem(ch, ch->command + 8, b, 4, 1)) { die(ch); return 0; }
            break;
        }
        case MC_DBDMA_NOP:
            break;
        case MC_DBDMA_STOP:
            ch->status &= (uint16_t)~MC_DBDMA_ACTIVE;   /* 15.7.6: CommandPtr stays */
            ch->loaded = 0;
            ch->stopped = 1;
            return 0;
        default:                                         /* reserved: treated as a dead channel† */
            die(ch);
            return 0;
        }
        /* CHRP I/O 15.7.1: the record reaches the device before the
         * completion wait. A device may use that end to assert a status
         * bit which releases the wait; signal it once even if held. */
        if (cmd == MC_DBDMA_OUTPUT_LAST && !ch->ended) {
            ch->ended = 1;
            if (ch->port.end) ch->port.end(ch->port.dev);
        }
        if (!complete(ch)) return 0;
    }
    return mc_dbdma_busy(ch);
}

int mc_dbdma_stable(const mc_dbdma *ch, uint32_t offset, unsigned size) {
    return (offset & ~3u) == MC_DBDMA_STATUS && size <= 4u - (offset & 3u) &&
           ch->status_read_valid && ch->status_read == ch->status;
}

uint32_t mc_dbdma_read(mc_dbdma *ch, uint32_t offset, unsigned size) {
    uint32_t v;
    switch (offset & 0xFCu) {
    case MC_DBDMA_STATUS: v = ch->status; ch->status_read = ch->status; ch->status_read_valid = 1; break;
    case MC_DBDMA_COMMAND: v = ch->command; break;
    case MC_DBDMA_INT_SELECT: v = ch->select[0]; break;
    case MC_DBDMA_BRANCH_SELECT: v = ch->select[1]; break;
    case MC_DBDMA_WAIT_SELECT: v = ch->select[2]; break;
    default: v = 0; break;                  /* ChannelControl reads 0 (Table 322) */
    }
    v >>= 8 * (offset & 3u);
    return size == 4 ? v : v & ((1u << (8 * size)) - 1u);
}

static void control(mc_dbdma *ch, uint16_t mask, uint16_t data) {
    const uint16_t old = ch->status;
    mask &= WRITABLE;
    uint16_t s = (uint16_t)((old & ~mask) | (data & mask));
    if ((old & MC_DBDMA_RUN) && !(s & MC_DBDMA_RUN)) {
        /* Abort (Table 326, run): the transfer ends, status returns, the
         * command's interrupt if it asks; dead clears. */
        ch->status = s;
        if (ch->loaded && (old & MC_DBDMA_ACTIVE)) {
            write_status(ch, 0);
            if (test(ch->word[0] >> 20, condition(ch, 0))) interrupt(ch);
        }
        s = (uint16_t)(ch->status & ~(MC_DBDMA_ACTIVE | MC_DBDMA_DEAD | MC_DBDMA_WAKE | MC_DBDMA_FLUSH));
        ch->loaded = 0;
        ch->stopped = 0;
        ch->waiting = 0;
    } else if (s & MC_DBDMA_RUN) {
        if (!(old & MC_DBDMA_RUN)) { ch->stopped = 0; ch->loaded = 0; ch->waiting = 0; }
        if ((s & MC_DBDMA_WAKE) && ch->stopped) { ch->stopped = 0; ch->loaded = 0; }  /* 15.4.3 wake: refetch CommandPtr */
        if ((s & MC_DBDMA_PAUSE) || (s & MC_DBDMA_DEAD) || ch->stopped) s &= (uint16_t)~MC_DBDMA_ACTIVE;
        else s |= MC_DBDMA_ACTIVE;
    }
    ch->status = s;
    if ((s & MC_DBDMA_FLUSH)) {
        /* 15.7.1: a partial status of an input command, flush set. */
        if (ch->loaded && (cmd_of(ch) == MC_DBDMA_INPUT_MORE || cmd_of(ch) == MC_DBDMA_INPUT_LAST)) write_status(ch, 0);
        ch->status &= (uint16_t)~MC_DBDMA_FLUSH;
    }
    if (ch->waiting && !test(ch->word[0] >> 16, condition(ch, 2))) complete(ch);
}

int mc_dbdma_write(mc_dbdma *ch, uint32_t offset, unsigned size, uint32_t value) {
    const unsigned reg = offset & 0xFCu, shift = 8 * (offset & 3u);
    if (size < 4) value = (value & ((1u << (8 * size)) - 1u)) << shift;
    switch (reg) {
    case MC_DBDMA_CONTROL:
        control(ch, (uint16_t)(value >> 16), (uint16_t)value);
        break;
    case MC_DBDMA_COMMAND:
        if (!(ch->status & (MC_DBDMA_RUN | MC_DBDMA_ACTIVE))) ch->command = value & ~15u;
        break;
    case MC_DBDMA_INT_SELECT: case MC_DBDMA_BRANCH_SELECT: case MC_DBDMA_WAIT_SELECT:
        ch->select[(reg - MC_DBDMA_INT_SELECT) / 4] = value & 0x00FF00FFu;
        break;
    default:
        break;
    }
    return mc_dbdma_busy(ch);
}

int mc_dbdma_kick(mc_dbdma *ch) { return mc_dbdma_busy(ch); }

int mc_dbdma_set_bits(mc_dbdma *ch, uint8_t mask, uint8_t value) {
    ch->status = (uint16_t)((ch->status & ~mask) | (value & mask));
    if (ch->waiting && !test(ch->word[0] >> 16, condition(ch, 2))) complete(ch);
    return mc_dbdma_busy(ch);
}
