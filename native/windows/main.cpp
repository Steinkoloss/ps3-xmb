// SPDX-License-Identifier: GPL-2.0-or-later
// PS3 XMB wallpaper as a standalone app: one OpenGL window spanning every monitor, placed
// behind the desktop icons on Windows (the WorkerW layer that Explorer paints the wallpaper
// in), with a tray icon. On first start it imports the renderer's data from Sony's PS3 system
// software (downloaded from Sony unless a PS3UPDAT.PUP is given) with the bundled Python.
//   ps3-xmb [--window] [--import PS3UPDAT.PUP] [--snapshot frame.png]
#include "OriginalScene.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QCursor>
#include <QDate>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLockFile>
#include <QMenu>
#include <QMessageBox>
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
#include <QSystemTrayIcon>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>
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

#ifdef _WIN32
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
    void initializeGL() override {
        try {
            const QDir data(data_);
            scene_ = std::make_unique<OriginalScene>(data.filePath("shaders"), data.filePath("reference"),
                data.filePath("cpu"), QString(), "clock", unsigned(QDate::currentDate().month()), hourNow());
            clock_.start();
        } catch (const std::exception& error) {
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
            const auto size = this->size() * devicePixelRatio();
            scene_->advance(std::min(0.25, clock_.nsecsElapsed() / 1e9), size);
            clock_.restart();
            scene_->render(defaultFramebufferObject(), size);
            // Testing: save the 300th frame (about five seconds in) and quit.
            if (!snapshot_.isEmpty() && ++frames_ == 300) {
                grabFramebuffer().save(snapshot_);
                QTimer::singleShot(0, qApp, &QApplication::quit);
            }
        } catch (const std::exception& error) {
            scene_.reset();
            QMessageBox::critical(nullptr, "PS3 XMB", QString("The wallpaper stopped:\n%1").arg(error.what()));
            QApplication::exit(1);
            return;
        }
        if (!paused_) update();
    }
    QString data_, snapshot_;
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
    QApplication application(argc, argv);
    application.setApplicationName("PS3 XMB");
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
    } else
#endif
    {
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
    QObject::connect(&watch, &QTimer::timeout, [&] { wallpaper.setPaused(pause->isChecked() || fullscreenAppInFront()); });
    QObject::connect(pause, &QAction::toggled, [&] { wallpaper.setPaused(pause->isChecked() || fullscreenAppInFront()); });
    watch.start(1000);

    const int result = application.exec();
    wallpaper.destroy();
    restoreDesktop();
    return result;
}
