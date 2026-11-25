/* Livadix - the Am29F010 flash memory (am29f010.h has the sources).
 * Livadix's own code. */
#include "am29f010.h"

#include <string.h>

/* Where the command sequence stands (data sheet, table 4). */
enum {
    S_READ,         /* reading the array */
    S_UNLOCK1,      /* $AA to $5555 seen */
    S_UNLOCK2,      /* then $55 to $2AAA: the command comes next */
    S_AUTOSELECT,
    S_PROGRAM,      /* $A0: the next write programs a byte */
    S_ERASE,        /* $80: the second unlock comes next */
    S_ERASE1,
    S_ERASE2        /* then $10 to $5555 or $30 to a sector */
};

void mc_am29f010_init(mc_am29f010 *f) {
    memset(f->data, 0xFF, sizeof f->data);
    f->state = S_READ;
    f->dirty = 0;
}

void mc_am29f010_reset(mc_am29f010 *f) { f->state = S_READ; }

uint8_t mc_am29f010_read(const mc_am29f010 *f, uint32_t address) {
    address &= MC_AM29F010_SIZE - 1u;
    if (f->state == S_AUTOSELECT) {
        switch (address & 0xFFu) {
        case 0: return 0x01;        /* AMD */
        case 1: return 0x20;        /* Am29F010 */
        default: return 0;          /* the sector is not protected */
        }
    }
    return f->data[address];
}

void mc_am29f010_write(mc_am29f010 *f, uint32_t address, uint8_t value) {
    address &= MC_AM29F010_SIZE - 1u;
    const uint32_t command = address & 0x7FFFu;
    if (f->state == S_PROGRAM) {
        const uint8_t next = f->data[address] & value;
        if (next != f->data[address]) {
            f->data[address] = next;
            f->dirty = 1;
        }
        f->state = S_READ;
        return;
    }
    if (value == 0xF0 && f->state != S_ERASE2) {
        f->state = S_READ;
        return;
    }
    switch (f->state) {
    case S_READ:
    case S_AUTOSELECT:
        if (command == 0x5555u && value == 0xAA) f->state = S_UNLOCK1;
        break;
    case S_UNLOCK1:
        f->state = command == 0x2AAAu && value == 0x55 ? S_UNLOCK2 : S_READ;
        break;
    case S_UNLOCK2:
        if (command != 0x5555u) f->state = S_READ;
        else if (value == 0x90) f->state = S_AUTOSELECT;
        else if (value == 0xA0) f->state = S_PROGRAM;
        else if (value == 0x80) f->state = S_ERASE;
        else f->state = S_READ;
        break;
    case S_ERASE:
        f->state = command == 0x5555u && value == 0xAA ? S_ERASE1 : S_READ;
        break;
    case S_ERASE1:
        f->state = command == 0x2AAAu && value == 0x55 ? S_ERASE2 : S_READ;
        break;
    case S_ERASE2:
        if (value == 0x10 && command == 0x5555u) {
            memset(f->data, 0xFF, sizeof f->data);
            f->dirty = 1;
        } else if (value == 0x30) {
            memset(f->data + (address & ~(MC_AM29F010_SECTOR - 1u)), 0xFF, MC_AM29F010_SECTOR);
            f->dirty = 1;
        }
        f->state = S_READ;
        break;
    default:
        f->state = S_READ;
        break;
    }
}
