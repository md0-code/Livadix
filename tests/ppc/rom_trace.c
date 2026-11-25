/* Bare, read-only ROM coverage harness, NOT a Macintosh hardware model.
 * Sources: MPC601UM 2.2.5.2 (reset XER), 5.3/5.4 (exception vectors,
 * reset and checkstop).
 * ROM maps only at FFC00000..FFFFFFFF; reset entry FFF00100.
 * RAM maps only at 00000000..(ram_mib * 1MiB - 1); no aliases or fake MMIO.
 * Usage: rom_trace ROM [retired-target [ram-mib [model]]], defaults 10000000, 64, 601.
 */
#include "ppc.h"
#include "ppc_model.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_BASE UINT32_C(0xffc00000)
#define ROM_SIZE (4u * 1024u * 1024u)
#define RESET_PC UINT32_C(0xfff00100)
#define EXCEPTION_SLOTS 256u
#define FAULT_SIGNATURES 128u

typedef struct bare_memory {
    unsigned char *rom, *ram;
    size_t ram_size;
    uint64_t unmapped;
    uint32_t last_unmapped;
    bool last_write;
} bare_memory;

typedef struct fault_signature {
    uint32_t pc, opcode, vector, srr0, srr1, dar;
    unsigned repeats;
} fault_signature;

static unsigned char *region(bare_memory *m, uint32_t address, unsigned size, bool write)
{
    uint64_t end = (uint64_t)address + size;
    if ((size == 1u || size == 2u || size == 4u) && end <= UINT64_C(0x100000000)) {
        if (end <= m->ram_size) return m->ram + address;
        if (!write && address >= ROM_BASE) return m->rom + (address - ROM_BASE);
    }
    ++m->unmapped;
    m->last_unmapped = address;
    m->last_write = write;
    return NULL;
}

static bool read_bus(void *opaque, uint32_t address, unsigned size, uint32_t *value)
{
    unsigned char *p = region((bare_memory *)opaque, address, size, false);
    unsigned i;
    if (p == NULL) return false;
    *value = 0;
    for (i = 0; i < size; ++i) *value = (*value << 8) | p[i];
    return true;
}

static bool write_bus(void *opaque, uint32_t address, unsigned size, uint32_t value)
{
    unsigned char *p = region((bare_memory *)opaque, address, size, true);
    unsigned i;
    if (p == NULL) return false;
    for (i = 0; i < size; ++i) p[i] = (unsigned char)(value >> (8u * (size - i - 1u)));
    return true;
}

static bool number(const char *text, uint64_t *value)
{
    char *end;
    errno = 0;
    *value = strtoull(text, &end, 10);
    return errno == 0 && end != text && *end == '\0' && text[0] != '-' && *value != 0;
}

static bool repeated_fault(fault_signature *seen, unsigned *used, unsigned *next,
                           const ppc_cpu *cpu, uint32_t pc)
{
    unsigned i;
    for (i = 0; i < *used; ++i) {
        fault_signature *f = &seen[i];
        if (f->pc == pc && f->opcode == cpu->last_opcode &&
            f->vector == cpu->last_exception && f->srr0 == cpu->srr0 &&
            f->srr1 == cpu->srr1 && f->dar == cpu->dar)
            return ++f->repeats >= 64u;
    }
    i = *used < FAULT_SIGNATURES ? (*used)++ : (*next)++ % FAULT_SIGNATURES;
    seen[i].pc = pc;
    seen[i].opcode = cpu->last_opcode;
    seen[i].vector = cpu->last_exception;
    seen[i].srr0 = cpu->srr0;
    seen[i].srr1 = cpu->srr1;
    seen[i].dar = cpu->dar;
    seen[i].repeats = 1;
    return false;
}

int main(int argc, char **argv)
{
    bare_memory memory = {0};
    ppc_cpu cpu;
    ppc_model model = PPC_MODEL_601;
    ppc_bus bus;
    FILE *file;
    uint64_t target = UINT64_C(10000000), ram_mib = 64, attempts = 0, retired = 0;
    uint64_t primary[64] = {0}, vectors[EXCEPTION_SLOTS] = {0}, exceptions = 0;
    uint64_t retired_rom = 0, retired_ram = 0;
    fault_signature faults[FAULT_SIGNATURES];
    unsigned used = 0, next = 0, consecutive_faults = 0, same_pc = 0;
    uint32_t last_pc = RESET_PC, last_opcode = 0, previous_pc = 0;
    const char *reason = "retired target reached";
    int status = 1;
    unsigned i;
    long length;
    if (argc < 2 || argc > 5 || (argc > 2 && !number(argv[2], &target)) ||
        (argc > 3 && (!number(argv[3], &ram_mib) || ram_mib > 512)) ||
        (argc > 4 && !ppc_model_parse(argv[4], &model))) {
        fprintf(stderr, "usage: rom_trace ROM [retired-target [ram-mib (1..512) [model]]]\n");
        return 2;
    }
    if (target > UINT64_MAX - UINT64_C(1000000)) {
        fprintf(stderr, "retired target too large\n");
        return 2;
    }
    file = fopen(argv[1], "rb");
    if (file == NULL) {
        int error = errno;
        perror("ROM (read-only)");
        return error == ENOENT ? 77 : 1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) != (long)ROM_SIZE ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "ROM must be exactly 4MiB\n");
        fclose(file);
        return 1;
    }
    memory.rom = (unsigned char *)malloc(ROM_SIZE);
    memory.ram_size = (size_t)ram_mib * 1024u * 1024u;
    memory.ram = (unsigned char *)calloc(memory.ram_size, 1);
    if (memory.rom == NULL || memory.ram == NULL ||
        fread(memory.rom, 1, ROM_SIZE, file) != ROM_SIZE) {
        fprintf(stderr, "cannot allocate RAM/read ROM\n");
        fclose(file);
        free(memory.rom);
        free(memory.ram);
        return 1;
    }
    fclose(file);
    memset(faults, 0, sizeof(faults));
    bus.opaque = &memory;
    bus.read = read_bus;
    bus.write = write_bus;
    ppc_init_model(&cpu, bus, model);
    if (cpu.pc != RESET_PC) {
        reason = "core reset vector is not FFF00100";
        goto report;
    }
    /* Preserve each model's real reset MSR; do not enable ME to prolong a
     * later-model trace. No external IRQs, clocks or fake MMIO are invented. */
    while (retired < target) {
        ppc_result result;
        last_pc = cpu.pc;
        if (++attempts > target + UINT64_C(1000000)) {
            reason = "attempt ceiling (non-retiring instructions do not meet target)";
            break;
        }
        result = ppc_step(&cpu);
        last_opcode = cpu.last_opcode;
        if (result == PPC_UNIMPLEMENTED) {
            reason = "unimplemented instruction/access";
            break;
        }
        if (result == PPC_CHECKSTOP) {
            reason = "checkstop";
            break;
        }
        if (result == PPC_EXCEPTION) {
            uint32_t vector = cpu.last_exception;
            ++exceptions;
            if (exceptions == 1)
                printf("first exception: vector=%04" PRIx32 " PC=%08" PRIx32
                       " opcode=%08" PRIx32 " SRR0=%08" PRIx32 " SRR1=%08" PRIx32 "\n",
                       vector, last_pc, last_opcode, cpu.srr0, cpu.srr1);
            if ((vector & 0xffu) != 0 || (vector >> 8) >= EXCEPTION_SLOTS) {
                reason = "invalid exception-vector report";
                break;
            }
            ++vectors[vector >> 8];
            if (++consecutive_faults >= 32u || repeated_fault(faults, &used, &next, &cpu, last_pc)) {
                reason = "repeated exception loop (not useful ROM coverage)";
                break;
            }
            continue;
        }
        if (result != PPC_OK || last_opcode == 0) {
            reason = "invalid retirement/result (zero is not an implemented instruction)";
            break;
        }
        consecutive_faults = 0;
        ++retired;
        ++primary[last_opcode >> 26];
        if (last_pc >= ROM_BASE) ++retired_rom;
        else if ((uint64_t)last_pc < memory.ram_size) ++retired_ram;
        same_pc = last_pc == previous_pc ? same_pc + 1u : 1u;
        previous_pc = last_pc;
        if (same_pc >= 4096u) {
            reason = "stalled one-instruction loop (not useful ROM coverage)";
            break;
        }
    }
    if (retired == target && strcmp(reason, "retired target reached") == 0) status = 0;
report:
    printf("bare ROM trace (%s): %s\n", ppc_model_information(model)->name, reason);
    printf("mapping: RAM 00000000..%08" PRIx32 " (%" PRIu64 " MiB), "
           "ROM FFC00000..FFFFFFFF read-only; no MMIO/aliases\n",
           (uint32_t)(memory.ram_size - 1u), ram_mib);
    printf("retired=%" PRIu64 "/%" PRIu64 " attempts=%" PRIu64
           " exceptions=%" PRIu64 " ROM=%" PRIu64 " RAM=%" PRIu64 "\n",
           retired, target, attempts, exceptions, retired_rom, retired_ram);
    printf("last PC=%08" PRIx32 " opcode=%08" PRIx32 " next PC=%08" PRIx32
           " MSR=%08" PRIx32 " SRR0=%08" PRIx32 " SRR1=%08" PRIx32 "\n",
           last_pc, last_opcode, cpu.pc, cpu.msr, cpu.srr0, cpu.srr1);
    printf("unmapped bus accesses=%" PRIu64 " last=%08" PRIx32 " (%s)\n",
           memory.unmapped, memory.last_unmapped, memory.last_write ? "write" : "read/fetch");
    printf("retired primary-opcode histogram:");
    for (i = 0; i < 64u; ++i)
        if (primary[i] != 0) printf(" %u:%" PRIu64, i, primary[i]);
    printf("\nexception-vector histogram:");
    for (i = 0; i < EXCEPTION_SLOTS; ++i)
        if (vectors[i] != 0) printf(" %04x:%" PRIu64, i << 8, vectors[i]);
    printf("\nThis is CPU coverage only, not a hardware or Macintosh boot test.\n");
    free(memory.rom);
    free(memory.ram);
    return status;
}
