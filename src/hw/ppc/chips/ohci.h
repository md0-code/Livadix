/* Livadix's PCI OHCI USB host with one full-speed absolute HID pointer.
 * OpenHCI 1.0a (Compaq/Microsoft/National Semiconductor), chapters 4,
 * 6 and 7, appendix A; USB 1.1 chapter 9; HID 1.11 sections 6-8.
 * The PCI identity and the attached pointer are Livadix's own design.
 * No other emulator source was used. */
#ifndef MC_OHCI_H
#define MC_OHCI_H
#include "hw.h"
#include "pci.h"

typedef struct mc_ohci {
    mc_dev dev;
    mc_pci_config cfg;
    uint32_t reg[64];
    uint64_t next_frame, reset_due, resume_due, last_report;
    unsigned frame_fraction, done_delay;
    uint8_t address, configuration, idle, protocol, halted, pending, pending_value;
    uint8_t response[256], report[8];
    uint8_t report_queue[32][8], report_edge[32];
    unsigned report_read, report_count;
    unsigned response_size, response_offset, response_limit;
    int control_stall, report_pending, interrupt_polled;
} mc_ohci;

void mc_ohci_init(mc_ohci *u, const mc_hw_host *host);
void mc_ohci_reset(mc_ohci *u);
mc_pci_function mc_ohci_function(mc_ohci *u);
int mc_ohci_pointer_ready(const mc_ohci *u);
int mc_ohci_pointer(mc_ohci *u, int x, int y, unsigned width, unsigned height, int button);
#endif
