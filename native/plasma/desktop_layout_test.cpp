// Checks physicalLayout (DesktopLayout.h); exits non-zero on failure.
#include "DesktopLayout.h"
#include <cstdio>

static int failures = 0;
static void expect(bool condition, const char* what) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
static bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

int main() {
    // 24" 1080p left of a 27" 1440p, bottoms aligned (logical y = 360).
    {
        const std::vector<LayoutScreen> screens{{QRect(0, 360, 1920, 1080), QSizeF(527, 296)},
                                                {QRect(1920, 0, 2560, 1440), QSizeF(590, 334)}};
        const auto r = physicalLayout(screens, 1);
        expect(near(r[0].right(), r[1].left()), "side-by-side screens touch");
        expect(near(r[0].bottom(), r[1].bottom()), "bottom-aligned screens stay bottom-aligned");
        expect(near(r[0].width(), 527), "screen keeps its physical width");
        expect(near(r[0].height(), 527.0 / 1920 * 1080), "height follows the pixel pitch");
    }
    // Left screen lower than the right one's bottom: the offset follows the neighbour's pitch.
    {
        const std::vector<LayoutScreen> screens{{QRect(0, 563, 1920, 1080), QSizeF(527, 296)},
                                                {QRect(1920, 0, 2560, 1440), QSizeF(590, 334)}};
        const auto r = physicalLayout(screens, 1);
        expect(near(r[0].bottom(), r[1].bottom() + 203 * 590.0 / 2560), "bottom offset at the neighbour's pitch");
    }
    // Tops aligned, screen on the right of the reference.
    {
        const std::vector<LayoutScreen> screens{{QRect(0, 0, 2560, 1440), QSizeF(590, 334)},
                                                {QRect(2560, 0, 1920, 1080), QSizeF(527, 296)}};
        const auto r = physicalLayout(screens, 0);
        expect(near(r[1].left(), r[0].right()) && near(r[1].top(), r[0].top()), "top-aligned right screen");
    }
    // Stacked vertically, centred: anchors the nearer horizontal edge.
    {
        const std::vector<LayoutScreen> screens{{QRect(0, 0, 2560, 1440), QSizeF(590, 334)},
                                                {QRect(320, 1440, 1920, 1080), QSizeF(527, 296)}};
        const auto r = physicalLayout(screens, 0);
        expect(near(r[1].top(), r[0].bottom()), "stacked screens touch");
        expect(near(r[1].left(), r[0].left() + 320 * 590.0 / 2560), "stacked offset at the neighbour's pitch");
    }
    // Chain of three, and a screen without a physical size (96 dpi).
    {
        const std::vector<LayoutScreen> screens{{QRect(0, 0, 1920, 1080), QSizeF()},
                                                {QRect(1920, 0, 1920, 1080), QSizeF(527, 296)},
                                                {QRect(3840, 0, 1920, 1080), QSizeF(527, 296)}};
        const auto r = physicalLayout(screens, 1);
        expect(near(r[0].right(), r[1].left()) && near(r[1].right(), r[2].left()), "chained screens touch");
        expect(near(r[0].width(), 1920 * 25.4 / 96), "unknown size assumes 96 dpi");
    }
    // A detached screen keeps its logical position at the reference pitch.
    {
        const std::vector<LayoutScreen> screens{{QRect(0, 0, 1920, 1080), QSizeF(527, 296)},
                                                {QRect(5000, 0, 1920, 1080), QSizeF(527, 296)}};
        const auto r = physicalLayout(screens, 0);
        expect(near(r[1].left(), 5000 * 527.0 / 1920), "detached screen position");
    }
    // Scene axis: a slightly offset side-by-side pair snaps level, centred between the centres.
    {
        const std::vector<QRectF> screens{QRectF(-84, 35, 527, 296), QRectF(443, 0, 590, 332)};
        const auto f = sceneFrame(screens);
        expect(near(f.angle, 0), "slightly offset screens snap level");
        expect(near(f.anchor.y(), (screens[0].center().y() + screens[1].center().y()) / 2), "axis runs between the centres");
        const auto left = f.corners(screens[0]), right = f.corners(screens[1]);
        expect(near(left[1].x(), right[0].x()), "touching screens meet on the canvas");
        for (const auto& corner : {left[0], left[2], right[0], right[2]})
            expect(corner.x() > -1e-9 && corner.x() < 1 + 1e-9 && corner.y() > -1e-9 && corner.y() < 1 + 1e-9, "screens lie on the canvas");
    }
    // A stacked pair a little off to the side snaps vertical; a clear diagonal keeps its angle.
    {
        const auto vertical = sceneFrame({QRectF(0, 0, 590, 332), QRectF(60, 332, 527, 296)});
        expect(near(vertical.angle, 3.14159265358979323846 / 2), "slightly offset stack snaps vertical");
        const std::vector<QRectF> diagonal{QRectF(0, 0, 527, 296), QRectF(527, 450, 590, 332)};
        const auto f = sceneFrame(diagonal);
        const QPointF d = diagonal[1].center() - diagonal[0].center();
        expect(near(f.angle, std::atan2(d.y(), d.x())), "diagonal pair follows the centre line");
        expect(near(f.canvas(diagonal[0].center()).y(), 0.5) && near(f.canvas(diagonal[1].center()).y(), 0.5), "both centres on the axis");
    }
    // Axis modes: level ignores the tilt, exact keeps a small one.
    {
        const std::vector<QRectF> screens{QRectF(-84, 83, 527, 296), QRectF(443, 0, 590, 332)};
        const QPointF d = screens[1].center() - screens[0].center();
        expect(near(sceneFrame(screens, SceneAxis::Level).angle, 0), "level mode stays level");
        expect(near(sceneFrame(screens, SceneAxis::Exact).angle, std::atan2(d.y(), d.x())), "exact mode keeps the small tilt");
        expect(near(sceneFrame({QRectF(0, 0, 590, 332), QRectF(60, 332, 527, 296)}, SceneAxis::Level).angle, 0), "level mode for a stack");
        expect(sceneAxis("exact") == SceneAxis::Exact && sceneAxis("bogus") == SceneAxis::Automatic, "mode names");
    }
    // One screen: the canvas is the screen.
    {
        const QRectF screen(10, 20, 527, 296);
        const auto c = sceneFrame({screen}).corners(screen);
        expect(near(c[0].x(), 0) && near(c[0].y(), 0) && near(c[2].x(), 1) && near(c[2].y(), 1), "single screen fills the canvas");
    }
    if (!failures) std::printf("desktop layout ok\n");
    return failures ? 1 : 0;
}
