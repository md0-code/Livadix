/* mc_cpu_tests faults: the restartable-fault sweep.
 *
 * Every opcode word the 68040 accepts is run once from a random state in
 * user mode with every access through the bus, counting its reads (the
 * instruction fetches included) and writes. Then each access is refused in
 * turn:
 *   - a refused read must leave the registers as they were before the
 *     instruction and stack a format $7 frame with the instruction's address;
 *     the handler's RTE must then re-run it to the same result as the
 *     undisturbed run;
 *   - a refused write either restarts the same way (MOVEM, MOVE16, MOVEP),
 *     or completes the instruction and stacks the next PC with the pending
 *     writes in WB2/WB3; performing those writebacks must give the same
 *     memory as the undisturbed run.
 * The supervisor stack is far from the data the instructions reach, as an
 * operating system's is.
 *
 * A second pass turns translation on (identity-mapped 4 KiB page tables)
 * and, instead of the bus, the MMU refuses: every page each instruction
 * touches is made invalid in turn, and every page it writes is made write
 * protected in turn. The fault must carry the SSW's ATC bit; the "handler"
 * repairs the descriptor and flushes the ATC, then restarts the instruction
 * or completes its writebacks (WB2/WB3), with the same outcome checks. */
#include "cpu_test.h"
#include "../test.h"

#define CODE 0x1000u
#define HANDLERS 0xE0000u
#define ISP 0xF8000u
#define USP 0x30000u
#define RAM (1u << 20)

typedef struct snap {
    uint32_t r[16], pc, sr;
} snap;

typedef struct sweep {
    testbus bus;
    m68k_cpu *cpu;
    uint8_t *pristine, *expect;
    unsigned vectors, vector;
    uint64_t rng;
} sweep;

static uint32_t next(sweep *s) {
    s->rng = s->rng * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(s->rng >> 32);
}

static void hook(void *ctx, m68k_cpu *cpu, unsigned vector, uint32_t pc) {
    sweep *s = ctx;
    (void)cpu;
    (void)pc;
    s->vectors++;
    s->vector = vector;
}

/* RAM back to its pristine bytes, then the state `st` with `code`. */
static void restore(sweep *s, const snap *st, const uint16_t *code) {
    for (size_t i = 0; i < s->bus.ndirty; i++) s->bus.ram[s->bus.dirty[i]] = s->pristine[s->bus.dirty[i]];
    s->bus.ndirty = 0;
    for (unsigned i = 0; i < 7; i++) tb_put(&s->bus, CODE + 2 * i, 2, code[i]);
    m68k_cpu *c = s->cpu;
    c->events = 0;
    c->status = M68K_OK;
    c->wb_n = 0;
    c->ipl = 0;
    m68k_set_sr(c, 0x2700);
    c->r[15] = ISP;
    c->isp = ISP;
    c->usp = st->r[15];
    m68k_set_sr(c, (uint16_t)st->sr);
    for (unsigned i = 0; i < 16; i++) c->r[i] = st->r[i];
    c->pc = st->pc;
    s->bus.reads = s->bus.writes = 0;
    s->bus.fail_read_at = s->bus.fail_write_at = -1;
    s->vectors = 0;
}

static void take(m68k_cpu *c, snap *st) {
    for (unsigned i = 0; i < 16; i++) st->r[i] = c->r[i];
    st->pc = c->pc;
    st->sr = m68k_sr(c);
}

/* Registers D0-A6 and the user stack pointer as `want` has them. */
static int same_regs(m68k_cpu *c, const snap *want) {
    for (unsigned i = 0; i < 15; i++)
        if (c->r[i] != want->r[i]) return 0;
    return m68k_get_usp(c) == want->r[15];
}

/* The translation tables of the MMU pass (and their page, supervisor only). */
#define TABLES 0xC0000u
#define PTABLES (TABLES + 0x400u)

static int excluded(uint32_t a) {
    return (a >= ISP - 0x1000 && a < ISP) || (a >= TABLES && a < TABLES + 0x1000);
}

/* Memory as the undisturbed run left it, outside the supervisor stack and
 * the translation tables. */
static int same_memory(sweep *s, const testbus *ref) {
    for (size_t i = 0; i < ref->ndirty; i++) {
        const uint32_t a = ref->dirty[i];
        if (excluded(a)) continue;
        if (s->bus.ram[a] != s->expect[a]) return 0;
    }
    for (size_t i = 0; i < s->bus.ndirty; i++) {
        const uint32_t a = s->bus.dirty[i];
        if (excluded(a)) continue;
        if (s->bus.ram[a] != s->expect[a]) return 0;
    }
    return 1;
}

/* Performs the writebacks of the format $7 frame at sp in the order the
 * handler must (WB1, WB2, WB3; MC68040 UM 8.4.6.7). WB1D is memory aligned
 * (Table 8-5), the others register aligned. */
static void writebacks(sweep *s, uint32_t sp) {
    static const uint8_t st_at[3] = {0x12, 0x10, 0x0E}, a_at[3] = {0x28, 0x20, 0x18};
    for (unsigned slot = 0; slot < 3; slot++) {
        const uint32_t status = tb_get(&s->bus, sp + st_at[slot], 2);
        if (!(status & 0x80u)) continue;
        const uint32_t a = tb_get(&s->bus, sp + a_at[slot], 4);
        uint32_t d = tb_get(&s->bus, sp + a_at[slot] + 4, 4);
        const unsigned size = ((status >> 5) & 3u) == 1 ? 1u : ((status >> 5) & 3u) == 2 ? 2u : 4u;
        if (slot == 0) {
            const unsigned rot = 8u * (a & 3u);
            if (rot) d = (d << rot) | (d >> (32u - rot));
            d >>= 32u - 8u * size;
        }
        tb_write(&s->bus, a, size, 5, d);
    }
}

static void report(unsigned op, const char *what, unsigned k) {
    static unsigned shown;
    if (shown++ < 30) fprintf(stderr, "fault sweep: %04X (%s): %s %u\n", op, m68k_decode(M68K_68040, (uint16_t)op).name, what, k);
    mc_test_failures++;
}

/* A random case of opcode `op` (as the bus pass makes them). */
static void make_case(sweep *s, unsigned op, uint16_t *code, snap *st0) {
    code[0] = (uint16_t)op;
    for (unsigned i = 1; i < 7; i++) {
        const uint32_t r = next(s);
        code[i] = (uint16_t)((r & 3u) == 0 ? r >> 16 : (r >> 16) & 0xFFu);
    }
    for (unsigned i = 0; i < 8; i++) st0->r[i] = (next(s) & 1u) ? next(s) : next(s) & 0x3Fu;
    for (unsigned i = 8; i < 15; i++) st0->r[i] = 0x10000u + (next(s) % 0x20000u & ~1u);
    st0->r[15] = USP;
    st0->pc = CODE;
    st0->sr = next(s) & 0x1Fu;
}

static uint32_t fresh_pd(uint32_t page) { return page << 12 | 1u | (page == TABLES >> 12 ? 0x80u : 0u); }

/* The MMU pass (see the top of the file). */
static void mmu_pass(sweep *s, testbus *ref) {
    m68k_cpu *c = s->cpu;
    /* Root and pointer tables: one root entry, four pointer entries, then
     * four page tables of 64 descriptors covering the 1 MiB of RAM. */
    for (uint32_t a = TABLES; a < TABLES + 0x400u; a += 4) tb_put(&s->bus, a, 4, 0);
    tb_put(&s->bus, TABLES, 4, (TABLES + 0x200u) | 3u);
    for (uint32_t i = 0; i < 4; i++) tb_put(&s->bus, TABLES + 0x200u + 4u * i, 4, (PTABLES + 256u * i) | 3u);
    for (uint32_t p = 0; p < 256; p++) tb_put(&s->bus, PTABLES + 4u * p, 4, fresh_pd(p));
    memcpy(s->pristine + TABLES, s->bus.ram + TABLES, 0x1000);
    memcpy(s->expect + TABLES, s->bus.ram + TABLES, 0x1000);
    m68k_movec_write(c, 0x806, TABLES);
    m68k_movec_write(c, 0x807, TABLES);
    m68k_movec_write(c, 0x003, 0x8000);
    s->rng = 0x68041;
    unsigned long opcodes = 0, skipped = 0, restarts = 0, completed = 0;
    for (unsigned op = 0; op < 65536; op++) {
        if (!m68k_decode(M68K_68040, (uint16_t)op).valid) continue;
        snap st0, st1;
        uint16_t code[7];
        make_case(s, op, code, &st0);
        restore(s, &st0, code);
        m68k_pflusha(c);
        s->bus.nalog = 0;
        s->bus.logging = 1;
        m68k_run(c, 1);
        s->bus.logging = 0;
        if (s->vectors || c->status != M68K_OK) {
            skipped++;
            continue;
        }
        int clobbers = 0;
        for (size_t i = 0; i < s->bus.ndirty; i++) {
            const uint32_t a = s->bus.dirty[i];
            clobbers |= a < 0x400u || (a >= HANDLERS && a < HANDLERS + 0x1000u) || (a >= ISP - 0x1000u && a < ISP);
        }
        if (clobbers) {
            skipped++;
            continue;
        }
        opcodes++;
        take(c, &st1);
        ref->ndirty = 0;
        for (size_t i = 0; i < s->bus.ndirty; i++) {
            const uint32_t a = s->bus.dirty[i];
            s->expect[a] = s->bus.ram[a];
            if (ref->ndirty == ref->cap) {
                ref->cap = ref->cap ? ref->cap * 2 : 256;
                ref->dirty = realloc(ref->dirty, ref->cap * sizeof *ref->dirty);
            }
            ref->dirty[ref->ndirty++] = a;
        }
        /* The data pages the instruction touched, and whether it wrote them. */
        uint32_t pages[16];
        uint8_t written[16];
        unsigned npages = 0;
        for (size_t i = 0; i < s->bus.nalog; i++) {
            const uint32_t page = s->bus.alog[i] >> 12;
            /* The vector table, the handlers and the supervisor stack stay
             * mapped: losing them is a double fault, as on the chip. */
            if (page == 0 || page == TABLES >> 12 || page == CODE >> 12 || page == HANDLERS >> 12 ||
                page == (ISP - 1u) >> 12 || page >= 256)
                continue;
            unsigned k = 0;
            while (k < npages && pages[k] != page) k++;
            if (k == npages) {
                if (npages == 16) continue;
                pages[npages] = page;
                written[npages++] = 0;
            }
            written[k] |= s->bus.awrite[i];
        }
        for (unsigned k = 0; k < npages; k++) {
            for (unsigned variant = 0; variant < 2; variant++) {
                if (variant == 1 && !written[k]) continue;
                const uint32_t pda = PTABLES + 4u * pages[k];
                restore(s, &st0, code);
                m68k_pflusha(c);
                tb_put(&s->bus, pda, 4, variant ? fresh_pd(pages[k]) | 4u : 0);
                m68k_run(c, 1);
                const uint32_t sp = c->r[15];
                const char *what = variant ? "write-protected page" : "invalid page";
                if (s->vectors != 1 || s->vector != 2 || tb_get(&s->bus, sp + 6, 2) != 0x7008 ||
                    !(tb_get(&s->bus, sp + 0x0C, 2) & 0x0400u)) {
                    report(op, what, pages[k]);
                } else if (tb_get(&s->bus, sp + 0x10, 2) & 0x80u) {
                    /* Completed: the refused write in WB2 (WB1 empty). */
                    if (tb_get(&s->bus, sp + 0x12, 2) & 0x80u || tb_get(&s->bus, sp + 2, 4) != st1.pc ||
                        !same_regs(c, &st1) || tb_get(&s->bus, sp, 2) != st1.sr) {
                        report(op, "completion state wrong after an MMU write fault, page", pages[k]);
                    } else {
                        tb_put(&s->bus, pda, 4, fresh_pd(pages[k]));
                        m68k_pflusha(c);
                        writebacks(s, sp);
                        if (!same_memory(s, ref)) report(op, "MMU writebacks do not complete the instruction, page", pages[k]);
                        else completed++;
                    }
                } else if (!same_regs(c, &st0) || tb_get(&s->bus, sp, 2) != st0.sr) {
                    report(op, "registers changed before an MMU restart, page", pages[k]);
                } else {
                    tb_put(&s->bus, pda, 4, fresh_pd(pages[k]));
                    m68k_pflusha(c);
                    m68k_run(c, 2);
                    if (c->pc != st1.pc || !same_regs(c, &st1) || m68k_sr(c) != st1.sr || !same_memory(s, ref))
                        report(op, "MMU restart gave another result, page", pages[k]);
                    else restarts++;
                }
                tb_put(&s->bus, pda, 4, fresh_pd(pages[k]));
            }
        }
        for (size_t i = 0; i < ref->ndirty; i++) s->expect[ref->dirty[i]] = s->pristine[ref->dirty[i]];
    }
    m68k_movec_write(c, 0x003, 0);
    m68k_pflusha(c);
    printf("fault sweep, MMU: %lu opcode words (%lu left out); %lu MMU faults restarted, %lu completed through writebacks\n",
           opcodes, skipped, restarts, completed);
}

int mc_cpu_fault_sweep(void) {
    sweep *s = calloc(1, sizeof *s);
    tb_init(&s->bus, RAM, 0xFFFFFFFFu, 1);
    tb_unmap(&s->bus, 0, RAM);              /* every access through the bus */
    s->rng = 0x68040;
    for (uint32_t a = 0; a < RAM; a += 4) tb_put(&s->bus, a, 4, next(s));
    for (unsigned v = 0; v < 256; v++) {
        tb_put(&s->bus, 4 * v, 4, HANDLERS + 16 * v);
        tb_put(&s->bus, HANDLERS + 16 * v, 2, v == 2 ? 0x4E73 : 0x4E71);   /* access error: RTE */
    }
    s->pristine = malloc(RAM);
    s->expect = malloc(RAM);
    memcpy(s->pristine, s->bus.ram, RAM);
    memcpy(s->expect, s->bus.ram, RAM);
    m68k_bus mb = tb_bus(&s->bus);
    s->cpu = calloc(1, sizeof *s->cpu);
    m68k_init(s->cpu, M68K_68040, &mb);
    s->cpu->on_exception = hook;
    s->cpu->hook_ctx = s;
    m68k_cpu *c = s->cpu;
    testbus ref;
    memset(&ref, 0, sizeof ref);
    unsigned long opcodes = 0, read_faults = 0, write_restarts = 0, completed = 0, skipped = 0;

    for (unsigned op = 0; op < 65536; op++) {
        if (!m68k_decode(M68K_68040, (uint16_t)op).valid) continue;
        snap st0, st1;
        uint16_t code[7];
        make_case(s, op, code, &st0);      /* user mode */

        /* The undisturbed run. */
        restore(s, &st0, code);
        m68k_run(c, 1);
        if (s->vectors || c->status != M68K_OK) {
            skipped++;
            continue;
        }
        /* An instance that rewrites the vector table, the handlers or the
         * supervisor stack would change the exception it is tested with. */
        int clobbers = 0;
        for (size_t i = 0; i < s->bus.ndirty; i++) {
            const uint32_t a = s->bus.dirty[i];
            clobbers |= a < 0x400u || (a >= HANDLERS && a < HANDLERS + 0x1000u) || (a >= ISP - 0x1000u && a < ISP);
        }
        if (clobbers) {
            skipped++;
            continue;
        }
        opcodes++;
        take(c, &st1);
        const long reads = s->bus.reads, writes = s->bus.writes;
        ref.ndirty = 0;
        for (size_t i = 0; i < s->bus.ndirty; i++) {
            const uint32_t a = s->bus.dirty[i];
            s->expect[a] = s->bus.ram[a];
            if (ref.ndirty == ref.cap) {
                ref.cap = ref.cap ? ref.cap * 2 : 256;
                ref.dirty = realloc(ref.dirty, ref.cap * sizeof *ref.dirty);
            }
            ref.dirty[ref.ndirty++] = a;
        }

        for (long k = 0; k < reads + writes; k++) {
            const int is_read = k < reads;
            restore(s, &st0, code);
            if (is_read) s->bus.fail_read_at = k;
            else s->bus.fail_write_at = k - reads;
            m68k_run(c, 1);
            s->bus.fail_read_at = s->bus.fail_write_at = -1;
            const uint32_t sp = c->r[15];
            if (s->vectors != 1 || s->vector != 2 || tb_get(&s->bus, sp + 6, 2) != 0x7008) {
                char what[96];
                snprintf(what, sizeof what, "%u exception(s), last vector %u, for %s access", s->vectors, s->vector,
                         is_read ? "read" : "write");
                report(op, what, (unsigned)k);
                continue;
            }
            const uint32_t pc = tb_get(&s->bus, sp + 2, 4);
            if (!((tb_get(&s->bus, sp + 0x10, 2) | tb_get(&s->bus, sp + 0x12, 2)) & 0x80u)) {
                /* Restarted: nothing of the instruction remains. */
                if (!same_regs(c, &st0) || tb_get(&s->bus, sp, 2) != st0.sr) {
                    report(op, "registers changed before the restart, access", (unsigned)k);
                    continue;
                }
                m68k_run(c, 2);           /* RTE, then the instruction again */
                if (c->pc != st1.pc || !same_regs(c, &st1) || m68k_sr(c) != st1.sr || !same_memory(s, &ref)) {
                    report(op, "restart gave another result, access", (unsigned)k);
                    continue;
                }
                if (is_read) read_faults++;
                else write_restarts++;
            } else {
                /* Completed with pending writebacks. */
                if (is_read || pc != st1.pc || !same_regs(c, &st1) || tb_get(&s->bus, sp, 2) != st1.sr) {
                    report(op, "completion state wrong, access", (unsigned)k);
                    continue;
                }
                writebacks(s, sp);
                if (!same_memory(s, &ref)) {
                    report(op, "writebacks do not complete the instruction, access", (unsigned)k);
                    continue;
                }
                completed++;
            }
        }
        for (size_t i = 0; i < ref.ndirty; i++) s->expect[ref.dirty[i]] = s->pristine[ref.dirty[i]];
    }
    printf("fault sweep: %lu opcode words (%lu left out: an exception undisturbed, or a write to the vectors); %lu read faults restarted, "
           "%lu write faults restarted, %lu write faults completed through writebacks\n",
           opcodes, skipped, read_faults, write_restarts, completed);
    mmu_pass(s, &ref);
    free(ref.dirty);
    free(s->pristine);
    free(s->expect);
    m68k_free(s->cpu);
    free(s->cpu);
    tb_free(&s->bus);
    free(s);
    return mc_test_failures;
}
