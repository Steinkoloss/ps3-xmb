#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// One private guest address space. No PS3 OS, emulator, or captured animation.
class RopeSolver {
public:
    using Nodes = std::array<std::array<float, 4>, 361>;
    using Vector = std::array<float, 4>;
    using Matrix = std::array<float, 16>;
    struct Texture { unsigned width, height; std::vector<uint8_t> rgba; };
    struct ExposureTables { std::array<std::vector<float>, 2> rgba; Texture noise; };
    RopeSolver(const std::string& rendererImage, const std::string& mathImage);
    ~RopeSolver();
    RopeSolver(const RopeSolver&) = delete;
    RopeSolver& operator=(const RopeSolver&) = delete;
    void reset(unsigned state);
    void parameter(std::string_view name, float value);
    void step();
    void advance(double seconds);
    Nodes positions() const;
    float time() const;
    uint32_t randomState() const;
    Matrix modelMatrix(Vector position, Vector axis, float angle);
    Matrix multiply(const Matrix& left, const Matrix& right);
    std::array<Texture, 2> textures();
    ExposureTables exposure(float exposure, float white, float threshold, float gamma, float dither);
private:
    struct State;
    std::unique_ptr<State> state_;
};
