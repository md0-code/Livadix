/* The decode step: per model, a 65,536-entry table from opcode word to
 * handler, generated from the encoding patterns of m68k_ops.c.
 *
 * Source: M68000 Family Programmer's Reference Manual, section 8
 * ("Instruction Format Summary", the operation code map) and section 2.2
 * (the effective-addressing-mode categories each instruction accepts). An
 * opcode decodes to the pattern that matches it with a valid addressing mode
 * for the model; a more specialised handler for the same instruction has a
 * higher priority. Two patterns of equal priority matching one opcode are a
 * table error (m68k_table_conflicts). Opcodes nothing matches are illegal
 * instructions; line A and line F have their own patterns. */
#include "m68k_internal.h"

m68k_handler m68k_illegal_handler(void);
m68k_handler m68k_specialise(uint32_t op, m68k_handler h);
/* The 68000's patterns and handlers (m68k_000.c). */
extern const m68k_pattern m68k_patterns000[];
extern const unsigned m68k_pattern_count000;
m68k_handler m68k_illegal_handler000(void);

static m68k_handler tables[M68K_MODEL_COUNT][65536];
static uint16_t chosen[M68K_MODEL_COUNT][65536];   /* pattern index + 1; 0 = none */
static unsigned conflicts[M68K_MODEL_COUNT];
static int built;

static unsigned ea_class(unsigned mode, unsigned reg) {
    if (mode < 7) return 1u << mode;
    if (reg <= 4) return 1u << (7 + reg);
    return 0;
}

/* The pattern list a model decodes from: the 68000 has its own (m68k_000.c). */
static const m68k_pattern *patterns_of(unsigned model) { return model == M68K_68000 ? m68k_patterns000 : m68k_patterns; }

void m68k_tables_init(void) {
    if (built) return;
    /* Patterns by line (every pattern fixes the top four bits); [1] the 68000's. */
    static uint16_t by_line[2][16][256];
    static unsigned count[2][16];
    for (unsigned i = 0; i < m68k_pattern_count; i++) {
        const unsigned line = m68k_patterns[i].match >> 12;
        by_line[0][line][count[0][line]++] = (uint16_t)i;
    }
    for (unsigned i = 0; i < m68k_pattern_count000; i++) {
        const unsigned line = m68k_patterns000[i].match >> 12;
        by_line[1][line][count[1][line]++] = (uint16_t)i;
    }
    for (unsigned model = 0; model < M68K_MODEL_COUNT; model++) {
        const unsigned bit = model == M68K_68000 ? MM_ALL : 1u << model;
        const unsigned l = model == M68K_68000;
        const m68k_pattern *const list = patterns_of(model);
        for (unsigned op = 0; op < 65536; op++) {
            const unsigned line = op >> 12;
            int best = -1, best_prio = -1;
            for (unsigned k = 0; k < count[l][line]; k++) {
                const m68k_pattern *p = &list[by_line[l][line][k]];
                if (!(p->models & bit) || (op & p->mask) != p->match) continue;
                if (p->ea && !(p->ea & ea_class((op >> 3) & 7u, op & 7u))) continue;
                if (p->ea2 && !(p->ea2 & ea_class((op >> 6) & 7u, (op >> 9) & 7u))) continue;
                if ((int)p->prio > best_prio) {
                    best = by_line[l][line][k];
                    best_prio = p->prio;
                } else if ((int)p->prio == best_prio) {
                    conflicts[model]++;
                }
            }
            chosen[model][op] = (uint16_t)(best + 1);
            if (l) tables[model][op] = best < 0 ? m68k_illegal_handler000() : list[best].fn;
            else tables[model][op] = best < 0 ? m68k_illegal_handler() : m68k_specialise(op, list[best].fn);
        }
    }
    built = 1;
}

const m68k_handler *m68k_table_for(m68k_model model) {
    m68k_tables_init();
    return tables[model];
}

m68k_opinfo m68k_decode(m68k_model model, uint16_t op) {
    m68k_opinfo info = {"illegal", NULL, 0};
    m68k_tables_init();
    if (model >= M68K_MODEL_COUNT) return info;
    info.handler = tables[model][op];
    const unsigned c = chosen[model][op];
    if (c) {
        info.name = patterns_of(model)[c - 1].name;
        info.valid = strcmp(info.name, "line A") != 0 && strcmp(info.name, "line F") != 0;
    }
    return info;
}

unsigned m68k_table_conflicts(m68k_model model) {
    m68k_tables_init();
    return model < M68K_MODEL_COUNT ? conflicts[model] : 0;
}
