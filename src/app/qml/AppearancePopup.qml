// SPDX-License-Identifier: GPL-3.0-or-later
// Appearance (Fusion's A key): the colour a part is shown in. A row of swatches; a click sets
// the colour (one undo step, saved with the project), "Default" goes back to the colour of
// the part's kind. Esc or a click outside closes it.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Popup {
    id: root

    required property AppController controller
    property var partId: 0
    property string current: ""

    /// Swatches: distinct in light and dark themes and from the selection blue.
    readonly property var palette: ["#c94f45", "#e8853d", "#e9c046", "#5aa65a", "#2ea89a", "#3f7fd9",
                                    "#7e5fcf", "#d7609f", "#8d6e5a", "#4a5560", "#eceff1"]

    padding: Theme.space2
    // Takes the keyboard on open, so Esc closes it right away (CloseOnEscape needs focus).
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    /// Opens the swatches for part `id` at x, y (parent coordinates), kept inside the parent.
    function openFor(id, x, y) {
        const info = controller.partInfo(id)
        if (!info.id || info.kind === "sketch" || info.kind === "plane")
            return false
        partId = id
        current = info.color ?? ""
        root.x = Math.max(0, Math.min(x, parent.width - implicitWidth))
        root.y = Math.max(0, Math.min(y, parent.height - implicitHeight))
        open()
        return true
    }

    function apply(color) {
        const id = partId
        close()
        if (color !== current)
            controller.execute("part.set_color", { id: id, color: color })
    }

    background: Rectangle {
        color: Theme.surfaceRaised
        border.color: Theme.border
        radius: 6
    }

    contentItem: ColumnLayout {
        spacing: Theme.space1
        Label {
            text: qsTr("Appearance")
            font.pixelSize: Theme.fontSmall
            color: Theme.textMuted
        }
        GridLayout {
            columns: 6
            columnSpacing: 4
            rowSpacing: 4
            Repeater {
                model: root.palette
                delegate: AbstractButton {
                    id: swatch
                    required property string modelData
                    objectName: "swatch" + modelData
                    implicitWidth: 26
                    implicitHeight: 26
                    hoverEnabled: true
                    onClicked: root.apply(modelData)
                    ToolTip.visible: hovered
                    ToolTip.text: modelData
                    background: Rectangle {
                        radius: 4
                        color: swatch.modelData
                        border.width: root.current === swatch.modelData ? 2 : 1
                        border.color: root.current === swatch.modelData ? Theme.accent
                                      : swatch.hovered ? Theme.borderStrong : Theme.border
                    }
                }
            }
            AbstractButton {
                id: defaultSwatch
                objectName: "swatchDefault"
                implicitWidth: 26
                implicitHeight: 26
                hoverEnabled: true
                onClicked: root.apply("")
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Default colour")
                background: Rectangle {
                    radius: 4
                    clip: true
                    color: Theme.bodyColor
                    border.width: root.current === "" ? 2 : 1
                    border.color: root.current === "" ? Theme.accent : defaultSwatch.hovered ? Theme.borderStrong : Theme.border
                    Rectangle { // diagonal stroke: "no colour of its own"
                        anchors.centerIn: parent
                        width: parent.width * 1.2
                        height: 1.5
                        rotation: -45
                        color: Theme.textMuted
                    }
                }
            }
        }
    }
}
