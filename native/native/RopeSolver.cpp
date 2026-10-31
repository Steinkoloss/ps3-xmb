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
#include <map>
#include <stdexcept>
#include <vector>

extern void moyou_math_func_00095F60(ppu_context*);
extern void moyou_math_func_00093F94(ppu_context*);
extern void moyou_math_func_00093B38(ppu_context*);
extern void moyou_math_func_00092D34(ppu_context*);
extern void moyou_math_func_00092E90(ppu_context*);

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
constexpr uint32_t stack = 0x01ff0000, particleStack = 0x01f80000, arrays = 0x01010000;
constexpr uint32_t menu = 0x01002000, controller = 0x01004000, handle = 0x01005000;
constexpr uint32_t heapBase = 0x01100000;
// Particle system object and its allocations (firmware new/memalign).
constexpr uint32_t particleBase = 0x01500000, particleEnd = 0x01a00000;
uint32_t particleCursor = particleBase;
// Native particle render object: two vertex buffers named by tokens.
constexpr uint32_t vertexToken = 0x7e000000;
std::array<uint32_t, 2> vertexBuffers{};
// qgl_base (dev_flash/vsh/module/qglbase.sprx) random 0x2e67e2d8 at 0x5f578: a shared
// counter (bss, 0 at load) hashed like the exposure noise; tick 0xd15d6a2c at 0x4dfa0 is a
// per-frame counter. On the console both also advance for other qgl users.
uint32_t randomState = 0, ticks = 0, gridData = 0, uniformObjects = 0;
constexpr uint32_t toneObject = 0x01008000;
constexpr std::array<uint32_t, 7> arrayFields = {0x44,0x74,0xa4,0xd4,0x104,0x134,0x164};
constexpr uint32_t arrayBytes = 361 * 16;
uint32_t heapCursor = heapBase;
std::map<uint32_t, RopeSolver::Texture> capturedTextures;
std::map<uint32_t, std::array<uint32_t, 4>> lockedTextures;

bool allowed(uint64_t address, size_t size, bool write) {
    const auto end = address + size;
    return end >= address && ((address >= 0x10000 && end <= 0x800000 && !write) ||
        // Unrelocated variable imports (lis rX,0): instance counters of other modules.
        (address >= 0x10 && end <= 0x100) ||
        (address >= object && end <= 0x01020000) ||
        (address >= heapBase && end <= 0x01500000) ||
        (address >= particleBase && end <= particleEnd) ||
        (address >= 0x01f00000 && end <= 0x02000000) ||
        (address >= imageBase && end <= imageBase + renderer_image_size &&
            (!write || address >= imageBase + 0x9f000)));
}

// 4 KiB pages wholly inside one permitted region (bit 0 read, bit 1 write); other
// accesses take the exact region test.
std::array<uint8_t, 0x04000000 / 0x1000> pagePermissions{};
void buildPagePermissions() {
    for (uint64_t page = 0; page < pagePermissions.size(); ++page)
        pagePermissions[page] = uint8_t(allowed(page << 12, 0x1000, false) | allowed(page << 12, 0x1000, true) << 1);
}

[[gnu::noinline, gnu::cold]] void checkExact(uint64_t address, size_t size, bool write) {
    if (!allowed(address, size, write))
        throw std::runtime_error("Original rope accessed an unadapted guest address: " + std::to_string(address));
}

inline void check(uint64_t address, size_t size, bool write = false) {
    const auto page = address >> 12;
    if (page < pagePermissions.size() && ((address + size - 1) >> 12) == page &&
        (pagePermissions[page] & (write ? 2 : 1))) return;
    checkExact(address, size, write);
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
void firmwareMath(ppu_context* ctx, void (*function)(ppu_context*)) {
    const auto toc = ctx->gpr[2], savedStack = ctx->gpr[1];
    ctx->gpr[2] = 0x006f5558;
    function(ctx);
    drain(ctx);
    ctx->gpr[2] = toc;
    if (ctx->gpr[1] != savedStack) throw std::runtime_error("Original math did not restore its stack");
}
}

extern "C" {
uint8_t vm_read8(uint64_t address) { check(address, 1); return vm_base[address]; }
uint16_t vm_read16(uint64_t address) {
    check(address, 2);
    uint16_t v; std::memcpy(&v, vm_base + address, 2); return __builtin_bswap16(v);
}
uint32_t vm_read32(uint64_t address) {
    check(address, 4);
    uint32_t v; std::memcpy(&v, vm_base + address, 4); return __builtin_bswap32(v);
}
uint64_t vm_read64(uint64_t address) {
    check(address, 8);
    uint64_t v; std::memcpy(&v, vm_base + address, 8); return __builtin_bswap64(v);
}
void vm_write8(uint64_t address, uint8_t value) { check(address, 1, true); vm_base[address] = value; }
void vm_write16(uint64_t address, uint16_t value) {
    check(address, 2, true);
    value = __builtin_bswap16(value); std::memcpy(vm_base + address, &value, 2);
}
void vm_write32(uint64_t address, uint32_t value) {
    check(address, 4, true);
    value = __builtin_bswap32(value); std::memcpy(vm_base + address, &value, 4);
}
void vm_write64(uint64_t address, uint64_t value) {
    check(address, 8, true);
    value = __builtin_bswap64(value); std::memcpy(vm_base + address, &value, 8);
}
#if __has_include("moyou_rope_dispatch.inc")
const std::map<uint32_t, void (*)(ppu_context*)> lifted = {
#include "moyou_rope_dispatch.inc"
};
#else
const std::map<uint32_t, void (*)(ppu_context*)> lifted;
#endif
uint32_t particleAllocate(uint64_t bytes, uint64_t alignment);
// Vectormath Matrix4::lookAt: column-major view matrix.
RopeSolver::Matrix lookAt(const float* eye, const float* target, const float* up) {
    float f[3], s[3], u[3];
    for (unsigned i = 0; i < 3; ++i) f[i] = target[i] - eye[i];
    const auto normalize = [](float* x) { const float l = std::sqrt(x[0]*x[0]+x[1]*x[1]+x[2]*x[2]); for (unsigned i = 0; i < 3; ++i) x[i] /= l; };
    normalize(f);
    s[0] = f[1]*up[2]-f[2]*up[1]; s[1] = f[2]*up[0]-f[0]*up[2]; s[2] = f[0]*up[1]-f[1]*up[0];
    normalize(s);
    u[0] = s[1]*f[2]-s[2]*f[1]; u[1] = s[2]*f[0]-s[0]*f[2]; u[2] = s[0]*f[1]-s[1]*f[0];
    const auto dot = [](const float* a, const float* b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; };
    return {s[0],u[0],-f[0],0, s[1],u[1],-f[1],0, s[2],u[2],-f[2],0, -dot(s,eye),-dot(u,eye),dot(f,eye),1};
}
constexpr uint32_t nativeAllocate = 0x01fe0000;   // fake RSX memory manager method
void ps3_indirect_call(ppu_context* ctx) {
    if (uint32_t(ctx->ctr) == nativeAllocate) {
        // (result, this, bytes, ..., alignment, ...): a refcounted block handle.
        const auto block = particleAllocate(0x40, 16);
        vm_write32(block + 0x10, particleAllocate(ctx->gpr[5], ctx->gpr[7] ? ctx->gpr[7] : 16));
        vm_write32(ctx->gpr[3], block);
        return;
    }
    auto function = lifted.find(uint32_t(ctx->ctr));
    if (function == lifted.end())
        throw std::runtime_error("Original CPU indirect call leaves the lifted closure: " + std::to_string(uint32_t(ctx->ctr)));
    function->second(ctx);
}
// Firmware new/delete and heap alloc/free in the particle arena: first fit over freed
// blocks (coalesced), else the bump cursor.
std::map<uint32_t, uint32_t> particleBlocks, particleFree;
uint32_t particleAllocate(uint64_t bytes, uint64_t alignment) {
    if (alignment < 16) alignment = 16;
    if (alignment & (alignment - 1) || alignment > 0x10000) throw std::runtime_error("Unadapted original allocation alignment");
    bytes = std::max<uint64_t>(16, (bytes + 15) & ~uint64_t(15));
    for (auto block = particleFree.begin(); block != particleFree.end(); ++block) {
        const uint64_t start = block->first, end = start + block->second;
        const uint64_t address = (start + alignment - 1) & ~(alignment - 1);
        if (address + bytes > end) continue;
        particleFree.erase(block);
        if (address > start) particleFree[uint32_t(start)] = uint32_t(address - start);
        if (address + bytes < end) particleFree[uint32_t(address + bytes)] = uint32_t(end - address - bytes);
        particleBlocks[uint32_t(address)] = uint32_t(bytes);
        std::memset(vm_base + address, 0, bytes);
        return uint32_t(address);
    }
    const uint64_t address = (particleCursor + alignment - 1) & ~(alignment - 1);
    if (bytes > particleEnd - address) throw std::runtime_error("Original particle allocations exceed native arena");
    if (address > particleCursor) particleFree[particleCursor] = uint32_t(address - particleCursor);
    particleCursor = uint32_t(address + bytes);
    particleBlocks[uint32_t(address)] = uint32_t(bytes);
    std::memset(vm_base + address, 0, bytes);
    return uint32_t(address);
}
void particleRelease(uint64_t address) {
    if (!address) return;
    auto block = particleBlocks.find(uint32_t(address));
    if (block == particleBlocks.end()) throw std::runtime_error("Original code freed an unknown allocation: " + std::to_string(address));
    uint32_t start = block->first, size = block->second;
    particleBlocks.erase(block);
    auto next = particleFree.lower_bound(start);
    if (next != particleFree.end() && next->first == start + size) { size += next->second; next = particleFree.erase(next); }
    if (next != particleFree.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == start) { start = previous->first; size += previous->second; particleFree.erase(previous); }
    }
    particleFree[start] = size;
}
void ps3_indirect_call_to(ppu_context*, uint32_t) { throw std::runtime_error("Unadapted original CPU indirect target"); }
// One guest thread per context and no sharing: lightweight mutex lock (0x66) / unlock (0x68) succeed.
void lv2_syscall(ppu_context* ctx) {
    if (ctx->gpr[11] != 0x66 && ctx->gpr[11] != 0x68) throw std::runtime_error("Unadapted original system call: " + std::to_string(ctx->gpr[11]));
    ctx->gpr[3] = 0;
}
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
    case 0xb5e28191: firmwareMath(ctx, moyou_math_func_00095F60); return;
    case 0x38e69f09: firmwareMath(ctx, moyou_math_func_00093F94); return;
    case 0xa713f8cf: firmwareMath(ctx, moyou_math_func_00093B38); return;
    case 0xfcf08193: firmwareMath(ctx, moyou_math_func_00092D34); return;
    case 0x23b985f7: firmwareMath(ctx, moyou_math_func_00092E90); return;
    case 0xffff0003: {
        const auto bytes = ctx->gpr[4];
        if (bytes > 0x100000 || heapCursor + bytes > 0x01500000)
            throw std::runtime_error("Original texture allocation exceeds native arena");
        for (unsigned i = 0; i < 4; ++i) vm_write32(ctx->gpr[3] + i * 4, 0);
        vm_write32(ctx->gpr[3] + 4, bytes);
        vm_write32(ctx->gpr[3] + 12, heapCursor);
        heapCursor = (heapCursor + bytes + 15) & ~15u;
        return;
    }
    case 0xffff0004:
        vm_write32(ctx->gpr[3] + 8, 0); vm_write32(ctx->gpr[3] + 12, 0); return;
    case 0xadf765cd: {
        const auto width = ctx->gpr[5], height = ctx->gpr[6];
        if (ctx->gpr[4] != 0 || ctx->gpr[7] != 4 || ctx->gpr[8] != 0x17 ||
            !width || !height || width * height > 65536)
            throw std::runtime_error("Unadapted original procedural texture upload");
        auto size = width * height * 4;
        check(ctx->gpr[9], size);
        capturedTextures[ctx->gpr[3]] = {unsigned(width),unsigned(height),
            std::vector<uint8_t>(vm_base + ctx->gpr[9], vm_base + ctx->gpr[9] + size)};
        return;
    }
    case 0x9d994c18: {
        auto id = uint32_t(ctx->gpr[3]), width = uint32_t(ctx->gpr[5]), height = uint32_t(ctx->gpr[6]);
        auto format = uint32_t(ctx->gpr[7]);
        if (ctx->gpr[4] != 0 || width > 128 || height > 128 ||
            (format != 0x8029 && format != 0x8018)) throw std::runtime_error("Unadapted original exposure texture lock");
        auto found = lockedTextures.find(id);
        if (found == lockedTextures.end()) {
            auto bytes = width * height * 4;
            if (heapCursor + bytes > 0x01500000) throw std::runtime_error("Original exposure textures exceed native arena");
            found = lockedTextures.emplace(id, std::array<uint32_t,4>{heapCursor,width,height,format}).first;
            heapCursor = (heapCursor + bytes + 127) & ~127u;
        }
        vm_write32(ctx->gpr[8], width * 4);
        ctx->gpr[3] = found->second[0];
        return;
    }
    case 0x83b2cc6f: ctx->gpr[3] = particleAllocate(ctx->gpr[3], 16); return;            // operator new
    case 0xed3da02b: ctx->gpr[3] = particleAllocate(ctx->gpr[3], ctx->gpr[4]); return;   // aligned new
    case 0xd84b3689: particleRelease(ctx->gpr[3]); return;                             // delete
    case 0xf3b69a9a: ctx->gpr[3] = 1; return;                                          // common heap handle
    case 0x9308c8c0: ctx->gpr[3] = particleAllocate(ctx->gpr[5], ctx->gpr[4]); return;   // heap alloc (named)
    case 0x89c88658: ctx->gpr[3] = particleAllocate(ctx->gpr[5], ctx->gpr[4]); return;   // heap alloc
    case 0xaddb8a27: particleRelease(ctx->gpr[4]); return;                             // heap free
    case 0xad4384f2: ctx->gpr[3] = particleAllocate(ctx->gpr[4], 16); return;            // heap alloc (queue)
    // GPU resource setup for the particle draw; the native renderer owns these.
    case 0x1e3764f7: ctx->gpr[3] = particleAllocate(0x40, 16); return;                 // buffer object
    case 0xd4fd336d: case 0x85bd0c33: case 0xc59c1fe5: case 0x90e4802b: case 0x97aeded8:
        ctx->gpr[3] = 0; return;
    case 0x5262639a: ctx->gpr[3] = 0; return;
    case 0xf80196c1: ctx->gpr[3] = particleAllocate(16, 16); return;
    case 0x8461e528: ctx->gpr[3] = 0; return;                                          // system time (stats only)
    case 0xffff0005: {                                                                // render object ctor
        const auto renderObject = uint32_t(ctx->gpr[3]), capacity = uint32_t(ctx->gpr[4]);
        if (!capacity || capacity > 0x4000) throw std::runtime_error("Unadapted original particle capacity");
        vm_write32(renderObject, 0x0209d8e0);
        vm_write32(renderObject + 8, 1);       // held by the adapter; never released
        vm_write32(renderObject + 0x10, capacity);
        for (uint32_t i = 0; i < 2; ++i) {
            vertexBuffers[i] = particleAllocate(capacity * 32, 0x80);
            vm_write32(renderObject + 0x1c + i * 0xc, vertexToken + i);
            const auto list = particleAllocate(16, 16);
            vm_write32(list, 2);
            vm_write32(renderObject + 0x44 + i * 4, list);
        }
        return;
    }
    case 0xffff0006: {                                                                // vertex array -> pointer
        const auto token = uint32_t(ctx->gpr[3]) - vertexToken;
        if (token > 1 || !vertexBuffers[token]) throw std::runtime_error("Unadapted original vertex array");
        ctx->gpr[3] = vertexBuffers[token];
        return;
    }
    case 0x4ddb926b: ctx->fpr[1] = std::pow(float(ctx->fpr[1]), float(ctx->fpr[2])); return;  // powf
    case 0x9379e36e: ctx->fpr[1] = std::tan(float(ctx->fpr[1])); return;                      // tanf
    case 0x5b162b7f: {                                                                       // memmove
        const auto size = ctx->gpr[5];
        check(ctx->gpr[3], size, true); check(ctx->gpr[4], size);
        std::memmove(vm_base + ctx->gpr[3], vm_base + ctx->gpr[4], size);
        return;
    }
    case 0x2e67e2d8: {
        const uint32_t n = ++randomState, v = n ^ (n << 13);
        const uint32_t hash = (v * (v * v * 15731u + 789221u) + 1376312589u) & 0x7fffffffu;
        ctx->fpr[1] = std::fma(float(hash), -9.31322574615478515625e-10f, 1.0f);   // frsp, fmadds
        return;
    }
    case 0xd15d6a2c: ctx->gpr[3] = ticks; return;
    case 0xab37ac81: case 0x91d4236f:   // matrix uniform set: keep the value in the uniform object
        check(ctx->gpr[3] + 0x10, 64, true); check(ctx->gpr[4], 64);
        std::memcpy(vm_base + ctx->gpr[3] + 0x10, vm_base + ctx->gpr[4], 64);
        return;
    case 0x0e428679: {                  // Matrix4 inverse (general cofactor inverse; qgl's own is not available)
        check(ctx->gpr[3], 64, true); check(ctx->gpr[4], 64);
        double m[16], r[16];
        for (unsigned i = 0; i < 16; ++i) m[i] = readFloat(ctx->gpr[4] + i * 4);
        r[0] = m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
        r[4] = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
        r[8] = m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
        r[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
        r[1] = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
        r[5] = m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
        r[9] = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
        r[13] = m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
        r[2] = m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
        r[6] = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
        r[10] = m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
        r[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
        r[3] = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
        r[7] = m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
        r[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
        r[15] = m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
        const double det = m[0]*r[0]+m[1]*r[4]+m[2]*r[8]+m[3]*r[12];
        if (det == 0 || !std::isfinite(det)) throw std::runtime_error("Singular original matrix inverse");
        for (unsigned i = 0; i < 16; ++i) writeFloat(ctx->gpr[3] + i * 4, float(r[i] / det));
        return;
    }
    // Particle draw submission (texture/vertex binding, state, draw): the native renderer draws.
    case 0x58ce6d93: case 0x5d4e8794: case 0x7adfc9b9: case 0xfa6d4f6f: case 0x17427a25: case 0x0c3b402a:
    case 0xd9f235f2: case 0x21b94163: case 0x84d310d3: case 0x1af503ef: case 0xa07ee0e6: case 0x15b229db:
    case 0x2ecddceb: case 0x56c4aefe: case 0x5f9b0f4b: case 0x8cf83e4a: case 0xb41d0556: case 0xdcc86364:
    case 0xea37227c: case 0xf3a17538: case 0xf679d0d7: case 0xf8be4852: case 0x476a66ad:
        return;
    case 0x2e5d5ba2: {                  // quaternion of the camera rotation (unrotated look-at camera)
        check(ctx->gpr[3], 16, true);
        for (unsigned c = 0; c < 3; ++c) for (unsigned r = 0; r < 3; ++r)
            if (readFloat(ctx->gpr[4] + (c * 4 + r) * 4) != (c == r ? 1.0f : 0.0f)) throw std::runtime_error("Unadapted original camera rotation");
        for (unsigned i = 0; i < 4; ++i) writeFloat(ctx->gpr[3] + i * 4, i == 3 ? 1.0f : 0.0f);
        return;
    }
    case 0xc2c9c741: {                   // Matrix4::lookAt(eye v2, target v3, up v4), column-major
        check(ctx->gpr[3], 64, true);
        float v[3][4];
        for (unsigned r = 0; r < 3; ++r) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(&ctx->vr[2 + r]);
            for (unsigned i = 0; i < 4; ++i)
                v[r][i] = std::bit_cast<float>(uint32_t(bytes[i*4]) << 24 | uint32_t(bytes[i*4+1]) << 16 | uint32_t(bytes[i*4+2]) << 8 | bytes[i*4+3]);
        }
        const auto view = lookAt(v[0], v[1], v[2]);
        for (unsigned i = 0; i < 16; ++i) writeFloat(ctx->gpr[3] + i * 4, view[i]);
        return;
    }
    case 0xffff0001: throw std::runtime_error("Original math requested an unadapted floating-point exception");
    case 0xffff0002: return; // GPU FFD submission is consumed by the rendering adapter.
    default: throw std::runtime_error("Unadapted original CPU import: " + std::to_string(nid));
    }
}
}

struct RopeSolver::State {
    // Rope/tone calls and particle calls run on separate threads with separate
    // contexts and stacks; they write disjoint guest memory.
    ppu_context cpu{}, particleCpu{};
    State() {
        if (vm_base) throw std::runtime_error("The CPU baseline currently permits one rope instance");
        auto* memory = mmap(nullptr, ppu_vm_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) throw std::runtime_error("Cannot reserve original CPU address space");
        vm_base = static_cast<uint8_t*>(memory);
        for (const auto [start, size] : std::array<std::array<uint32_t, 2>, 5>{
                {{0x00000000,0x00800000},{object,0x00020000},{heapBase,0x00400000},{0x01f00000,0x00200000},
                 {particleBase,particleEnd - particleBase}}}) {
            if (mprotect(vm_base + start, size, PROT_READ | PROT_WRITE)) {
                munmap(vm_base, ppu_vm_size); vm_base = nullptr;
                throw std::runtime_error("Cannot map original CPU memory regions");
            }
        }
        cpu.gpr[1] = stack;
        cpu.gpr[2] = 0x020a7d70;
        cpu.vscr = 0x10000;
        particleCpu = cpu;
        particleCpu.gpr[1] = particleStack;
        heapCursor = heapBase;
        // Per-instance state: a restarted simulation must not inherit the previous one.
        particleCursor = particleBase;
        particleBlocks.clear();
        particleFree.clear();
        vertexBuffers = {};
        ::randomState = ticks = gridData = uniformObjects = 0;
        buildPagePermissions();
        capturedTextures.clear();
        lockedTextures.clear();
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
    vm_write32(menu + 0xde0, handle);
    vm_write32(handle, controller);
    vm_write32(controller + 0x3a0, object);
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
    vm_write32(object + 0x270, state);
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

RopeSolver::Matrix RopeSolver::modelMatrix(Vector position, Vector axis, float angle) {
    static constexpr std::array<std::pair<uint32_t, uint32_t>, 13> fields = {{
        {0x1b8,0x274},{0x218,0x278},{0x278,0x27c},{0x2d8,0x280},{0x338,0x284},
        {0x398,0x288},{0x3f8,0x29c},{0x458,0x28c},{0x4b8,0x290},{0x518,0x294},
        {0x578,0x298},{0xf8,0x270},{0x158,0x2b8},
    }};
    for (const auto& [field, original] : fields) vm_write32(menu + field, vm_read32(object + original));
    for (unsigned i = 0; i < 3; ++i) {
        writeFloat(menu + 0x5d8 + i * 0x60, position[i]);
        writeFloat(menu + 0x6f8 + i * 0x60, axis[i]);
        vm_write8(menu + 0x5d0 + i * 0x60, 1);
        vm_write8(menu + 0x6f0 + i * 0x60, 1);
        vm_write32(menu + 0x878 + i * 0x60, vm_read32(object + 0x2a0 + i * 4));
    }
    writeFloat(menu + 0x818, angle);
    vm_write8(menu + 0x810, 1);
    auto& cpu = state_->cpu;
    cpu.gpr[3] = menu;
    moyou_rope_func_02029CC8(&cpu);
    drain(&cpu);
    Matrix matrix;
    for (unsigned i = 0; i < matrix.size(); ++i) matrix[i] = readFloat(object + 0x1f0 + i * 4);
    return matrix;
}

RopeSolver::Matrix RopeSolver::multiply(const Matrix& left, const Matrix& right) {
    constexpr uint32_t destination = 0x01006000, a = destination + 64, b = a + 64;
    for (unsigned i = 0; i < 16; ++i) { writeFloat(a + i * 4, left[i]); writeFloat(b + i * 4, right[i]); }
    auto& cpu = state_->cpu;
    cpu.gpr[3] = destination; cpu.gpr[4] = a; cpu.gpr[5] = b;
    moyou_rope_func_0203F290(&cpu);
    drain(&cpu);
    Matrix matrix;
    for (unsigned i = 0; i < 16; ++i) matrix[i] = readFloat(destination + i * 4);
    return matrix;
}

uint32_t RopeSolver::createParticles(unsigned capacity) {
    auto& cpu = state_->particleCpu;
    const auto system = particleAllocate(0x1980, 0x80), handles = particleAllocate(16, 16);
    // Both handles name one memory manager whose vtable slot 5 allocates natively.
    const auto manager = particleAllocate(0x40, 16), vtable = particleAllocate(0x40, 16), opd = particleAllocate(16, 16);
    vm_write32(opd, nativeAllocate);
    vm_write32(vtable + 0x14, opd);
    vm_write32(manager, vtable);
    vm_write32(manager + 8, 1);
    vm_write32(handles, manager);
    vm_write32(handles + 4, manager);
    cpu.gpr[3] = system;
    cpu.gpr[4] = handles + 8;
    cpu.gpr[5] = capacity;
    cpu.gpr[6] = handles;
    cpu.gpr[7] = handles + 4;
    moyou_rope_func_02060B14(&cpu); drain(&cpu);
    return system;
}

// ParticlesMenu apply (0x020329a4): menu items in registration order 0x02034664 → sys+0x1490.
void RopeSolver::particleParameters(uint32_t system, const std::map<std::string, float>& menu,
                                    const std::map<std::string, float>& ui, const std::map<std::string, float>& spe) {
    static constexpr std::array<const char*, 52> names = {"emitvelmin","emitvelmul","emitvelvar","emitconeangle",
        "emitnegprob","emitvelzscale","emitperframe","emitprob","agingspeed","agingvariance","friction",
        "spintimescale","deltatime","gravity","winddirx","winddiry","winddirz","windscale","windscale10",
        "brownianscale","spotposx","spotposy","spotposz","spotattnx","spotattny","spotattnz","specularpower",
        "specularcoeff","lambertcoeff","exposure","fresnel","colorcontrol","iridescentexp","globalalpha",
        "sizemiddle","sizenear","sizefar","nearfocus","nearfocusdist","nearfocuspow","neardarkness",
        "nearfuzziness","farfocus","farfocusdist","farfocuspow","fardarkness","nearalign","sizealign","glare",
        "glarescale","glarep1","glarep2"};
    std::array<float, 52> item{};
    for (unsigned i = 0; i < names.size(); ++i) {
        auto found = menu.find(names[i]);
        if (found == menu.end()) throw std::runtime_error(std::string("Missing original particle parameter: ") + names[i]);
        item[i] = found->second;
    }
    const auto block = system + 0x1490;
    const auto put = [&](unsigned word, float value) { writeFloat(block + word * 4, value); };
    for (unsigned i = 0; i < 6; ++i) put(i, item[i]);
    vm_write32(block + 6 * 4, uint32_t(int32_t(item[6])));     // emit per frame, truncated
    put(7, item[7]); put(8, item[8]); put(9, item[9]);
    for (unsigned i = 0; i < 4; ++i) put(0xc + i, item[10 + i]);
    for (unsigned i = 0; i < 3; ++i) put(0x10 + i, item[14 + i]);
    put(0x13, 0);
    put(0x14, item[18] * readFloat(0x020924b4) + item[17]);
    put(0x15, item[19]);
    for (unsigned i = 0; i < 3; ++i) { put(0x18 + i, item[20 + i]); put(0x1c + i, item[23 + i]); }
    put(0x1b, 0); put(0x1f, 0);
    put(0x20, item[26]); put(0x23, item[29]); put(0x21, item[27]); put(0x22, item[28]);
    for (unsigned i = 0; i < 5; ++i) put(0x24 + i, item[30 + i]);
    put(0x29, item[35]); put(0x2a, item[36]);
    for (unsigned i = 0; i < 4; ++i) put(0x2b + i, item[37 + i]);
    for (unsigned i = 0; i < 11; ++i) put(0x2f + i, item[41 + i]);
    // PARTICLES_UI follows in file order (words 0x3a-0x47), and PARTICLES_SPE fills the delta
    // block sys+0x15c0 at the words its values adjust. Layout read from a running 4.93 XMB
    // (RPCS3): live/menu words equal the UI file, delta words equal the SPE file.
    static constexpr std::array<const char*, 14> uiNames = {"brownian","rshakebrw","dpadrotmax","dpadscalex",
        "dpadscaley","dshakerotmax","dshakerotimp","dshakebrwimp","dshakethresh","dshakexcoeff","dshakegcoeff",
        "iconwind","iconwindsclx","iconwindscly"};
    const auto required = [](const std::map<std::string, float>& values, const char* name) {
        auto found = values.find(name);
        if (found == values.end()) throw std::runtime_error(std::string("Missing original particle parameter: ") + name);
        return found->second;
    };
    for (unsigned i = 0; i < uiNames.size(); ++i) put(0x3a + i, required(ui, uiNames[i]));
    for (const auto& [name, word] : std::initializer_list<std::pair<const char*, unsigned>>{
            {"deltatime",0xe},{"glare",0x36},{"specularpower",0x20},{"sizemiddle",0x28},{"globalalpha",0x27}})
        writeFloat(system + 0x15c0 + word * 4, required(spe, name));
}

RopeSolver::Matrix RopeSolver::cameraView() const {
    float v[3][4];
    for (unsigned r = 0; r < 3; ++r) for (unsigned i = 0; i < 4; ++i) v[r][i] = readFloat(0x02093730 + r * 16 + i * 4);
    return lookAt(v[0], v[1], v[2]);
}

RopeSolver::Matrix RopeSolver::inverseCameraView() const {
    // Rigid view: transpose the rotation, rotate the negated translation.
    const auto m = cameraView();
    Matrix r{};
    for (unsigned c = 0; c < 3; ++c) for (unsigned i = 0; i < 3; ++i) r[c * 4 + i] = m[i * 4 + c];
    for (unsigned i = 0; i < 3; ++i) r[12 + i] = -(r[i] * m[12] + r[4 + i] * m[13] + r[8 + i] * m[14]);
    r[15] = 1;
    return r;
}

uint32_t RopeSolver::createParticleLayer(uint32_t system) {
    auto& cpu = state_->particleCpu;
    // Camera (constructor 0x02024a68): near, far, FOV constants, eye/target/up.
    const auto camera = particleAllocate(0x100, 16), handles = particleAllocate(16, 16);
    vm_write32(camera + 8, 1);
    for (unsigned i = 0; i < 3; ++i) vm_write32(camera + 0x18 + i * 4, vm_read32(0x0209208c + i * 4));
    writeFloat(camera + 0x14, 16.0f / 9.0f);
    std::memcpy(vm_base + camera + 0x60, vm_base + 0x02093730, 48);
    vm_write32(handles, camera);
    // Original ParticlesLayer constructor 0x020322b8(this, camera, ...); init attaches the system.
    const auto layer = particleAllocate(0x1100, 16);
    cpu.gpr[3] = layer;
    cpu.gpr[4] = handles;
    cpu.gpr[5] = cpu.gpr[6] = cpu.gpr[7] = handles + 4;
    moyou_rope_func_020322B8(&cpu); drain(&cpu);
    vm_write32(layer + 0x18, system);
    // Animated colour (current +0xd0, from +0xe0, to +0xf0) and intensity (+0x120/+0x124/+0x128)
    // start at 0 and are faded in by the XMB boot animations. Start after the fade-in, at the
    // values a running 4.93 XMB holds (RPCS3): colour (1,1,1,0), intensity 1.
    for (unsigned i = 0; i < 3; ++i)
        for (uint32_t field : {0xd0u, 0xe0u, 0xf0u}) writeFloat(layer + field + i * 4, 1);
    for (unsigned i = 0; i < 3; ++i) writeFloat(layer + 0x120 + i * 4, 1);
    return layer;
}

unsigned RopeSolver::particleFrame(uint32_t system, uint32_t layer, const std::vector<std::array<float, 8>>& grid,
                                   unsigned width, unsigned height, const Matrix& columns, float aspect, float seconds) {
    if (grid.size() != size_t(width) * height) throw std::runtime_error("Invalid particle emission grid");
    if (!gridData) gridData = particleAllocate(grid.size() * 32, 0x80);
    for (size_t i = 0; i < grid.size(); ++i)
        for (unsigned c = 0; c < 8; ++c) writeFloat(gridData + i * 32 + c * 4, grid[i][c]);
    writeFloat(vm_read32(layer + 0xb0) + 0x14, aspect);
    // ParticlesLayer frame 0x02031f44: live parameters from the menu copy plus the
    // SPE deltas scaled by the layer's clamped factors (+0x140, +0x160).
    std::memmove(vm_base + system + 0x1360, vm_base + system + 0x1490, 0x128);
    const auto clamp = [](float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; };
    const float first = clamp(readFloat(layer + 0x140), 0, 1), second = clamp(readFloat(layer + 0x160), 0, 1);
    const auto adjust = [&](unsigned word, float factor, float lo, float hi) {
        writeFloat(system + 0x1360 + word * 4,
            clamp(factor * readFloat(system + 0x15c0 + word * 4) + readFloat(system + 0x1360 + word * 4), lo, hi));
    };
    adjust(0xe, first, readFloat(0x02092464), readFloat(0x02092468));   // delta time
    adjust(0x36, first, 0, 1);                                           // glare
    adjust(0x20, first, 0, 100);                                         // specular power
    adjust(0x28, second, 0, 90);                                         // size middle
    adjust(0x27, second, 0, 1);                                          // global alpha
    // Grid (0x0202b288): vertices, width, height, matrix columns.
    vm_write32(layer + 0x40, gridData);
    vm_write32(layer + 0x44, width);
    vm_write32(layer + 0x48, height);
    for (unsigned i = 0; i < 16; ++i) writeFloat(layer + 0x50 + i * 4, columns[i]);
    auto& cpu = state_->particleCpu;
    cpu.gpr[3] = layer;
    cpu.fpr[1] = seconds;
    moyou_rope_func_02031494(&cpu); drain(&cpu);   // frame logic, simulation inputs, emitter
    ++ticks;
    // 0x02031f44 → 0x0205de2c(system, camera FOV): the field takes the camera's FOV
    // (0x02023ed8 reads camera+0x20; 0.925 on a running XMB), then the PPU update path runs.
    writeFloat(system + 0x1358, readFloat(vm_read32(layer + 0xb0) + 0x20));
    cpu.gpr[3] = system;
    moyou_rope_func_0205DDC8(&cpu); drain(&cpu);   // PPU update path
    return vm_read32(system + 0x16fc);
}

RopeSolver::ParticleUniforms RopeSolver::particleUniforms(uint32_t system) {
    auto& cpu = state_->particleCpu;
    // qgl context (unrelocated global 0x34) with one identity modelview and its cached inverse.
    if (!vm_read32(0x34)) {
        const auto context = particleAllocate(0x400, 16), entry = particleAllocate(0x100, 16);
        const auto view = cameraView(), inverse = inverseCameraView();
        for (unsigned i = 0; i < 16; ++i) { writeFloat(entry + i * 4, view[i]); writeFloat(entry + 0x40 + i * 4, inverse[i]); }
        vm_write8(entry + 0x80, 1);
        vm_write32(context + 0x48 + 4, entry);
        vm_write32(context + 0x48 + 0x10, 1);
        vm_write32(0x34, context);
    }
    // Uniform objects are reused every frame (reset to zero) rather than allocated.
    if (!uniformObjects) uniformObjects = particleAllocate(0x900, 16);
    std::memset(vm_base + uniformObjects, 0, 0x900);
    const auto vertex = uniformObjects, fragment = uniformObjects + 0x400, handles = uniformObjects + 0x800;
    vm_write32(handles, vertex);
    vm_write32(handles + 4, fragment);
    // Unbound uniform objects (binding record +0xc == 0): setters keep values, skip uploads.
    const auto unbound = uniformObjects + 0x840;
    for (uint32_t offset : {0x30u,0x90u,0xf0u,0x150u,0x180u,0x1e0u,0x210u,0x240u,0x270u,0x2a0u,0x2d0u,0x300u,0x330u,0x360u})
        vm_write32(vertex + offset + 4, unbound);
    for (uint32_t offset : {0x30u,0x50u,0xb0u,0xe0u,0x100u,0x160u,0x190u}) vm_write32(fragment + offset + 4, unbound);
    cpu.gpr[3] = system;
    cpu.gpr[4] = handles;
    cpu.gpr[5] = handles + 4;
    moyou_rope_func_0205C254(&cpu); drain(&cpu);
    ParticleUniforms uniforms;
    const auto vector = [&](uint32_t object) { Vector v; for (unsigned i = 0; i < 4; ++i) v[i] = readFloat(object + 0x10 + i * 4); return v; };
    const auto matrix = [&](uint32_t object) { Matrix m; for (unsigned i = 0; i < 16; ++i) m[i] = readFloat(object + 0x10 + i * 4); return m; };
    uniforms.camera = matrix(vertex + 0xf0);
    uniforms.lightPack = matrix(vertex + 0x180);
    for (const auto& [name, offset] : std::initializer_list<std::pair<const char*, uint32_t>>{
            {"_ParticleSize",0x150},{"_Transparency",0x1e0},{"_Focus",0x210},{"_FocusCurves",0x240},
            {"_FrontFacingQuaternion",0x270},{"_NearControl",0x2a0},{"_LifeBoundsMin",0x2d0},
            {"_LifeBoundsMax",0x300},{"_Darkness",0x330},{"_Glare",0x360}})
        uniforms.vertex[name] = vector(vertex + offset);
    uniforms.fragmentLightPack = matrix(fragment + 0x50);
    uniforms.fragment["_Color"] = vector(fragment + 0xb0);
    // Fragment object (binders 0x02060044..0x0206023c): LightPack 0x50, Color 0xb0, Gamma 0xe0,
    // NearControl 0x100, Glare 0x160, IridescentExp 0x190.
    uniforms.fragment["_NearControl"] = vector(fragment + 0x100);
    uniforms.fragment["_Glare"] = vector(fragment + 0x160);
    uniforms.fragment["_Gamma"] = {readFloat(fragment + 0xe0 + 0xc - 0x10 + 0x10),0,0,0};
    uniforms.fragment["_IridescentExp"] = {readFloat(fragment + 0x190 + 0xc),0,0,0};
    uniforms.passes = (vm_read32(system + 0x1484) + 1) & 3;
    return uniforms;
}


uint32_t RopeSolver::guestWord(uint32_t address) const { return vm_read32(address); }

std::vector<uint8_t> RopeSolver::guestBytes(uint32_t address, uint32_t size) const {
    return {vm_base + address, vm_base + address + size};
}

std::vector<float> RopeSolver::particleVertices(uint32_t system) const {
    const auto count = vm_read32(system + 0x16fc), buffer = vertexBuffers[1 - vm_read32(system + 0x16f4)];
    // Record (0x0205d7b0, attribute setup 0x0205e5c8): float4 position+size, half4 rotation
    // (attribute 11), half4 previous position (attribute 10, unused by the programs).
    const auto half = [](uint16_t bits) {
        const uint32_t sign = uint32_t(bits & 0x8000) << 16, exponent = (bits >> 10) & 0x1f, mantissa = bits & 0x3ff;
        if (!exponent) return (bits & 0x8000 ? -1.0f : 1.0f) * std::ldexp(float(mantissa), -24);
        if (exponent == 31) return std::bit_cast<float>(sign | 0x7f800000u | (mantissa << 13));
        return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
    };
    std::vector<float> values(count * 8);
    for (unsigned p = 0; p < count; ++p) {
        const auto record = buffer + p * 32;
        for (unsigned i = 0; i < 4; ++i) values[p * 8 + i] = readFloat(record + i * 4);
        for (unsigned i = 0; i < 4; ++i) values[p * 8 + 4 + i] = half(vm_read16(record + 16 + i * 2));
    }
    return values;
}

std::array<RopeSolver::Texture, 2> RopeSolver::textures() {
    heapCursor = heapBase;
    capturedTextures.clear();
    vm_write32(object + 0x14, 0x2001);
    vm_write32(object + 0x28, 0x2002);
    // Original module constructor 01470 computes pow(2,4) for the stripe LUT.
    vm_write32(0x020a0f58, 16);
    auto& cpu = state_->cpu;
    cpu.gpr[3] = object;
    moyou_rope_func_0204BAD0(&cpu); drain(&cpu);
    cpu.gpr[3] = object;
    moyou_rope_func_0204B928(&cpu); drain(&cpu);
    if (!capturedTextures.contains(0x2001) || !capturedTextures.contains(0x2002))
        throw std::runtime_error("Original texture generator did not upload both LUTs");
    return {capturedTextures.at(0x2001),capturedTextures.at(0x2002)};
}

// Dither noise of the original exposure update 0x02071794 (loop at 0x02071ba4): the
// same RNG word (tone+0x150), hash, single-precision steps and byte packing.
std::vector<uint8_t> RopeSolver::exposureNoise() {
    std::vector<uint8_t> noise(32 * 32 * 4);
    uint32_t state = vm_read32(toneObject + 0x150);
    const float dither = readFloat(toneObject + 0x2dc);
    for (unsigned pixel = 0; pixel < 32 * 32; ++pixel) {
        const uint32_t n = ++state, v = n ^ (n << 13);
        const uint32_t hash = (v * (v * v * 15731u + 789221u) + 1376312589u) & 0x7fffffffu;
        float f = std::fma(float(hash), -9.31322574615478515625e-10f, 1.0f);
        f = std::fma(f, 0.5f, 0.5f);
        f = f * dither;
        f = f * 8.0f;
        if (f < -1.0f) f = -1.0f; else if (f > 1.0f) f = 1.0f;
        const float scaled = f * 255.0f;
        const auto byte = uint8_t(int32_t(scaled));   // fctiwz, low byte
        noise[pixel * 4] = noise[pixel * 4 + 1] = noise[pixel * 4 + 2] = byte;
        noise[pixel * 4 + 3] = 0;
    }
    vm_write32(toneObject + 0x150, state);
    return noise;
}

RopeSolver::ExposureTables RopeSolver::exposure(float value, float white, float threshold, float gamma, float dither) {
    // The lookup tables depend only on these inputs; reuse them and generate the
    // per-frame noise natively (checked against the original by --noise-check).
    const std::array<float, 5> inputs{value, white, threshold, gamma, dither};
    if (exposureCache_ && exposureInputs_ == inputs) {
        ExposureTables tables = *exposureCache_;
        tables.noise.rgba = exposureNoise();
        return tables;
    }
    writeFloat(toneObject + 0x29c, value);
    writeFloat(toneObject + 0x2a0, white);
    writeFloat(toneObject + 0x2a8, threshold);
    writeFloat(toneObject + 0x2e0, gamma);
    writeFloat(toneObject + 0x2dc, dither);
    vm_write32(toneObject + 0x138, 0x3001);
    vm_write32(toneObject + 0x140, 0x3002);
    vm_write32(toneObject + 0x148, 0x3003);
    auto& cpu = state_->cpu;
    cpu.gpr[3] = toneObject;
    moyou_rope_func_02071794(&cpu); drain(&cpu);
    ExposureTables tables;
    for (unsigned table = 0; table < 2; ++table) {
        auto address = lockedTextures.at(0x3001 + table)[0];
        auto& output = tables.rgba[table];
        output.resize(128 * 128 * 4);
        for (unsigned pixel = 0; pixel < 128 * 128; ++pixel) {
            for (unsigned channel = 0; channel < 2; ++channel) {
                auto h = vm_read16(address + pixel * 4 + channel * 2);
                uint32_t word = uint32_t(h & 0x8000) << 16;
                auto exponent = (h >> 10) & 31, mantissa = h & 1023;
                if (exponent == 31) word |= 0x7f800000 | (mantissa << 13);
                else if (exponent) word |= uint32_t(exponent + 112) << 23 | (mantissa << 13);
                else if (mantissa) {
                    int shift = 0;
                    while (!(mantissa & 1024)) { mantissa <<= 1; ++shift; }
                    word |= uint32_t(113 - shift) << 23 | ((mantissa & 1023) << 13);
                }
                // Original G16R16F samplers expose the transposed RG lookup
                // through XY, and the blue/glare lookup through ZW.
                const auto destination = table == 0 ? 1 - channel : 3 - channel;
                output[pixel * 4 + destination] = std::bit_cast<float>(word);
            }
            if (table == 0) output[pixel * 4 + 3] = 1;
        }
    }
    const auto address = lockedTextures.at(0x3003)[0];
    tables.noise = {32,32,std::vector<uint8_t>(vm_base + address, vm_base + address + 32 * 32 * 4)};
    exposureCache_ = std::make_unique<ExposureTables>(tables);
    exposureInputs_ = inputs;
    return tables;
}

bool RopeSolver::checkExposureNoise(unsigned frames) {
    for (unsigned frame = 0; frame < frames; ++frame) {
        const auto saved = vm_read32(toneObject + 0x150);
        const auto native = exposureNoise();
        vm_write32(toneObject + 0x150, saved);
        exposureCache_.reset();
        const auto original = exposure(exposureInputs_[0], exposureInputs_[1], exposureInputs_[2], exposureInputs_[3], exposureInputs_[4]);
        if (original.noise.rgba != native) return false;
    }
    return true;
}

RopeSolver::GlareWeights RopeSolver::glareWeights(Vector radii, float sumPower, float level) {
    for (unsigned channel = 0; channel < 3; ++channel) {
        if (!std::isfinite(radii[channel]) || radii[channel] < 0.34f || radii[channel] > 2.66f)
            throw std::runtime_error("Original Gaussian radius exceeds its fixed weight array");
        writeFloat(toneObject + 0x2b0 + channel * 4,radii[channel]);
    }
    if (!std::isfinite(sumPower) || !std::isfinite(level)) throw std::runtime_error("Invalid original glare parameters");
    auto& cpu = state_->cpu;
    cpu.gpr[3] = toneObject;
    moyou_rope_func_02071384(&cpu); drain(&cpu);
    GlareWeights result;
    for (unsigned channel = 0; channel < 3; ++channel) {
        const auto radius = vm_read32(toneObject + 0x288 + channel * 4);
        if (radius >= 8) throw std::runtime_error("Original Gaussian exceeds its shader tap count");
        for (unsigned tap = 0; tap <= radius; ++tap)
            result.taps[tap][channel] = readFloat(toneObject + 0x1c8 + channel * 64 + (radius + tap) * 4);
    }
    float sum = 0;
    for (unsigned i = 0; i < 6; ++i) {
        cpu.fpr[1] = sumPower; cpu.fpr[2] = i;
        firmwareMath(&cpu,moyou_math_func_00093F94);
        result.levels[i] = static_cast<float>(cpu.fpr[1]);
        sum = static_cast<float>(double(sum) + cpu.fpr[1]);
    }
    for (auto& weight : result.levels) weight = static_cast<float>(double(weight) / sum) * level * level;
    return result;
}
