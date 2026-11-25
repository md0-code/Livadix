/* Livadix — MESH (Macintosh Enhanced SCSI Hardware, 343S1146), the fast SCSI
 * controller of the internal bus of the 7500, 8500, 9500, 7600, 8600 and
 * 9600, behind Grand Central with a DBDMA channel.
 *
 * Source: CHRP I/O Device Reference (Apple, IBM, Motorola, 1996), chapter
 * 12 "SCSI": Table 283 (sixteen byte registers at $10 steps: $00/$10
 * transfer count, $20 FIFO, $30 sequence, $40/$50 bus status 0/1, $60 FIFO
 * count, $70 exception, $80 error, $90 interrupt mask, $A0 interrupt, $B0
 * source ID, $C0 destination ID, $D0 synchronous parameters, $E0 MESH ID,
 * $F0 selection time-out), 12.4 (the controller's and its DBDMA channel's
 * interrupts wired together), 12.5.1-12.5.16: the transfer counter (0 means
 * 64 KB; decremented per byte written to the FIFO or moved by DMA for
 * output, per byte taken from the bus for input; the command done when it
 * and the FIFO count are 0), the 16-byte FIFO, the sequence register (bit 7
 * DMA, 6 target mode, 5 ATN, 4 active negation, bits 3-0 the command:
 * Table 289 Arbitrate 1, Select 2, Command 3, Status 4, DataOut 5, DataIn 6,
 * MsgOut 7, MsgIn 8, BusFree 9, EnParChk A, DisParChk B, EnReSel C,
 * DisReSel D, RstMESH E, FlushFIFO F; Table 290), bus status 0 (7 REQ32, 6
 * ACK32, 5 REQ, 4 ACK, 3 ATN, 2 MSG, 1 C/D, 0 I/O) and 1 (7 RST, 6 BSY, 5
 * SEL), the exception register (5 SelWAtn, 4 Selected, 3 Reselected, 2
 * ArbLost, 1 PhaseMM, 0 SelTO; cleared by writing ones), the error register
 * (6 UnExpDisc, 5 SCSIRst, 4 SeqErr, 3-0 parity), the interrupt mask and
 * interrupt registers (2 error, 1 exception, 0 CmdDone; CmdDone cleared by
 * the next command or by writing 1; after a reset CmdDone is set and the
 * mask is 0), the MESH ID (bits 4-0 the version), the selection time-out
 * (10 ms per count at 50 MHz); Table 290's commands (the information
 * transfer commands run until the count is exhausted, a phase mismatch or
 * an error; ACK stays asserted after an input command until the next one;
 * BusFree waits for BSY to drop, a REQ first being a phase mismatch;
 * RstMESH resets the controller, not the bus (bus status 1 bit 7 does);
 * EnParChk, DisParChk, EnReSel and DisReSel generate no interrupt).
 * Power Macintosh 7500 and 8500 Developer Note (Apple, 1995), "MESH
 * High-Speed SCSI Interface" (internal bus, up to 10 MB/s). Open Firmware's
 * mesh FCode in ROM 9630C68B (file $346DBC): "AAPL,343S1146", registers
 * $100 bytes at the I/O offset, clock-frequency 50 MHz.
 *
 * As ROM 9630C68B's native SCSI SIM uses it: the MESH ID register reads
 * $E2 (the SIM takes the chip for a MESH at $E1 or above, $FFEBBBEC);
 * after Status it waits for CmdDone, then for REQ to drop, before MsgIn
 * (RAM copy at $001D7904): an input command is done when its count is, and
 * ACK stays held after every input until the next command.
 *
 * Livadix's model: the controller is the bus's initiator (scsi_bus.h); targets
 * answer at once, so a command's bus work happens when it is issued (or as
 * the FIFO and DMA move its bytes) and its interrupt comes `latency` C15M
 * periods later. Selection that nobody answers ends after the time-out
 * register's time. Target mode, reselection, synchronous timing and parity
 * errors are not modelled (nothing selects the Macintosh; targets are
 * asynchronous here). The register index is the ops' offset (the board
 * attaches the $100-byte window with shift 4).
 *
 * DMA: the DBDMA channel's stream (mc_mesh_dma_read / mc_mesh_dma_write);
 * MC_MESH_LINE_DRQ rises when a DMA command can move data. An input whose
 * target leaves the phase before the count ends answers MC_DBDMA_END after
 * the exception.
 *
 * Output lines (1 = asserted): MC_MESH_LINE_IRQ, MC_MESH_LINE_DRQ.
 * MC_MESH_LINE_DMA_STATUS carries active-low CmdDone/Exception/Error in
 * DBDMA s5/s6/s7, independent of InterruptMask (Apple AppleMesh mesh.cpp,
 * initCP, https://github.com/apple-oss-distributions/AppleMesh).
 *
 * Livadix's own code. */
#ifndef MC_MESH_H
#define MC_MESH_H

#include "hw.h"
#include "scsi_bus.h"

#include <stdint.h>

enum { MC_MESH_LINE_IRQ = 0, MC_MESH_LINE_DRQ = 1, MC_MESH_LINE_DMA_STATUS = 2 };

enum {
    MC_MESH_COUNT0, MC_MESH_COUNT1, MC_MESH_FIFO, MC_MESH_SEQUENCE, MC_MESH_STATUS0, MC_MESH_STATUS1,
    MC_MESH_FIFO_COUNT, MC_MESH_EXCEPTION, MC_MESH_ERROR, MC_MESH_INT_MASK, MC_MESH_INTERRUPT,
    MC_MESH_SOURCE_ID, MC_MESH_DEST_ID, MC_MESH_SYNC, MC_MESH_ID, MC_MESH_SEL_TIMEOUT
};
enum {
    MC_MESH_CMD_ARBITRATE = 1, MC_MESH_CMD_SELECT, MC_MESH_CMD_COMMAND, MC_MESH_CMD_STATUS, MC_MESH_CMD_DATA_OUT,
    MC_MESH_CMD_DATA_IN, MC_MESH_CMD_MSG_OUT, MC_MESH_CMD_MSG_IN, MC_MESH_CMD_BUS_FREE, MC_MESH_CMD_EN_PARITY,
    MC_MESH_CMD_DIS_PARITY, MC_MESH_CMD_EN_RESEL, MC_MESH_CMD_DIS_RESEL, MC_MESH_CMD_RESET, MC_MESH_CMD_FLUSH
};
enum { MC_MESH_SEQ_DMA = 0x80, MC_MESH_SEQ_TMODE = 0x40, MC_MESH_SEQ_ATN = 0x20 };
enum { MC_MESH_EXC_SELTO = 0x01, MC_MESH_EXC_PHASE = 0x02, MC_MESH_EXC_ARBLOST = 0x04 };
enum { MC_MESH_ERR_PARITY = 0x01, MC_MESH_ERR_SEQ = 0x10, MC_MESH_ERR_RESET = 0x20, MC_MESH_ERR_DISCONNECT = 0x40 };
enum { MC_MESH_INT_DONE = 0x01, MC_MESH_INT_EXCEPTION = 0x02, MC_MESH_INT_ERROR = 0x04 };

typedef struct mc_mesh {
    mc_dev dev;
    mc_scsi_bus *bus;
    uint8_t id_version;             /* the MESH ID register */
    uint32_t latency;               /* C15M periods from bus completion to the interrupt */
    uint32_t tc;                    /* the transfer counter, 0-65536 */
    uint8_t fifo[16];
    unsigned fifo_n;
    uint8_t sequence, exception, error, mask, done;
    uint8_t source, dest, sync, sel_timeout, status1_out;
    int command;                    /* the information transfer command running (0: none) */
    int ack_held;                   /* ACK left asserted after an input */
    int owner;                      /* the controller won arbitration */
    int pending;                    /* a completion waits for its deadline */
    uint8_t pend_done, pend_exception, pend_error;
    int irq, drq;
    uint8_t dma_status;
} mc_mesh;

extern const mc_dev_ops mc_mesh_ops;

/* A controller on `bus`, its ID register `id_version` (bits 4-0 the
 * version), reset. */
void mc_mesh_init(mc_mesh *m, const mc_hw_host *host, const char *name, mc_scsi_bus *bus, uint8_t id_version);
void mc_mesh_reset(mc_mesh *m);
/* The DBDMA channel's stream (mc_dbdma_port read and write). */
int mc_mesh_dma_read(void *m, uint8_t *byte);
int mc_mesh_dma_write(void *m, uint8_t byte);

#endif
