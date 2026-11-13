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
    // Native ports of the SPU stages (same maths; float rounding differs at the 1e-6 level).
    // MOYOU_SURFACE=original runs the translated SPU code instead; MOYOU_VERIFY_NATIVE=1
    // runs both and reports the largest difference.
    Grid project(const Grid& nodes, const Matrix& matrix);
    Mesh subdivide(const Grid& clipGrid);
    Grid projectOriginal(const Grid& nodes, const Matrix& matrix);
    Mesh subdivideOriginal(const Grid& clipGrid);
private:
    Grid projectNative(const Grid& nodes, const Matrix& matrix) const;
    Mesh subdivideNative(const Grid& clipGrid) const;
    std::array<Vector, 7 * 7 * 11> lattice_{};
    Vector scale1_{}, scale2_{}, offset_{}, reciprocal_{};
    std::array<Vector, 128> controls_{};
    bool original_ = false, verify_ = false, spuOpen_ = false;
    std::string localStore_;
    // Opens the translated SPU program on the original local store (first original call).
    void openOriginal();
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
