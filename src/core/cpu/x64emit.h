/* A small x86-64 instruction encoder for the translator (jit_x64.c).
 *
 * Livadix's own code, written from the Intel 64 and IA-32 Architectures
 * Software Developer's Manual, Volume 2 (chapter 2, "Instruction Format":
 * legacy prefixes, the REX prefix with its W, R, X and B bits, the ModR/M
 * and SIB bytes, displacement and immediate fields, RIP-relative
 * addressing; chapters 3-5, the opcode of each instruction used here) and
 * the AMD64 Architecture Programmer's Manual, Volume 3 (the same encodings).
 *
 * Operands are either a register or a memory reference
 * [base + index * scale + disp32]. Sizes are 1, 2, 4 or 8 bytes. */
#ifndef MC_X64EMIT_H
#define MC_X64EMIT_H

#include <stdint.h>
#include <string.h>

enum { XAX, XCX, XDX, XBX, XSP, XBP, XSI, XDI, X8, X9, X10, X11, X12, X13, X14, X15 };

/* Condition codes (the low nibble of Jcc, SETcc and CMOVcc). */
enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };

/* Group-1 arithmetic operations (the /digit of 80/81/83 and opcode bits 5-3). */
enum { ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP };
/* Group-2 shifts and rotates (the /digit of C0/C1/D0-D3). */
enum { SH_ROL, SH_ROR, SH_RCL, SH_RCR, SH_SHL, SH_SHR, SH_SAL, SH_SAR };

typedef struct xo {
    int8_t reg;     /* >= 0: a register operand */
    int8_t base;    /* memory: base register, -1 none */
    int8_t index;   /* memory: index register, -1 none */
    uint8_t scale;  /* 1, 2, 4, 8 */
    int32_t disp;
} xo;

static inline xo XR(int r) { xo o = {(int8_t)r, -1, -1, 1, 0}; return o; }
static inline xo XM(int base, int32_t disp) { xo o = {-1, (int8_t)base, -1, 1, disp}; return o; }
static inline xo XMI(int base, int index, unsigned scale, int32_t disp) {
    xo o = {-1, (int8_t)base, (int8_t)index, (uint8_t)scale, disp};
    return o;
}

typedef struct xbuf {
    uint8_t *p, *end;
    int overflow;
} xbuf;

static inline void x8(xbuf *b, unsigned v) {
    if (b->p < b->end) *b->p++ = (uint8_t)v;
    else b->overflow = 1;
}
static inline void x16(xbuf *b, unsigned v) { x8(b, v); x8(b, v >> 8); }
static inline void x32(xbuf *b, uint32_t v) { x8(b, v); x8(b, v >> 8); x8(b, v >> 16); x8(b, v >> 24); }
static inline void x64v(xbuf *b, uint64_t v) { x32(b, (uint32_t)v); x32(b, (uint32_t)(v >> 32)); }

static inline int fits8(int64_t v) { return v >= -128 && v <= 127; }
static inline int fits32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

/* Operand-size prefix and REX for an instruction whose ModR/M reg field is
 * `r` and r/m operand is `o`. `r_byte`, `rm_byte`: that field names a byte
 * register, so registers 4-7 need a REX prefix to mean SPL, BPL, SIL and
 * DIL rather than AH, CH, DH and BH. */
static inline void x_prefix(xbuf *b, unsigned size, unsigned r, int r_byte, xo o, int rm_byte) {
    if (size == 2) x8(b, 0x66);
    unsigned rex = 0;
    if (size == 8) rex |= 8;
    if (r & 8) rex |= 4;
    if (o.reg >= 0) {
        if (o.reg & 8) rex |= 1;
    } else {
        if (o.index >= 0 && (o.index & 8)) rex |= 2;
        if (o.base >= 0 && (o.base & 8)) rex |= 1;
    }
    int need = rex != 0;
    if (r_byte && (r & 15) >= 4 && (r & 15) <= 7) need = 1;
    if (rm_byte && o.reg >= 4 && o.reg <= 7) need = 1;
    if (need) x8(b, 0x40 | rex);
}

static inline void x_modrm(xbuf *b, unsigned r, xo o) {
    r &= 7;
    if (o.reg >= 0) {
        x8(b, 0xC0 | r << 3 | (o.reg & 7));
        return;
    }
    const unsigned ss = o.scale == 8 ? 3 : o.scale == 4 ? 2 : o.scale == 2 ? 1 : 0;
    if (o.base < 0) {
        /* [index * scale + disp32] or [disp32]: SIB with base 101, mod 00. */
        x8(b, 0x04 | r << 3);
        x8(b, ss << 6 | (unsigned)((o.index >= 0 ? o.index : 4) & 7) << 3 | 5);
        x32(b, (uint32_t)o.disp);
        return;
    }
    unsigned mod;
    if (o.disp == 0 && (o.base & 7) != 5) mod = 0;
    else if (fits8(o.disp)) mod = 1;
    else mod = 2;
    if (o.index >= 0 || (o.base & 7) == 4) {
        x8(b, mod << 6 | r << 3 | 4);
        x8(b, ss << 6 | (unsigned)((o.index >= 0 ? o.index : 4) & 7) << 3 | (unsigned)(o.base & 7));
    } else {
        x8(b, mod << 6 | r << 3 | (unsigned)(o.base & 7));
    }
    if (mod == 1) x8(b, (uint8_t)o.disp);
    else if (mod == 2) x32(b, (uint32_t)o.disp);
}

/* One instruction: prefixes, `n` opcode bytes, ModR/M (+SIB, disp). */
static inline void x_op(xbuf *b, unsigned size, const uint8_t *opc, unsigned n, unsigned r, int r_is_reg, xo o) {
    x_prefix(b, size, r, r_is_reg && size == 1, o, size == 1);
    for (unsigned i = 0; i < n; i++) x8(b, opc[i]);
    x_modrm(b, r, o);
}
static inline void x_op1(xbuf *b, unsigned size, unsigned opc, unsigned r, int r_is_reg, xo o) {
    const uint8_t c[1] = {(uint8_t)opc};
    x_op(b, size, c, 1, r, r_is_reg, o);
}
static inline void x_op2(xbuf *b, unsigned size, unsigned opc1, unsigned r, int r_is_reg, xo o) {
    const uint8_t c[2] = {0x0F, (uint8_t)opc1};
    x_op(b, size, c, 2, r, r_is_reg, o);
}

/* --- moves ------------------------------------------------------------- */

/* MOV reg, r/m (8A/8B) */
static inline void x_load(xbuf *b, unsigned size, int dst, xo src) { x_op1(b, size, size == 1 ? 0x8A : 0x8B, (unsigned)dst, 1, src); }
/* MOV r/m, reg (88/89) */
static inline void x_store(xbuf *b, unsigned size, xo dst, int src) { x_op1(b, size, size == 1 ? 0x88 : 0x89, (unsigned)src, 1, dst); }
/* MOV between operands where one is a register. */
static inline void x_mov(xbuf *b, unsigned size, xo dst, xo src) {
    if (dst.reg >= 0) {
        if (src.reg == dst.reg && size >= 4) {
            if (size == 4) x_load(b, 4, dst.reg, src);   /* clears the upper half as a 32-bit move does */
            return;
        }
        x_load(b, size, dst.reg, src);
    } else {
        x_store(b, size, dst, src.reg);
    }
}
/* MOV r32, imm32 (B8+r) or a sign-extended 64-bit immediate. */
static inline void x_mov_ri(xbuf *b, int r, uint32_t imm) {
    if (r & 8) x8(b, 0x41);
    x8(b, 0xB8 + (r & 7));
    x32(b, imm);
}
static inline void x_mov_ri64(xbuf *b, int r, uint64_t imm) {
    if (imm <= 0xFFFFFFFFu) {
        x_mov_ri(b, r, (uint32_t)imm);
        return;
    }
    x8(b, 0x48 | ((r & 8) ? 1 : 0));
    x8(b, 0xB8 + (r & 7));
    x64v(b, imm);
}
/* MOV r/m, imm (C6 /0 ib, C7 /0 iw/id; size 8 sign-extends imm32). */
static inline void x_mov_mi(xbuf *b, unsigned size, xo dst, uint32_t imm) {
    if (dst.reg >= 0 && size == 4) {
        x_mov_ri(b, dst.reg, imm);
        return;
    }
    x_op1(b, size, size == 1 ? 0xC6 : 0xC7, 0, 0, dst);
    if (size == 1) x8(b, imm);
    else if (size == 2) x16(b, imm);
    else x32(b, imm);
}
/* MOVZX r32, r/m8 or r/m16 (0F B6, 0F B7); MOVSX (0F BE, 0F BF). */
static inline void x_movzx(xbuf *b, int dst, xo src, unsigned srcsize) {
    x_prefix(b, 4, (unsigned)dst, 0, src, srcsize == 1);
    x8(b, 0x0F);
    x8(b, srcsize == 1 ? 0xB6 : 0xB7);
    x_modrm(b, (unsigned)dst, src);
}
static inline void x_movsx(xbuf *b, int dst, xo src, unsigned srcsize) {
    x_prefix(b, 4, (unsigned)dst, 0, src, srcsize == 1);
    x8(b, 0x0F);
    x8(b, srcsize == 1 ? 0xBE : 0xBF);
    x_modrm(b, (unsigned)dst, src);
}
/* MOVSXD r64, r/m32 (REX.W 63). */
static inline void x_movsxd(xbuf *b, int dst, xo src) { x_op1(b, 8, 0x63, (unsigned)dst, 1, src); }
/* LEA reg, m (8D). */
static inline void x_lea(xbuf *b, unsigned size, int dst, xo m) { x_op1(b, size, 0x8D, (unsigned)dst, 1, m); }

/* --- arithmetic --------------------------------------------------------- */

/* OP r/m, reg */
static inline void x_alu_mr(xbuf *b, unsigned op, unsigned size, xo dst, int src) {
    x_op1(b, size, op << 3 | (size == 1 ? 0 : 1), (unsigned)src, 1, dst);
}
/* OP reg, r/m */
static inline void x_alu_rm(xbuf *b, unsigned op, unsigned size, int dst, xo src) {
    x_op1(b, size, op << 3 | (size == 1 ? 2 : 3), (unsigned)dst, 1, src);
}
static inline void x_alu(xbuf *b, unsigned op, unsigned size, xo dst, xo src) {
    if (src.reg >= 0) x_alu_mr(b, op, size, dst, src.reg);
    else x_alu_rm(b, op, size, dst.reg, src);
}
/* OP r/m, imm (80 /op ib; 83 /op ib sign-extended; 81 /op iw/id) */
static inline void x_alu_i(xbuf *b, unsigned op, unsigned size, xo dst, int32_t imm) {
    if (size == 1) {
        x_op1(b, 1, 0x80, op, 0, dst);
        x8(b, (uint8_t)imm);
    } else if (fits8(imm)) {
        x_op1(b, size, 0x83, op, 0, dst);
        x8(b, (uint8_t)imm);
    } else {
        x_op1(b, size, 0x81, op, 0, dst);
        if (size == 2) x16(b, (uint32_t)imm);
        else x32(b, (uint32_t)imm);
    }
}
/* OP r/m, imm32 always in the 32-bit form (81 /op id), for an immediate
 * patched later; returns the position of the immediate. */
static inline uint8_t *x_alu_i32(xbuf *b, unsigned op, unsigned size, xo dst, int32_t imm) {
    x_op1(b, size, 0x81, op, 0, dst);
    uint8_t *at = b->p;
    x32(b, (uint32_t)imm);
    return at;
}
/* TEST r/m, reg (84/85); TEST r/m, imm (F6 /0 ib, F7 /0 iw/id) */
static inline void x_test(xbuf *b, unsigned size, xo a, int r) { x_op1(b, size, size == 1 ? 0x84 : 0x85, (unsigned)r, 1, a); }
static inline void x_test_i(xbuf *b, unsigned size, xo a, uint32_t imm) {
    x_op1(b, size, size == 1 ? 0xF6 : 0xF7, 0, 0, a);
    if (size == 1) x8(b, imm);
    else if (size == 2) x16(b, imm);
    else x32(b, imm);
}
/* Group 3: NOT /2, NEG /3, MUL /4, IMUL /5, DIV /6, IDIV /7 (F6/F7). */
static inline void x_grp3(xbuf *b, unsigned sub, unsigned size, xo a) { x_op1(b, size, size == 1 ? 0xF6 : 0xF7, sub, 0, a); }
/* IMUL reg, r/m (0F AF) */
static inline void x_imul(xbuf *b, unsigned size, int dst, xo src) { x_op2(b, size, 0xAF, (unsigned)dst, 1, src); }
/* Shifts: by an immediate (C0/C1 /n ib, D0/D1 /n for 1) or by CL (D2/D3 /n). */
static inline void x_shift_i(xbuf *b, unsigned sub, unsigned size, xo a, unsigned n) {
    if (n == 1) {
        x_op1(b, size, size == 1 ? 0xD0 : 0xD1, sub, 0, a);
    } else {
        x_op1(b, size, size == 1 ? 0xC0 : 0xC1, sub, 0, a);
        x8(b, n);
    }
}
static inline void x_shift_cl(xbuf *b, unsigned sub, unsigned size, xo a) { x_op1(b, size, size == 1 ? 0xD2 : 0xD3, sub, 0, a); }
/* BT/BTS/BTR/BTC r/m, imm8 (0F BA /4-/7 ib) and BT r/m, reg (0F A3). */
static inline void x_bt_i(xbuf *b, unsigned sub, unsigned size, xo a, unsigned bit) {
    x_op2(b, size, 0xBA, sub, 0, a);
    x8(b, bit);
}
static inline void x_bt_r(xbuf *b, unsigned size, xo a, int r) { x_op2(b, size, 0xA3, (unsigned)r, 1, a); }
/* BSWAP r32 (0F C8+r) */
static inline void x_bswap(xbuf *b, int r) {
    if (r & 8) x8(b, 0x41);
    x8(b, 0x0F);
    x8(b, 0xC8 + (r & 7));
}
/* SETcc r/m8 (0F 90+cc) */
static inline void x_setcc(xbuf *b, unsigned cc, xo a) {
    x_prefix(b, 1, 0, 0, a, 1);
    x8(b, 0x0F);
    x8(b, 0x90 + cc);
    x_modrm(b, 0, a);
}
/* CMOVcc reg, r/m (0F 40+cc) */
static inline void x_cmov(xbuf *b, unsigned cc, unsigned size, int dst, xo src) { x_op2(b, size, 0x40 + cc, (unsigned)dst, 1, src); }
/* XCHG reg, r/m (86/87) */
static inline void x_xchg(xbuf *b, unsigned size, xo a, int r) { x_op1(b, size, size == 1 ? 0x86 : 0x87, (unsigned)r, 1, a); }
/* MOVBE reg, m (0F 38 F0) and MOVBE m, reg (0F 38 F1): 16, 32 bits. */
static inline void x_movbe_load(xbuf *b, unsigned size, int dst, xo m) {
    const uint8_t c[3] = {0x0F, 0x38, 0xF0};
    x_op(b, size, c, 3, (unsigned)dst, 1, m);
}
static inline void x_movbe_store(xbuf *b, unsigned size, xo m, int src) {
    const uint8_t c[3] = {0x0F, 0x38, 0xF1};
    x_op(b, size, c, 3, (unsigned)src, 1, m);
}

/* --- control ------------------------------------------------------------ */

/* Jumps return the position of their rel32 field for x_patch. */
static inline uint8_t *x_jcc(xbuf *b, unsigned cc) {
    x8(b, 0x0F);
    x8(b, 0x80 + cc);
    uint8_t *at = b->p;
    x32(b, 0);
    return at;
}
static inline uint8_t *x_jmp(xbuf *b) {
    x8(b, 0xE9);
    uint8_t *at = b->p;
    x32(b, 0);
    return at;
}
/* Points the rel32 field at `at` to `target`. */
static inline void x_patch(uint8_t *at, const uint8_t *target) {
    if (!at) return;
    const int32_t rel = (int32_t)(target - (at + 4));
    memcpy(at, &rel, 4);
}
static inline void x_jcc_to(xbuf *b, unsigned cc, const uint8_t *target) { x_patch(x_jcc(b, cc), target); }
static inline void x_jmp_to(xbuf *b, const uint8_t *target) { x_patch(x_jmp(b), target); }
/* JMP r/m64 (FF /4), CALL r/m64 (FF /2) */
static inline void x_jmp_m(xbuf *b, xo a) { x_op1(b, 4, 0xFF, 4, 0, a); }
static inline void x_call_m(xbuf *b, xo a) { x_op1(b, 4, 0xFF, 2, 0, a); }
/* CALL to an absolute address through RAX. */
static inline void x_call_abs(xbuf *b, const void *fn) {
    x_mov_ri64(b, XAX, (uint64_t)(uintptr_t)fn);
    x_call_m(b, XR(XAX));
}
/* LEA reg64, [RIP + disp32] to an absolute target. */
static inline void x_lea_rip(xbuf *b, int dst, const uint8_t *target) {
    x8(b, 0x48 | ((dst & 8) ? 4 : 0));
    x8(b, 0x8D);
    x8(b, 0x05 | (dst & 7) << 3);
    x32(b, (uint32_t)(int32_t)(target - (b->p + 4)));
}
/* JRCXZ rel8 (E3 cb): jumps when RCX is zero, leaving the flags alone. */
static inline void x_jrcxz(xbuf *b, int8_t rel) {
    x8(b, 0xE3);
    x8(b, (uint8_t)rel);
}
static inline void x_push(xbuf *b, int r) {
    if (r & 8) x8(b, 0x41);
    x8(b, 0x50 + (r & 7));
}
static inline void x_pop(xbuf *b, int r) {
    if (r & 8) x8(b, 0x41);
    x8(b, 0x58 + (r & 7));
}
static inline void x_ret(xbuf *b) { x8(b, 0xC3); }

#endif /* MC_X64EMIT_H */
