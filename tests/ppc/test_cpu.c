#include "ppc.h"
#include <stdio.h>
#include <string.h>

/* Instruction words and expected effects: MPC601UM chapter 10;
   exception state: chapter 5; reset/RTC/DEC: chapter 2. */
static unsigned assertions, failures;
#define CHECK(x) do { ++assertions; if (!(x)) { ++failures; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
typedef struct memory_image { uint8_t data[8192]; bool fail; } memory_image;
static bool rd(void *opaque, uint32_t ea, unsigned size, uint32_t *v)
{
    memory_image *m = opaque;
    if (m->fail || (uint64_t)ea + size > sizeof(m->data)) return false;
    *v = 0;
    for (unsigned i = 0; i < size; ++i) *v = (*v << 8) | m->data[ea + i];
    return true;
}
static bool wr(void *opaque, uint32_t ea, unsigned size, uint32_t v)
{
    memory_image *m = opaque;
    if (m->fail || (uint64_t)ea + size > sizeof(m->data)) return false;
    for (unsigned i = 0; i < size; ++i) m->data[ea + i] = (uint8_t)(v >> (8u * (size - i - 1u)));
    return true;
}
static uint32_t xop(unsigned xo, unsigned d, unsigned a, unsigned b, bool oe, bool rc)
{
    return (31u << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1) | (oe ? 0x400u : 0u) | (rc ? 1u : 0u);
}
static uint32_t dop(unsigned op, unsigned d, unsigned a, uint32_t imm)
{
    return (op << 26) | (d << 21) | (a << 16) | (imm & 65535u);
}
static uint32_t spr_op(unsigned xo, unsigned d, unsigned spr)
{
    return xop(xo, d, spr & 31u, spr >> 5, false, false);
}
static ppc_result step(ppc_cpu *c, memory_image *m, uint32_t op)
{
    c->pc = 0x1000; c->msr &= ~PPC_MSR_IR; m->fail = false;
    wr(m, c->pc, 4, op);
    return ppc_step(c);
}
static void integer(ppc_cpu *c, memory_image *m)
{
    c->gpr[0] = 0x12345678;
    CHECK(step(c,m,dop(14,3,0,0xffff)) == PPC_OK); CHECK(c->gpr[3] == UINT32_MAX);
    CHECK(step(c,m,dop(15,4,0,0x8000)) == PPC_OK); CHECK(c->gpr[4] == 0x80000000u);
    c->gpr[1] = 0x7fffffffu; c->gpr[2] = 1;
    CHECK(step(c,m,xop(266,3,1,2,true,true)) == PPC_OK);
    CHECK(c->gpr[3] == 0x80000000u); CHECK((c->xer & (PPC_XER_OV|PPC_XER_SO)) == (PPC_XER_OV|PPC_XER_SO)); CHECK((c->cr >> 28) == 9u);
    c->gpr[1] = 1;
    CHECK(step(c,m,xop(266,3,1,2,true,true)) == PPC_OK); CHECK(c->gpr[3] == 2); CHECK(!(c->xer & PPC_XER_OV)); CHECK(c->xer & PPC_XER_SO);
    c->xer = 0; c->gpr[1] = UINT32_MAX;
    CHECK(step(c,m,xop(10,3,1,2,false,true)) == PPC_OK); CHECK(c->gpr[3] == 0); CHECK(c->xer & PPC_XER_CA); CHECK(c->cr >> 28 == 2);
    c->gpr[1] = 0; c->gpr[2] = 0; c->xer = PPC_XER_CA;
    CHECK(step(c,m,xop(136,3,1,2,false,false)) == PPC_OK); CHECK(c->gpr[3] == 0); CHECK(c->xer & PPC_XER_CA);
    c->xer = 0;
    CHECK(step(c,m,xop(136,3,1,2,false,false)) == PPC_OK); CHECK(c->gpr[3] == UINT32_MAX); CHECK(!(c->xer & PPC_XER_CA));
    c->gpr[1] = 0x80000000u;
    CHECK(step(c,m,xop(104,3,1,0,true,true)) == PPC_OK); CHECK(c->gpr[3] == 0x80000000u); CHECK(c->xer & PPC_XER_OV);
    c->gpr[1] = 0x80000000u; c->gpr[2] = UINT32_MAX;
    CHECK(step(c,m,xop(491,3,1,2,true,false)) == PPC_OK); CHECK(c->xer & PPC_XER_OV);
    c->gpr[1] = 0xfffffff9u; c->gpr[2] = 3;
    CHECK(step(c,m,xop(491,3,1,2,true,false)) == PPC_OK); CHECK(c->gpr[3] == 0xfffffffeu); CHECK(!(c->xer & PPC_XER_OV));
    c->gpr[1] = UINT32_MAX; c->gpr[2] = 2;
    CHECK(step(c,m,xop(75,3,1,2,false,false)) == PPC_OK); CHECK(c->gpr[3] == UINT32_MAX);
    CHECK(step(c,m,xop(11,3,1,2,false,false)) == PPC_OK); CHECK(c->gpr[3] == 1);
    c->gpr[3] = 0x80000001u; c->gpr[2] = 0;
    CHECK(step(c,m,xop(792,3,1,2,false,true)) == PPC_OK); CHECK(c->gpr[1] == 0x80000001u); CHECK(!(c->xer & PPC_XER_CA));
    c->gpr[2] = 32;
    CHECK(step(c,m,xop(792,3,1,2,false,true)) == PPC_OK); CHECK(c->gpr[1] == UINT32_MAX); CHECK(c->xer & PPC_XER_CA);
    CHECK(step(c,m,xop(24,3,1,2,false,false)) == PPC_OK); CHECK(c->gpr[1] == 0);
    c->gpr[2] = 64;
    CHECK(step(c,m,xop(24,3,1,2,false,false)) == PPC_OK); CHECK(c->gpr[1] == c->gpr[3]);
    c->gpr[3] = 0;
    CHECK(step(c,m,xop(26,3,1,0,false,true)) == PPC_OK); CHECK(c->gpr[1] == 32);
    c->gpr[3] = 0x80000000u;
    CHECK(step(c,m,xop(26,3,1,0,false,false)) == PPC_OK); CHECK(c->gpr[1] == 0);
    c->gpr[1] = UINT32_MAX; c->xer = 0;
    CHECK(step(c,m,dop(8,3,1,0xffff)) == PPC_OK); CHECK(c->gpr[3] == 0); CHECK(c->xer & PPC_XER_CA);
    c->gpr[1] = 1;
    CHECK(step(c,m,dop(12,3,1,0xffff)) == PPC_OK); CHECK(c->gpr[3] == 0); CHECK(c->xer & PPC_XER_CA);
    c->cr = 0; c->xer = PPC_XER_SO; c->gpr[1] = 0x80000000u;
    CHECK(step(c,m,dop(11,8,1,0)) == PPC_OK); CHECK(((c->cr >> 20) & 15u) == 9u);
    CHECK(step(c,m,dop(10,8,1,0)) == PPC_OK); CHECK(((c->cr >> 20) & 15u) == 5u);
    c->gpr[3] = 0x12345678u;
    CHECK(step(c,m,(21u<<26)|(3u<<21)|(1u<<16)|(8u<<11)|(8u<<6)|(15u<<1)) == PPC_OK); CHECK(c->gpr[1] == 0x00560000u);
}
static void branches(ppc_cpu *c, memory_image *m)
{
    c->msr = PPC_MSR_ME; c->cr = 0; c->ctr = 2;
    CHECK(step(c,m,(16u<<26)|(16u<<21)|8u|1u) == PPC_OK); CHECK(c->pc == 0x1008); CHECK(c->ctr == 1); CHECK(c->lr == 0x1004);
    CHECK(step(c,m,(16u<<26)|(16u<<21)|8u) == PPC_OK); CHECK(c->pc == 0x1004); CHECK(c->ctr == 0);
    CHECK(step(c,m,(18u<<26)|0x03fffffcu) == PPC_OK); CHECK(c->pc == 0xffc);
    c->lr = 0x1237;
    CHECK(step(c,m,(19u<<26)|(20u<<21)|(16u<<1)|1u) == PPC_OK); CHECK(c->pc == 0x1234); CHECK(c->lr == 0x1004);
    c->ctr = 0x2347;
    CHECK(step(c,m,(19u<<26)|(20u<<21)|(528u<<1)) == PPC_OK); CHECK(c->pc == 0x2344); CHECK(c->ctr == 0x2347);
    c->cr = 0x80000000u;
    CHECK(step(c,m,(19u<<26)|(2u<<21)|(0u<<16)|(1u<<11)|(193u<<1)) == PPC_OK); CHECK(c->cr == 0xa0000000u);
    c->xer = PPC_XER_SO | PPC_XER_OV | PPC_XER_CA | 7u;
    CHECK(step(c,m,xop(512,12,0,0,false,false)) == PPC_OK); CHECK(((c->cr >> 16) & 15u) == 14u); CHECK(c->xer == 7);
}
static void exceptions(ppc_cpu *c, memory_image *m)
{
    c->msr = PPC_MSR_ME | PPC_MSR_IP | PPC_MSR_PR | PPC_MSR_FP | PPC_MSR_DR;
    CHECK(step(c,m,xop(83,3,0,0,false,false)) == PPC_EXCEPTION); CHECK(c->srr0 == 0x1000); CHECK(c->srr1 == (0x7050u | 0x40000u)); CHECK(c->pc == 0xfff00700u); CHECK(c->msr == 0x1040u);
    c->msr = PPC_MSR_ME | PPC_MSR_IP;
    CHECK(step(c,m,(17u<<26)|2u) == PPC_EXCEPTION); CHECK(c->srr0 == 0x1004); CHECK(c->pc == 0xfff00c00u);
    c->srr0 = 0x2003; c->srr1 = PPC_MSR_ME | PPC_MSR_PR;
    CHECK(step(c,m,(19u<<26)|(50u<<1)) == PPC_OK); CHECK(c->pc == 0x2000); CHECK(c->msr == (PPC_MSR_ME|PPC_MSR_PR));
    c->msr = PPC_MSR_ME;
    CHECK(step(c,m,(63u<<26)|(1u<<21)|(72u<<1)) == PPC_EXCEPTION); CHECK(c->last_exception == PPC_EX_FPU);
    c->msr = PPC_MSR_ME | PPC_MSR_SE;
    CHECK(step(c,m,dop(14,3,0,1)) == PPC_EXCEPTION); CHECK(c->last_exception == PPC_EX_RUN_MODE); CHECK(c->srr0 == 0x1004); CHECK(c->gpr[3] == 1);
    c->msr = PPC_MSR_ME; c->gpr[1] = 5;
    CHECK(step(c,m,dop(3,4,1,5)) == PPC_EXCEPTION); CHECK(c->srr1 & 0x20000u);
    c->msr = PPC_MSR_ME;
    CHECK(step(c,m,xop(371,3,12,8,false,false)) == PPC_EXCEPTION); CHECK(c->srr1 & 0x80000u);
    CHECK(step(c,m,xop(370,0,0,0,false,false)) == PPC_EXCEPTION); CHECK(c->srr1 & 0x80000u);
    c->msr = PPC_MSR_ME; c->gpr[3] = 0xaabbccddu;
    CHECK(step(c,m,spr_op(339,3,100)) == PPC_OK); CHECK(c->gpr[3] == 0xaabbccddu);
    c->msr = PPC_MSR_ME | PPC_MSR_PR;
    CHECK(step(c,m,spr_op(339,3,511)) == PPC_EXCEPTION); CHECK(c->srr1 & 0x40000u);
    c->msr = PPC_MSR_ME | PPC_MSR_EE; c->external_irq = true;
    CHECK(step(c,m,dop(14,3,0,7)) == PPC_EXCEPTION); CHECK(c->last_exception == PPC_EX_EXTERNAL); CHECK(c->srr0 == 0x1000); CHECK(c->gpr[3] == 0xaabbccddu); c->external_irq = false;
    c->msr = 0; c->pc = 0xf0000000u; m->fail = true;
    CHECK(ppc_step(c) == PPC_CHECKSTOP); m->fail = false;
}
static void timer(ppc_cpu *c, memory_image *m)
{
    ppc_reset(c);
    CHECK(c->msr == 0x1040); CHECK(c->pc == 0xfff00100u); CHECK(c->pvr == 0x10001); CHECK(c->hid[0] == 0x80010080u);
    c->dec = 0x80000000u;
    ppc_clock(c, 1); CHECK(c->dec == 0x7fffff80u); CHECK(!c->decrementer_pending); CHECK(c->rtcl == 128);
    c->dec = 0; c->rtcl = 999999872; c->rtcu = 3;
    ppc_clock(c, 1); CHECK(c->dec == 0xffffff80u); CHECK(c->decrementer_pending); CHECK(c->rtcl == 0); CHECK(c->rtcu == 4);
    c->msr = PPC_MSR_ME | PPC_MSR_EE;
    CHECK(step(c,m,dop(14,3,0,1)) == PPC_EXCEPTION); CHECK(c->last_exception == PPC_EX_DECREMENTER); CHECK(!c->decrementer_pending);
    c->msr = PPC_MSR_ME | PPC_MSR_PR;
    CHECK(step(c,m,spr_op(339,3,4)) == PPC_OK); CHECK(c->gpr[3] == 4);
    c->msr = PPC_MSR_ME; c->gpr[3] = UINT32_MAX;
    CHECK(step(c,m,spr_op(467,3,21)) == PPC_OK); CHECK(c->rtcl == 0x3fffff80u);
    c->dec = 0; c->decrementer_pending = false;
    CHECK(step(c,m,spr_op(467,3,22)) == PPC_OK); CHECK(c->dec == 0xffffff80u); CHECK(c->decrementer_pending);
}
static void loads(ppc_cpu *c, memory_image *m)
{
    ppc_reset(c); c->msr = PPC_MSR_ME; c->gpr[1] = 0x201;
    wr(m,0x201,4,0x89abcdefu);
    CHECK(step(c,m,dop(32,3,1,0)) == PPC_OK); CHECK(c->gpr[3] == 0x89abcdefu);
    c->gpr[3] = 0xaabbccddu;
    CHECK(step(c,m,dop(37,3,1,4)) == PPC_OK); CHECK(c->gpr[1] == 0x205);
    uint32_t v; rd(m,0x205,4,&v); CHECK(v == 0xaabbccddu);
    CHECK(step(c,m,dop(33,1,1,0)) == PPC_EXCEPTION); CHECK(c->srr1 & 0x80000u);
    c->msr = PPC_MSR_ME; c->gpr[1] = 0x200; c->gpr[2] = 1;
    CHECK(step(c,m,xop(534,3,1,2,false,false)) == PPC_OK); CHECK(c->gpr[3] == 0xefcdab89u);
    CHECK(step(c,m,dop(42,3,1,1)) == PPC_OK); CHECK(c->gpr[3] == 0xffff89abu);
    c->gpr[1] = 0x200; c->gpr[2] = 0;
    wr(m,0x200,4,0x12345678u);
    CHECK(step(c,m,xop(20,3,1,2,false,false)) == PPC_OK); CHECK(c->reserved); CHECK(c->gpr[3] == 0x12345678u);
    c->gpr[4] = 0xfeedfaceu;
    CHECK(step(c,m,xop(150,4,1,2,false,true)) == PPC_OK); CHECK(!c->reserved); CHECK((c->cr >> 28) == 2u);
    rd(m,0x200,4,&v); CHECK(v == 0xfeedfaceu);
    CHECK(step(c,m,xop(150,3,1,2,false,true)) == PPC_OK); CHECK((c->cr >> 28) == 0);
    c->gpr[1] = 0x220; memset(m->data+0x220,0x55,32);
    CHECK(step(c,m,xop(1014,0,1,2,false,false)) == PPC_OK);
    for (unsigned i=0;i<32;++i) CHECK(m->data[0x220+i] == 0);
    c->gpr[1] = 0x240; wr(m,0x240,4,0x11223344u); wr(m,0x244,4,0x55667788u);
    CHECK(step(c,m,xop(597,30,1,6,false,false)) == PPC_OK); CHECK(c->gpr[30] == 0x11223344u); CHECK(c->gpr[31] == 0x55660000u);
}
int main(void)
{
    memory_image m; ppc_cpu c;
    memset(&m,0,sizeof(m)); ppc_init(&c,(ppc_bus){&m,rd,wr});
    integer(&c,&m); branches(&c,&m); exceptions(&c,&m); timer(&c,&m); loads(&c,&m);
    printf("601 CPU: %u assertions, %u failures\n",assertions,failures);
    return failures ? 1 : 0;
}
