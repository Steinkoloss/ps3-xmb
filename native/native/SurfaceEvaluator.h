#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

class SurfaceEvaluator {
public:
    using Vector = std::array<float, 4>;
    using Grid = std::array<Vector, 361>;
    using Matrix = std::array<float, 16>;
    using Mesh = std::vector<std::array<float, 8>>;
    explicit SurfaceEvaluator(const std::string& originalLocalStore);
    void lattice(const std::array<Vector, 128>& controls, Vector inputScale, Vector outputScale, Vector offset);
    Grid project(const Grid& nodes, const Matrix& matrix);
    Mesh subdivide(const Grid& clipGrid);
private:
    bool latticeReady_ = false;
};
