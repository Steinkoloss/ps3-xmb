#include "XmbSettings.h"
#include <QSettings>
#include <QStandardPaths>

namespace {
// One store per process, whatever number of QML engines create the singleton.
struct Store : QObject {
    Q_OBJECT
public:
    QSettings file{QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/ps3-xmb.conf", QSettings::IniFormat};
signals:
    void changed();
};
Store& store() { static Store instance; return instance; }
}

XmbSettings::XmbSettings(QObject* parent) : QObject(parent) {
    connect(&store(), &Store::changed, this, &XmbSettings::changed);
}

bool XmbSettings::stored() const { return !store().file.allKeys().isEmpty(); }

QVariant XmbSettings::value(const QString& key, const QVariant& fallback) const {
    return store().file.value(key, fallback);
}

void XmbSettings::set(const QString& key, const QVariant& value) {
    auto& s = store();
    if (s.file.contains(key) && s.file.value(key) == value) return;
    s.file.setValue(key, value);
    s.file.sync();
    emit s.changed();
}

#include "XmbSettings.moc"
