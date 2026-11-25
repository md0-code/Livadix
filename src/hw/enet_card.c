/* Livadix — Apple's Ethernet cards (enet_card.h). Livadix's own code. */
#include "enet_card.h"

#include "dp8390.h"
#include "sonic.h"
#include "../core/romdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NB_RAM_OFFSET 0xD0000u          /* MinorBaseOS of the Ethernet sResource */
#define NB_RAM_SIZE 0x4000u             /* 16 KB: pages 00h-3Fh */
#define NB_NIC_OFFSET 0xE0000u          /* the driver's dCtlDevBase + 10000h */
#define NB_DECODE 0x000FFFFFu           /* A19-A0 */
#define LC_PROM_OFFSET 0x400000u

struct mc_enet_card {
    unsigned kind, slot;
    mc_nubus *nubus;
    const mc_hw_host *board_host;
    mc_hw_host host;                    /* the board's, with the card's interrupt line */
    mc_dev rom_dev, ram_dev, nic_dev;   /* windows of the NB card */
    uint8_t *rom;
    uint32_t rom_size, rom_window;
    uint8_t lanes;                      /* ByteLanes' low nibble: bit l, byte lane l carries the ROM */
    mc_net_host net;
    mc_dp8390 nic;
    uint8_t ram[NB_RAM_SIZE];
    mc_sonic sonic;
    mc_sonic_prom prom;
};

static unsigned lane_count(uint8_t lanes) { return (lanes & 1u) + (lanes >> 1 & 1u) + (lanes >> 2 & 1u) + (lanes >> 3 & 1u); }

/* ---- the card's host: the chip's INT drives the slot's /NMRQ ------------------- */

static uint64_t card_now(void *ctx) {
    const mc_enet_card *c = ctx;
    return c->board_host->now(c->board_host->ctx);
}
static void card_schedule(void *ctx, void *dev, uint64_t when) {
    const mc_enet_card *c = ctx;
    c->board_host->schedule(c->board_host->ctx, dev, when);
}
static void card_line(void *ctx, void *dev, int line, int state) {
    mc_enet_card *c = ctx;
    (void)dev;
    if (line == 0) mc_nubus_irq(c->nubus, c->slot, state);   /* MC_DP8390_LINE_IRQ, MC_SONIC_LINE_IRQ */
}
static int card_dma(void *ctx, uint32_t address, void *data, uint32_t length, int write) {
    const mc_enet_card *c = ctx;
    return c->board_host->dma && c->board_host->dma(c->board_host->ctx, address, data, length, write);
}

/* ---- the declaration ROM on its byte lanes (Designing Cards and Drivers,
 * chapter 8, "Byte lanes"): as the Slot Manager tests them, the byte at an
 * address is lane (address & 3) (ROM 368CADFE at $40805FAA: from the top
 * address down, the ByteLanes byte must carry the bit of its address's
 * lane); the image holds the bytes of the lanes used, in address order,
 * ending at the slot's top address. --------------------------------------- */

static uint32_t rom_read(void *dev, uint32_t offset, unsigned size) {
    mc_enet_card *c = (mc_enet_card *)((char *)dev - offsetof(mc_enet_card, rom_dev));
    const unsigned per = lane_count(c->lanes);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        const uint32_t a = offset + i;
        const unsigned lane = a & 3u;
        uint8_t b = 0xFF;                                   /* no byte on that lane */
        if (c->lanes >> lane & 1u) {
            unsigned rank = 0;                              /* lanes before it in address order */
            for (unsigned l = 0; l < lane; l++) rank += c->lanes >> l & 1u;
            const uint32_t k = (a >> 2) * per + rank;
            if (k < c->rom_size) b = c->rom[k];
        }
        v = v << 8 | b;
    }
    return v;
}
static void rom_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)dev; (void)offset; (void)size; (void)value;
}
static const mc_dev_ops rom_ops = {rom_read, rom_write, NULL, NULL};

/* ---- the NB card's buffer memory and NIC ------------------------------------- */

static uint32_t ram_read(void *dev, uint32_t offset, unsigned size) {
    mc_enet_card *c = (mc_enet_card *)((char *)dev - offsetof(mc_enet_card, ram_dev));
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | c->ram[(offset + i) % NB_RAM_SIZE];
    return v;
}
static void ram_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_enet_card *c = (mc_enet_card *)((char *)dev - offsetof(mc_enet_card, ram_dev));
    for (unsigned i = 0; i < size; i++) c->ram[(offset + i) % NB_RAM_SIZE] = (uint8_t)(value >> (8 * (size - 1 - i)));
}
static const mc_dev_ops ram_ops = {ram_read, ram_write, NULL, NULL};

static uint32_t nic_read(void *dev, uint32_t offset, unsigned size) {
    mc_enet_card *c = (mc_enet_card *)((char *)dev - offsetof(mc_enet_card, nic_dev));
    const uint32_t v = mc_dp8390_read(&c->nic, 15u - ((offset >> 2) & 15u));
    return size == 1 ? v : size == 2 ? v * 0x0101u : v * 0x01010101u;
}
static void nic_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_enet_card *c = (mc_enet_card *)((char *)dev - offsetof(mc_enet_card, nic_dev));
    (void)size;
    mc_dp8390_write(&c->nic, 15u - ((offset >> 2) & 15u), (uint8_t)value);
}
static const mc_dev_ops nic_ops = {nic_read, nic_write, NULL, NULL};

/* ---- the declaration ROM file -------------------------------------------------- */

static uint8_t *read_file(const char *path, uint32_t size) {
    FILE *f = path ? fopen(path, "rb") : NULL;
    uint8_t *data = f ? malloc(size) : NULL;
    const int ok = data && fread(data, 1, size, f) == size && fgetc(f) == EOF;
    if (f) fclose(f);
    if (!ok) { free(data); return NULL; }
    return data;
}

/* From the roms folders by size and CRC32 (docs/NOTES.md), else under its
 * standard name beside the Macintosh ROM's file. */
static uint8_t *load_rom(const mc_config *config, const mc_firmware *want) {
    const mc_rom_file *file = mc_rom_index_firmware(config->roms, want);
    uint8_t *data = file ? read_file(file->path, want->size) : NULL;
    if (data) return data;
    char path[MC_CONFIG_PATH + 32];
    snprintf(path, sizeof path, "%s", config->rom);
    char *cut = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (back > cut) cut = back;
    if (cut) cut[1] = 0;
    else path[0] = 0;
    strncat(path, want->file, sizeof path - strlen(path) - 1);
    data = read_file(path, want->size);
    if (data && mc_firmware_identify(data, want->size) != want) { free(data); data = NULL; }
    return data;
}

/* ---- fitting ----------------------------------------------------------------------- */

static int attach_nb(mc_enet_card *c, mc_bus *bus) {
    const int region = mc_bus_add_io(bus, mc_nubus_slot_base(c->slot), 0x01000000u, NB_DECODE, 0);
    return region && mc_bus_attach(bus, region, NB_RAM_OFFSET, NB_RAM_SIZE, &c->ram_dev, &ram_ops, 0) &&
           mc_bus_attach(bus, region, NB_NIC_OFFSET, 0x40u, &c->nic_dev, &nic_ops, 0) &&
           mc_bus_attach(bus, region, NB_DECODE + 1u - c->rom_window, c->rom_window, &c->rom_dev, &rom_ops, 0);
}

static int attach_lc(mc_enet_card *c) {
    return mc_nubus_map_device(c->nubus, c->slot, 0, 0x100u, &c->sonic, &mc_sonic_ops) &&
           mc_nubus_map_device(c->nubus, c->slot, LC_PROM_OFFSET, 0x10u, &c->prom, &mc_sonic_prom_ops) &&
           mc_nubus_map_device(c->nubus, c->slot, 0x01000000u - c->rom_window, c->rom_window, &c->rom_dev, &rom_ops);
}

mc_enet_card *mc_enet_card_fit_on(const mc_enet_card_env *env, mc_nubus *nubus, unsigned kind, unsigned slot) {
    const uint32_t want = env->config->network_card;
    if (want == MC_NET_CARD_NONE) return NULL;
    const int explicit = want != MC_NET_CARD_AUTO;
    if (!kind) {
        if (explicit) snprintf(env->error, env->error_size, "network.card: the %s has no slot for an Ethernet card", env->name);
        return NULL;
    }
    const unsigned wanted = want == MC_NET_CARD_NUBUS ? MC_ENET_CARD_NUBUS : want == MC_NET_CARD_LC ? MC_ENET_CARD_LC : kind;
    const char *name = wanted == MC_ENET_CARD_NUBUS ? "EtherTalk NB Card" : "Ethernet LC Card";
    if (wanted != kind) {
        snprintf(env->error, env->error_size, "network.card: the %s has no %s for the %s", env->name,
                 kind == MC_ENET_CARD_NUBUS ? "LC processor-direct slot" : "NuBus slot", name);
        return NULL;
    }
    const mc_firmware *fw = mc_firmware_find(kind == MC_ENET_CARD_NUBUS ? "card-ethernet-nb" : "card-ethernet-lc");
    uint8_t *rom = fw ? load_rom(env->config, fw) : NULL;
    if (!rom || !slot) {
        if (explicit)
            snprintf(env->error, env->error_size, rom ? "network.card: no free slot for the %s" :
                     "network.card: the %s's declaration ROM (%s) is in neither the roms folders nor the ROM's folder",
                     name, fw ? fw->file : "?");
        free(rom);
        return NULL;
    }
    mc_enet_card *c = calloc(1, sizeof *c);
    if (!c) { free(rom); snprintf(env->error, env->error_size, "Out of memory"); return NULL; }
    c->kind = kind;
    c->slot = slot;
    c->nubus = nubus;
    c->board_host = env->host;
    c->host = (mc_hw_host){c, card_now, card_schedule, card_line, card_dma};
    c->rom = rom;
    c->rom_size = fw->size;
    c->lanes = rom[fw->size - 1] & 0x0Fu;
    const unsigned per = lane_count(c->lanes);
    c->rom_window = per ? fw->size / per * 4u : 0;
    c->rom_dev = (mc_dev){&rom_ops, &c->host, "Ethernet card ROM"};
    int ok = per && mc_nubus_fit(nubus, slot);
    if (ok && kind == MC_ENET_CARD_NUBUS) {
        c->ram_dev = (mc_dev){&ram_ops, &c->host, "Ethernet card RAM"};
        c->nic_dev = (mc_dev){&nic_ops, &c->host, "Ethernet card NIC"};
        mc_dp8390_init(&c->nic, &c->host, c->ram, NB_RAM_SIZE);
        ok = attach_nb(c, env->bus);
    } else if (ok) {
        mc_sonic_init(&c->sonic, &c->host, 2);
        mc_sonic_prom_init(&c->prom, &c->host, mc_sonic_default_address, 0, 1);
        ok = attach_lc(c);
    }
    if (!ok) {
        snprintf(env->error, env->error_size, "network.card: the %s cannot be fitted in slot $%X", name, slot);
        mc_enet_card_free(c);
        return NULL;
    }
    return c;
}

mc_enet_card *mc_enet_card_fit(mc_hw *hw, mc_nubus *nubus, unsigned kind, unsigned slot) {
    const mc_enet_card_env env = {&hw->config, hw->model->name, &hw->host, &hw->bus, hw->error, sizeof hw->error};
    return mc_enet_card_fit_on(&env, nubus, kind, slot);
}

void mc_enet_card_reset(mc_enet_card *c) {
    if (!c) return;
    if (c->kind == MC_ENET_CARD_NUBUS) mc_dp8390_ops.reset(&c->nic);
    else mc_sonic_ops.reset(&c->sonic);
}

static void net_transmit(void *ctx, const uint8_t *frame, size_t length) {
    const mc_net_host *net = ctx;
    if (net->transmit) net->transmit(net->ctx, frame, length);
}

void mc_enet_card_net_host(mc_enet_card *c, const mc_net_host *host) {
    if (!c) return;
    if (host) c->net = *host;
    else memset(&c->net, 0, sizeof c->net);
    if (c->kind == MC_ENET_CARD_NUBUS) {
        const mc_dp8390_port port = {host ? &c->net : NULL, host ? net_transmit : NULL};
        mc_dp8390_set_port(&c->nic, host ? &port : NULL);
    } else {
        const mc_sonic_port port = {host ? &c->net : NULL, host ? net_transmit : NULL};
        mc_sonic_set_port(&c->sonic, host ? &port : NULL);
    }
}

int mc_enet_card_net_receive(mc_enet_card *c, const uint8_t *frame, size_t length) {
    if (!c) return 1;   /* no Ethernet: the frame is lost */
    return c->kind == MC_ENET_CARD_NUBUS ? mc_dp8390_receive(&c->nic, frame, length)
                                         : mc_sonic_receive(&c->sonic, frame, length);
}

void mc_enet_card_free(mc_enet_card *c) {
    if (!c) return;
    free(c->rom);
    free(c);
}
