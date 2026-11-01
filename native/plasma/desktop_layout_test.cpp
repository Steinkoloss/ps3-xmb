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
    if (!failures) std::printf("desktop layout ok\n");
    return failures ? 1 : 0;
}
