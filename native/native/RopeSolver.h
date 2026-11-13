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
    Matrix modelMatrixOriginal(Vector position, Vector axis, float angle);
    Matrix multiply(const Matrix& left, const Matrix& right);
    Matrix multiplyOriginal(const Matrix& left, const Matrix& right);
    std::array<Texture, 2> textures();
    std::array<Texture, 2> texturesOriginal();
    ExposureTables exposure(float exposure, float white, float threshold, float gamma, float dither);
    ExposureTables exposureOriginal(float exposure, float white, float threshold, float gamma, float dither);
    // Compares native noise with the original exposure update for successive frames.
    bool checkExposureNoise(unsigned frames);
    GlareWeights glareWeights(Vector radii, float sumPower, float level);
    // Original particle system (PPU path); returns its guest address.
    uint32_t createParticles(unsigned capacity);
    // PARTICLES, PARTICLES_UI and PARTICLES_SPE menus (keys lower case, spaces/underscores removed).
    void particleParameters(uint32_t system, const std::map<std::string, float>& menu,
                            const std::map<std::string, float>& ui, const std::map<std::string, float>& spe);
    uint32_t createParticleLayer(uint32_t system);
    // Original camera (constructor 0x02024a68): look-at view, column-major.
    Matrix cameraView() const;
    Matrix inverseCameraView() const;
    // One original ParticlesLayer frame on a world-space ribbon grid (column-major matrix on
    // 8-float vertices): frame logic and emitter, then the PPU update. Returns the live count.
    unsigned particleFrame(uint32_t system, uint32_t layer, const std::vector<std::array<float, 8>>& grid,
                           unsigned width, unsigned height, const Matrix& columns, float aspect, float seconds);
    struct ParticleUniforms {
        Matrix camera{}, lightPack{}, fragmentLightPack{};
        std::map<std::string, Vector> vertex, fragment;
        unsigned passes = 0;  // bit 0: quads program, bit 1: second program
    };
    // Original draw setup 0x0205c254: uniform values for both particle programs.
    ParticleUniforms particleUniforms(uint32_t system);
    std::vector<float> particleVertices(uint32_t system) const;
    // Pointer interaction (not in the firmware): add velocity to rope nodes (model space) and
    // to live particles near a cursor moving across the screen. clip: P·V(·M) column-major;
    // cursor and motion in NDC (y up); radius in NDC height units; aspect = width/height.
    void nudgeRope(const Matrix& clip, const Matrix& modelFromClip, float x, float y, float dx, float dy,
                   float aspect, float radius, float strength);
    void nudgeParticles(uint32_t system, const Matrix& clip, float x, float y, float dx, float dy,
                        float aspect, float radius, float strength);
    uint32_t guestWord(uint32_t address) const;  // development inspection
    unsigned checkSinCos(unsigned samples);  // native _FSin vs the math module; returns mismatches
    std::vector<uint8_t> guestBytes(uint32_t address, uint32_t size) const;
private:
    void particleUpdate(uint32_t system);
    std::vector<uint8_t> exposureNoise();
    struct State;
    std::unique_ptr<State> state_;
    std::unique_ptr<ExposureTables> exposureCache_;
    std::array<float, 5> exposureInputs_{};
};
