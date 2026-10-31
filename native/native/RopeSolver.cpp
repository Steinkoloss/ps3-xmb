#include "RopeSolver.h"
#include "rope_program.h"
#include "rope_digests.h"
#include <openssl/sha.h>
#include <sys/mman.h>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

extern void moyou_math_func_00095F60(ppu_context*);

extern "C" {
uint8_t* vm_base = nullptr;
int vm_inline_ok = 0, vm_inline_st_ok = 0;
uint32_t ppu_vm_size = 0x04000000, ppu_hle_inject_base = 0;
uint32_t g_barrier_sync_watch = 0, g_null_sweep_hi = 0;
const unsigned char* g_spu_coh_bits = nullptr;
PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*) = nullptr;
}

namespace {
constexpr uint32_t imageBase = 0x02000000, object = 0x01000000, deformer = 0x01001000;
constexpr uint32_t stack = 0x01ff0000, arrays = 0x01010000;
constexpr std::array<uint32_t, 7> arrayFields = {0x44,0x74,0xa4,0xd4,0x104,0x134,0x164};
constexpr uint32_t arrayBytes = 361 * 16;

void check(uint64_t address, size_t size, bool write = false) {
    const auto end = address + size;
    bool valid = end >= address && ((address >= 0x10000 && end <= 0x800000 && !write) ||
        (address >= object && end <= 0x01020000) ||
        (address >= 0x01f00000 && end <= 0x02000000) ||
        (address >= imageBase && end <= imageBase + renderer_image_size && !write));
    if (!valid) throw std::runtime_error("Original rope accessed an unadapted guest address: " + std::to_string(address));
}

void load(const std::string& path, uint32_t destination, size_t size, const uint8_t* expected) {
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> bytes(size);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)) ||
        file.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Wrong-sized original CPU image: " + path);
    uint8_t digest[SHA256_DIGEST_LENGTH];
    if (!SHA256(bytes.data(), bytes.size(), digest) || std::memcmp(digest, expected, sizeof digest))
        throw std::runtime_error("Original CPU image failed its integrity check: " + path);
    std::memcpy(vm_base + destination, bytes.data(), bytes.size());
}

float readFloat(uint32_t address) { return std::bit_cast<float>(vm_read32(address)); }
void writeFloat(uint32_t address, float value) { vm_write32(address, std::bit_cast<uint32_t>(value)); }
void drain(ppu_context* ctx) {
    unsigned count = 0;
    while (g_trampoline_fn) {
        if (++count > 10000) throw std::runtime_error("Original CPU continuation limit exceeded");
        auto next = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        next(ctx);
    }
}
}

extern "C" {
uint8_t vm_read8(uint64_t address) { check(address, 1); return vm_base[address]; }
uint16_t vm_read16(uint64_t address) {
    check(address, 2);
    return uint16_t(vm_base[address]) << 8 | vm_base[address + 1];
}
uint32_t vm_read32(uint64_t address) {
    check(address, 4);
    return uint32_t(vm_base[address]) << 24 | uint32_t(vm_base[address + 1]) << 16 |
        uint32_t(vm_base[address + 2]) << 8 | vm_base[address + 3];
}
uint64_t vm_read64(uint64_t address) { return uint64_t(vm_read32(address)) << 32 | vm_read32(address + 4); }
void vm_write8(uint64_t address, uint8_t value) { check(address, 1, true); vm_base[address] = value; }
void vm_write16(uint64_t address, uint16_t value) {
    check(address, 2, true);
    for (unsigned i = 0; i < 2; ++i) vm_base[address + i] = value >> (8 - i * 8);
}
void vm_write32(uint64_t address, uint32_t value) {
    check(address, 4, true);
    for (unsigned i = 0; i < 4; ++i) vm_base[address + i] = value >> (24 - i * 8);
}
void vm_write64(uint64_t address, uint64_t value) {
    check(address, 8, true);
    for (unsigned i = 0; i < 8; ++i) vm_base[address + i] = value >> (56 - i * 8);
}
void ps3_indirect_call(ppu_context*) { throw std::runtime_error("Unadapted original CPU indirect call"); }
void ps3_indirect_call_to(ppu_context*, uint32_t) { throw std::runtime_error("Unadapted original CPU indirect target"); }
void ps3_hle_call(unsigned nid, ppu_context* ctx) {
    switch (nid) {
    case 0x831d70a5: {
        auto size = ctx->gpr[5];
        if (size > 0x100000) throw std::runtime_error("Original memcpy exceeds rope arena");
        check(ctx->gpr[3], size, true);
        check(ctx->gpr[4], size);
        std::memcpy(vm_base + ctx->gpr[3], vm_base + ctx->gpr[4], size);
        return;
    }
    case 0xb5e28191: {
        const auto toc = ctx->gpr[2], savedStack = ctx->gpr[1];
        ctx->gpr[2] = 0x006f5558;
        moyou_math_func_00095F60(ctx);
        drain(ctx);
        ctx->gpr[2] = toc;
        if (ctx->gpr[1] != savedStack) throw std::runtime_error("Original math did not restore its stack");
        return;
    }
    case 0xffff0001: throw std::runtime_error("Original math requested an unadapted floating-point exception");
    case 0xffff0002: return; // GPU FFD submission is consumed by the rendering adapter.
    default: throw std::runtime_error("Unadapted original CPU import: " + std::to_string(nid));
    }
}
}

struct RopeSolver::State {
    ppu_context cpu{};
    State() {
        if (vm_base) throw std::runtime_error("The CPU baseline currently permits one rope instance");
        auto* memory = mmap(nullptr, ppu_vm_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) throw std::runtime_error("Cannot reserve original CPU address space");
        vm_base = static_cast<uint8_t*>(memory);
        for (const auto [start, size] : std::array<std::array<uint32_t, 2>, 3>{
                {{0x00000000,0x00800000},{object,0x00020000},{0x01f00000,0x00200000}}}) {
            if (mprotect(vm_base + start, size, PROT_READ | PROT_WRITE)) {
                munmap(vm_base, ppu_vm_size); vm_base = nullptr;
                throw std::runtime_error("Cannot map original CPU memory regions");
            }
        }
        cpu.gpr[1] = stack;
        cpu.gpr[2] = 0x020a7d70;
        cpu.vscr = 0x10000;
    }
    ~State() { munmap(vm_base, ppu_vm_size); vm_base = nullptr; }
};

RopeSolver::RopeSolver(const std::string& image, const std::string& math) : state_(std::make_unique<State>()) {
    load(image, imageBase, renderer_image_size, renderer_image_digest);
    load(math, 0, math_image_size, math_image_digest);
    vm_write32(object, deformer);
    for (unsigned i = 0; i < arrayFields.size(); ++i) vm_write32(object + arrayFields[i], arrays + i * arrayBytes);
    vm_write32(object + 0x2c, 19);
    vm_write32(object + 0x30, 19);
}
RopeSolver::~RopeSolver() = default;

void RopeSolver::reset(unsigned state) {
    if (state > 2) throw std::runtime_error("Invalid original rope state");
    const uint32_t positions[] = {0x02095a40,0x02097c18,0x02099df0};
    const uint32_t velocities[] = {0x02096b2c,0x02098d04,0x0209aedc};
    for (unsigned node = 0; node < 361; ++node) {
        for (unsigned component = 0; component < 4; ++component) {
            auto p = component < 3 ? vm_read32(positions[state] + node * 12 + component * 4) : 0x3f800000;
            auto v = component < 3 ? vm_read32(velocities[state] + node * 12 + component * 4) : 0;
            for (unsigned array : {0u,2u,3u}) vm_write32(arrays + array * arrayBytes + node * 16 + component * 4, p);
            vm_write32(arrays + 6 * arrayBytes + node * 16 + component * 4, v);
        }
    }
    for (uint32_t field : {0x2c0u,0x2c4u,0x2ccu,0x2c8u,0x2d4u}) vm_write32(object + field, 0);
    for (uint32_t field : {0x14u,0xb4u,0xb8u}) vm_write32(deformer + field, 0);
    vm_write8(deformer + 0xbc, 1);
    g_trampoline_fn = nullptr;
}

void RopeSolver::parameter(std::string_view name, float value) {
    if (!std::isfinite(value)) throw std::runtime_error("Non-finite original rope parameter");
    if (name == "FFDPARAM1") { writeFloat(deformer + 0xac, value); return; }
    static constexpr std::array<std::pair<std::string_view, uint32_t>, 15> fields = {{
        {"DAMPING",0x274},{"LENGTH",0x278},{"TENSION",0x27c},{"SPACING",0x280},
        {"THINNESS",0x284},{"BRIGHTNESS",0x288},{"MIPMAPBIAS",0x29c},{"FRESNEL",0x28c},
        {"TIMESTEP",0x294},{"PERTURBATION",0x298},{"FALLOFF",0x290},{"ENDX",0x2a0},
        {"ENDY",0x2a4},{"ENDZ",0x2a8},{"TIME",0x2c0},
    }};
    for (const auto& [key, offset] : fields) if (key == name) { writeFloat(object + offset, value); return; }
    throw std::runtime_error("Unknown original rope parameter: " + std::string(name));
}

void RopeSolver::step() {
    auto& cpu = state_->cpu;
    cpu.gpr[3] = object;
    const auto savedStack = cpu.gpr[1];
    moyou_rope_func_0204BED0(&cpu);
    drain(&cpu);
    if (cpu.gpr[1] != savedStack || cpu.gpr[2] != 0x020a7d70)
        throw std::runtime_error("Original solver did not restore its calling convention");
}

void RopeSolver::advance(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 1)
        throw std::runtime_error("Invalid original frame interval");
    auto& cpu = state_->cpu;
    cpu.gpr[3] = object;
    cpu.fpr[1] = seconds;
    const auto savedStack = cpu.gpr[1];
    moyou_rope_func_0204F814(&cpu);
    drain(&cpu);
    if (cpu.gpr[1] != savedStack || cpu.gpr[2] != 0x020a7d70)
        throw std::runtime_error("Original frame did not restore its calling convention");
}

RopeSolver::Nodes RopeSolver::positions() const {
    Nodes nodes;
    for (unsigned node = 0; node < 361; ++node)
        for (unsigned c = 0; c < 4; ++c) {
            nodes[node][c] = readFloat(arrays + node * 16 + c * 4);
            if (!std::isfinite(nodes[node][c])) throw std::runtime_error("Non-finite original rope coordinate");
        }
    return nodes;
}
float RopeSolver::time() const { return readFloat(object + 0x2c0); }
uint32_t RopeSolver::randomState() const { return vm_read32(object + 0x2d4); }
