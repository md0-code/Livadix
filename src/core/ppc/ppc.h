#ifndef MC_PPC_H
#define MC_PPC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MPC601UM chapters 2, 5: numeric masks use the manual's MSB-first bits. */
#define PPC_MSR_LE UINT32_C(0x00000001)
#define PPC_MSR_DR UINT32_C(0x00000010)
#define PPC_MSR_IR UINT32_C(0x00000020)
#define PPC_MSR_IP UINT32_C(0x00000040)
#define PPC_MSR_FE1 UINT32_C(0x00000100)
#define PPC_MSR_BE UINT32_C(0x00000200)
#define PPC_MSR_SE UINT32_C(0x00000400)
#define PPC_MSR_FE0 UINT32_C(0x00000800)
#define PPC_MSR_ME UINT32_C(0x00001000)
#define PPC_MSR_FP UINT32_C(0x00002000)
#define PPC_MSR_PR UINT32_C(0x00004000)
#define PPC_MSR_EE UINT32_C(0x00008000)
#define PPC_MSR_ILE UINT32_C(0x00010000)
#define PPC_MSR_TGPR UINT32_C(0x00020000)
#define PPC_MSR_POW UINT32_C(0x00040000)
#define PPC_MSR_RI UINT32_C(0x00000002)
#define PPC_XER_CA UINT32_C(0x20000000)
#define PPC_XER_OV UINT32_C(0x40000000)
#define PPC_XER_SO UINT32_C(0x80000000)
#define PPC_EX_RESET UINT32_C(0x100)
#define PPC_EX_MACHINE UINT32_C(0x200)
#define PPC_EX_DSI UINT32_C(0x300)
#define PPC_EX_ISI UINT32_C(0x400)
#define PPC_EX_EXTERNAL UINT32_C(0x500)
#define PPC_EX_ALIGNMENT UINT32_C(0x600)
#define PPC_EX_PROGRAM UINT32_C(0x700)
#define PPC_EX_FPU UINT32_C(0x800)
#define PPC_EX_DECREMENTER UINT32_C(0x900)
#define PPC_EX_SYSCALL UINT32_C(0xc00)
#define PPC_EX_TRACE UINT32_C(0xd00)
#define PPC_EX_ITLB UINT32_C(0x1000)
#define PPC_EX_DTLB_LOAD UINT32_C(0x1100)
#define PPC_EX_DTLB_STORE UINT32_C(0x1200)
#define PPC_EX_IABR UINT32_C(0x1300)
/* MPC601UM 5.4.12 and its addendum: the 601's run mode exception, which
   also takes the 601's MSR[SE] trace (later models trace through $D00). */
#define PPC_EX_RUN_MODE UINT32_C(0x2000)
/* MPC601UM 5.4.3, PEM 6.4.3 table 6-9: DSISR[6] store, DSISR[9] DABR. */
#define PPC_DSISR_STORE UINT32_C(0x02000000)
#define PPC_DSISR_DABR UINT32_C(0x00400000)

typedef enum ppc_model {
    PPC_MODEL_601, PPC_MODEL_603, PPC_MODEL_603E,
    PPC_MODEL_604, PPC_MODEL_604E, PPC_MODEL_750, PPC_MODEL_COUNT
} ppc_model;
typedef struct ppc_tlb_entry { uint32_t vsid, page, pte; bool valid; } ppc_tlb_entry;
typedef enum ppc_access { PPC_READ, PPC_WRITE, PPC_FETCH } ppc_access;
typedef enum ppc_result { PPC_OK, PPC_EXCEPTION, PPC_UNIMPLEMENTED, PPC_CHECKSTOP } ppc_result;
/* Callbacks transfer numerical big-endian values; false signals a bus error.
   A 64-bit access is two 32-bit transfers. Nothing owns guest media. */
typedef bool (*ppc_bus_read)(void *, uint32_t, unsigned, uint32_t *);
typedef bool (*ppc_bus_write)(void *, uint32_t, unsigned, uint32_t);
typedef struct ppc_bus { void *opaque; ppc_bus_read read; ppc_bus_write write; } ppc_bus;
typedef struct ppc_cpu {
    ppc_model model;
    uint32_t gpr[32];
    uint64_t fpr[32];
    uint32_t pc, msr, cr, xer, lr, ctr, mq, fpscr;
    uint32_t sr[16], bat[4][2], sdr1, dar, dsisr, srr0, srr1;
    uint32_t sprg[4], hid[16], ear, pvr, dec, rtcu, rtcl;
    uint32_t dbat[4][2], tgpr[4], imiss, icmp, dmiss, dcmp, hash1, hash2, rpa;
    uint32_t mmcr[2], pmc[4], sia, sda, iabr, dabr, l2cr, ictc, thrm[3];
    ppc_tlb_entry tlb[2][32][2];
    unsigned char tlb_lru[2][32];
    uint32_t exceptions;        /* exceptions taken, counted (the 68k accelerator's shadow check) */
    uint32_t tlb_writes;        /* the software TLB's loads and invalidations, counted: the translator compares its copy after one */
    uint64_t time_base;
    uint32_t reservation, last_opcode, last_pc, last_exception, mmu_wim;
    bool reserved, external_irq, decrementer_pending;
    ppc_result result;
    uint64_t instructions;
    ppc_bus bus;
} ppc_cpu;

void ppc_init(ppc_cpu *, ppc_bus);
void ppc_init_model(ppc_cpu *, ppc_bus, ppc_model);
void ppc_reset(ppc_cpu *);
ppc_result ppc_step(ppc_cpu *);
/* The instruction `op` fetched from pc, after ppc_step's interrupt check,
   fetch and instruction breakpoint: the caller has set result = PPC_OK,
   last_pc = pc, last_opcode = op and last_exception = 0 (the translator's
   call-out, ppc_jit.c). */
ppc_result ppc_execute(ppc_cpu *, uint32_t op);
void ppc_exception(ppc_cpu *, uint32_t vector, uint32_t cause, uint32_t saved_pc);
void ppc_clock(ppc_cpu *, uint32_t ticks);
bool ppc_translate(ppc_cpu *, uint32_t ea, ppc_access, uint32_t *pa);
bool ppc_alignment(ppc_cpu *, uint32_t ea);
bool ppc_preflight(ppc_cpu *, uint32_t ea, unsigned bytes, bool sequence, bool floating);
bool ppc_load(ppc_cpu *, uint32_t ea, unsigned size, uint32_t *);
bool ppc_store(ppc_cpu *, uint32_t ea, unsigned size, uint32_t);
/* The data address breakpoint (601 HID5, later DABR) for one instruction's
   `bytes` at `ea`; `sequence` checks word by word (strings, multiples).
   Takes the DSI and returns false on a match. */
bool ppc_data_breakpoint(ppc_cpu *, uint32_t ea, unsigned bytes, bool store, bool sequence);
void ppc_record(ppc_cpu *, uint32_t);
void ppc_overflow(ppc_cpu *, bool);
/* Return false only when the opcode does not belong to the module.
   Recognised illegal encodings raise PROGRAM, never silently become no-ops. */
bool ppc_power(ppc_cpu *, uint32_t opcode);
bool ppc_fpu(ppc_cpu *, uint32_t opcode);
/* ppc_fpu's effects for the ordinary cases, on host arithmetic; false: none made. */
bool ppc_fpu_fast(ppc_cpu *, uint32_t opcode);
uint64_t ppc_fp_from_single(uint32_t bits);
uint32_t ppc_fp_to_single(uint64_t bits);
#endif
