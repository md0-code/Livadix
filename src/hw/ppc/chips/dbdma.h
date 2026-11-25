/* Livadix — a DBDMA channel: descriptor-based DMA, the bus-master engine of
 * Grand Central (grand_central.h) and of the later Macintosh I/O
 * controllers, one channel per data stream.
 *
 * Source: CHRP I/O Device Reference (Apple, IBM, Motorola, 1996), chapter
 * 15 "Descriptor-Based DMA": 15.4 the channel registers (Table 321: $00
 * ChannelControl, $04 ChannelStatus, $0C CommandPtrLo, $10 InterruptSelect,
 * $14 BranchSelect, $18 WaitSelect; Table 322 their read values and write
 * effects: CommandPtrLo stored only while run and active are 0), Table 324
 * (ChannelControl: bits 31-16 a mask of the status bits bits 15-0 write),
 * Table 326 (ChannelStatus: 15 run, 14 pause, 13 flush, 12 wake, 11 dead,
 * 10 active, 8 bt, 7-0 s7-s0), Tables 330 and 337-342 (the interrupt,
 * branch and wait conditions: (s7..s0 & mask) == (value & mask), mask in
 * bits 23-16 and value in bits 7-0 of the select registers; the i, b and w
 * fields: 0 never, 1 if the condition is set, 2 if clear, 3 always); 15.7
 * the 16-byte commands (Table 333: reqCount bits 15-0, w 17-16, b 19-18,
 * i 21-20, key 26-24, cmd 31-28 of the first little-endian word; then
 * address, cmdDep and the xferStatus/resCount word), Table 334 (OUTPUT_MORE
 * 0, OUTPUT_LAST 1, INPUT_MORE 2, INPUT_LAST 3, STORE_QUAD 4, LOAD_QUAD 5,
 * NOP 6, STOP 7), Tables 335-336 (key: STREAM0-3 0-3, REGS 5, SYSTEM 6,
 * DEVICE 7), 15.7.3-15.7.4 (STORE_QUAD and LOAD_QUAD: 1, 2 or 4 bytes,
 * Table 345's count mapping, data32 in cmdDep), 15.7.6 (STOP leaves
 * CommandPtr on itself; wake refetches it), 15.6.2 (an unrecoverable bus
 * error sets dead and ends the channel; a hardwired interrupt follows),
 * 15.7.1 (xferStatus is the ChannelStatus with active set, written with
 * resCount at completion; a wait comes before the completion sequence;
 * the flush bit forces a partial status of an input command).
 * "Since the devices ... are PCI devices, only little endian addressing
 * for DBDMA is supported" (15.3): registers and commands are little-endian.
 *
 * Livadix's model: a transfer moves bytes between memory (the owner's bus
 * master path) and the device's stream port as fast as the port gives or
 * takes them; when the port has no byte (or no room) the channel waits for
 * mc_dbdma_kick, which the owner calls when the device asks for service
 * (its DMA request). An input device ends a record early by answering
 * MC_DBDMA_END (the INPUT command completes with its residual count). The
 * channel works without guest time passing; mc_dbdma_run stops after a
 * budget of commands and bytes so a channel program that loops on itself
 * cannot hold the machine (the owner continues it from an event).
 * Only key STREAM0-3 (all one stream here), REGS, SYSTEM and DEVICE (when
 * the port has a register path) are served.
 *
 * Livadix's own code. */
#ifndef MC_DBDMA_H
#define MC_DBDMA_H

#include <stdint.h>

/* Registers (Table 321). */
enum {
    MC_DBDMA_CONTROL = 0x00, MC_DBDMA_STATUS = 0x04, MC_DBDMA_COMMAND_HI = 0x08, MC_DBDMA_COMMAND = 0x0C,
    MC_DBDMA_INT_SELECT = 0x10, MC_DBDMA_BRANCH_SELECT = 0x14, MC_DBDMA_WAIT_SELECT = 0x18
};
#define MC_DBDMA_SIZE 0x100u            /* a channel's register block */

/* ChannelStatus (Table 326). */
enum {
    MC_DBDMA_RUN = 0x8000, MC_DBDMA_PAUSE = 0x4000, MC_DBDMA_FLUSH = 0x2000, MC_DBDMA_WAKE = 0x1000,
    MC_DBDMA_DEAD = 0x0800, MC_DBDMA_ACTIVE = 0x0400, MC_DBDMA_BT = 0x0100
};

/* Commands (Table 334) and keys (Table 335). */
enum {
    MC_DBDMA_OUTPUT_MORE, MC_DBDMA_OUTPUT_LAST, MC_DBDMA_INPUT_MORE, MC_DBDMA_INPUT_LAST,
    MC_DBDMA_STORE_QUAD, MC_DBDMA_LOAD_QUAD, MC_DBDMA_NOP, MC_DBDMA_STOP
};
enum { MC_DBDMA_KEY_REGS = 5, MC_DBDMA_KEY_SYSTEM = 6, MC_DBDMA_KEY_DEVICE = 7 };

/* What a port's read returns. */
enum { MC_DBDMA_NONE = 0, MC_DBDMA_BYTE = 1, MC_DBDMA_END = 2 };

/* The device's stream. */
typedef struct mc_dbdma_port {
    void *dev;
    /* INPUT: the next byte from the device: MC_DBDMA_BYTE with *byte,
     * MC_DBDMA_NONE when it has none now, MC_DBDMA_END when its record
     * ended (no byte). NULL: the device has no input. */
    int (*read)(void *dev, uint8_t *byte);
    /* OUTPUT: 1 when the device took the byte, 0 when it cannot now. */
    int (*write)(void *dev, uint8_t byte);
    /* KEY_DEVICE quads: the device's register at `address` (NULL: none). */
    int (*reg)(void *dev, uint32_t address, unsigned size, uint32_t *value, int write);
    /* OUTPUT_LAST's data all went to the device: the record (a frame)
     * ends there (NULL: the device takes a stream). */
    void (*end)(void *dev);
} mc_dbdma_port;

/* What the channel needs of its owner. */
typedef struct mc_dbdma_host {
    void *ctx;
    /* Bus master access to memory, as mc_hw_host.dma (0 on a bus error). */
    int (*dma)(void *ctx, uint32_t address, void *data, uint32_t length, int write);
    /* The channel's hardwired interrupt (a pulse: the owner latches it). */
    void (*interrupt)(void *ctx, unsigned channel);
} mc_dbdma_host;

typedef struct mc_dbdma {
    const mc_dbdma_host *host;
    unsigned index;                 /* the channel number the owner gives it */
    mc_dbdma_port port;
    uint16_t status;
    uint16_t status_read;
    int status_read_valid;
    uint32_t command;               /* CommandPtrLo */
    uint32_t select[3];             /* interrupt, branch, wait select */
    /* The command in progress. */
    int loaded;                     /* the descriptor at `command` is fetched */
    uint32_t word[4];
    uint32_t done;                  /* bytes transferred */
    int waiting;                    /* held by its wait condition */
    int ended;                      /* the device ended the record */
    int stopped;                    /* idle after a STOP until wake */
    uint32_t moved;                 /* bytes moved since reset (diagnostics) */
} mc_dbdma;

void mc_dbdma_init(mc_dbdma *ch, const mc_dbdma_host *host, unsigned index, const mc_dbdma_port *port);
void mc_dbdma_reset(mc_dbdma *ch);
void mc_dbdma_set_port(mc_dbdma *ch, const mc_dbdma_port *port);
/* Register access (offset within the channel's block; little-endian
 * value, 1, 2 or 4 bytes). Returns 1 from a write when the channel has
 * work to continue (mc_dbdma_run). */
int mc_dbdma_stable(const mc_dbdma *, uint32_t offset, unsigned size);
uint32_t mc_dbdma_read(mc_dbdma *ch, uint32_t offset, unsigned size);
int mc_dbdma_write(mc_dbdma *ch, uint32_t offset, unsigned size, uint32_t value);
/* The device has a byte to give or room to take one (its DMA request). */
int mc_dbdma_kick(mc_dbdma *ch);
/* Hardwired status bits s7-s0 (the device or board sets them). */
int mc_dbdma_set_bits(mc_dbdma *ch, uint8_t mask, uint8_t value);
/* Runs the channel for up to `budget` commands (and budget x 4 KB of
 * data); 1 when it stopped with work left. */
int mc_dbdma_run(mc_dbdma *ch, unsigned budget);
/* 1 when the channel is running a command it can progress on now. */
int mc_dbdma_busy(const mc_dbdma *ch);

#endif
