#pragma once
#include "OriginalSimulation.h"

// The original background, ribbon, composition and glare pass graph.
class OriginalDisplay {
public:
    OriginalDisplay(const QString& shaders, const QString& reference, const QString& preset,
                    OriginalSimulation::Constants constants, unsigned month, double hour);
    // Month 1-12; hour 0-24, or negative for the preset's fixed hour.
    void setTime(unsigned month, double hour);
    // Firmware draws the glint pass (draw mask bit 1); off hides it.
    void setGlints(bool enabled) { glints_ = enabled; }
    void show(const OriginalSimulation::Frame& frame, QSize size);
    void render(GLuint target, QSize size) { display_->render(target,size); }
    QImage capture(QSize size);
private:
    void build();
    QJsonObject backgroundTime() const;
    QString shaders_, reference_, preset_;
    OriginalSimulation::Constants constants_;
    std::map<QString,float> line_, background_, hdr_;
    unsigned month_ = 0;
    double hour_ = 0;
    bool glints_ = true;
    std::unique_ptr<Renderer> display_;
};

// Simulation and display sharing one context and thread (preview and captures).
class OriginalScene {
public:
    OriginalScene(const QString& shaders, const QString& reference, const QString& cpu,
                  const QString& spline, const QString& preset, unsigned month = 7, double hour = -1);
    void advance(double seconds, QSize size);
    void render(GLuint target, QSize size) { display_.render(target,size); }
    QImage capture(QSize size) { return display_.capture(size); }
    const std::vector<float>& mesh() const { return frame_.vertices; }
private:
    OriginalSimulation simulation_;
    OriginalDisplay display_;
    OriginalSimulation::Frame frame_;
};
