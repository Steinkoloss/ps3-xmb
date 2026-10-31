#pragma once
#include <array>
#include <cstdint>
#include <map>
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
    struct GlareWeights { std::array<Vector,8> taps{}; std::array<float,6> levels{}; };
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
    GlareWeights glareWeights(Vector radii, float sumPower, float level);
    // Original particle system (PPU path); returns its guest address.
    uint32_t createParticles(unsigned capacity);
    void particleParameters(uint32_t system, const std::map<std::string, float>& menu);
    uint32_t createParticleLayer(uint32_t system);
    // Original camera (constructor 0x02024a68): look-at view, column-major.
    Matrix cameraView() const;
    Matrix inverseCameraView() const;
    // Original emitter on a world-space grid: column-major matrix applied to 8-float vertices.
    void emitParticles(uint32_t layer, const std::vector<std::array<float, 8>>& grid, unsigned width,
                       unsigned height, const Matrix& columns);
    unsigned stepParticles(uint32_t system, float seconds);
    struct ParticleUniforms {
        Matrix camera{}, lightPack{}, fragmentLightPack{};
        std::map<std::string, Vector> vertex, fragment;
        unsigned passes = 0;  // bit 0: quads program, bit 1: second program
    };
    // Original draw setup 0x0205c254: uniform values for both particle programs.
    ParticleUniforms particleUniforms(uint32_t system);
    std::vector<float> particleVertices(uint32_t system) const;
    uint32_t guestWord(uint32_t address) const;  // development inspection
private:
    struct State;
    std::unique_ptr<State> state_;
};
