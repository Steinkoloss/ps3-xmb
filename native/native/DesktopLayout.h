#pragma once
#include <QRect>
#include <QString>
#include <QRectF>
#include <QSizeF>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

// Physical desktop layout (millimetres) from the logical screen layout. Screens differ in
// pixel pitch, so the logical layout alone would show a spanning scene at a different size on
// each. Each screen keeps its physical size; screens that touch in the logical layout touch
// physically, aligned at the nearer matching edge (tops or bottoms, lefts or rights) plus the
// remaining offset at the neighbour's pitch. Screens touching nothing keep their logical
// position at the reference screen's pitch.
struct LayoutScreen {
    QRect geometry;     // logical pixels, desktop coordinates
    QSizeF millimetres; // reported physical size; empty if unknown (96 dpi is assumed)
};

inline double layoutPitch(const LayoutScreen& screen) {
    if (screen.millimetres.width() > 0 && screen.geometry.width() > 0)
        return screen.millimetres.width() / screen.geometry.width();
    return 25.4 / 96;
}

inline std::vector<QRectF> physicalLayout(const std::vector<LayoutScreen>& screens, size_t reference) {
    std::vector<QRectF> placed(screens.size());
    std::vector<bool> done(screens.size(), false);
    if (screens.empty()) return placed;
    reference = std::min(reference, screens.size() - 1);
    const auto& r = screens[reference];
    placed[reference] = QRectF(QPointF(r.geometry.topLeft()) * layoutPitch(r), QSizeF(r.geometry.size()) * layoutPitch(r));
    done[reference] = true;
    for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < screens.size(); ++i) {
            if (done[i]) continue;
            const QRect s = screens[i].geometry;
            const QSizeF size = QSizeF(s.size()) * layoutPitch(screens[i]);
            for (size_t j = 0; j < screens.size() && !done[i]; ++j) {
                if (!done[j]) continue;
                const QRect n = screens[j].geometry;
                const QRectF& physical = placed[j];
                const double p = layoutPitch(screens[j]);
                const bool sideBySide = (s.left() == n.right() + 1 || s.right() + 1 == n.left()) && s.top() <= n.bottom() && s.bottom() >= n.top();
                const bool stacked = (s.top() == n.bottom() + 1 || s.bottom() + 1 == n.top()) && s.left() <= n.right() && s.right() >= n.left();
                if (!sideBySide && !stacked) continue;
                QPointF at;
                if (sideBySide) {
                    at.setX(s.left() > n.left() ? physical.right() : physical.left() - size.width());
                    const int top = s.top() - n.top(), bottom = (s.bottom() + 1) - (n.bottom() + 1);
                    at.setY(std::abs(bottom) < std::abs(top) ? physical.bottom() + bottom * p - size.height() : physical.top() + top * p);
                } else {
                    at.setY(s.top() > n.top() ? physical.bottom() : physical.top() - size.height());
                    const int left = s.left() - n.left(), right = (s.right() + 1) - (n.right() + 1);
                    at.setX(std::abs(right) < std::abs(left) ? physical.right() + right * p - size.width() : physical.left() + left * p);
                }
                placed[i] = QRectF(at, size);
                done[i] = progress = true;
            }
        }
    }
    for (size_t i = 0; i < screens.size(); ++i)
        if (!done[i])
            placed[i] = QRectF(QPointF(screens[i].geometry.topLeft()) * layoutPitch(r), QSizeF(screens[i].geometry.size()) * layoutPitch(screens[i]));
    return placed;
}

// Where the scene lies on a multi-screen desktop. The scene's horizontal axis runs along the
// line through the screen centres (their principal axis for three or more), snapped to exactly
// horizontal or vertical unless it is clearly diagonal, so screens that are only slightly offset
// show a level scene. The canvas is centred on that axis and covers every screen.
struct SceneFrame {
    double angle = 0;      // axis direction in radians, desktop coordinates (y down), (-pi/2, pi/2]
    QPointF anchor;        // millimetres; a point on the axis (the mean screen centre)
    double uMin = -0.5, uMax = 0.5;  // canvas extent along the axis, millimetres from anchor
    double half = 0.5;     // canvas half-height across the axis, millimetres

    double aspect() const { return (uMax - uMin) / (2 * half); }
    // Canvas coordinates (0-1, top-left origin) of a desktop point in millimetres.
    QPointF canvas(QPointF point) const {
        const double c = std::cos(angle), s = std::sin(angle);
        const QPointF d = point - anchor;
        const double u = d.x() * c + d.y() * s, v = -d.x() * s + d.y() * c;
        return {(u - uMin) / (uMax - uMin), (v + half) / (2 * half)};
    }
    // A screen's corners on the canvas: top-left, top-right, bottom-right, bottom-left.
    std::array<QPointF, 4> corners(const QRectF& screen) const {
        return {canvas(screen.topLeft()), canvas(screen.topRight()), canvas(screen.bottomRight()), canvas(screen.bottomLeft())};
    }
};

// How the scene axis follows the screens: snapped to level/upright when within 20 degrees,
// always level, or exactly along the centre line.
enum class SceneAxis { Automatic, Level, Exact };

inline SceneAxis sceneAxis(const QString& name) {
    if (name == QLatin1String("level")) return SceneAxis::Level;
    if (name == QLatin1String("exact")) return SceneAxis::Exact;
    return SceneAxis::Automatic;
}

inline SceneFrame sceneFrame(const std::vector<QRectF>& screens, SceneAxis mode = SceneAxis::Automatic, double snapDegrees = 20) {
    SceneFrame frame;
    if (screens.empty()) return frame;
    constexpr double pi = 3.14159265358979323846;
    QPointF mean;
    for (const auto& screen : screens) mean += screen.center();
    mean /= double(screens.size());
    double angle = 0;
    double xx = 0, yy = 0, xy = 0;
    for (const auto& screen : screens) {
        const QPointF d = screen.center() - mean;
        xx += d.x() * d.x(); yy += d.y() * d.y(); xy += d.x() * d.y();
    }
    if (xx + yy > 1e-9) angle = 0.5 * std::atan2(2 * xy, xx - yy);   // (-pi/2, pi/2]
    const double snap = snapDegrees * pi / 180;
    if (mode == SceneAxis::Level) angle = 0;
    else if (mode == SceneAxis::Automatic) {
        if (std::abs(angle) <= snap) angle = 0;
        else if (std::abs(angle) >= pi / 2 - snap) angle = pi / 2;
    }
    frame.angle = angle;
    frame.anchor = mean;
    const double c = std::cos(angle), s = std::sin(angle);
    double uMin = 1e300, uMax = -1e300, half = 0;
    for (const auto& screen : screens)
        for (const QPointF corner : {screen.topLeft(), screen.topRight(), screen.bottomRight(), screen.bottomLeft()}) {
            const QPointF d = corner - mean;
            const double u = d.x() * c + d.y() * s, v = -d.x() * s + d.y() * c;
            uMin = std::min(uMin, u); uMax = std::max(uMax, u);
            half = std::max(half, std::abs(v));
        }
    frame.uMin = uMin; frame.uMax = uMax; frame.half = std::max(half, 1e-6);
    return frame;
}
