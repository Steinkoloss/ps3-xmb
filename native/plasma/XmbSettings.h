#pragma once
#include <QObject>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

// Wallpaper settings shared by every screen (~/.config/ps3-xmb.conf). Plasma keeps wallpaper
// configuration per screen; the screens show one scene, so they read these instead, and a
// change made in any screen's settings is written here and reaches all of them.
class XmbSettings : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QString preset READ preset WRITE setPreset NOTIFY changed)
    Q_PROPERTY(int month READ month WRITE setMonth NOTIFY changed)
    Q_PROPERTY(int hour READ hour WRITE setHour NOTIFY changed)
    Q_PROPERTY(bool glints READ glints WRITE setGlints NOTIFY changed)
    Q_PROPERTY(QString sceneAxis READ sceneAxis WRITE setSceneAxis NOTIFY changed)
    // False until something has been stored; the first screen then seeds it from its own settings.
    Q_PROPERTY(bool stored READ stored NOTIFY changed)
public:
    explicit XmbSettings(QObject* parent = nullptr);
    QString preset() const { return value("Preset", QStringLiteral("clock")).toString(); }
    int month() const { return value("Month", 0).toInt(); }
    int hour() const { return value("Hour", -1).toInt(); }
    bool glints() const { return value("Glints", true).toBool(); }
    QString sceneAxis() const { return value("SceneAxis", QStringLiteral("auto")).toString(); }
    bool stored() const;
    void setPreset(const QString& v) { set("Preset", v); }
    void setMonth(int v) { set("Month", v); }
    void setHour(int v) { set("Hour", v); }
    void setGlints(bool v) { set("Glints", v); }
    void setSceneAxis(const QString& v) { set("SceneAxis", v); }
signals:
    void changed();
private:
    QVariant value(const QString& key, const QVariant& fallback) const;
    void set(const QString& key, const QVariant& value);
};
