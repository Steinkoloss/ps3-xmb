import QtQuick
import org.kde.plasma.plasmoid
import PS3Xmb

WallpaperItem {
    id: root

    XmbBackground {
        anchors.fill: parent
        preset: root.configuration.Preset
        month: root.configuration.Month
        hour: root.configuration.Hour
        glints: root.configuration.Glints
    }
}
