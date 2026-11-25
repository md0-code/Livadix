/* The floating-point unit: the 68881/68882 coprocessors beside a 68020 or
 * 68030, the 68040's own FPU and the 68LC040 without one.
 *
 * Sources: MC68881/MC68882 User's Manual (2 programming model: FPCR, FPSR
 * condition codes, quotient, EXC and AEXC bytes and their equations, FPIAR;
 * 3 data formats; 4.4 conditional tests; 4.5.4 NaNs; 4.5.5 post processing;
 * 4.6 the instructions and their operation tables; 4.8-4.9 encodings; 6.1
 * coprocessor-detected exceptions, pre- and mid-instruction reporting,
 * trap-enabled and trap-disabled results, recovery; 6.4 FSAVE/FRESTORE, the
 * null, idle and busy state frames, BIU flag bit 27 (exception pending),
 * format words $1F18/$3F18 (68881) and $1F38 (68882)). M68000 PRM section 5
 * (encodings, FMOVEM register lists, FDBcc, FTRAPcc, FMOVECR offsets) and
 * 8.2 (coprocessor frame format $9). MC68040 User's Manual section 9 (9.2
 * programming model, 9.5.2 the 32 conditional tests, 9.6.1 unimplemented
 * instructions (Table 9-10) with their format $2 frame, 9.6.2 unsupported
 * data types (vector 55, format $0 or $3), 9.7 the arithmetic exceptions,
 * 9.8 the null, idle, unimplemented-instruction and busy state frames
 * (Figure 9-10, Table 9-16, the CMDREG3B mapping of Figure 9-11, the
 * denormal layout of Figure 9-9)), 8.4.4 format $3 and appendix A
 * (68LC040: every FP instruction a line F with the format $4 frame).
 *
 * Model choices: instructions complete at once, so a 68881/2
 * never saves a busy frame; an enabled exception is held pending (EXC PEND)
 * and reported at the next FP instruction, or at once for FMOVE to memory
 * (format $9). The 68040 traps what its manual lists as unimplemented or
 * unsupported to the system's FPSP unless the full-set option is on; the
 * exceptions it calls nonmaskable are reported only when enabled, with the
 * default results the FPSP would store computed here. */
#include "m68k_internal.h"
#include "fpu_soft.h"
#include "fpu_trans.h"

enum { FPS_NULL, FPS_IDLE, FPS_UNIMP, FPS_BUSY };

/* Exception vectors (MC68881 UM Table 6-1; MC68040 UM Table 9-9). */
enum { V_BSUN = 48, V_INEX = 49, V_DZ = 50, V_UNFL = 51, V_OPERR = 52, V_OVFL = 53, V_SNAN = 54, V_UNSUPP = 55 };

/* Operand formats of the source specifier (MC68881 UM 4.8). */
enum { F_L, F_S, F_X, F_P, F_W, F_D, F_B, F_PDYN };
static const uint8_t fmt_size[8] = {4, 4, 12, 12, 2, 8, 1, 12};

#define FPCR_MASK 0x0000FFF0u
#define FPSR_MASK 0x0FFFFFF8u
#define CC_N 0x08000000u
#define CC_Z 0x04000000u
#define CC_I 0x02000000u
#define CC_NAN 0x01000000u

typedef struct operand {
    fpx v;
    m68k_fx80 raw;      /* the operand as an extended image (ETEMP) */
    uint8_t tag;        /* STAG/DTAG encoding */
    uint8_t unsupported;/* denormal, unnormal or packed (68040) */
    uint32_t packed[3];
    uint8_t is_packed;
} operand;

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"   /* the 68040 test is kept for the unimplemented 040 paths */
#endif
static int is_040(const m68k_cpu *cpu) { return cpu->model == M68K_68040 || cpu->model == M68K_68LC040; }

static const fpfmt *prec_fmt(const m68k_fpu *f) {
    switch ((f->fpcr >> 6) & 3u) {
    case 1: return &FMT_S;
    case 2: return &FMT_D;
    default: return &FMT_X;
    }
}
static unsigned rnd(const m68k_fpu *f) { return (f->fpcr >> 4) & 3u; }

/* Highest-priority enabled exception's vector (MC68881 UM 6.1: BSUN highest,
 * INEX lowest), 0 if none. */
static unsigned exc_vector(unsigned bits) {
    if (bits & FX_BSUN) return V_BSUN;
    if (bits & FX_SNAN) return V_SNAN;
    if (bits & FX_OPERR) return V_OPERR;
    if (bits & FX_OVFL) return V_OVFL;
    if (bits & FX_UNFL) return V_UNFL;
    if (bits & FX_DZ) return V_DZ;
    if (bits & (FX_INEX1 | FX_INEX2)) return V_INEX;
    return 0;
}

/* The EXC byte of an instruction and the AEXC equations (MC68881 UM 2.3.4). */
static void post_exc(m68k_fpu *f, unsigned exc) {
    unsigned a = 0;
    if (exc & (FX_BSUN | FX_SNAN | FX_OPERR)) a |= 0x80u;
    if (exc & FX_OVFL) a |= 0x40u;
    if ((exc & FX_UNFL) && (exc & FX_INEX2)) a |= 0x20u;
    if (exc & FX_DZ) a |= 0x10u;
    if (exc & (FX_INEX1 | FX_INEX2 | FX_OVFL)) a |= 0x08u;
    f->fpsr = (f->fpsr & ~0x0000FF00u) | (exc & 0xFF00u) | a;
}

static void set_cc(m68k_fpu *f, fpx r) {
    uint32_t cc = r.s ? CC_N : 0;
    if (r.k == FX_ZERO) cc |= CC_Z;
    else if (r.k == FX_INF) cc |= CC_I;
    else if (r.k == FX_NAN) cc |= CC_NAN;
    f->fpsr = (f->fpsr & ~0x0F000000u) | cc;
}

/* STAG/DTAG of an extended image (MC68040 UM 9.8). */
static unsigned tag_of(m68k_fx80 r) {
    const unsigned be = r.se & 0x7FFFu;
    if (be == 0x7FFF) return (r.m & 0x7FFFFFFFFFFFFFFFull) ? 3u : 2u;
    if (r.m == 0) return be ? 4u : 1u;          /* an unnormal zero counts as unnormal */
    if (!(r.m >> 63)) return 4u;                /* denormal or unnormal */
    return 0;
}

/* --- reset, type -------------------------------------------------------------------- */

void m68k_fpu_reset(m68k_cpu *cpu) {
    m68k_fpu *f = &cpu->fpu;
    const uint8_t type = f->type, full = f->full;
    memset(f, 0, sizeof *f);
    f->type = type;
    f->full = full;
    for (unsigned i = 0; i < 8; i++) {
        f->fp[i].se = 0x7FFF;
        f->fp[i].m = 0xFFFFFFFFFFFFFFFFull;
    }
    f->state = FPS_NULL;
}

void m68k_set_fpu(m68k_cpu *cpu, m68k_fpu_type type, int full) {
    cpu->fpu.type = (uint8_t)type;
    cpu->fpu.full = (uint8_t)(type == M68K_FPU_68040 && full);
    m68k_fpu_reset(cpu);
}

/* --- operand transfer ------------------------------------------------------------------ */

/* The words of a source operand: from Dn, the instruction stream or memory.
 * *ea gets the effective address (0 for Dn and immediate). Returns 0 for an
 * addressing mode the format does not allow. */
static int fetch_operand(m68k_cpu *cpu, unsigned mode, unsigned reg, unsigned fmt, uint32_t w[3], uint32_t *ea) {
    const unsigned size = fmt_size[fmt];
    *ea = 0;
    w[0] = w[1] = w[2] = 0;
    if (mode == 0) {
        if (size > 4) return 0;
        w[0] = D(reg) & (size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu);
        return 1;
    }
    if (mode == 1) return 0;
    if (mode == 7 && reg == 4) {
        if (size == 1) w[0] = fetch16(cpu) & 0xFFu;
        else if (size == 2) w[0] = fetch16(cpu);
        else for (unsigned i = 0; i < size / 4; i++) w[i] = fetch32(cpu);
        return 1;
    }
    if (mode == 7 && reg > 4) return 0;
    const uint32_t a = ea_addr(cpu, mode, reg, size);
    *ea = a;
    if (size == 1) w[0] = rd8(cpu, a);
    else if (size == 2) w[0] = rd16(cpu, a);
    else for (unsigned i = 0; i < size / 4; i++) w[i] = rd32(cpu, a + 4 * i);
    return 1;
}

/* Converts source words to an operand (MC68881 UM 3.5.1). */
static void make_operand(unsigned fmt, const uint32_t w[3], operand *o, unsigned mode, unsigned *exc) {
    memset(o, 0, sizeof *o);
    switch (fmt) {
    case F_L: o->v = fx_from_int((int32_t)w[0]); break;
    case F_W: o->v = fx_from_int((int16_t)w[0]); break;
    case F_B: o->v = fx_from_int((int8_t)w[0]); break;
    case F_S: {
        o->v = fx_from_s(w[0]);
        if (((w[0] >> 23) & 0xFF) == 0 && (w[0] & 0x7FFFFF)) {
            o->unsupported = 1;
            o->tag = 5;
            o->raw.se = (uint16_t)((w[0] >> 31) << 15);
            o->raw.m = (uint64_t)(w[0] & 0x7FFFFF) << 40;
            return;
        }
        break;
    }
    case F_D: {
        const uint64_t d = (uint64_t)w[0] << 32 | w[1];
        o->v = fx_from_d(d);
        if (((d >> 52) & 0x7FF) == 0 && (d & 0xFFFFFFFFFFFFFull)) {
            o->unsupported = 1;
            o->tag = 5;
            o->raw.se = (uint16_t)((d >> 63) << 15);
            o->raw.m = (d & 0xFFFFFFFFFFFFFull) << 11;
            return;
        }
        break;
    }
    case F_X: {
        m68k_fx80 r;
        r.se = (uint16_t)(w[0] >> 16);
        r.m = (uint64_t)w[1] << 32 | w[2];
        o->raw = r;
        o->tag = (uint8_t)tag_of(r);
        o->unsupported = o->tag == 4;
        o->v = fx_from_x(r);
        return;
    }
    default:                                    /* packed */
        o->is_packed = 1;
        o->unsupported = 1;
        memcpy(o->packed, w, sizeof o->packed);
        o->raw.se = 0;
        o->raw.m = (uint64_t)w[1] << 32 | w[2];
        o->v = ft_from_packed(w, mode, exc);
        return;
    }
    o->raw = fx_to_x(o->v);
    o->tag = (uint8_t)tag_of(o->raw);
}

static void reg_operand(const m68k_fpu *f, unsigned r, operand *o) {
    o->is_packed = 0;
    o->packed[0] = o->packed[1] = o->packed[2] = 0;
    o->raw = f->fp[r];
    o->tag = (uint8_t)tag_of(o->raw);
    o->unsupported = o->tag == 4;
    o->v = fx_from_x(o->raw);
}

/* --- exceptions ------------------------------------------------------------------------ */

static uint16_t cmd3_of(uint16_t c1) {
    /* Figure 9-11: CMDREG3B = 0, DST, CMD with bits 5-2 of CMD rotated. */
    const unsigned cmd = c1 & 0x7Fu;
    const unsigned b = (cmd & 0x40u) | ((cmd & 0x04u) << 3) | ((cmd & 0x20u) >> 1) | ((cmd & 0x10u) >> 1) |
                       ((cmd & 0x08u) >> 1) | (cmd & 0x03u);
    return (uint16_t)((c1 & 0x0380u) | b);
}

static uint16_t cmd1_of(uint16_t ext) {
    /* FSQRT is $5 in CMDREG1B (MC68040 UM 9.8). */
    return (ext & 0x7Fu) == 0x04u ? (uint16_t)((ext & ~0x7Fu) | 0x05u) : ext;
}

/* Fills the 68040 frame fields for an E1 exception (CU) or E3 (WB). */
static void frame_e1(m68k_fpu *f, uint16_t ext, const operand *src, const operand *dst, int t) {
    f->cmdreg1b = cmd1_of(ext);
    f->etemp = src ? src->raw : f->etemp;
    f->stag = src ? src->tag : 0;
    if (src && src->is_packed) {
        f->etemp.se = 0;
        f->etemp.m = (uint64_t)src->packed[1] << 32 | src->packed[2];
        f->fptemp.se = 0;
        f->fptemp.m = src->packed[0];
    } else if (dst) {
        f->fptemp = dst->raw;
        f->dtag = dst->tag;
    }
    f->e1 = 1;
    f->e3 = 0;
    f->t = (uint8_t)t;
}
static void frame_e3(m68k_fpu *f, uint16_t ext, fpx inter) {
    f->cmdreg3b = cmd3_of(cmd1_of(ext));
    f->e3 = 1;
    f->e1 = 0;
    f->t = 0;
    /* WBTEMP: sign, the 16-bit biased exponent (bit 15 apart), the mantissa
     * and its guard, round and sticky bits. */
    const int32_t be = inter.e + 16383;
    f->wbtemp.se = (uint16_t)((inter.s ? 0x8000u : 0) | ((uint32_t)be & 0x7FFFu));
    f->wbte15 = (uint8_t)(((uint32_t)be >> 15) & 1u);
    f->wbtemp.m = inter.m;
    f->wbm66 = 0;
    f->wbm1 = (uint8_t)(inter.lo >> 63);
    f->wbm0 = (uint8_t)((inter.lo >> 62) & 1u);
    f->sbit = (uint8_t)((inter.lo & 0x3FFFFFFFFFFFFFFFull) != 0);
}

/* An exception that interrupts the FP instruction about to execute: the
 * instruction is undone and restarted after the handler. */
static void pre_instruction(m68k_cpu *cpu, unsigned vector) {
    m68k_exception_restart(cpu, vector);
}

/* 68881/68882 and 68040: a pending exception is reported when the next FP
 * instruction starts (MC68881 UM 6.4.2.2; MC68040 UM 9.7). Returns 1 if
 * one was taken. The 68881 clears EXC PEND on the acknowledge, the 68882
 * keeps it until the handler's FSAVE. */
static int take_pending(m68k_cpu *cpu) {
    m68k_fpu *f = &cpu->fpu;
    if (!f->pending) return 0;
    const unsigned v = f->pend_vec ? f->pend_vec : exc_vector(f->fpsr & f->fpcr & 0xFF00u);
    if (!v) {
        f->pending = 0;
        return 0;
    }
    if (f->type == M68K_FPU_68881) f->pending = 0;
    if (f->type == M68K_FPU_68040) f->pending = 0;
    pre_instruction(cpu, v);
    return 1;
}

/* After an arithmetic instruction with an FP register destination: an
 * enabled exception is posted for the next FP instruction. */
static void post_pending(m68k_fpu *f, unsigned exc) {
    const unsigned en = exc & f->fpcr & 0xFF00u;
    if (!en) return;
    f->pending = 1;
    f->pend_vec = (uint8_t)exc_vector(en);
    f->pend_concurrent = f->type == M68K_FPU_68882;
    if (f->type == M68K_FPU_68040) f->state = FPS_BUSY;
}

/* --- the 68LC040: every FP instruction a format $4 line F (MC68040 UM A.5) ------------ */

static void lc040_trap(m68k_cpu *cpu, uint32_t ea) {
    m68k_undo_steps(cpu);
    m68k_exception_fp_unavailable(cpu, ea);
}

/* --- general instructions (type 000) -------------------------------------------------- */

/* Opmodes the 68881/68882 define; the 68040 adds $40-$6F single/double
 * rounding forms (MC68040 UM 9.2; PRM 5). */
static int opmode_valid(unsigned m, int e40) {
    static const uint8_t ok[64] = {
        1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1,   /* $00-$0F */
        1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1,   /* $10-$1F */
        1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,   /* $20-$2F */
        1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0};  /* $30-$3F */
    if (m < 64) return ok[m];
    if (!e40) return 0;
    switch (m) {
    case 0x40: case 0x41: case 0x44: case 0x45: case 0x58: case 0x5A: case 0x5C: case 0x5E:
    case 0x60: case 0x62: case 0x63: case 0x64: case 0x66: case 0x67: case 0x68: case 0x6C:
        return 1;
    default:
        return 0;
    }
}

/* What the 68040 executes itself (Table 9-10 lists the others). */
static int native_040(unsigned m) {
    switch (m) {
    case 0x00: case 0x04: case 0x18: case 0x1A: case 0x20: case 0x22: case 0x23: case 0x24: case 0x27:
    case 0x28: case 0x38: case 0x3A:
        return 1;
    default:
        return m >= 0x40;
    }
}

/* The operations whose overflow, underflow and inexact results the 68040
 * reports from its write-back stage (E3; MC68040 UM 9.8 E3, Table 9-16):
 * FADD, FSUB, FMUL, FDIV, FSQRT and their single/double forms. */
static int e3_op(unsigned m) {
    switch (m) {
    case 0x04: case 0x20: case 0x22: case 0x23: case 0x28:
    case 0x41: case 0x45: case 0x60: case 0x62: case 0x63: case 0x64: case 0x66: case 0x67: case 0x68: case 0x6C:
        return 1;
    default:
        return 0;
    }
}

/* The 68881/2 instructions an FSAVE right after them finds still running
 * (general, save_frame): FSQRT, the transcendental functions, FDIV, FSGLDIV,
 * FMOD, FREM. */
static int long_op(unsigned m) {
    if (m >= 0x30 && m <= 0x37) return 1;                       /* FSINCOS */
    switch (m) {
    case 0x02: case 0x04: case 0x06: case 0x08: case 0x09: case 0x0A: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
    case 0x10: case 0x11: case 0x12: case 0x14: case 0x15: case 0x16: case 0x19: case 0x1C: case 0x1D: case 0x1E:
    case 0x20: case 0x21: case 0x24: case 0x25:
        return 1;
    default:
        return 0;
    }
}
static int dyadic(unsigned m) { return (m >= 0x20 && m <= 0x28) || m == 0x38 || (m >= 0x60 && m <= 0x6C); }

/* Rounding of each opmode: the FPCR precision, or forced single (FS*) or
 * double (FD*) on the 68040. */
static const fpfmt *op_fmt(const m68k_fpu *f, unsigned m) {
    if (m >= 0x40) return (m & 0x04u) ? &FMT_D : &FMT_S;
    return prec_fmt(f);
}

static fpx nan_result(fpx a, fpx b, int dyad, unsigned *exc) {
    /* MC68881 UM 4.5.4: a NaN operand is the result; with two, the
     * destination's; a signaling NaN sets SNAN and is quieted. */
    if (fx_is_snan(a) || (dyad && fx_is_snan(b))) *exc |= FX_SNAN;
    if (dyad && b.k == FX_NAN) return fx_quiet(b);
    return fx_quiet(a);
}

static fpx sgl_trunc(fpx v) {
    if (v.k == FX_NORM) {
        v.m &= 0xFFFFFF0000000000ull;
        v.lo = 0;
    }
    return v;
}

/* Computes opmode m on src (and dst); the unrounded result and its format. */
static fpx compute(m68k_fpu *f, unsigned m, fpx s, fpx d, unsigned *exc, unsigned *qbyte, int *qset,
                   const fpfmt **fmt) {
    *fmt = op_fmt(f, m);
    const unsigned mode = rnd(f);
    unsigned b = m;
    if (m >= 0x40) {
        switch (m & ~0x04u) {
        case 0x40: b = 0x00; break;
        case 0x41: b = 0x04; break;
        case 0x58: b = 0x18; break;
        case 0x5A: b = 0x1A; break;
        case 0x60: b = 0x20; break;
        case 0x62: b = 0x22; break;
        case 0x63: b = 0x23; break;
        case 0x68: b = 0x28; break;
        default: break;
        }
    }
    switch (b) {
    case 0x00: return s;                                           /* FMOVE */
    case 0x01: return fx_round_int(s, mode, exc);                  /* FINT */
    case 0x03: return fx_round_int(s, RND_Z, exc);                 /* FINTRZ */
    case 0x04: return fx_sqrt(s, exc);                             /* FSQRT */
    case 0x18: s.s = 0; return s;                                  /* FABS */
    case 0x1A: s.s ^= 1; return s;                                 /* FNEG */
    case 0x1E:                                                     /* FGETEXP */
        if (s.k == FX_INF) { *exc |= FX_OPERR; return fx_default_nan(); }
        if (s.k == FX_ZERO) return s;
        return fx_from_int(s.e);
    case 0x1F:                                                     /* FGETMAN */
        if (s.k == FX_INF) { *exc |= FX_OPERR; return fx_default_nan(); }
        if (s.k == FX_ZERO) return s;
        s.e = 0;
        return s;
    case 0x20: return fx_div(d, s, exc);                           /* FDIV */
    case 0x21: *qset = 1; return fx_rem(d, s, 0, exc, qbyte);      /* FMOD */
    case 0x22: return fx_add(d, s, mode, exc);                     /* FADD */
    case 0x23: return fx_mul(d, s, exc);                           /* FMUL */
    case 0x24: {                                                   /* FSGLDIV */
        static const fpfmt sgl = {24, -16383, 16383};
        *fmt = &sgl;
        return fx_div(sgl_trunc(d), sgl_trunc(s), exc);
    }
    case 0x25: *qset = 1; return fx_rem(d, s, 1, exc, qbyte);      /* FREM */
    case 0x26:                                                     /* FSCALE */
        if (s.k == FX_INF) { *exc |= FX_OPERR; return fx_default_nan(); }
        if (d.k != FX_NORM || s.k == FX_ZERO) return d;
        {
            unsigned e2 = 0;
            const fpx n = fx_round_int(s, RND_Z, &e2);
            int32_t k = 0;
            if (n.k == FX_NORM) k = n.e >= 20 ? 0x100000 : (int32_t)(n.m >> (63 - n.e));
            if (n.s) k = -k;
            return fx_scale(d, k);
        }
    case 0x27: {                                                   /* FSGLMUL */
        static const fpfmt sgl = {24, -16383, 16383};
        *fmt = &sgl;
        return fx_mul(sgl_trunc(d), sgl_trunc(s), exc);
    }
    case 0x28: s.s ^= 1; return fx_add(d, s, mode, exc);           /* FSUB */
    default:
        return ft_eval(b, s, exc);                                 /* transcendental */
    }
}

/* FCMP and FTST: condition codes only (MC68881 UM 4.6 FCMP: dst - src). */
static void compare(m68k_fpu *f, fpx d, fpx s, unsigned *exc) {
    if (d.k == FX_NAN || s.k == FX_NAN) {
        if (fx_is_snan(d) || fx_is_snan(s)) *exc |= FX_SNAN;
        f->fpsr = (f->fpsr & ~0x0F000000u) | CC_NAN;
        return;
    }
    fpx r;
    if (d.k == FX_INF && s.k == FX_INF && d.s == s.s) {
        r = fx_zero(d.s);                       /* equal infinities: Z, N as their sign */
    } else if (d.k == FX_INF) {
        r = fx_inf(d.s);
    } else if (s.k == FX_INF) {
        r = fx_inf(!s.s);
    } else {
        unsigned e2 = 0;
        fpx ns = s;
        ns.s ^= 1;
        r = fx_add(d, ns, RND_N, &e2);
        if (r.k == FX_ZERO) r.s = (uint8_t)(d.k == FX_ZERO && s.k == FX_ZERO ? (d.s & !s.s) : 0);
    }
    set_cc(f, r);
}

/* An extended image that is a normalized finite number or an exact zero
 * (no NaN, infinity, denormal or unnormal): the operands the common case
 * takes without the operand records. */
static int plain_x(m68k_fx80 x) {
    const unsigned be = x.se & 0x7FFFu;
    if (be == 0x7FFFu) return 0;
    return (x.m >> 63) ? be != 0 : (x.m == 0 && be == 0);
}
static fpx plain_fpx(m68k_fx80 x) {
    fpx v;
    v.s = (uint8_t)(x.se >> 15);
    v.m = x.m;
    v.lo = 0;
    if (!x.m) {
        v.k = FX_ZERO;
        v.e = 0;
    } else {
        v.k = FX_NORM;
        v.e = (int32_t)(x.se & 0x7FFFu) - 16383;
    }
    return v;
}

/* The arithmetic most code runs (FMOVE, FABS, FNEG, FADD, FSUB, FMUL,
 * FDIV, FCMP, FTST and the 68040's single and double forms of them) on
 * plain operands with no exception enabled, pending or being resumed:
 * what general() does for them, without its operand records. */
static int fast_op(const m68k_cpu *cpu, unsigned m) {
    const m68k_fpu *f = &cpu->fpu;
    if (f->pending || f->resume || (f->fpcr & 0xFF00u) || cpu->model == M68K_68LC040) return 0;
    switch (m) {
    case 0x00: case 0x18: case 0x1A: case 0x20: case 0x22: case 0x23: case 0x28: case 0x38: case 0x3A: return 1;
    case 0x40: case 0x44: case 0x58: case 0x5A: case 0x5C: case 0x5E:
    case 0x60: case 0x62: case 0x63: case 0x64: case 0x66: case 0x67: case 0x68: case 0x6C:
        return f->type == M68K_FPU_68040;
    default: return 0;
    }
}
static void fast_tail(m68k_cpu *cpu, uint32_t ext, fpx s, fpx d) {
    m68k_fpu *f = &cpu->fpu;
    const unsigned ry = (ext >> 7) & 7u, m = ext & 0x7Fu;
    unsigned exc = 0;
    f->fpiar = cpu->ipc;
    if (f->state == FPS_NULL || f->state == FPS_UNIMP) f->state = FPS_IDLE;
    if (f->type != M68K_FPU_68040 && long_op(m)) {
        f->busy_at = cpu->instructions + 1u;
        f->cmdreg1b = (uint16_t)ext;
        f->etemp = fx_to_x(s);
        f->fptemp = fx_to_x(d);
    }
    if (m == 0x38) {
        compare(f, d, s, &exc);
        post_exc(f, exc);
        return;
    }
    if (m == 0x3A) {
        set_cc(f, s);
        post_exc(f, exc);
        return;
    }
    const fpfmt *fmt;
    unsigned qbyte = 0;
    int qset = 0;
    fpx r = compute(f, m, s, d, &exc, &qbyte, &qset, &fmt);
    if (fmt == &FMT_X && r.k == FX_NORM && r.e >= -16383 && r.e <= 16382) {
        /* fx_round at extended precision, inline: a normalized result
         * clear of underflow and of overflow after rounding (the bounds
         * fx_round tests), rounded on its low bits in the FPCR's mode. */
        uint64_t mm = r.m;
        int32_t e = r.e;
        if (r.lo) {
            int up;
            switch (rnd(f)) {
            case RND_N: up = r.lo > 0x8000000000000000ull || (r.lo == 0x8000000000000000ull && (mm & 1u)); break;
            case RND_Z: up = 0; break;
            case RND_M: up = r.s; break;
            default: up = !r.s; break;
            }
            exc |= FX_INEX2;
            if (up && ++mm == 0) {
                mm = 0x8000000000000000ull;
                e++;
            }
        }
        f->fp[ry].m = mm;
        f->fp[ry].se = (uint16_t)((r.s ? 0x8000u : 0) | (uint32_t)(e + 16383));
        f->fpsr = (f->fpsr & ~0x0F000000u) | (r.s ? CC_N : 0);
        post_exc(f, exc);
        return;
    }
    if (r.k == FX_NORM) r = fx_round(r, fmt, rnd(f), &exc);
    f->fp[ry] = fx_to_x(r);
    set_cc(f, r);
    post_exc(f, exc);
}
static int dst_plain(const m68k_fpu *f, unsigned m, unsigned ry) {
    return !dyadic(m) || plain_x(f->fp[ry]);
}

static void general(m68k_cpu *cpu, uint32_t op, uint32_t ext) {
    m68k_fpu *f = &cpu->fpu;
    if ((ext >> 13) == 0 && fast_op(cpu, ext & 0x7Fu)) {
        const unsigned rx = (ext >> 10) & 7u, ry = (ext >> 7) & 7u;
        if (plain_x(f->fp[rx]) && dst_plain(f, ext & 0x7Fu, ry)) {
            const unsigned m = ext & 0x7Fu;
            if ((m == 0x00 || m == 0x18 || m == 0x1A) && ((f->fpcr >> 6) & 3u) == 0) {
                /* FMOVE, FABS, FNEG at extended precision: the register
                 * itself, exact, its sign set as the operation says. */
                m68k_fx80 x = f->fp[rx];
                if (m == 0x18) x.se &= 0x7FFFu;
                else if (m == 0x1A) x.se ^= 0x8000u;
                f->fpiar = cpu->ipc;
                if (f->state == FPS_NULL || f->state == FPS_UNIMP) f->state = FPS_IDLE;
                f->fp[ry] = x;
                f->fpsr = (f->fpsr & ~0x0F000000u) | ((x.se & 0x8000u) ? CC_N : 0) | (x.m ? 0 : CC_Z);
                post_exc(f, 0);
                return;
            }
            fast_tail(cpu, ext, plain_fpx(f->fp[rx]), plain_fpx(f->fp[ry]));
            return;
        }
    }
    const unsigned opclass = ext >> 13, mode = OP_MODE(op), reg = OP_REG(op);
    const unsigned rx = (ext >> 10) & 7u, ry = (ext >> 7) & 7u, m = ext & 0x7Fu;
    const int e40 = f->type == M68K_FPU_68040, lc = cpu->model == M68K_68LC040;
    const int fmovecr = opclass == 2 && rx == 7;
    if (!fmovecr && !opmode_valid(m, e40 || lc)) {
        m68k_exception_line_f(cpu);
        return;
    }
    uint32_t w[3] = {0, 0, 0}, ea = 0;
    operand src, dst;
    memset(&src, 0, sizeof src);
    unsigned exc = 0;
    if (opclass == 2 && !fmovecr) {
        if (lc) {
            /* the effective address without reading the operand */
            const unsigned size = fmt_size[rx];
            if (mode >= 2 && !(mode == 7 && reg == 4)) ea = ea_addr(cpu, mode, reg, size);
            else if (mode == 7 && reg == 4) cpu->pc += size < 2 ? 2 : size;
            else if (mode == 1 || (mode == 0 && size > 4)) { m68k_exception_line_f(cpu); return; }
            lc040_trap(cpu, ea);
            return;
        }
        if (!fetch_operand(cpu, mode, reg, rx, w, &ea)) {
            m68k_exception_line_f(cpu);
            return;
        }
    }
    if (lc) {
        lc040_trap(cpu, 0);
        return;
    }
    if (take_pending(cpu)) return;
    if (opclass == 2 && !fmovecr) {
        make_operand(rx, w, &src, rnd(f), &exc);
        if (!exc && !src.unsupported && (src.v.k == FX_NORM || src.v.k == FX_ZERO) && fast_op(cpu, m) && dst_plain(f, m, ry)) {
            fast_tail(cpu, ext, src.v, plain_fpx(f->fp[ry]));
            return;
        }
    }
    else if (!fmovecr) reg_operand(f, rx, &src);
    reg_operand(f, ry, &dst);
    f->fpiar = cpu->ipc;
    if (f->state == FPS_NULL || f->state == FPS_UNIMP) f->state = FPS_IDLE;

    /* A restored busy frame's instruction, completed with the frame's
     * operands (the FPSP's normalized ETEMP/FPTEMP; op_frestore). */
    if (f->resume) {
        f->resume = 0;
        if ((f->cmdreg1b & 0x03FFu) == (cmd1_of((uint16_t)ext) & 0x03FFu)) {
            src.v = fx_from_x_e15(f->etemp, f->ete15);
            src.unsupported = 0;
            src.is_packed = 0;
            if (dyadic(m)) {
                dst.v = fx_from_x_e15(f->fptemp, f->fpte15);
                dst.unsupported = 0;
            }
        }
    }

    if (e40 && !f->full) {
        /* Unimplemented instructions (MC68040 UM 9.6.1): format $2, the next
         * PC and the effective address; FSAVE then gives the 26-word frame. */
        if (fmovecr || !native_040(m)) {
            memset(&f->cmdreg1b, 0, sizeof *f - offsetof(m68k_fpu, cmdreg1b));
            frame_e1(f, (uint16_t)ext, fmovecr ? NULL : &src, &dst, 0);
            /* E1 stays clear in this frame: the ROM's package reads E1 as
             * an unsupported (packed) operand to convert first (ROM
             * 420DBFF3 at $4088E194-$4088E24C, the decimal conversion at
             * $4088E4FE). */
            f->e1 = 0;
            f->state = FPS_UNIMP;
            /* The (An)+ or -(An) step stays done: the ROM's package does not
             * step the register again (traced). */
            cpu->events &= ~(uint32_t)EV_TRACE_NOW;    /* the emulation package traces it */
            m68k_exception_fmt2(cpu, VEC_LINE_F, cpu->pc, ea);
            return;
        }
        /* Unsupported data types (9.6.2): vector 55, a format $0 frame. Its
         * PC is the next instruction's, and an (An)+ or -(An) step stays
         * done: the ROM's package completes the instruction itself, or
         * restores the busy frame for the FPU to complete, then returns
         * with RTE to the stacked PC without changing it (ROM 420DBFF3,
         * vector 55 at $4088DAB0, exits at $4088DFC0 and $4088E0F0). */
        if (src.unsupported || (dyadic(m) && m != 0x3A && dst.unsupported)) {
            frame_e1(f, (uint16_t)ext, &src, &dst, 0);
            f->state = FPS_BUSY;
            m68k_exception(cpu, V_UNSUPP, cpu->pc);
            return;
        }
    }

    if (fmovecr) {
        int exact;
        const fpx c = ft_constant(m, &exact);
        unsigned e2 = 0;
        fpx r = fx_round(c, prec_fmt(f), rnd(f), &e2);
        (void)exact;
        exc |= e2;
        f->fp[ry] = fx_to_x(r);
        set_cc(f, r);
        post_exc(f, exc);
        post_pending(f, exc);
        return;
    }
    const fpx s = src.v, d = dst.v;
    /* 68881/2: the coprocessor runs on while the CPU fetches its next
     * instruction (MC68881 UM 6.4.2.3, the busy state frame); an FSAVE right
     * after one of the long instructions (the divides, FSQRT, FMOD, FREM and
     * the transcendental functions; Livadix's choice of which last longer than
     * the FSAVE's start, †) saves it busy (save_frame). */
    if (!e40 && long_op(m)) {
        f->busy_at = cpu->instructions + 1u;
        f->cmdreg1b = (uint16_t)ext;
        f->etemp = fx_to_x(s);
        f->fptemp = fx_to_x(d);
    }
    if (m == 0x38 || m == 0x3A) {                               /* FCMP, FTST */
        if (m == 0x38) compare(f, d, s, &exc);
        else {
            if (fx_is_snan(s)) exc |= FX_SNAN;
            set_cc(f, s);
        }
        post_exc(f, exc);
        post_pending(f, exc);
        return;
    }
    const int dy = dyadic(m);
    const int sincos = m >= 0x30 && m <= 0x37;
    fpx r;
    const fpfmt *fmt = op_fmt(f, m);
    unsigned qbyte = 0;
    int qset = 0;
    if (s.k == FX_NAN || (dy && d.k == FX_NAN)) {
        r = nan_result(s, d, dy, &exc);
        if (sincos) f->fp[m & 7u] = fx_to_x(r);
    } else if (sincos) {
        fpx c = ft_eval(FT_COS, s, &exc);
        fpx sn = ft_eval(FT_SIN, s, &exc);
        unsigned e2 = 0;
        c = fx_round(c, fmt, rnd(f), &e2);
        r = sn;
        if (!(exc & FX_OPERR)) f->fp[m & 7u] = fx_to_x(c);
        else f->fp[m & 7u] = fx_to_x(fx_default_nan());
    } else {
        r = compute(f, m, s, d, &exc, &qbyte, &qset, &fmt);
    }
    const fpx inter = r;
    if (r.k == FX_NORM) r = fx_round(r, fmt, rnd(f), &exc);
    if (qset) f->fpsr = (f->fpsr & ~0x00FF0000u) | (qbyte << 16);
    /* Destination: not written when an enabled SNAN, OPERR or DZ traps
     * (MC68881 UM 6.1.2, 6.1.3, 6.1.6), nor, on the 68040, an enabled
     * overflow or underflow (MC68040 UM 9.7.4-9.7.5). */
    const unsigned en = exc & f->fpcr & 0xFF00u;
    int write = !(en & (FX_SNAN | FX_OPERR | FX_DZ));
    if (e40 && (en & (FX_OVFL | FX_UNFL))) write = 0;
    if (write) {
        f->fp[ry] = fx_to_x(r);
        set_cc(f, r);
    }
    post_exc(f, exc);
    if (en) {
        /* the state frame contents for the handler */
        if (e40) {
            if ((en & (FX_OVFL | FX_UNFL | FX_INEX1 | FX_INEX2)) && !(en & (FX_SNAN | FX_OPERR | FX_DZ)) && e3_op(m)) {
                frame_e3(f, (uint16_t)ext, inter);
            } else {
                frame_e1(f, (uint16_t)ext, &src, &dst, 0);
                if (en & (FX_OVFL | FX_UNFL | FX_INEX1 | FX_INEX2)) f->fptemp = fx_to_x(inter.k == FX_NORM ? inter : r);
            }
        } else {
            f->cmdcond = (uint16_t)ext;
            f->excop = src.raw;
            if (en & (FX_OVFL | FX_UNFL)) {
                /* the intermediate with its exponent wrapped by -/+$6000
                 * (MC68881 UM 6.1.4, 6.1.5) */
                m68k_fx80 x = fx_to_x(inter.k == FX_NORM ? inter : r);
                const int32_t be = inter.e + 16383 + ((en & FX_OVFL) ? -0x6000 : 0x6000);
                x.se = (uint16_t)((inter.s ? 0x8000u : 0) | ((uint32_t)be & 0x7FFFu));
                x.m = inter.m;
                f->excop = x;
            }
        }
        post_pending(f, exc);
    }
}

/* FMOVE FPm,<ea> (opclass 011). */
static void fmove_out(m68k_cpu *cpu, uint32_t op, uint32_t ext) {
    m68k_fpu *f = &cpu->fpu;
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    const unsigned fmt = (ext >> 10) & 7u, ry = (ext >> 7) & 7u;
    const unsigned size = fmt_size[fmt];
    if (mode == 1 || (mode == 0 && size > 4) || (mode == 7 && reg > 1)) {
        m68k_exception_line_f(cpu);
        return;
    }
    uint32_t a = 0;
    if (mode != 0) a = ea_addr(cpu, mode, reg, size);
    if (cpu->model == M68K_68LC040) {
        lc040_trap(cpu, a);
        return;
    }
    if (take_pending(cpu)) return;
    f->fpiar = cpu->ipc;
    if (f->state == FPS_NULL || f->state == FPS_UNIMP) f->state = FPS_IDLE;
    operand src;
    reg_operand(f, ry, &src);
    const int e40 = f->type == M68K_FPU_68040;
    if (e40 && !f->full && (src.unsupported || fmt == F_P || fmt == F_PDYN)) {
        /* Unsupported data type, post-instruction: format $3 with the
         * destination's address (MC68040 UM 9.6.2). */
        frame_e1(f, (uint16_t)ext, &src, NULL, 1);
        f->state = FPS_BUSY;
        uint8_t x[4];
        x[0] = (uint8_t)(a >> 24); x[1] = (uint8_t)(a >> 16); x[2] = (uint8_t)(a >> 8); x[3] = (uint8_t)a;
        m68k_exception_frame(cpu, V_UNSUPP, 3, cpu->pc, x, 4);
        return;
    }
    fpx s = src.v;
    unsigned exc = 0;
    uint32_t out[3] = {0, 0, 0};
    unsigned words = 1;
    switch (fmt) {
    case F_L: case F_W: case F_B: {
        const int bits = fmt == F_L ? 32 : fmt == F_W ? 16 : 8;
        int32_t v;
        if (s.k == FX_NAN) {
            if (fx_is_snan(s)) exc |= FX_SNAN;
            exc |= FX_OPERR;
            v = (int32_t)((fx_is_snan(s) ? fx_quiet(s).m : s.m) >> (64 - bits));
        } else {
            v = fx_to_int(s, bits, rnd(f), &exc);
        }
        out[0] = (uint32_t)v & (bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1u);
        break;
    }
    case F_S:
        if (s.k == FX_NAN) { if (fx_is_snan(s)) exc |= FX_SNAN; s = fx_quiet(s); }
        else s = fx_round(s, &FMT_S, rnd(f), &exc);
        out[0] = fx_to_s(s);
        break;
    case F_D: {
        if (s.k == FX_NAN) { if (fx_is_snan(s)) exc |= FX_SNAN; s = fx_quiet(s); }
        else s = fx_round(s, &FMT_D, rnd(f), &exc);
        const uint64_t d = fx_to_d(s);
        out[0] = (uint32_t)(d >> 32);
        out[1] = (uint32_t)d;
        words = 2;
        break;
    }
    case F_X: {
        if (s.k == FX_NAN) { if (fx_is_snan(s)) exc |= FX_SNAN; s = fx_quiet(s); }
        else s = fx_round(s, &FMT_X, rnd(f), &exc);
        const m68k_fx80 x = fx_to_x(s);
        out[0] = (uint32_t)x.se << 16;
        out[1] = (uint32_t)(x.m >> 32);
        out[2] = (uint32_t)x.m;
        words = 3;
        break;
    }
    default: {                                  /* packed, static or dynamic k */
        int k = fmt == F_P ? (int)(ext & 0x7Fu) : (int)(D((ext >> 4) & 7u) & 0x7Fu);
        if (k & 0x40) k -= 0x80;
        if (fx_is_snan(s)) exc |= FX_SNAN;
        ft_to_packed(s, k, rnd(f), &exc, out);
        words = 3;
        break;
    }
    }
    /* Memory first (a refused write restarts the instruction), then the
     * status. */
    if (mode == 0) {
        set_dn(cpu, reg, out[0], size);
    } else {
        cpu->restart_writes = 1;
        if (size == 1) wr8(cpu, a, out[0]);
        else if (size == 2) wr16(cpu, a, out[0]);
        else for (unsigned i = 0; i < words; i++) wr32(cpu, a + 4 * i, out[i]);
        cpu->restart_writes = 0;
    }
    post_exc(f, exc);
    const unsigned en = exc & f->fpcr & 0xFF00u;
    if (!en) return;
    if (e40) {
        /* post-instruction, at once: format $3 with the destination address */
        frame_e1(f, (uint16_t)ext, &src, NULL, 1);
        f->state = FPS_BUSY;
        uint8_t x[4];
        x[0] = (uint8_t)(a >> 24); x[1] = (uint8_t)(a >> 16); x[2] = (uint8_t)(a >> 8); x[3] = (uint8_t)a;
        m68k_exception_frame(cpu, exc_vector(en), 3, cpu->pc, x, 4);
        return;
    }
    /* 68881/2: a mid-instruction exception (format $9): the next PC, the
     * instruction's address, the evaluated destination at +$10 (MC68881 UM
     * 6.1; MC68030 UM 8.4.3). */
    f->cmdcond = (uint16_t)ext;
    f->excop = src.raw;
    if (f->type == M68K_FPU_68882) {
        f->pending = 1;
        f->pend_vec = (uint8_t)exc_vector(en);
    }
    uint8_t x[12];
    memset(x, 0, sizeof x);
    const uint32_t ia = cpu->ipc;
    x[0] = (uint8_t)(ia >> 24); x[1] = (uint8_t)(ia >> 16); x[2] = (uint8_t)(ia >> 8); x[3] = (uint8_t)ia;
    x[8] = (uint8_t)(a >> 24); x[9] = (uint8_t)(a >> 16); x[10] = (uint8_t)(a >> 8); x[11] = (uint8_t)a;
    m68k_exception_frame(cpu, exc_vector(en), 9, cpu->pc, x, 12);
}

/* FMOVEM to and from the control registers (opclass 100/101). */
static void fmovem_ctrl(m68k_cpu *cpu, uint32_t op, uint32_t ext) {
    m68k_fpu *f = &cpu->fpu;
    const unsigned mode = OP_MODE(op), reg = OP_REG(op), list = (ext >> 10) & 7u;
    const int to_mem = (ext >> 13) & 1u;
    const unsigned n = (list & 1u) + ((list >> 1) & 1u) + ((list >> 2) & 1u);
    if (!n || (ext & 0x03FFu) || (mode == 0 && n > 1) || (mode == 1 && list != 1) ||
        /* (An)+ to memory and -(An) from memory for one register only: FMOVE
         * takes them (PRM FMOVE system control register, its two tables),
         * FMOVEM of several does not. */
        (to_mem && ((mode == 3 && n > 1) || (mode == 7 && reg > 1))) || (!to_mem && mode == 4 && n > 1) ||
        (mode == 7 && reg > 4)) {
        m68k_exception_line_f(cpu);
        return;
    }
    uint32_t a = 0;
    uint32_t v[3] = {0, 0, 0};
    if (mode >= 2 && !(mode == 7 && reg == 4)) {
        if (mode == 4) {                                /* -(An): the whole block below An */
            a = A(reg) - 4 * n;
            undo_push(cpu, 8 + reg, A(reg));
            A(reg) = a;
        } else if (mode == 3) {
            a = A(reg);
            undo_push(cpu, 8 + reg, a);
            A(reg) = a + 4 * n;
        } else {
            a = ea_addr(cpu, mode, reg, 4);
        }
    }
    if (!to_mem && mode == 7 && reg == 4)
        for (unsigned i = 0; i < n; i++) v[i] = fetch32(cpu);
    if (cpu->model == M68K_68LC040) {
        lc040_trap(cpu, mode >= 2 && !(mode == 7 && reg == 4) ? a : 0);
        return;
    }
    /* The 68882 runs an arithmetic instruction concurrently with the CPU
     * and reports its exception when the next FP instruction starts, an
     * FMOVE of a control register too (it waits for the arithmetic one to
     * end): the IIci ROM's FPU test divides by zero with the exception
     * enabled and expects it taken at FMOVE.L FPSR,D1 (ROM 368CADFE at
     * $408459B6-$408459D6, $40845C5A). An exception pending from FRESTORE
     * waits for another instruction, as MC68881 UM 4-67 states. */
    if ((f->type == M68K_FPU_68040 || (f->pending && f->pend_concurrent)) && take_pending(cpu)) return;
    if (f->state == FPS_NULL) f->state = FPS_IDLE;
    uint32_t *regs[3] = {&f->fpcr, &f->fpsr, &f->fpiar};
    static const uint32_t masks[3] = {FPCR_MASK, FPSR_MASK, 0xFFFFFFFFu};
    if (to_mem) {
        unsigned k = 0;
        for (unsigned i = 0; i < 3; i++)
            if (list & (4u >> i)) v[k++] = *regs[i];
        if (mode == 0) D(reg) = v[0];
        else if (mode == 1) A(reg) = v[0];
        else {
            cpu->restart_writes = 1;
            for (unsigned i = 0; i < n; i++) wr32(cpu, a + 4 * i, v[i]);
            cpu->restart_writes = 0;
        }
        return;
    }
    if (mode == 0) v[0] = D(reg);
    else if (mode == 1) v[0] = A(reg);
    else if (!(mode == 7 && reg == 4))
        for (unsigned i = 0; i < n; i++) v[i] = rd32(cpu, a + 4 * i);
    unsigned k = 0;
    for (unsigned i = 0; i < 3; i++)
        if (list & (4u >> i)) *regs[i] = v[k++] & masks[i];
}

/* FMOVEM data registers (opclass 110/111; PRM FMOVEM). */
static void fmovem_data(m68k_cpu *cpu, uint32_t op, uint32_t ext) {
    m68k_fpu *f = &cpu->fpu;
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    const int to_mem = (ext >> 13) & 1u;
    const unsigned lmode = (ext >> 11) & 3u;
    const int predec = !(lmode & 2u);
    if ((ext & 0x0700u) || (predec && (mode != 4 || !to_mem)) || (!predec && (mode == 4 || (mode == 3 && to_mem))) ||
        mode < 2 || (mode == 7 && reg > (to_mem ? 1u : 3u)) || (lmode & 1u ? (ext & 0x8Fu) : 0)) {
        m68k_exception_line_f(cpu);
        return;
    }
    unsigned list = (lmode & 1u) ? (D((ext >> 4) & 7u) & 0xFFu) : (ext & 0xFFu);
    unsigned n = 0;
    for (unsigned i = 0; i < 8; i++) n += (list >> i) & 1u;
    uint32_t a;
    if (mode == 4) {
        a = A(reg) - 12u * n;
        undo_push(cpu, 8 + reg, A(reg));
        A(reg) = a;
    } else if (mode == 3) {
        a = A(reg);
        undo_push(cpu, 8 + reg, a);
        A(reg) = a + 12u * n;
    } else {
        a = ea_addr(cpu, mode, reg, 12);
    }
    if (cpu->model == M68K_68LC040) {
        lc040_trap(cpu, a);
        return;
    }
    if (f->type == M68K_FPU_68040 && take_pending(cpu)) return;
    if (f->state == FPS_NULL) f->state = FPS_IDLE;
    /* Register order: FP0 first in memory; in the predecrement list bit 0
     * is FP0 (bit 7 FP7), otherwise bit 7 is FP0. */
    if (to_mem) {
        cpu->restart_writes = 1;
        uint32_t p = a;
        for (unsigned r = 0; r < 8; r++) {
            const unsigned bit = predec ? r : 7u - r;
            if (!(list & (1u << bit))) continue;
            wr32(cpu, p, (uint32_t)f->fp[r].se << 16);
            wr32(cpu, p + 4, (uint32_t)(f->fp[r].m >> 32));
            wr32(cpu, p + 8, (uint32_t)f->fp[r].m);
            p += 12;
        }
        cpu->restart_writes = 0;
        return;
    }
    m68k_fx80 v[8];
    uint32_t p = a;
    for (unsigned r = 0; r < 8; r++) {
        if (!(list & (1u << (7u - r)))) continue;
        v[r].se = (uint16_t)(rd32(cpu, p) >> 16);
        v[r].m = (uint64_t)rd32(cpu, p + 4) << 32 | rd32(cpu, p + 8);
        p += 12;
    }
    for (unsigned r = 0; r < 8; r++)
        if (list & (1u << (7u - r))) f->fp[r] = v[r];
}

/* --- conditionals (type 001, 010, 011) --------------------------------------------------- */

/* The predicate (MC68040 UM Table 9-8): 0 false, 1 true, -1 not a
 * predicate. Sets BSUN for the IEEE nonaware tests on NaN. */
static int predicate(m68k_fpu *f, unsigned p, unsigned *exc) {
    if (p > 0x1F) return -1;
    const int n = (f->fpsr & CC_N) != 0, z = (f->fpsr & CC_Z) != 0, nan = (f->fpsr & CC_NAN) != 0;
    if ((p & 0x10u) && nan) *exc |= FX_BSUN;
    switch (p & 0x0Fu) {
    case 0x0: return 0;
    case 0x1: return z;
    case 0x2: return !(nan || z || n);
    case 0x3: return z || !(nan || n);
    case 0x4: return n && !(nan || z);
    case 0x5: return z || (n && !nan);
    case 0x6: return !(nan || z);
    case 0x7: return !nan;
    case 0x8: return nan;
    case 0x9: return nan || z;
    case 0xA: return nan || !(n || z);
    case 0xB: return nan || z || !n;
    case 0xC: return nan || (n && !z);
    case 0xD: return nan || z || n;
    case 0xE: return !z;
    default: return 1;
    }
}

/* Evaluates a predicate for a conditional instruction: -1 when an
 * exception was taken (or the predicate is reserved: line F). */
static int conditional(m68k_cpu *cpu, unsigned p) {
    m68k_fpu *f = &cpu->fpu;
    unsigned exc = 0;
    const int c = predicate(f, p, &exc);
    if (c < 0) {
        m68k_exception_line_f(cpu);
        return -1;
    }
    if (f->type != M68K_FPU_68040 && f->state == FPS_NULL) f->state = FPS_IDLE;
    if (exc) {
        f->fpsr |= FX_BSUN | 0x80u;             /* EXC BSUN, AEXC IOP */
        if (f->fpcr & FX_BSUN) {
            pre_instruction(cpu, V_BSUN);
            return -1;
        }
    }
    return c;
}

static void op_cond(m68k_cpu *cpu, uint32_t op) {
    m68k_fpu *f = &cpu->fpu;
    const unsigned type = (op >> 6) & 7u, mode = OP_MODE(op), reg = OP_REG(op);
    const int lc = cpu->model == M68K_68LC040;
    if (type == 2 || type == 3) {                               /* FBcc.W / FBcc.L */
        const uint32_t base = cpu->pc;
        const uint32_t disp = type == 2 ? (uint32_t)(int32_t)(int16_t)fetch16(cpu) : fetch32(cpu);
        if (lc) { lc040_trap(cpu, 0); return; }
        if ((op & 0x3Fu) > 0x1F) { m68k_exception_line_f(cpu); return; }
        if (take_pending(cpu)) return;
        const int c = conditional(cpu, op & 0x3Fu);
        cpu->cyc_extra += m68k_timing_fpu(cpu, op, 0, M68K_FPT_BCC, 0, c > 0);
        if (c > 0) jump(cpu, base + disp);
        return;
    }
    const uint32_t ext = fetch16(cpu);
    if ((ext & 0xFFC0u) || ext > 0x1F) {
        if (lc && !(ext & 0xFFC0u)) { lc040_trap(cpu, 0); return; }
        m68k_exception_line_f(cpu);
        return;
    }
    if (mode == 1) {                                            /* FDBcc */
        const uint32_t base = cpu->pc;
        const uint32_t disp = (uint32_t)(int32_t)(int16_t)fetch16(cpu);
        if (lc) { lc040_trap(cpu, 0); return; }
        if (take_pending(cpu)) return;
        const int c = conditional(cpu, ext);
        if (c != 0) {
            cpu->cyc_extra += m68k_timing_fpu(cpu, op, ext, M68K_FPT_DBCC, 1, 0);
            return;
        }
        const uint32_t w = (D(reg) - 1u) & 0xFFFFu;
        D(reg) = (D(reg) & 0xFFFF0000u) | w;
        cpu->cyc_extra += m68k_timing_fpu(cpu, op, ext, M68K_FPT_DBCC, 0, w != 0xFFFFu);
        if (w != 0xFFFFu) jump(cpu, base + disp);
        return;
    }
    if (mode == 7 && reg >= 2) {                                /* FTRAPcc */
        if (reg > 4) { m68k_exception_line_f(cpu); return; }
        if (reg == 2) (void)fetch16(cpu);
        else if (reg == 3) (void)fetch32(cpu);
        if (lc) { lc040_trap(cpu, 0); return; }
        if (take_pending(cpu)) return;
        const int c = conditional(cpu, ext);
        cpu->cyc_extra += m68k_timing_fpu(cpu, op, ext, M68K_FPT_TRAPCC, 0, c > 0);
        if (c > 0) m68k_exception_fmt2(cpu, VEC_TRAPCC, cpu->pc, cpu->ipc);
        return;
    }
    /* FScc <ea>: data alterable */
    if (mode == 7 && reg > 1) { m68k_exception_line_f(cpu); return; }
    uint32_t a = 0;
    if (mode != 0) a = ea_addr(cpu, mode, reg, 1);
    if (lc) { lc040_trap(cpu, a); return; }
    if (take_pending(cpu)) return;
    const int c = conditional(cpu, ext);
    if (c < 0) return;
    cpu->cyc_extra += m68k_timing_fpu(cpu, op, ext, M68K_FPT_SCC, 0, c);
    if (mode == 0) set_dn(cpu, reg, c ? 0xFFu : 0, 1);
    else wr8(cpu, a, c ? 0xFFu : 0);
    (void)f;
}

/* --- FSAVE and FRESTORE (MC68881 UM 6.4; MC68040 UM 9.8) ------------------------------- */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

static void put_x(uint8_t *p, m68k_fx80 x) {   /* sign/exponent word at p, mantissa at p + 4 */
    put16(p, x.se);
    put32(p + 4, (uint32_t)(x.m >> 32));
    put32(p + 8, (uint32_t)x.m);
}
static m68k_fx80 get_x(const uint8_t *p) {
    m68k_fx80 x;
    x.se = (uint16_t)get16(p);
    x.m = (uint64_t)get32(p + 4) << 32 | get32(p + 8);
    return x;
}

/* The common fields of the 68040 unimplemented-instruction and busy frames,
 * at offset `o` = 0 (unimp) or $30 (busy) for the shared block (Figure 9-10:
 * CMDREG3B at $04, STAG/WBTM66/WBTM1/WBTM0/SBIT at $0C, CMDREG1B at $10,
 * DTAG/WBTE15 at $14, E1/E3/T at $18, FPTEMP at $1C, ETEMP at $28). */
static void put_040_block(uint8_t *fr, unsigned o, const m68k_fpu *f) {
    put16(fr + o + 0x04, f->cmdreg3b & 0x07FFu);
    fr[o + 0x0C] = (uint8_t)(f->stag << 5 | f->ete15 << 4 | f->wbm66 << 2 | f->wbm1 << 1 | f->wbm0);
    fr[o + 0x0D] = (uint8_t)(f->sbit << 7);
    put16(fr + o + 0x10, f->cmdreg1b);
    fr[o + 0x14] = (uint8_t)(f->dtag << 5 | f->fpte15 << 4);
    fr[o + 0x15] = (uint8_t)(f->wbte15 << 4);
    fr[o + 0x18] = (uint8_t)(f->e1 << 2 | f->e3 << 1);
    fr[o + 0x19] = (uint8_t)(f->t << 4);
    put_x(fr + o + 0x1C, f->fptemp);
    put_x(fr + o + 0x28, f->etemp);
}
static void get_040_block(const uint8_t *fr, unsigned o, m68k_fpu *f) {
    f->cmdreg3b = (uint16_t)(get16(fr + o + 0x04) & 0x07FFu);
    f->stag = (uint8_t)(fr[o + 0x0C] >> 5);
    f->wbm66 = (uint8_t)((fr[o + 0x0C] >> 2) & 1u);
    f->wbm1 = (uint8_t)((fr[o + 0x0C] >> 1) & 1u);
    f->wbm0 = (uint8_t)(fr[o + 0x0C] & 1u);
    f->sbit = (uint8_t)(fr[o + 0x0D] >> 7);
    f->cmdreg1b = (uint16_t)get16(fr + o + 0x10);
    f->dtag = (uint8_t)(fr[o + 0x14] >> 5);
    /* ETE15 and FPTE15: the 16th exponent bit of ETEMP and FPTEMP, which
     * the ROM's package sets when it normalizes a denormal operand before
     * it restores the frame (ROM 420DBFF3 at $4088E918-$4088E91E and
     * $4088DC4A-$4088DC54; MC68040 UM 9.8 names ETE and FPTE). */
    f->ete15 = (uint8_t)((fr[o + 0x0C] >> 4) & 1u);
    f->fpte15 = (uint8_t)((fr[o + 0x14] >> 4) & 1u);
    f->wbte15 = (uint8_t)((fr[o + 0x15] >> 4) & 1u);
    f->e1 = (uint8_t)((fr[o + 0x18] >> 2) & 1u);
    f->e3 = (uint8_t)((fr[o + 0x18] >> 1) & 1u);
    f->t = (uint8_t)((fr[o + 0x19] >> 4) & 1u);
    f->fptemp = get_x(fr + o + 0x1C);
    f->etemp = get_x(fr + o + 0x28);
}

/* Builds the frame FSAVE stores; returns its size. */
static unsigned save_frame(m68k_cpu *cpu, uint8_t *fr) {
    m68k_fpu *f = &cpu->fpu;
    memset(fr, 0, 216);
    if (f->state == FPS_NULL) return 4;
    if (f->type == M68K_FPU_68040) {
        if (f->state == FPS_UNIMP) {
            put32(fr, 0x41300000u);
            put_040_block(fr, 0, f);
            return 0x34;
        }
        if (f->state == FPS_BUSY) {
            put32(fr, 0x41600000u);
            put_040_block(fr, 0x30, f);
            /* WBTEMP at $18-$23, FPIARCU at $28 */
            fr[0x18] = (uint8_t)((f->wbtemp.se >> 8) & 0xFFu);
            fr[0x19] = (uint8_t)f->wbtemp.se;
            put32(fr + 0x1C, (uint32_t)(f->wbtemp.m >> 32));
            put32(fr + 0x20, (uint32_t)f->wbtemp.m);
            put32(fr + 0x28, f->fpiar);
            return 0x64;
        }
        put32(fr, 0x41000000u);
        return 4;
    }
    const int is82 = f->type == M68K_FPU_68882;
    /* 68881 busy $1FB4 (184 bytes), 68882 busy $1FD4 (216 bytes; MC68881 UM
     * 6.4.2.3, Figures 6-4 and 6-5): the arithmetic instruction just before
     * this FSAVE is still executing on a real coprocessor (a long one like
     * FREM: the IIci ROM's FPU test saves it, resets the FPU with a null
     * frame and restores it, ROM 368CADFE at $408458F8). The internal state
     * is undocumented; Livadix keeps the command word at +$04, the source
     * operand at +$08 and the destination's at +$14 (†). */
    if (f->busy_at && cpu->instructions == f->busy_at && !f->pending) {
        const unsigned busy = is82 ? 0xD4u : 0xB4u;
        put32(fr, 0x1F000000u | busy << 16);
        put16(fr + 4, f->cmdreg1b);
        put_x(fr + 0x08, f->etemp);
        put_x(fr + 0x14, f->fptemp);
        return 4 + busy;
    }
    /* 68881 idle $1F18 (28 bytes), 68882 idle $1F38 (60 bytes): command/
     * condition register, exceptional operand, operand register, BIU flags
     * (bit 27 clear: an exception is pending). */
    const unsigned size = is82 ? 0x38u : 0x18u;
    put32(fr, 0x1F000000u | size << 16);
    put16(fr + 4, f->cmdcond);
    put16(fr + 6, 0xFFFF);
    const unsigned op_at = is82 ? 0x28u : 0x08u;
    put_x(fr + op_at, f->excop);
    put32(fr + op_at + 12, 0);
    put32(fr + op_at + 16, f->pending ? 0x7400FFFFu : 0x7C00FFFFu);
    return 4 + size;
}

static void op_fsave(m68k_cpu *cpu, uint32_t op) {
    m68k_fpu *f = &cpu->fpu;
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    if (mode < 2 || mode == 3 || (mode == 7 && reg > 1)) { m68k_exception_line_f(cpu); return; }
    uint8_t fr[216];
    const unsigned size = cpu->model == M68K_68LC040 ? 4 : save_frame(cpu, fr);
    uint32_t a;
    if (mode == 4) {
        a = A(reg) - size;
        undo_push(cpu, 8 + reg, A(reg));
        A(reg) = a;
    } else {
        a = ea_addr(cpu, mode, reg, 4);
    }
    if (cpu->model == M68K_68LC040) { lc040_trap(cpu, a); return; }
    /* null or idle, the 68040's unimplemented-instruction or a 68881/2
     * idle frame, a busy frame (m68k_timing.c) */
    cpu->cyc_extra += m68k_timing_fpu(cpu, op, 0, M68K_FPT_SAVE, size == 4 ? 0u : size >= 100 ? 2u : 1u, 0);
    cpu->restart_writes = 1;
    for (unsigned i = 0; i < size; i += 4) wr32(cpu, a + i, get32(fr + i));
    cpu->restart_writes = 0;
    /* After FSAVE the FPU is idle and nothing is pending (MC68881 UM 6.4.1;
     * MC68040 UM 9.8). */
    f->pending = 0;
    f->pend_vec = 0;
    f->resume = 0;
    f->pend_concurrent = 0;
    if (f->state != FPS_NULL) f->state = FPS_IDLE;
}

static void op_frestore(m68k_cpu *cpu, uint32_t op) {
    m68k_fpu *f = &cpu->fpu;
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    if (mode < 2 || mode == 4 || (mode == 7 && reg > 3)) { m68k_exception_line_f(cpu); return; }
    uint32_t a = mode == 3 ? A(reg) : ea_addr(cpu, mode, reg, 4);
    if (cpu->model == M68K_68LC040) { lc040_trap(cpu, a); return; }
    const uint32_t head = rd32(cpu, a);
    const unsigned version = head >> 24, fsize = (head >> 16) & 0xFFu;
    unsigned total;
    const int e40 = f->type == M68K_FPU_68040;
    if (version == 0) total = 4;
    else if (e40 && version == 0x41 && (fsize == 0x00 || fsize == 0x30 || fsize == 0x60)) total = 4 + fsize;
    else if (!e40 && version == 0x1F &&
             (fsize == (f->type == M68K_FPU_68882 ? 0x38u : 0x18u) || fsize == (f->type == M68K_FPU_68882 ? 0xD4u : 0xB4u)))
        total = 4 + fsize;
    else if (f->type == M68K_FPU_68881 && version == 0x3F && fsize == 0x18) total = 4 + fsize;
    else {
        /* an unknown version or size: format error at the FRESTORE */
        m68k_undo_steps(cpu);
        cpu->pc = cpu->ipc;
        m68k_exception(cpu, VEC_FORMAT, cpu->ipc);
        return;
    }
    cpu->cyc_extra += m68k_timing_fpu(cpu, op, 0, M68K_FPT_RESTORE, total == 4 ? 0u : total >= 100 ? 2u : 1u, 0);
    uint8_t fr[216];
    put32(fr, head);
    for (unsigned i = 4; i < total; i += 4) put32(fr + i, rd32(cpu, a + i));
    if (mode == 3) {
        undo_push(cpu, 8 + reg, A(reg));
        A(reg) = a + total;
    }
    f->pending = 0;
    f->pend_vec = 0;
    f->resume = 0;
    f->pend_concurrent = 0;
    if (version == 0) {
        /* the null frame resets the FPU (MC68881 UM 6.4.2.1) */
        m68k_fpu_reset(cpu);
        return;
    }
    f->state = FPS_IDLE;
    if (e40) {
        if (fsize == 0x30) {
            get_040_block(fr, 0, f);
        } else if (fsize == 0x60) {
            get_040_block(fr, 0x30, f);
            f->wbtemp.se = (uint16_t)get16(fr + 0x18);
            f->wbtemp.m = (uint64_t)get32(fr + 0x1C) << 32 | get32(fr + 0x20);
            if (f->e1 || f->e3) {
                /* still exceptional: reported at the next FP instruction */
                f->state = FPS_BUSY;
                f->pending = 1;
                const unsigned en = f->fpsr & f->fpcr & 0xFF00u;
                f->pend_vec = (uint8_t)(en ? exc_vector(en) : V_UNSUPP);
            } else if (!f->t) {
                /* The FPU completes the frame's instruction now, from ETEMP
                 * (and FPTEMP): the package restores such a frame after it
                 * normalized the operands and returns past the instruction
                 * (ROM 420DBFF3 at $4088E0F0). */
                const uint32_t fpiar = f->fpiar;
                const uint16_t c1 = f->cmdreg1b;
                const unsigned dreg = (c1 >> 7) & 7u, opm = c1 & 0x7Fu;
                f->resume = 1;
                general(cpu, 0xF200u, (uint32_t)(dreg << 10 | dreg << 7 | (opm == 0x05u ? 0x04u : opm)));
                f->resume = 0;
                f->fpiar = fpiar;
            }
        }
        return;
    }
    const int is82 = f->type == M68K_FPU_68882;
    if (fsize == (is82 ? 0xD4u : 0xB4u)) {
        /* A busy frame (save_frame): the coprocessor completes its
         * instruction from the frame's operands into its destination. */
        const uint16_t ext = (uint16_t)get16(fr + 4);
        const unsigned dreg = (ext >> 7) & 7u;
        const uint32_t fpiar = f->fpiar;
        f->cmdreg1b = cmd1_of(ext);
        f->etemp = get_x(fr + 0x08);
        f->fptemp = get_x(fr + 0x14);
        f->ete15 = f->fpte15 = 0;
        f->resume = 1;
        general(cpu, 0xF200u, (uint32_t)(dreg << 10 | dreg << 7 | (ext & 0x7Fu)));
        f->resume = 0;
        f->fpiar = fpiar;
        f->busy_at = 0;
        return;
    }
    if (fsize == (is82 ? 0x38u : 0x18u)) {
        const unsigned op_at = is82 ? 0x28u : 0x08u;
        f->cmdcond = (uint16_t)get16(fr + 4);
        f->excop = get_x(fr + op_at);
        const uint32_t biu = get32(fr + op_at + 16);
        if (!(biu & 0x08000000u)) {
            f->pending = 1;
            f->pend_vec = 0;
        }
    }
}

/* --- the entry point --------------------------------------------------------------------- */

void m68k_op_fpu(m68k_cpu *cpu, uint32_t op) {
    m68k_fpu *f = &cpu->fpu;
    /* Coprocessor ID 1 only; no FPU on a 68020/030 (no coprocessor answers)
     * is line F (MC68030 UM 10). */
    if (((op >> 9) & 7u) != 1 || (f->type == M68K_FPU_NONE && cpu->model != M68K_68LC040)) {
        m68k_exception_line_f(cpu);
        return;
    }
    const unsigned type = (op >> 6) & 7u;
    switch (type) {
    case 0: {
        const uint32_t ext = fetch16(cpu);
        const unsigned opclass = ext >> 13;
        /* The instruction's count (m68k_timing.c); a line F or privilege
         * trap then counts the exception's time alone. */
        switch (opclass) {
        case 0: case 2:
            cpu->cyc_extra += m68k_timing_fpu(cpu, op, ext, M68K_FPT_GENERAL, 0, 0);
            general(cpu, op, ext);
            return;
        case 3:
            cpu->cyc_extra += m68k_timing_fpu(cpu, op, ext, M68K_FPT_OUT, 0, 0);
            fmove_out(cpu, op, ext);
            return;
        case 4: case 5: {
            const unsigned list = (ext >> 10) & 7u;
            cpu->cyc_extra += m68k_timing_fpu(cpu, op, ext, M68K_FPT_CTRL, (list & 1u) + ((list >> 1) & 1u) + (list >> 2), 0);
            fmovem_ctrl(cpu, op, ext);
            return;
        }
        case 6: case 7: {
            uint32_t list = (ext & 0x0800u) ? D((ext >> 4) & 7u) & 0xFFu : ext & 0xFFu;
            unsigned n = 0;
            for (; list; list &= list - 1u) n++;
            cpu->cyc_extra += m68k_timing_fpu(cpu, op, ext, M68K_FPT_DATA, n, 0);
            fmovem_data(cpu, op, ext);
            return;
        }
        default: m68k_exception_line_f(cpu); return;
        }
    }
    case 1: case 2: case 3: op_cond(cpu, op); return;
    case 4:
        if (!cpu->s && cpu->model != M68K_68LC040) { m68k_exception_privilege(cpu); return; }
        op_fsave(cpu, op);
        return;
    case 5:
        if (!cpu->s && cpu->model != M68K_68LC040) { m68k_exception_privilege(cpu); return; }
        op_frestore(cpu, op);
        return;
    default:
        m68k_exception_line_f(cpu);
        return;
    }
}
