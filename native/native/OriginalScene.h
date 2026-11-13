#pragma once
#include <array>
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
    // Part of the simulated canvas this display shows (top-left origin, 0-1): screens of a
    // multi-monitor desktop each show their slice of one wider scene.
    void setViewport(QRectF viewport) {
        setScreen({viewport.topLeft(),viewport.topRight(),viewport.bottomRight(),viewport.bottomLeft()},1);
    }
    // General form: the screen's corners (top-left, top-right, bottom-right, bottom-left) on the
    // canvas (0-1, top-left origin) of the given width/height aspect. A rotated screen renders
    // the corners' bounding box and maps it onto the target.
    void setScreen(const std::array<QPointF,4>& corners, double canvasAspect) { corners_ = corners; aspect_ = canvasAspect; }
    void show(const OriginalSimulation::Frame& frame, QSize size);
    void render(GLuint target, QSize size) { display_->render(target,size); }
    QImage capture(QSize size);
private:
    void build();
    QJsonObject backgroundTime() const;
    void applyMenus(const OriginalSimulation::Frame& frame);
    unsigned menus_ = 0;
    QString shaders_, reference_, preset_;
    OriginalSimulation::Constants constants_;
    std::map<QString,float> line_, background_, hdr_;
    unsigned month_ = 0;
    double hour_ = 0;
    bool glints_ = true;
    std::array<QPointF,4> corners_{QPointF(0,0),QPointF(1,0),QPointF(1,1),QPointF(0,1)};
    double aspect_ = 1;
    std::unique_ptr<Renderer> display_;
};

// Simulation and display sharing one context and thread (preview and captures).
class OriginalScene {
public:
    OriginalScene(const QString& shaders, const QString& reference, const QString& cpu,
                  const QString& spline, const QString& preset, unsigned month = 7, double hour = -1);
    void advance(double seconds, QSize size);
    // Simulate for the whole canvas, display only the viewport (pixels within the canvas).
    void advance(double seconds, QSize canvas, QRect viewport);
    // Simulate for a canvas (only its aspect matters) and show one screen of it: corners on the
    // canvas as OriginalDisplay::setScreen, rendered at the screen's pixel size.
    void advance(double seconds, QSize canvas, const std::array<QPointF,4>& corners, QSize screen);
    void render(GLuint target, QSize size) { display_.render(target,size); }
    QImage capture(QSize size) { return display_.capture(size); }
    const std::vector<float>& mesh() const { return frame_.vertices; }
    void setPointer(std::optional<std::array<float,2>> pointer) { simulation_.setPointer(pointer); }
    // Month colour and time of day (the "clock" preset follows the hour).
    void setTime(unsigned month, double hour) { simulation_.setHour(hour); display_.setTime(month, hour); }
private:
    OriginalSimulation simulation_;
    OriginalDisplay display_;
    OriginalSimulation::Frame frame_;
};
