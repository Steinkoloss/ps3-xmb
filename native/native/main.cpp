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
#include <QFile>
#include <QtEndian>
#include <bit>
#include <cstdio>

struct ScenePaths { QString reference, cpu, spline, preset; unsigned month; double hour; };

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
                original_ = std::make_unique<OriginalScene>(shaders_,paths_.reference,paths_.cpu,paths_.spline,paths_.preset,paths_.month,paths_.hour);
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
                       {"month", "Original month colour, 1 through 12", "number", "7"},
                       {"hour", "Original background clock hour, or -1 for the selected preset", "hour", "-1"},
                       {"frames", "Number of original 60 Hz updates before a capture", "count", "121"},
                       {"mesh-checkpoint", "Save the live 128 x 128 clip-space surface for comparison", "file"},
                       {"width", "Capture width", "pixels", "1920"}, {"height", "Capture height", "pixels", "1080"},
                       {"viewport", "Show only this part of the width x height canvas (live scene)", "x,y,w,h"},
                       {"sequence", "Also save every Nth update as output-NNNNN.png (live scene)", "count", "0"}});
    parser.process(application);
    if (!parser.isSet("shaders") || (!parser.isSet("graph") && !parser.isSet("reference"))) parser.showHelp(2);
    ScenePaths paths{parser.value("reference"),parser.value("cpu"),parser.value("spline"),parser.value("preset"),
        parser.value("month").toUInt(),parser.value("hour").toDouble()};
    if (parser.isSet("output")) {
        try {
            QSize size(parser.value("width").toInt(), parser.value("height").toInt());
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
                OriginalScene scene(parser.value("shaders"),paths.reference,paths.cpu,paths.spline,paths.preset,paths.month,paths.hour);
                const auto canvas = size;
                QRect viewport(QPoint(),canvas);
                if (parser.isSet("viewport")) {
                    const auto fields = parser.value("viewport").split(',');
                    if (fields.size() != 4) throw std::runtime_error("Invalid viewport");
                    viewport = QRect(fields[0].toInt(),fields[1].toInt(),fields[2].toInt(),fields[3].toInt());
                    if (viewport.isEmpty() || !QRect(QPoint(),canvas).contains(viewport)) throw std::runtime_error("Viewport outside the canvas");
                    size = viewport.size();
                }
                if (!frames) scene.advance(0,canvas,viewport);
                const auto every = parser.value("sequence").toUInt();
                for (unsigned i = 0; i < frames; ++i) {
                    scene.advance(1.0/60,canvas,viewport);
                    if (every && (i + 1) % every == 0) {
                        auto name = parser.value("output");
                        name.insert(name.lastIndexOf('.'), QString("-%1").arg(i + 1, 5, 10, QChar('0')));
                        if (!scene.capture(size).save(name)) throw std::runtime_error("Cannot save sequence frame");
                    }
                }
                frame = scene.capture(size);
                if (parser.isSet("mesh-checkpoint")) {
                    QByteArray bytes;
                    for (float value : scene.mesh()) {
                        const auto word = qToLittleEndian(std::bit_cast<uint32_t>(value));
                        bytes.append(reinterpret_cast<const char*>(&word),sizeof word);
                    }
                    QFile output(parser.value("mesh-checkpoint"));
                    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size())
                        throw std::runtime_error("Cannot save original surface checkpoint");
                }
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
