// SPDX-License-Identifier: GPL-3.0-or-later
// Preferences (Fusion: profile menu > Preferences). Changes apply immediately and are
// stored with QSettings by the Preferences singleton.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Dialog {
    id: root

    title: qsTr("Preferences")
    modal: true
    standardButtons: Dialog.Close
    width: 520

    function indexOf(options, value) {
        for (let i = 0; i < options.length; ++i) {
            if (options[i].value === value)
                return i
        }
        return 0
    }

    GridLayout {
        anchors.fill: parent
        columns: 2
        columnSpacing: 12
        rowSpacing: 8

        Label { text: qsTr("General"); font.bold: true; Layout.columnSpan: 2 }

        Label { text: qsTr("Length unit") }
        ComboBox {
            id: unitBox
            Layout.fillWidth: true
            model: Preferences.lengthUnits
            textRole: "label"
            valueRole: "value"
            currentIndex: root.indexOf(Preferences.lengthUnits, Preferences.lengthUnit)
            onActivated: Preferences.lengthUnit = currentValue
        }
        Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.7
            text: qsTr("Changes how measured lengths are shown. Input fields, files and commands always use millimetres.")
        }

        Label { text: qsTr("Theme") }
        ComboBox {
            Layout.fillWidth: true
            model: Preferences.themes
            textRole: "label"
            valueRole: "value"
            currentIndex: root.indexOf(Preferences.themes, Preferences.theme)
            onActivated: Preferences.theme = currentValue
        }

        Label { text: qsTr("Navigation"); font.bold: true; Layout.columnSpan: 2; Layout.topMargin: 6 }

        Label { text: qsTr("Mouse") }
        ComboBox {
            Layout.fillWidth: true
            model: Preferences.mouseProfiles
            textRole: "label"
            valueRole: "value"
            currentIndex: root.indexOf(Preferences.mouseProfiles, Preferences.mouseProfile)
            onActivated: Preferences.mouseProfile = currentValue
        }

        Item { width: 1; height: 1 }
        CheckBox {
            text: qsTr("Reverse zoom direction of the wheel")
            checked: Preferences.invertZoom
            onToggled: Preferences.invertZoom = checked
        }

        Item { width: 1; height: 1 }
        CheckBox {
            text: qsTr("Reverse orbit direction")
            checked: Preferences.invertOrbit
            onToggled: Preferences.invertOrbit = checked
        }

        Item { width: 1; height: 1 }
        CheckBox {
            text: qsTr("Two-finger scroll on a touchpad pans (pinch zooms)")
            checked: Preferences.touchpadScrollPans
            onToggled: Preferences.touchpadScrollPans = checked
        }

        Label { text: qsTr("Project"); font.bold: true; Layout.columnSpan: 2; Layout.topMargin: 6 }

        Label { text: qsTr("Autosave") }
        RowLayout {
            CheckBox {
                id: autosaveOn
                text: qsTr("every")
                checked: Preferences.autosaveSeconds > 0
                onToggled: Preferences.autosaveSeconds = checked ? Math.max(1, minutes.value) * 60 : 0
            }
            SpinBox {
                id: minutes
                from: 1
                to: 60
                enabled: autosaveOn.checked
                value: Preferences.autosaveSeconds > 0 ? Math.max(1, Math.round(Preferences.autosaveSeconds / 60)) : 2
                onValueModified: Preferences.autosaveSeconds = value * 60
            }
            Label { text: qsTr("minutes") }
        }

        Label { text: qsTr("Export"); font.bold: true; Layout.columnSpan: 2; Layout.topMargin: 6 }

        Label { text: qsTr("Thinnest wall") }
        RowLayout {
            // Tenths of a millimetre; 0 turns the wall check off.
            SpinBox {
                id: minWall
                from: 0
                to: 100
                editable: true
                value: Math.round(Preferences.minWallThickness * 10)
                textFromValue: (v, locale) => v === 0 ? qsTr("off") : Number(v / 10).toLocaleString(locale, "f", 1)
                // Typed values: "off", or millimetres with a dot or a comma ("0.6", "1,2").
                validator: RegularExpressionValidator { regularExpression: new RegExp("^(" + qsTr("off") + "|\\d{0,2}([.,]\\d?)?)$") }
                valueFromText: (text, locale) => {
                    const t = text.trim()
                    const mm = Number(t.replace(",", "."))
                    return t === qsTr("off") || t === "" || isNaN(mm) ? 0 : Math.min(100, Math.round(mm * 10))
                }
                onValueModified: Preferences.minWallThickness = value / 10
            }
            Label { text: qsTr("mm, checked before export (3D printing)") }
        }

        Item { Layout.columnSpan: 2; Layout.preferredHeight: 4 }
        Button {
            Layout.columnSpan: 2
            text: qsTr("Restore defaults")
            onClicked: Preferences.resetToDefaults()
        }
    }
}
