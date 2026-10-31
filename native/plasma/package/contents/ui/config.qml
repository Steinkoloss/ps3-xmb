import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

Kirigami.FormLayout {
    id: root
    twinFormLayouts: parentLayout

    property string cfg_Preset
    property alias cfg_Month: month.value
    property alias cfg_Hour: hour.value
    property alias cfg_Glints: glints.checked
    property alias formLayout: root

    QQC2.ComboBox {
        Kirigami.FormData.label: "Scene set:"
        readonly property var values: ["clock", "day", "night", "yoake", "higure"]
        model: ["Follow the clock (console)", "Day", "Night", "Dawn (yoake)", "Dusk (higure)"]
        currentIndex: Math.max(0, values.indexOf(root.cfg_Preset))
        onActivated: index => root.cfg_Preset = values[index]
    }
    QQC2.SpinBox {
        id: month
        Kirigami.FormData.label: "Month colour:"
        from: 0; to: 12
        textFromValue: value => value === 0 ? "Follow calendar" : Qt.locale().standaloneMonthName(value - 1)
    }
    QQC2.CheckBox {
        id: glints
        Kirigami.FormData.label: "Glints:"
        text: "Draw the glint pass"
    }
    QQC2.SpinBox {
        id: hour
        Kirigami.FormData.label: "Time of day:"
        from: -1; to: 23
        textFromValue: value => value < 0 ? "Follow clock" : value + ":00"
    }
}
