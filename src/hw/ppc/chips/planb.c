/* Livadix — Plan B, the video input stub (planb.h).
 *
 * Livadix's own code. */
#include "planb.h"

#include <string.h>

void mc_planb_init(mc_planb *p, const mc_hw_host *host, const char *name) {
    memset(p, 0, sizeof *p);
    p->dev.host = host;
    p->dev.name = name;
    /* PCI 2.0 6.2: class "other multimedia" ($048000)†. */
    mc_pci_config_init(&p->cfg, 0x106Bu, 0x0004u, 0x00u, 0x048000u, 0x00u);
    mc_pci_config_bar(&p->cfg, 0, MC_PLANB_REGS, 0);
}

static uint32_t fn_config_read(void *d, unsigned reg, unsigned size) { return mc_pci_config_read(&((mc_planb *)d)->cfg, reg, size); }
static void fn_config_write(void *d, unsigned reg, unsigned size, uint32_t value) { mc_pci_config_write(&((mc_planb *)d)->cfg, reg, size, value); }
static int fn_mem_read(void *d, uint32_t address, unsigned size, uint32_t *value) {
    (void)size;
    if (!mc_pci_config_hit(&((mc_planb *)d)->cfg, 0, address, NULL)) return 0;
    *value = 0;
    return 1;
}
static int fn_mem_write(void *d, uint32_t address, unsigned size, uint32_t value) {
    (void)size;
    (void)value;
    return mc_pci_config_hit(&((mc_planb *)d)->cfg, 0, address, NULL);
}

mc_pci_function mc_planb_function(mc_planb *p) {
    return (mc_pci_function){p, fn_config_read, fn_config_write, fn_mem_read, fn_mem_write};
}
