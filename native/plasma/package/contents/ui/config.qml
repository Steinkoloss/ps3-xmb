import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

Kirigami.FormLayout {
    id: root
    twinFormLayouts: parentLayout

    property string cfg_Preset
    property alias cfg_Month: month.value
    property alias cfg_Hour: hour.value
    property int cfg_FrameRate
    property alias cfg_Glints: glints.checked
    property alias formLayout: root

    QQC2.ComboBox {
        Kirigami.FormData.label: "Scene set:"
        readonly property var values: ["day", "night", "yoake", "higure"]
        model: ["Day", "Night", "Dawn (yoake)", "Dusk (higure)"]
        currentIndex: Math.max(0, values.indexOf(root.cfg_Preset))
        onActivated: index => root.cfg_Preset = values[index]
    }
    QQC2.SpinBox {
        id: month
        Kirigami.FormData.label: "Month colour:"
        from: 0; to: 12
        textFromValue: value => value === 0 ? "Follow calendar" : Qt.locale().standaloneMonthName(value - 1)
    }
    QQC2.ComboBox {
        Kirigami.FormData.label: "Frame rate:"
        model: ["60 fps", "30 fps (saves CPU)"]
        currentIndex: root.cfg_FrameRate === 30 ? 1 : 0
        onActivated: index => root.cfg_FrameRate = index === 1 ? 30 : 60
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
