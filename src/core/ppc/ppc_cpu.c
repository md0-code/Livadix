#include "ppc.h"
#include "ppc_model.h"
#include "ppc_fp_soft.h"
#include <string.h>

/* MPC601UM 2.7.1, table 2-36; 5.2, table 5-4. */
#define MSR_601 UINT32_C(0x0000fd70)
#define PROGRAM_ILLEGAL UINT32_C(0x00080000)
#define PROGRAM_PRIVILEGED UINT32_C(0x00040000)
#define PROGRAM_TRAP UINT32_C(0x00020000)

static int32_t signed32(uint32_t v)
{
    return v <= INT32_MAX ? (int32_t)v : -1 - (int32_t)(~v);
}
static int32_t signed16(uint32_t v)
{
    return (v & 0x8000u) ? (int32_t)(v & 0xffffu) - 65536 : (int32_t)(v & 0xffffu);
}
static uint32_t rotate(uint32_t v, unsigned n)
{
    n &= 31u;
    return n ? (v << n) | (v >> (32u - n)) : v;
}
static uint32_t mask(unsigned first, unsigned last)
{
    uint32_t left = UINT32_MAX >> first, right = UINT32_MAX << (31u - last);
    return first <= last ? left & right : left | right;
}
static void cr_field(ppc_cpu *c, unsigned f, uint32_t v)
{
    unsigned shift = 28u - 4u * f;
    c->cr = (c->cr & ~(15u << shift)) | ((v & 15u) << shift);
}
void ppc_record(ppc_cpu *c, uint32_t v)
{
    cr_field(c, 0, (v & 0x80000000u ? 8u : v ? 4u : 2u) | (c->xer >> 31));
}
void ppc_overflow(ppc_cpu *c, bool overflow)
{
    c->xer = (c->xer & ~PPC_XER_OV) | (overflow ? PPC_XER_OV | PPC_XER_SO : 0u);
}
static void carry(ppc_cpu *c, bool value)
{
    c->xer = (c->xer & ~PPC_XER_CA) | (value ? PPC_XER_CA : 0u);
}
static void illegal(ppc_cpu *c)
{
    ppc_exception(c, PPC_EX_PROGRAM, PROGRAM_ILLEGAL, c->last_pc);
}
static bool privileged(ppc_cpu *c)
{
    if (!(c->msr & PPC_MSR_PR)) return true;
    ppc_exception(c, PPC_EX_PROGRAM, PROGRAM_PRIVILEGED, c->last_pc);
    return false;
}
void ppc_init(ppc_cpu *c, ppc_bus bus)
{
    memset(c, 0, sizeof(*c));
    c->bus = bus;
    ppc_reset(c);
}
void ppc_reset(ppc_cpu *c)
{
    ppc_bus bus = c->bus;
    ppc_model model = c->model;
    memset(c, 0, sizeof(*c));
    c->bus = bus;
    c->model = model;
    c->pc = UINT32_C(0xfff00100);
    if (model != PPC_MODEL_601) { ppc_model_reset(c); return; }
    c->msr = UINT32_C(0x1040);
    c->pvr = UINT32_C(0x00010001);
    c->hid[0] = UINT32_C(0x80010080);
}
void ppc_exception(ppc_cpu *c, uint32_t vector, uint32_t cause, uint32_t saved_pc)
{
    /* MPC601UM 5.2 and individual exception register-setting tables. */
    uint32_t old = c->msr;
    ++c->exceptions;
    if (c->model != PPC_MODEL_601) { ppc_model_exception(c, vector, saved_pc, cause); return; }
    c->srr0 = saved_pc;
    c->srr1 = (old & 0xffffu) | cause;
    c->last_exception = vector;
    /* MPC601UM 3.5.7: every synchronous/asynchronous exception clears it. */
    c->reserved = false;
    if (vector == PPC_EX_MACHINE && !(old & PPC_MSR_ME) && (c->hid[0] & 0x80000000u)) {
        c->result = PPC_CHECKSTOP;
        return;
    }
    c->msr = old & (PPC_MSR_ME | PPC_MSR_IP);
    if (vector == PPC_EX_MACHINE) c->msr &= ~PPC_MSR_ME;
    c->pc = (old & PPC_MSR_IP ? UINT32_C(0xfff00000) : 0u) | vector;
    c->result = PPC_EXCEPTION;
}
void ppc_clock(ppc_cpu *c, uint32_t ticks)
{
    if (c->model != PPC_MODEL_601) {
        if (ticks > c->dec) c->decrementer_pending = true;
        c->dec -= ticks; c->time_base += ticks;
        return;
    }
    /* MPC601UM 2.2.5.3 and 2.3.3.5: one external tick is 128 ns;
       DEC has seven unimplemented low bits, RTC rolls at one second. */
    uint64_t ns = (uint64_t)c->rtcl + (uint64_t)ticks * 128u;
    uint64_t decrement = (uint64_t)ticks * 128u;
    if (decrement > c->dec) c->decrementer_pending = true;
    c->dec -= (uint32_t)decrement;
    c->rtcu += (uint32_t)(ns / UINT64_C(1000000000));
    c->rtcl = (uint32_t)(ns % UINT64_C(1000000000));
}
static bool spr_read(ppc_cpu *c, unsigned spr, uint32_t *v)
{
    /* MPC601UM 3.3.6, tables 3-40/41: RTC aliases and unified BAT SPRs. */
    if ((spr & 16u) && !privileged(c)) return false;
    if (c->model != PPC_MODEL_601) {
        if (ppc_model_spr_read(c, spr, v)) return true;
        illegal(c); return false;
    }
    switch (spr) {
    case 0: *v = c->mq; break;
    case 1: *v = c->xer; break;
    case 4: *v = c->rtcu; break;
    case 5: *v = c->rtcl; break;
    case 6: *v = c->dec; break;
    case 8: *v = c->lr; break;
    case 9: *v = c->ctr; break;
    case 18: *v = c->dsisr; break;
    case 19: *v = c->dar; break;
    case 22: *v = c->dec; break;
    case 25: *v = c->sdr1; break;
    case 26: *v = c->srr0; break;
    case 27: *v = c->srr1; break;
    case 282: *v = c->ear; break;
    case 287: *v = c->pvr; break;
    default:
        if (spr >= 272 && spr <= 275) *v = c->sprg[spr - 272];
        else if (spr >= 528 && spr <= 535) *v = c->bat[(spr - 528) / 2][spr & 1u];
        else if (spr == 1008 || spr == 1009 || spr == 1010 || spr == 1013 || spr == 1023)
            *v = c->hid[spr - 1008];
        else return false; /* Invalid mfspr is a no-op on 601 (UM 10, mfspr). */
    }
    return true;
}
static bool spr_write(ppc_cpu *c, unsigned spr, uint32_t v)
{
    if ((spr & 16u) && !privileged(c)) return false;
    if (c->model != PPC_MODEL_601) {
        if (ppc_model_spr_write(c, spr, v)) return true;
        illegal(c); return false;
    }
    switch (spr) {
    case 0: c->mq = v; break;
    case 1: c->xer = v & UINT32_C(0xe000ff7f); break;
    case 8: c->lr = v; break;
    case 9: c->ctr = v; break;
    case 18: c->dsisr = v; break;
    case 19: c->dar = v; break;
    case 20: c->rtcu = v; break;
    case 21: c->rtcl = v & UINT32_C(0x3fffff80); break;
    case 22:
        if (!(c->dec & 0x80000000u) && (v & 0x80000000u)) c->decrementer_pending = true;
        c->dec = v & UINT32_C(0xffffff80); break;
    case 25: c->sdr1 = v; break;
    case 26: c->srr0 = v; break;
    case 27: c->srr1 = v; break;
    case 282: c->ear = v; break;
    default:
        if (spr >= 272 && spr <= 275) c->sprg[spr - 272] = v;
        else if (spr >= 528 && spr <= 535) c->bat[(spr - 528) / 2][spr & 1u] = v;
        else if (spr == 1008 || spr == 1009 || spr == 1010 || spr == 1013 || spr == 1023)
            c->hid[spr - 1008] = v;
        else return true; /* Invalid mtspr is a no-op on 601 (UM 10, mtspr). */
    }
    return true;
}
static bool trap(unsigned to, uint32_t a, uint32_t b)
{
    return ((to & 16u) && signed32(a) < signed32(b)) ||
           ((to & 8u) && signed32(a) > signed32(b)) ||
           ((to & 4u) && a == b) || ((to & 2u) && a < b) || ((to & 1u) && a > b);
}
static void compare(ppc_cpu *c, unsigned f, uint32_t a, uint32_t b, bool sign)
{
    bool lt = sign ? signed32(a) < signed32(b) : a < b;
    cr_field(c, f, (a == b ? 2u : lt ? 8u : 4u) | (c->xer >> 31));
}
static bool branch_taken(const ppc_cpu *c, unsigned bo, unsigned bi)
{
    uint32_t ctr = (bo & 4u) ? c->ctr : c->ctr - 1u;
    bool count_ok = (bo & 4u) || ((ctr != 0) != ((bo & 2u) != 0));
    return count_ok && ((bo & 16u) || (((c->cr >> (31u - bi)) & 1u) == ((bo >> 3) & 1u)));
}
static bool branch_condition(ppc_cpu *c, unsigned bo, unsigned bi)
{
    bool taken = branch_taken(c, bo, bi);
    if (!(bo & 4u)) --c->ctr;
    return taken;
}
/* The target of a taken b, bc, bclr or bcctr; false for anything else. */
static bool branch_target(const ppc_cpu *c, uint32_t op, uint32_t *target)
{
    unsigned primary = op >> 26, bo = (op >> 21) & 31u, bi = (op >> 16) & 31u;
    unsigned xo = (op >> 1) & 1023u;
    uint32_t displacement;
    if (primary == 18) {
        displacement = op & 0x03fffffcu;
        if (displacement & 0x02000000u) displacement |= UINT32_C(0xfc000000);
    } else if (primary == 16) {
        if (!branch_taken(c, bo, bi)) return false;
        displacement = op & 0xfffcu;
        if (displacement & 0x8000u) displacement |= UINT32_C(0xffff0000);
    } else if (primary == 19 && (xo == 16 || (xo == 528 && (bo & 4u)))) {
        if (!branch_taken(c, bo, bi)) return false;
        *target = (xo == 16 ? c->lr : c->ctr) & ~3u;
        return true;
    } else return false;
    *target = (op & 2u ? 0u : c->pc) + displacement;
    return true;
}
static bool instruction_breakpoint(ppc_cpu *c, uint32_t op)
{
    uint32_t target;
    if (c->model == PPC_MODEL_601) {
        /* MPC601UM 2.3.3.12.2-3 (tables 2-23/24), 5.4.12 and its addendum
           (table 5-26): HID1[1-3] selects the compare, HID2 holds the word
           EA; the run mode exception is taken before the instruction (or
           the branch, in branch target mode) executes, SRR1 = the whole MSR.
           Table 2-23 gives HID1[8-9] = 10 for the trap, 5.4.12 and the
           addendum b'01': both select it. Hard stop needs the 601's debug
           port, which Macintosh boards do not connect: no action. */
        unsigned mode = (c->hid[1] >> 28) & 7u, response = (c->hid[1] >> 22) & 3u;
        bool hit = false;
        if (response != 1u && response != 2u) return false;
        if (mode == 2u || mode == 6u) hit = ((c->pc ^ c->hid[2]) & ~3u) == 0;
        else if (mode == 7u) hit = branch_target(c, op, &target) && ((target ^ c->hid[2]) & ~3u) == 0;
        if (hit) ppc_exception(c, PPC_EX_RUN_MODE, c->msr & 0xffff0000u, c->pc);
        return hit;
    }
    /* MPC603EUM 2.1.2.6, 4.5.15 (TE ignored); MPC604EUM 2.1.2.1, 4.5.14;
       MPC750UM 2.1.2.1, 4.5.14: IABR[0-29] word, BE enables, TE must equal
       MSR[IR]; taken before the matching instruction executes. */
    (void)op;
    if (!(c->iabr & 2u) || ((c->pc ^ c->iabr) & ~3u)) return false;
    if (c->model >= PPC_MODEL_604 && ((c->iabr & 1u) != 0) != ((c->msr & PPC_MSR_IR) != 0)) return false;
    ppc_exception(c, PPC_EX_IABR, 0, c->pc);
    return true;
}
static bool xl(ppc_cpu *c, uint32_t op)
{
    unsigned xo = (op >> 1) & 1023u, bt = (op >> 21) & 31u;
    unsigned ba = (op >> 16) & 31u, bb = (op >> 11) & 31u;
    uint32_t a = (c->cr >> (31u - ba)) & 1u, b = (c->cr >> (31u - bb)) & 1u, r;
    switch (xo) {
    case 0: cr_field(c, bt >> 2, (c->cr >> (28u - (ba >> 2) * 4u)) & 15u); return true;
    case 16: case 528: {
        uint32_t target = (xo == 16 ? c->lr : c->ctr) & ~3u;
        if (xo == 528 && !(bt & 4u)) { illegal(c); return true; }
        if (branch_condition(c, bt, ba)) c->pc = target;
        if (op & 1u) c->lr = c->last_pc + 4u;
        return true;
    }
    case 50:
        if (privileged(c)) {
            if (c->model == PPC_MODEL_601) c->msr = c->srr1 & MSR_601;
            else ppc_model_set_msr(c, c->srr1, true);
            c->pc = c->srr0 & ~3u;
        }
        return true;
    case 150: return true; /* MPC601UM 10: isync, coherent interpreter. */
    case 33: r = !(a | b); break;
    case 129: r = a & !b; break;
    case 193: r = a ^ b; break;
    case 225: r = !(a & b); break;
    case 257: r = a & b; break;
    case 289: r = !(a ^ b); break;
    case 417: r = a | !b; break;
    case 449: r = a | b; break;
    default: return false;
    }
    c->cr = (c->cr & ~(1u << (31u - bt))) | ((r & 1u) << (31u - bt));
    return true;
}
static bool span(ppc_cpu *c, uint32_t ea, unsigned bytes, ppc_access access,
                 bool sequence, bool floating)
{
    uint32_t pa;
    if (!ppc_preflight(c, ea, bytes, sequence, floating)) return false;
    if (!bytes) return true;
    if (!ppc_translate(c, ea, access, &pa)) return false;
    if (((ea & 4095u) + bytes) > 4096u && !ppc_translate(c, ea + bytes - 1u, access, &pa)) return false;
    /* MPC750UM table 4-3: a DABR match ranks below translation faults. */
    return ppc_data_breakpoint(c, ea, bytes, access == PPC_WRITE, sequence);
}
static bool multiple(ppc_cpu *c, uint32_t ea, unsigned reg, bool store)
{
    unsigned n = (32u - reg) * 4u;
    uint32_t temp[32];
    if (c->model != PPC_MODEL_601 && (ea & 3u)) return ppc_alignment(c, ea);
    if (!span(c, ea, n, store ? PPC_WRITE : PPC_READ, true, false)) return false;
    for (unsigned r = reg; r < 32; ++r, ea += 4u) {
        if (store) { if (!ppc_store(c, ea, 4, c->gpr[r])) return false; }
        else if (!ppc_load(c, ea, 4, &temp[r])) return false;
    }
    if (!store) for (unsigned r = reg; r < 32; ++r) c->gpr[r] = temp[r];
    return true;
}
static bool string_access(ppc_cpu *c, uint32_t op, bool store, bool indexed)
{
    unsigned d = (op >> 21) & 31u, a = (op >> 16) & 31u, b = (op >> 11) & 31u;
    unsigned bytes = indexed ? c->xer & 127u : b ? b : 32u;
    uint32_t ea = (a ? c->gpr[a] : 0u) + (indexed ? c->gpr[b] : 0u), temp[32];
    bool changed[32] = {false};
    memcpy(temp, c->gpr, sizeof(temp));
    if (!span(c, ea, bytes, store ? PPC_WRITE : PPC_READ, true, false)) return false;
    for (unsigned i = 0; i < bytes; ++i) {
        unsigned r = (d + i / 4u) & 31u, shift = 24u - (i & 3u) * 8u;
        uint32_t v;
        if (store) { if (!ppc_store(c, ea + i, 1, c->gpr[r] >> shift)) return false; }
        else {
            if (!ppc_load(c, ea + i, 1, &v)) return false;
            if ((a && r == a) || (indexed && r == b)) continue; /* 601-specific skipped address registers. */
            if (!(i & 3u)) temp[r] = 0;
            temp[r] |= v << shift;
            changed[r] = true;
        }
    }
    if (!store) for (unsigned r = 0; r < 32; ++r) if (changed[r]) c->gpr[r] = temp[r];
    return true;
}
static bool memory(ppc_cpu *c, uint32_t op, unsigned size, bool store, bool update,
                   bool indexed, bool sign, bool reversed, bool fp)
{
    unsigned d = (op >> 21) & 31u, a = (op >> 16) & 31u, b = (op >> 11) & 31u;
    uint32_t ea = (a ? c->gpr[a] : 0u) + (indexed ? c->gpr[b] : (uint32_t)signed16(op));
    uint32_t v = 0, low;
    bool le_double = fp && size == 8 && c->model != PPC_MODEL_601 && ppc_little_endian(c);
    uint32_t first = ea + (le_double ? 4u : 0u), second = ea + (le_double ? 0u : 4u);
    uint64_t wide;
    if (update && (!a || (!store && !fp && a == d))) { illegal(c); return false; }
    if (fp && !(c->msr & PPC_MSR_FP)) { ppc_exception(c, PPC_EX_FPU, 0, c->last_pc); return false; }
    if (!span(c, ea, size, store ? PPC_WRITE : PPC_READ, false, fp)) return false;
    if (store) {
        /* MPC604EUM tables 2-32/33, MPC750UM 2-40/41: FP loads and stores
           convert formats only; NI does not apply to them. */
        wide = fp ? c->fpr[d] : c->gpr[d];
        if (fp && size == 4) wide = ppc_fp_to_single(wide);
        v = size == 8 ? (uint32_t)(wide >> 32) : (uint32_t)wide;
        if (reversed) v = size == 2 ? ((v & 255u) << 8) | ((v >> 8) & 255u) :
            ((v & 255u) << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u) | (v >> 24);
        if (!ppc_store(c, first, size == 8 ? 4 : size, v)) return false;
        if (size == 8 && !ppc_store(c, second, 4, (uint32_t)wide)) return false;
    } else {
        if (!ppc_load(c, first, size == 8 ? 4 : size, &v)) return false;
        if (reversed) v = size == 2 ? ((v & 255u) << 8) | ((v >> 8) & 255u) :
            ((v & 255u) << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u) | (v >> 24);
        if (sign) v = (uint32_t)signed16(v);
        if (fp) {
            if (size == 8) { if (!ppc_load(c, second, 4, &low)) return false; c->fpr[d] = ((uint64_t)v << 32) | low; }
            else c->fpr[d] = ppc_fp_from_single(v);
        } else c->gpr[d] = v;
    }
    if (update) c->gpr[a] = ea;
    return true;
}
static bool extended(ppc_cpu *c, uint32_t op)
{
    unsigned d = (op >> 21) & 31u, a = (op >> 16) & 31u, b = (op >> 11) & 31u;
    unsigned xo = (op >> 1) & 1023u, base = xo & 511u;
    uint32_t x = c->gpr[a], y = c->gpr[b], s = c->gpr[d], r = 0, v, pa;
    bool oe = (op & 0x400u) != 0, rc = (op & 1u) != 0, overflow = false, arithmetic = true;
    uint64_t sum;
    int64_t exact;
    /* MPC601UM chapter 10: XO-form arithmetic uses bit21 for OE;
       X-form opcodes use all ten XO bits and must not alias XO-form. */
    switch (base) {
    case 8: case 10: case 40: case 104: case 136: case 138:
    case 200: case 202: case 232: case 234: case 266:
        if (base == 104) { sum = (uint64_t)(~x) + 1u; exact = -(int64_t)signed32(x); }
        else if (base == 8 || base == 40 || base == 136 || base == 200 || base == 232) {
            uint32_t add = base == 200 ? 0u : base == 232 ? UINT32_MAX : y;
            unsigned cin = base == 8 || base == 40 ? 1u : (c->xer & PPC_XER_CA ? 1u : 0u);
            sum = (uint64_t)(~x) + add + cin;
            exact = -(int64_t)signed32(x) + signed32(add) + (int64_t)cin - 1;
        } else {
            uint32_t add = base == 202 ? 0u : base == 234 ? UINT32_MAX : y;
            unsigned cin = base == 138 || base == 202 || base == 234 ? (c->xer & PPC_XER_CA ? 1u : 0u) : 0u;
            sum = (uint64_t)x + add + cin;
            exact = (int64_t)signed32(x) + signed32(add) + cin;
        }
        r = (uint32_t)sum;
        overflow = exact > INT32_MAX || exact < INT32_MIN;
        if (base != 40 && base != 104 && base != 266) carry(c, (sum >> 32) != 0);
        break;
    case 235:
        exact = (int64_t)signed32(x) * signed32(y);
        r = (uint32_t)exact; overflow = exact > INT32_MAX || exact < INT32_MIN; break;
    case 459:
        overflow = y == 0; r = y ? x / y : 0u; break;
    case 491:
        overflow = !y || (x == 0x80000000u && y == UINT32_MAX);
        r = overflow ? 0u : (uint32_t)(signed32(x) / signed32(y)); break;
    default: arithmetic = false; break;
    }
    if (arithmetic) {
        c->gpr[d] = r;
        if (oe) ppc_overflow(c, overflow);
        if (rc) ppc_record(c, r);
        return true;
    }
    switch (xo) {
    case 0: case 32:
        if (op & 0x00200000u) illegal(c);
        else compare(c, d >> 2, x, y, xo == 0);
        return true;
    case 4:
        if (trap(d, x, y)) ppc_exception(c, PPC_EX_PROGRAM, PROGRAM_TRAP, c->last_pc);
        return true;
    case 11: c->gpr[d] = (uint32_t)(((uint64_t)x * y) >> 32); if (rc) ppc_record(c, c->gpr[d]); return true;
    case 75: c->gpr[d] = (uint32_t)((uint64_t)((int64_t)signed32(x) * signed32(y)) >> 32); if (rc) ppc_record(c, c->gpr[d]); return true;
    case 19: c->gpr[d] = c->cr; return true;
    case 83: if (privileged(c)) c->gpr[d] = c->msr; return true;
    case 144:
        for (unsigned f = 0; f < 8; ++f) if (op & (1u << (19u - f))) cr_field(c, f, s >> (28u - 4u * f));
        return true;
    case 146:
        if (privileged(c)) {
            if (c->model == PPC_MODEL_601) c->msr = s & MSR_601;
            else ppc_model_set_msr(c, s, false);
        }
        return true;
    case 210: if (privileged(c)) c->sr[a & 15u] = s; return true;
    case 242: if (privileged(c)) c->sr[y >> 28] = s; return true;
    case 339: if (spr_read(c, ((op >> 16) & 31u) | ((op >> 6) & 992u), &v)) c->gpr[d] = v; return true;
    case 467: spr_write(c, ((op >> 16) & 31u) | ((op >> 6) & 992u), s); return true;
    case 512: cr_field(c, d >> 2, c->xer >> 28); c->xer &= UINT32_C(0x1fffffff); return true;
    case 595: if (privileged(c)) c->gpr[d] = c->sr[a & 15u]; return true;
    case 659: if (privileged(c)) c->gpr[d] = c->sr[y >> 28]; return true;
    case 371: illegal(c); return true; /* mftb is not implemented by 601, UM appendix C. */
    case 20: {
        uint32_t ea = (a ? x : 0u) + y;
        if (ea & 3u) { ppc_alignment(c, ea); return true; }
        if (!ppc_translate(c, ea, PPC_READ, &pa) || !ppc_data_breakpoint(c, ea, 4, false, false)) return true;
        if (ppc_load(c, ea, 4, &v)) {
            c->gpr[d] = v; c->reservation = pa & ~31u; c->reserved = true;
        }
        return true;
    }
    case 150: {
        uint32_t ea = (a ? x : 0u) + y;
        bool success = false, had = c->reserved;
        c->reserved = false;
        if (!rc) { illegal(c); return true; }
        if (ea & 3u) { ppc_alignment(c, ea); return true; }
        if (ppc_translate(c, ea, PPC_WRITE, &pa)) {
            /* MPC601UM 3.5.7: on 601 the stwcx address need not match. */
            /* MPC601UM table 2-26: without the reservation no DABR match;
               PEM 6.4.3 leaves it undefined, the same rule is kept. */
            if (had && ppc_reservation_matches(c, pa)) {
                if (!ppc_data_breakpoint(c, ea, 4, true, false)) return true;
                success = ppc_store(c, ea, 4, s);
            }
            if (c->result == PPC_OK) cr_field(c, 0, (success ? 2u : 0u) | (c->xer >> 31));
        }
        return true;
    }
    case 23: case 55: return memory(c, op, 4, false, xo == 55, true, false, false, false), true;
    case 87: case 119: return memory(c, op, 1, false, xo == 119, true, false, false, false), true;
    case 151: case 183: return memory(c, op, 4, true, xo == 183, true, false, false, false), true;
    case 215: case 247: return memory(c, op, 1, true, xo == 247, true, false, false, false), true;
    case 279: case 311: return memory(c, op, 2, false, xo == 311, true, false, false, false), true;
    case 343: case 375: return memory(c, op, 2, false, xo == 375, true, true, false, false), true;
    case 407: case 439: return memory(c, op, 2, true, xo == 439, true, false, false, false), true;
    case 534: return memory(c, op, 4, false, false, true, false, true, false), true;
    case 790: return memory(c, op, 2, false, false, true, false, true, false), true;
    case 662: return memory(c, op, 4, true, false, true, false, true, false), true;
    case 918: return memory(c, op, 2, true, false, true, false, true, false), true;
    case 533: case 597: return string_access(c, op, false, xo == 533), true;
    case 661: case 725: return string_access(c, op, true, xo == 661), true;
    case 535: case 567: return memory(c, op, 4, false, xo == 567, true, false, false, true), true;
    case 599: case 631: return memory(c, op, 8, false, xo == 631, true, false, false, true), true;
    case 663: case 695: return memory(c, op, 4, true, xo == 695, true, false, false, true), true;
    case 727: case 759: return memory(c, op, 8, true, xo == 759, true, false, false, true), true;
    case 310: case 438:
        if (!(c->ear & 0x80000000u)) { c->dar = (a ? x : 0u) + y; c->dsisr = xo == 438 ? 0x02400000u : 0x00400000u; ppc_exception(c, PPC_EX_DSI, 0, c->last_pc); }
        else memory(c, op, 4, xo == 438, false, true, false, false, false);
        return true;
    case 54: case 86: case 246: case 278: case 598: case 854: case 982:
        /* MPC601UM 4.5, 10: coherent cache, no outstanding stores. */
        return true;
    case 470: case 306: if (privileged(c)) { /* No cached translation entries yet. */ } return true;
    case 370: case 566: case 983: illegal(c); return true; /* 601 UM table10-6. */
    case 1014: {
        uint32_t address = (a ? x : 0u) + y, ea = address & ~31u;
        /* MPC601UM 10 dcbz: T segments are no-ops; W/I mappings fault. */
        if ((c->model == PPC_MODEL_601 || (c->msr & PPC_MSR_DR)) &&
            (c->sr[address >> 28] & 0x80000000u)) return true;
        if (!ppc_translate(c, address, PPC_WRITE, &pa)) return true;
        if (c->mmu_wim & 0x60u) { ppc_alignment(c, address); return true; }
        for (unsigned i = 0; i < 32; i += 4) if (!ppc_store(c, ea + i, 4, 0)) break;
        return true;
    }
    case 24: r = (y & 32u) ? 0u : s << (y & 31u); break;
    case 26: r = 0; while (r < 32u && !(s & (0x80000000u >> r))) ++r; break;
    case 28: r = s & y; break;
    case 60: r = s & ~y; break;
    case 124: r = ~(s | y); break;
    case 284: r = ~(s ^ y); break;
    case 316: r = s ^ y; break;
    case 412: r = s | ~y; break;
    case 444: r = s | y; break;
    case 476: r = ~(s & y); break;
    case 536: r = (y & 32u) ? 0u : s >> (y & 31u); break;
    case 792: case 824: {
        unsigned n = xo == 824 ? b : y & 63u;
        uint32_t fill = s & 0x80000000u ? UINT32_MAX : 0u;
        r = n >= 32u ? fill : n ? (s >> n) | (fill << (32u - n)) : s;
        carry(c, (s & 0x80000000u) && (n >= 32u || (n && (s & (UINT32_MAX >> (32u - n))))));
        break;
    }
    case 922: r = (uint32_t)signed16(s); break;
    case 954: r = s & 255u; if (r & 128u) r |= UINT32_C(0xffffff00); break;
    default: return false;
    }
    c->gpr[a] = r;
    if (rc) ppc_record(c, r);
    return true;
}
ppc_result ppc_step(ppc_cpu *c)
{
    uint32_t op, pa;
    c->result = PPC_OK;
    c->last_pc = c->pc;
    c->last_exception = 0;
    if (c->msr & PPC_MSR_EE) {
        if (c->external_irq) { ppc_exception(c, PPC_EX_EXTERNAL, 0, c->pc); return c->result; }
        if (c->decrementer_pending) { c->decrementer_pending = false; ppc_exception(c, PPC_EX_DECREMENTER, 0, c->pc); return c->result; }
    }
    if (ppc_power_waiting(c)) return c->result;
    if (!ppc_translate(c, c->pc, PPC_FETCH, &pa)) return c->result;
    if (!c->bus.read || !c->bus.read(c->bus.opaque, pa, 4, &op)) {
        ppc_exception(c, PPC_EX_MACHINE, 0, c->pc); return c->result;
    }
    c->last_opcode = op;
    /* MPC750UM table 4-3: fetch exceptions, then the breakpoint. */
    if (instruction_breakpoint(c, op)) return c->result;
    return ppc_execute(c, op);
}
ppc_result ppc_execute(ppc_cpu *c, uint32_t op)
{
    uint32_t x, y, r, imm, old_msr;
    unsigned primary, d, a, b;
    bool handled = true;
    old_msr = c->msr;
    c->pc += 4u;
    primary = op >> 26; d = (op >> 21) & 31u; a = (op >> 16) & 31u; b = (op >> 11) & 31u;
    x = c->gpr[a]; y = c->gpr[b]; imm = (uint32_t)signed16(op);
    /* Instruction encodings and semantics: MPC601UM chapter 10; PEM ch8. */
    if (c->model != PPC_MODEL_601 && ppc_model_decode(c, op)) goto finished;
    switch (primary) {
    case 0: illegal(c); break;
    case 3: if (trap(d, x, imm)) ppc_exception(c, PPC_EX_PROGRAM, PROGRAM_TRAP, c->last_pc); break;
    case 7: c->gpr[d] = (uint32_t)((int64_t)signed32(x) * signed16(op)); break;
    case 8: { uint64_t sum = (uint64_t)(~x) + imm + 1u; c->gpr[d] = (uint32_t)sum; carry(c, (sum >> 32) != 0); break; }
    case 10: case 11:
        if (op & 0x00200000u) illegal(c);
        else compare(c, d >> 2, x, primary == 10 ? op & 65535u : imm, primary == 11);
        break;
    case 12: case 13: { uint64_t sum = (uint64_t)x + imm; r = (uint32_t)sum; carry(c, (sum >> 32) != 0); c->gpr[d] = r; if (primary == 13) ppc_record(c, r); break; }
    case 14: c->gpr[d] = (a ? x : 0u) + imm; break;
    case 15: c->gpr[d] = (a ? x : 0u) + (imm << 16); break;
    case 16: {
        uint32_t displacement = op & 0xfffcu;
        if (displacement & 0x8000u) displacement |= UINT32_C(0xffff0000);
        if (branch_condition(c, d, a)) c->pc = (op & 2u ? 0u : c->last_pc) + displacement;
        if (op & 1u) c->lr = c->last_pc + 4u;
        break;
    }
    case 17: ppc_exception(c, PPC_EX_SYSCALL, 0, c->pc); break;
    case 18: {
        uint32_t displacement = op & 0x03fffffcu;
        if (displacement & 0x02000000u) displacement |= UINT32_C(0xfc000000);
        c->pc = (op & 2u ? 0u : c->last_pc) + displacement;
        if (op & 1u) c->lr = c->last_pc + 4u;
        break;
    }
    case 19: handled = xl(c, op); break;
    case 20: case 21: case 23: {
        uint32_t m = mask((op >> 6) & 31u, (op >> 1) & 31u);
        r = rotate(c->gpr[d], primary == 23 ? y : b) & m;
        if (primary == 20) r |= x & ~m;
        c->gpr[a] = r;
        if (op & 1u) ppc_record(c, r);
        break;
    }
    case 24: c->gpr[a] = c->gpr[d] | (op & 65535u); break;
    case 25: c->gpr[a] = c->gpr[d] | (op << 16); break;
    case 26: c->gpr[a] = c->gpr[d] ^ (op & 65535u); break;
    case 27: c->gpr[a] = c->gpr[d] ^ (op << 16); break;
    case 28: case 29: r = c->gpr[d] & (primary == 28 ? op & 65535u : op << 16); c->gpr[a] = r; ppc_record(c, r); break;
    case 31: handled = extended(c, op); break;
    case 32: case 33: memory(c, op, 4, false, primary == 33, false, false, false, false); break;
    case 34: case 35: memory(c, op, 1, false, primary == 35, false, false, false, false); break;
    case 36: case 37: memory(c, op, 4, true, primary == 37, false, false, false, false); break;
    case 38: case 39: memory(c, op, 1, true, primary == 39, false, false, false, false); break;
    case 40: case 41: memory(c, op, 2, false, primary == 41, false, false, false, false); break;
    case 42: case 43: memory(c, op, 2, false, primary == 43, false, true, false, false); break;
    case 44: case 45: memory(c, op, 2, true, primary == 45, false, false, false, false); break;
    case 46: case 47: multiple(c, (a ? x : 0u) + imm, d, primary == 47); break;
    case 48: case 49: memory(c, op, 4, false, primary == 49, false, false, false, true); break;
    case 50: case 51: memory(c, op, 8, false, primary == 51, false, false, false, true); break;
    case 52: case 53: memory(c, op, 4, true, primary == 53, false, false, false, true); break;
    case 54: case 55: memory(c, op, 8, true, primary == 55, false, false, false, true); break;
    case 59: case 63:
        if (!(c->msr & PPC_MSR_FP)) ppc_exception(c, PPC_EX_FPU, 0, c->last_pc);
        else handled = ppc_fpu(c, op);
        break;
    default: handled = false; break;
    }
finished:
    if (!handled && c->model == PPC_MODEL_601) handled = ppc_power(c, op);
    if (!handled) { c->pc = c->last_pc; c->result = PPC_UNIMPLEMENTED; }
    if (c->result == PPC_OK) {
        bool returning = primary == 19 && ((op >> 1) & 1023u) == 50;
        bool synchronizing = c->model == PPC_MODEL_750 && primary == 19 && ((op >> 1) & 1023u) == 150;
        bool branch = primary == 16 || primary == 18 ||
            (primary == 19 && (((op >> 1) & 1023u) == 16 || ((op >> 1) & 1023u) == 528));
        ++c->instructions;
        if (((old_msr & PPC_MSR_SE) && !returning && !synchronizing) ||
            (c->model != PPC_MODEL_601 && (old_msr & PPC_MSR_BE) && branch))
            ppc_exception(c, c->model == PPC_MODEL_601 ? PPC_EX_RUN_MODE : PPC_EX_TRACE, 0, c->pc);
    }
    return c->result;
}
