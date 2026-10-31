#pragma once
#include "Renderer.h"
#include "RopeSolver.h"
#include "SurfaceEvaluator.h"
#include <future>
#include <map>

// Time-of-day preset blend (0x02011600, table 0x0209c990): each interval between keyframe times
// blends one preset into the next; the weight is eased (measured on a running 4.93 XMB:
// menu values follow 3w²-2w³ of the linear interval fraction).
struct PresetBlend { QString from, to; float weight = 0; };
PresetBlend presetBlend(double hour);
// Base menu plus preset override. Preset "clock" blends the time-of-day presets at hour (0-24).
std::map<QString,float> originalMenu(const QString& reference, const QString& preset, const QString& name, double hour = -1);

// Original CPU rope, SPU surface and GPU FFD stages. Owns one OpenGL pass, so the
// thread that constructs and steps it needs its own current context.
class OriginalSimulation {
public:
    struct Frame { std::vector<float> vertices; std::array<std::vector<float>,2> exposure; RopeSolver::Texture noise;
        std::vector<float> particles;  // 8 floats each, original PPU-path output
        RopeSolver::ParticleUniforms particleUniforms; RopeSolver::Matrix projection{}, view{};  // projection: P·V
        unsigned menus = 0; double menuHour = -1; RopeSolver::GlareWeights glare{}; };  // menu generation applied
    // Outputs the display needs once; computed in the original call order.
    struct Constants { std::array<RopeSolver::Texture,2> textures; RopeSolver::GlareWeights glare; };
    OriginalSimulation(const QString& shaders, const QString& reference, const QString& cpu,
                       const QString& spline, const QString& preset, double hour = -1);
    ~OriginalSimulation();
    // Preset "clock": re-blend the menus for this hour on the next step.
    void setHour(double hour) { pendingHour_ = hour; }
    Frame step(double seconds, QSize size);
    const Constants& constants() const { return constants_; }
private:
    void deformation(float time);
    void applyMenus(double hour);
    QString reference_, preset_;
    double pendingHour_ = -1, menuHour_ = -1;
    unsigned menus_ = 0;
    void requestDeformation(float time);
    void collectDeformation();
    bool deformationPending_ = false;
    std::unique_ptr<RopeSolver> rope_;
    std::unique_ptr<SurfaceEvaluator> surface_;
    std::unique_ptr<Renderer> ffd_;
    std::array<SurfaceEvaluator::Vector,128> lattice_{};
    RopeSolver::Matrix model_{};
    SurfaceEvaluator::Vector scale1_{}, scale2_{}, offset_{};
    std::map<QString,float> hdr_;
    uint32_t particles_ = 0, particleLayer_ = 0;
    struct Particles { std::vector<float> vertices; RopeSolver::ParticleUniforms uniforms; };
    std::future<Particles> particleJob_;
    Constants constants_;
};
