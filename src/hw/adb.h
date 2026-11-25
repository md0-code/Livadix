/* Livadix — the Apple Desktop Bus and its devices.
 *
 * From the Guide to the Macintosh Family Hardware, 2nd ed., chapter 8 "Apple
 * Desktop Bus": the command byte (address, Talk/Listen/Flush/SendReset,
 * register, Table 8-13), Talk answered within 260 us or a timeout (a device
 * with nothing to send does not answer register 0, but always answers
 * register 3), register 3 (Table 8-15: exceptional event, SRQ enable,
 * address, device handler ID), the reserved handler IDs (Table 8-17: $FE
 * moves a device that saw no collision, $FD on the activator, $00 sets
 * address and SRQ enable, $FF self-test), collision detection and address
 * relocation, Service Requests from devices with data, default addresses
 * (Table 8-16: keyboards 2, mice 3); the Apple Extended Keyboard (register
 * 0 with two key transitions, Table 8-10; register 2 modifiers and LEDs,
 * Table 8-11; handler 2, and 3 for the right-hand modifier codes $7B-$7D,
 * Figure 8-10) and the Apple Standard Mouse (register 0, Table 8-4: button
 * in bit 15, 0 = down, Y and X two's-complement counts; handlers 1 and 2,
 * 100 and 200 counts per inch).
 *
 * The Applejack, the Pippin's controller: Apple's Pippin Technical Note
 * PPBD-JHACEF003 "Applejack Input Device Driver" (version 003, 1996),
 * Figure 6 "ADB Register 0 Four-Byte Packet and Bit Number to Button
 * Mapping" (switch bits b17-b0, 0 = down, the 7-bit Y and X displacements
 * of the trackball) and its ADB address 3 (Figure 1). That the device
 * starts as a mouse with handler 1 (two bytes: the fire buttons in the two
 * button bits; another switch changing makes it answer without a change)
 * and sends the four bytes once its handler is $46 is as observed on the
 * hardware (lampmerchant, tashnotes, "Atmark Pippin Controller"). */
#ifndef MC_ADB_H
#define MC_ADB_H

#include <stdint.h>

#define MC_ADB_MAX_DEVICES 4
#define MC_ADB_KEY_QUEUE 64

typedef struct mc_adb_device mc_adb_device;

typedef struct mc_adb_device_ops {
    /* Data for Talk register 0-2 (register 3 is handled by the bus); returns
     * the byte count (2-8), 0 for no answer. */
    unsigned (*talk)(mc_adb_device *d, unsigned reg, uint8_t *out);
    void (*listen)(mc_adb_device *d, unsigned reg, const uint8_t *data, unsigned n);
    /* Whether a Talk register 0 would answer (the device's Service Request). */
    int (*pending)(const mc_adb_device *d);
    void (*reset)(mc_adb_device *d);
    /* Whether the device accepts this handler ID. */
    int (*handler_ok)(const mc_adb_device *d, uint8_t handler);
} mc_adb_device_ops;

struct mc_adb_device {
    const mc_adb_device_ops *ops;
    uint8_t default_address, address;
    uint8_t default_handler, handler;
    uint8_t srq_enable;
    uint8_t collided;           /* lost a collision since its last successful transmission */
};

typedef struct mc_adb_keyboard {
    mc_adb_device dev;
    uint8_t queue[MC_ADB_KEY_QUEUE];    /* transition codes, bit 7 = released */
    unsigned head, count;
    uint8_t down[128];
    uint8_t leds;                       /* register 2 bits 2-0 (0 = lit) */
} mc_adb_keyboard;

typedef struct mc_adb_mouse {
    mc_adb_device dev;
    int dx, dy;                         /* counts not reported yet */
    uint8_t button, reported_button;
} mc_adb_mouse;

/* The Applejack's switches, by the technical note's bit numbers. */
enum {
    MC_APPLEJACK_LEFT_FIRE = 1u << 17, MC_APPLEJACK_RIGHT_FIRE = 1u << 16,
    MC_APPLEJACK_GREEN = 1u << 15, MC_APPLEJACK_RED = 1u << 14,
    MC_APPLEJACK_DOWN = 1u << 13, MC_APPLEJACK_RIGHT = 1u << 12,
    MC_APPLEJACK_LEFT = 1u << 11, MC_APPLEJACK_UP = 1u << 10,
    MC_APPLEJACK_YELLOW = 1u << 9, MC_APPLEJACK_BLUE = 1u << 8,
    MC_APPLEJACK_DIAMOND = 1u << 2, MC_APPLEJACK_CIRCLE = 1u << 1, MC_APPLEJACK_SQUARE = 1u << 0,
    MC_APPLEJACK_SWITCHES = 0x3FF07u
};

typedef struct mc_adb_applejack {
    mc_adb_device dev;
    int dx, dy;                         /* trackball counts not reported yet */
    uint32_t down, reported;            /* MC_APPLEJACK_* held; as last sent */
} mc_adb_applejack;

typedef struct mc_adb_bus {
    mc_adb_device *dev[MC_ADB_MAX_DEVICES];
    unsigned count;
} mc_adb_bus;

void mc_adb_bus_init(mc_adb_bus *bus);
void mc_adb_attach(mc_adb_bus *bus, mc_adb_device *dev);
/* SendReset (or the bus reset at power-on): every device to its defaults. */
void mc_adb_reset(mc_adb_bus *bus);
/* A command: for Talk the answer's bytes go to `out` and the count is
 * returned (0: timeout); for Listen `data`/`n` is what the host sends. */
unsigned mc_adb_command(mc_adb_bus *bus, uint8_t command, const uint8_t *data, unsigned n, uint8_t *out);
/* A Service Request: a device other than the one at `address` has data
 * and its SRQ enabled. */
int mc_adb_srq(const mc_adb_bus *bus, unsigned address);

void mc_adb_keyboard_init(mc_adb_keyboard *k);
/* A key transition (Extended Keyboard code, as handler 3 sends it);
 * returns 0 when the queue is full. */
int mc_adb_keyboard_key(mc_adb_keyboard *k, uint8_t code, int down);
void mc_adb_mouse_init(mc_adb_mouse *m);
void mc_adb_mouse_move(mc_adb_mouse *m, int dx, int dy, int button);

void mc_adb_applejack_init(mc_adb_applejack *a);
/* The trackball moved. */
void mc_adb_applejack_move(mc_adb_applejack *a, int dx, int dy);
/* Switches (MC_APPLEJACK_*) pressed or released. */
void mc_adb_applejack_switch(mc_adb_applejack *a, uint32_t switches, int down);

#endif
