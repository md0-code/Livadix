#ifndef MC_PPC_FP_SOFT_H
#define MC_PPC_FP_SOFT_H
#include <stdbool.h>
#include <stdint.h>

/* MPC601UM 2.2.3, tables 2-1/2-2 and 5-17: MSB-first FPSCR fields. */
#define PPC_FP_FX       UINT32_C(0x80000000)
#define PPC_FP_FEX      UINT32_C(0x40000000)
#define PPC_FP_VX       UINT32_C(0x20000000)
#define PPC_FP_OX       UINT32_C(0x10000000)
#define PPC_FP_UX       UINT32_C(0x08000000)
#define PPC_FP_ZX       UINT32_C(0x04000000)
#define PPC_FP_XX       UINT32_C(0x02000000)
#define PPC_FP_VXSNAN   UINT32_C(0x01000000)
#define PPC_FP_VXISI    UINT32_C(0x00800000)
#define PPC_FP_VXIDI    UINT32_C(0x00400000)
#define PPC_FP_VXZDZ    UINT32_C(0x00200000)
#define PPC_FP_VXIMZ    UINT32_C(0x00100000)
#define PPC_FP_VXVC     UINT32_C(0x00080000)
#define PPC_FP_FR       UINT32_C(0x00040000)
#define PPC_FP_FI       UINT32_C(0x00020000)
#define PPC_FP_FPRF     UINT32_C(0x0001f000)
#define PPC_FP_FPCC     UINT32_C(0x0000f000)
#define PPC_FP_VXSOFT   UINT32_C(0x00000400)
#define PPC_FP_VXSQRT   UINT32_C(0x00000200)
#define PPC_FP_VXCVI    UINT32_C(0x00000100)
#define PPC_FP_VE       UINT32_C(0x00000080)
#define PPC_FP_OE       UINT32_C(0x00000040)
#define PPC_FP_UE       UINT32_C(0x00000020)
#define PPC_FP_ZE       UINT32_C(0x00000010)
#define PPC_FP_XE       UINT32_C(0x00000008)
#define PPC_FP_NI       UINT32_C(0x00000004)
#define PPC_FP_RESERVED UINT32_C(0x00000804)
#define PPC_FP_INVALID (PPC_FP_VXSNAN | PPC_FP_VXISI | PPC_FP_VXIDI | \
    PPC_FP_VXZDZ | PPC_FP_VXIMZ | PPC_FP_VXVC | PPC_FP_VXSOFT | \
    PPC_FP_VXSQRT | PPC_FP_VXCVI)
#define PPC_FP_STICKY (PPC_FP_OX | PPC_FP_UX | PPC_FP_ZX | PPC_FP_XX | PPC_FP_INVALID)

typedef enum ppc_fp_operation {
    PPC_FP_ADD, PPC_FP_SUB, PPC_FP_MUL, PPC_FP_DIV,
    PPC_FP_MADD, PPC_FP_MSUB, PPC_FP_NMADD, PPC_FP_NMSUB,
    PPC_FP_ROUND_SINGLE, PPC_FP_TO_INT, PPC_FP_TO_INT_ZERO
} ppc_fp_operation;

typedef struct ppc_fp_result {
    uint64_t bits;
    uint32_t raised;       /* Exception subcauses and nonsticky FR/FI. */
    unsigned fprf;
    bool write;
    bool classify;        /* Integer conversion leaves undefined FPRF unchanged. */
} ppc_fp_result;

ppc_fp_result ppc_fp_compute(ppc_fp_operation, bool single,
    uint64_t a, uint64_t b, uint64_t c, uint32_t fpscr);
ppc_fp_result ppc_fp_compute_later(ppc_fp_operation, bool single,
    uint64_t a, uint64_t b, uint64_t c, uint32_t fpscr, bool flush_inputs);
ppc_fp_result ppc_fp_estimate(uint64_t bits, uint32_t fpscr, bool square_root, bool flush_inputs);
uint64_t ppc_fp_flush(uint64_t bits, bool single);
unsigned ppc_fp_compare(uint64_t a, uint64_t b, bool ordered,
    uint32_t fpscr, uint32_t *raised);
uint32_t ppc_fp_summary(uint32_t fpscr);
/* MPC601UM 3.5.9.1/3.5.10.1: no rounding and no FPSCR changes. */
uint64_t ppc_fp_from_single(uint32_t bits);
uint32_t ppc_fp_to_single(uint64_t bits);
#endif
