#include "RopeSolver.h"
#include "rope_program.h"
#include "rope_digests.h"
#include "CellEstimate.h"
#include <openssl/sha.h>
#include <sys/mman.h>
#include <bit>
#include <immintrin.h>
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
// The math module's expf, pow, floorf and modf run natively (bit-identical over the live
// scene: mesh, particle arena and image); MOYOU_LIBM=original runs the firmware's.
float firmwareSinCos(float x, int which);
float firmwareExp(float x);
double firmwarePowInteger(double x, unsigned n);
float vmaxf(float x, float y);
float vminf(float x, float y);
void particleLayerFrame(uint32_t layer);
bool verifyNative();
void reportDifference(const char* name, const void* native, const void* original, size_t bytes);
bool nativeLibm(const char*) {
    static const bool original = std::getenv("MOYOU_LIBM") && !std::strcmp(std::getenv("MOYOU_LIBM"), "original");
    return !original;
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

// Inlined into the translations (LTO); the permission check stays on every access.
extern "C" {
[[gnu::always_inline]] uint8_t vm_read8(uint64_t address) { check(address, 1); return vm_base[address]; }
[[gnu::always_inline]] uint16_t vm_read16(uint64_t address) {
    check(address, 2);
    uint16_t v; std::memcpy(&v, vm_base + address, 2); return __builtin_bswap16(v);
}
[[gnu::always_inline]] uint32_t vm_read32(uint64_t address) {
    check(address, 4);
    uint32_t v; std::memcpy(&v, vm_base + address, 4); return __builtin_bswap32(v);
}
[[gnu::always_inline]] uint64_t vm_read64(uint64_t address) {
    check(address, 8);
    uint64_t v; std::memcpy(&v, vm_base + address, 8); return __builtin_bswap64(v);
}
[[gnu::always_inline]] void vm_write8(uint64_t address, uint8_t value) { check(address, 1, true); vm_base[address] = value; }
[[gnu::always_inline]] void vm_write16(uint64_t address, uint16_t value) {
    check(address, 2, true);
    value = __builtin_bswap16(value); std::memcpy(vm_base + address, &value, 2);
}
[[gnu::always_inline]] void vm_write32(uint64_t address, uint32_t value) {
    check(address, 4, true);
    value = __builtin_bswap32(value); std::memcpy(vm_base + address, &value, 4);
}
[[gnu::always_inline]] void vm_write64(uint64_t address, uint64_t value) {
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
// Matrix4 inverse (general cofactor inverse; qgl's own is not available): column-major.
RopeSolver::Matrix inverseMatrix(uint32_t source) {
        double m[16], r[16];
    for (unsigned i = 0; i < 16; ++i) m[i] = readFloat(source + i * 4);
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
    RopeSolver::Matrix result;
    for (unsigned i = 0; i < 16; ++i) result[i] = float(r[i] / det);
    return result;
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
void createRenderObject(uint32_t renderObject, uint32_t capacity);
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
    case 0xb5e28191:   // _FSin(float, which)
        if (nativeLibm("sin")) { ctx->fpr[1] = firmwareSinCos(float(ctx->fpr[1]), int(ctx->gpr[4])); return; }
        firmwareMath(ctx, moyou_math_func_00095F60); return;
    case 0x38e69f09:   // pow
        if (nativeLibm("pow")) { ctx->fpr[1] = std::pow(ctx->fpr[1], ctx->fpr[2]); return; }
        firmwareMath(ctx, moyou_math_func_00093F94); return;
    case 0xa713f8cf:   // modf(double, double*): the double takes r3's slot (PPC64 ABI), the pointer is r4
        if (nativeLibm("modf")) {
            double whole; ctx->fpr[1] = std::modf(ctx->fpr[1], &whole);
            vm_write64(uint32_t(ctx->gpr[4]), std::bit_cast<uint64_t>(whole)); return;
        }
        firmwareMath(ctx, moyou_math_func_00093B38); return;
    case 0xfcf08193:   // expf
        if (nativeLibm("expf")) { ctx->fpr[1] = firmwareExp(float(ctx->fpr[1])); return; }
        firmwareMath(ctx, moyou_math_func_00092D34); return;
    case 0x23b985f7:   // floorf
        if (nativeLibm("floorf")) { ctx->fpr[1] = std::floor(float(ctx->fpr[1])); return; }
        firmwareMath(ctx, moyou_math_func_00092E90); return;
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
    case 0xffff0005: createRenderObject(uint32_t(ctx->gpr[3]), uint32_t(ctx->gpr[4])); return;   // render object ctor
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
    case 0x0e428679: {                  // Matrix4 inverse
        check(ctx->gpr[3], 64, true); check(ctx->gpr[4], 64);
        const auto inverse = inverseMatrix(uint32_t(ctx->gpr[4]));
        for (unsigned i = 0; i < 16; ++i) writeFloat(ctx->gpr[3] + i * 4, inverse[i]);
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

namespace {
// VMX lanes as the rope solver's helpers compute them (vmaddfp with +0 is a product that
// turns -0 into +0; with -0 it is a plain product).
struct V4 { float v[4]; };
V4 loadV4(uint32_t address) { V4 r; for (unsigned i = 0; i < 4; ++i) r.v[i] = readFloat(address + i * 4); return r; }
void storeV4(uint32_t address, const V4& a) { for (unsigned i = 0; i < 4; ++i) writeFloat(address + i * 4, a.v[i]); }
V4 sub(const V4& a, const V4& b) { V4 r; for (unsigned i = 0; i < 4; ++i) r.v[i] = a.v[i] - b.v[i]; return r; }
V4 add(const V4& a, const V4& b) { V4 r; for (unsigned i = 0; i < 4; ++i) r.v[i] = a.v[i] + b.v[i]; return r; }
V4 scale(const V4& a, float f) { V4 r; for (unsigned i = 0; i < 4; ++i) r.v[i] = std::fma(a.v[i], f, -0.0f); return r; }
// 0x0204584c lane 0: products with a +0 addend, then (p0 + p1) + (p2 + p3).
float dot0(const V4& a) {
    float p[4];
    for (unsigned i = 0; i < 4; ++i) p[i] = std::fma(a.v[i], a.v[i], 0.0f);
    return (p[0] + p[1]) + (p[2] + p[3]);
}
float estimate(float s) { return std::bit_cast<float>(cellRsqrtEstimate(std::bit_cast<uint32_t>(s), true)); }
// 0x02046284: √s from the estimate and one Newton step; s itself when s or the estimate is 0.
float ropeLength(float s) {
    const float e = estimate(s), t = std::fma(e, s, 0.0f), h = std::fma(t, 0.5f, 0.0f);
    if (s == 0 || e == 0) return s;
    return std::fma(std::fma(-e, t, 1.0f), h, t);
}
// 0x0204597c: 1/√s refined once; the estimate if that gives NaN.
float ropeInverse(float s) {
    const float e = estimate(s), t = std::fma(e, s, 0.0f), h = std::fma(e, 0.5f, 0.0f);
    const float r = std::fma(std::fma(-e, t, 1.0f), h, e);
    return r == r ? r : e;
}
// 0x02024228: rotation about a unit axis (columns; _FSin for cosine and sine).
using Rotation = std::array<std::array<float, 4>, 4>;
Rotation axisRotation(const std::array<float, 3>& a, float radians) {
    const float c = firmwareSinCos(radians, 1), s = firmwareSinCos(radians, 0), k = 1.0f - c;
    const std::array<float, 3> ka{k * a[0], k * a[1], k * a[2]}, sa{s * a[0], s * a[1], s * a[2]};
    const auto P = [&](unsigned i, unsigned j) { return ka[i] * a[j]; };
    return {{{c + P(0,0), P(1,0) + sa[2], P(2,0) - sa[1], 0},
             {P(0,1) - sa[2], P(1,1) + c, P(2,1) + sa[0], 0},
             {P(0,2) + sa[1], P(1,2) - sa[0], P(2,2) + c, 0},
             {0, 0, 0, 1}}};
}
// 0x0204ab04: the solver's hash RNG (qgl's Perlin hash) in [-1, 1].
float ropeRandom(uint32_t& state) {
    const uint32_t n = ++state, v = (n << 13) ^ n;
    const uint32_t hash = (v * (v * v * 15731u + 789221u) + 1376312589u) & 0x7fffffffu;
    return std::fma(float(hash), -9.31322574615478515625e-10f, 1.0f);
}
}

// Rope step 0x0204bed0: previous state ← current, time advance (wrapping the deformer at 10),
// structural and bend springs to the nodes one and two to the left and above, a random
// perturbation, integration with damping, then the row constraints 0x0204bce0.
void RopeSolver::step() {
    std::vector<uint8_t> before;
    if (verifyNative()) before.assign(vm_base + object, vm_base + 0x01020000);
    const unsigned W = vm_read32(object + 0x2c), H = vm_read32(object + 0x30), count = W * H;
    const uint32_t previous = vm_read32(object + 0xa4), current = vm_read32(object + 0xd4), velocity = vm_read32(object + 0x164);
    std::memmove(vm_base + previous, vm_base + current, size_t(count) * 16);
    std::vector<V4> P(count), Vel(count);
    for (unsigned i = 0; i < count; ++i) { P[i] = loadV4(current + i * 16); Vel[i] = loadV4(velocity + i * 16); }
    const float timestep = readFloat(object + 0x294) * 9.99999975e-05f;
    float time = timestep + readFloat(object + 0x2c0);
    writeFloat(object + 0x2c0, time);
    if (time > 10.0f) {
        writeFloat(object + 0x2c0, 0.0f);
        const uint32_t d = vm_read32(object);
        writeFloat(d + 0xb8, readFloat(d + 0xb4));
        vm_write8(d + 0x18, 0);
        writeFloat(d + 0xb4, 0.0f);
        writeFloat(d + 0x14, 1.0f);
        vm_write32(d + 0x10, vm_read32(d + 0xc));
    }
    const float tension = readFloat(object + 0x27c), bendTension = tension * 10.0f, length = readFloat(object + 0x278);
    const float perturbation = readFloat(object + 0x298);
    uint32_t random = vm_read32(object + 0x2d4);
    const auto spring = [&](unsigned node, unsigned neighbour) {
        const V4 d = sub(P[neighbour], P[node]);
        const float strength = (ropeLength(dot0(d)) - length) * tension;
        const V4 force = scale(scale(d, ropeInverse(dot0(d))), strength);
        Vel[node] = add(force, Vel[node]);
        Vel[neighbour] = sub(Vel[neighbour], force);
    };
    const auto bend = [&](unsigned node, unsigned neighbour) {
        const V4 d = sub(P[neighbour], P[node]);
        const float stretch = std::fma(length, -2.0f, ropeLength(dot0(d)));
        const V4 force = scale(scale(scale(d, ropeInverse(dot0(d))), stretch), bendTension);
        Vel[node] = add(force, Vel[node]);
        Vel[neighbour] = sub(Vel[neighbour], force);
    };
    for (unsigned row = 0; row < H; ++row)
        for (unsigned column = 0; column < W; ++column) {
            const unsigned node = row * W + column;
            if (column > 0) spring(node, node - 1);
            if (column > 1) bend(node, node - 2);
            if (row > 0) spring(node, node - W);
            if (row > 1) bend(node, node - 2 * W);
            const float x = ropeRandom(random), y = ropeRandom(random), z = ropeRandom(random);
            Vel[node] = add(scale(V4{{x, y, z, 0.0f}}, perturbation), Vel[node]);
        }
    vm_write32(object + 0x2d4, random);
    const float damping = readFloat(object + 0x274) * readFloat(object + 0x294);
    for (unsigned i = 0; i < count; ++i) {
        P[i] = add(scale(Vel[i], timestep), P[i]);
        Vel[i] = sub(Vel[i], scale(Vel[i], damping));
    }
    // 0x0204bce0: smoothed time, a push on each row's first node, and each row's last node
    // pinned to an oscillating point scaled by the end position.
    const float smoothed = std::fma(readFloat(object + 0x2c0) - readFloat(object + 0x2c4), 0.100000001f, readFloat(object + 0x2c4));
    writeFloat(object + 0x2c4, smoothed);
    const float endY = readFloat(object + 0x2a4), endZ = readFloat(object + 0x2a8);
    for (unsigned row = 0; row < H; ++row) {
        const float push = readFloat(object + 0x294) * 0.0199999996f;
        const float t = float(int32_t(row)) / float(int32_t(H)) + smoothed;
        Vel[row * W] = add(V4{{push, 0.0f, 0.0f, 0.0f}}, Vel[row * W]);
        const unsigned last = row * W + W - 1;
        const float y = std::fma(firmwareSinCos(t * 11.0f, 0), 0.5f, 0.5f) * endY;
        const float z = ((firmwareSinCos(t * 15.0f, 1) + 1.0f) * 0.5f) * endZ;
        P[last] = V4{{0.0f, y, z, 1.0f}};
        Vel[last] = V4{{0.0f, 0.0f, 0.0f, 0.0f}};
    }
    for (unsigned i = 0; i < count; ++i) { storeV4(current + i * 16, P[i]); storeV4(velocity + i * 16, Vel[i]); }
    if (!before.empty()) {
        std::vector<uint8_t> native(vm_base + object, vm_base + 0x01020000);
        std::memcpy(vm_base + object, before.data(), before.size());
        stepOriginal();
        reportDifference("rope step", native.data(), vm_base + object, native.size());
        std::memcpy(vm_base + object, native.data(), native.size());
    }
}

// Rope frame 0x0204f814: fixed 60 Hz steps from the accumulated time, then each node blended
// between the last two states and scaled in y/z by the deformer's profile along x.
void RopeSolver::advance(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 1)
        throw std::runtime_error("Invalid original frame interval");
    std::vector<uint8_t> before;
    if (verifyNative()) before.assign(vm_base + object, vm_base + 0x01020000);
    // fmuls on a double operand, as the translation computes it: double product, then single.
    const float frames = float(seconds * 60.0);
    float accumulated = readFloat(object + 0x2cc) + frames;
    writeFloat(object + 0x2d0, frames);
    writeFloat(object + 0x2cc, accumulated);
    while (readFloat(object + 0x2cc) > 1.0f) {
        step();
        writeFloat(object + 0x2cc, readFloat(object + 0x2cc) - 1.0f);
    }
    accumulated = readFloat(object + 0x2cc);
    const unsigned count = vm_read32(object + 0x2c) * vm_read32(object + 0x30);
    const uint32_t blended = vm_read32(object + 0x44), previous = vm_read32(object + 0xa4), current = vm_read32(object + 0xd4);
    const float origin = readFloat(vm_read32(object) + 0xac);
    for (unsigned i = 0; i < count; ++i) {
        V4 p = add(scale(loadV4(previous + i * 16), 1.0f - accumulated), scale(loadV4(current + i * 16), accumulated));
        const float along = p.v[0] - origin;
        float wave, t;
        if (along < 0.0f) { wave = firmwareSinCos(0.0f, 1); t = 0.0f; }
        else { wave = firmwareSinCos(along * 1.57079637f, 1); t = along / 5.0f; }
        const float clamped = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        const float falloff = (1.0f - clamped) * (1.0f - clamped);
        const float profile = 1.29999995f - wave * (falloff * std::fma(1.0f - clamped, -2.0f, 3.0f));
        const V4 factor{{1.0f, profile, profile, 1.0f}};
        for (unsigned lane = 0; lane < 4; ++lane) p.v[lane] = std::fma(factor.v[lane], p.v[lane], -0.0f);
        storeV4(blended + i * 16, p);
    }
    if (!before.empty()) {
        std::vector<uint8_t> native(vm_base + object, vm_base + 0x01020000);
        std::memcpy(vm_base + object, before.data(), before.size());
        advanceOriginal(seconds);
        reportDifference("rope frame", native.data(), vm_base + object, native.size());
        std::memcpy(vm_base + object, native.data(), native.size());
    }
}

void RopeSolver::stepOriginal() {
    auto& cpu = state_->cpu;
    cpu.gpr[3] = object;
    const auto savedStack = cpu.gpr[1];
    moyou_rope_func_0204BED0(&cpu);
    drain(&cpu);
    if (cpu.gpr[1] != savedStack || cpu.gpr[2] != 0x020a7d70)
        throw std::runtime_error("Original solver did not restore its calling convention");
}

void RopeSolver::advanceOriginal(double seconds) {
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

// 0x02029cc8 with only position, axis and angle set (the menu copy-back is an identity): the
// axis normalised with the Cell estimate and one Newton step, the axis-angle rotation of
// 0x02024228 (_FSin for cosine and sine), then (identity basis | position) · rotation. Every
// lane operation is in the firmware's order; plain VMX products are fused with -0.
RopeSolver::Matrix RopeSolver::modelMatrix(Vector position, Vector axis, float angle) {
    const float x = axis[0], y = axis[1], z = axis[2];
    const float sum = (x * x + y * y) + (z * z + 0.0f);
    const float estimate = std::bit_cast<float>(cellRsqrtEstimate(std::bit_cast<uint32_t>(sum), true));
    const float t = estimate * sum, half = estimate * 0.5f;
    float inverse = std::fma(std::fma(-estimate, t, 1.0f), half, estimate);
    if (inverse != inverse) inverse = estimate;
    const auto R = axisRotation({x * inverse, y * inverse, z * inverse}, angle * 0.01745329238474369f);
    const float pos[4] = {position[0], position[1], position[2], 1};
    Matrix native;
    for (unsigned column = 0; column < 4; ++column)
        for (unsigned lane = 0; lane < 4; ++lane) {
            const auto basis = [&](unsigned axisIndex) {
                return std::fma(R[column][axisIndex], lane == axisIndex ? 1.0f : 0.0f, -0.0f);
            };
            native[column * 4 + lane] = std::fma(pos[lane], R[column][3], (basis(0) + basis(1)) + basis(2));
        }
    if (!verifyNative()) return native;
    const auto original = modelMatrixOriginal(position, axis, angle);
    reportDifference("model matrix", native.data(), original.data(), sizeof native);
    return native;
}

RopeSolver::Matrix RopeSolver::modelMatrixOriginal(Vector position, Vector axis, float angle) {
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

namespace {
// 0x0202b4f4: matrix (column-major) times vector, as the firmware orders its VMX fused
// multiply-adds: x1·c1 (+ -0, a plain product), then x0·c0, x2·c2, x3·c3 fused in turn.
std::array<float, 4> transform(const float* m, const float* v) {
    std::array<float, 4> r;
    for (unsigned i = 0; i < 4; ++i) {
        float t = v[1] * m[4 + i];
        t = std::fma(v[0], m[i], t);
        t = std::fma(v[2], m[8 + i], t);
        r[i] = std::fma(v[3], m[12 + i], t);
    }
    return r;
}
bool verifyNative() { static const bool verify = std::getenv("MOYOU_VERIFY_NATIVE") != nullptr; return verify; }
void reportDifference(const char* name, const void* native, const void* original, size_t bytes) {
    size_t differ = 0;
    for (size_t i = 0; i < bytes; ++i) differ += static_cast<const uint8_t*>(native)[i] != static_cast<const uint8_t*>(original)[i];
    if (differ) std::fprintf(stderr, "native %s: %zu bytes differ\n", name, differ);
}
}

// 0x0203f290: left · right, each column of right through transform().
RopeSolver::Matrix RopeSolver::multiply(const Matrix& left, const Matrix& right) {
    Matrix native;
    for (unsigned column = 0; column < 4; ++column) {
        const auto c = transform(left.data(), right.data() + column * 4);
        std::copy(c.begin(), c.end(), native.begin() + column * 4);
    }
    if (!verifyNative()) return native;
    const auto original = multiplyOriginal(left, right);
    reportDifference("multiply", native.data(), original.data(), sizeof native);
    return native;
}

RopeSolver::Matrix RopeSolver::multiplyOriginal(const Matrix& left, const Matrix& right) {
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

// Render object (adapter for the firmware's particle vertex buffers, 0x0209d8e0): two vertex
// buffers addressed by token and two submission lists. (C linkage: declared beside the imports.)
extern "C" void createRenderObject(uint32_t renderObject, uint32_t capacity) {
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
}

namespace {
// Parameter block constructor 0x02058ba8 (live +0x1360, menu +0x1490, SPE deltas +0x15c0):
// every default is a constant of the renderer image, read from where the firmware loads it.
void particleParameterDefaults(uint32_t block) {
    static constexpr std::array<std::pair<uint16_t, uint32_t>, 59> floats = {{
        {0x00,0x2092ef8},{0x04,0x2092efc},{0x08,0x2092f00},{0x0c,0x2092f04},{0x10,0x2092f08},{0x14,0x2092f0c},
        {0x1c,0x2092f10},{0x20,0x2092ef8},
        {0x30,0x2092f14},{0x34,0x2092f18},{0x38,0x2092f1c},{0x3c,0x2092f20},{0x50,0x2092f24},{0x54,0x2092f28},
        {0x80,0x2092f2c},{0x84,0x2092f30},{0x88,0x2092f34},{0x8c,0x2092f38},{0x90,0x2092f3c},{0x94,0x2092f40},
        {0x98,0x2092f40},{0x9c,0x2092f44},
        {0xa0,0x2092f48},{0xa4,0x2092f4c},{0xa8,0x2092f50},{0xac,0x2092f54},{0xb0,0x2092f58},{0xb4,0x2092f5c},
        {0xb8,0x2092f60},{0xbc,0x2092f64},{0xc0,0x2092f68},{0xc4,0x2092f6c},{0xc8,0x2092f70},{0xcc,0x2092f74},
        {0xd0,0x2092f78},{0xd4,0x2092f7c},
        {0xd8,0x2092fa8},{0xdc,0x2092fac},{0xe0,0x2092fb0},{0xe4,0x2092fb4},
        {0xe8,0x2092f80},{0xec,0x2092f84},{0xf0,0x2092f88},{0xf4,0x2092f8c},{0xf8,0x2092f8c},{0xfc,0x2092f90},
        {0x100,0x2092f8c},{0x104,0x2092f94},{0x108,0x2092f98},{0x10c,0x2092f9c},{0x110,0x2092fa0},{0x114,0x2092fa4},
        {0x118,0x2092f8c},{0x11c,0x2092f8c},
        // 16-byte vectors
        {0x40,0x20949a0},{0x60,0x20949b0},{0x70,0x20949c0},{0,0},{0,0}}};
    for (unsigned i = 0; i < 54; ++i) vm_write32(block + floats[i].first, vm_read32(floats[i].second));
    for (unsigned i = 54; i < 57; ++i)
        for (unsigned w = 0; w < 4; ++w) vm_write32(block + floats[i].first + w * 4, vm_read32(floats[i].second + w * 4));
    vm_write32(block + 0x18, 4);
    vm_write32(block + 0x24, 0);
    vm_write8(block + 0x120, 1);
    vm_write8(block + 0x121, 0);
    vm_write32(block + 0x124, 2);
}

// Simulation input constructor 0x0205d510 (system+0x100 and +0xa00): gravity, friction, the
// 32 x 16 flow grid descriptor over the 512-entry table at +0x80, identity frustum matrices,
// open bounds and the 60 Hz step.
void particleSimulationDefaults(uint32_t S) {
    const auto identity = [](uint32_t at) { for (unsigned i = 0; i < 4; ++i) writeFloat(at + i * 20, 1.0f); };
    writeFloat(S + 0x04, -4.99999987e-06f);
    writeFloat(S + 0x0c, 1.0f);
    for (unsigned i = 0; i < 3; ++i) writeFloat(S + 0x10 + i * 4, 0.00999999978f);
    vm_write32(S + 0x680, 512);
    identity(S + 0x700);
    identity(S + 0x740);
    vm_write32(S + 0x780, S + 0x80);
    vm_write32(S + 0x784, 32); vm_write32(S + 0x788, 16);
    writeFloat(S + 0x790, 32.0f); writeFloat(S + 0x794, 16.0f);
    vm_write32(S + 0x7a8, 1);
    for (unsigned i = 0; i < 3; ++i) vm_write32(S + 0x7b4 + i * 4, 1);
    vm_write32(S + 0x7c0, 32); vm_write32(S + 0x7c4, 16); vm_write32(S + 0x7c8, 31); vm_write32(S + 0x7cc, 15);
    for (unsigned i = 0; i < 4; ++i) { writeFloat(S + 0x800 + i * 4, -10000.0f); writeFloat(S + 0x810 + i * 4, 10000.0f); }
    identity(S + 0x840);
    writeFloat(S + 0x8a0, 0.200000003f);
    writeFloat(S + 0x8a8, 0.0500000007f);
    writeFloat(S + 0x8ac, 100.0f);
    for (unsigned i = 0; i < 3; ++i) writeFloat(S + 0x8b0 + i * 4, 0.0166666675f);
    writeFloat(S + 0x8bc, 1.0f);
}
}

// Particle system constructor 0x02060b14(system, ..., capacity, memory managers) as the arena
// it leaves: two particle arrays (dead particles: age -666, scale 1), the free index list, the
// sort buffer (a tenth of four per particle, 24 bytes each), the command queue, three
// parameter blocks, two simulation input blocks, the render object and the image's render
// object registry (0x020a0f80) and instance counter (0x020a0f60). Same allocations in the same
// order as the original.
uint32_t RopeSolver::createParticles(unsigned capacity) {
    if (!verifyNative()) return createParticlesNative(capacity);
    const std::vector<uint8_t> before(vm_base + particleBase, vm_base + particleEnd);
    const std::vector<uint8_t> globalsBefore(vm_base + 0x020a0f60, vm_base + 0x020a0f90);
    const auto blocks = particleBlocks, holes = particleFree;
    const auto cursor = particleCursor;
    const auto buffers = vertexBuffers;
    const auto original = createParticlesOriginal(capacity);
    const std::vector<uint8_t> arena(vm_base + particleBase, vm_base + particleEnd);
    const std::vector<uint8_t> globals(vm_base + 0x020a0f60, vm_base + 0x020a0f90);
    const auto originalBlocks = particleBlocks, originalHoles = particleFree;
    const auto originalCursor = particleCursor;
    const auto originalBuffers = vertexBuffers;
    std::memcpy(vm_base + particleBase, before.data(), before.size());
    std::memcpy(vm_base + 0x020a0f60, globalsBefore.data(), globalsBefore.size());
    particleBlocks = blocks; particleFree = holes; particleCursor = cursor; vertexBuffers = buffers;
    const auto system = createParticlesNative(capacity);
    reportDifference("particle system", vm_base + particleBase, arena.data(), arena.size());
    reportDifference("particle system globals", vm_base + 0x020a0f60, globals.data(), globals.size());
    if (system != original || particleBlocks != originalBlocks || particleFree != originalHoles ||
        particleCursor != originalCursor || vertexBuffers != originalBuffers)
        std::fprintf(stderr, "native particle system: 1 bytes differ (allocator)\n");
    return system;
}

uint32_t RopeSolver::createParticlesNative(unsigned capacity) {
    const auto system = particleAllocate(0x1980, 0x80);
    // Adapter memory manager the original's allocations go through (see createParticlesOriginal).
    const auto handles = particleAllocate(16, 16), manager = particleAllocate(0x40, 16);
    const auto vtable = particleAllocate(0x40, 16), opd = particleAllocate(16, 16);
    vm_write32(opd, nativeAllocate);
    vm_write32(vtable + 0x14, opd);
    vm_write32(manager, vtable);
    vm_write32(manager + 8, 1);
    vm_write32(handles, manager);
    vm_write32(handles + 4, manager);
    const uint32_t count = (capacity + 511) & ~511u;
    vm_write32(system, 0x0209d8f0);
    vm_write32(0x020a0f60, vm_read32(0x020a0f60) + 1);
    vm_write32(system + 0x2c, 15);     // two empty names (short strings, capacity 15)
    vm_write32(system + 0x4c, 15);
    vm_write32(system + 0x54, count);
    for (uint32_t array : {system + 0x60, system + 0x80}) {   // front and back particle arrays
        const auto particles = particleAllocate(count * 0x30, 0x80);
        for (uint32_t i = 0; i < count; ++i) {
            writeFloat(particles + i * 0x30 + 0xc, -666.0f);
            writeFloat(particles + i * 0x30 + 0x2c, 1.0f);
        }
        vm_write32(array, particles);
        vm_write32(array + 4, count);
        vm_write32(array + 0x10, particles);
        vm_write32(array + 0x14, count);
        vm_write32(array + 0x18, count);
    }
    vm_write32(system + 0xa0, system + 0x60);
    vm_write32(system + 0xa4, system + 0x80);
    particleSimulationDefaults(system + 0x100);
    particleSimulationDefaults(system + 0xa00);
    const auto freeList = particleAllocate(count * 4, 0x80);
    for (uint32_t i = 0; i < count; ++i) vm_write32(freeList + i * 4, i);
    vm_write32(system + 0x1300, freeList);
    vm_write32(system + 0x1304, count);
    vm_write32(system + 0x1310, freeList);
    vm_write32(system + 0x1314, count);
    vm_write32(system + 0x1318, count);
    const auto sorted = uint32_t(int64_t(float(count * 4) * 0.100000001f));
    const auto sortBuffer = particleAllocate(sorted * 24, 16);
    vm_write32(system + 0x132c, sorted);
    vm_write32(system + 0x1330, 1);
    vm_write32(system + 0x1334, sortBuffer);
    vm_write32(system + 0x1338, sorted);
    for (unsigned i = 0; i < 5; ++i) writeFloat(system + 0x1340 + i * 4, 1.0f);   // colour, intensity
    vm_write32(system + 0x1354, 0xffffffffu);
    for (uint32_t block : {0x1360u, 0x1490u, 0x15c0u}) particleParameterDefaults(system + block);
    // Command queue 0x0205cd14: three 4-byte channels, 1536 bytes of storage.
    const auto queue = particleAllocate(0x600, 1);
    for (unsigned i = 0; i < 3; ++i) vm_write32(system + 0x1724 + i * 0x28, 4);
    vm_write32(system + 0x179c, 1);
    vm_write32(system + 0x17bc, queue);
    vm_write32(system + 0x1858, queue);
    vm_write32(system + 0x185c, 0x5ff);
    // Render object 0x0205eb00, held by the system, the registry and the adapter.
    const auto render = particleAllocate(0x54, 16);
    createRenderObject(render, count);
    vm_write32(render + 8, 3);
    vm_write8(render + 0xc, 1);
    vm_write32(system + 0x16f0, render);
    vm_write32(system + 0x17c0, vm_read32(render + 0x44));
    vm_write32(system + 0x17c4, vm_read32(render + 0x44));
    if (vm_read32(0x020a0f80)) throw std::runtime_error("Unadapted second original particle system");
    const auto registry = particleAllocate(4, 4);
    vm_write32(registry, render);
    vm_write32(0x020a0f80, registry);
    vm_write32(0x020a0f84, registry + 4);
    vm_write32(0x020a0f88, registry + 4);
    return system;
}

uint32_t RopeSolver::createParticlesOriginal(unsigned capacity) {
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

// ParticlesLayer constructor 0x020322b8 as the state it leaves: the camera reference, the
// flow decay and weave spring constants, two 16-entry motion histories (+0x200 positions,
// +0x400 deltas), two shake axes (+0x700 on x, +0xb80 on w; 64-entry residual rings, 5-entry
// intercept rings), the tilt direction "none", the handler flags (+0x1003..+0x1006) and an
// empty animation list (+0x1018, a sentinel node).
uint32_t RopeSolver::particleLayerNative(uint32_t cameraHandle) {
    const uint32_t camera = vm_read32(cameraHandle);
    const auto layer = particleAllocate(0x1100, 16);
    vm_write32(layer, 0x0209d4a8);
    vm_write32(camera + 8, vm_read32(camera + 8) + 1);
    vm_write32(layer + 0xb0, camera);
    writeFloat(layer + 0xbc, 0.980000019f);
    writeFloat(layer + 0xc0, 1.0f);
    writeFloat(layer + 0x190, 0.0299999993f);
    vm_write32(layer + 0x1c0, 4);
    writeFloat(layer + 0x1d0, 0.0399999991f);
    vm_write8(layer + 0x1e4, 1);
    for (uint32_t history : {0x200u, 0x400u}) {
        vm_write32(layer + history + 0x100, 16);
        vm_write32(layer + history + 0x184, 1);
    }
    for (const auto& [axis, component] : {std::pair{0x700u, 0u}, std::pair{0xb80u, 3u}}) {
        const uint32_t X = layer + axis;
        vm_write32(X, component);
        vm_write32(X + 0x180, 64);
        vm_write32(X + 0x204, 1);
        vm_write32(X + 0x314, 5);
        vm_write32(X + 0x384, 1);
        writeFloat(X + 0x408, 1.0f);
    }
    for (uint32_t flag = 0x1003; flag <= 0x1006; ++flag) vm_write8(layer + flag, 1);
    const auto list = particleAllocate(0x40, 16);
    for (unsigned i = 0; i < 3; ++i) vm_write32(list + i * 4, list);
    vm_write8(list + 0x30, 1);
    vm_write8(list + 0x31, 1);
    vm_write32(layer + 0x1018, list);
    return layer;
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
    // ParticlesLayer constructor 0x020322b8(this, camera, ...); init attaches the system.
    uint32_t layer;
    if (!verifyNative()) layer = particleLayerNative(handles);
    else {
        const std::vector<uint8_t> before(vm_base + particleBase, vm_base + particleEnd);
        const auto blocks = particleBlocks, holes = particleFree;
        const auto cursor = particleCursor;
        layer = particleAllocate(0x1100, 16);
        cpu.gpr[3] = layer;
        cpu.gpr[4] = handles;
        cpu.gpr[5] = cpu.gpr[6] = cpu.gpr[7] = handles + 4;
        moyou_rope_func_020322B8(&cpu); drain(&cpu);
        const std::vector<uint8_t> arena(vm_base + particleBase, vm_base + particleEnd);
        const auto originalBlocks = particleBlocks, originalHoles = particleFree;
        const auto originalCursor = particleCursor;
        std::memcpy(vm_base + particleBase, before.data(), before.size());
        particleBlocks = blocks; particleFree = holes; particleCursor = cursor;
        const auto native = particleLayerNative(handles);
        reportDifference("particle layer", vm_base + particleBase, arena.data(), arena.size());
        if (native != layer || particleBlocks != originalBlocks || particleFree != originalHoles || particleCursor != originalCursor)
            std::fprintf(stderr, "native particle layer: 1 bytes differ (allocator)\n");
    }
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
    if (!allowed(gridData, grid.size() * 32, true)) throw std::runtime_error("Particle emission grid outside guest memory");
    auto* words = reinterpret_cast<uint32_t*>(vm_base + gridData);
    for (size_t i = 0; i < grid.size(); ++i)
        for (unsigned c = 0; c < 8; ++c) words[i * 8 + c] = __builtin_bswap32(std::bit_cast<uint32_t>(grid[i][c]));
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
    // Layer frame: native (particleLayerFrame) unless MOYOU_PARTICLE_FRAME=original;
    // MOYOU_VERIFY_NATIVE=1 compares it with the original every frame.
    const char* frameMode = std::getenv("MOYOU_PARTICLE_FRAME");
    const bool nativeFrame = !frameMode || std::strcmp(frameMode, "original");
    if (!nativeFrame) {
        cpu.gpr[3] = layer;
        cpu.fpr[1] = seconds;
        moyou_rope_func_02031494(&cpu); drain(&cpu);   // frame logic, simulation inputs, emitter
    } else if (verifyNative()) {
        // Development (MOYOU_EXERCISE_INPUT=1): random controller motion (layer+0x180) and D-pad
        // tilts so the interaction handlers' branches run; both paths see the same input.
        if (std::getenv("MOYOU_EXERCISE_INPUT")) {
            static uint32_t seed = 12345;
            const auto next = [] { seed = seed * 1664525u + 1013904223u; return float(int32_t(seed) >> 8) * (1.0f / 8388608.0f); };
            static float phase = 0;
            phase += 0.37f;
            // Alternating calm and shaking stretches of under a second.
            const float shake = std::fmod(phase, 30.0f) < 15.0f ? 0.0f : 3.0f;
            for (unsigned i = 0; i < 4; ++i)
                writeFloat(layer + 0x180 + i * 4, std::sin(phase * (1.0f + i)) * shake + next() * shake);
            if ((seed >> 24) % 23 == 0) vm_write32(layer + 0x1c0, (seed >> 8) % 4);
        }
        // The original from a snapshot (arena, allocator, qgl RNG), then the native frame from the
        // same state; arena bytes, allocator and RNG must agree.
        const std::vector<uint8_t> before(vm_base + particleBase, vm_base + particleEnd);
        const auto blocks = particleBlocks, holes = particleFree;
        const auto cursor = particleCursor, random = ::randomState;
        cpu.gpr[3] = layer;
        cpu.fpr[1] = seconds;
        moyou_rope_func_02031494(&cpu); drain(&cpu);
        const std::vector<uint8_t> original(vm_base + particleBase, vm_base + particleEnd);
        const auto originalBlocks = particleBlocks, originalHoles = particleFree;
        const auto originalCursor = particleCursor, originalRandom = ::randomState;
        std::memcpy(vm_base + particleBase, before.data(), before.size());
        particleBlocks = blocks; particleFree = holes; particleCursor = cursor; ::randomState = random;
        particleLayerFrame(layer);
        reportDifference("particle frame", vm_base + particleBase, original.data(), original.size());
        for (size_t i = 0, shown = 0; i + 4 <= original.size() && shown < 4; i += 4)
            if (std::memcmp(vm_base + particleBase + i, original.data() + i, 4)) {
                const uint32_t address = uint32_t(particleBase + i);
                const uint32_t a = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(original.data() + i));
                const uint32_t b = vm_read32(address);
                std::fprintf(stderr, "  %08x (layer%+d system%+d): original %08x %.9g native %08x %.9g\n", address,
                    int(address - layer), int(address - system), a, std::bit_cast<float>(a), b, std::bit_cast<float>(b));
                ++shown;
            }
        if (particleBlocks != originalBlocks || particleFree != originalHoles || particleCursor != originalCursor)
            std::fprintf(stderr, "native particle frame: 1 bytes differ (allocator)\n");
        if (::randomState != originalRandom)
            std::fprintf(stderr, "native particle frame: 4 bytes differ (random state %u vs %u)\n", ::randomState, originalRandom);
    } else particleLayerFrame(layer);
    ++ticks;
    // 0x02031f44 → 0x0205de2c(system, camera FOV): the field takes the camera's FOV
    // (0x02023ed8 reads camera+0x20; 0.925 on a running XMB), then the PPU update path runs.
    writeFloat(system + 0x1358, readFloat(vm_read32(layer + 0xb0) + 0x20));
    if (std::getenv("MOYOU_PARTICLES") && !std::strcmp(std::getenv("MOYOU_PARTICLES"), "original")) {
        cpu.gpr[3] = system;
        moyou_rope_func_0205DDC8(&cpu); drain(&cpu);   // PPU update path
    } else if (std::getenv("MOYOU_VERIFY_NATIVE")) {
        // Run the original on a copy of the particle arena, then the native path from the
        // same state, and report the first difference.
        std::vector<uint8_t> before(vm_base + particleBase, vm_base + particleEnd);
        cpu.gpr[3] = system;
        moyou_rope_func_0205DDC8(&cpu); drain(&cpu);
        std::vector<uint8_t> original(vm_base + particleBase, vm_base + particleEnd);
        std::memcpy(vm_base + particleBase, before.data(), before.size());
        particleUpdate(system);
        const auto* now = vm_base + particleBase;
        size_t differences = 0, first = 0;
        for (size_t i = 0; i < original.size(); ++i)
            if (now[i] != original[i])
                { if (!differences++) first = i; }
        static unsigned frame = 0;
        ++frame;
        if (differences) {
            std::fprintf(stderr, "native particles frame %u: %zu bytes differ, first at %08zx\n",
                frame, differences, first + particleBase);
            const uint32_t array = vm_read32(vm_read32(system + 0xa4) + 0x10), vertices = vm_read32(system + 0x1700);
            unsigned shown = 0;
            for (size_t i = 0; i < original.size() && shown < 6; i += 4)
                if (std::memcmp(now + i, original.data() + i, 4)) {
                    const uint32_t address = uint32_t(i + particleBase);
                    uint32_t a, b; std::memcpy(&a, original.data() + i, 4); std::memcpy(&b, now + i, 4);
                    a = __builtin_bswap32(a); b = __builtin_bswap32(b);
                    std::fprintf(stderr, "  %08x particle %d field %+d: original %08x %.9g native %08x %.9g\n", address,
                        address >= array ? int((address - array) / 0x30) : -1, address >= array ? int((address - array) % 0x30) : int(address) - int(vertices),
                        a, std::bit_cast<float>(a), b, std::bit_cast<float>(b));
                    ++shown;
                }
        }
        else if (frame % 600 == 0) std::fprintf(stderr, "native particles frame %u: identical\n", frame);
    } else particleUpdate(system);
    return vm_read32(system + 0x16fc);
}

// Draw setup 0x0205c254: each uniform is a vector or matrix taken from the system's live
// parameters (system+0x1340..0x1444), the life bounds and the qgl modelview's cached inverse;
// the setters (0x0205bec0, 0x020409ec, ...) only store it. Nothing is set while no particle is
// alive or the system is hidden (+0x1704). The draw submission itself belongs to the renderer.
RopeSolver::ParticleUniforms RopeSolver::particleUniforms(uint32_t system) {
    ParticleUniforms uniforms;
    const auto vector = [&](uint32_t address) { Vector v; for (unsigned i = 0; i < 4; ++i) v[i] = readFloat(address + i * 4); return v; };
    const auto field = [&](uint32_t offset) { return readFloat(system + offset); };
    const bool drawn = vm_read32(system + 0x16fc) && !vm_read8(system + 0x1704);
    for (const char* name : {"_ParticleSize", "_Transparency", "_Focus", "_FocusCurves", "_FrontFacingQuaternion", "_NearControl",
                             "_LifeBoundsMin", "_LifeBoundsMax", "_Darkness", "_Glare"}) uniforms.vertex[name] = {};
    for (const char* name : {"_Color", "_NearControl", "_Glare", "_Gamma", "_IridescentExp"}) uniforms.fragment[name] = {};
    if (drawn) {
        uniforms.camera = inverseCameraView();   // 0x0204081c: the modelview stack top's inverse
        for (unsigned c = 0; c < 3; ++c) for (unsigned r = 0; r < 3; ++r)
            if (uniforms.camera[c * 4 + r] != (c == r ? 1.0f : 0.0f)) throw std::runtime_error("Unadapted original camera rotation");
        // Light pack 0x0203c17c: camera position, light direction | intensity, second direction, colour terms.
        const Matrix light{uniforms.camera[12], uniforms.camera[13], uniforms.camera[14], uniforms.camera[15],
                           field(0x13c0), field(0x13c4), field(0x13c8), field(0x13e0),
                           field(0x13d0), field(0x13d4), field(0x13d8), 0.0f,
                           field(0x13e8), field(0x13e4), field(0x13ec), field(0x13f4)};
        uniforms.lightPack = uniforms.fragmentLightPack = light;
        auto& v = uniforms.vertex;
        v["_ParticleSize"] = {field(0x1400), field(0x1404), field(0x1408), 0};
        v["_Darkness"] = {field(0x1418), field(0x142c), 0, 0};
        v["_Transparency"] = {field(0x13f0), field(0x13fc), 0, 0};
        v["_Focus"] = {field(0x140c), field(0x140c) + field(0x1410), field(0x1420), field(0x1420) + field(0x1424)};
        v["_FrontFacingQuaternion"] = {0, 0, 0, 1};
        v["_NearControl"] = {field(0x1434), field(0x141c), field(0x1430), 0};
        v["_LifeBoundsMin"] = vector(system + 0x900);
        v["_LifeBoundsMax"] = vector(system + 0x910);
        v["_FocusCurves"] = {field(0x1414), field(0x1428), field(0x1358), 0};
        v["_Glare"] = {field(0x1438), field(0x143c), field(0x1440), field(0x1444)};
        auto& f = uniforms.fragment;
        f["_Color"] = vector(system + 0x1340);
        f["_Gamma"] = {field(0x1350), 0, 0, 0};
        f["_NearControl"] = v["_NearControl"];
        f["_Glare"] = v["_Glare"];
        f["_IridescentExp"] = {field(0x13f8), 0, 0, 0};
    }
    uniforms.passes = (vm_read32(system + 0x1484) + 1) & 3;
    if (!verifyNative()) return uniforms;
    const auto original = particleUniformsOriginal(system);
    const auto compare = [](const char* name, const auto& a, const auto& b) { reportDifference(name, a.data(), b.data(), sizeof a); };
    compare("uniform camera", uniforms.camera, original.camera);
    compare("uniform light pack", uniforms.lightPack, original.lightPack);
    compare("uniform fragment light pack", uniforms.fragmentLightPack, original.fragmentLightPack);
    for (const auto* maps : {&uniforms.vertex, &uniforms.fragment}) {
        const auto& other = maps == &uniforms.vertex ? original.vertex : original.fragment;
        if (maps->size() != other.size()) std::fprintf(stderr, "native uniforms: 1 bytes differ (names)\n");
        for (const auto& [name, value] : *maps) {
            const auto found = other.find(name);
            if (found == other.end()) std::fprintf(stderr, "native uniform %s: 1 bytes differ (missing)\n", name.c_str());
            else compare(("uniform " + name).c_str(), value, found->second);
        }
    }
    if (uniforms.passes != original.passes) std::fprintf(stderr, "native uniform passes: 1 bytes differ\n");
    return uniforms;
}

RopeSolver::ParticleUniforms RopeSolver::particleUniformsOriginal(uint32_t system) {
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

unsigned RopeSolver::checkExp(unsigned samples) {
    auto& cpu = state_->cpu;
    unsigned differences = 0;
    uint32_t seed = 777;
    const auto check = [&](float x) {
        if (std::isinf(x) || x > 88.0f || x < -87.0f) return;   // overflow/underflow raise FP exceptions (unadapted)
        cpu.fpr[1] = x;
        firmwareMath(&cpu, moyou_math_func_00092D34);
        const float original = float(cpu.fpr[1]), native = firmwareExp(x);
        if (std::bit_cast<uint32_t>(original) != std::bit_cast<uint32_t>(native) && !(original != original && native != native))
            if (differences++ < 8) std::fprintf(stderr, "expf(%a): original %a native %a\n", x, original, native);
    };
    for (float x : {0.0f, -0.0f, 1e-30f, -1e-30f, 2e-8f, -2e-8f, 1.0f, -1.0f, 87.0f, -86.9f}) check(x);
    for (unsigned i = 0; i < samples; ++i) {
        seed = seed * 1664525u + 1013904223u;
        check(float(int32_t(seed)) / float(1u << (seed % 31)) * 1e-2f);
    }
    return differences;
}

unsigned RopeSolver::checkSinCos(unsigned samples) {
    // Native _FSin against the math module's for random bit patterns plus edge cases.
    auto& cpu = state_->cpu;
    unsigned differences = 0;
    uint32_t seed = 12345;
    const auto check = [&](float x, int which) {
        if (std::isinf(x)) return;   // the module raises an FP exception there (unadapted)
        cpu.fpr[1] = x; cpu.gpr[4] = uint64_t(which);
        firmwareMath(&cpu, moyou_math_func_00095F60);
        const float original = float(cpu.fpr[1]), native = firmwareSinCos(x, which);
        if (std::bit_cast<uint32_t>(original) != std::bit_cast<uint32_t>(native) && !(original != original && native != native)) {
            if (differences++ < 8) std::fprintf(stderr, "_FSin(%a, %d): original %a native %a\n", x, which, original, native);
        }
    };
    for (float x : {0.0f, -0.0f, 1e-40f, -1e-40f, 40.7f, -40.7f, 40.700001f, 1e6f, -1e9f, 3.4e38f,
                    std::bit_cast<float>(0x7fc00001u)})
        for (int which : {0, 1}) check(x, which);
    for (unsigned i = 0; i < samples; ++i) {
        seed = seed * 1664525u + 1013904223u;
        // Mostly moderate magnitudes (the scene's phases), some arbitrary bit patterns.
        const float x = i % 4 ? float(int32_t(seed)) / float(1u << (seed % 31)) : std::bit_cast<float>(seed);
        check(x, int(i & 1));
    }
    return differences;
}

namespace {
// Clip coordinates of a point (column-major matrix) and the cursor falloff at its screen position.
std::array<float, 4> project(const RopeSolver::Matrix& m, float x, float y, float z) {
    std::array<float, 4> r{};
    for (unsigned row = 0; row < 4; ++row) r[row] = m[row] * x + m[4 + row] * y + m[8 + row] * z + m[12 + row];
    return r;
}
float falloff(const std::array<float, 4>& clip, float x, float y, float aspect, float radius) {
    if (clip[3] <= 1e-4f) return 0;
    const float u = (clip[0] / clip[3] - x) * aspect, v = clip[1] / clip[3] - y;
    return std::exp(-(u * u + v * v) / (radius * radius));
}
}

void RopeSolver::nudgeRope(const Matrix& clip, const Matrix& modelFromClip, float x, float y, float dx, float dy,
                           float aspect, float radius, float strength) {
    // Velocities are array 6 (see reset); a screen-space push becomes a model-space velocity.
    constexpr uint32_t velocities = arrays + 6 * arrayBytes;
    for (unsigned node = 0; node < 361; ++node) {
        const uint32_t p = arrays + node * 16, v = velocities + node * 16;
        const auto c = project(clip, readFloat(p), readFloat(p + 4), readFloat(p + 8));
        const float weight = falloff(c, x, y, aspect, radius) * strength;
        if (weight < 1e-5f) continue;
        // Clip-space displacement at this depth, mapped back through the inverse transform.
        const float cx = dx * c[3], cy = dy * c[3];
        for (unsigned i = 0; i < 3; ++i)
            writeFloat(v + i * 4, readFloat(v + i * 4) + weight * (modelFromClip[i] * cx + modelFromClip[4 + i] * cy));
    }
}

void RopeSolver::nudgeParticles(uint32_t system, const Matrix& clip, float x, float y, float dx, float dy,
                                float aspect, float radius, float strength) {
    // Particle records (0x30 bytes): position+life at +0, velocity at +0x10; life -666 is dead.
    const uint32_t array = vm_read32(vm_read32(system + 0xa4) + 0x10), count = vm_read32(vm_read32(system + 0xa4) + 0x14);
    // The camera has no rotation (eye on +z looking down -z), so a screen push at clip depth w
    // is a world displacement of (dx·w/P00, dy·w/P11, 0).
    const float sx = 1 / clip[0], sy = 1 / clip[5];
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t p = array + i * 0x30, v = p + 0x10;
        if (readFloat(p + 12) < 0) continue;
        const auto c = project(clip, readFloat(p), readFloat(p + 4), readFloat(p + 8));
        const float weight = falloff(c, x, y, aspect, radius) * strength;
        if (weight < 1e-5f) continue;
        writeFloat(v, readFloat(v) + weight * dx * c[3] * sx);
        writeFloat(v + 4, readFloat(v + 4) + weight * dy * c[3] * sy);
    }
}

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

// 0x0204bad0 (stripe LUT 0x2001, 16 x 4) and 0x0204b928 (encode LUT 0x2002, 4096 x 1).
std::array<RopeSolver::Texture, 2> RopeSolver::textures() {
    const auto clampByte = [](float v) { return uint8_t(int32_t(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v)); };
    Texture stripes{16, 4, std::vector<uint8_t>(16 * 4 * 4)};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 16; ++column) {
            const float j = float(row) / 3.0f;
            const float u = float(column) / float(15) - 0.5f;
            const float exponent = ((j * 20.0f) * j) * j;
            const float brightness = std::fma(j, 0.5f, 0.5f);
            const float base = std::fma(u * -4.0f, u, 1.00999999f);
            float value = float(std::pow(double(base), double(exponent)) * double(brightness));
            if (value > 1.0f) value = 1.0f;
            const auto byte = uint8_t(int32_t(value * 255.0f));
            for (unsigned channel = 0; channel < 4; ++channel) stripes.rgba[(row * 16 + column) * 4 + channel] = byte;
        }
    Texture encode{4096, 1, std::vector<uint8_t>(4096 * 4)};
    for (unsigned i = 0; i < 4096; ++i) {
        const double scaled = double((float(int32_t(i)) / 4095.0f) * 32.0f * 4.0f);
        double whole;
        const float fraction = float(std::modf(scaled, &whole));
        const uint8_t low = clampByte((fraction * 0.25f) * 255.0f), high = clampByte((float(whole) * 0.015625f) * 255.0f);
        encode.rgba[i * 4 + 0] = low; encode.rgba[i * 4 + 1] = high;
        encode.rgba[i * 4 + 2] = low; encode.rgba[i * 4 + 3] = high;
    }
    std::array<Texture, 2> native{stripes, encode};
    if (!verifyNative()) return native;
    const auto original = texturesOriginal();
    for (unsigned i = 0; i < 2; ++i) {
        if (original[i].width != native[i].width || original[i].height != native[i].height || original[i].rgba.size() != native[i].rgba.size())
            std::fprintf(stderr, "native textures: %u bytes differ (shape)\n", 1u);
        else reportDifference(i ? "encode LUT" : "stripe LUT", native[i].rgba.data(), original[i].rgba.data(), native[i].rgba.size());
    }
    return native;
}

std::array<RopeSolver::Texture, 2> RopeSolver::texturesOriginal() {
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

namespace {
// Half float as the exposure update packs it (0x02071860): truncated mantissa, exponents
// below the half range flush to +0, above it clamp to 31.
uint16_t exposureHalf(float value) {
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const int32_t exponent = int32_t((bits >> 23) & 0xff) - 112;
    if (!bits || exponent < 0) return 0;
    return uint16_t(((bits >> 16) & 0x8000) | uint32_t(std::min(exponent, 31)) << 10 | ((bits >> 13) & 0x3ff));
}
float halfToFloat(uint16_t h) {
    uint32_t word = uint32_t(h & 0x8000) << 16;
    auto exponent = (h >> 10) & 31, mantissa = h & 1023;
    if (exponent == 31) word |= 0x7f800000 | (mantissa << 13);
    else if (exponent) word |= uint32_t(exponent + 112) << 23 | (mantissa << 13);
    else if (mantissa) {
        int shift = 0;
        while (!(mantissa & 1024)) { mantissa <<= 1; ++shift; }
        word |= uint32_t(113 - shift) << 23 | ((mantissa & 1023) << 13);
    }
    return std::bit_cast<float>(word);
}
}

// Exposure update 0x02071794: tone curve L = x(1 + x/w²)/(1 + x) over x = i/127·16·exposure,
// and glare G = L/clamp(L,0,1)·(L − threshold)/8, packed as halves into the 128 x 128 RG
// lookups 0x3001 (L[y], L[x]) and 0x3002 (G[max(x,y)], L[x]); the 32 x 32 dither noise
// (exposureNoise) advances the RNG word every call.
RopeSolver::ExposureTables RopeSolver::exposure(float value, float white, float threshold, float gamma, float dither) {
    const std::array<float, 5> inputs{value, white, threshold, gamma, dither};
    if (exposureCache_ && exposureInputs_ == inputs && !verifyNative()) {
        ExposureTables tables = *exposureCache_;
        tables.noise.rgba = exposureNoise();
        return tables;
    }
    writeFloat(toneObject + 0x29c, value);
    writeFloat(toneObject + 0x2a0, white);
    writeFloat(toneObject + 0x2a8, threshold);
    writeFloat(toneObject + 0x2e0, gamma);
    writeFloat(toneObject + 0x2dc, dither);
    std::array<uint16_t, 128> tone{}, glare{};
    const float inverseWhite = 1.0f / (white * white);
    for (unsigned i = 0; i < 128; ++i) {
        const float x = ((float(int32_t(i)) / 127.0f) * 16.0f) * value;
        const float shoulder = 1.0f / (x + 1.0f);
        const float L = shoulder * (x * std::fma(inverseWhite, x, 1.0f));
        tone[i] = exposureHalf(L * gamma);
        const float clamped = L < 0.0f ? 0.0f : L > 1.0f ? 1.0f : L;
        // 0/0 at i = 0: PowerPC's default NaN is positive, x86's negative.
        float ratio = L / clamped;
        if (ratio != ratio && L == L && clamped == clamped) ratio = std::bit_cast<float>(0x7fc00000u);
        glare[i] = exposureHalf((ratio * (L - threshold)) * 0.125f);
    }
    ExposureTables tables;
    for (unsigned table = 0; table < 2; ++table) {
        auto& output = tables.rgba[table];
        output.assign(128 * 128 * 4, 0.0f);
        for (unsigned y = 0; y < 128; ++y)
            for (unsigned x = 0; x < 128; ++x) {
                const unsigned pixel = y * 128 + x;
                // Original G16R16F samplers expose the transposed RG lookup through XY, and the
                // blue/glare lookup through ZW.
                if (table == 0) {
                    output[pixel * 4 + 1] = halfToFloat(tone[y]);
                    output[pixel * 4 + 0] = halfToFloat(tone[x]);
                    output[pixel * 4 + 3] = 1;
                } else {
                    output[pixel * 4 + 3] = halfToFloat(glare[std::max(x, y)]);
                    output[pixel * 4 + 2] = halfToFloat(tone[x]);
                }
            }
    }
    const auto state = vm_read32(toneObject + 0x150);
    tables.noise = {32, 32, exposureNoise()};
    if (verifyNative()) {
        const auto after = vm_read32(toneObject + 0x150);
        vm_write32(toneObject + 0x150, state);
        const auto original = exposureOriginal(value, white, threshold, gamma, dither);
        for (unsigned table = 0; table < 2; ++table)
            reportDifference(table ? "exposure glare table" : "exposure tone table", tables.rgba[table].data(),
                original.rgba[table].data(), tables.rgba[table].size() * sizeof(float));
        reportDifference("exposure noise", tables.noise.rgba.data(), original.noise.rgba.data(), tables.noise.rgba.size());
        if (vm_read32(toneObject + 0x150) != after) std::fprintf(stderr, "native exposure: 4 bytes differ (RNG word)\n");
    }
    exposureCache_ = std::make_unique<ExposureTables>(tables);
    exposureInputs_ = inputs;
    return tables;
}

RopeSolver::ExposureTables RopeSolver::exposureOriginal(float value, float white, float threshold, float gamma, float dither) {
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
    return tables;
}

bool RopeSolver::checkExposureNoise(unsigned frames) {
    for (unsigned frame = 0; frame < frames; ++frame) {
        const auto saved = vm_read32(toneObject + 0x150);
        const auto native = exposureNoise();
        vm_write32(toneObject + 0x150, saved);
        exposureCache_.reset();
        const auto original = exposureOriginal(exposureInputs_[0], exposureInputs_[1], exposureInputs_[2], exposureInputs_[3], exposureInputs_[4]);
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
    // 0x02071384 → 0x02071260 per channel: radius ⌊3σ⌋ − 1, Gaussian taps (0x02070cec)
    // e^(−x²/2σ²) / (σ·√2π) at x = t − radius, summed in order and normalised.
    GlareWeights result;
    std::array<std::array<float, 16>, 3> weights{};
    std::array<int32_t, 3> radius{};
    for (unsigned channel = 0; channel < 3; ++channel) {
        const float sigma = radii[channel];
        radius[channel] = int32_t(std::floor(sigma * 3.0f)) - 1;
        if (radius[channel] < 0 || radius[channel] >= 8) throw std::runtime_error("Original Gaussian exceeds its shader tap count");
        float sum = 0;
        const int32_t count = radius[channel] * 2 + 1;
        for (int32_t t = 0; t < count; ++t) {
            const float x = float(t) - float(radius[channel]);
            const float g = firmwareExp((-x * x) / (sigma * (sigma + sigma))) / (sigma * 2.50662827f);
            weights[channel][t] = g;
            sum = sum + g;
        }
        for (int32_t t = 0; t < count; ++t) weights[channel][t] = weights[channel][t] / sum;
        for (int32_t tap = 0; tap <= radius[channel]; ++tap) result.taps[tap][channel] = weights[channel][radius[channel] + tap];
    }
    if (verifyNative()) {
        auto& cpu = state_->cpu;
        cpu.gpr[3] = toneObject;
        moyou_rope_func_02071384(&cpu); drain(&cpu);
        for (unsigned channel = 0; channel < 3; ++channel) {
            const auto original = vm_read32(toneObject + 0x288 + channel * 4);
            std::array<float, 16> taps{};
            for (unsigned t = 0; t < 2 * original + 1 && t < 16; ++t) taps[t] = readFloat(toneObject + 0x1c8 + channel * 64 + t * 4);
            reportDifference("glare radius", &radius[channel], &original, 4);
            reportDifference("glare taps", weights[channel].data(), taps.data(), sizeof taps);
        }
    }
    auto& cpu = state_->cpu;
    float sum = 0;
    for (unsigned i = 0; i < 6; ++i) {
        cpu.fpr[1] = sumPower; cpu.fpr[2] = i;
        if (nativeLibm("pow")) {
            const double native = firmwarePowInteger(double(sumPower), i);
            if (verifyNative()) {
                firmwareMath(&cpu,moyou_math_func_00093F94);
                reportDifference("glare level pow", &native, &cpu.fpr[1], sizeof native);
            }
            cpu.fpr[1] = native;
        } else firmwareMath(&cpu,moyou_math_func_00093F94);
        result.levels[i] = static_cast<float>(cpu.fpr[1]);
        sum = static_cast<float>(double(sum) + cpu.fpr[1]);
    }
    for (auto& weight : result.levels) weight = static_cast<float>(double(weight) / sum) * level * level;
    return result;
}


namespace {
// ---- Native particle layer frame ------------------------------------------------------
V4 v4(float x, float y, float z, float w) { return V4{{x, y, z, w}}; }
V4 fmaLanes(const V4& a, const V4& b, const V4& c) { V4 r; for (unsigned i = 0; i < 4; ++i) r.v[i] = std::fma(a.v[i], b.v[i], c.v[i]); return r; }
V4 fmaLanes(const V4& a, float b, const V4& c) { V4 r; for (unsigned i = 0; i < 4; ++i) r.v[i] = std::fma(a.v[i], b, c.v[i]); return r; }
const V4 negativeZero = v4(-0.0f, -0.0f, -0.0f, -0.0f), positiveZero = v4(0.0f, 0.0f, 0.0f, 0.0f);
// Lane 0 of the firmware's horizontal sum of a·b products (+0 addend).
float dotSum(const V4& a, const V4& b) {
    float p[4];
    for (unsigned i = 0; i < 4; ++i) p[i] = std::fma(a.v[i], b.v[i], 0.0f);
    return (p[0] + p[1]) + (p[2] + p[3]);
}
float clampFirmware(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }  // 0x020030a0
float atLeast(float value, float minimum) { return value < minimum ? minimum : value; }     // 0x0202b46c

// 0x0202d6e8/0x0202d640: the emission grid vertex (u mod W, v mod H) through the grid matrix.
V4 sampleGrid(uint32_t layer, int32_t u, int32_t v) {
    const auto W = int32_t(vm_read32(layer + 0x44)), H = int32_t(vm_read32(layer + 0x48));
    int32_t uu = u % W, vv = v - (v / H) * H;
    if (uu < 0) uu += W;
    if (vv < 0) vv += H;
    const uint32_t grid = vm_read32(layer + 0x40);
    if (!grid) return positiveZero;
    const V4 vertex = loadV4(grid + uint32_t(uu + vv * W) * 32);
    float m[16];
    for (unsigned i = 0; i < 16; ++i) m[i] = readFloat(layer + 0x50 + i * 4);
    const auto r = transform(m, vertex.v);
    return v4(r[0], r[1], r[2], r[3]);
}

// 0x0202d780: std::vector<{float4, int u, int v}>::push_back (32-byte slots, 24 bytes copied;
// growth by half the capacity) with the firmware's new/delete in the particle arena.
void pushPoint(uint32_t vector, const V4& position, int32_t u, int32_t v) {
    const uint32_t begin = vm_read32(vector + 4), end = vm_read32(vector + 8), capacity = vm_read32(vector + 0xc);
    const auto put = [&](uint32_t at, const V4& p, uint32_t a, uint32_t b) { storeV4(at, p); vm_write32(at + 0x10, a); vm_write32(at + 0x14, b); };
    uint32_t bytes;
    if (begin) {
        const uint32_t size = uint32_t(int32_t(end - begin) >> 5), slots = uint32_t(int32_t(capacity - begin) >> 5);
        if (slots > size) { put(end, position, uint32_t(u), uint32_t(v)); vm_write32(vector + 8, end + 32); return; }
        if (size == 0x7ffffff) throw std::runtime_error("Original emission list too long");
        const uint32_t half = slots >> 1;
        const uint32_t grown = 0x7ffffffu - half >= slots ? slots + half : 0;
        bytes = (size + 1 > grown ? size + 1 : grown) << 5;
    } else bytes = 32;
    const uint32_t block = particleAllocate(bytes & ~31u, 16);
    uint32_t at = block;
    for (uint32_t source = begin; source != end; source += 32, at += 32)
        put(at, loadV4(source), vm_read32(source + 0x10), vm_read32(source + 0x14));
    put(at, position, uint32_t(u), uint32_t(v));
    uint32_t count = 1;
    if (begin) { particleRelease(begin); count = uint32_t(int32_t(end - begin) >> 5) + 1; }
    vm_write32(vector + 0xc, block + bytes);
    vm_write32(vector + 4, block);
    vm_write32(vector + 8, block + count * 32);
}

// Ring slot (last - back) mod period, as the firmware's divw/mullw remainder plus period.
int32_t ringSlot(int32_t last, int32_t back, int32_t period) {
    int32_t r = last - back;
    r -= (r / period) * period;
    return r < 0 ? r + period : r;
}
V4 withoutW(V4 a) { a.v[3] = 0.0f; return a; }
// VMX lane operations with the VSCR non-Java mode the firmware runs in: subnormal inputs and
// results become zero of the same sign (FPU code keeps them).
float flush(float x) { return (std::bit_cast<uint32_t>(x) & 0x7f800000u) ? x : std::copysign(0.0f, x); }
float vmadd(float a, float c, float b) {
    const float r = std::fma(flush(a), flush(c), flush(b));
    return r != r ? std::bit_cast<float>(0x7fc00000u) : flush(r);
}
float vadd(float a, float b) { return flush(flush(a) + flush(b)); }
V4 vmadd(const V4& a, const V4& c, const V4& b) { V4 r; for (unsigned i = 0; i < 4; ++i) r.v[i] = vmadd(a.v[i], c.v[i], b.v[i]); return r; }
V4 vmadd(float a, const V4& c, const V4& b) { return vmadd(v4(a, a, a, a), c, b); }
V4 vscale(const V4& a, float f) { return vmadd(a, v4(f, f, f, f), negativeZero); }
V4 vsub(const V4& a, const V4& b) { V4 r; for (unsigned i = 0; i < 4; ++i) r.v[i] = flush(flush(a.v[i]) - flush(b.v[i])); return r; }
// dot0 / ropeLength / ropeInverse in flushed lanes.
float vdot(const V4& a) {
    float p[4];
    for (unsigned i = 0; i < 4; ++i) p[i] = vmadd(a.v[i], a.v[i], 0.0f);
    return vadd(vadd(p[0], p[1]), vadd(p[2], p[3]));
}
float vlength(float s) {
    s = flush(s);
    const float e = estimate(s), t = vmadd(e, s, 0.0f), h = vmadd(t, 0.5f, 0.0f);
    if (s == 0 || e == 0) return s;
    return vmadd(flush(-std::fma(e, t, -1.0f)), h, t);
}
float vinverse(float s) {
    s = flush(s);
    const float e = estimate(s), t = vmadd(e, s, 0.0f), h = vmadd(e, 0.5f, 0.0f);
    const float r = vmadd(flush(-std::fma(e, t, -1.0f)), h, e);
    return r == r ? r : e;
}
// transform() in flushed lanes.
V4 vtransform(const float* m, const V4& x) {
    V4 r;
    for (unsigned i = 0; i < 4; ++i) {
        float t = vmadd(x.v[1], m[4 + i], -0.0f);
        t = vmadd(x.v[0], m[i], t);
        t = vmadd(x.v[2], m[8 + i], t);
        r.v[i] = vmadd(x.v[3], m[12 + i], t);
    }
    return r;
}

// 0x0202bcbc: shake detector for one axis of the motion history (X+0 selects x/y/z). Fits a
// line to the last 16 deltas and one to their running sum; a sustained residual above the
// threshold (live+0x108) flips the shake sign and kicks X+0x410.
void shakeDetect(uint32_t X, uint32_t h, uint32_t d, uint32_t live) {
    const auto component = int32_t(vm_read32(X));
    {
        int32_t last = int32_t(vm_read32(d + 0x180));
        const auto period = int32_t(vm_read32(d + 0x100));
        for (unsigned i = 0; i < 16; ++i, --last) {
            int32_t slot = last - (last / period) * period;
            if (slot < 0) slot += period;
            vm_write32(X + 4 + i * 4, vm_read32(d + uint32_t(slot) * 16 + uint32_t(component) * 4));
        }
    }
    float y[16], sum[16], fit[16];
    for (unsigned i = 0; i < 16; ++i) y[i] = readFloat(X + 4 + i * 4);
    {
        float n = 0, sx = 0, sy = 0, sxy = 0, sxx = 0;
        for (unsigned i = 0; i < 16; ++i) {
            n += 1.0f;
            const float x = float(i);
            sy += y[i]; sx += x; sxy = std::fma(x, y[i], sxy); sxx = std::fma(x, x, sxx);
        }
        float slope = 0, intercept = 0;
        if (n != 0) {
            const float a = sxx * n, b = sxy * n, c = sy * sxx;
            const float den = std::fma(sx, sx, -a);
            slope = std::fma(sy, sx, -b) / den;
            intercept = std::fma(sxy, sx, -c) / den;
        }
        writeFloat(X + 0x280, slope);
        writeFloat(X + 0x284, intercept);
        const uint32_t k = vm_read32(X + 0x384);
        vm_write32(X + 0x380, k);
        writeFloat(X + 0x300 + k * 4, intercept);
        vm_write32(X + 0x384, (k + 1) % 5);
    }
    {
        float running = 0;
        for (unsigned i = 0; i < 16; ++i) sum[i] = running = running + y[i];
        float n = 0, sx = 0, sy = 0, sxy = 0, sxx = 0;
        for (unsigned i = 0; i < 16; ++i) {
            n += 1.0f;
            const float x = float(i);
            sy += sum[i]; sx += x; sxy = std::fma(x, sum[i], sxy); sxx = std::fma(x, x, sxx);
        }
        float intercept = 0, slope = 0;
        if (n != 0) {
            const float c = sxx * sy, b = sxy * n, a = sxx * n;
            const float den = std::fma(sx, sx, -a);
            intercept = std::fma(sx, sxy, -c) / den;
            slope = std::fma(sx, sy, -b) / den;
        }
        for (unsigned i = 0; i < 16; ++i) fit[i] = std::fma(slope, float(i), intercept);
    }
    float residual = 0;
    for (unsigned i = 0; i < 16; ++i) residual += std::fabs(sum[i] - fit[i]);
    writeFloat(X + 0x44, residual);
    const uint32_t ring = X + 0x80;
    const auto k = int32_t(vm_read32(ring + 0x184));
    vm_write32(ring + 0x180, uint32_t(k));
    vm_write32(ring + 0x184, uint32_t(k + 1) & 63);
    const auto period = int32_t(vm_read32(ring + 0x100));
    writeFloat(ring + uint32_t(k) * 4, residual);
    float kick = readFloat(X + 0x410) * 0.600000024f;
    writeFloat(X + 0x410, kick);
    float recent = 0;
    for (int32_t j = 0; j < 3; ++j) recent += readFloat(ring + uint32_t(ringSlot(k, j, period)) * 4);
    const float level = atLeast(0.0f, std::fabs(recent / 3.0f) - readFloat(live + 0x108));
    writeFloat(X + 0x400, level);
    if (level != 0 && readFloat(X + 0x404) == 0) {
        const float w = readFloat(h + uint32_t(ringSlot(int32_t(vm_read32(h + 0x180)), 0, int32_t(vm_read32(h + 0x100)))) * 16 + 12);
        const float sign = w > 0.0f ? -1.0f : 1.0f;
        const float previous = readFloat(X + 0x408);
        const float signedLevel = sign * readFloat(X + 0x400);
        writeFloat(X + 0x408, sign);
        const float turn = previous * sign;
        kick = signedLevel * 10.0f;
        writeFloat(X + 0x410, kick);
        if (turn < 0.0f && vm_read32(X + 0x414) <= 0x77) writeFloat(X + 0x410, kick + kick);
        vm_write32(X + 0x414, 0);
        kick = readFloat(X + 0x410);
    }
    writeFloat(X + 0x40c, kick);
    writeFloat(X + 0x404, level);
    vm_write32(X + 0x414, vm_read32(X + 0x414) + 1);
}

// 0x0202c27c: one shake axis, its spring (X+0x41c position, +0x420 velocity, +0x424 push) and
// the push it adds along the view's up column.
V4 shakeAxis(uint32_t X, uint32_t h, uint32_t d, uint32_t spring, const V4& column, uint32_t live) {
    shakeDetect(X, h, d, live);
    const float kick = readFloat(X + 0x40c);
    if (std::fabs(kick) > 0.0f) {
        writeFloat(X + 0x424, std::fma(kick, readFloat(live + 0x100), readFloat(X + 0x424)));
        writeFloat(spring + 8, std::fabs(readFloat(X + 0x40c) * readFloat(live + 0x104)) + readFloat(spring + 8));
    }
    const float x = readFloat(X + 0x41c), velocity = readFloat(X + 0x420), push = readFloat(X + 0x424);
    writeFloat(X + 0x424, 0.0f);
    const float v = std::fma(-velocity, 0.200000003f, push) + velocity;
    writeFloat(X + 0x420, v);
    writeFloat(X + 0x41c, v + x);
    const float amount = clampFirmware(readFloat(X + 0x420), -1.0f, 1.0f);
    writeFloat(X + 0x418, amount);
    return vscale(vscale(column, amount), readFloat(live + 0xfc));
}

V4 cameraField(uint32_t camera, uint32_t offset) { return loadV4(camera + offset); }

// 0x0202c3e0: two shake axes (layer+0x700, layer+0xb80) push the flow offset (system+0x980).
void particleShake(uint32_t layer) {
    const uint32_t system = vm_read32(layer + 0x18), live = system + 0x1360, camera = vm_read32(layer + 0xb0);
    const V4 eye = cameraField(camera, 0x60), target = cameraField(camera, 0x70), up = cameraField(camera, 0x80);
    const auto view = lookAt(eye.v, target.v, up.v);
    const V4 column = v4(view[4], view[5], view[6], view[7]);
    const uint32_t h = layer + 0x200, d = layer + 0x400, spring = layer + 0x1d8;
    const V4 first = shakeAxis(layer + 0x700, h, d, spring, column, live);
    storeV4(system + 0x980, vmadd(readFloat(live + 0x10c), first, loadV4(system + 0x980)));
    const V4 second = shakeAxis(layer + 0xb80, h, d, spring, column, live);
    storeV4(system + 0x980, vmadd(readFloat(live + 0x110), second, loadV4(system + 0x980)));
}

// 0x0202c758: length of the newest motion delta, and that times live+0xec (layer+0x600/0x604).
void particleMotion(uint32_t layer) {
    const uint32_t d = layer + 0x400;
    const auto slot = ringSlot(int32_t(vm_read32(d + 0x180)), 0, int32_t(vm_read32(d + 0x100)));
    const float length = vlength(vdot(withoutW(loadV4(d + uint32_t(slot) * 16))));
    writeFloat(layer + 0x600, length);
    writeFloat(layer + 0x604, length * readFloat(vm_read32(layer + 0x18) + 0x144c));
}

// 0x0202d02c: the camera tilt. A pending D-pad direction (layer+0x1c0: 0 left, 1 right, 2 up,
// 3 down, 4 none) pushes the tilt spring (+0x198/+0x19c/+0x1a0) and the weave spring
// (+0x1e0), and sets the tilt axis (+0x1b0) in view space; the tilt adds to the flow offset
// (system+0x980), whose length rotates the field (system+0x940). Eye minus twice the view
// direction goes to system+0x930.
void particleTilt(uint32_t layer) {
    const uint32_t system = vm_read32(layer + 0x18), live = system + 0x1360, camera = vm_read32(layer + 0xb0);
    const V4 eye = cameraField(camera, 0x60), target = cameraField(camera, 0x70), up = cameraField(camera, 0x80);
    V4 direction = withoutW(vsub(eye, target));
    direction = vscale(direction, vinverse(vdot(direction)));
    const auto view = lookAt(eye.v, target.v, up.v);
    storeV4(system + 0x930, vsub(eye, vscale(direction, 2.0f)));
    const auto mode = int32_t(vm_read32(layer + 0x1c0));
    if (mode != 4) {
        static constexpr float directions[4][2] = {{-1, 0}, {1, 0}, {0, 1}, {0, -1}};
        if (mode < 0 || mode > 3) throw std::runtime_error("Unadapted original particle tilt direction");
        const float dx = directions[mode][0], dy = directions[mode][1];
        const V4 local = v4(-dy, dx, 0, 0);
        const V4 axis = vtransform(view.data(), local);
        const float across = readFloat(live + 0xf8), along = readFloat(live + 0xf4);
        storeV4(layer + 0x1b0, vmadd(v4(across, along, 1.0f, 0.0f), axis, negativeZero));
        writeFloat(layer + 0x1a0, readFloat(layer + 0x190) + readFloat(layer + 0x1a0));
        const float p0 = vmadd(across, -dy, 0.0f), p1 = vmadd(along, dx, 0.0f);
        const float amount = vadd(vadd(p0, p1), vadd(0.0f, 0.0f));
        writeFloat(layer + 0x1e0, std::fma(readFloat(layer + 0x1d0), std::fabs(amount), readFloat(layer + 0x1e0)));
    }
    const float x = readFloat(layer + 0x198), velocity = readFloat(layer + 0x19c), push = readFloat(layer + 0x1a0);
    writeFloat(layer + 0x1a0, 0.0f);
    const float v = std::fma(-velocity, 0.0199999996f, push) + velocity;
    writeFloat(layer + 0x19c, v);
    writeFloat(layer + 0x198, v + x);
    const float tilt = clampFirmware(readFloat(layer + 0x19c), 0.0f, 1.0f);
    writeFloat(layer + 0x194, tilt);
    const V4 offset = vmadd(vmadd(readFloat(live + 0xf0), loadV4(layer + 0x1b0), negativeZero),
                            v4(tilt, tilt, tilt, tilt), loadV4(system + 0x980));
    storeV4(system + 0x980, offset);
    const float s = vdot(withoutW(offset));
    const float length = vlength(s);
    if (length >= 9.99999975e-06f) {
        const V4 axis = vscale(offset, vinverse(s));
        const auto R = axisRotation({axis.v[0], axis.v[1], axis.v[2]}, length);
        for (unsigned c = 0; c < 4; ++c) storeV4(system + 0x940 + c * 16, v4(R[c][0], R[c][1], R[c][2], R[c][3]));
    } else {
        for (unsigned c = 0; c < 4; ++c) storeV4(system + 0x940 + c * 16, v4(c == 0, c == 1, c == 2, c == 3));
    }
    vm_write32(layer + 0x1c0, 4);
    vm_write8(layer + 0x1c4, 0);
}

float qglRandom();
void particleEmit(uint32_t layer);

// ParticlesLayer frame 0x02031494 (after its grid set-up): history ring, the smoothed
// brightness, colour/intensity hand-off, simulation inputs, the flow-field decay, the camera
// frustum matrices and the emitter 0x02030b80, with the interaction handlers (tilt, motion,
// shake). Running tweens (the boot animations, with firmware callbacks) are not adapted.
void particleLayerFrame(uint32_t layer) {
    const uint32_t system = vm_read32(layer + 0x18), live = system + 0x1360, S = system + 0x100;
    storeV4(system + 0x980, positiveZero);
    {   // 0x0202b2dc
        const uint32_t h = layer + 0x200, d = layer + 0x400;
        const auto index = int32_t(vm_read32(h + 0x184)), period = int32_t(vm_read32(h + 0x100));
        if (period <= 0) throw std::runtime_error("Unadapted original particle history");
        vm_write32(h + 0x180, uint32_t(index));
        storeV4(h + uint32_t(index) * 16, loadV4(layer + 0x180));
        int32_t previous = (index - 1) % period, current = index % period;
        vm_write32(h + 0x184, uint32_t(index + 1) & 15);
        if (current < 0) current += period;
        if (previous < 0) previous += period;
        const auto slot = int32_t(vm_read32(d + 0x184));
        vm_write32(d + 0x184, uint32_t(slot + 1) & 15);
        vm_write32(d + 0x180, uint32_t(slot));
        storeV4(d + uint32_t(slot) * 16, sub(loadV4(h + uint32_t(current) * 16), loadV4(h + uint32_t(previous) * 16)));
    }
    if (vm_read8(layer + 0x1003)) particleTilt(layer);
    if (vm_read8(layer + 0x1004)) particleMotion(layer);
    if (vm_read8(layer + 0x1005)) particleShake(layer);
    {
        const float x = readFloat(layer + 0x1d8), velocity = readFloat(layer + 0x1dc), push = readFloat(layer + 0x1e0);
        writeFloat(layer + 0x1e0, 0.0f);
        float v = std::fma(-velocity, 0.400000006f, push);
        v = std::fma(0.0f - x, 0.00400000019f, v) + velocity;
        writeFloat(layer + 0x1dc, v);
        writeFloat(layer + 0x1d8, v + x);
        writeFloat(layer + 0x1d4, clampFirmware(readFloat(layer + 0x1d8), 0.0f, 1.0f));
    }
    if (vm_read8(layer + 0x110) || vm_read8(layer + 0x13c) || vm_read8(layer + 0x15c) || vm_read8(layer + 0x17c))
        throw std::runtime_error("Unadapted original particle animation");
    storeV4(system + 0x1340, loadV4(layer + 0xd0));
    writeFloat(system + 0x1350, readFloat(layer + 0x120));
    // Simulation inputs.
    writeFloat(S, 0.0f); writeFloat(S + 4, readFloat(live + 0x3c)); writeFloat(S + 8, 0.0f);
    const float friction = readFloat(live + 0x30);
    for (unsigned i = 0; i < 3; ++i) writeFloat(S + 16 + i * 4, friction);
    writeFloat(system + 0x9a8, readFloat(live + 0xa0));
    writeFloat(system + 0x9ac, readFloat(live + 0x34));
    const float step = readFloat(live + 0x38);
    storeV4(system + 0x9b0, v4(step, step, step, 1.0f));
    {
        const V4 wind = loadV4(live + 0x40);
        const float s = dotSum(v4(wind.v[0], wind.v[1], wind.v[2], 0.0f), v4(wind.v[0], wind.v[1], wind.v[2], 0.0f));
        if (0.0001f > ropeLength(s)) storeV4(system + 0x990, loadV4(layer + 0x680));
        else {
            const float inverse = ropeInverse(s), scaleWind = readFloat(live + 0x50);
            const V4 direction = fmaLanes(wind, inverse, negativeZero);
            storeV4(system + 0x990, fmaLanes(direction, scaleWind, loadV4(layer + 0x680)));
        }
    }
    writeFloat(system + 0x9a4, std::fma(readFloat(layer + 0x1d4), readFloat(live + 0xe8), readFloat(live + 0x54)) + readFloat(layer + 0x604));
    writeFloat(system + 0x9a0, readFloat(layer + 0xc0));
    {   // 0x0202c588: flow table decay
        const float decay = readFloat(layer + 0xbc);
        for (uint32_t entry = 0; entry < 512 * 3; ++entry) {
            float f = std::fma(float(int8_t(vm_read8(system + 0x180 + entry))), 0.00787401572f, -0.0f);
            f = vminf(vmaxf(std::fma(f, decay, -0.0f), -1.0f), 1.0f);
            const float scaled = std::fma(f, 127.0f, -0.0f);
            vm_write8(system + 0x180 + entry, uint8_t(scaled != scaled ? 0 : int32_t(scaled)));
        }
    }
    {   // Camera frustum at depth 9 (E at sys+0x800, inverse at sys+0x840) and bounds.
        const uint32_t camera = vm_read32(layer + 0xb0);
        float eye[4], target[4], up[4];
        for (unsigned i = 0; i < 4; ++i) { eye[i] = readFloat(camera + 0x60 + i * 4); target[i] = readFloat(camera + 0x70 + i * 4); up[i] = readFloat(camera + 0x80 + i * 4); }
        const auto view = lookAt(eye, target, up);
        const float tangent = std::tan(readFloat(camera + 0x20) * 0.5f);
        float height = tangent * 9.0f, width = height * readFloat(camera + 0x14);
        height = height + height; width = width + width;
        V4 back;
        for (unsigned i = 0; i < 3; ++i) back.v[i] = std::fma(9.0f, -view[8 + i], -0.0f);
        back.v[3] = 1.0f;
        const auto apply = [](const float* m, const V4& x) { const auto r = transform(m, x.v); return v4(r[0], r[1], r[2], r[3]); };
        float N[16];
        const V4 columns[4] = {apply(view.data(), v4(1, 0, 0, 0)), apply(view.data(), v4(0, 1, 0, 0)), apply(view.data(), v4(0, 0, 1, 0)), apply(view.data(), back)};
        for (unsigned c = 0; c < 4; ++c) for (unsigned i = 0; i < 4; ++i) N[c * 4 + i] = columns[c].v[i];
        const V4 A = apply(N, fmaLanes(v4(width, width, width, width), v4(1, 0, 0, 0), negativeZero));
        const V4 B = apply(N, fmaLanes(v4(height, height, height, height), v4(0, 1, 0, 0), negativeZero));
        const V4 C = apply(N, fmaLanes(v4(1, 1, 1, 1), v4(0, 0, 1, 0), negativeZero));
        const V4 D = apply(N, v4(0, 0, 0, 1));
        const V4 zero = positiveZero;
        V4 b0 = fmaLanes(B, zero, negativeZero), a0 = fmaLanes(A, zero, negativeZero), c0 = fmaLanes(C, zero, negativeZero);
        V4 col0 = add(A, b0), col2 = add(a0, b0), col1 = add(a0, B);
        const V4 halfB = fmaLanes(v4(-0.5f, -0.5f, -0.5f, -0.5f), B, negativeZero);
        col0 = add(col0, c0);
        const V4 d0 = fmaLanes(D, zero, negativeZero);
        col1 = add(col1, c0);
        col2 = add(col2, C);
        const V4 corner = fmaLanes(v4(-0.5f, -0.5f, -0.5f, -0.5f), A, halfB);
        col0 = add(col0, d0); col1 = add(col1, d0); col2 = add(col2, d0);
        const V4 shifted = fmaLanes(zero, C, corner);
        storeV4(system + 0x800, col0); storeV4(system + 0x810, col1); storeV4(system + 0x820, col2);
        storeV4(system + 0x830, fmaLanes(v4(1, 1, 1, 1), D, shifted));
        const auto inverse = inverseMatrix(system + 0x800);
        for (unsigned i = 0; i < 16; ++i) writeFloat(system + 0x840 + i * 4, inverse[i]);
        storeV4(system + 0x900, v4(-10.0f, -10.0f, -12.0f, 0.0f));
        storeV4(system + 0x910, v4(10.0f, 10.0f, 7.0f, 0.0f));
        if (vm_read8(layer + 0xb4)) {
            vm_write8(layer + 0xb4, 0);
            storeV4(system + 0x900, positiveZero);
            storeV4(system + 0x910, positiveZero);
        }
    }
    particleEmit(layer);
}

float qglRandom() {
    const uint32_t n = ++randomState, v = n ^ (n << 13);
    const uint32_t hash = (v * (v * v * 15731u + 789221u) + 1376312589u) & 0x7fffffffu;
    return std::fma(float(hash), -9.31322574615478515625e-10f, 1.0f);
}

// Emitter 0x02030b80: new points (0x0202dde4 random grid samples, 0x0202dbf4 an occasional
// streak along u) go to the next list; each point of the current list whose grid velocity is
// fast enough spawns a particle along its (randomly flipped) direction inside an emission cone,
// depth-scaled along the view (0x0202b560), through four queued commands.
void particleEmit(uint32_t layer) {
    const uint32_t system = vm_read32(layer + 0x18), live = system + 0x1360;
    const uint32_t next = layer + 0x90, current = layer + 0xa0;
    if (vm_read32(next + 8) != vm_read32(next + 4)) vm_write32(next + 8, vm_read32(next + 4));
    const auto W = int32_t(vm_read32(layer + 0x44)), H = int32_t(vm_read32(layer + 0x48));
    const auto gridIndex = [&](int32_t size) { return int32_t(((qglRandom() + 1.0f) * 0.5f) * float(size - 1)); };
    {   // 0x0202dde4
        const float chance = std::fma(qglRandom(), 0.5f, 0.5f);
        if (!(chance >= readFloat(live + 0x1c)) && vm_read32(live + 0x18)) {
            uint32_t emitted = 0;
            do {
                const int32_t u = gridIndex(W), v = gridIndex(H);
                pushPoint(next, sampleGrid(layer, u, v), u, v);
                ++emitted;
            } while (vm_read32(live + 0x18) != emitted);
        }
    }
    {   // 0x0202dbf4
        if (!vm_read32(layer + 0x1008)) {
            if (std::fma(qglRandom(), 0.5f, 0.5f) < 0.0599999987f) {
                vm_write32(layer + 0x100c, uint32_t(gridIndex(W)));
                vm_write32(layer + 0x1010, uint32_t(gridIndex(H)));
                vm_write32(layer + 0x1008, uint32_t(int32_t(qglRandom() * 80.0f) + 100));
            }
        } else if (!(ticks & 1)) {
            const auto u = int32_t(vm_read32(layer + 0x100c));
            if (W > u) {
                vm_write32(layer + 0x100c, uint32_t(u + 1));
                const auto v = int32_t(vm_read32(layer + 0x1010));
                pushPoint(next, sampleGrid(layer, u, v), u, v);
                vm_write32(layer + 0x1008, vm_read32(layer + 0x1008) - 1);
            } else vm_write32(layer + 0x1008, 0);
        }
    }
    const uint32_t camera = vm_read32(layer + 0xb0), queue = system + 0x1328;
    for (uint32_t index = 0;; ++index) {
        const uint32_t begin = vm_read32(current + 4);
        const uint32_t count = begin ? uint32_t(int32_t(vm_read32(current + 8) - begin) >> 5) : 0;
        if (count == index) {
            if (vm_read32(current + 8) != begin) vm_write32(current + 8, begin);
            for (uint32_t field : {4u, 8u, 0xcu}) {
                const uint32_t a = vm_read32(current + field), b = vm_read32(next + field);
                vm_write32(current + field, b); vm_write32(next + field, a);
            }
            return;
        }
        const uint32_t point = begin + index * 32;
        const V4 now = sampleGrid(layer, int32_t(vm_read32(point + 0x10)), int32_t(vm_read32(point + 0x14)));
        V4 displacement = sub(now, loadV4(point));
        displacement.v[3] = 0.0f;
        const float inverseStep = 1.0f / readFloat(live + 0x38);
        const V4 velocity = fmaLanes(displacement, inverseStep, negativeZero);
        const float speedSquared = dotSum(velocity, velocity);
        if (0.0001f > ropeLength(speedSquared)) continue;
        V4 direction = fmaLanes(velocity, ropeInverse(speedSquared), negativeZero);
        if ((qglRandom() + 1.0f) * 0.5f < readFloat(live + 0x10))
            for (auto& lane : direction.v) lane = std::bit_cast<float>(std::bit_cast<uint32_t>(lane) ^ 0x80000000u);
        const float dx = direction.v[0], dy = direction.v[1], dz = direction.v[2];
        const V4 c4 = v4(0.0f, dz, -dy, 0.0f), c6 = v4(-dz, 0.0f, dx, 0.0f), c5 = v4(dy, -dx, 0.0f, 0.0f);
        const float s4 = dotSum(c4, c4), s6 = dotSum(c6, c6), s5 = dotSum(c5, c5);
        const V4* chosen = &c5; float chosenSum = s5;
        if (s4 > s6) { if (s4 > s5) { chosen = &c4; chosenSum = s4; } }
        else if (s6 > s5) { chosen = &c6; chosenSum = s6; }
        const V4 perpendicular = fmaLanes(*chosen, ropeInverse(chosenSum), negativeZero);
        const float spread = (qglRandom() + 1.0f) * 0.5f;
        const float theta = spread * (readFloat(live + 0xc) * 0.0174532924f);
        const auto yzx = [](const V4& a) { return v4(a.v[1], a.v[2], a.v[0], a.v[3]); };
        const auto zxy = [](const V4& a) { return v4(a.v[2], a.v[0], a.v[1], a.v[3]); };
        const V4 cross = sub(fmaLanes(yzx(direction), zxy(perpendicular), negativeZero),
                             fmaLanes(zxy(direction), yzx(perpendicular), negativeZero));
        const float phi = ((qglRandom() + 1.0f) * 0.5f) * 6.28318548f;
        const float sinTheta = firmwareSinCos(theta, 0), cosPhi = firmwareSinCos(phi, 1), sinPhi = firmwareSinCos(phi, 0);
        const float a = sinTheta * sinPhi, b = sinTheta * cosPhi, cosTheta = firmwareSinCos(theta, 1);
        const V4 bend = fmaLanes(cross, a, negativeZero);
        const V4 tail = fmaLanes(positiveZero, v4(0, 0, 0, 1), negativeZero);
        const V4 around = fmaLanes(v4(b, b, b, b), perpendicular, bend);
        const V4 emission = add(fmaLanes(direction, cosTheta, around), tail);
        // 0x0202b560: scale the component along the view's z column by the emit z-scale.
        float eye[4], target[4], up[4];
        for (unsigned i = 0; i < 4; ++i) { eye[i] = readFloat(camera + 0x60 + i * 4); target[i] = readFloat(camera + 0x70 + i * 4); up[i] = readFloat(camera + 0x80 + i * 4); }
        const auto view = lookAt(eye, target, up);
        const V4 axis = v4(view[8], view[9], view[10], 0.0f);
        const V4 offset = sub(v4(emission.v[0], emission.v[1], emission.v[2], 1.0f), v4(0, 0, 0, 1));
        const float along = std::fma(dotSum(offset, axis), 1.0f / dotSum(axis, axis), -0.0f);
        const V4 projected = fmaLanes(axis, along, v4(0, 0, 0, 1));
        const float zScale = readFloat(live + 0x14);
        const V4 scaled = add(fmaLanes(projected, zScale, negativeZero), sub(emission, projected));
        const V4 heading = v4(scaled.v[0], scaled.v[1], scaled.v[2], emission.v[3]);
        const float gridSpeed = ropeLength(dotSum(velocity, velocity));
        float multiplier = readFloat(live + 0x4);
        multiplier = multiplier * std::fma(qglRandom(), readFloat(live + 0x8), 1.0f);
        const float speed = atLeast(std::pow(float(gridSpeed * multiplier), float(0.5f)), readFloat(live + 0x0));
        const float aging = readFloat(live + 0x20), agingRandom = qglRandom(), agingVariance = readFloat(live + 0x24);
        const auto command = [&](uint32_t type) -> uint32_t {
            const uint32_t used = vm_read32(queue), capacity = vm_read32(queue + 4);
            if (capacity == used) return 0;
            const uint32_t entry = vm_read32(queue + 0xc) + used * 24;
            vm_write32(queue, used + 1);
            vm_write32(entry, type);
            if (type) vm_write32(entry + 4, 0xffffffffu);
            return entry;
        };
        // A full queue drops the rest of this particle's commands.
        if (!command(0)) continue;
        const uint32_t placed = command(1);
        if (!placed) continue;
        writeFloat(placed + 8, now.v[0]); writeFloat(placed + 12, now.v[1]); writeFloat(placed + 16, now.v[2]);
        const uint32_t launched = command(2);
        if (!launched) continue;
        const V4 launch = fmaLanes(heading, speed, negativeZero);
        writeFloat(launched + 8, launch.v[0]); writeFloat(launched + 12, launch.v[1]); writeFloat(launched + 16, launch.v[2]);
        const uint32_t timed = command(4);
        if (!timed) continue;
        const float lifetime = aging * std::fma(agingRandom, agingVariance, 1.0f);
        writeFloat(timed + 8, 0.00100000005f < lifetime ? lifetime : 0.00100000005f);
    }
}

}

// ---- Native particle update ---------------------------------------------------------
// The PPU update 0x0205ddc8 with its per-particle loop 0x0205d7b0 written out natively;
// the bookkeeping around the loop stays translated. Same arithmetic in the same order:
// VMX lanes are IEEE single (vmaddfp with a -0 addend is a product; fused otherwise),
// VSCR[NJ] is the host's DAZ/FTZ, vrsqrtefp is Cell's estimate (CellEstimate.h) and vrefp
// exact, as in the translation; sine/cosine is a port of the math module's (firmwareSinCos).
// MOYOU_PARTICLES=original runs the translated loop; MOYOU_VERIFY_NATIVE=1 runs both and
// compares the particle arena byte for byte.
extern void moyou_rope_func_0205CA80(ppu_context*);
extern void moyou_rope_func_0205CAD0(ppu_context*);
extern void moyou_rope_func_0205CA00(ppu_context*);
extern void moyou_rope_func_0205C934(ppu_context*);
extern void moyou_rope_func_0205C988(ppu_context*);
extern void moyou_rope_func_0205C9C8(ppu_context*);
extern void moyou_rope_func_0205C8D4(ppu_context*);
extern void moyou_rope_func_02087B50(ppu_context*);

namespace {
struct Lanes { float v[4]; float& operator[](int i) { return v[i]; } float operator[](int i) const { return v[i]; } };
Lanes loadLanes(uint32_t address) { Lanes l; for (int i = 0; i < 4; ++i) l[i] = readFloat(address + i * 4); return l; }
void storeLanes(uint32_t address, const Lanes& l) { for (int i = 0; i < 4; ++i) writeFloat(address + i * 4, l[i]); }
Lanes operator+(const Lanes& a, const Lanes& b) { Lanes r; for (int i = 0; i < 4; ++i) r[i] = a[i] + b[i]; return r; }
Lanes operator-(const Lanes& a, const Lanes& b) { Lanes r; for (int i = 0; i < 4; ++i) r[i] = a[i] - b[i]; return r; }
Lanes operator*(const Lanes& a, const Lanes& b) { Lanes r; for (int i = 0; i < 4; ++i) r[i] = a[i] * b[i]; return r; }
Lanes operator*(const Lanes& a, float s) { Lanes r; for (int i = 0; i < 4; ++i) r[i] = a[i] * s; return r; }
Lanes withW(Lanes a, float w) { a[3] = w; return a; }
// 0x0202b4f4: columns at m; y*c1, then x*c0, z*c2, w*c3 fused in that order.
Lanes transform(uint32_t m, const Lanes& v) {
    const auto c0 = loadLanes(m), c1 = loadLanes(m + 0x10), c2 = loadLanes(m + 0x20), c3 = loadLanes(m + 0x30);
    Lanes r;
    for (int i = 0; i < 4; ++i) {
        float t = v[1] * c1[i];
        t = std::fma(v[0], c0[i], t);
        t = std::fma(v[2], c2[i], t);
        r[i] = std::fma(v[3], c3[i], t);
    }
    return r;
}
// 0x020586f8: Park-Miller step, mantissa into [2,4), minus 3.
float lcg(uint32_t& state) {
    state *= 16807u;
    return std::bit_cast<float>(((state >> 9) & 0x7fffffu) | 0x40000000u) - 3.0f;
}
// 0x0205cf30: bilinear flow-map texel (signed bytes / 127) around the scaled position.
Lanes flow(uint32_t S, Lanes position) {
    const uint32_t T = S + 0x780;
    vm_write32(T, S + 0x80);                                  // 0x0205d0f0: map base
    const auto scale = loadLanes(T + 0x10);
    for (int i = 0; i < 4; ++i) position[i] = (scale[i] * position[i]) - 0.5f;
    Lanes cell, fraction;
    for (int i = 0; i < 4; ++i) { cell[i] = std::floor(position[i]); fraction[i] = position[i] - cell[i]; }
    const int x = int(cell[0]), y = int(cell[1]);
    const auto field = [&](uint32_t o) { return int32_t(vm_read32(T + o)); };
    const auto texel = [&](int tx, int ty) {
        tx = std::max(std::min(tx, field(0x48)), field(0x20));
        ty = std::max(std::min(ty, field(0x4c)), field(0x24));
        const uint32_t at = uint32_t((ty * field(4) + tx) * 3) + vm_read32(T);
        Lanes t;
        for (int i = 0; i < 3; ++i) t[i] = float(int8_t(vm_read8(at + i))) * 0.007874015718698502f;
        t[3] = 0.0f * 0.007874015718698502f;
        return t;
    };
    const auto s0 = texel(x, y), s1 = texel(x + field(0x28), y + field(0x2c));
    const auto s2 = texel(x + field(0x30), y + field(0x34)), s3 = texel(x + field(0x38), y + field(0x3c));
    const auto a = s0 + (s1 - s0) * fraction[0];
    const auto b = s2 + (s3 - s2) * fraction[0];
    return a + (b - a) * fraction[1];
}
// 0x0203ff4c: a ⊗ b (vectormath), terms added in the original order.
Lanes quatMultiply(const Lanes& a, const Lanes& b) {
    Lanes r = b * a[3];
    const Lanes px{{b[3], b[2], b[1], b[0]}}, py{{b[2], b[3], b[0], b[1]}}, pz{{b[1], b[0], b[3], b[2]}};
    const float sx[] = {1, -1, 1, -1}, sy[] = {1, 1, -1, -1}, sz[] = {-1, 1, 1, -1};
    for (int i = 0; i < 4; ++i) r[i] = std::fma(px[i] * a[0], sx[i], r[i]);
    for (int i = 0; i < 4; ++i) r[i] = std::fma(py[i] * a[1], sy[i], r[i]);
    for (int i = 0; i < 4; ++i) r[i] = std::fma(pz[i] * a[2], sz[i], r[i]);
    return r;
}
// 0x02045aa8: q * rsqrt(dot) with one Newton step on the (exact) estimate.
Lanes quatNormalize(const Lanes& q) {
    Lanes s;
    for (int i = 0; i < 4; ++i) s[i] = std::fma(q[i], q[i], 0.0f);
    const float dot = (s[0] + s[1]) + (s[2] + s[3]);
    const float estimate = std::bit_cast<float>(cellRsqrtEstimate(std::bit_cast<uint32_t>(dot), true));  // vrsqrtefp
    const float refined = std::fma(-(std::fma(estimate, estimate * dot, -1.0f)), estimate * 0.5f, estimate);
    return q * (refined == refined ? refined : estimate);
}
// 0x0205d674: truncating float -> half, flushed below 2^-14 and to infinity above 2^15.
uint16_t half(float value) {
    const uint32_t x = std::bit_cast<uint32_t>(value), e = (x >> 23) & 0xff;
    uint32_t h = int32_t(e) > 0x70 ? (((e - 0x70) << 10) & ~0x3ffu) | ((x >> 13) & 0x3ff) : 0;
    if (int32_t(e) > 0x8e) h = 0x7c00;
    return uint16_t((h & 0x7fff) | ((x >> 16) & 0x8000));
}
// Math module pow (0x93f94, Dinkumware) for x > 0 normal and a small non-negative integer
// exponent n, the glare level weights' only use: x = m·2^e with m moved into [√½, √2), m^n by
// binary exponentiation in double precision (skipped when m is 1), then scaled by 2^(n·e)
// (_Dexp of 0 → _Dscale, exact for normal results).
double firmwarePowInteger(double x, unsigned n) {
    if (n == 0) return 1.0;
    int e;
    double m = std::frexp(x, &e);
    if (m < 0.7071067811865476) { m = m + m; --e; }
    const int k = int(double(n) * double(e));
    double p = 1.0;
    if (m != 1.0)
        for (double b = m;;) {
            if (n & 1) p = p * b;
            n >>= 1;
            if (!n) break;
            b = b * b;
        }
    return std::ldexp(p, k);
}

// Math module expf (0x92d34 → Dinkumware _FExp 0x95bf4): NaN returns itself, ±infinity
// gives +infinity / 0, zero gives 1, |x| > 229 saturates. Otherwise n = trunc(x·log2(e) ± ½)
// as a 16-bit integer, g = x − n·ln2 in two parts, e^g as (a + b + 1)/(a − b + 1) in double
// precision (1 when |g| < 2^-25), then scaled by 2^n (_FDscale; exact for normal results).
float firmwareExp(float x) {
    if (x != x) return x;
    if (std::isinf(x)) return x < 0 ? 0.0f : x;
    if (x == 0) return 1.0f;
    if (x < -229.0f) return 0.0f;
    if (x > 229.0f) return std::numeric_limits<float>::infinity();
    float g = x * 1.44269502f;
    g = (g < 0.0f ? -0.5f : 0.5f) + g;
    const int16_t n = int16_t(int32_t(g));
    const float k = float(n);
    float r = std::fma(k, -0.693147063f, x);
    r = std::fma(k, -1.17304637e-07f, r);
    constexpr float tiny = 2.98023224e-08f;
    float value = 1.0f;
    if (!(-tiny < r && r < tiny)) {
        const float r2 = r * r;
        const float a = r2 * 0.0999748558f, b = r * std::fma(r2, 0.00832082611f, 0.5f);
        value = float(((double(a) + double(b)) + 1.0) / ((double(a) - double(b)) + 1.0));
    }
    return std::ldexp(value, n);
}

// Math module 0x95f60 (_FSin; sine for which = 0, cosine for 1). Classified first (0x95ad8):
// NaN returns itself, infinity the default NaN, zero returns itself (sine) or 1 (cosine).
// |x| > 40.7 is first reduced with the module's fmodf (0x93284, exact, so equal to IEEE
// fmodf) by 2π in single precision. Then quadrant n from x * 2/π, two-part reduction,
// single-precision fused polynomials.
float firmwareSinCos(float x, int which) {
    if (x != x) return x;
    if (std::isinf(x)) return std::bit_cast<float>(0x7fc00000u);
    if (x == 0) return which ? 1.0f : x;
    if (double(x) < -40.7 || double(x) > 40.7) x = std::fmod(x, 6.2831854820251465f);
    const float scaled = x * 0.6366197467f;
    const int n = int(scaled > 0.0f ? scaled + 0.5f : scaled - 0.5f);
    const unsigned quadrant = unsigned((n & 3) + which);
    const float k = float(double(n));
    float r = std::fma(k, -1.570796251f, x);
    r = std::fma(k, -7.549790126e-08f, r);
    const float epsilon = 0.000244140625f;
    if (-epsilon >= r || r >= epsilon) {
        const float r2 = r * r;
        if (quadrant & 1) {
            float p = std::fma(r2, -0.001360224909f, 0.04165669531f);
            p = std::fma(r2, p, -0.4999990165f);
            r = std::fma(r2, p, 1.0f);
        } else {
            const float r3 = r * r2;
            float p = std::fma(r2, -0.0001950727019f, 0.008332075551f);
            p = std::fma(r2, p, -0.1666665226f);
            r = std::fma(r3, p, r);
        }
    } else if (quadrant & 1) r = 1.0f;
    return quadrant & 2 ? -r : r;
}
float vmaxf(float x, float y) {
    if (x != x || y != y) return x + y;
    if (x == 0 && y == 0) return std::signbit(x) ? y : x;
    return x > y ? x : y;
}
float vminf(float x, float y) {
    if (x != x || y != y) return x + y;
    if (x == 0 && y == 0) return std::signbit(x) ? x : y;
    return x < y ? x : y;
}
}

namespace {
// Profiling timers (0x0205c9c8 start, 0x0205c8d4 stop): the system time import is 0 here, so
// every sample is 0 s; running sum/min/max and a period average (0x02059464) as the firmware.
void timerStart(uint32_t timer) { vm_write64(timer, 0); vm_write8(timer + 0x20, 1); }
void timerStop(uint32_t timer) {
    if (!vm_read8(timer + 0x20)) return;
    const float elapsed = 0.0f / 1000000.0f;
    const uint32_t s = timer + 8;
    const auto count = int32_t(vm_read32(s + 8));
    if (count == 0) {
        writeFloat(s, elapsed); writeFloat(s + 0x10, elapsed); writeFloat(s + 0x14, elapsed);
    } else {
        writeFloat(s, readFloat(s) + elapsed);
        const float low = readFloat(s + 0x10), high = readFloat(s + 0x14);
        writeFloat(s + 0x10, elapsed < low ? elapsed : low);
        writeFloat(s + 0x14, elapsed > high ? elapsed : high);
    }
    const auto next = count + 1;
    if (next == int32_t(vm_read32(s + 4))) {
        vm_write32(s + 8, 0);
        writeFloat(s + 0xc, readFloat(s) / float(next));
    } else vm_write32(s + 8, uint32_t(next));
    vm_write8(timer + 0x20, 0);
}
void copyBytes(uint32_t destination, uint32_t source, uint32_t size) { std::memmove(vm_base + destination, vm_base + source, size); }
}

// 0x0205ca80: clear the current buffer's draw count, start the frame timer.
void RopeSolver::particleBegin(uint32_t system) {
    const uint32_t render = vm_read32(system + 0x16f0);
    vm_write32(vm_read32(render + 0x44 + vm_read32(system + 0x16f4) * 4), 0);
    timerStart(system + 0x1790);
}

// 0x0205cad0: the command queue (sys+0x1328: count, capacity, entries of 24 bytes): type 0
// allocates from the free list, 1 sets a position, 2 sets a velocity, 3 adds to it, 4 sets
// its w; skipped (but cleared) while sys+0x920 is set.
void RopeSolver::particleCommands(uint32_t system) {
    const uint32_t queue = system + 0x1328;
    const auto count = int32_t(vm_read32(queue));
    writeFloat(system + 0x1900, float(count) / float(int32_t(vm_read32(queue + 4))));
    timerStart(system + 0x1718);
    const uint32_t records = vm_read32(vm_read32(system + 0xa4) + 0x10);
    const auto record = [&](int32_t index) { return records + uint32_t(index) * 48; };
    if (!vm_read8(system + 0x920))
        for (int32_t i = 0; i < count; ++i) {
            const uint32_t command = vm_read32(queue + 0xc) + uint32_t(i) * 24;
            if (vm_read32(command) == 0) {
                // 0x0205b6bc: pop a free index (0x02059228), w = 1, then zero position and velocity.
                vm_write32(system + 0x1354, 0xffffffffu);
                const uint32_t freeCount = vm_read32(system + 0x1314);
                if (freeCount) {
                    vm_write32(system + 0x1354, vm_read32(vm_read32(system + 0x1310) + freeCount * 4 - 4));
                    vm_write32(system + 0x1314, freeCount - 1);
                    writeFloat(record(int32_t(vm_read32(system + 0x1354))) + 12, 1.0f);
                }
                const auto index = int32_t(vm_read32(system + 0x1354));
                if (index >= 0)
                    for (uint32_t b = 0; b < 32; b += 4) writeFloat(record(index) + b, 0.0f);
            }
            auto index = int32_t(vm_read32(command + 4));
            if (index < 0) {
                index = int32_t(vm_read32(system + 0x1354));
                vm_write32(command + 4, uint32_t(index));
                if (index < 0) continue;
            }
            const float x = readFloat(command + 8), y = readFloat(command + 0xc), z = readFloat(command + 0x10);
            const uint32_t position = record(index), velocity = position + 16;
            switch (vm_read32(command)) {
            case 1: writeFloat(position, x); writeFloat(position + 4, y); writeFloat(position + 8, z); break;
            case 2: writeFloat(velocity, x); writeFloat(velocity + 4, y); writeFloat(velocity + 8, z); break;
            case 3:
                writeFloat(velocity, x + readFloat(velocity)); writeFloat(velocity + 4, y + readFloat(velocity + 4));
                writeFloat(velocity + 8, z + readFloat(velocity + 8)); writeFloat(velocity + 12, 0.0f + readFloat(velocity + 12));
                break;
            case 4: writeFloat(velocity + 12, x); break;
            default: break;
            }
        }
    vm_write32(queue, 0);
    timerStop(system + 0x1718);
}

// 0x0205ca00: start the update timer, take the current vertex buffer, reset its count and
// snapshot the simulation inputs (sys+0x100) into sys+0xa00 (0x020599bc).
void RopeSolver::particleSnapshot(uint32_t system) {
    timerStart(system + 0x1768);
    const uint32_t render = vm_read32(system + 0x16f0);
    const auto token = vm_read32(render + 0x1c + vm_read32(system + 0x16f4) * 12) - vertexToken;
    if (token > 1 || !vertexBuffers[token]) throw std::runtime_error("Unadapted original vertex array");
    vm_write32(system + 0x1700, vertexBuffers[token]);
    vm_write32(system + 0x16f8, 0);
    const uint32_t d = system + 0xa00, s = system + 0x100;
    copyBytes(d + 16, s + 16, 16);
    copyBytes(d, s, 16);
    const uint32_t entries = vm_read32(s + 128 + 0x600);
    vm_write32(d + 128 + 0x600, entries);
    copyBytes(d + 128, s + 128, entries * 3);
    copyBytes(d + 128 + 1664, s + 128 + 1664, 64);
    copyBytes(d + 128 + 1728, s + 128 + 1728, 64);
    copyBytes(d + 128 + 1792, s + 128 + 1792, 80);
    vm_write8(d + 0x820, vm_read8(s + 0x820));
    copyBytes(d + 2048, s + 2048, 32);
    copyBytes(d + 2096, s + 2096, 16);
    copyBytes(d + 2112, s + 2112, 64);
    copyBytes(d + 2176, s + 2176, 16);
    copyBytes(d + 0x8ac, s + 0x8ac, 4);
    copyBytes(d + 2192, s + 2192, 16);
    copyBytes(d + 2224, s + 2224, 16);
    copyBytes(d + 0x8a0, s + 0x8a0, 12);
}

void RopeSolver::particleUpdate(uint32_t system) {
    // 0x0205ddc8 around its loop.
    vm_write32(system + 0xa4, system + 0x60);
    vm_write32(system + 0xa0, system + 0x60);
    particleBegin(system);
    particleCommands(system);
    particleSnapshot(system);
    // 0x0205d7b0
    timerStart(system + 0x1740);
    vm_write32(system + 0x1888, 0);
    const uint32_t S = system + 0x100;
    const float invStep = 1.0f * (1.0f / readFloat(S + 0x8b0));
    const Lanes inverse{{1.0f * (1.0f / readFloat(S + 0x8b0)), 1.0f * (1.0f / readFloat(S + 0x8b4)),
                         1.0f * (1.0f / readFloat(S + 0x8b8)), 1.0f * (1.0f / readFloat(S + 0x8bc))}};
    (void)invStep;
    uint32_t seeds[3] = {0x98756161u, 0x21324889u, 0x82181158u};
    const auto sincos = [&](float x, int which) { return firmwareSinCos(x, which); };
    const uint32_t array = vm_read32(vm_read32(system + 0xa4) + 0x10), count = vm_read32(vm_read32(system + 0xa4) + 0x14);
    const auto S0 = withW(loadLanes(S), 0.0f), S10 = loadLanes(S + 0x10), S890 = loadLanes(S + 0x890);
    const auto step = loadLanes(S + 0x8b0), pivot = withW(loadLanes(S + 0x830), 1.0f);
    const float brownian = readFloat(S + 0x8a4), flowScale = readFloat(S + 0x8a0), spin = readFloat(S + 0x8ac) * 0.01666666753590107f;
    const auto lower = loadLanes(S + 0x800), upper = loadLanes(S + 0x810);
    const unsigned mxcsr = _mm_getcsr();
    _mm_setcsr(mxcsr | 0x8040);   // DAZ | FTZ: VSCR[NJ]
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t p = array + i * 0x30, velocity = p + 0x10, rotation = p + 0x20;
        if (readFloat(p + 12) == -666.0f) continue;
        const auto flowField = transform(S + 0x700, withW(flow(S, transform(S + 0x740, withW(loadLanes(p), 1.0f))), 0.0f));
        Lanes random;
        random[0] = lcg(seeds[0]); random[1] = lcg(seeds[1]); random[2] = lcg(seeds[2]); random[3] = 0;
        const auto acceleration = withW(((random * brownian) + S890) + flowField * flowScale, 0.0f);
        const auto force = S0 + acceleration;
        const auto position = withW(loadLanes(p), 1.0f);
        const auto swirl = (pivot + transform(S + 0x840, position - pivot)) - position;
        auto v = (swirl * inverse) + loadLanes(velocity);
        storeLanes(velocity, v);
        v = ((force - S10 * v) * step) + v;
        storeLanes(velocity, v);
        const auto moved = (v * step) + loadLanes(p);
        storeLanes(p, moved);
        const float age = moved[3], phase = age * 6.2831854820251465f;
        const Lanes omega{{sincos(phase * 0.3700000047683716f, 0), sincos(phase * 0.17000000178813934f, 1),
                           sincos(phase * 0.3100000023841858f, 1), 0.0f}};
        const auto q = loadLanes(rotation) + (quatMultiply(omega, loadLanes(rotation)) * 0.5f) * spin;
        storeLanes(rotation, q);
        storeLanes(rotation, quatNormalize(q));
        bool inside = 0.9999899864196777f > age;
        if (inside) {
            Lanes clamped;
            for (int k = 0; k < 4; ++k) clamped[k] = vminf(vmaxf(moved[k], lower[k]), upper[k]);
            inside = clamped[0] == moved[0] && clamped[1] == moved[1] && clamped[2] == moved[2];
        }
        if (!inside) {
            // 0x0205bb54: free-list push (sys+0x1310), then mark dead.
            if (readFloat(p + 12) != -666.0f) {
                const uint32_t list = vm_read32(system + 0x1310), used = vm_read32(system + 0x1314);
                vm_write32(list + used * 4, i);
                vm_write32(system + 0x1314, used + 1);
                writeFloat(p + 12, -666.0f);
            }
            continue;
        }
        vm_write32(system + 0x1888, vm_read32(system + 0x1888) + 1);
        const uint32_t out = vm_read32(system + 0x1700);
        const float early = age < 0.019999999552965164f ? age : 0.019999999552965164f;
        float late = age - 0.9399999976158142f;
        if (!(late >= 0.0f)) late = 0.0f;
        const float grow = early * 50.0f;
        storeLanes(out, withW(moved, std::fma(late * -16.66666603088379f, grow, grow)));
        const auto r = loadLanes(rotation);
        for (int k = 0; k < 4; ++k) vm_write16(out + 0x10 + k * 2, half(r[k]));
        for (int k = 0; k < 4; ++k) vm_write16(out + 0x18 + k * 2, half(0.0f));
        vm_write32(system + 0x1700, out + 0x20);
        vm_write32(system + 0x16f8, vm_read32(system + 0x16f8) + 1);
    }
    _mm_setcsr(mxcsr);
    vm_write32(system + 0x1880, vm_read32(system + 0x16f8));
    timerStop(system + 0x1740);
    // 0x0205c934: flip the double buffer and publish the count; 0x0205c988: counters.
    vm_write32(system + 0x16f4, 1 - vm_read32(system + 0x16f4));
    if (vm_read8(system + 0x30)) vm_write32(system + 0x16f8, vm_read32(system + 0x1880));
    vm_write32(system + 0x16fc, vm_read32(system + 0x16f8));
    timerStop(system + 0x1768);
    vm_write32(system + 0x1714, vm_read32(system + 0x1888));
    vm_write32(system + 0x1710, vm_read32(vm_read32(system + 0xa4) + 0x18));
    timerStop(system + 0x1790);
}
