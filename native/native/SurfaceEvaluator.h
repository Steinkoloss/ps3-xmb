#pragma once
#include <array>
#include <barrier>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class SurfaceEvaluator {
public:
    using Vector = std::array<float, 4>;
    using Grid = std::array<Vector, 361>;
    using Matrix = std::array<float, 16>;
    using Mesh = std::vector<std::array<float, 8>>;
    // threads 0: hardware concurrency (at most 4); MOYOU_SPU_THREADS overrides.
    explicit SurfaceEvaluator(const std::string& originalLocalStore, unsigned threads = 0);
    ~SurfaceEvaluator();
    SurfaceEvaluator(const SurfaceEvaluator&) = delete;
    SurfaceEvaluator& operator=(const SurfaceEvaluator&) = delete;
    void lattice(const std::array<Vector, 128>& controls, Vector inputScale, Vector outputScale, Vector offset);
    Grid project(const Grid& nodes, const Matrix& matrix);
    Mesh subdivide(const Grid& clipGrid);
private:
    struct Write { uint32_t address; std::vector<uint8_t> bytes; };
    void write(uint32_t address, const void* bytes, size_t size);
    // Runs job(worker) on every context; workers first replay the logged writes.
    void run(const std::function<void(unsigned)>& job);
    void work(unsigned worker);
    bool latticeReady_ = false;
    std::vector<Write> log_;
    std::vector<std::thread> workers_;
    std::unique_ptr<std::barrier<>> start_, done_;
    std::function<void(unsigned)> job_;
    std::vector<std::string> errors_;
    bool stop_ = false;
};
