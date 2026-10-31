/* Replay original SPU subroutines from canonical register/local-store snapshots.
 * External channels and code modification require an explicit adapter. They
 * fail here so an incomplete execution cannot become an accuracy checkpoint. */
#include "spu/spu_context.h"
#include "program_digests.h"
#include "SpuRuntime.h"
#include <inttypes.h>
#include <openssl/sha.h>
#include <setjmp.h>

extern void moyou_spline_spu_recomp_register(void);
extern void moyou_particles_spu_recomp_register(void);

typedef void (*Function)(spu_context*);
static Function functions[SPU_LS_SIZE / 4];
static uint8_t original[SPU_LS_SIZE];
static spu_context context;
static jmp_buf escape;
static uint64_t instructions, budget;
static uint32_t stop_address, stop_code;
static const char* failure;

volatile int g_spu_direct = 0;
int g_spu_ls_watch_n = 0;
uint8_t g_spu_stack_reset_hint[1024];
SPU_THREAD_LOCAL Function g_spu_trampoline_fn;

static void fail(const char* message) {
    failure = message;
    longjmp(escape, 1);
}

static uint32_t be32(const uint8_t* bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
           (uint32_t)bytes[2] << 8 | bytes[3];
}

static void put32(uint8_t* bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (24 - 8 * i));
}

static int read_file(const char* path, uint8_t* bytes, size_t size) {
    FILE* file = fopen(path, "rb");
    if (!file) return 0;
    int valid = fread(bytes, 1, size, file) == size && fgetc(file) == EOF;
    if (ferror(file)) valid = 0;
    fclose(file);
    return valid;
}

void spu_register_function(uint32_t address, Function function) {
    if (address >= SPU_LS_SIZE || address % 4 || functions[address / 4]) {
        fputs("Invalid or duplicated translated function\n", stderr);
        exit(2);
    }
    functions[address / 4] = function;
}

void spu_indirect_branch(spu_context* ctx) {
    if (ctx->pc >= SPU_LS_SIZE || ctx->pc % 4 || !functions[ctx->pc / 4])
        fail("Branch target has no verified native translation");
    functions[ctx->pc / 4](ctx);
}

void spu_drain_call(spu_context* ctx, uint32_t return_pc) {
    while (ctx->pc != return_pc) {
        Function next = g_spu_trampoline_fn;
        g_spu_trampoline_fn = NULL;
        if (!next) fail("Translated call lost its architectural continuation");
        next(ctx);
    }
    g_spu_trampoline_fn = NULL;
}

void spu_img_restore(spu_context* ctx, int32_t image) {
    if (ctx->image_id != image) fail("Unexpected SPU overlay switch");
}

void spu_trace_pc(spu_context* ctx, uint32_t pc) {
    ctx->pc = pc;
    if (instructions == budget) fail("Instruction budget exceeded");
    ++instructions;
    if (pc > SPU_LS_SIZE - 4 || pc % 4 || memcmp(ctx->ls + pc, original + pc, 4))
        fail("Local-store code changed; static translation is no longer valid");
    if (ctx->host_depth > 256) fail("Translated call depth exceeded");
}

void spu_trace_rt(spu_context* ctx, uint32_t rt) { (void)ctx; (void)rt; }
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr) {
    (void)l; (void)w; (void)p; (void)pc; (void)lr;
    fail("Unexpected local-store diagnostic hook");
}

void spu_stop(spu_context* ctx) {
    stop_code = ctx->stop_code;
    fail("SPU stopped before the requested return checkpoint");
}
void spu_halt(spu_context* ctx) { (void)ctx; fail("SPU halted before the requested return checkpoint"); }
u128 spu_rdch(spu_context* ctx, uint32_t channel) {
    (void)ctx; (void)channel; fail("SPU channel input needs a captured runtime adapter");
    return (u128){0};
}
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    (void)ctx; (void)channel; (void)value; fail("SPU channel output needs a captured runtime adapter");
}
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel) {
    (void)ctx; (void)channel; fail("SPU channel status needs a captured runtime adapter");
    return 0;
}

int moyou_spu_open(unsigned program, const char* path) {
    failure = NULL;
    if (program > 1 || !read_file(path, original, sizeof original)) {
        failure = "Cannot read original SPU image"; return 0;
    }
    uint8_t digest[SHA256_DIGEST_LENGTH];
    if (!SHA256(original, sizeof original, digest) || memcmp(digest, program_digests[program], sizeof digest)) {
        failure = "Original SPU image failed its integrity check"; return 0;
    }
    memset(functions, 0, sizeof functions);
    spu_context_init(&context, 0);
    memcpy(context.ls, original, sizeof original);
    for (unsigned lane = 0; lane < 4; ++lane) context.gpr[1]._u32[lane] = 0x3f000;
    if (program == 0) moyou_spline_spu_recomp_register();
    else moyou_particles_spu_recomp_register();
    g_spu_trampoline_fn = NULL;
    return 1;
}

int moyou_spu_call(uint32_t entry, uint64_t limit) {
    if (entry >= SPU_LS_SIZE || entry % 4 || entry == 0x2ff0 || limit == 0 || limit > 100000000) {
        failure = "Invalid original SPU subroutine call"; return 0;
    }
    failure = NULL;
    instructions = 0;
    budget = limit;
    stop_address = 0x2ff0;
    stop_code = 0;
    context.status = SPU_STATUS_RUNNING;
    context.pc = entry;
    context.host_depth = 0;
    for (unsigned lane = 0; lane < 4; ++lane) context.gpr[0]._u32[lane] = stop_address;
    g_spu_trampoline_fn = NULL;
    if (setjmp(escape)) return 0;
    spu_indirect_branch(&context);
    spu_drain_call(&context, stop_address);
    return 1;
}

int moyou_spu_write(uint32_t address, const void* bytes, size_t size) {
    if (address > SPU_LS_SIZE || size > SPU_LS_SIZE - address) {
        failure = "Original SPU write exceeds local store"; return 0;
    }
    memcpy(context.ls + address, bytes, size);
    return 1;
}
int moyou_spu_read(uint32_t address, void* bytes, size_t size) {
    if (address > SPU_LS_SIZE || size > SPU_LS_SIZE - address) {
        failure = "Original SPU read exceeds local store"; return 0;
    }
    memcpy(bytes, context.ls + address, size);
    return 1;
}
int moyou_spu_set_register(unsigned reg, const uint32_t words[4]) {
    if (reg >= 128) { failure = "Invalid original SPU register"; return 0; }
    for (unsigned lane = 0; lane < 4; ++lane) context.gpr[reg]._u32[lane] = words[lane];
    return 1;
}
int moyou_spu_get_register(unsigned reg, uint32_t words[4]) {
    if (reg >= 128) { failure = "Invalid original SPU register"; return 0; }
    for (unsigned lane = 0; lane < 4; ++lane) words[lane] = context.gpr[reg]._u32[lane];
    return 1;
}
const char* moyou_spu_error(void) { return failure ? failure : "Unknown SPU execution failure"; }
uint64_t moyou_spu_instructions(void) { return instructions; }

#ifndef MOYOU_SPU_LIBRARY
int main(int argc, char** argv) {
    if (argc != 5 || (strcmp(argv[1], "spline") && strcmp(argv[1], "particles"))) {
        fputs("Usage: moyou-spu-replay spline|particles original-ls.bin input.snapshot output.snapshot\n"
              "Input: 32-byte BE header (MOYOUI01, entry u32, return u32, budget u64, zero u64),\n"
              "then 128 registers of four BE u32 lanes, then 256 KiB local store.\n"
              "Output: MOYOUO01, pc u32, status u32, executed u64, stop u32, zero u32; same buffers.\n", stderr);
        return 2;
    }
    static uint8_t input[32 + 128 * 16 + SPU_LS_SIZE];
    if (!read_file(argv[2], original, sizeof original) || !read_file(argv[3], input, sizeof input)) {
        fputs("Cannot read an exact-size original image and input snapshot\n", stderr);
        return 2;
    }
    uint8_t digest[SHA256_DIGEST_LENGTH];
    unsigned program_index = !strcmp(argv[1], "particles");
    if (!SHA256(original, sizeof original, digest) || memcmp(digest, program_digests[program_index], sizeof digest)) {
        fputs("Original local-store image does not match the compiled program\n", stderr);
        return 2;
    }
    uint32_t entry = be32(input + 8);
    stop_address = be32(input + 12);
    budget = (uint64_t)be32(input + 16) << 32 | be32(input + 20);
    if (memcmp(input, "MOYOUI01", 8) || be32(input + 24) || be32(input + 28) ||
            entry >= SPU_LS_SIZE || entry % 4 || stop_address >= SPU_LS_SIZE ||
            stop_address % 4 || entry == stop_address || budget == 0 || budget > 100000000) {
        fputs("Invalid snapshot header\n", stderr);
        return 2;
    }
    spu_context_init(&context, 0);
    context.status = SPU_STATUS_RUNNING;
    context.pc = entry;
    for (unsigned reg = 0; reg < 128; ++reg)
        for (unsigned lane = 0; lane < 4; ++lane)
            context.gpr[reg]._u32[lane] = be32(input + 32 + reg * 16 + lane * 4);
    memcpy(context.ls, input + 32 + 128 * 16, SPU_LS_SIZE);
    if (!strcmp(argv[1], "spline")) moyou_spline_spu_recomp_register();
    else moyou_particles_spu_recomp_register();
    if (setjmp(escape)) {
        fprintf(stderr, "%s at PC %05X after %" PRIu64 " instructions (stop %X)\n",
                failure, context.pc, instructions, stop_code);
        return 1;
    }
    spu_indirect_branch(&context);
    spu_drain_call(&context, stop_address);
    memcpy(input, "MOYOUO01", 8);
    put32(input + 8, context.pc);
    put32(input + 12, context.status);
    put32(input + 16, (uint32_t)(instructions >> 32));
    put32(input + 20, (uint32_t)instructions);
    put32(input + 24, stop_code);
    for (unsigned reg = 0; reg < 128; ++reg)
        for (unsigned lane = 0; lane < 4; ++lane)
            put32(input + 32 + reg * 16 + lane * 4, context.gpr[reg]._u32[lane]);
    memcpy(input + 32 + 128 * 16, context.ls, SPU_LS_SIZE);
    FILE* output = fopen(argv[4], "wb");
    if (!output) { perror("Cannot open output snapshot"); return 2; }
    int valid = fwrite(input, 1, sizeof input, output) == sizeof input;
    if (fclose(output)) valid = 0;
    if (!valid) { fputs("Cannot write output snapshot\n", stderr); return 2; }
    printf("Returned at %05X after %" PRIu64 " original instructions\n", context.pc, instructions);
    return 0;
}
#endif
