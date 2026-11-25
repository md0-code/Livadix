#ifndef MC_PPC_TEST_SUPPORT_H
#define MC_PPC_TEST_SUPPORT_H
#include "ppc.h"
#include <stdio.h>
#include <string.h>

static unsigned assertions, failures;
#define CHECK(x) do { ++assertions; if (!(x)) { ++failures; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
typedef struct test_memory {
    uint8_t bytes[262144];
    unsigned reads, writes;
    bool fail_read, fail_write;
} test_memory;
static inline bool test_read(void *opaque, uint32_t address, unsigned size, uint32_t *value)
{
    test_memory *m = opaque;
    ++m->reads;
    if (m->fail_read || (uint64_t)address + size > sizeof(m->bytes)) return false;
    *value = 0;
    for (unsigned i = 0; i < size; ++i) *value = (*value << 8) | m->bytes[address + i];
    return true;
}
static inline bool test_write(void *opaque, uint32_t address, unsigned size, uint32_t value)
{
    test_memory *m = opaque;
    ++m->writes;
    if (m->fail_write || (uint64_t)address + size > sizeof(m->bytes)) return false;
    for (unsigned i = 0; i < size; ++i) m->bytes[address + i] = (uint8_t)(value >> (8 * (size - i - 1)));
    return true;
}
static inline void test_setup(ppc_cpu *c, test_memory *m)
{
    ppc_bus bus = {m, test_read, test_write};
    memset(m, 0, sizeof(*m));
    ppc_init(c, bus);
    c->msr = PPC_MSR_ME | PPC_MSR_FP;
    c->pc = c->last_pc = 0x1000;
}
static inline ppc_result test_step(ppc_cpu *c, test_memory *m, uint32_t op)
{
    c->pc = 0x1000;
    test_write(m, c->pc, 4, op);
    m->reads = m->writes = 0;
    return ppc_step(c);
}
static inline uint32_t test_x(unsigned xo, unsigned d, unsigned a, unsigned b, bool rc)
{
    return (31u << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1) | (rc ? 1u : 0u);
}
static inline int test_report(const char *name)
{
    printf("%s: %u assertions, %u failures\n", name, assertions, failures);
    return failures ? 1 : 0;
}
#endif
