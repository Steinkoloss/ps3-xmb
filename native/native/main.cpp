#include "Renderer.h"
#ifdef MOYOU_HAVE_LIVE_SCENE
#include "OriginalScene.h"
#endif
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLWindow>
#include <QSurfaceFormat>
#include <QTimer>
#include <QElapsedTimer>
#include <cstdio>

struct ScenePaths { QString reference, cpu, spline, preset; };

class Preview final : public QOpenGLWindow {
public:
    Preview(QString shaders, QString graph, ScenePaths scene) : shaders_(std::move(shaders)), graph_(std::move(graph)), paths_(std::move(scene)) {
        setTitle("PS3 XMB — native renderer development preview");
        resize(1280, 720);
    }
    ~Preview() override {
        makeCurrent();
#ifdef MOYOU_HAVE_LIVE_SCENE
        original_.reset();
#endif
        renderer_.reset(); doneCurrent();
    }
private:
    void initializeGL() override {
        try {
#ifdef MOYOU_HAVE_LIVE_SCENE
            if (!paths_.reference.isEmpty()) {
                original_ = std::make_unique<OriginalScene>(shaders_,paths_.reference,paths_.cpu,paths_.spline,paths_.preset);
                clock_.start();
                return;
            }
#endif
            renderer_ = std::make_unique<Renderer>(shaders_, graph_);
        }
        catch (const std::exception& error) { qCritical("%s", error.what()); QGuiApplication::exit(1); }
    }
    void paintGL() override {
        try {
#ifdef MOYOU_HAVE_LIVE_SCENE
            if (original_) {
                auto delta = std::min(0.25,double(clock_.nsecsElapsed()) / 1e9);
                clock_.restart();
                original_->advance(delta,size() * devicePixelRatio());
                original_->render(defaultFramebufferObject(),size() * devicePixelRatio());
                update();
                return;
            }
#endif
            if (renderer_) renderer_->render(defaultFramebufferObject(), size() * devicePixelRatio());
        }
        catch (const std::exception& error) { qCritical("%s", error.what()); QGuiApplication::exit(1); }
    }
    QString shaders_, graph_;
    std::unique_ptr<Renderer> renderer_;
    ScenePaths paths_;
    QElapsedTimer clock_;
#ifdef MOYOU_HAVE_LIVE_SCENE
    std::unique_ptr<OriginalScene> original_;
#endif
};

int main(int argc, char** argv) {
    QSurfaceFormat format;
    format.setVersion(4, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName("moyou-preview");
    QCommandLineParser parser;
    parser.setApplicationDescription("Native original-program renderer and live-wave validation preview. Complete scene fidelity remains under validation.");
    parser.addHelpOption();
    parser.addOptions({{{"s", "shaders"}, "Generated original shader directory", "directory"},
                       {{"g", "graph"}, "Original render-pass graph", "file"},
                       {{"o", "output"}, "Render one frame to a PNG without opening a preview", "file"},
                       {"reference", "Verified original renderer resources for the live scene", "directory"},
                       {"cpu", "Private original CPU translation directory", "directory"},
                       {"spline", "Verified original spline local store", "file"},
                       {"preset", "Original time-of-day preset", "name", "night"},
                       {"frames", "Number of original 60 Hz updates before a capture", "count", "121"},
                       {"width", "Capture width", "pixels", "1920"}, {"height", "Capture height", "pixels", "1080"}});
    parser.process(application);
    if (!parser.isSet("shaders") || (!parser.isSet("graph") && !parser.isSet("reference"))) parser.showHelp(2);
    ScenePaths paths{parser.value("reference"),parser.value("cpu"),parser.value("spline"),parser.value("preset")};
    if (parser.isSet("output")) {
        try {
            const QSize size(parser.value("width").toInt(), parser.value("height").toInt());
            if (size.width() < 1 || size.height() < 1 || size.width() > 8192 || size.height() > 8192)
                throw std::runtime_error("Invalid capture dimensions");
            QOpenGLContext context;
            context.setFormat(format);
            if (!context.create()) throw std::runtime_error("Cannot create native OpenGL context");
            QOffscreenSurface surface;
            surface.setFormat(context.format());
            surface.create();
            if (!context.makeCurrent(&surface)) throw std::runtime_error("Cannot activate native OpenGL context");
            QImage frame;
            if (parser.isSet("reference")) {
#ifdef MOYOU_HAVE_LIVE_SCENE
                bool valid;
                auto frames = parser.value("frames").toUInt(&valid);
                if (!valid || frames > 36000) throw std::runtime_error("Invalid original frame count");
                OriginalScene scene(parser.value("shaders"),paths.reference,paths.cpu,paths.spline,paths.preset);
                if (!frames) scene.advance(0,size);
                for (unsigned i = 0; i < frames; ++i) scene.advance(1.0/60,size);
                frame = scene.capture(size);
#else
                throw std::runtime_error("Build the original CPU/SPU translations to enable the live scene");
#endif
            } else {
                Renderer renderer(parser.value("shaders"), parser.value("graph"));
                frame = renderer.capture(size);
            }
            if (!frame.save(parser.value("output"))) throw std::runtime_error("Cannot save native frame");
            return 0;
        } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
    }
    Preview window(parser.value("shaders"), parser.value("graph"),paths);
    window.show();
    return application.exec();
}
