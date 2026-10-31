#pragma once
#include "Renderer.h"
#include "RopeSolver.h"
#include "SurfaceEvaluator.h"
#include <map>

class OriginalScene {
public:
    OriginalScene(const QString& shaders, const QString& reference, const QString& cpu,
                  const QString& spline, const QString& preset, unsigned month = 7, double hour = -1);
    void advance(double seconds, QSize size);
    void render(GLuint target, QSize size);
    QImage capture(QSize size);
    const std::vector<float>& mesh() const { return vertices_; }
private:
    std::map<QString,float> menu(const QString& name) const;
    void deformation(float time);
    std::unique_ptr<RopeSolver> rope_;
    std::unique_ptr<SurfaceEvaluator> surface_;
    std::unique_ptr<Renderer> ffd_, display_;
    std::array<SurfaceEvaluator::Vector,128> lattice_{};
    RopeSolver::Matrix model_{};
    SurfaceEvaluator::Vector scale1_{}, scale2_{}, offset_{};
    QString reference_, preset_;
    std::map<QString,float> hdr_;
    std::vector<float> vertices_;
};
