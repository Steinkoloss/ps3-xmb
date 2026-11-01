#include "XmbItem.h"
#include "DesktopLayout.h"
#include <QTime>
#include "OriginalScene.h"
#include <QDateTime>
#include <QDir>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>
#include <QScreen>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <functional>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

// Hand-over between the simulation thread and the scene-graph renderers.
struct XmbChannel {
    std::mutex mutex;
    QString shaders, reference, preset;
    std::optional<OriginalSimulation::Constants> constants;
    std::shared_ptr<const OriginalSimulation::Frame> frame;   // latest; every screen reads it
    QRectF canvas;   // desktop area (millimetres) the latest frame was simulated for
    uint64_t frameId = 0, consumed = 0;
    // Wakes the simulation when a renderer takes a frame.
    std::function<void()> taken;
    // Reports a renderer failure to the items (queued to the GUI thread).
    std::function<void(QString)> failed;
};

// One simulation per process (the translated firmware has a single guest memory), shared
// by every wallpaper item with the same data and preset. It is simulated for the union of
// their screens, and each screen shows its part, so the scene spans a multi-monitor desktop.
class XmbSimulator {
public:
    static std::shared_ptr<XmbSimulator> acquire(const QString& data, const QString& preset, double hour) {
        static std::map<QString, std::weak_ptr<XmbSimulator>> simulators;
        auto& slot = simulators[data + QLatin1Char('\n') + preset];
        if (auto shared = slot.lock()) return shared;
        std::shared_ptr<XmbSimulator> created(new XmbSimulator(data, preset, hour));
        // Renderers keep the channel; its callbacks must not outlive the simulator.
        created->channel_->failed = [weak = std::weak_ptr<XmbSimulator>(created)](const QString& message) {
            if (auto simulator = weak.lock()) simulator->fail(message);
        };
        created->start();
        slot = created;
        return created;
    }
    ~XmbSimulator() {
        { std::lock_guard lock(control_->mutex); stop_ = true; }
        control_->wake.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    static QSurfaceFormat format() {
        QSurfaceFormat format;
        format.setVersion(4,3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        return format;
    }
    std::shared_ptr<XmbChannel> channel() const { return channel_; }
    // Subscribers (GUI thread): updated after every frame, told about failures. The
    // simulation runs while any of them runs, for the union of their desktop rectangles.
    void subscribe(XmbItem* item, QRectF area, bool running) {
        { std::lock_guard lock(items_); subscribers_[item] = {area, running}; }
        refresh();
    }
    void unsubscribe(XmbItem* item) {
        { std::lock_guard lock(items_); subscribers_.erase(item); }
        refresh();
    }
    void setArea(XmbItem* item, QRectF area) {
        { std::lock_guard lock(items_); if (subscribers_.count(item)) subscribers_[item].area = area; }
        refresh();
    }
    void setRunning(XmbItem* item, bool running) {
        { std::lock_guard lock(items_); if (subscribers_.count(item)) subscribers_[item].running = running; }
        refresh();
    }
    void setHour(double hour) { hour_ = hour; }
    void fail(const QString& message) {
        std::lock_guard lock(items_);
        for (const auto& [item, state] : subscribers_)
            QMetaObject::invokeMethod(item,"fail",Qt::QueuedConnection,Q_ARG(QString,message));
    }
private:
    struct Subscriber { QRectF area; bool running = true; };
    struct Control { std::mutex mutex; std::condition_variable wake; };
    XmbSimulator(const QString& data, const QString& preset, double hour) :
        channel_(std::make_shared<XmbChannel>()), control_(std::make_shared<Control>()), root_(data), hour_(hour) {
        channel_->shaders = root_.filePath("shaders");
        channel_->reference = root_.filePath("reference");
        channel_->preset = preset;
        channel_->taken = [control = control_] { { std::lock_guard lock(control->mutex); } control->wake.notify_all(); };
        surface_.setFormat(format());
        surface_.create();
    }
    void start() {
        thread_ = std::thread([this, cpu = root_.filePath("cpu"), spline = root_.filePath("spline/original-ls.bin")] { run(cpu,spline); });
    }
    void refresh() {
        QRectF canvas;
        bool running = false;
        {
            std::lock_guard lock(items_);
            for (const auto& [item, state] : subscribers_) {
                running = running || state.running;
                if (!state.area.isEmpty()) canvas = canvas.united(state.area);
            }
        }
        { std::lock_guard lock(canvasMutex_); canvas_ = canvas; }
        { std::lock_guard lock(control_->mutex); running_ = running; }
        control_->wake.notify_all();
    }
    void updateItems() {
        std::lock_guard lock(items_);
        for (const auto& [item, state] : subscribers_) QMetaObject::invokeMethod(item,"update",Qt::QueuedConnection);
    }
    void run(const QString& cpu, const QString& spline) {
        try {
            QOpenGLContext context;
            context.setFormat(format());
            if (!context.create() || !context.makeCurrent(&surface_))
                throw std::runtime_error("Cannot create the simulation OpenGL context");
            const double fixed = hour_;
            OriginalSimulation simulation(channel_->shaders,channel_->reference,cpu,spline,channel_->preset,
                fixed >= 0 ? fixed : QTime::currentTime().msecsSinceStartOfDay() / 3.6e6);
            { std::lock_guard lock(channel_->mutex); channel_->constants = simulation.constants(); }
            updateItems();
            using Clock = std::chrono::steady_clock;
            auto last = Clock::now(), next = last;
            for (;;) {
                {
                    std::unique_lock lock(control_->mutex);
                    if (!running_) {
                        control_->wake.wait(lock,[&] { return running_ || stop_; });
                        last = next = Clock::now();
                    }
                    if (stop_) break;
                }
                const auto now = Clock::now();
                const auto seconds = std::min(0.25,std::chrono::duration<double>(now - last).count());
                last = now;
                QRectF canvas;
                { std::lock_guard lock(canvasMutex_); canvas = canvas_; }
                if (canvas.isEmpty()) canvas = QRectF(0,0,16,9);
                const double fixed = hour_;
                simulation.setHour(fixed >= 0 ? fixed : QTime::currentTime().msecsSinceStartOfDay() / 3.6e6);
                auto frame = std::make_shared<const OriginalSimulation::Frame>(simulation.step(seconds,
                    QSize(std::max(1,int(std::lround(canvas.width() * 10))),std::max(1,int(std::lround(canvas.height() * 10))))));
                {
                    std::lock_guard lock(channel_->mutex);
                    channel_->frame = std::move(frame);
                    channel_->canvas = canvas;
                    ++channel_->frameId;
                }
                updateItems();
                const auto period = std::chrono::nanoseconds(1'000'000'000 / 60);
                next = std::max(next + period,Clock::now() - period);
                std::unique_lock lock(control_->mutex);
                control_->wake.wait_until(lock,next,[&] { return stop_; });
                // Simulate only frames that get shown: wait until a screen takes this one.
                control_->wake.wait(lock,[&] {
                    if (stop_ || !running_) return true;
                    std::lock_guard frameLock(channel_->mutex);
                    return channel_->consumed + 1 >= channel_->frameId;  // one frame in flight
                });
                if (stop_) break;
            }
            context.doneCurrent();
        } catch (const std::exception& error) {
            fail(QString::fromUtf8(error.what()));
        }
    }
    std::shared_ptr<XmbChannel> channel_;
    std::shared_ptr<Control> control_;
    QDir root_;
    QOffscreenSurface surface_;
    std::thread thread_;
    std::mutex items_;
    std::map<XmbItem*, Subscriber> subscribers_;
    bool running_ = false, stop_ = false;
    std::mutex canvasMutex_;
    QRectF canvas_;
    std::atomic<double> hour_{-1};
};

namespace {
class XmbRenderer final : public QQuickFramebufferObject::Renderer {
public:
    void synchronize(QQuickFramebufferObject* object) override {
        auto item = static_cast<XmbItem*>(object);
        auto channel = item->channel();
        if (channel != channel_) { channel_ = std::move(channel); display_.reset(); shown_ = 0; failed_ = false; }
        month_ = item->month();
        hour_ = item->hour();
        glints_ = item->glints();
        area_ = item->desktopArea();
    }
    QOpenGLFramebufferObject* createFramebufferObject(const QSize& size) override {
        QOpenGLFramebufferObjectFormat format;
        format.setInternalTextureFormat(GL_RGBA8);
        return new QOpenGLFramebufferObject(size,format);
    }
    void render() override {
        auto& gl = *QOpenGLContext::currentContext()->functions();
        const auto size = framebufferObject()->size();
        bool drawn = false;
        if (channel_ && !failed_) {
            try { drawn = draw(size); }
            catch (const std::exception& error) {
                failed_ = true; display_.reset();
                if (channel_->failed) channel_->failed(QString::fromUtf8(error.what()));
            }
        }
        framebufferObject()->bind();
        if (!drawn) { gl.glClearColor(0,0,0,1); gl.glClear(GL_COLOR_BUFFER_BIT); }
        else {
            // The original display leaves alpha at zero; the desktop needs an opaque surface.
            gl.glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_TRUE);
            gl.glClearColor(0,0,0,1);
            gl.glClear(GL_COLOR_BUFFER_BIT);
            gl.glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        }
        QQuickOpenGLUtils::resetOpenGLState();
    }
private:
    bool draw(QSize size) {
        const auto now = QDateTime::currentDateTime();
        const unsigned month = month_ > 0 ? unsigned(month_) : unsigned(now.date().month());
        const double hour = hour_ >= 0 ? hour_ : now.time().msecsSinceStartOfDay() / 3.6e6;
        std::shared_ptr<const OriginalSimulation::Frame> frame;
        QRectF canvas;
        uint64_t id;
        {
            std::lock_guard lock(channel_->mutex);
            if (!display_ && channel_->constants) {
                display_ = std::make_unique<OriginalDisplay>(channel_->shaders,channel_->reference,channel_->preset,
                    *channel_->constants,month,hour);
                monthShown_ = month; hourShown_ = hour;
            }
            id = channel_->frameId;
            if (display_ && id != shown_) { frame = channel_->frame; canvas = channel_->canvas; }
            channel_->consumed = std::max(channel_->consumed, id);
        }
        if (channel_->taken) channel_->taken();
        if (!display_) return false;
        if (month != monthShown_ || std::abs(hour - hourShown_) > 1.0 / 3600) {
            display_->setTime(month,hour);
            monthShown_ = month; hourShown_ = hour;
            if (frame) shown_ = 0;
        }
        display_->setGlints(glints_);
        if (frame && !frame->vertices.empty()) {
            // This screen's part of the canvas the frame was simulated for.
            QRectF viewport(0,0,1,1);
            if (!canvas.isEmpty() && !area_.isEmpty() && canvas.adjusted(-0.01,-0.01,0.01,0.01).contains(area_))
                viewport = QRectF(double(area_.x() - canvas.x()) / canvas.width(),double(area_.y() - canvas.y()) / canvas.height(),
                    double(area_.width()) / canvas.width(),double(area_.height()) / canvas.height());
            if (viewport != viewport_) {
                viewport_ = viewport;
                qInfo("PS3 XMB background: canvas %.0f,%.0f %.0fx%.0f mm, screen %.0f,%.0f %.0fx%.0f mm, viewport %.4f,%.4f %.4fx%.4f, target %dx%d",
                    canvas.x(),canvas.y(),canvas.width(),canvas.height(),area_.x(),area_.y(),area_.width(),area_.height(),
                    viewport.x(),viewport.y(),viewport.width(),viewport.height(),size.width(),size.height());
            }
            display_->setViewport(viewport);
            display_->show(*frame,size);
            shown_ = id;
        }
        else if (!shown_) return false;
        display_->render(framebufferObject()->handle(),size);
        return true;
    }
    std::shared_ptr<XmbChannel> channel_;
    std::unique_ptr<OriginalDisplay> display_;
    uint64_t shown_ = 0;
    int month_ = 0;
    double hour_ = -1;
    unsigned monthShown_ = 0;
    double hourShown_ = 0;
    bool failed_ = false, glints_ = true;
    QRectF area_;
    QRectF viewport_;
};
}

XmbItem::XmbItem(QQuickItem* parent) : QQuickFramebufferObject(parent) {}
XmbItem::~XmbItem() { if (simulator_) simulator_->unsubscribe(this); }

QQuickFramebufferObject::Renderer* XmbItem::createRenderer() const { return new XmbRenderer; }

std::shared_ptr<XmbChannel> XmbItem::channel() const { return simulator_ ? simulator_->channel() : nullptr; }

void XmbItem::setDataDirectory(const QString& directory) {
    if (directory == dataDirectory_) return;
    dataDirectory_ = directory; emit sourceChanged(); restart();
}
void XmbItem::setPreset(const QString& preset) {
    if (preset == preset_) return;
    preset_ = preset; emit sourceChanged(); restart();
}
void XmbItem::setMonth(int month) {
    if (month == month_ || month < 0 || month > 12) return;
    month_ = month; emit timeChanged(); update();
}
void XmbItem::setHour(double hour) {
    if (hour == hour_ || hour > 24) return;
    hour_ = hour; emit timeChanged(); update();
    if (simulator_) simulator_->setHour(hour);
}
void XmbItem::setRunning(bool running) {
    if (running == running_) return;
    running_ = running;
    if (simulator_) simulator_->setRunning(this, running);
    emit runningChanged();
}

void XmbItem::setFrameRate(int rate) {
    rate = std::clamp(rate, 1, 60);
    if (rate == frameRate_) return;
    frameRate_ = rate;
    emit frameRateChanged();
}

void XmbItem::setGlints(bool enabled) {
    if (enabled == glints_) return;
    glints_ = enabled;
    emit glintsChanged();
    update();
}

void XmbItem::componentComplete() {
    QQuickFramebufferObject::componentComplete();
    restart();
}

void XmbItem::geometryChange(const QRectF& next, const QRectF& previous) {
    QQuickFramebufferObject::geometryChange(next,previous);
    areaChanged();
}

// This screen's place in the physical desktop (DesktopLayout.h), from every screen's
// logical geometry and reported size.
static QRectF physicalScreen(const QScreen* target) {
    const auto screens = QGuiApplication::screens();
    std::vector<LayoutScreen> layout;
    size_t reference = 0, index = screens.size();
    for (qsizetype i = 0; i < screens.size(); ++i) {
        layout.push_back({screens[i]->geometry(), screens[i]->physicalSize()});
        if (screens[i] == QGuiApplication::primaryScreen()) reference = size_t(i);
        if (screens[i] == target) index = size_t(i);
    }
    if (index == size_t(screens.size())) return {};
    return physicalLayout(layout, reference)[index];
}

QRectF XmbItem::desktopArea() const {
    std::lock_guard lock(areaMutex_);
    return area_;
}

void XmbItem::areaChanged() {
    QRectF area;
    if (auto* w = window(); w && w->screen()) {
        const auto screen = physicalScreen(w->screen());
        if (!screen.isEmpty()) {
            const double p = layoutPitch({w->screen()->geometry(), w->screen()->physicalSize()});
            area = QRectF(screen.topLeft() + mapToScene(QPointF()) * p,size() * p);
        }
    }
    { std::lock_guard lock(areaMutex_); area_ = area; }
    if (simulator_) simulator_->setArea(this, area);
    update();
}

void XmbItem::itemChange(ItemChange change, const ItemChangeData& data) {
    QQuickFramebufferObject::itemChange(change,data);
    if (change != ItemSceneChange) return;
    for (const auto& connection : layoutConnections_) QObject::disconnect(connection);
    layoutConnections_.clear();
    if (!data.window) return;
    // Any change to the desktop layout (screens added, removed, moved, resized, rescaled)
    // re-places every screen, so watch them all.
    const auto watchAll = [this] {
        for (const auto& connection : screenConnections_) QObject::disconnect(connection);
        screenConnections_.clear();
        for (auto* screen : QGuiApplication::screens()) {
            screenConnections_.push_back(connect(screen,&QScreen::geometryChanged,this,[this] { areaChanged(); }));
            screenConnections_.push_back(connect(screen,&QScreen::physicalSizeChanged,this,[this] { areaChanged(); }));
        }
        areaChanged();
    };
    layoutConnections_.push_back(connect(data.window,&QWindow::screenChanged,this,[this] { areaChanged(); }));
    layoutConnections_.push_back(connect(qGuiApp,&QGuiApplication::screenAdded,this,watchAll));
    layoutConnections_.push_back(connect(qGuiApp,&QGuiApplication::screenRemoved,this,watchAll));
    layoutConnections_.push_back(connect(qGuiApp,&QGuiApplication::primaryScreenChanged,this,[this] { areaChanged(); }));
    watchAll();
}

void XmbItem::restart() {
    if (!isComponentComplete()) return;
    if (simulator_) simulator_->unsubscribe(this);
    simulator_.reset();
    if (!error_.isEmpty()) { error_.clear(); emit errorChanged(); }
    auto data = dataDirectory_;
    if (data.isEmpty()) data = QStandardPaths::locate(QStandardPaths::GenericDataLocation,"ps3-xmb",QStandardPaths::LocateDirectory);
    if (data.isEmpty()) { fail("Install the local firmware-derived data to ~/.local/share/ps3-xmb"); return; }
    // The base menus lack the background's day/night entries; the console always applies a
    // set, so an unset preset follows the clock like the console.
    simulator_ = XmbSimulator::acquire(data,preset_.isEmpty() ? QStringLiteral("clock") : preset_,hour_);
    QTimer::singleShot(120000,this,[this] { if (error_.isEmpty()) retryDelay_ = 2; });
    simulator_->subscribe(this,desktopArea(),running_);
    if (hour_ >= 0) simulator_->setHour(hour_);
    update();
}

void XmbItem::fail(const QString& message) {
    // Keep the wallpaper alive: restart the simulation, backing off on repeated failures.
    qWarning("PS3 XMB background: %s; restarting in %d s",qUtf8Printable(message),retryDelay_);
    error_ = message;
    emit errorChanged();
    QTimer::singleShot(retryDelay_ * 1000,this,[this] { restart(); });
    retryDelay_ = std::min(retryDelay_ * 2,60);
}
