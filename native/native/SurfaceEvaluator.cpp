#include "SurfaceEvaluator.h"
#include "SpuRuntime.h"
#include "SpuEstimate.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {
void checked(int success) { if (!success) throw std::runtime_error(moyou_spu_error()); }
void argument(unsigned reg, uint32_t value) {
    uint32_t words[] = {value,0,0,0};
    checked(moyou_spu_set_register(reg, words));
}
void vectorArgument(unsigned reg, const SurfaceEvaluator::Vector& vector) {
    uint32_t words[4];
    for (unsigned i = 0; i < 4; ++i) words[i] = std::bit_cast<uint32_t>(vector[i]);
    checked(moyou_spu_set_register(reg, words));
}
SurfaceEvaluator::Vector result() {
    uint32_t words[4];
    checked(moyou_spu_get_register(3, words));
    SurfaceEvaluator::Vector vector;
    for (unsigned i = 0; i < 4; ++i) vector[i] = std::bit_cast<float>(words[i]);
    return vector;
}
std::array<uint8_t, 16> bigEndian(const SurfaceEvaluator::Vector& vector) {
    std::array<uint8_t, 16> bytes;
    for (unsigned i = 0; i < 4; ++i) {
        if (!std::isfinite(vector[i])) throw std::runtime_error("Non-finite original surface input");
        const auto word = std::bit_cast<uint32_t>(vector[i]);
        for (unsigned j = 0; j < 4; ++j) bytes[i * 4 + j] = word >> (24 - j * 8);
    }
    return bytes;
}
float word(const uint8_t* bytes) {
    return std::bit_cast<float>(uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 |
        uint32_t(bytes[2]) << 8 | bytes[3]);
}
}

SurfaceEvaluator::SurfaceEvaluator(const std::string& original, unsigned threads) {
    if (const auto* mode = std::getenv("MOYOU_SURFACE")) original_ = !std::strcmp(mode, "original");
    verify_ = std::getenv("MOYOU_VERIFY_NATIVE") != nullptr;
    // The native stages need no SPU: the translation and its local store are opened only to
    // run or compare with the original.
    localStore_ = original;
    errors_.resize(1);   // one context until workers are started below
    if (!original_ && !verify_) return;
    openOriginal();
    if (const auto* forced = std::getenv("MOYOU_SPU_THREADS")) threads = unsigned(std::atoi(forced));
    if (!threads) threads = std::clamp(std::thread::hardware_concurrency(), 1u, 4u);
    threads = std::clamp(threads, 1u, 16u);
    errors_.resize(threads);
    if (threads == 1) return;
    // Every context starts from the initialized one; per-call results do not depend on
    // which context ran earlier calls (checked bit-identical against one thread).
    checked(moyou_spu_publish());
    start_ = std::make_unique<std::barrier<>>(threads);
    done_ = std::make_unique<std::barrier<>>(threads);
    for (unsigned worker = 1; worker < threads; ++worker) workers_.emplace_back([this, worker] { work(worker); });
}

void SurfaceEvaluator::openOriginal() {
    if (spuOpen_) return;
    checked(moyou_spu_open(0, localStore_.c_str()));
    argument(3, 1);
    argument(4, 0xffff);
    checked(moyou_spu_call(0x5fd8, 10000));
    spuOpen_ = true;
    if (latticeReady_) lattice(controls_, scale1_, scale2_, offset_);   // into the local store too
}

SurfaceEvaluator::~SurfaceEvaluator() {
    if (workers_.empty()) return;
    stop_ = true;
    start_->arrive_and_wait();
    for (auto& worker : workers_) worker.join();
}

void SurfaceEvaluator::work(unsigned worker) {
    if (!moyou_spu_attach()) errors_[worker] = moyou_spu_error();
    size_t applied = 0;
    for (;;) {
        start_->arrive_and_wait();
        if (stop_) return;
        try {
            if (!errors_[worker].empty()) throw std::runtime_error(errors_[worker]);
            for (; applied < log_.size(); ++applied)
                checked(moyou_spu_write(log_[applied].address, log_[applied].bytes.data(), log_[applied].bytes.size()));
            job_(worker);
        } catch (const std::exception& error) { errors_[worker] = error.what(); }
        done_->arrive_and_wait();   // main clears the log here
        done_->arrive_and_wait();
        applied = 0;
    }
}

void SurfaceEvaluator::write(uint32_t address, const void* bytes, size_t size) {
    if (!spuOpen_) return;
    checked(moyou_spu_write(address, bytes, size));
    if (!workers_.empty()) log_.push_back({address, std::vector<uint8_t>(static_cast<const uint8_t*>(bytes),
        static_cast<const uint8_t*>(bytes) + size)});
}

void SurfaceEvaluator::run(const std::function<void(unsigned)>& job) {
    if (workers_.empty()) { job(0); return; }
    job_ = job;
    start_->arrive_and_wait();
    try { job(0); } catch (const std::exception& error) { errors_[0] = error.what(); }
    done_->arrive_and_wait();
    // All contexts hold every logged write now; start the next log empty.
    log_.clear();
    done_->arrive_and_wait();
    for (auto& error : errors_)
        if (!error.empty()) { const auto message = error; error.clear(); throw std::runtime_error(message); }
}

void SurfaceEvaluator::lattice(const std::array<Vector, 128>& controls, Vector scale1, Vector scale2, Vector offset) {
    for (unsigned z = 0; z < 7; ++z)
        for (unsigned y = 0; y < 7; ++y)
            for (unsigned x = 0; x < 11; ++x) {
                const auto a = std::clamp(int(x) - 1, 0, 7);
                const auto b = std::clamp(int(y) - 1, 0, 3);
                const auto c = std::clamp(int(z) - 1, 0, 3);
                // Original CPU VMX multiply-add precedes border replication.
                auto control = controls[(c * 4 + b) * 8 + a];
                for (unsigned i = 0; i < 4; ++i) control[i] = std::fma(control[i], scale1[i], offset[i]);
                { const auto bytes = bigEndian(control); write(0xb380 + ((z * 7 + y) * 11 + x) * 16, bytes.data(), bytes.size()); }
            }
    for (unsigned z = 0; z < 7; ++z)
        for (unsigned y = 0; y < 7; ++y)
            for (unsigned x = 0; x < 11; ++x) {
                auto control = controls[(std::clamp(int(z) - 1, 0, 3) * 4 + std::clamp(int(y) - 1, 0, 3)) * 8 + std::clamp(int(x) - 1, 0, 7)];
                for (unsigned i = 0; i < 4; ++i) control[i] = std::fma(control[i], scale1[i], offset[i]);
                lattice_[(z * 7 + y) * 11 + x] = control;
            }
    scale2_ = scale2;
    offset_ = offset;
    // The original divides by the input scale with an estimate (0x3440: frest, fi, then a
    // product with 1.0, which is exact).
    for (unsigned i = 0; i < 4; ++i) reciprocal_[i] = spuReciprocal(scale1[i]);
    controls_ = controls; scale1_ = scale1;
    if (spuOpen_) {
        const Vector ones{1, 1, 1, 1};
        const auto bytes = bigEndian(ones);
        checked(moyou_spu_write(0x12000, bytes.data(), bytes.size()));
        argument(3, 0x12000);
        vectorArgument(4, scale1);
        checked(moyou_spu_call(0x3440, 100));
        const auto original = result();
        if (std::memcmp(original.data(), reciprocal_.data(), sizeof original))
            std::fprintf(stderr, "native reciprocal: 16 bytes differ\n");
    }
    { const auto bytes = bigEndian(scale1); write(0xd590, bytes.data(), bytes.size()); }
    { const auto bytes = bigEndian(scale2); write(0xd5a0, bytes.data(), bytes.size()); }
    { const auto bytes = bigEndian(offset); write(0xd5b0, bytes.data(), bytes.size()); }
    latticeReady_ = true;
}

SurfaceEvaluator::Grid SurfaceEvaluator::projectOriginal(const Grid& nodes, const Matrix& matrix) {
    openOriginal();
    if (!latticeReady_) throw std::runtime_error("The original surface requires its live FFD lattice");
    for (unsigned column = 0; column < 4; ++column) {
        Vector v;
        for (unsigned row = 0; row < 4; ++row) v[row] = matrix[column * 4 + row];
        const auto bytes = bigEndian(v);
        write(0xb300 + column * 16, bytes.data(), bytes.size());
    }
    Grid projected;
    const auto count = unsigned(errors_.size());
    run([&](unsigned worker) {
        for (unsigned node = worker; node < 361; node += count) {
            vectorArgument(3, nodes[node]);
            checked(moyou_spu_call(0x3c68, 20000));
            vectorArgument(4, result());
            argument(3, 0xb300);
            checked(moyou_spu_call(0x3bf0, 1000));
            projected[node] = result();
        }
    });
    return projected;
}

SurfaceEvaluator::Mesh SurfaceEvaluator::subdivideOriginal(const Grid& grid) {
    openOriginal();
    for (unsigned node = 0; node < grid.size(); ++node) {
        const auto bytes = bigEndian(grid[node]);
        write(0x9c00 + node * 16, bytes.data(), bytes.size());
    }
    Mesh mesh(128 * 128);
    const auto count = unsigned(errors_.size());
    run([&](unsigned worker) {
    for (unsigned patch = worker; patch < 256; patch += count) {
        const uint8_t index[] = {0,0,0,static_cast<uint8_t>(patch)};
        checked(moyou_spu_write(0xe640, index, sizeof index));
        argument(3, 0x12000);
        checked(moyou_spu_call(0x45c0, 20000));
        uint8_t vertices[0x800];
        checked(moyou_spu_read(0x12000, vertices, sizeof vertices));
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 8; ++x) {
                auto& vertex = mesh[((patch / 16) * 8 + y) * 128 + (patch % 16) * 8 + x];
                for (unsigned c = 0; c < 8; ++c) {
                    vertex[c] = word(vertices + (y * 8 + x) * 32 + c * 4);
                    if (!std::isfinite(vertex[c])) throw std::runtime_error("Non-finite original surface result");
                }
            }
    }
    });
    return mesh;
}

namespace {
// Uniform cubic B-spline weights and their derivatives (the original basis, 0xd5c0).
std::array<float, 4> basis(float t) {
    const float s = 1 - t;
    return {s * s * s / 6, (3 * t * t * t - 6 * t * t + 4) / 6, (-3 * t * t * t + 3 * t * t + 3 * t + 1) / 6, t * t * t / 6};
}
std::array<float, 4> slope(float t) {
    const float s = 1 - t;
    return {-s * s / 2, (3 * t * t - 4 * t) / 2, (-3 * t * t + 2 * t + 1) / 2, t * t / 2};
}
template <class Values> float largest(const Values& a, const Values& b) {
    float error = 0;
    for (size_t i = 0; i < a.size(); ++i)
        for (size_t c = 0; c < a[i].size(); ++c) error = std::max(error, std::abs(a[i][c] - b[i][c]) / std::max(1.0f, std::abs(b[i][c])));
    return error;
}
}

// FFD (0x3c68): lattice coordinates (node - offset) / inputScale * (8,4,4), clamped to the
// lattice; cubic B-spline over the 11 x 7 x 7 bordered controls, plus the normalised
// coordinate, times the output scale. Then the camera matrix (0x3bf0).
SurfaceEvaluator::Grid SurfaceEvaluator::projectNative(const Grid& nodes, const Matrix& matrix) const {
    if (!latticeReady_) throw std::runtime_error("The original surface requires its live FFD lattice");
    static constexpr float extent[] = {8, 4, 4};
    Grid projected;
    for (unsigned node = 0; node < nodes.size(); ++node) {
        Vector n;
        for (unsigned i = 0; i < 4; ++i) n[i] = (nodes[node][i] - offset_[i]) * reciprocal_[i];
        int cell[3];
        std::array<float, 4> weight[3];
        for (unsigned a = 0; a < 3; ++a) {
            const float u = n[a] * extent[a];
            cell[a] = std::clamp(int(u), 0, int(extent[a]) - 1);
            weight[a] = basis(std::clamp(u - float(cell[a]), 0.0f, 1.0f));
        }
        Vector sum{};
        for (unsigned k = 0; k < 4; ++k)
            for (unsigned j = 0; j < 4; ++j) {
                const float wz = weight[2][k] * weight[1][j];
                const auto* row = &lattice_[((cell[2] + k) * 7 + cell[1] + j) * 11 + cell[0]];
                for (unsigned i = 0; i < 4; ++i) {
                    const float w = wz * weight[0][i];
                    for (unsigned c = 0; c < 4; ++c) sum[c] += w * row[i][c];
                }
            }
        Vector deformed;
        for (unsigned c = 0; c < 4; ++c) deformed[c] = (sum[c] + n[c]) * scale2_[c];
        for (unsigned r = 0; r < 4; ++r)
            projected[node][r] = matrix[r] * deformed[0] + matrix[4 + r] * deformed[1] + matrix[8 + r] * deformed[2] + matrix[12 + r] * deformed[3];
    }
    return projected;
}

// Patches (0x45c0): 16 x 16 bicubic B-spline patches over the 19 x 19 controls, each sampled
// 8 x 8 at t = k * 16/127; position, then normal = dv x du (unnormalised), then 0.
SurfaceEvaluator::Mesh SurfaceEvaluator::subdivideNative(const Grid& grid) const {
    std::array<std::array<float, 4>, 8> b, d;
    for (unsigned k = 0; k < 8; ++k) { b[k] = basis(float(k) * 16.0f / 127.0f); d[k] = slope(float(k) * 16.0f / 127.0f); }
    Mesh mesh(128 * 128);
    for (unsigned pr = 0; pr < 16; ++pr)
        for (unsigned pc = 0; pc < 16; ++pc) {
            // Along u for each of the four control rows: position and u-derivative.
            Vector along[8][4], alongSlope[8][4];
            for (unsigned x = 0; x < 8; ++x)
                for (unsigned r = 0; r < 4; ++r) {
                    Vector p{}, s{};
                    for (unsigned i = 0; i < 4; ++i) {
                        const auto& control = grid[(pr + r) * 19 + pc + i];
                        for (unsigned c = 0; c < 4; ++c) { p[c] += b[x][i] * control[c]; s[c] += d[x][i] * control[c]; }
                    }
                    along[x][r] = p; alongSlope[x][r] = s;
                }
            for (unsigned y = 0; y < 8; ++y)
                for (unsigned x = 0; x < 8; ++x) {
                    Vector p{}, du{}, dv{};
                    for (unsigned r = 0; r < 4; ++r)
                        for (unsigned c = 0; c < 4; ++c) {
                            p[c] += b[y][r] * along[x][r][c];
                            du[c] += b[y][r] * alongSlope[x][r][c];
                            dv[c] += d[y][r] * along[x][r][c];
                        }
                    auto& vertex = mesh[(pr * 8 + y) * 128 + pc * 8 + x];
                    for (unsigned c = 0; c < 4; ++c) vertex[c] = p[c];
                    vertex[4] = dv[1] * du[2] - dv[2] * du[1];
                    vertex[5] = dv[2] * du[0] - dv[0] * du[2];
                    vertex[6] = dv[0] * du[1] - dv[1] * du[0];
                    vertex[7] = 0;
                }
        }
    return mesh;
}

namespace {
template <class Values> void requireFinite(const Values& values, const char* message) {
    for (const auto& value : values) for (float c : value) if (!std::isfinite(c)) throw std::runtime_error(message);
}
}

SurfaceEvaluator::Grid SurfaceEvaluator::project(const Grid& nodes, const Matrix& matrix) {
    if (original_) return projectOriginal(nodes, matrix);
    requireFinite(nodes, "Non-finite original surface input");
    auto native = projectNative(nodes, matrix);
    requireFinite(native, "Non-finite original surface result");
    if (verify_) std::fprintf(stderr, "native project: largest relative difference %.3g\n", largest(native, projectOriginal(nodes, matrix)));
    return native;
}

SurfaceEvaluator::Mesh SurfaceEvaluator::subdivide(const Grid& grid) {
    if (original_) return subdivideOriginal(grid);
    requireFinite(grid, "Non-finite original surface input");
    auto native = subdivideNative(grid);
    requireFinite(native, "Non-finite original surface result");
    if (verify_) std::fprintf(stderr, "native subdivide: largest relative difference %.3g\n", largest(native, subdivideOriginal(grid)));
    return native;
}
