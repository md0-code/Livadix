/* Livadix — the PCI machines' NVRAM (nvram.h).
 *
 * Livadix's own code. */
#include "nvram.h"

#include <string.h>

void mc_nvram_init(mc_nvram *n, const mc_hw_host *host, const char *name, const uint8_t *image) {
    memset(n, 0, sizeof *n);
    n->dev.ops = &mc_nvram_page_ops;
    n->dev.host = host;
    n->dev.name = name;
    if (image) memcpy(n->data, image, MC_NVRAM_SIZE);
}

/* The page register (a little-endian value through Grand Central). */
static uint32_t page_read(void *p, uint32_t offset, unsigned size) {
    const mc_nvram *n = p;
    const uint32_t v = n->page >> (8 * (offset & 3u));
    return size == 4 ? v : v & ((1u << (8 * size)) - 1u);
}

static void page_write(void *p, uint32_t offset, unsigned size, uint32_t value) {
    mc_nvram *n = p;
    if (offset & 3u) return;
    n->page = (size == 4 ? value : value & ((1u << (8 * size)) - 1u)) & 0x3FFu;
}

static uint32_t at(const mc_nvram *n, uint32_t offset) {
    return ((n->page << 5) | ((offset >> 4) & 31u)) & (MC_NVRAM_SIZE - 1u);
}

static uint32_t data_read(void *p, uint32_t offset, unsigned size) {
    const mc_nvram *n = p;
    (void)size;
    return (offset & 15u) ? 0 : n->data[at(n, offset)];
}

static void data_write(void *p, uint32_t offset, unsigned size, uint32_t value) {
    mc_nvram *n = p;
    (void)size;
    if (offset & 15u) return;
    n->data[at(n, offset)] = (uint8_t)value;
    n->dirty = 1;
}

const mc_dev_ops mc_nvram_page_ops = {page_read, page_write, NULL, NULL, NULL};
const mc_dev_ops mc_nvram_data_ops = {data_read, data_write, NULL, NULL, NULL};
