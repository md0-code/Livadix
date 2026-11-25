/* Chip unit tests: a fake machine host (src/hw/hw.h) that keeps guest time,
 * one deadline per device and the state of every output line, so a chip can
 * be driven through its registers alone. Every chip test links hw_test.c;
 * the standalone chip programs define main(), the mc_hw_tests suites are
 * run by tests/hw/main.c. */
#ifndef MC_HW_TEST_H
#define MC_HW_TEST_H

#include "../test.h"
#include "hw.h"

#include <stdint.h>

#define HW_FAKE_DEVS 8
#define HW_FAKE_LINES 32

typedef struct hw_fake {
    mc_hw_host host;
    uint64_t now;
    void *dev[HW_FAKE_DEVS];
    uint64_t deadline[HW_FAKE_DEVS];
    int line[HW_FAKE_DEVS][HW_FAKE_LINES];   /* last state per device and line, as 0/1 */
    unsigned changes[HW_FAKE_DEVS][HW_FAKE_LINES];   /* transitions seen */
    int value[HW_FAKE_DEVS][HW_FAKE_LINES];  /* last reported value (-1: never reported) */
    unsigned reports[HW_FAKE_DEVS][HW_FAKE_LINES];   /* reports seen */
    /* Called on every line report (wires one device's line to another). */
    void (*on_line)(struct hw_fake *f, void *dev, int line, int state);
    void *user;
} hw_fake;

/* A host at time 0 with no deadline armed. */
void hw_fake_init(hw_fake *f);
/* The deadline `dev` armed (MC_NEVER when none). */
uint64_t hw_fake_deadline(hw_fake *f, const void *dev);
/* State and transition count of a device's output line. */
int hw_fake_line(hw_fake *f, const void *dev, int line);
unsigned hw_fake_changes(hw_fake *f, const void *dev, int line);
/* The last value reported on a line (port lines carry a byte), -1 when the
 * line was never reported, and the number of reports. */
int hw_fake_value(hw_fake *f, const void *dev, int line);
unsigned hw_fake_reports(hw_fake *f, const void *dev, int line);
/* Moves time to `to`, delivering every deadline reached on the way (in time
 * order, through each device's ops->event; devices are mc_dev-headed). */
void hw_fake_advance(hw_fake *f, uint64_t to);
/* Advances by `periods` C15M periods. */
void hw_fake_step(hw_fake *f, uint64_t periods);
/* Writes a work/ path for test files: <MC_WORK>/tests/<suite>/<name>; the
 * folder is created. */
const char *hw_work_path(const char *suite, const char *name);

/* The mc_hw_tests suites (tests/hw/main.c). */
int mc_test_bus(void);
int mc_test_via(void);
int mc_test_rtc(void);
int mc_test_adb(void);
int mc_test_dafb(void);

#endif
