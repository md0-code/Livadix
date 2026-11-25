/* Livadix — the Apple Desktop Bus and its devices (see adb.h for sources).
 * Livadix's own code. */
#include "adb.h"

#include <string.h>

void mc_adb_bus_init(mc_adb_bus *bus) { memset(bus, 0, sizeof *bus); }

void mc_adb_attach(mc_adb_bus *bus, mc_adb_device *dev) {
    if (bus->count < MC_ADB_MAX_DEVICES) bus->dev[bus->count++] = dev;
}

static void device_reset(mc_adb_device *d) {
    d->address = d->default_address;
    d->handler = d->default_handler;
    d->srq_enable = 1;
    d->collided = 0;
    if (d->ops->reset) d->ops->reset(d);
}

void mc_adb_reset(mc_adb_bus *bus) {
    for (unsigned i = 0; i < bus->count; i++) device_reset(bus->dev[i]);
}

int mc_adb_srq(const mc_adb_bus *bus, unsigned address) {
    for (unsigned i = 0; i < bus->count; i++) {
        const mc_adb_device *d = bus->dev[i];
        if (d->address != address && d->srq_enable && d->ops->pending(d)) return 1;
    }
    return 0;
}

/* Listen register 3 (Table 8-17). */
static void listen3(mc_adb_device *d, const uint8_t *data, unsigned n) {
    if (n < 2) return;
    const uint8_t address = data[0] & 0x0Fu, handler = data[1];
    switch (handler) {
    case 0xFF: break;                           /* self-test: nothing to do */
    case 0xFE:                                  /* move unless it lost a collision */
        if (!d->collided) d->address = address;
        d->collided = 0;
        break;
    case 0xFD: break;                           /* move on the activator: never pressed here */
    case 0x00:
        d->address = address;
        d->srq_enable = (data[0] >> 5) & 1u;
        break;
    default:
        if (d->ops->handler_ok && d->ops->handler_ok(d, handler)) d->handler = handler;
        break;
    }
}

unsigned mc_adb_command(mc_adb_bus *bus, uint8_t command, const uint8_t *data, unsigned n, uint8_t *out) {
    const unsigned address = command >> 4, reg = command & 3u;
    switch ((command >> 2) & 3u) {
    case 0:
        if ((command & 0x0Fu) == 0x00) mc_adb_reset(bus);       /* SendReset */
        else if ((command & 0x0Fu) == 0x01) {                   /* Flush */
            for (unsigned i = 0; i < bus->count; i++)
                if (bus->dev[i]->address == address && bus->dev[i]->ops->reset) bus->dev[i]->ops->reset(bus->dev[i]);
        }
        return 0;
    case 2:                                                     /* Listen */
        for (unsigned i = 0; i < bus->count; i++) {
            mc_adb_device *d = bus->dev[i];
            if (d->address != address) continue;
            if (reg == 3) listen3(d, data, n);
            else if (d->ops->listen) d->ops->listen(d, reg, data, n);
        }
        return 0;
    case 3: {                                                   /* Talk */
        unsigned answered = 0;
        for (unsigned i = 0; i < bus->count; i++) {
            mc_adb_device *d = bus->dev[i];
            if (d->address != address) continue;
            uint8_t buf[8];
            unsigned got;
            if (reg == 3) {
                buf[0] = (uint8_t)(0x40u | (d->srq_enable ? 0x20u : 0) | d->address);
                buf[1] = d->handler;
                got = 2;
            } else {
                if (answered) {
                    /* Another device at this address already holds the bus:
                     * this one sees the collision and keeps its data. */
                    if (d->ops->pending(d)) d->collided = 1;
                    continue;
                }
                got = d->ops->talk(d, reg, buf);
            }
            if (!got) continue;
            if (answered) {
                d->collided = 1;                                /* lost the collision */
                continue;
            }
            memcpy(out, buf, got);
            answered = got;
            d->collided = 0;
        }
        return answered;
    }
    default:
        return 0;                                               /* reserved */
    }
}

/* --- the Apple Extended Keyboard ------------------------------------------------ */

static int keyboard_pending(const mc_adb_device *d) { return ((const mc_adb_keyboard *)d)->count != 0; }

/* With handler 2 the right-hand Shift, Option and Control send the
 * left-hand codes (Figure 8-10). */
static uint8_t keyboard_code(const mc_adb_keyboard *k, uint8_t event) {
    if (k->dev.handler == 3) return event;
    const uint8_t code = event & 0x7Fu, up = event & 0x80u;
    if (code == 0x7B) return (uint8_t)(0x38u | up);
    if (code == 0x7C) return (uint8_t)(0x3Au | up);
    if (code == 0x7D) return (uint8_t)(0x36u | up);
    return event;
}

static int key_down(const mc_adb_keyboard *k, uint8_t code) { return k->down[code & 0x7F]; }

static unsigned keyboard_talk(mc_adb_device *d, unsigned reg, uint8_t *out) {
    mc_adb_keyboard *k = (mc_adb_keyboard *)d;
    if (reg == 0) {
        if (!k->count) return 0;
        for (unsigned i = 0; i < 2; i++) {
            if (k->count) {
                out[i] = keyboard_code(k, k->queue[k->head]);
                k->head = (k->head + 1) % MC_ADB_KEY_QUEUE;
                k->count--;
            } else out[i] = 0xFF;
        }
        return 2;
    }
    if (reg == 2) {
        /* Table 8-11: a 0 bit is a key held or an LED lit. */
        uint16_t v = 0xFFFFu;
        if (key_down(k, 0x33)) v &= ~(uint16_t)(1u << 14);     /* Delete */
        if (key_down(k, 0x39)) v &= ~(uint16_t)(1u << 13);     /* Caps Lock */
        if (key_down(k, 0x7F)) v &= ~(uint16_t)(1u << 12);     /* Reset (power key) */
        if (key_down(k, 0x36) || key_down(k, 0x7D)) v &= ~(uint16_t)(1u << 11);
        if (key_down(k, 0x38) || key_down(k, 0x7B)) v &= ~(uint16_t)(1u << 10);
        if (key_down(k, 0x3A) || key_down(k, 0x7C)) v &= ~(uint16_t)(1u << 9);
        if (key_down(k, 0x37)) v &= ~(uint16_t)(1u << 8);
        if (key_down(k, 0x47)) v &= ~(uint16_t)(1u << 7);      /* Num Lock/Clear */
        if (key_down(k, 0x6B)) v &= ~(uint16_t)(1u << 6);      /* Scroll Lock */
        v = (uint16_t)((v & ~7u) | (k->leds & 7u));
        out[0] = (uint8_t)(v >> 8);
        out[1] = (uint8_t)v;
        return 2;
    }
    return 0;
}

static void keyboard_listen(mc_adb_device *d, unsigned reg, const uint8_t *data, unsigned n) {
    mc_adb_keyboard *k = (mc_adb_keyboard *)d;
    if (reg == 2 && n >= 2) k->leds = data[1] & 7u;
}

static void keyboard_reset(mc_adb_device *d) {
    mc_adb_keyboard *k = (mc_adb_keyboard *)d;
    k->head = k->count = 0;
    k->leds = 7;
}

static int keyboard_handler_ok(const mc_adb_device *d, uint8_t handler) {
    (void)d;
    return handler == 2 || handler == 3;
}

static const mc_adb_device_ops keyboard_ops = {keyboard_talk, keyboard_listen, keyboard_pending, keyboard_reset, keyboard_handler_ok};

void mc_adb_keyboard_init(mc_adb_keyboard *k) {
    memset(k, 0, sizeof *k);
    k->dev.ops = &keyboard_ops;
    k->dev.default_address = 2;
    k->dev.default_handler = 2;
    device_reset(&k->dev);
}

int mc_adb_keyboard_key(mc_adb_keyboard *k, uint8_t code, int down) {
    code &= 0x7Fu;
    if (k->down[code] == (down != 0)) return 1;
    /* The power key sends $7F in both bytes (Figure 8-10). */
    const unsigned need = code == 0x7F ? 2u : 1u;
    if (k->count + need > MC_ADB_KEY_QUEUE) return 0;
    k->down[code] = (uint8_t)(down != 0);
    for (unsigned i = 0; i < need; i++) {
        k->queue[(k->head + k->count) % MC_ADB_KEY_QUEUE] = (uint8_t)(code | (down ? 0u : 0x80u));
        k->count++;
    }
    return 1;
}

/* --- the Apple Standard Mouse ---------------------------------------------------- */

static int mouse_pending(const mc_adb_device *d) {
    const mc_adb_mouse *m = (const mc_adb_mouse *)d;
    return m->dx || m->dy || m->button != m->reported_button;
}

static int clamp63(int v) { return v > 63 ? 63 : v < -64 ? -64 : v; }

static unsigned mouse_talk(mc_adb_device *d, unsigned reg, uint8_t *out) {
    mc_adb_mouse *m = (mc_adb_mouse *)d;
    if (reg != 0 || !mouse_pending(d)) return 0;
    const int dx = clamp63(m->dx), dy = clamp63(m->dy);
    m->dx -= dx;
    m->dy -= dy;
    m->reported_button = m->button;
    out[0] = (uint8_t)((m->button ? 0u : 0x80u) | ((unsigned)dy & 0x7Fu));
    out[1] = (uint8_t)(0x80u | ((unsigned)dx & 0x7Fu));
    return 2;
}

static void mouse_reset(mc_adb_device *d) {
    mc_adb_mouse *m = (mc_adb_mouse *)d;
    m->dx = m->dy = 0;
    m->reported_button = m->button;
}

static int mouse_handler_ok(const mc_adb_device *d, uint8_t handler) {
    (void)d;
    return handler == 1 || handler == 2;
}

static const mc_adb_device_ops mouse_ops = {mouse_talk, NULL, mouse_pending, mouse_reset, mouse_handler_ok};

void mc_adb_mouse_init(mc_adb_mouse *m) {
    memset(m, 0, sizeof *m);
    m->dev.ops = &mouse_ops;
    m->dev.default_address = 3;
    m->dev.default_handler = 1;
    device_reset(&m->dev);
}

/* --- the Applejack ------------------------------------------------------------ */

static int applejack_pending(const mc_adb_device *d) {
    const mc_adb_applejack *a = (const mc_adb_applejack *)d;
    return a->dx || a->dy || a->down != a->reported;
}

static unsigned applejack_talk(mc_adb_device *d, unsigned reg, uint8_t *out) {
    mc_adb_applejack *a = (mc_adb_applejack *)d;
    if (reg != 0 || !applejack_pending(d)) return 0;
    const int dx = clamp63(a->dx), dy = clamp63(a->dy);
    a->dx -= dx;
    a->dy -= dy;
    a->reported = a->down;
    const uint32_t up = ~a->down;       /* a switch reads 0 while it is down */
    out[0] = (uint8_t)(((up & MC_APPLEJACK_LEFT_FIRE) ? 0x80u : 0u) | ((unsigned)dy & 0x7Fu));
    out[1] = (uint8_t)(((up & MC_APPLEJACK_RIGHT_FIRE) ? 0x80u : 0u) | ((unsigned)dx & 0x7Fu));
    if (d->handler != 0x46) return 2;
    out[2] = (uint8_t)(up >> 8);
    out[3] = (uint8_t)up;               /* b7-b3 are not connected: 1 */
    return 4;
}

static void applejack_reset(mc_adb_device *d) {
    mc_adb_applejack *a = (mc_adb_applejack *)d;
    a->dx = a->dy = 0;
    a->reported = a->down;
}

static int applejack_handler_ok(const mc_adb_device *d, uint8_t handler) {
    (void)d;
    return handler == 1 || handler == 0x46;
}

static const mc_adb_device_ops applejack_ops = {applejack_talk, NULL, applejack_pending, applejack_reset,
                                                applejack_handler_ok};

void mc_adb_applejack_init(mc_adb_applejack *a) {
    memset(a, 0, sizeof *a);
    a->dev.ops = &applejack_ops;
    a->dev.default_address = 3;
    a->dev.default_handler = 1;
    device_reset(&a->dev);
}

void mc_adb_applejack_move(mc_adb_applejack *a, int dx, int dy) {
    a->dx = a->dx + dx > 4096 ? 4096 : a->dx + dx < -4096 ? -4096 : a->dx + dx;
    a->dy = a->dy + dy > 4096 ? 4096 : a->dy + dy < -4096 ? -4096 : a->dy + dy;
}

void mc_adb_applejack_switch(mc_adb_applejack *a, uint32_t switches, int down) {
    switches &= MC_APPLEJACK_SWITCHES;
    a->down = down ? a->down | switches : a->down & ~switches;
}

void mc_adb_mouse_move(mc_adb_mouse *m, int dx, int dy, int button) {
    /* Bounded so a long stall cannot pile up an unbounded motion. */
    m->dx = m->dx + dx > 4096 ? 4096 : m->dx + dx < -4096 ? -4096 : m->dx + dx;
    m->dy = m->dy + dy > 4096 ? 4096 : m->dy + dy < -4096 ? -4096 : m->dy + dy;
    m->button = (uint8_t)(button != 0);
}
