import QtQuick
import org.kde.plasma.plasmoid
import PS3Xmb

WallpaperItem {
    id: root

    // Every screen shows the shared settings (XmbSettings). Plasma stores wallpaper settings per
    // screen, so a change applied in this screen's dialog is copied there for all screens; the
    // first screen to start seeds the shared settings from its own.
    property bool ready: false
    readonly property var configured: [root.configuration.Preset, root.configuration.Month, root.configuration.Hour,
                                       root.configuration.Glints, root.configuration.SceneAxis]
    function share() {
        XmbSettings.preset = root.configuration.Preset || "clock"
        XmbSettings.month = root.configuration.Month
        XmbSettings.hour = root.configuration.Hour
        XmbSettings.glints = root.configuration.Glints
        XmbSettings.sceneAxis = root.configuration.SceneAxis || "auto"
    }
    onConfiguredChanged: if (ready) share()
    Component.onCompleted: {
        if (!XmbSettings.stored) share()
        ready = true
    }

    XmbBackground {
        anchors.fill: parent
        preset: XmbSettings.preset
        month: XmbSettings.month
        hour: XmbSettings.hour
        glints: XmbSettings.glints
        sceneAxis: XmbSettings.sceneAxis
    }
}
