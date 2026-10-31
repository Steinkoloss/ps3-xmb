#include "XmbItem.h"
#include "OriginalScene.h"
#include <QDateTime>
#include <QDir>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>
#include <QStandardPaths>
#include <algorithm>
#include <atomic>
#include <functional>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

// Hand-over between the simulation thread and the scene-graph renderer.
struct XmbChannel {
    std::mutex mutex;
    QString shaders, reference, preset;
    std::optional<OriginalSimulation::Constants> constants;
    OriginalSimulation::Frame frame;
    uint64_t frameId = 0, consumed = 0;
    // Wakes the simulation when the renderer takes a frame.
    std::function<void()> taken;
};

class XmbSimulator {
public:
    XmbSimulator(XmbItem* item, const QString& data, const QString& preset) : item_(item), channel_(std::make_shared<XmbChannel>()) {
        const QDir root(data);
        channel_->shaders = root.filePath("shaders");
        channel_->reference = root.filePath("reference");
        channel_->preset = preset;
        channel_->taken = [this] { { std::lock_guard lock(control_); } wake_.notify_all(); };
        surface_.setFormat(format());
        surface_.create();
        thread_ = std::thread([this, cpu = root.filePath("cpu"), spline = root.filePath("spline/original-ls.bin")] { run(cpu,spline); });
    }
    ~XmbSimulator() {
        { std::lock_guard lock(control_); stop_ = true; }
        wake_.notify_all();
        thread_.join();
    }
    static QSurfaceFormat format() {
        QSurfaceFormat format;
        format.setVersion(4,3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        return format;
    }
    std::shared_ptr<XmbChannel> channel() const { return channel_; }
    void setSize(QSize size) { width_ = size.width(); height_ = size.height(); }
    void setRunning(bool running) {
        { std::lock_guard lock(control_); running_ = running; }
        wake_.notify_all();
    }
    void setFrameRate(int rate) { frameRate_ = std::clamp(rate, 1, 60); }
private:
    void run(const QString& cpu, const QString& spline) {
        try {
            QOpenGLContext context;
            context.setFormat(format());
            if (!context.create() || !context.makeCurrent(&surface_))
                throw std::runtime_error("Cannot create the simulation OpenGL context");
            OriginalSimulation simulation(channel_->shaders,channel_->reference,cpu,spline,channel_->preset);
            { std::lock_guard lock(channel_->mutex); channel_->constants = simulation.constants(); }
            using Clock = std::chrono::steady_clock;
            auto last = Clock::now(), next = last;
            for (;;) {
                {
                    std::unique_lock lock(control_);
                    if (!running_) {
                        wake_.wait(lock,[&] { return running_ || stop_; });
                        last = next = Clock::now();
                    }
                    if (stop_) break;
                }
                const auto now = Clock::now();
                const auto seconds = std::min(0.25,std::chrono::duration<double>(now - last).count());
                last = now;
                QSize size(width_,height_);
                if (size.isEmpty()) size = QSize(1920,1080);
                auto frame = simulation.step(seconds,size);
                {
                    std::lock_guard lock(channel_->mutex);
                    channel_->frame = std::move(frame);
                    ++channel_->frameId;
                }
                QMetaObject::invokeMethod(item_,"update",Qt::QueuedConnection);
                const auto period = std::chrono::nanoseconds(1'000'000'000 / frameRate_.load());
                next = std::max(next + period,Clock::now() - period);
                std::unique_lock lock(control_);
                wake_.wait_until(lock,next,[&] { return stop_; });
                // Simulate only frames that get shown: wait until this one is taken.
                wake_.wait(lock,[&] {
                    if (stop_) return true;
                    std::lock_guard frameLock(channel_->mutex);
                    return channel_->consumed + 1 >= channel_->frameId;  // one frame in flight
                });
                if (stop_) break;
            }
            context.doneCurrent();
        } catch (const std::exception& error) {
            QMetaObject::invokeMethod(item_,"fail",Qt::QueuedConnection,Q_ARG(QString,QString::fromUtf8(error.what())));
        }
    }
    XmbItem* item_;
    std::shared_ptr<XmbChannel> channel_;
    QOffscreenSurface surface_;
    std::thread thread_;
    std::mutex control_;
    std::condition_variable wake_;
    bool running_ = true, stop_ = false;
    std::atomic<int> width_{0}, height_{0}, frameRate_{60};
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
                qWarning("PS3 XMB background stopped: %s",error.what());
                failed_ = true; display_.reset();
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
        OriginalSimulation::Frame frame;
        uint64_t id;
        {
            std::lock_guard lock(channel_->mutex);
            if (!display_ && channel_->constants) {
                display_ = std::make_unique<OriginalDisplay>(channel_->shaders,channel_->reference,channel_->preset,
                    *channel_->constants,month,hour);
                monthShown_ = month; hourShown_ = hour;
            }
            id = channel_->frameId;
            if (display_ && id != shown_) frame = std::move(channel_->frame);
            channel_->consumed = id;
        }
        if (channel_->taken) channel_->taken();
        if (!display_) return false;
        if (month != monthShown_ || std::abs(hour - hourShown_) > 1.0 / 3600) {
            display_->setTime(month,hour);
            monthShown_ = month; hourShown_ = hour;
            if (!frame.vertices.empty()) shown_ = 0;
        }
        if (!frame.vertices.empty()) { display_->show(frame,size); shown_ = id; }
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
    bool failed_ = false;
};
}

XmbItem::XmbItem(QQuickItem* parent) : QQuickFramebufferObject(parent) {}
XmbItem::~XmbItem() = default;

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
}
void XmbItem::setRunning(bool running) {
    if (running == running_) return;
    running_ = running;
    if (simulator_) simulator_->setRunning(running);
    emit runningChanged();
}

void XmbItem::setFrameRate(int rate) {
    rate = std::clamp(rate, 1, 60);
    if (rate == frameRate_) return;
    frameRate_ = rate;
    if (simulator_) simulator_->setFrameRate(rate);
    emit frameRateChanged();
}

void XmbItem::componentComplete() {
    QQuickFramebufferObject::componentComplete();
    restart();
}

void XmbItem::geometryChange(const QRectF& next, const QRectF& previous) {
    QQuickFramebufferObject::geometryChange(next,previous);
    if (simulator_) {
        const auto ratio = window() ? window()->effectiveDevicePixelRatio() : 1.0;
        simulator_->setSize((next.size() * ratio).toSize());
    }
}

void XmbItem::restart() {
    if (!isComponentComplete()) return;
    simulator_.reset();
    if (!error_.isEmpty()) { error_.clear(); emit errorChanged(); }
    auto data = dataDirectory_;
    if (data.isEmpty()) data = QStandardPaths::locate(QStandardPaths::GenericDataLocation,"ps3-xmb",QStandardPaths::LocateDirectory);
    if (data.isEmpty()) { fail("Install the local firmware-derived data to ~/.local/share/ps3-xmb"); return; }
    simulator_ = std::make_unique<XmbSimulator>(this,data,preset_);
    const auto ratio = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    simulator_->setSize((size() * ratio).toSize());
    simulator_->setRunning(running_);
    simulator_->setFrameRate(frameRate_);
    update();
}

void XmbItem::fail(const QString& message) {
    qWarning("PS3 XMB background: %s",qUtf8Printable(message));
    error_ = message;
    emit errorChanged();
}
