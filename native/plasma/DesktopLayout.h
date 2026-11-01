#pragma once
#include <QRect>
#include <QRectF>
#include <QSizeF>
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
