/* Instruction timing: the clock cycles each instruction takes on the CPU
 * model, from Motorola's tables.
 *
 * Sources (the column used, and how an instruction's time is composed):
 *   MC68040 UM section 10 (1993): every instruction and addressing mode has
 *   an <ea> calculate and an execute time (lead + base); the integer unit's
 *   stages overlap, so an instruction is charged the longer of the two, the
 *   pipeline's pace for it. The tables assume cache hits ("cache-hit best
 *   case"). For BR = PC (full extension format) one clock is added to both
 *   (10.1, supposition 1). Floating point (10.7): the integer unit's support
 *   time plus the FPU stages' times (conversion, execution, normalization)
 *   for normalized operands, halves rounded up. Section 10 gives no time
 *   for interrupts, access, address, trace, privilege or format errors: they
 *   take TRAP #n's 16 clocks (10.5), as the divide-by-zero note of 10.6 has
 *   it (16 clocks of exception processing plus the <ea> calculation).
 *   MC68030 UM section 11 (3rd ed. 1990): the instruction-cache case column
 *   (11.3.1: instructions in the cache, two-clock bus cycles, no overlap),
 *   the operation's time plus the effective-address table its footnote
 *   names (11.6: fea, fiea, cea, ciea, jea).
 *   MC68020 UM section 10 (3rd ed. 1989): the cache case column (10.2:
 *   instruction in the cache, no overlap), composed the same way; MOVE's
 *   table already holds both effective addresses.
 *   MC68881/MC68882 UM section 8 (1987): the overall execution times beside
 *   an MC68020 (Tables 8-2, 8-3) plus the effective-address time of Table
 *   8-1 (cache case), Tables 8-6 to 8-8 for FMOVE/FMOVEM of control and data
 *   registers, the conditional instructions and FSAVE/FRESTORE.
 *
 * Caches, write-backs, wait states, table searches and overlap between
 * instructions are not modelled beyond what the chosen column holds. The
 * interpreter charges `op[opcode]` after each instruction plus what the
 * instruction adds at run time (m68k_internal.h, cyc_extra): a branch taken,
 * MOVEM's registers, a full-format extension word, an exception. The
 * translator sums the same values per block (jit_x64.c).
 *
 * Livadix's own code; the numbers are Motorola's. */
#include "m68k_internal.h"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-const-variable"   /* manual rows no table uses */
#endif

#include <stdlib.h>

/* --- effective addresses ------------------------------------------------------------- */

enum { EC_DN, EC_AN, EC_AI, EC_PI, EC_PD, EC_DI, EC_PCDI, EC_AW, EC_AL, EC_IMM, EC_IX, EC_PCIX, EC_FULL, EC_NONE };

/* An operand's class; for the full extension format (PRM 2.1) also its
 * variant in the 68020/030 tables (v20, below) and its 68040 row (r40). */
typedef struct tea { uint8_t cls, pc, v20, r40; } tea;

static tea ea_brief(unsigned mode, unsigned reg) {
    tea t = {EC_NONE, 0, 0, 0};
    static const uint8_t m[7] = {EC_DN, EC_AN, EC_AI, EC_PI, EC_PD, EC_DI, EC_IX};
    static const uint8_t r7[5] = {EC_AW, EC_AL, EC_PCDI, EC_PCIX, EC_IMM};
    if (mode < 7) t.cls = m[mode];
    else if (reg < 5) t.cls = r7[reg];
    return t;
}

/* 68020/030 full-format variants (the rows of their address tables; B a
 * base and an index or neither, An a base register with a word
 * displacement):
 *   0 (d16,An,Xn)  1 (B)  2 (d16,B)  3 (d32,B)
 *   4-6 ([d16,An],..) with no, word, long outer displacement
 *   7-9 ([B],..)  10-12 ([d16,B],..)  13-15 ([d32,B],..)
 * 68040 rows (10.6): 11 (BR,Xn), 12 (bd,BR,Xn), 13 ([bd,BR,Xn]),
 * 14 ([bd,BR,Xn],od), 15 ([bd,BR],Xn), 16 ([bd,BR],Xn,od); a post-indexed
 * form with the index suppressed times as pre-indexed (10.1, 1). */
static tea ea_full(uint16_t ext, int pc) {
    const unsigned bs = (ext >> 7) & 1u, is = (ext >> 6) & 1u, bd = (ext >> 4) & 3u, iis = ext & 7u;
    tea t = {EC_FULL, (uint8_t)(pc && !bs), 0, 0};     /* BR = PC: a PC-relative mode, its base not suppressed */
    const unsigned od = iis & 3u, post = (iis & 4u) && !is;
    if (!iis) {
        t.v20 = (uint8_t)(bd <= 1 ? 1 : bd == 2 ? (bs ? 2 : 0) : 3);
        t.r40 = (uint8_t)(bd <= 1 ? 11 : 12);
    } else {
        const unsigned o = od ? od - 1u : 0u;
        t.v20 = (uint8_t)(bd <= 1 ? 7 + o : bd == 2 ? (bs ? 10 : 4) + o : 13 + o);
        t.r40 = (uint8_t)(post ? (od <= 1 ? 15 : 16) : (od <= 1 ? 13 : 14));
    }
    return t;
}

/* --- the 68040 (MC68040 UM section 10) --------------------------------------------------- */

/* <ea> calculate, execute (lead + base). Rows in the order of 10.6: Dn, An,
 * (An), (An)+, -(An), (d16,An), (d16,PC), (xxx).W/L, #<xxx>, (d8,An,Xn),
 * (d8,PC,Xn), then the six full-format rows. Two variants where the manual
 * gives T1/T2 (shift count immediate/register, bit number #/Dn, word/long,
 * bit field offset and width immediate/register). */
typedef struct ce { uint8_t c, e; } ce;

/* 10.6, printed pages 10-13 to 10-28 */
static const ce t40_alu[17] = {{1,1}, {1,1}, {1,1}, {1,1}, {1,1}, {1,1}, {3,3}, {1,1}, {1,1}, {3,3}, {5,5}, {6,6}, {7,7}, {10,10}, {11,12}, {11,11}, {12,13}};   /* ADD, AND, EOR, OR, SUB, TST */
static const ce t40_adda[17] = {{1,2}, {1,1}, {1,2}, {2,3}, {2,3}, {2,3}, {3,4}, {1,2}, {1,1}, {4,5}, {5,6}, {6,7}, {7,8}, {10,11}, {11,13}, {11,12}, {12,14}};
static const ce t40_alui[17] = {{1,1}, {0,0}, {1,1}, {2,2}, {2,2}, {2,2}, {0,0}, {2,2}, {0,0}, {3,3}, {0,0}, {7,7}, {8,8}, {10,11}, {11,12}, {11,12}, {12,13}};   /* ADDI, ANDI, EORI, ORI, SUBI */
static const ce t40_quick[17] = {{1,1}, {1,1}, {1,1}, {2,2}, {2,2}, {2,2}, {0,0}, {1,1}, {0,0}, {3,3}, {0,0}, {7,7}, {8,8}, {10,10}, {11,12}, {11,11}, {12,13}};  /* ADDQ, SUBQ */
static const ce t40_asl[2][17] = {{{1,3}, {0,0}, {1,3}, {1,3}, {1,3}, {1,3}, {0,0}, {1,3}, {0,0}, {3,5}, {0,0}, {7,9}, {8,10}, {10,12}, {11,13}, {11,13}, {12,14}},
    {{1,4}, {0,0}, {1,3}, {1,3}, {1,3}, {1,3}, {0,0}, {1,3}, {0,0}, {3,5}, {0,0}, {7,9}, {8,10}, {10,12}, {11,13}, {11,13}, {12,14}}};
static const ce t40_asr[2][17] = {{{1,2}, {0,0}, {1,2}, {1,2}, {1,2}, {1,2}, {0,0}, {1,2}, {0,0}, {3,4}, {0,0}, {7,8}, {8,9}, {10,11}, {11,12}, {11,12}, {12,13}},   /* ASR, LSL, LSR */
    {{1,3}, {0,0}, {1,2}, {1,2}, {1,2}, {1,2}, {0,0}, {1,2}, {0,0}, {3,4}, {0,0}, {7,8}, {8,9}, {10,11}, {11,12}, {11,12}, {12,13}}};
static const ce t40_bchg[2][17] = {{{1,3}, {0,0}, {1,3}, {1,3}, {1,3}, {2,4}, {0,0}, {2,4}, {0,0}, {3,5}, {0,0}, {7,9}, {8,10}, {10,12}, {11,13}, {11,13}, {12,14}},   /* BCHG, BCLR, BSET */
    {{1,4}, {0,0}, {1,4}, {1,4}, {1,4}, {1,4}, {0,0}, {1,4}, {0,0}, {3,6}, {0,0}, {7,10}, {8,11}, {10,13}, {11,14}, {11,14}, {12,15}}};
static const ce t40_bfchg[2][17] = {{{3,6}, {0,0}, {9,10}, {0,0}, {0,0}, {9,10}, {0,0}, {9,10}, {0,0}, {10,11}, {0,0}, {13,14}, {14,15}, {16,17}, {17,18}, {17,18}, {18,19}},   /* BFCHG, BFCLR, BFSET */
    {{4,7}, {0,0}, {9,10}, {0,0}, {0,0}, {9,10}, {0,0}, {9,10}, {0,0}, {10,11}, {0,0}, {13,14}, {14,15}, {16,17}, {17,18}, {17,18}, {18,19}}};
static const ce t40_bfext[2][17] = {{{1,4}, {0,0}, {9,9}, {0,0}, {0,0}, {9,9}, {10,10}, {9,9}, {0,0}, {10,10}, {11,11}, {13,13}, {14,14}, {16,16}, {17,17}, {17,17}, {18,18}},   /* BFEXTS, BFEXTU */
    {{2,5}, {0,0}, {9,9}, {0,0}, {0,0}, {9,9}, {10,10}, {9,9}, {0,0}, {10,10}, {11,11}, {13,13}, {14,14}, {16,16}, {17,17}, {17,17}, {18,18}}};
static const ce t40_bfffo[2][17] = {{{3,6}, {0,0}, {9,11}, {0,0}, {0,0}, {9,11}, {10,12}, {9,11}, {0,0}, {10,12}, {11,13}, {13,15}, {14,16}, {16,18}, {17,19}, {17,19}, {18,20}},
    {{4,7}, {0,0}, {9,11}, {0,0}, {0,0}, {9,11}, {10,12}, {9,11}, {0,0}, {10,12}, {11,13}, {13,15}, {14,16}, {16,18}, {17,19}, {17,19}, {18,20}}};
static const ce t40_bfins[2][17] = {{{2,5}, {0,0}, {9,9}, {0,0}, {0,0}, {9,9}, {0,0}, {9,9}, {0,0}, {10,10}, {0,0}, {13,13}, {14,14}, {16,16}, {17,17}, {17,17}, {18,18}},
    {{3,6}, {0,0}, {9,9}, {0,0}, {0,0}, {9,9}, {0,0}, {9,9}, {0,0}, {10,10}, {0,0}, {13,13}, {14,14}, {16,16}, {17,17}, {17,17}, {18,18}}};
static const ce t40_bftst[2][17] = {{{1,3}, {0,0}, {9,9}, {0,0}, {0,0}, {9,9}, {10,10}, {9,9}, {0,0}, {10,10}, {11,11}, {13,13}, {14,14}, {16,16}, {17,17}, {17,17}, {18,18}},
    {{2,4}, {0,0}, {9,9}, {0,0}, {0,0}, {9,9}, {10,10}, {9,9}, {0,0}, {10,10}, {11,11}, {13,13}, {14,14}, {16,16}, {17,17}, {17,17}, {18,18}}};
static const ce t40_btst[2][17] = {{{1,1}, {0,0}, {1,1}, {1,1}, {1,1}, {2,2}, {3,3}, {2,2}, {0,0}, {3,3}, {5,5}, {7,7}, {8,8}, {10,10}, {11,11}, {11,11}, {12,12}},
    {{1,2}, {0,0}, {1,2}, {1,2}, {1,2}, {1,2}, {3,4}, {1,2}, {0,0}, {3,4}, {5,6}, {6,8}, {7,9}, {9,11}, {10,12}, {10,12}, {11,13}}};
static const ce t40_cas[17] = {{0,0}, {0,0}, {36,37}, {37,36}, {37,36}, {37,36}, {0,0}, {36,36}, {0,0}, {36,36}, {0,0}, {36,36}, {37,37}, {42,40}, {42,42}, {42,41}, {42,42}};
static const ce t40_chk[17] = {{8,8}, {0,0}, {9,9}, {9,9}, {9,9}, {9,9}, {10,10}, {9,9}, {8,8}, {10,10}, {11,11}, {12,12}, {13,13}, {16,16}, {17,17}, {17,17}, {18,18}};
static const ce t40_chk2[17] = {{0,0}, {0,0}, {11,11}, {0,0}, {0,0}, {11,11}, {12,12}, {11,11}, {0,0}, {13,13}, {14,14}, {15,15}, {16,16}, {19,19}, {20,20}, {20,20}, {21,21}};
static const ce t40_clr[17] = {{1,1}, {0,0}, {1,1}, {1,1}, {1,1}, {1,1}, {0,0}, {1,1}, {0,0}, {3,3}, {0,0}, {6,6}, {7,7}, {9,9}, {10,10}, {10,10}, {11,11}};
static const ce t40_cmp[17] = {{1,1}, {1,1}, {1,1}, {1,1}, {1,1}, {1,1}, {3,3}, {1,1}, {1,1}, {3,3}, {5,5}, {6,6}, {7,7}, {9,9}, {10,10}, {10,10}, {11,11}};
static const ce t40_cmpa[17] = {{1,1}, {1,1}, {1,1}, {2,2}, {2,2}, {2,2}, {3,3}, {1,1}, {1,1}, {3,3}, {5,5}, {6,6}, {7,7}, {9,9}, {10,10}, {10,10}, {11,11}};   /* CMPA.L */
static const ce t40_cmpi[17] = {{1,1}, {0,0}, {1,1}, {2,2}, {2,2}, {2,2}, {3,3}, {2,2}, {0,0}, {3,3}, {5,6}, {6,7}, {7,8}, {9,10}, {10,11}, {10,11}, {11,12}};
static const ce t40_cmp2[17] = {{0,0}, {0,0}, {13,13}, {0,0}, {0,0}, {13,13}, {14,14}, {13,13}, {0,0}, {15,15}, {16,16}, {17,17}, {18,18}, {21,21}, {22,22}, {22,22}, {23,23}};
static const ce t40_divw[17] = {{8,27}, {0,0}, {8,27}, {8,27}, {8,27}, {8,27}, {11,30}, {8,27}, {8,27}, {11,30}, {12,31}, {13,32}, {14,33}, {17,36}, {18,37}, {18,37}, {19,38}};
static const ce t40_divl[17] = {{9,44}, {0,0}, {9,44}, {9,44}, {9,44}, {11,46}, {12,47}, {11,46}, {10,45}, {12,47}, {13,48}, {14,49}, {15,50}, {18,53}, {19,54}, {19,54}, {20,55}};
static const ce t40_jmp[17] = {{0,0}, {0,0}, {3,3}, {0,0}, {0,0}, {4,4}, {6,6}, {3,3}, {0,0}, {6,6}, {7,7}, {8,8}, {9,9}, {12,12}, {12,12}, {13,13}, {14,14}};
static const ce t40_jsr[17] = {{0,0}, {0,0}, {3,3}, {0,0}, {0,0}, {4,4}, {6,6}, {3,3}, {0,0}, {6,6}, {7,7}, {8,8}, {9,9}, {12,12}, {13,13}, {13,13}, {14,14}};
static const ce t40_lea[17] = {{0,0}, {0,0}, {1,1}, {0,0}, {0,0}, {2,2}, {4,4}, {1,1}, {0,0}, {4,4}, {5,5}, {6,6}, {7,7}, {9,9}, {10,10}, {10,10}, {11,11}};
static const ce t40_from_ccr[17] = {{1,2}, {0,0}, {1,2}, {1,2}, {1,2}, {1,2}, {0,0}, {1,2}, {0,0}, {3,4}, {0,0}, {6,7}, {7,8}, {10,11}, {11,12}, {11,12}, {12,13}};
static const ce t40_to_ccr[17] = {{1,2}, {0,0}, {1,2}, {1,2}, {1,2}, {1,2}, {3,4}, {1,2}, {1,2}, {3,4}, {4,5}, {6,7}, {7,8}, {10,11}, {11,12}, {11,12}, {12,13}};
static const ce t40_from_sr[17] = {{2,3}, {0,0}, {2,3}, {2,3}, {2,3}, {2,3}, {0,0}, {2,3}, {0,0}, {4,5}, {0,0}, {6,7}, {7,8}, {10,11}, {11,12}, {11,12}, {12,13}};
/* MOVE to SR: (BR,Xn) is printed blank; (bd,BR,Xn)'s row stands for it. */
static const ce t40_to_sr[17] = {{9,9}, {0,0}, {10,10}, {10,10}, {10,10}, {10,10}, {11,11}, {10,10}, {9,9}, {11,11}, {12,12}, {14,14}, {14,14}, {17,17}, {18,18}, {18,18}, {19,19}};
static const ce t40_moves_an[17] = {{0,0}, {0,0}, {28,28}, {28,28}, {17,17}, {29,28}, {0,0}, {17,17}, {0,0}, {29,28}, {0,0}, {21,21}, {22,22}, {35,34}, {31,31}, {36,35}, {32,32}};   /* MOVES <ea>,An */
static const ce t40_moves_dn[17] = {{0,0}, {0,0}, {20,23}, {20,23}, {11,12}, {21,23}, {0,0}, {11,14}, {0,0}, {21,23}, {0,0}, {15,16}, {16,17}, {26,29}, {23,26}, {27,30}, {24,27}};   /* MOVES <ea>,Dn */
static const ce t40_moves_w[17] = {{0,0}, {0,0}, {13,13}, {13,13}, {11,11}, {14,13}, {0,0}, {11,11}, {0,0}, {14,13}, {0,0}, {15,15}, {16,16}, {21,19}, {20,20}, {21,20}, {21,21}};   /* MOVES Rn,<ea> */
static const ce t40_muls[2][17] = {{{1,16}, {0,0}, {1,16}, {1,16}, {1,16}, {1,16}, {3,18}, {1,16}, {1,16}, {3,18}, {5,20}, {6,21}, {7,22}, {9,24}, {10,25}, {10,25}, {11,26}},
    {{1,20}, {0,0}, {1,20}, {1,20}, {1,20}, {2,20}, {3,22}, {2,20}, {1,20}, {3,22}, {5,24}, {6,25}, {7,26}, {9,28}, {10,29}, {10,29}, {11,30}}};
static const ce t40_mulu[2][17] = {{{1,14}, {0,0}, {1,14}, {1,14}, {1,14}, {1,14}, {3,14}, {1,14}, {1,14}, {3,16}, {5,18}, {6,19}, {7,20}, {9,22}, {10,23}, {10,23}, {11,24}},
    {{1,20}, {0,0}, {1,20}, {1,20}, {1,20}, {2,20}, {3,20}, {2,20}, {1,20}, {3,22}, {5,24}, {6,25}, {7,26}, {9,28}, {10,29}, {10,29}, {11,30}}};
static const ce t40_nbcd[17] = {{1,3}, {0,0}, {1,2}, {1,2}, {1,2}, {1,2}, {0,0}, {1,2}, {0,0}, {3,4}, {0,0}, {6,7}, {7,8}, {9,10}, {10,11}, {10,11}, {11,12}};
static const ce t40_neg[17] = {{1,1}, {0,0}, {1,1}, {1,1}, {1,1}, {1,1}, {0,0}, {1,1}, {0,0}, {3,3}, {0,0}, {6,6}, {7,7}, {9,9}, {10,10}, {10,10}, {11,11}};   /* NEG, NEGX, NOT */
static const ce t40_pea[17] = {{0,0}, {0,0}, {2,2}, {0,0}, {0,0}, {2,2}, {4,4}, {2,2}, {0,0}, {4,4}, {6,6}, {7,7}, {8,8}, {10,10}, {11,11}, {11,11}, {12,12}};
static const ce t40_rol[2][17] = {{{1,3}, {0,0}, {1,3}, {1,3}, {1,3}, {1,3}, {0,0}, {1,3}, {0,0}, {3,5}, {0,0}, {6,8}, {7,9}, {9,11}, {10,12}, {10,12}, {11,13}},
    {{1,4}, {0,0}, {1,3}, {1,3}, {1,3}, {1,3}, {0,0}, {1,3}, {0,0}, {3,5}, {0,0}, {6,8}, {7,9}, {9,11}, {10,12}, {10,12}, {11,13}}};
static const ce t40_roxl[2][17] = {{{1,5}, {0,0}, {1,2}, {1,2}, {1,2}, {1,2}, {0,0}, {1,2}, {0,0}, {3,4}, {0,0}, {6,7}, {7,8}, {9,10}, {10,11}, {10,11}, {11,12}},
    {{1,6}, {0,0}, {1,2}, {1,2}, {1,2}, {1,2}, {0,0}, {1,2}, {0,0}, {3,4}, {0,0}, {6,7}, {7,8}, {9,10}, {10,11}, {10,11}, {11,12}}};
static const ce t40_scc[17] = {{1,2}, {0,0}, {1,2}, {1,2}, {1,2}, {1,2}, {0,0}, {1,2}, {0,0}, {4,5}, {0,0}, {6,7}, {7,8}, {10,11}, {11,12}, {11,12}, {12,13}};
static const ce t40_suba[17] = {{1,1}, {1,2}, {1,2}, {2,3}, {2,3}, {2,3}, {3,4}, {1,2}, {1,2}, {4,5}, {5,6}, {6,7}, {7,8}, {9,10}, {10,11}, {10,11}, {11,12}};
static const ce t40_tas[17] = {{1,2}, {0,0}, {26,26}, {26,26}, {26,26}, {26,26}, {0,0}, {26,26}, {0,0}, {27,27}, {0,0}, {30,29}, {31,30}, {33,33}, {35,34}, {34,34}, {36,35}};
/* MOVEA.L (10-23); MOVEA.W adds one clock to the execute time but for Dn
 * and #<xxx> (its note a). */
static const ce t40_movea[17] = {{1,1}, {1,1}, {1,1}, {1,1}, {1,1}, {1,1}, {3,3}, {1,1}, {1,1}, {4,4}, {5,5}, {6,6}, {7,7}, {10,10}, {11,11}, {11,11}, {12,12}};
/* MOVEM (10-23) without the registers' part: <list>,<ea> and <ea>,<list>;
 * each register adds a clock to both stages (D' + A', D' at least one). */
static const ce t40_movem_w[17] = {{0,0}, {0,0}, {2,2}, {0,0}, {2,2}, {2,2}, {0,0}, {2,2}, {0,0}, {9,9}, {0,0}, {11,11}, {12,12}, {15,15}, {16,16}, {16,16}, {17,17}};
static const ce t40_movem_r[17] = {{0,0}, {0,0}, {3,3}, {3,3}, {0,0}, {3,3}, {4,4}, {3,3}, {0,0}, {10,10}, {11,11}, {12,12}, {13,13}, {16,16}, {17,17}, {17,17}, {18,18}};

/* 10.4 MOVE: source rows (Dn, (An), (An)+, -(An), (d16,An), (d16,PC),
 * (xxx), #<xxx>, (d8,An,Xn), (d8,PC,Xn), (bd,BR,Xn), the four memory
 * indirect forms) x destination columns (Dn, (An), (An)+, -(An), (d16,An),
 * (xxx), (d8,An,Xn), (bd,An,Xn), the four memory indirect forms). */
static const ce t40_move[15][12] = {
    {{1,1}, {1,1}, {1,1}, {1,1}, {1,1}, {1,1}, {3,3}, {7,7}, {10,10}, {11,11}, {11,11}, {12,12}},
    {{1,1}, {1,1}, {2,2}, {2,2}, {2,2}, {1,1}, {4,4}, {7,7}, {10,10}, {11,11}, {11,11}, {12,12}},
    {{1,1}, {2,2}, {2,2}, {2,2}, {2,2}, {2,2}, {4,4}, {7,7}, {10,10}, {11,11}, {11,11}, {12,12}},
    {{1,1}, {2,2}, {2,2}, {2,2}, {2,2}, {2,2}, {4,4}, {7,7}, {10,10}, {11,11}, {11,11}, {12,12}},
    {{1,1}, {2,2}, {2,2}, {2,2}, {2,2}, {2,2}, {4,4}, {7,7}, {10,10}, {11,11}, {11,11}, {12,12}},
    {{3,3}, {3,3}, {3,3}, {3,3}, {4,4}, {4,4}, {8,8}, {10,10}, {13,13}, {14,14}, {14,14}, {15,15}},
    {{1,1}, {1,1}, {2,2}, {2,2}, {2,2}, {2,2}, {4,4}, {7,7}, {10,10}, {11,11}, {11,11}, {12,12}},
    {{1,1}, {1,1}, {2,2}, {2,2}, {2,2}, {2,2}, {3,3}, {7,7}, {10,10}, {11,11}, {11,11}, {12,12}},
    {{3,3}, {4,4}, {5,5}, {5,5}, {5,5}, {5,5}, {8,8}, {10,10}, {13,13}, {14,14}, {14,14}, {15,15}},
    {{5,5}, {5,5}, {6,6}, {6,6}, {6,6}, {6,6}, {9,9}, {11,11}, {14,14}, {15,15}, {15,15}, {16,16}},
    {{7,7}, {7,7}, {8,8}, {8,8}, {8,8}, {8,8}, {11,11}, {13,13}, {16,16}, {17,17}, {17,17}, {18,18}},
    {{10,10}, {10,10}, {11,11}, {11,11}, {11,11}, {11,11}, {14,14}, {16,16}, {19,19}, {20,20}, {20,20}, {21,21}},
    {{11,11}, {11,11}, {12,12}, {12,12}, {12,12}, {12,12}, {15,15}, {17,17}, {20,20}, {21,21}, {21,21}, {22,22}},
    {{11,11}, {11,11}, {12,12}, {12,12}, {12,12}, {12,12}, {15,15}, {17,17}, {20,20}, {21,21}, {21,21}, {22,22}},
    {{12,12}, {12,12}, {13,13}, {13,13}, {13,13}, {13,13}, {16,16}, {18,18}, {21,21}, {22,22}, {22,22}, {23,23}},
};

static unsigned r40(const tea *t) {
    static const uint8_t row[EC_NONE] = {0, 1, 2, 3, 4, 5, 6, 7, 7, 8, 9, 10, 0};
    return t->cls == EC_FULL ? t->r40 : t->cls < EC_NONE ? row[t->cls] : 0;
}
static unsigned max2(unsigned a, unsigned b) { return a > b ? a : b; }
/* The 68040's time for a row: the longer stage; BR = PC adds one to each. */
static unsigned g40(const ce *g, const tea *t) {
    const ce x = g[r40(t)];
    const unsigned pc = t->cls == EC_FULL && t->pc;
    return max2(x.c + pc, x.e + pc);
}

/* --- the 68020 and 68030 effective-address tables ------------------------------------------- */

/* Indices: 0 Dn, 1 An, 2 (An), 3 (An)+, 4 -(An), 5 (d16,An) or (d16,PC),
 * 6 (xxx).W, 7 (xxx).L, 8 #<data>.B/.W, 9 (d8,An,Xn) or (d8,PC,Xn),
 * 10-25 the full-format variants (ea_full), 26 #<data>.L. In the fiea and
 * ciea tables (the immediate source, .W or .L, then the destination) 8 and
 * 26 are an immediate destination of a two-word instruction. */
typedef uint8_t eat[27];

static unsigned ix23(const tea *t, int imm_long) {
    switch (t->cls) {
    case EC_DN: return 0;
    case EC_AN: return 1;
    case EC_AI: return 2;
    case EC_PI: return 3;
    case EC_PD: return 4;
    case EC_DI: case EC_PCDI: return 5;
    case EC_AW: return 6;
    case EC_AL: return 7;
    case EC_IMM: return imm_long ? 26 : 8;
    case EC_IX: case EC_PCIX: return 9;
    case EC_FULL: return 10u + t->v20;
    default: return 0;
    }
}

/* MC68030 UM 11.6.1-11.6.5, instruction-cache case. Rows the manual does
 * not print take their nearest printed one: fiea #<data>.W,#<data>.B/.W as
 * .L; the 68030 full-format destinations ([d16,An]) of MOVE as
 * ([d16,An],Xn). */
static const eat fea30 = {0, 0, 3, 3, 4, 4, 4, 4, 2, 6, 6, 6, 8, 12, 10, 12, 12, 10, 12, 12, 12, 14, 14, 16, 18, 18, 4};
static const eat fiea30w = {2, 2, 3, 5, 4, 4, 6, 6, 6, 8, 8, 8, 10, 14, 12, 14, 14, 12, 14, 14, 14, 16, 16, 18, 20, 20, 6};
static const eat fiea30l = {4, 4, 4, 7, 4, 6, 8, 8, 8, 10, 10, 10, 12, 16, 14, 16, 16, 14, 16, 16, 16, 18, 18, 20, 22, 22, 8};
static const eat cea30 = {0, 0, 2, 2, 2, 2, 2, 4, 0, 4, 6, 6, 8, 12, 10, 12, 12, 10, 12, 12, 12, 14, 14, 16, 18, 18, 0};
static const eat ciea30w = {2, 2, 2, 4, 2, 4, 4, 6, 4, 6, 8, 8, 10, 14, 12, 14, 14, 12, 14, 14, 14, 16, 16, 18, 20, 20, 4};
static const eat ciea30l = {4, 4, 4, 6, 4, 6, 6, 8, 6, 8, 10, 10, 12, 16, 14, 16, 16, 14, 16, 16, 16, 18, 18, 20, 22, 22, 6};
static const eat jea30 = {0, 0, 2, 0, 0, 4, 2, 2, 0, 6, 6, 6, 8, 12, 10, 12, 12, 10, 12, 12, 12, 14, 14, 16, 18, 18, 0};
/* 11.7.1, the MMU instructions' effective addresses */
static const eat mmu30 = {0, 0, 4, 4, 4, 4, 4, 6, 0, 4, 8, 8, 10, 16, 12, 12, 14, 12, 12, 14, 14, 14, 16, 20, 20, 22, 0};
/* 11.6.6, MOVE's destination, plus the source's fea; [k][1] for a memory
 * or immediate source where the manual separates it (SOURCE,(An) and the
 * like). */
static const uint8_t move30[27][2] = {
    {2, 2}, {2, 2}, {3, 4}, {3, 4}, {4, 4}, {4, 4}, {4, 4}, {6, 6}, {0, 0}, {6, 6},
    {8, 8}, {8, 8}, {10, 10}, {14, 14}, {10, 10}, {12, 12}, {14, 14}, {10, 10}, {12, 12}, {14, 14},
    {12, 12}, {14, 14}, {16, 16}, {16, 16}, {18, 18}, {20, 20}, {0, 0}};

/* MC68020 UM 10.2.1-10.2.5, cache case. The 68020 has no separate ([d16,An])
 * rows: they are its ([d16,B],I) rows. Rows the manual does not print:
 * ciea -(An) as (An); fiea ([d16,B],I,d32) as ([d16,B],I,d16). */
static const eat fea20 = {0, 0, 4, 4, 5, 5, 4, 4, 2, 7, 7, 7, 9, 13, 14, 16, 16, 12, 14, 14, 14, 16, 16, 18, 20, 20, 4};
static const eat fiea20w = {2, 2, 4, 6, 5, 5, 5, 6, 4, 9, 9, 9, 11, 15, 16, 18, 18, 14, 16, 16, 16, 18, 18, 20, 22, 22, 6};
static const eat fiea20l = {4, 4, 4, 8, 7, 7, 7, 8, 6, 11, 11, 11, 13, 17, 18, 20, 20, 16, 18, 18, 18, 20, 20, 22, 24, 24, 8};
static const eat cea20 = {0, 0, 2, 2, 2, 2, 2, 4, 0, 4, 6, 6, 8, 12, 13, 15, 15, 11, 13, 13, 13, 15, 15, 17, 19, 19, 0};
static const eat ciea20w = {2, 2, 2, 4, 2, 4, 4, 4, 2, 6, 8, 8, 10, 14, 15, 17, 17, 13, 15, 15, 15, 17, 17, 19, 21, 21, 2};
static const eat ciea20l = {4, 4, 4, 6, 4, 6, 6, 8, 4, 8, 10, 10, 12, 16, 17, 19, 19, 15, 17, 17, 17, 19, 19, 21, 23, 23, 4};
static const eat jea20 = {0, 0, 2, 0, 0, 4, 2, 2, 0, 6, 6, 6, 8, 12, 13, 15, 15, 11, 13, 13, 13, 15, 15, 17, 19, 19, 0};
/* 10.2.6 MOVE, cache case: source rows (Rn, #<data>.B/.W, #<data>.L, (An),
 * (An)+, -(An), (d16,An), (xxx).W, (xxx).L, (d8,An,Xn), (d16,An,Xn), (B),
 * (d16,B), (d32,B), ([B],I) and its d16/d32, ([d16,B],I) and its d16/d32,
 * ([d32,B],I) and its d16/d32) x destination columns (An, Dn, then the same
 * memory modes). */
static const uint8_t move20[23][22] = {
    {2, 2, 4, 4, 5, 5, 4, 6, 7, 9, 8, 10, 14, 12, 14, 15, 14, 16, 17, 18, 20, 21},
    {4, 4, 6, 6, 7, 7, 6, 8, 7, 9, 8, 10, 14, 12, 14, 15, 14, 16, 17, 18, 20, 21},
    {6, 6, 8, 8, 9, 9, 8, 10, 9, 11, 10, 12, 16, 14, 16, 17, 16, 18, 19, 20, 22, 23},
    {6, 6, 7, 7, 7, 7, 7, 9, 9, 11, 10, 12, 16, 14, 16, 17, 16, 18, 19, 20, 22, 23},
    {6, 6, 7, 7, 7, 7, 7, 9, 9, 11, 10, 12, 16, 14, 16, 17, 16, 18, 19, 20, 22, 23},
    {7, 7, 8, 8, 8, 8, 8, 10, 10, 12, 11, 13, 17, 15, 17, 18, 17, 19, 20, 21, 23, 24},
    {7, 7, 8, 8, 8, 8, 8, 10, 10, 12, 11, 13, 17, 15, 17, 18, 17, 19, 20, 21, 23, 24},
    {6, 6, 7, 7, 7, 7, 7, 9, 9, 11, 10, 12, 16, 14, 16, 17, 16, 18, 19, 20, 22, 23},
    {6, 6, 7, 7, 7, 7, 7, 9, 9, 11, 10, 12, 16, 14, 16, 17, 16, 18, 19, 20, 22, 23},
    {9, 9, 10, 10, 10, 10, 10, 12, 12, 14, 13, 15, 19, 17, 19, 20, 19, 21, 22, 23, 25, 26},
    {9, 9, 10, 10, 10, 10, 10, 12, 12, 14, 13, 15, 19, 17, 19, 20, 19, 21, 22, 23, 25, 26},
    {9, 9, 10, 10, 10, 10, 10, 12, 12, 14, 13, 15, 19, 17, 19, 20, 19, 21, 22, 23, 25, 26},
    {11, 11, 12, 12, 12, 12, 12, 14, 14, 16, 15, 17, 21, 19, 21, 22, 21, 23, 24, 25, 27, 28},
    {15, 15, 16, 16, 16, 16, 16, 18, 18, 20, 19, 21, 25, 23, 25, 26, 25, 27, 28, 29, 31, 32},
    {14, 14, 15, 15, 15, 15, 15, 17, 17, 19, 18, 20, 24, 22, 24, 25, 24, 26, 27, 28, 30, 31},
    {16, 16, 17, 17, 17, 17, 17, 19, 19, 21, 20, 22, 26, 24, 26, 27, 26, 28, 29, 30, 32, 33},
    {16, 16, 17, 17, 17, 17, 17, 19, 19, 21, 20, 22, 26, 24, 26, 27, 26, 28, 29, 30, 32, 33},
    {16, 16, 17, 17, 17, 17, 17, 19, 19, 21, 20, 22, 26, 24, 26, 27, 26, 28, 29, 30, 32, 33},
    {18, 18, 19, 19, 19, 19, 19, 21, 21, 23, 22, 24, 28, 26, 28, 29, 28, 30, 31, 32, 34, 35},
    {18, 18, 19, 19, 19, 19, 19, 21, 21, 23, 22, 24, 28, 26, 28, 29, 28, 30, 31, 32, 34, 35},
    {20, 20, 21, 21, 21, 21, 21, 23, 23, 25, 24, 26, 30, 28, 30, 31, 30, 32, 33, 34, 36, 37},
    {22, 22, 23, 23, 23, 23, 23, 25, 25, 27, 26, 28, 32, 30, 32, 33, 32, 34, 35, 36, 38, 39},
    {22, 22, 23, 23, 23, 23, 23, 25, 25, 27, 26, 28, 32, 30, 32, 33, 32, 34, 35, 36, 38, 39},
};
static unsigned move20_row(const tea *t, int imm_long) {
    static const uint8_t full[16] = {10, 11, 12, 13, 17, 18, 19, 14, 15, 16, 17, 18, 19, 20, 21, 22};
    switch (t->cls) {
    case EC_DN: case EC_AN: return 0;
    case EC_IMM: return imm_long ? 2 : 1;
    case EC_AI: return 3;
    case EC_PI: return 4;
    case EC_PD: return 5;
    case EC_DI: case EC_PCDI: return 6;
    case EC_AW: return 7;
    case EC_AL: return 8;
    case EC_IX: case EC_PCIX: return 9;
    case EC_FULL: return full[t->v20];
    default: return 0;
    }
}
static unsigned move20_col(const tea *t) {
    static const uint8_t full[16] = {9, 10, 11, 12, 16, 17, 18, 13, 14, 15, 16, 17, 18, 19, 20, 21};
    switch (t->cls) {
    case EC_AN: return 0;
    case EC_DN: return 1;
    case EC_AI: return 2;
    case EC_PI: return 3;
    case EC_PD: return 4;
    case EC_DI: return 5;
    case EC_AW: return 6;
    case EC_AL: return 7;
    case EC_IX: return 8;
    case EC_FULL: return full[t->v20];
    default: return 1;
    }
}

/* --- instruction families --------------------------------------------------------------- */

enum {
    F_NONE, F_MOVE, F_MOVEA, F_MOVEQ, F_ALU_ER, F_ALU_RE, F_EOR, F_CMP, F_ADDA, F_SUBA, F_CMPA, F_ALUI, F_CMPI, F_QUICK,
    F_ADDX_R, F_ADDX_M, F_BCD_R, F_BCD_M, F_CMPM, F_PACK_R, F_PACK_M, F_UNPK_R, F_UNPK_M, F_NBCD,
    F_CLR, F_NEG, F_TST, F_EXT, F_EXTB, F_SWAP, F_EXG, F_SCC, F_TAS,
    F_SHIFT_R, F_SHIFT_M, F_BIT_I, F_BIT_R, F_BF,
    F_BCC, F_BRA, F_BSR, F_DBCC, F_JMP, F_JSR, F_LEA, F_PEA, F_LINK_W, F_LINK_L, F_UNLK, F_RTS, F_RTR, F_RTD, F_RTE,
    F_TRAP, F_TRAPV, F_TRAPCC, F_CHK, F_CMP2, F_CAS, F_CAS2, F_MULW, F_MULL, F_DIVW, F_DIVL,
    F_MOVEM_RM, F_MOVEM_MR, F_MOVEP, F_MOVEC, F_MOVES, F_TO_USP, F_FROM_USP, F_FROM_CCR, F_TO_CCR, F_FROM_SR, F_TO_SR,
    F_CCR_I, F_SR_I, F_NOP, F_STOP, F_RESET, F_ILLEGAL, F_LINEA, F_LINEF, F_BKPT, F_MOVE16, F_CINV, F_CPUSH,
    F_PFLUSH, F_PTEST, F_PMMU, F_FPU, F_CALLM, F_RTM, F_COUNT
};

static unsigned family_of(const char *n, unsigned op) {
    static const struct { const char *name; uint8_t f; } exact[] = {
        {"MOVEQ", F_MOVEQ}, {"EXT.W", F_EXT}, {"EXT.L", F_EXT}, {"EXTB.L", F_EXTB}, {"SWAP", F_SWAP}, {"EXG", F_EXG},
        {"Scc", F_SCC}, {"TAS", F_TAS}, {"NBCD", F_NBCD}, {"shift mem", F_SHIFT_M}, {"DBcc", F_DBCC}, {"JMP", F_JMP},
        {"JSR", F_JSR}, {"LEA", F_LEA}, {"PEA", F_PEA}, {"LINK.W", F_LINK_W}, {"LINK.L", F_LINK_L}, {"UNLK", F_UNLK},
        {"RTS", F_RTS}, {"RTR", F_RTR}, {"RTD", F_RTD}, {"RTE", F_RTE}, {"TRAP", F_TRAP}, {"TRAPV", F_TRAPV},
        {"TRAPcc", F_TRAPCC}, {"TRAPcc.W", F_TRAPCC}, {"TRAPcc.L", F_TRAPCC}, {"CHK.W", F_CHK}, {"CHK.L", F_CHK},
        {"MULU.W", F_MULW}, {"MULS.W", F_MULW}, {"MULx.L", F_MULL}, {"DIVU.W", F_DIVW}, {"DIVS.W", F_DIVW},
        {"DIVx.L", F_DIVL}, {"MOVEP", F_MOVEP}, {"MOVEC", F_MOVEC}, {"MOVE to USP", F_TO_USP},
        {"MOVE from USP", F_FROM_USP}, {"MOVE from CCR", F_FROM_CCR}, {"MOVE to CCR", F_TO_CCR},
        {"MOVE from SR", F_FROM_SR}, {"MOVE to SR", F_TO_SR}, {"ORI to CCR", F_CCR_I}, {"ANDI to CCR", F_CCR_I},
        {"EORI to CCR", F_CCR_I}, {"ORI to SR", F_SR_I}, {"ANDI to SR", F_SR_I}, {"EORI to SR", F_SR_I}, {"NOP", F_NOP},
        {"STOP", F_STOP}, {"RESET", F_RESET}, {"ILLEGAL", F_ILLEGAL}, {"line A", F_LINEA}, {"line F", F_LINEF},
        {"BKPT", F_BKPT}, {"MOVE16", F_MOVE16}, {"CINV", F_CINV}, {"CPUSH", F_CPUSH}, {"PFLUSH", F_PFLUSH},
        {"PTEST", F_PTEST}, {"PMMU", F_PMMU}, {"FPU", F_FPU}, {"FSAVE/FRESTORE", F_FPU}, {"CALLM", F_CALLM},
        {"RTM", F_RTM}, {"CAS2.W", F_CAS2}, {"CAS2.L", F_CAS2},
    };
    for (size_t i = 0; i < sizeof exact / sizeof exact[0]; i++)
        if (!strcmp(n, exact[i].name)) return exact[i].f;
    if (!strcmp(n, "Bcc")) {
        const unsigned cc = (op >> 8) & 15u;
        return cc == 0 ? F_BRA : cc == 1 ? F_BSR : F_BCC;
    }
    const size_t len = strlen(n);
    /* Sized names: the part before the size suffix. */
    char base[24];
    size_t b = 0;
    while (b < len && n[b] != '.' && b + 1 < sizeof base) { base[b] = n[b]; b++; }
    base[b] = 0;
    if (!strcmp(base, "MOVE")) return F_MOVE;
    if (!strcmp(base, "MOVEA")) return F_MOVEA;
    if (!strcmp(base, "ADDA")) return F_ADDA;
    if (!strcmp(base, "SUBA")) return F_SUBA;
    if (!strcmp(base, "CMPA")) return F_CMPA;
    if (!strcmp(base, "CMP")) return F_CMP;
    if (!strcmp(base, "EOR")) return F_EOR;
    if (!strcmp(base, "ADD") || !strcmp(base, "SUB") || !strcmp(base, "AND") || !strcmp(base, "OR"))
        return (op & 0x0100u) ? F_ALU_RE : F_ALU_ER;
    if (!strcmp(base, "ORI") || !strcmp(base, "ANDI") || !strcmp(base, "SUBI") || !strcmp(base, "ADDI") || !strcmp(base, "EORI"))
        return F_ALUI;
    if (!strcmp(base, "CMPI")) return F_CMPI;
    if (!strcmp(base, "ADDQ") || !strcmp(base, "SUBQ")) return F_QUICK;
    if (!strcmp(base, "ADDX") || !strcmp(base, "SUBX")) return (op & 8u) ? F_ADDX_M : F_ADDX_R;
    if (!strcmp(base, "ABCD") || !strcmp(base, "SBCD")) return (op & 8u) ? F_BCD_M : F_BCD_R;
    if (!strcmp(base, "PACK")) return (op & 8u) ? F_PACK_M : F_PACK_R;
    if (!strcmp(base, "UNPK")) return (op & 8u) ? F_UNPK_M : F_UNPK_R;
    if (!strcmp(base, "CMPM")) return F_CMPM;
    if (!strcmp(base, "CLR")) return F_CLR;
    if (!strcmp(base, "NEG") || !strcmp(base, "NEGX") || !strcmp(base, "NOT")) return F_NEG;
    if (!strcmp(base, "TST")) return F_TST;
    if (!strcmp(base, "shift")) return F_SHIFT_R;
    if (!strcmp(base, "BTST") || !strcmp(base, "BCHG") || !strcmp(base, "BCLR") || !strcmp(base, "BSET"))
        return (op & 0x0100u) ? F_BIT_R : F_BIT_I;
    if (n[0] == 'B' && n[1] == 'F') return F_BF;
    if (!strcmp(base, "CMP2/CHK2")) return F_CMP2;
    if (!strcmp(base, "CAS")) return F_CAS;
    if (!strcmp(base, "MOVEM")) return (op & 0x0400u) ? F_MOVEM_MR : F_MOVEM_RM;
    if (!strcmp(base, "MOVES")) return F_MOVES;
    return F_NONE;
}

/* Operand size of the S3 encodings, MOVE's and the like: 1, 2 or 4. */
static unsigned size67(unsigned op) { const unsigned s = (op >> 6) & 3u; return s == 0 ? 1u : s == 1 ? 2u : 4u; }
static unsigned move_size(unsigned op) { return (op >> 12) == 1 ? 1u : (op >> 12) == 3 ? 2u : 4u; }

/* --- the cost of one instruction --------------------------------------------------------------- */

/* A timing query: the instruction and what it found at run time. */
typedef struct tq {
    uint16_t op;
    tea ea, ea2;                /* the EA in bits 5-0; MOVE's destination */
    uint8_t nd, na;             /* MOVEM's data and address registers */
    unsigned flags;
} tq;

enum {
    Q_TAKEN = M68K_TQ_TAKEN,        /* Bcc/BRA/BSR taken; DBcc branching */
    Q_EXPIRED = M68K_TQ_EXPIRED,    /* DBcc: cc false, counter expired */
    Q_TRAP = M68K_TQ_TRAP,          /* CHK, CHK2, TRAPcc, TRAPV, a divide by zero: the exception taken */
    Q_REGARG = M68K_TQ_REGARG,      /* bit fields: offset or width in a register */
    Q_BIG = M68K_TQ_BIG,            /* a register shift count above the operand's size (68030) */
    Q_SPAN = M68K_TQ_SPAN,          /* a bit field spanning two long words (68020/030: five bytes) */
    Q_FAIL = M68K_TQ_FAIL,          /* CAS/CAS2: the compare failed */
    Q_CHK2 = M68K_TQ_CHK2,          /* CMP2/CHK2: the CHK2 form */
    Q_AN = M68K_TQ_AN,              /* CHK2/CMP2 of an address register; MOVES to/from An */
    Q_WRAP = M68K_TQ_WRAP,          /* CHK2/CMP2: the upper bound below the lower */
    Q_SIGNED = M68K_TQ_SIGNED,      /* MULx.L/DIVx.L: signed */
    Q_WRITE = M68K_TQ_WRITE,        /* MOVES Rn,<ea> */
    Q_CRB = M68K_TQ_CRB             /* MOVEC Rn to SFC, DFC or CACR (68030 Cr-B) */
};

static unsigned char fam[M68K_MODEL_COUNT][65536];

static void query(m68k_model model, uint16_t op, tq *q) {
    memset(q, 0, sizeof *q);
    q->op = op;
    q->ea = ea_brief((op >> 3) & 7u, op & 7u);
    const unsigned f = fam[model][op];
    if (f == F_MOVE) q->ea2 = ea_brief((op >> 6) & 7u, (op >> 9) & 7u);
    else q->ea2.cls = EC_NONE;
}

static unsigned movem_regs(unsigned nd, unsigned na) { return (nd ? nd : 1u) + na; }

static unsigned cost040(const tq *q, unsigned f) {
    const unsigned op = q->op;
    const tea *e = &q->ea;
    const int dn = e->cls == EC_DN;
    switch (f) {
    case F_MOVE: {
        static const uint8_t src[EC_NONE] = {0, 0, 1, 2, 3, 4, 5, 6, 6, 7, 8, 9, 0};
        static const uint8_t dst[EC_NONE] = {0, 0, 1, 2, 3, 4, 0, 5, 5, 0, 6, 0, 0};
        static const uint8_t mi[17] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10, 10, 11, 12, 13, 14};
        const unsigned s = e->cls == EC_FULL ? mi[e->r40] : src[e->cls];
        const unsigned d = q->ea2.cls == EC_FULL ? (q->ea2.r40 <= 12 ? 7u : q->ea2.r40 - 5u) : dst[q->ea2.cls];
        const ce x = t40_move[s][d];
        const unsigned pc = (e->cls == EC_FULL && e->pc) ? 1u : 0u;
        return max2(x.c + pc, x.e + pc);
    }
    case F_MOVEA: {
        unsigned t = g40(t40_movea, e);
        if (((op >> 12) == 3) && !dn && e->cls != EC_IMM) {
            const ce x = t40_movea[r40(e)];
            const unsigned pc = e->cls == EC_FULL && e->pc;
            t = max2(x.c + pc, x.e + 1u + pc);
        }
        return t;
    }
    case F_MOVEQ: return 1;
    case F_ALU_ER: case F_ALU_RE: case F_EOR: case F_TST: return g40(t40_alu, e);
    case F_CMP: return g40(t40_cmp, e);
    case F_ADDA: return g40(t40_adda, e);
    case F_SUBA: return g40(t40_suba, e);
    case F_CMPA: return g40(t40_cmpa, e);
    case F_ALUI: return g40(t40_alui, e);
    case F_CMPI: return g40(t40_cmpi, e);
    case F_QUICK: return g40(t40_quick, e);
    case F_ADDX_R: return 1;
    case F_ADDX_M: return 3;
    case F_BCD_R: return 3;
    case F_BCD_M: return 4;
    case F_CMPM: return 3;
    case F_PACK_R: return 3;
    case F_PACK_M: return 5;
    case F_UNPK_R: return 4;
    case F_UNPK_M: return 6;
    case F_NBCD: return g40(t40_nbcd, e);
    case F_CLR: return g40(t40_clr, e);
    case F_NEG: return g40(t40_neg, e);
    case F_EXT: return ((op >> 6) & 7u) == 2 ? 2 : 1;
    case F_EXTB: return 1;
    case F_SWAP: return 2;
    case F_EXG: return ((op >> 3) & 0x1Fu) == 0x09 ? 2 : 1;
    case F_SCC: return g40(t40_scc, e);
    case F_TAS: return g40(t40_tas, e);
    case F_SHIFT_R: {
        const unsigned type = (op >> 3) & 3u, left = (op >> 8) & 1u, k = (op & 0x20u) ? 1u : 0u;
        const tea r = {EC_DN, 0, 0, 0};
        const ce (*g)[17] = type == 0 ? (left ? t40_asl : t40_asr) : type == 1 ? t40_asr : type == 2 ? t40_roxl : t40_rol;
        return g40(g[k], &r);
    }
    case F_SHIFT_M: {
        const unsigned type = (op >> 9) & 3u, left = (op >> 8) & 1u;
        const ce (*g)[17] = type == 0 ? (left ? t40_asl : t40_asr) : type == 1 ? t40_asr : type == 2 ? t40_roxl : t40_rol;
        return g40(g[0], e);
    }
    case F_BIT_I: case F_BIT_R: {
        const unsigned k = f == F_BIT_R, t = (op >> 6) & 3u;
        /* BTST Dn,#<data>: 10.6 gives no #<xxx> row; the immediate operand
         * comes with the instruction, which the 68040's other tables (ADD,
         * CMP, TST) charge as Dn: Livadix takes the Dn row. */
        if (t == 0 && e->cls == EC_IMM) return g40(t40_btst[k], &(tea){.cls = EC_DN});
        return g40(t == 0 ? t40_btst[k] : t40_bchg[k], e);
    }
    case F_BF: {
        /* 10.6 notes: a field spanning two long words adds ten and nine
         * clocks (BFCHG, BFCLR, BFSET), seven and seven (BFINS), two to
         * the execute time (the others). */
        const unsigned k = (q->flags & Q_REGARG) ? 1u : 0u, which = (op >> 8) & 7u;
        const ce *g;
        unsigned sc = 0, se = 2;
        switch (which) {
        case 0: g = t40_bftst[k]; break;
        case 1: case 3: g = t40_bfext[k]; break;
        case 5: g = t40_bfffo[k]; break;
        case 7: g = t40_bfins[k]; sc = se = 7; break;
        default: g = t40_bfchg[k]; sc = 10; se = 9; break;
        }
        const ce x = g[r40(e)];
        const unsigned pc = e->cls == EC_FULL && e->pc, span = (q->flags & Q_SPAN) != 0;
        return max2(x.c + pc + (span ? sc : 0u), x.e + pc + (span ? se : 0u));
    }
    case F_BCC: return (q->flags & Q_TAKEN) ? 2 : 3;
    case F_BRA: return 2;
    case F_BSR: return 2;
    case F_DBCC: return (q->flags & Q_TAKEN) ? 3 : 4;   /* false, count > -1: 3; count = -1 and true: 4 */
    case F_JMP: return g40(t40_jmp, e);
    case F_JSR: return g40(t40_jsr, e);
    case F_LEA: return g40(t40_lea, e);
    case F_PEA: return g40(t40_pea, e);
    case F_LINK_W: case F_LINK_L: return 3;
    case F_UNLK: return 2;
    case F_RTS: return 5;
    case F_RTR: return 7;
    case F_RTD: return 6;
    case F_RTE: return 0;               /* by frame format (m68k_op_rte) */
    case F_TRAP: return 0;              /* the exception (x_trap) */
    case F_TRAPV: case F_TRAPCC: return (q->flags & Q_TRAP) ? 19 : 5;
    case F_CHK: return (q->flags & Q_TRAP) ? 16u + t40_chk[r40(e)].c : g40(t40_chk, e);
    case F_CMP2: {
        const ce *g = (q->flags & Q_CHK2) ? t40_chk2 : t40_cmp2;
        const ce x = g[r40(e)];
        unsigned add = e->cls == EC_FULL && e->pc;
        if (q->flags & Q_AN) add++;
        if (q->flags & Q_WRAP) add += 3;
        if (q->flags & Q_TRAP) return 16u + x.c + add;
        return max2(x.c + add, x.e + add);
    }
    case F_CAS: return g40(t40_cas, e);
    case F_CAS2: return (q->flags & Q_FAIL) ? 51 : 56;
    case F_MULW: return g40(((op >> 8) & 1u) ? t40_muls[0] : t40_mulu[0], e);
    case F_MULL: return g40((q->flags & Q_SIGNED) ? t40_muls[1] : t40_mulu[1], e);
    case F_DIVW: return (q->flags & Q_TRAP) ? 16u + t40_divw[r40(e)].c : g40(t40_divw, e);
    case F_DIVL: return (q->flags & Q_TRAP) ? 16u + t40_divl[r40(e)].c : g40(t40_divl, e);
    case F_MOVEM_RM: return g40(t40_movem_w, e) + movem_regs(q->nd, q->na);
    case F_MOVEM_MR: return g40(t40_movem_r, e) + movem_regs(q->nd, q->na) + (((op >> 6) & 1u) ? 0u : q->na);
    case F_MOVEP: {
        const unsigned opm = (op >> 6) & 7u;   /* 4 .W to Dn, 5 .L to Dn, 6 .W to memory, 7 .L to memory */
        return opm == 4 ? 7 : opm == 5 ? 10 : opm == 6 ? 11 : 13;
    }
    case F_MOVEC: return (op & 1u) ? 7 : 11;
    case F_MOVES: return g40((q->flags & Q_WRITE) ? t40_moves_w : (q->flags & Q_AN) ? t40_moves_an : t40_moves_dn, e);
    case F_TO_USP: return 7;
    case F_FROM_USP: return 3;
    case F_FROM_CCR: return g40(t40_from_ccr, e);
    case F_TO_CCR: return g40(t40_to_ccr, e);
    case F_FROM_SR: return g40(t40_from_sr, e);
    case F_TO_SR: return g40(t40_to_sr, e);
    case F_CCR_I: return 4;
    case F_SR_I: return 9;
    case F_NOP: return 8;
    case F_STOP: return 8;              /* not in the tables: NOP's synchronizing time */
    case F_RESET: return 521;
    case F_ILLEGAL: case F_LINEA: case F_LINEF: case F_BKPT: return 0;  /* the exception (x_trap) */
    case F_MOVE16: {
        /* (Ax)+,(Ay)+ 8; by bits 4-3: (An)+,xxx.L 7, xxx.L,(An)+ 8,
         * (An),xxx.L 7, xxx.L,(An) 7 */
        static const uint8_t t[4] = {7, 8, 7, 7};
        if ((op & 0xFFF8u) == 0xF620u) return 8;
        return t[(op >> 3) & 3u];
    }
    case F_CINV: return ((op >> 3) & 3u) == 2 ? 266 : 9;
    case F_CPUSH: return ((op >> 3) & 3u) == 1 ? 6 : 267;
    case F_PFLUSH: return ((op >> 3) & 3u) == 2 ? 27 : 11;
    case F_PTEST: return 25;
    default: return 0;                  /* FPU and the rest: charged by their handlers */
    }
}

/* The 68020 and 68030 share the composition; the constants differ. */
static unsigned cost2030(m68k_model model, const tq *q, unsigned f) {
    const int m30 = model == M68K_68030;
    const unsigned op = q->op;
    const tea *e = &q->ea;
    const eat *fea = m30 ? &fea30 : &fea20, *cea = m30 ? &cea30 : &cea20, *jea = m30 ? &jea30 : &jea20;
    const eat *fiw = m30 ? &fiea30w : &fiea20w, *fil = m30 ? &fiea30l : &fiea20l;
    const eat *ciw = m30 ? &ciea30w : &ciea20w;
    const unsigned sz = size67(op);
    const unsigned x = ix23(e, 0);
#define FEA(sz_) ((*fea)[ix23(e, (sz_) == 4)])
#define FIEA(sz_) ((sz_) == 4 ? (*fil)[x] : (*fiw)[x])
#define FIEA_W2 ((*fiw)[ix23(e, 1)])                /* two-word instructions: #<data>.W, then the (long) operand */
#define CIEA_W2 ((*ciw)[ix23(e, 1)])
    const int dn = e->cls == EC_DN || e->cls == EC_AN;
    switch (f) {
    case F_MOVE: case F_MOVEA: {
        const unsigned s = f == F_MOVEA ? ((op >> 12) == 3 ? 2u : 4u) : move_size(op);
        tea d = q->ea2;
        if (f == F_MOVEA) d.cls = EC_AN;
        if (!m30) return move20[move20_row(e, s == 4)][move20_col(&d)];
        const unsigned di = d.cls == EC_FULL ? 10u + d.v20 : ix23(&d, 0);
        const unsigned mem = !(e->cls == EC_DN || e->cls == EC_AN);
        return move30[di][mem] + (*fea)[ix23(e, s == 4)];
    }
    case F_MOVEQ: return 2;
    case F_ALU_ER: return 2 + FEA(sz);
    case F_ALU_RE: return (m30 ? 3 : 4) + FEA(sz);
    case F_EOR: return dn ? 2 : (m30 ? 3 : 4) + FEA(sz);
    case F_CMP: return 2 + FEA(sz);
    case F_ADDA: case F_SUBA: {
        const unsigned w = !((op >> 8) & 1u);
        if (!m30) return 2 + FEA(w ? 2 : 4);
        return (w ? 4 : 2) + FEA(w ? 2 : 4);
    }
    case F_CMPA: return 4 + FEA(((op >> 8) & 1u) ? 4 : 2);
    case F_ALUI: return (dn ? 2 : (m30 ? 3 : 4)) + FIEA(sz);
    case F_CMPI: return 2 + FIEA(sz);
    case F_QUICK: return (dn ? 2 : (m30 ? 3 : 4)) + (dn ? 0 : FEA(sz));
    case F_ADDX_R: return 2;
    case F_ADDX_M: return m30 ? 9 : 12;
    case F_BCD_R: return 4;
    case F_BCD_M: return m30 ? 13 : 16;
    case F_CMPM: return m30 ? 8 : 9;
    case F_PACK_R: return 6;
    case F_PACK_M: return m30 ? 11 : 13;
    case F_UNPK_R: return 8;
    case F_UNPK_M: return m30 ? 11 : 13;
    case F_NBCD: return 6 + (dn ? 0 : FEA(1));    /* the tables give NBCD Dn only; memory adds its fea */
    case F_CLR: return dn ? 2 : (m30 ? 3 : 4) + (*cea)[x];
    case F_NEG: return dn ? 2 : (m30 ? 3 : 4) + FEA(sz);
    case F_TST: return 2 + FEA(sz);
    case F_EXT: case F_EXTB: return 4;
    case F_SWAP: return 4;
    case F_EXG: return m30 ? 4 : 2;
    case F_SCC: return dn ? 4 : (m30 ? 5 : 6) + (*cea)[x];
    case F_TAS: return dn ? 4 : 12 + (*cea)[x];
    case F_SHIFT_R: {
        const unsigned type = (op >> 3) & 3u, left = (op >> 8) & 1u, reg = (op & 0x20u) != 0;
        if (!m30) {
            if (type == 1) return reg ? 6 : 4;
            if (type == 0) return left ? 8 : 6;
            return type == 2 ? 12 : 8;
        }
        if (type == 1 || (type == 0 && !left)) {
            if (!reg) return 4;
            return (q->flags & Q_BIG) ? (type == 1 ? 8 : 10) : 6;
        }
        if (type == 0) return reg ? 8 : 6;
        if (type == 2) return 12;
        return reg ? 8 : 6;
    }
    case F_SHIFT_M: {
        const unsigned type = (op >> 9) & 3u, left = (op >> 8) & 1u;
        unsigned t;
        if (m30) t = type == 1 ? 4 : type == 0 ? (left ? 6 : 4) : type == 2 ? 4 : 6;
        else t = type == 1 ? 5 : type == 0 ? (left ? 6 : 5) : type == 2 ? 5 : 7;
        return t + FEA(2);
    }
    case F_BIT_I: case F_BIT_R: {
        const unsigned t = (op >> 6) & 3u, base = m30 ? (t ? 6u : 4u) : 4u;
        if (dn) return base;
        return base + (f == F_BIT_I ? (*fiw)[ix23(e, 0)] : FEA(1));
    }
    case F_BF: {
        /* Dn, memory (<5 bytes), memory (5 bytes); plus the ciea of the
         * second word (#<data>.W) and the operand. */
        static const uint8_t t30[8][3] = {{8, 10, 14}, {10, 12, 18}, {14, 14, 22}, {10, 12, 18}, {14, 14, 22}, {20, 22, 28}, {14, 14, 22}, {12, 12, 18}};
        static const uint8_t t20[8][3] = {{6, 11, 15}, {8, 13, 18}, {12, 16, 24}, {8, 13, 18}, {12, 16, 24}, {18, 24, 32}, {12, 16, 24}, {10, 14, 20}};
        const unsigned which = (op >> 8) & 7u;
        const uint8_t *t = m30 ? t30[which] : t20[which];
        if (e->cls == EC_DN) return t[0];
        return t[(q->flags & Q_SPAN) ? 2 : 1] + CIEA_W2;
    }
    case F_BCC: {
        if (q->flags & Q_TAKEN) return 6;
        const unsigned d = op & 0xFFu;
        return d == 0 || d == 0xFF ? 6u : 4u;
    }
    case F_BRA: return 6;
    case F_BSR: return m30 ? 6 : 7;
    case F_DBCC: return (q->flags & Q_EXPIRED) ? 10 : 6;
    case F_JMP: return 4 + (*jea)[x];
    case F_JSR: return (m30 ? 4 : 5) + (*jea)[x];
    case F_LEA: return 2 + (*cea)[x];
    case F_PEA: return (m30 ? 4 : 5) + (*cea)[x];
    case F_LINK_W: return m30 ? 4 : 5;
    case F_LINK_L: return 6;
    case F_UNLK: return m30 ? 5 : 6;
    case F_RTS: return m30 ? 9 : 10;
    case F_RTR: return m30 ? 12 : 14;
    case F_RTD: return 10;
    case F_RTE: return 0;
    case F_TRAP: return 0;
    case F_TRAPV: return (q->flags & Q_TRAP) ? (m30 ? 22 : 25) : 4;
    case F_TRAPCC: {
        const unsigned k = op & 7u;     /* 2 .W, 3 .L, 4 none */
        if (q->flags & Q_TRAP) return m30 ? (k == 2 ? 24 : k == 3 ? 26 : 22) : 25;
        return k == 2 ? 6 : k == 3 ? 8 : 4;
    }
    case F_CHK:
        /* The 68020's table has no exception-taken row: TRAP #n's 20 added. */
        if (m30) return ((q->flags & Q_TRAP) ? 28 : 8) + FEA(((op >> 7) & 1u) ? 2 : 4);
        return 8 + FEA(((op >> 7) & 1u) ? 2 : 4) + ((q->flags & Q_TRAP) ? 20u : 0u);
    case F_CMP2: {
        const unsigned s2 = (op >> 9) & 3u;
        const unsigned ea = (*fiw)[ix23(e, s2 == 2)];
        if (q->flags & Q_TRAP) return (m30 ? 40u : 38u) + ea;
        return (m30 && !(q->flags & Q_CHK2) ? 20u : 18u) + ea;     /* 68030: CMP2 20, CHK2 18 */
    }
    case F_CAS: return (m30 ? ((q->flags & Q_FAIL) ? 11 : 13) : ((q->flags & Q_FAIL) ? 12 : 15)) + CIEA_W2;
    case F_CAS2: return m30 ? 24 : ((q->flags & Q_FAIL) ? 22 : 25);
    case F_MULW: return (m30 ? 28 : 27) + FEA(2);
    case F_MULL: return (m30 ? 44 : 43) + FIEA_W2;
    case F_DIVW: {
        if (q->flags & Q_TRAP) return (m30 ? 18 : 20) + FEA(2);   /* the exception, the division not made */
        const unsigned s = (op >> 8) & 1u;
        return (s ? 56 : 44) + FEA(2);
    }
    case F_DIVL:
        if (q->flags & Q_TRAP) return (m30 ? 18 : 20) + FIEA_W2;
        return ((q->flags & Q_SIGNED) ? 90 : 78) + FIEA_W2;
    case F_MOVEM_RM: return 4 + (m30 ? 2u : 3u) * (q->nd + q->na) + CIEA_W2;
    case F_MOVEM_MR: return 8 + 4u * (q->nd + q->na) + CIEA_W2;
    case F_MOVEP: {
        const unsigned opm = (op >> 6) & 7u;
        if (m30) return (opm & 1u) ? 14 : 10;
        return opm == 4 ? 12 : opm == 5 ? 18 : opm == 6 ? 11 : 17;
    }
    case F_MOVEC: return (op & 1u) ? (m30 ? ((q->flags & Q_CRB) ? 12 : 6) : 12) : 6;
    case F_MOVES: return ((q->flags & Q_WRITE) ? 5 : 7) + CIEA_W2;
    case F_TO_USP: case F_FROM_USP: return m30 ? 4 : 2;
    case F_FROM_CCR: case F_FROM_SR: return dn ? 4 : (m30 ? 4 : 5) + (*cea)[x];
    case F_TO_CCR: return 4 + FEA(2);
    case F_TO_SR: return 8 + FEA(2);
    case F_CCR_I: case F_SR_I: return 12;
    case F_NOP: return 2;
    case F_STOP: return 8;
    case F_RESET: return 518;
    case F_ILLEGAL: case F_LINEA: case F_LINEF: return 0;
    case F_BKPT: return m30 ? 9 : 10;
    case F_CALLM: return 30 + FIEA_W2;      /* type 0 */
    case F_RTM: return 19;                  /* type 0 */
    default: return 0;
    }
#undef FEA
#undef FIEA
#undef FIEA_W2
#undef CIEA_W2
}

static unsigned cost_of(m68k_model model, const tq *q) {
    const unsigned f = fam[model][q->op];
    return model >= M68K_68040 ? cost040(q, f) : cost2030(model, q, f);
}

/* --- the per-model tables ----------------------------------------------------------------- */

static uint16_t tables[M68K_MODEL_COUNT][65536];
static uint16_t unit_table[65536];
static m68k_timing timings[M68K_MODEL_COUNT];
/* Untimed (m68k_init's default, the tests'): one count per instruction,
 * nothing added. */
static const m68k_timing unit_timing = {.unit = 1, .bcc_taken = 1, .dbcc_branch = 1, .dbcc_expired = 1, .op = unit_table};
static int built;

void m68k_timing_init(void) {
    if (built) return;
    m68k_tables_init();
    for (unsigned op = 0; op < 65536; op++) unit_table[op] = 1;
    for (unsigned model = 0; model < M68K_MODEL_COUNT; model++) {
        if (model == M68K_68000) {
            /* The 68000 counts its bus cycles itself (m68k_000.c): timed,
             * its table is zero. */
            timings[model].op = tables[model];
            continue;
        }
        for (unsigned op = 0; op < 65536; op++) {
            const m68k_opinfo info = m68k_decode((m68k_model)model, (uint16_t)op);
            fam[model][op] = (unsigned char)(info.name ? family_of(info.name, op) : F_NONE);
        }
        for (unsigned op = 0; op < 65536; op++) {
            tq q;
            query((m68k_model)model, (uint16_t)op, &q);
            tables[model][op] = (uint16_t)cost_of((m68k_model)model, &q);
        }
        m68k_timing *t = &timings[model];
        memset(t, 0, sizeof *t);
        const int e40 = model >= M68K_68040, m30 = model == M68K_68030;
        t->unit = 0;
        /* Bcc taken (all sizes) and BRA; DBcc cc false: branching, expired. */
        t->bcc_taken = e40 ? 2 : 6;
        t->dbcc_branch = e40 ? 3 : 6;
        t->dbcc_expired = e40 ? 4 : 10;
        t->movem_reg[0] = e40 ? 1 : m30 ? 2 : 3;    /* registers to memory */
        t->movem_reg[1] = e40 ? 1 : 4;              /* memory to registers */
        t->movem_040 = (uint8_t)e40;
        /* Exceptions (MC68040 UM 10.5 TRAP #n and the 10.6 DIV note; MC68030
         * UM 11.6.17-18: TRAP #n, TRACE, interrupts, bus cycle faults short
         * and long; MC68020 UM 10.2.17-18). */
        t->x_trap = e40 ? 16 : m30 ? 18 : 20;
        t->x_trace = e40 ? 16 : m30 ? 22 : 25;
        t->x_access = e40 ? 16 : m30 ? 62 : 79;
        t->x_address = e40 ? 16 : m30 ? 36 : 43;
        t->x_int = e40 ? 16 : m30 ? 23 : 26;
        t->x_int_m = e40 ? 0 : m30 ? 10 : 15;       /* the M-stack interrupt's second frame */
        /* RTE by frame format (68040 10.5; 68030 11.6.18; 68020 10.2.18). */
        static const uint16_t rte40[16] = {13, 23, 14, 20, 15, 0, 0, 23};
        static const uint16_t rte30[16] = {18, 12, 18, 0, 0, 0, 0, 0, 0, 26, 36, 76};
        static const uint16_t rte20[16] = {21, 16, 21, 0, 0, 0, 0, 0, 0, 32, 43, 92};
        memcpy(t->rte, e40 ? rte40 : m30 ? rte30 : rte20, sizeof t->rte);
        t->op = tables[model];
    }
    built = 1;
}

const uint16_t *m68k_timing_table(m68k_model model, int timed) {
    m68k_timing_init();
    return timed && model < M68K_MODEL_COUNT ? tables[model] : unit_table;
}
const m68k_timing *m68k_timing_of(m68k_model model, int timed) {
    m68k_timing_init();
    return timed && model < M68K_MODEL_COUNT ? &timings[model] : &unit_timing;
}

void m68k_set_timing(m68k_cpu *cpu, int timed) {
    cpu->tm = m68k_timing_of(cpu->model, timed);
    cpu->ctab = cpu->tm->op;
    /* Translations hold the counts they were made with. */
    if (cpu->jit) m68k_jit_flush(cpu);
}
int m68k_timed(const m68k_cpu *cpu) { return cpu->tm && !cpu->tm->unit; }
unsigned m68k_cycles_of(m68k_model model, uint16_t op) { return m68k_timing_table(model, 1)[op]; }

/* --- what instructions add at run time ------------------------------------------------------- */

static int extra(const m68k_cpu *cpu, const tq *q) {
    if (cpu->tm->unit) return 0;
    return (int)cost_of(cpu->model, q) - (int)cpu->ctab[q->op];
}

int m68k_timing_full(const m68k_cpu *cpu, uint16_t op, unsigned slot, uint16_t ext, int pc) {
    if (cpu->tm->unit) return 0;
    tq q;
    query(cpu->model, op, &q);
    if (slot) q.ea2 = ea_full(ext, pc);
    else q.ea = ea_full(ext, pc);
    return extra(cpu, &q);
}

/* Which EA an index extension word at `at` belongs to: MOVE's source
 * extension words follow the opcode word, its destination's come after;
 * any other instruction has one EA that may be indexed (bits 5-0). */
unsigned m68k_timing_slot(const m68k_cpu *cpu, uint32_t at) {
    const unsigned op = cpu->op;
    if (!(op >> 12) || (op >> 12) > 3) return 0;
    const unsigned sm = (op >> 3) & 7u, sr = op & 7u;
    const int src_indexed = sm == 6 || (sm == 7 && sr == 3);
    return src_indexed && at == cpu->ipc + 2u ? 0u : 1u;
}

int m68k_timing_query(const m68k_cpu *cpu, uint16_t op, unsigned flags) {
    if (cpu->tm->unit) return 0;
    tq q;
    query(cpu->model, op, &q);
    q.flags = flags;
    return extra(cpu, &q);
}

int m68k_timing_movem(const m68k_timing *tm, uint16_t op, uint32_t mask) {
    if (tm->unit) return 0;
    unsigned nd = 0, na = 0;
    for (unsigned i = 0; i < 16; i++)
        if (mask & (1u << i)) {
            /* -(An) lists run backwards: bit 0 is A7 */
            const unsigned reg = ((op >> 3) & 7u) == 4 ? 15u - i : i;
            if (reg < 8) nd++;
            else na++;
        }
    if (tm->movem_040) {
        /* D' + A' with D' at least one (the table holds one); MOVEM.W to
         * registers adds a clock per address register (10.6 note c). */
        const unsigned w_mr = (op & 0x0400u) && !(op & 0x0040u);
        return (int)((nd ? nd : 1u) - 1u + na + (w_mr ? na : 0u));
    }
    return (int)(tm->movem_reg[(op >> 10) & 1u] * (nd + na));
}

/* --- floating point ---------------------------------------------------------------------- */

/* MC68881/MC68882 UM Table 8-1, the MPU's effective address for a
 * coprocessor instruction, cache case (by the 68020/030 index, ix23). */
static const eat fpea = {0, 0, 2, 6, 6, 2, 2, 4, 0, 4, 6, 6, 8, 14, 13, 13, 15, 11, 11, 13, 13, 13, 15, 19, 19, 21, 0};

/* Tables 8-2 (MC68881) and 8-3 (MC68882), totals: by opmode, the columns
 * FPm, integer, single, double, extended, packed. */
typedef struct fpop { uint8_t opmode; uint16_t t81[6], t82[6]; } fpop;
static const fpop fp_ops[] = {
    {0x00, {33, 60, 52, 58, 56, 870}, {21, 48, 34, 40, 46, 891}},          /* FMOVE to FPn */
    {0x01, {55, 82, 74, 80, 78, 892}, {58, 88, 71, 77, 83, 913}},          /* FINT */
    {0x02, {687, 714, 706, 712, 710, 1524}, {690, 720, 703, 709, 715, 1545}},   /* FSINH */
    {0x03, {55, 82, 74, 80, 78, 892}, {58, 88, 71, 77, 83, 913}},          /* FINTRZ */
    {0x04, {107, 134, 126, 132, 130, 944}, {110, 140, 123, 129, 135, 965}},     /* FSQRT */
    {0x06, {571, 598, 590, 596, 594, 1408}, {574, 604, 587, 593, 599, 1429}},   /* FLOGNP1 */
    {0x08, {545, 572, 564, 570, 568, 1382}, {548, 578, 561, 567, 573, 1403}},   /* FETOXM1 */
    {0x09, {661, 688, 680, 686, 684, 1498}, {664, 694, 677, 683, 689, 1519}},   /* FTANH */
    {0x0A, {403, 430, 422, 428, 426, 1240}, {406, 436, 419, 425, 431, 1261}},   /* FATAN */
    {0x0C, {581, 608, 600, 606, 604, 1418}, {584, 614, 597, 603, 609, 1439}},   /* FASIN */
    {0x0D, {693, 720, 712, 718, 716, 1530}, {696, 726, 709, 715, 721, 1551}},   /* FATANH */
    {0x0E, {391, 418, 410, 416, 414, 1228}, {394, 424, 407, 413, 419, 1249}},   /* FSIN */
    {0x0F, {473, 500, 492, 498, 496, 1310}, {476, 506, 489, 495, 501, 1331}},   /* FTAN */
    {0x10, {497, 524, 516, 522, 520, 1334}, {500, 530, 513, 519, 525, 1355}},   /* FETOX */
    {0x11, {567, 594, 586, 592, 590, 1404}, {570, 600, 583, 589, 595, 1425}},   /* FTWOTOX */
    {0x12, {567, 594, 586, 592, 590, 1404}, {570, 600, 583, 589, 595, 1425}},   /* FTENTOX */
    {0x14, {525, 552, 544, 550, 548, 1362}, {528, 558, 541, 547, 553, 1383}},   /* FLOGN */
    {0x15, {581, 608, 600, 606, 604, 1418}, {584, 614, 597, 603, 609, 1439}},   /* FLOG10 */
    {0x16, {581, 608, 600, 606, 604, 1418}, {584, 614, 597, 603, 609, 1439}},   /* FLOG2 */
    {0x18, {35, 62, 54, 60, 58, 872}, {38, 68, 51, 57, 63, 893}},          /* FABS */
    {0x19, {607, 634, 626, 632, 630, 1444}, {610, 640, 623, 629, 635, 1465}},   /* FCOSH */
    {0x1A, {35, 62, 54, 60, 58, 872}, {38, 68, 51, 57, 63, 893}},          /* FNEG */
    {0x1C, {625, 652, 644, 650, 648, 1462}, {628, 658, 641, 647, 653, 1483}},   /* FACOS */
    {0x1D, {391, 418, 410, 416, 414, 1228}, {394, 424, 407, 413, 419, 1249}},   /* FCOS */
    {0x1E, {45, 72, 64, 70, 68, 882}, {48, 78, 61, 67, 73, 903}},          /* FGETEXP */
    {0x1F, {31, 58, 50, 56, 54, 868}, {34, 64, 47, 53, 59, 889}},          /* FGETMAN */
    {0x20, {103, 132, 124, 130, 128, 940}, {108, 146, 121, 127, 133, 961}},     /* FDIV */
    {0x21, {70, 99, 91, 97, 95, 907}, {75, 113, 88, 94, 100, 928}},        /* FMOD */
    {0x22, {51, 80, 72, 78, 76, 888}, {56, 94, 69, 75, 81, 909}},          /* FADD */
    {0x23, {71, 100, 92, 98, 96, 908}, {76, 114, 89, 95, 101, 929}},       /* FMUL */
    {0x24, {69, 98, 90, 96, 94, 906}, {74, 112, 87, 93, 99, 927}},         /* FSGLDIV */
    {0x25, {100, 129, 121, 127, 125, 937}, {105, 143, 118, 124, 130, 958}},     /* FREM */
    {0x26, {41, 70, 62, 68, 66, 878}, {46, 84, 59, 65, 71, 899}},          /* FSCALE */
    {0x27, {59, 88, 80, 86, 84, 896}, {64, 102, 77, 83, 89, 917}},         /* FSGLMUL */
    {0x28, {51, 80, 72, 78, 76, 888}, {56, 94, 69, 75, 81, 909}},          /* FSUB */
    {0x30, {451, 478, 470, 476, 474, 1288}, {454, 484, 467, 473, 479, 1309}},   /* FSINCOS */
    {0x38, {33, 62, 54, 60, 58, 870}, {38, 76, 51, 57, 63, 891}},          /* FCMP */
    {0x3A, {33, 60, 52, 58, 56, 870}, {36, 66, 49, 55, 61, 891}},          /* FTST */
};

/* The 68040's integer-unit support for <ea>,FPn (10.7.2), the longer stage,
 * by row (r40) and source format column: byte/word, long, single, double,
 * extended; FPm as source is 3 (the extended column's FPn row). */
static const uint8_t fp40_in[17][5] = {
    {3, 3, 3, 0, 0}, {0, 0, 0, 0, 0}, {2, 2, 2, 2, 3}, {2, 2, 2, 2, 3}, {2, 2, 2, 2, 3}, {2, 2, 2, 2, 3}, {4, 4, 4, 4, 5},
    {3, 3, 3, 3, 4}, {5, 3, 3, 4, 5}, {5, 5, 5, 5, 6}, {6, 6, 6, 6, 7}, {7, 7, 7, 7, 8}, {8, 8, 8, 8, 9},
    {11, 11, 11, 11, 12}, {12, 12, 12, 12, 13}, {12, 12, 12, 12, 13}, {13, 13, 13, 13, 14}};
/* FMOVE FPn,<ea>: byte/word/long, single/double, extended. */
static const uint8_t fp40_out[17][3] = {
    {12, 4, 0}, {0, 0, 0}, {11, 3, 4}, {11, 3, 4}, {11, 3, 4}, {11, 3, 4}, {0, 0, 0}, {11, 3, 4}, {0, 0, 0},
    {11, 5, 6}, {0, 0, 0}, {10, 7, 8}, {11, 8, 9}, {11, 11, 12}, {12, 12, 13}, {12, 12, 13}, {13, 13, 14}};
/* One control register; FMOVEM list (three more per further register, one
 * more for a dynamic list); FScc. */
static const uint8_t fp40_ctl[17][3] = {
    {3, 0, 6}, {3, 0, 0}, {5, 17, 5}, {5, 17, 7}, {6, 16, 7}, {5, 17, 5}, {7, 0, 0}, {5, 19, 5}, {5, 19, 0},
    {6, 19, 8}, {7, 20, 0}, {8, 20, 10}, {9, 21, 11}, {12, 25, 14}, {14, 25, 15}, {13, 26, 15}, {15, 26, 16}};
/* FSAVE and FRESTORE: idle or null, short, long frame. */
static const uint8_t fp40_save[17][3] = {
    {0, 0, 0}, {0, 0, 0}, {12, 33, 50}, {0, 0, 0}, {11, 32, 49}, {12, 33, 50}, {0, 0, 0}, {13, 34, 51}, {0, 0, 0},
    {13, 34, 51}, {0, 0, 0}, {16, 37, 54}, {17, 38, 55}, {19, 40, 57}, {21, 42, 59}, {20, 41, 58}, {22, 46, 65}};
static const uint8_t fp40_rest[17][3] = {
    {0, 0, 0}, {0, 0, 0}, {13, 26, 40}, {13, 26, 40}, {0, 0, 0}, {13, 26, 40}, {0, 0, 0}, {14, 27, 41}, {0, 0, 0},
    {14, 27, 41}, {0, 0, 0}, {16, 29, 43}, {17, 30, 44}, {20, 33, 47}, {21, 34, 48}, {21, 34, 48}, {22, 35, 49}};

/* 10.7.3: the FPU's conversion + execution + normalization for normalized
 * operands (halves rounded up): conversion 2, 3 for an extended source from
 * memory, 6 for an integer one (FMOVE's 1.5 + 4.5); 0 for an instruction
 * the 68040 does not implement (it traps: the exception's time). */
static unsigned fp40_stage(unsigned m, unsigned fmt, int mem, unsigned fpcr) {
    unsigned b = m & 0x3Fu;
    int prec_x = ((fpcr >> 6) & 3u) == 0;
    if (m & 0x40u) {                /* FSxxx, FDxxx: single or double rounding */
        b = m & 0x3Bu;
        if (b == 0x01u) b = 0x04u;  /* FSSQRT, FDSQRT */
        prec_x = 0;
    }
    const int intg = mem && (fmt == 0 || fmt == 4 || fmt == 6);
    const unsigned conv = intg ? 6u : (mem && fmt == 2) ? 3u : 2u;
    switch (b) {
    case 0x22: case 0x28: return conv + 3u + 2u;               /* FADD, FSUB */
    case 0x23: case 0x27: return conv + 5u + 2u;               /* FMUL, FSGLMUL */
    case 0x20: case 0x24: return conv + 38u + 2u;              /* FDIV, FSGLDIV: 37.5 */
    case 0x04: return conv + 103u + 2u;                        /* FSQRT */
    case 0x38: case 0x3A: return conv + 3u + 1u;               /* FCMP, FTST */
    case 0x00: case 0x18: case 0x1A:                           /* FMOVE, FABS, FNEG */
        if (intg) return 8;                                    /* 1.5 + 4.5 + 2 */
        if (!mem) return prec_x ? 2 : 5;
        if (fmt == 1) return 3;
        if (fmt == 5) return prec_x || ((fpcr >> 6) & 3u) == 2 ? 3 : 5;
        return prec_x ? 4 : 6;
    default: return 0;
    }
}

static unsigned fp_col(unsigned fmt) {
    /* L S X P W D B P(k): integer, single, extended, packed, integer, double, integer, packed */
    static const uint8_t c[8] = {1, 2, 4, 5, 1, 3, 1, 5};
    return c[fmt & 7u];
}

/* The cost of an FPU instruction whose words are known: `op`, `ext` (the
 * second word of general and conditional forms), `n` registers moved,
 * `state` the frame of FSAVE/FRESTORE (0 null or idle, 1 unimplemented or
 * idle on the 68881/2, 2 busy), `cond` true/false for the conditional
 * forms (FBcc taken, FDBcc branching, FTRAPcc trapping). */
int m68k_timing_fpu(const m68k_cpu *cpu, uint32_t op, uint32_t ext, unsigned kind, unsigned n, int cond) {
    /* Without an FPU (the 68LC040) the instruction is the exception's time. */
    if (cpu->tm->unit || cpu->model == M68K_68LC040 || cpu->fpu.type == M68K_FPU_NONE) return 0;
    const unsigned mode = (op >> 3) & 7u, reg = op & 7u;
    const tea e = ea_brief(mode, reg);
    const int e40 = cpu->model >= M68K_68040;
    const int is82 = cpu->fpu.type == M68K_FPU_68882;
    const unsigned fpea_t = fpea[ix23(&e, 0)];
    switch (kind) {
    case M68K_FPT_GENERAL: {
        const unsigned opclass = ext >> 13, rx = (ext >> 10) & 7u, m = ext & 0x7Fu;
        if (opclass == 2 && rx == 7) return e40 && !cpu->fpu.full ? 0 : (is82 || e40) ? 32 : 29;   /* FMOVECR (the 68040 traps it) */
        const int mem = opclass == 2;
        int as82 = is82;
        if (e40) {
            const unsigned in = mem ? fp40_in[r40(&e)][rx == 4 || rx == 6 ? 0 : rx == 0 ? 1 : rx == 1 ? 2 : rx == 5 ? 3 : 4] : 3u;
            const unsigned st = fp40_stage(m, rx, mem, cpu->fpu.fpcr);
            if (st) return (int)(in + st);
            /* Not the 68040's own: it traps (the exception's time), or,
             * with emulation.fpu_full, it runs in the 68882's time. */
            if (!cpu->fpu.full) return 0;
            as82 = 1;
        }
        const fpop *p = NULL;
        unsigned mm = m;
        if ((mm & 0x78u) == 0x30u) mm = 0x30;
        for (size_t i = 0; i < sizeof fp_ops / sizeof fp_ops[0]; i++)
            if (fp_ops[i].opmode == mm) p = &fp_ops[i];
        if (!p) return 0;
        const unsigned col = mem ? fp_col(rx) : 0u;
        unsigned t = as82 ? p->t82[col] : p->t81[col];
        if (mem && mode == 0) t -= 5;           /* an MPU data register source (note **) */
        return (int)(t + (mem ? fpea_t : 0u));
    }
    case M68K_FPT_OUT: {                        /* FMOVE FPn,<ea> */
        const unsigned rx = (ext >> 10) & 7u;
        if (e40) {
            const unsigned k = rx == 2 ? 2u : (rx == 1 || rx == 5) ? 1u : 0u;
            return (int)(fp40_out[r40(&e)][k] + (k == 2 ? 4u : k == 1 ? 3u : 8u));
        }
        static const uint16_t o81[6] = {0, 100, 80, 86, 72, 2002}, o82[6] = {0, 110, 38, 44, 50, 2006};
        unsigned t = (is82 ? o82 : o81)[fp_col(rx)];
        if (rx == 7) t += 14;                   /* a dynamic k-factor */
        if (mode == 0) t -= 2;                  /* to an MPU data register */
        return (int)(t + fpea_t);
    }
    case M68K_FPT_CTRL: {                       /* FMOVE/FMOVEM of control registers */
        const unsigned dr = (ext >> 13) & 1u;   /* 1: to memory */
        if (e40) {
            const unsigned row = r40(&e);
            if (n <= 1) return fp40_ctl[row][0] + 2;
            return (int)(fp40_ctl[row][1] + 3u * (n - 1u) + 2u + 2u * n);
        }
        unsigned t;
        if (n <= 1) t = mode <= 1 ? (dr ? 31u : 28u) : (mode == 7 && reg == 4) ? 30u : 33u;
        else t = ((mode == 7 && reg == 4) ? 25u : 27u) + 6u * n;
        return (int)(t + (is82 ? 2u : 0u) + (mode <= 1 ? 0u : fpea_t));
    }
    case M68K_FPT_DATA: {                       /* FMOVEM data registers */
        const unsigned dr = (ext >> 13) & 1u, dyn = (ext >> 11) & 1u;
        if (e40) return (int)(fp40_ctl[r40(&e)][1] + 3u * (n ? n - 1u : 0u) + dyn + 2u + 3u * n);
        unsigned t = dr ? (dyn ? 51u : 37u) + 25u * n : (dyn ? 49u : 35u) + 31u * n;
        return (int)(t + (is82 ? 2u : 0u) + fpea_t);
    }
    case M68K_FPT_BCC:                          /* FBcc, FNOP */
        if (e40) return cond ? 7 : 6;
        return cond ? 20 : 18;
    case M68K_FPT_DBCC:                         /* cond: the branch taken */
        if (e40) return n ? 9 : 11;             /* n: the condition true */
        return n ? 20 : cond ? 20 : 24;
    case M68K_FPT_SCC:
        if (e40) return fp40_ctl[r40(&e)][2];
        return (mode == 0 ? 18 : (mode == 3 || mode == 4) ? (cond ? 22 : 21) : 20) + (mode == 0 ? 0 : fpea_t);
    case M68K_FPT_TRAPCC:
        if (e40) return cond ? 0 : 6;           /* trapped: the exception's time */
        if (cond) return (reg == 2 ? 41 : reg == 3 ? 43 : 39) - (int)cpu->tm->x_trap;   /* the trap adds x_trap */
        return reg == 2 ? 20 : reg == 3 ? 22 : 18;
    case M68K_FPT_SAVE: case M68K_FPT_RESTORE: {
        const int save = kind == M68K_FPT_SAVE;
        if (e40) return (save ? fp40_save : fp40_rest)[r40(&e)][n > 2 ? 2 : n];
        static const uint16_t s81[3] = {16, 52, 286}, r81[3] = {21, 57, 291}, s82[3] = {16, 100, 334}, r82[3] = {21, 105, 339};
        const uint16_t *t = save ? (is82 ? s82 : s81) : (is82 ? r82 : r81);
        return (int)(t[n > 2 ? 2 : n] + (*(cpu->model == M68K_68030 ? &cea30 : &cea20))[ix23(&e, 0)]);
    }
    default: return 0;
    }
}

/* --- the MMU instructions (68030 and the MC68851) ---------------------------------------------- */

/* MC68030 UM 11.7.2 (the MC68851 beside a 68020 takes the same; Livadix has
 * no MC68851 timing tables). */
int m68k_timing_pmmu(const m68k_cpu *cpu, uint32_t op, unsigned kind) {
    if (cpu->tm->unit) return 0;
    const tea e = ea_brief((op >> 3) & 7u, op & 7u);
    const unsigned ea = mmu30[ix23(&e, 0)];
    switch (kind) {
    case M68K_PMT_TO_RP: return 12 + (int)ea;
    case M68K_PMT_FROM_RP: return 4 + (int)ea;
    case M68K_PMT_TO_TT: return 12 + (int)ea;
    case M68K_PMT_FROM_TT: return 8 + (int)ea;
    case M68K_PMT_TO_SR: return 6 + (int)ea;
    case M68K_PMT_FROM_SR: case M68K_PMT_FROM_TC: return 4 + (int)ea;
    case M68K_PMT_TO_TC: return 38 + (int)ea;
    case M68K_PMT_FLUSHA: return 12;
    case M68K_PMT_FLUSH: return 16 + (int)ea;
    case M68K_PMT_LOAD: return 8 + (int)ea;
    case M68K_PMT_TEST: return 22 + (int)ea;
    case M68K_PMT_TEST_WALK: return 88 + (int)ea;      /* the maximum, a six-level search */
    default: return 0;
    }
}

/* The timing kind of a 68030/MC68851 general MMU instruction from its
 * second word (PMOVE, PFLUSH, PLOAD, PTEST). */
unsigned m68k_pmmu_kind(uint32_t ext) {
    const unsigned to_mem = (ext >> 9) & 1u;
    switch (ext >> 13) {
    case 0: return to_mem ? M68K_PMT_FROM_TT : M68K_PMT_TO_TT;
    case 2: {
        const unsigned preg = (ext >> 10) & 7u;
        if (preg == 0) return to_mem ? M68K_PMT_FROM_TC : M68K_PMT_TO_TC;
        return to_mem ? M68K_PMT_FROM_RP : M68K_PMT_TO_RP;
    }
    case 3: return to_mem ? M68K_PMT_FROM_SR : M68K_PMT_TO_SR;
    case 1: {
        const unsigned fmode = (ext >> 10) & 7u;
        return fmode == 1 ? M68K_PMT_FLUSHA : fmode == 0 ? M68K_PMT_LOAD : M68K_PMT_FLUSH;
    }
    case 4: return ((ext >> 10) & 7u) ? M68K_PMT_TEST_WALK : M68K_PMT_TEST;
    default: return M68K_PMT_NONE;
    }
}
