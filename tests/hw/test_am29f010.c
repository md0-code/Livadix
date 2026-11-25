/* The Am29F010 flash memory against its data sheet's command definitions
 * (am29f010.h): autoselect, byte programming, sector and chip erase, reset,
 * and sequences that are not commands. */
#include "hw_test.h"
#include "am29f010.h"
#include <stdio.h>

static mc_am29f010 flash;
static unsigned checks;
#define T(x) do { CHECK(x); checks++; } while (0)

static void unlock(void) {
    mc_am29f010_write(&flash, 0x5555, 0xAA);
    mc_am29f010_write(&flash, 0x2AAA, 0x55);
}
static void command(uint8_t value) {
    unlock();
    mc_am29f010_write(&flash, 0x5555, value);
}
static void program(uint32_t address, uint8_t value) {
    command(0xA0);
    mc_am29f010_write(&flash, address, value);
}

int main(void) {
    mc_am29f010_init(&flash);
    T(mc_am29f010_read(&flash, 0) == 0xFF && mc_am29f010_read(&flash, MC_AM29F010_SIZE - 1) == 0xFF && !flash.dirty);

    /* Autoselect: manufacturer and device, until the reset command. */
    command(0x90);
    T(mc_am29f010_read(&flash, 0) == 0x01 && mc_am29f010_read(&flash, 1) == 0x20 && mc_am29f010_read(&flash, 2) == 0);
    T(mc_am29f010_read(&flash, 0x4000) == 0x01 && mc_am29f010_read(&flash, 0x4001) == 0x20);
    mc_am29f010_write(&flash, 0, 0xF0);
    T(mc_am29f010_read(&flash, 0) == 0xFF && !flash.dirty);

    /* A write without the sequence changes nothing. */
    mc_am29f010_write(&flash, 0x1234, 0x00);
    T(mc_am29f010_read(&flash, 0x1234) == 0xFF && !flash.dirty);
    /* Nor does a sequence with a wrong address or data. */
    mc_am29f010_write(&flash, 0x5555, 0x55);
    mc_am29f010_write(&flash, 0x2AAA, 0xAA);
    mc_am29f010_write(&flash, 0x5555, 0x09);
    T(mc_am29f010_read(&flash, 0) == 0xFF && mc_am29f010_read(&flash, 1) == 0xFF);
    mc_am29f010_write(&flash, 0x5555, 0xAA);
    mc_am29f010_write(&flash, 0x2AAB, 0x55);
    mc_am29f010_write(&flash, 0x5555, 0xA0);
    mc_am29f010_write(&flash, 0x10, 0x00);
    T(mc_am29f010_read(&flash, 0x10) == 0xFF && !flash.dirty);

    /* Programming clears bits, one byte per command. */
    program(0x130C, 0x4E);
    T(mc_am29f010_read(&flash, 0x130C) == 0x4E && flash.dirty);
    mc_am29f010_write(&flash, 0x130D, 0x75);
    T(mc_am29f010_read(&flash, 0x130D) == 0xFF);
    program(0x130C, 0xF1);
    T(mc_am29f010_read(&flash, 0x130C) == 0x40);
    /* Only A14-A0 select the command addresses: the sequence works from
     * any sector. */
    mc_am29f010_write(&flash, 0x1D555, 0xAA);
    mc_am29f010_write(&flash, 0x1AAAA, 0x55);
    mc_am29f010_write(&flash, 0x1D555, 0xA0);
    mc_am29f010_write(&flash, 0x1FFFF, 0x5A);
    T(mc_am29f010_read(&flash, 0x1FFFF) == 0x5A);
    program(0x4000, 0x11);
    program(0x7FFF, 0x22);
    program(0x8000, 0x33);

    /* Sector erase: one 16 KiB sector goes back to $FF. */
    flash.dirty = 0;
    command(0x80);
    unlock();
    mc_am29f010_write(&flash, 0x5ABC, 0x30);
    T(mc_am29f010_read(&flash, 0x4000) == 0xFF && mc_am29f010_read(&flash, 0x7FFF) == 0xFF && flash.dirty);
    T(mc_am29f010_read(&flash, 0x130C) == 0x40 && mc_am29f010_read(&flash, 0x8000) == 0x33 && mc_am29f010_read(&flash, 0x1FFFF) == 0x5A);
    /* An erase setup without its second half erases nothing. */
    command(0x80);
    mc_am29f010_write(&flash, 0x8000, 0x30);
    T(mc_am29f010_read(&flash, 0x8000) == 0x33);

    /* Reset keeps the array and leaves autoselect. */
    command(0x90);
    mc_am29f010_reset(&flash);
    T(mc_am29f010_read(&flash, 0) == 0xFF && mc_am29f010_read(&flash, 0x130C) == 0x40);

    /* Chip erase. */
    command(0x80);
    command(0x10);
    T(mc_am29f010_read(&flash, 0x130C) == 0xFF && mc_am29f010_read(&flash, 0x8000) == 0xFF && mc_am29f010_read(&flash, 0x1FFFF) == 0xFF);

    printf("Am29F010: %u checks, %d failures\n", checks, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
