#include "Renderer.h"
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLWindow>
#include <QSurfaceFormat>
#include <QTimer>
#include <cstdio>

class Preview final : public QOpenGLWindow {
public:
    Preview(QString shaders, QString graph) : shaders_(std::move(shaders)), graph_(std::move(graph)) {
        setTitle("PS3 XMB — native renderer development preview");
        resize(1280, 720);
    }
    ~Preview() override { makeCurrent(); renderer_.reset(); doneCurrent(); }
private:
    void initializeGL() override {
        try { renderer_ = std::make_unique<Renderer>(shaders_, graph_); }
        catch (const std::exception& error) { qCritical("%s", error.what()); QGuiApplication::exit(1); }
    }
    void paintGL() override {
        if (!renderer_) return;
        try { renderer_->render(defaultFramebufferObject(), size() * devicePixelRatio()); }
        catch (const std::exception& error) { qCritical("%s", error.what()); QGuiApplication::exit(1); }
    }
    QString shaders_, graph_;
    std::unique_ptr<Renderer> renderer_;
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
    parser.setApplicationDescription("Native original-program renderer. Static capture graphs validate graphics; the full live simulation is not integrated yet.");
    parser.addHelpOption();
    parser.addOptions({{{"s", "shaders"}, "Generated original shader directory", "directory"},
                       {{"g", "graph"}, "Original render-pass graph", "file"},
                       {{"o", "output"}, "Render one frame to a PNG without opening a preview", "file"},
                       {"width", "Capture width", "pixels", "1920"}, {"height", "Capture height", "pixels", "1080"}});
    parser.process(application);
    if (!parser.isSet("shaders") || !parser.isSet("graph")) parser.showHelp(2);
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
            Renderer renderer(parser.value("shaders"), parser.value("graph"));
            if (!renderer.capture(size).save(parser.value("output"))) throw std::runtime_error("Cannot save native frame");
            return 0;
        } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
    }
    Preview window(parser.value("shaders"), parser.value("graph"));
    window.show();
    return application.exec();
}
