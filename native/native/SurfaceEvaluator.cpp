#include "SurfaceEvaluator.h"
#include "SpuRuntime.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
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
    checked(moyou_spu_open(0, original.c_str()));
    argument(3, 1);
    argument(4, 0xffff);
    checked(moyou_spu_call(0x5fd8, 10000));
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
    { const auto bytes = bigEndian(scale1); write(0xd590, bytes.data(), bytes.size()); }
    { const auto bytes = bigEndian(scale2); write(0xd5a0, bytes.data(), bytes.size()); }
    { const auto bytes = bigEndian(offset); write(0xd5b0, bytes.data(), bytes.size()); }
    latticeReady_ = true;
}

SurfaceEvaluator::Grid SurfaceEvaluator::project(const Grid& nodes, const Matrix& matrix) {
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

SurfaceEvaluator::Mesh SurfaceEvaluator::subdivide(const Grid& grid) {
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
