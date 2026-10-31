#pragma once
#include "Renderer.h"
#include "RopeSolver.h"
#include "SurfaceEvaluator.h"
#include <map>

std::map<QString,float> originalMenu(const QString& reference, const QString& preset, const QString& name);

// Original CPU rope, SPU surface and GPU FFD stages. Owns one OpenGL pass, so the
// thread that constructs and steps it needs its own current context.
class OriginalSimulation {
public:
    struct Frame { std::vector<float> vertices; std::array<std::vector<float>,2> exposure; RopeSolver::Texture noise;
        std::vector<float> particles; };  // particles: 8 floats each, original PPU-path output
    // Outputs the display needs once; computed in the original call order.
    struct Constants { std::array<RopeSolver::Texture,2> textures; RopeSolver::GlareWeights glare; };
    OriginalSimulation(const QString& shaders, const QString& reference, const QString& cpu,
                       const QString& spline, const QString& preset);
    Frame step(double seconds, QSize size);
    const Constants& constants() const { return constants_; }
private:
    void deformation(float time);
    std::unique_ptr<RopeSolver> rope_;
    std::unique_ptr<SurfaceEvaluator> surface_;
    std::unique_ptr<Renderer> ffd_;
    std::array<SurfaceEvaluator::Vector,128> lattice_{};
    RopeSolver::Matrix model_{};
    SurfaceEvaluator::Vector scale1_{}, scale2_{}, offset_{};
    std::map<QString,float> hdr_;
    uint32_t particles_ = 0, particleLayer_ = 0;
    Constants constants_;
};
