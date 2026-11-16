// SPDX-License-Identifier: GPL-2.0-or-later
// PS3 XMB wallpaper as a standalone app: one OpenGL window spanning every monitor, placed
// behind the desktop icons on Windows (the WorkerW layer that Explorer paints the wallpaper
// in), with a tray icon. On first start it imports the renderer's data from Sony's PS3 system
// software (downloaded from Sony unless a PS3UPDAT.PUP is given) with the bundled Python.
//   ps3-xmb [--window] [--import PS3UPDAT.PUP] [--snapshot frame.png]
// It needs OpenGL 4.3 from the graphics driver and says so when the driver has less.
#include "OriginalScene.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QCursor>
#include <QDate>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLockFile>
#include <QMenu>
#include <QMessageBox>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLWindow>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QSysInfo>
#include <QSystemTrayIcon>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>
#include <cstdio>
#include <cstring>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
QString dataDirectory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath("ps3-xmb");
}
bool imported() { return QFile::exists(QDir(dataDirectory()).filePath("cpu/renderer-image.bin")); }
double hourNow() { return QTime::currentTime().msecsSinceStartOfDay() / 3.6e6; }

// Qt messages and errors go to ps3-xmb.log in the data directory (for problem reports).
void logMessage(QtMsgType, const QMessageLogContext&, const QString& message) {
    QDir().mkpath(dataDirectory());
    QFile log(QDir(dataDirectory()).filePath("ps3-xmb.log"));
    if (log.size() > (1 << 20)) log.remove();
    const auto line = (QDateTime::currentDateTime().toString(Qt::ISODate) + " " + message + "\n").toUtf8();
    if (log.open(QIODevice::Append | QIODevice::Text)) log.write(line);
    std::fputs(line.constData(), stderr);
}

#ifdef _WIN32
// A crash goes to the log with its code and where it happened (module + offset), so a report
// can be traced back to the source.
LONG WINAPI logCrash(EXCEPTION_POINTERS* info) {
    const auto address = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    HMODULE module = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module))
        GetModuleFileNameW(module, name, MAX_PATH);
    const wchar_t* base = wcsrchr(name, L'\\');
    qCritical("Crashed: exception %08lx at %ls+%llx", info->ExceptionRecord->ExceptionCode, base ? base + 1 : name,
              static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
    return EXCEPTION_CONTINUE_SEARCH;
}
// The window Explorer paints the wallpaper in, behind the icons. Asking Progman for 0x052C
// makes Explorer create it. Before Windows 11 24H2 it is a top-level WorkerW after the one that
// holds SHELLDLL_DefView; since 24H2 it is a WorkerW child of Progman, and our window goes
// into Progman just behind the icon view.
struct DesktopLayer { HWND parent = nullptr, iconView = nullptr; };
DesktopLayer desktopLayer() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return {};
    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, nullptr);
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);
    HWND worker = nullptr;
    EnumWindows([](HWND top, LPARAM result) -> BOOL {
        if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr))
            *reinterpret_cast<HWND*>(result) = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&worker));
    if (worker) return {worker, nullptr};
    return {progman, FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr)};
}
// A foreground window covering its whole monitor (a game, a video): pause to leave it the GPU.
bool fullscreenAppInFront() {
    HWND front = GetForegroundWindow();
    if (!front || front == GetShellWindow()) return false;
    wchar_t name[64] = {};
    GetClassNameW(front, name, 64);
    if (!wcscmp(name, L"Progman") || !wcscmp(name, L"WorkerW")) return false;
    RECT window;
    MONITORINFO monitor{sizeof monitor};
    if (!GetWindowRect(front, &window) || !GetMonitorInfoW(MonitorFromWindow(front, MONITOR_DEFAULTTONEAREST), &monitor))
        return false;
    return window.left <= monitor.rcMonitor.left && window.top <= monitor.rcMonitor.top &&
           window.right >= monitor.rcMonitor.right && window.bottom >= monitor.rcMonitor.bottom;
}
#else
bool fullscreenAppInFront() { return false; }
#endif

QRect virtualDesktop() {
    QRect all;
    for (auto* screen : QGuiApplication::screens()) all = all.united(screen->geometry());
    return all;
}

class Wallpaper final : public QOpenGLWindow {
public:
    explicit Wallpaper(QString data, QString snapshot = {}) : data_(std::move(data)), snapshot_(std::move(snapshot)) {}
    ~Wallpaper() override { makeCurrent(); scene_.reset(); doneCurrent(); }
    void setPaused(bool paused) {
        if (paused == paused_) return;
        paused_ = paused;
        if (!paused_) { clock_.restart(); update(); }
    }
    bool paused() const { return paused_; }
private:
    bool event(QEvent* event) override {
        switch (event->type()) {
        case QEvent::Close: qInfo("Window: close requested"); break;
        case QEvent::Hide: qInfo("Window: hidden"); break;
        case QEvent::Expose: if (!exposed_) qInfo("Window: first shown (%s)", isExposed() ? "visible" : "not visible"); exposed_ = true; break;
        default: break;
        }
        return QOpenGLWindow::event(event);
    }
    bool exposed_ = false;
    void initializeGL() override {
        try {
            const QDir data(data_);
            scene_ = std::make_unique<OriginalScene>(data.filePath("shaders"), data.filePath("reference"),
                data.filePath("cpu"), QString(), "clock", unsigned(QDate::currentDate().month()), hourNow());
            clock_.start();
        } catch (const std::exception& error) {
            qCritical("Could not start: %s", error.what());
            QMessageBox::critical(nullptr, "PS3 XMB", QString("The wallpaper could not start:\n%1").arg(error.what()));
            QApplication::exit(1);
        }
    }
    void paintGL() override {
        if (!scene_) return;
        try {
            if (!minutes_.isValid() || minutes_.elapsed() >= 60000) {   // follow the clock and month
                scene_->setTime(unsigned(QDate::currentDate().month()), hourNow());
                minutes_.start();
            }
            // The cursor pushes the flakes, as on the console's controller.
            const QPoint cursor = mapFromGlobal(QCursor::pos());
            if (QRect(QPoint(), size()).contains(cursor))
                scene_->setPointer(std::array<float,2>{2.0f * cursor.x() / width() - 1, 1 - 2.0f * cursor.y() / height()});
            else scene_->setPointer(std::nullopt);
            QElapsedTimer work;
            work.start();
            const auto size = this->size() * devicePixelRatio();
            scene_->advance(std::min(0.25, clock_.nsecsElapsed() / 1e9), size);
            clock_.restart();
            scene_->render(defaultFramebufferObject(), size);
            // Frame cost in the log every 600 frames.
            spent_ += work.nsecsElapsed();
            if (++counted_ == 600) {
                qInfo("%dx%d: %.2f ms per frame", size.width(), size.height(), spent_ / 600 / 1e6);
                spent_ = 0; counted_ = 0;
            }
            // Testing: save the 300th frame (about five seconds in) and quit.
            if (!snapshot_.isEmpty() && ++frames_ == 300) {
                grabFramebuffer().save(snapshot_);
                QTimer::singleShot(0, qApp, &QApplication::quit);
            }
        } catch (const std::exception& error) {
            scene_.reset();
            qCritical("Stopped: %s", error.what());
            QMessageBox::critical(nullptr, "PS3 XMB", QString("The wallpaper stopped:\n%1").arg(error.what()));
            QApplication::exit(1);
            return;
        }
        if (!paused_) update();
    }
    QString data_, snapshot_;
    qint64 spent_ = 0;
    unsigned counted_ = 0;
    unsigned frames_ = 0;
    std::unique_ptr<OriginalScene> scene_;
    QElapsedTimer clock_, minutes_;
    bool paused_ = false;
};

// First start: take the renderer's data from Sony's PS3 system software with the bundled
// importer (tools/import_data.py) and Python.
class ImportDialog final : public QDialog {
public:
    explicit ImportDialog(QString firmware) {
        setWindowTitle("PS3 XMB");
        auto* layout = new QVBoxLayout(this);
        text_ = new QLabel(firmware.isEmpty()
            ? "The wallpaper is drawn by the PS3's own renderer, so it needs a few files from Sony's free\n"
              "PS3 system software. Download it from Sony now (about 200 MB, deleted afterwards)?"
            : "Importing the renderer's data from " + firmware + "...");
        layout->addWidget(text_);
        progress_ = new QProgressBar;
        progress_->setRange(0, 100);
        progress_->hide();
        layout->addWidget(progress_);
        auto* buttons = new QHBoxLayout;
        download_ = new QPushButton("Download from Sony");
        auto* choose = new QPushButton("Use a PS3UPDAT.PUP...");
        cancel_ = new QPushButton("Cancel");
        buttons->addWidget(download_); buttons->addWidget(choose); buttons->addStretch(); buttons->addWidget(cancel_);
        layout->addLayout(buttons);
        connect(download_, &QPushButton::clicked, this, [this] { start(QString()); });
        connect(choose, &QPushButton::clicked, this, [this] {
            const auto file = QFileDialog::getOpenFileName(this, "PS3 system software", QString(), "PS3UPDAT.PUP (*.PUP *.pup)");
            if (!file.isEmpty()) start(file);
        });
        connect(cancel_, &QPushButton::clicked, this, [this] { process_.kill(); reject(); });
        connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { output(process_.readAllStandardOutput()); });
        connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            if (status == QProcess::NormalExit && code == 0 && imported()) { accept(); return; }
            text_->setText("The import failed:\n" + QString::fromUtf8(process_.readAllStandardError()).trimmed().right(600));
            download_->setEnabled(true);
        });
        if (!firmware.isEmpty()) QTimer::singleShot(0, this, [this, firmware] { start(firmware); });
    }
private:
    void start(const QString& firmware) {
        const QDir app(QCoreApplication::applicationDirPath());
#ifdef _WIN32
        const QString python = app.filePath("python/python.exe");
#else
        const QString python = "python3";
#endif
        QStringList arguments{app.filePath("tools/import_data.py")};
        if (!firmware.isEmpty()) arguments << firmware;
        arguments << "--output" << dataDirectory();
        download_->setEnabled(false);
        progress_->show();
        text_->setText("Starting...");
        process_.start(python, arguments);
    }
    void output(const QByteArray& bytes) {
        for (const auto& part : QString::fromUtf8(bytes).split(QRegularExpression("[\r\n]"), Qt::SkipEmptyParts)) {
            const auto percent = QRegularExpression("(\\d+)%").match(part);
            if (percent.hasMatch()) progress_->setValue(percent.captured(1).toInt());
            text_->setText(part.trimmed());
        }
    }
    QLabel* text_;
    QProgressBar* progress_;
    QPushButton *download_, *cancel_;
    QProcess process_;
};

QIcon trayIcon() {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor(20, 60, 140));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(2, 2, 60, 60, 12, 12);
    QPainterPath wave;
    wave.moveTo(6, 40);
    wave.cubicTo(22, 18, 40, 52, 58, 26);
    painter.setPen(QPen(QColor(230, 240, 255), 5, Qt::SolidLine, Qt::RoundCap));
    painter.drawPath(wave);
    return QIcon(pixmap);
}

void setStartWithWindows(bool enabled) {
#ifdef _WIN32
    QSettings run("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
    if (enabled) run.setValue("PS3 XMB", "\"" + QDir::toNativeSeparators(QCoreApplication::applicationFilePath()) + "\"");
    else run.remove("PS3 XMB");
#else
    (void)enabled;
#endif
}
bool startsWithWindows() {
#ifdef _WIN32
    return QSettings("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat).contains("PS3 XMB");
#else
    return false;
#endif
}

// Back to the normal wallpaper when the app ends (Explorer repaints its layer).
void restoreDesktop() {
#ifdef _WIN32
    wchar_t path[MAX_PATH] = {};
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0))
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, path, SPIF_SENDCHANGE);
#endif
}
}

int main(int argc, char** argv) {
    // Desktop pixels as they are: the window spans monitors of different scale factors.
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    QSurfaceFormat format;
    format.setVersion(4, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);
    qInstallMessageHandler(logMessage);
#ifdef _WIN32
    SetUnhandledExceptionFilter(logCrash);
#endif
    QApplication application(argc, argv);
    application.setApplicationName("PS3 XMB");
    qInfo("PS3 XMB starting (%s)", qPrintable(QSysInfo::prettyProductName()));
    application.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription("PS3 XMB wave background as the desktop wallpaper.");
    parser.addHelpOption();
    parser.addOptions({{"window", "Show in a normal window instead of behind the desktop icons"},
                       {"import", "Import from this PS3UPDAT.PUP instead of downloading", "file"},
                       {"snapshot", "Testing: save the 300th frame to this PNG file and quit", "file"}});
    parser.process(application);

    QLockFile lock(QDir(QDir::tempPath()).filePath("ps3-xmb.lock"));
    if (!lock.tryLock(100)) return 0;   // already running

    if (!imported() || parser.isSet("import")) {
        ImportDialog dialog(parser.value("import"));
        if (dialog.exec() != QDialog::Accepted) return 0;
        if (!startsWithWindows()) setStartWithWindows(true);   // default on, switch off in the tray
    }

    // OpenGL 4.3 first: without it the window would stay empty, so say what is missing.
    {
        QOffscreenSurface surface;
        surface.create();
        QOpenGLContext probe;
        const bool created = probe.create() && probe.makeCurrent(&surface);
        const auto version = created ? probe.format().version() : qMakePair(0, 0);
        if (created) {
            auto* gl = probe.functions();
            qInfo("OpenGL %s, %s, %s", reinterpret_cast<const char*>(gl->glGetString(GL_VERSION)),
                  reinterpret_cast<const char*>(gl->glGetString(GL_RENDERER)), reinterpret_cast<const char*>(gl->glGetString(GL_VENDOR)));
            probe.doneCurrent();
        }
        if (version < qMakePair(4, 3)) {
            const auto message = created
                ? QString("Your graphics driver offers OpenGL %1.%2; the wallpaper needs OpenGL 4.3.").arg(version.first).arg(version.second)
                : QString("The graphics driver gave no OpenGL at all; the wallpaper needs OpenGL 4.3.");
            const auto advice = "\n\nInstall or update the driver for your graphics card from AMD, NVIDIA or Intel "
                "(any GPU from about 2012 on has OpenGL 4.3). Virtual machines and Remote Desktop sessions usually don't provide it.";
            qCritical("%s", qPrintable(message));
            QMessageBox::critical(nullptr, "PS3 XMB", message + advice);
            return 1;
        }
    }

    Wallpaper wallpaper(dataDirectory(), parser.value("snapshot"));
    const QRect desktop = virtualDesktop();
#ifdef _WIN32
    const auto layer = parser.isSet("window") ? DesktopLayer{} : desktopLayer();
    if (layer.parent) {
        wallpaper.setFlags(Qt::FramelessWindowHint);
        wallpaper.setParent(QWindow::fromWinId(WId(layer.parent)));
        wallpaper.setGeometry(0, 0, desktop.width(), desktop.height());
        wallpaper.show();
        if (layer.iconView)
            SetWindowPos(HWND(wallpaper.winId()), layer.iconView, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        wchar_t parent[64] = {};
        GetClassNameW(GetParent(HWND(wallpaper.winId())), parent, 64);
        qInfo("Behind the desktop icons: parent %ls%s, %dx%d", parent, layer.iconView ? " (below the icon view)" : "",
              desktop.width(), desktop.height());
    } else
#endif
    {
        if (!parser.isSet("window")) qWarning("No desktop layer found: showing a normal window");
        wallpaper.setTitle("PS3 XMB");
        wallpaper.resize(parser.isSet("window") ? QSize(1280, 720) : desktop.size());
        wallpaper.show();
    }

    QSystemTrayIcon tray(trayIcon());
    QMenu menu;
    auto* pause = menu.addAction("Pause");
    pause->setCheckable(true);
    auto* autostart = menu.addAction("Start with Windows");
    autostart->setCheckable(true);
    autostart->setChecked(startsWithWindows());
    menu.addSeparator();
    auto* quit = menu.addAction("Quit");
    tray.setContextMenu(&menu);
    tray.setToolTip("PS3 XMB");
    tray.show();
    QObject::connect(autostart, &QAction::toggled, [](bool on) { setStartWithWindows(on); });
    QObject::connect(quit, &QAction::triggered, &application, &QApplication::quit);
    // Paused by hand, or while a fullscreen app (a game, a video) is in front.
    QTimer watch;
    const auto follow = [&] {
        const bool paused = pause->isChecked() || fullscreenAppInFront();
        if (paused != wallpaper.paused()) qInfo(paused ? "Paused" : "Running");
        wallpaper.setPaused(paused);
    };
    QObject::connect(&watch, &QTimer::timeout, follow);
    QObject::connect(pause, &QAction::toggled, follow);
    watch.start(1000);

    QObject::connect(&application, &QGuiApplication::lastWindowClosed, [] { qInfo("Last window closed"); });
    const int result = application.exec();
    qInfo("Event loop ended (%d)", result);
    wallpaper.destroy();
    restoreDesktop();
    return result;
}
