/* Manual-derived black-box differential driver.
 * Sources: MPC601UM 2.2.4, 2.2.5.2, chapter 3; PEM 2.1.3, 2.1.5,
 * 3.1.3.1 and 4.2.1/4.2.4. No independent emulator implementation read.
 * Input is raw big-endian guest words, including the generated initialisation.
 * Output is 32 GPRs, CR, common XER, 32 FPRs and FPSCR, all big-endian.
 * Usage: oracle program.bin [instruction-budget]
 */
#include "ppc.h"
#include "ppc_model.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define CODE_BASE UINT32_C(0x00011000)
#define DATA_BASE UINT32_C(0x00020000)
#define DATA_SIZE 4096u
#define CODE_LIMIT (DATA_BASE - CODE_BASE)
/* PEM 2.1.5 Table 2-6: SO/OV/CA and byte count. MPC601UM 2.2.5.2
 * adds byte-compare bits 16-23, deliberately OUTSIDE this common oracle. */
#define COMMON_XER UINT32_C(0xe000007f)

typedef struct oracle_memory {
    unsigned char *code;
    size_t size;
    unsigned char data[DATA_SIZE];
} oracle_memory;

static unsigned char *region(oracle_memory *m, uint32_t address,
                             unsigned size, bool write)
{
    uint64_t end = (uint64_t)address + size;
    if ((size != 1u && size != 2u && size != 4u) || end > UINT64_C(0x100000000))
        return NULL;
    if (address >= DATA_BASE && end <= (uint64_t)DATA_BASE + DATA_SIZE)
        return m->data + (address - DATA_BASE);
    if (!write && address >= CODE_BASE && end <= (uint64_t)CODE_BASE + m->size)
        return m->code + (address - CODE_BASE);
    return NULL;
}

static bool read_bus(void *opaque, uint32_t address, unsigned size, uint32_t *value)
{
    unsigned char *p = region((oracle_memory *)opaque, address, size, false);
    unsigned i;
    if (p == NULL) return false;
    *value = 0;
    for (i = 0; i < size; ++i) *value = (*value << 8) | p[i];
    return true;
}

static bool write_bus(void *opaque, uint32_t address, unsigned size, uint32_t value)
{
    unsigned char *p = region((oracle_memory *)opaque, address, size, true);
    unsigned i;
    if (p == NULL) return false;
    for (i = 0; i < size; ++i) p[i] = (unsigned char)(value >> (8u * (size - i - 1u)));
    return true;
}

static bool put_be32(uint32_t value)
{
    unsigned char bytes[4];
    unsigned i;
    for (i = 0; i < 4u; ++i) bytes[i] = (unsigned char)(value >> (24u - 8u * i));
    return fwrite(bytes, 1, sizeof(bytes), stdout) == sizeof(bytes);
}

int main(int argc, char **argv)
{
    oracle_memory memory = {0};
    ppc_cpu cpu;
    ppc_model model = PPC_MODEL_601;
    ppc_bus bus;
    FILE *file;
    long length;
    uint64_t budget = UINT64_C(1000000), steps = 0;
    uint32_t end;
    unsigned i;
    if (argc < 2 || argc > 4 || (argc == 4 && !ppc_model_parse(argv[3], &model))) {
        fprintf(stderr, "usage: oracle program.bin [instruction-budget [model]]\n");
        return 2;
    }
    if (argc >= 3) {
        char *tail;
        errno = 0;
        budget = strtoull(argv[2], &tail, 10);
        if (errno != 0 || *tail != '\0' || tail == argv[2] || argv[2][0] == '-' || budget == 0) {
            fprintf(stderr, "invalid instruction budget\n");
            return 2;
        }
    }
    file = fopen(argv[1], "rb");
    if (file == NULL) {
        perror("program");
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
        (unsigned long)length > CODE_LIMIT || (length & 3L) != 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "program must contain 1..%u bytes of complete BE32 words\n", CODE_LIMIT);
        fclose(file);
        return 1;
    }
    memory.size = (size_t)length;
    memory.code = (unsigned char *)malloc(memory.size);
    if (memory.code == NULL || fread(memory.code, 1, memory.size, file) != memory.size) {
        fprintf(stderr, "cannot read program\n");
        fclose(file);
        free(memory.code);
        return 1;
    }
    fclose(file);
    bus.opaque = &memory;
    bus.read = read_bus;
    bus.write = write_bus;
    ppc_init_model(&cpu, bus, model);
    /* The guest prefix defines every GPR, CR, XER, LR and CTR before use.
     * r1 remains a defined scratch-buffer pointer, NOT a random destination.
     * User-mode common instructions only; no translation or guest OS. */
    cpu.pc = CODE_BASE;
    cpu.msr = PPC_MSR_PR | PPC_MSR_ME | PPC_MSR_FP;
    end = CODE_BASE + (uint32_t)memory.size;
    while (cpu.pc != end) {
        ppc_result result;
        uint32_t pc = cpu.pc;
        if (++steps > budget || pc < CODE_BASE || pc >= end || (pc & 3u) != 0) {
            fprintf(stderr, "program escaped/budget exhausted: pc=%08" PRIx32
                    " steps=%" PRIu64 "\n", pc, steps);
            free(memory.code);
            return 1;
        }
        result = ppc_step(&cpu);
        if (result != PPC_OK) {
            fprintf(stderr, "step failed: result=%d pc=%08" PRIx32
                    " opcode=%08" PRIx32 " vector=%08" PRIx32 "\n",
                    (int)result, pc, cpu.last_opcode, cpu.last_exception);
            free(memory.code);
            return 1;
        }
    }
#ifdef _WIN32
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
        perror("binary stdout");
        free(memory.code);
        return 1;
    }
#endif
    for (i = 0; i < 32u; ++i) {
        if (!put_be32(cpu.gpr[i])) goto output_error;
    }
    if (!put_be32(cpu.cr) || !put_be32(cpu.xer & COMMON_XER)) goto output_error;
    for (i = 0; i < 32u; ++i)
        if (!put_be32((uint32_t)(cpu.fpr[i] >> 32)) || !put_be32((uint32_t)cpu.fpr[i]))
            goto output_error;
    if (!put_be32(cpu.fpscr) || fflush(stdout) != 0) goto output_error;
    free(memory.code);
    return 0;
output_error:
    perror("state output");
    free(memory.code);
    return 1;
}
