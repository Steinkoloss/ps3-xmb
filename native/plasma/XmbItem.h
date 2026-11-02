#pragma once
#include <QQuickFramebufferObject>
#include <QtQml/qqmlregistration.h>
#include <memory>
#include <mutex>
#include <vector>

class XmbSimulator;
struct XmbChannel;

// Live original background for Qt Quick. The CPU/SPU simulation runs on its own
// thread and context; the original pass graph renders on the scene-graph thread.
class XmbItem : public QQuickFramebufferObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(XmbBackground)
    // Directory with shaders/, reference/, cpu/ and spline/original-ls.bin.
    Q_PROPERTY(QString dataDirectory READ dataDirectory WRITE setDataDirectory NOTIFY sourceChanged)
    // Original override set: "", "day", "night", "yoake" or "higure".
    Q_PROPERTY(QString preset READ preset WRITE setPreset NOTIFY sourceChanged)
    // 1-12, or 0 to follow the system calendar.
    Q_PROPERTY(int month READ month WRITE setMonth NOTIFY timeChanged)
    // 0-24, or negative to follow the system clock.
    Q_PROPERTY(double hour READ hour WRITE setHour NOTIFY timeChanged)
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)
    // Simulation updates per second (1-60); the simulation itself stays real-time.
    Q_PROPERTY(int frameRate READ frameRate WRITE setFrameRate NOTIFY frameRateChanged)
    // Original glint pass (particles_second).
    Q_PROPERTY(bool glints READ glints WRITE setGlints NOTIFY glintsChanged)
    // Multi-screen scene axis: "auto" (snap to level/upright within 20 degrees), "level" or "exact".
    // One simulation serves every screen, so the last screen to set it decides.
    Q_PROPERTY(QString sceneAxis READ sceneAxis WRITE setSceneAxis NOTIFY sceneAxisChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
public:
    explicit XmbItem(QQuickItem* parent = nullptr);
    ~XmbItem() override;
    Renderer* createRenderer() const override;

    QString dataDirectory() const { return dataDirectory_; }
    void setDataDirectory(const QString& directory);
    QString preset() const { return preset_; }
    void setPreset(const QString& preset);
    int month() const { return month_; }
    void setMonth(int month);
    double hour() const { return hour_; }
    void setHour(double hour);
    bool running() const { return running_; }
    void setRunning(bool running);
    int frameRate() const { return frameRate_; }
    bool glints() const { return glints_; }
    void setGlints(bool enabled);
    QString sceneAxis() const { return sceneAxis_; }
    void setSceneAxis(const QString& mode);
    void setFrameRate(int rate);
    QString error() const { return error_; }

    std::shared_ptr<XmbChannel> channel() const;
    QRectF desktopArea() const;   // physical (mm); read by the scene-graph renderer
    Q_INVOKABLE void fail(const QString& message);
signals:
    void sourceChanged();
    void timeChanged();
    void runningChanged();
    void frameRateChanged();
    void glintsChanged();
    void sceneAxisChanged();
    void errorChanged();
protected:
    void componentComplete() override;
    void geometryChange(const QRectF& next, const QRectF& previous) override;
    void itemChange(ItemChange change, const ItemChangeData& data) override;
private:
    void restart();
    void areaChanged();
    mutable std::mutex areaMutex_;
    QRectF area_;
    std::vector<QMetaObject::Connection> layoutConnections_, screenConnections_;
    QString dataDirectory_, preset_, error_;
    int month_ = 0;
    double hour_ = -1;
    bool running_ = true;
    int frameRate_ = 60;
    bool glints_ = true;
    QString sceneAxis_ = QStringLiteral("auto");
    int retryDelay_ = 2;
    std::shared_ptr<XmbSimulator> simulator_;
};
