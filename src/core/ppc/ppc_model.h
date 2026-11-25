#ifndef MC_PPC_MODEL_H
#define MC_PPC_MODEL_H
#include "ppc.h"

#define PPC_MODEL_SOFT_TLB 1u
#define PPC_MODEL_PMC2 2u
#define PPC_MODEL_PMC4 4u
#define PPC_MODEL_L2 8u

/* MPC601UM 3.5.7, MPC603EUM 2.3.4.7, and the Pippin 603 ROM's SCSI
 * queue ($FFEC87C8-$FFEC88A4 in 2BF65931): stwcx. may store to a
 * different block as long as the processor still holds a reservation. */
static inline bool ppc_reservation_matches(const ppc_cpu *c, uint32_t pa) {
    return c->model == PPC_MODEL_601 || c->model == PPC_MODEL_603 ||
           c->model == PPC_MODEL_603E || c->reservation == (pa & ~31u);
}

typedef struct ppc_model_info {
    const char *name;
    uint32_t pvr, msr_mask;
    unsigned features;
} ppc_model_info;

const ppc_model_info *ppc_model_information(ppc_model);
bool ppc_model_parse(const char *, ppc_model *);
bool ppc_little_endian(const ppc_cpu *);
bool ppc_power_waiting(const ppc_cpu *);
void ppc_model_reset(ppc_cpu *);
void ppc_model_set_msr(ppc_cpu *, uint32_t, bool returning);
void ppc_model_exception(ppc_cpu *, uint32_t, uint32_t, uint32_t);
bool ppc_model_spr_read(ppc_cpu *, unsigned, uint32_t *);
bool ppc_model_spr_write(ppc_cpu *, unsigned, uint32_t);
bool ppc_model_decode(ppc_cpu *, uint32_t);
bool ppc_classic_translate(ppc_cpu *, uint32_t, ppc_access, uint32_t *);
bool ppc_classic_debug_walk(ppc_cpu *, uint32_t, ppc_access, uint32_t *);
bool ppc_classic_preflight(ppc_cpu *, uint32_t, unsigned, bool, bool);
void ppc_classic_tlb_load(ppc_cpu *, uint32_t, bool instruction);
void ppc_classic_tlb_invalidate(ppc_cpu *, uint32_t);
#endif
