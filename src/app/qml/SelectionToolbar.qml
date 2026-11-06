// SPDX-License-Identifier: GPL-3.0-or-later
// Region selection tools over the 3D view: tool, combine mode and tool options.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: root

    /// The Viewport whose tool properties this toolbar edits.
    required property Item viewport

    padding: 6
    opacity: 0.95

    background: Rectangle {
        color: root.palette.window
        radius: 6
        // Keeps clicks on the toolbar background from reaching the 3D view.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
        }
    }

    ColumnLayout {
        spacing: 4

        RowLayout {
            spacing: 2
            ButtonGroup { id: toolGroup }
            Repeater {
                model: [
                    { tool: "navigate", text: qsTr("Navigate"), tip: qsTr("Orbit, pan and pick parts (Esc)") },
                    { tool: "region", text: qsTr("Region"), tip: qsTr("Click: select a smooth region up to sharp edges (1)") },
                    { tool: "lasso", text: qsTr("Lasso"), tip: qsTr("Drag: draw around the area to select (2)") },
                    { tool: "brush", text: qsTr("Brush"), tip: qsTr("Drag: paint the selection, [ and ] change the size (3)") }
                ]
                delegate: ToolButton {
                    required property var modelData
                    text: modelData.text
                    checkable: true
                    checked: root.viewport.tool === modelData.tool
                    ButtonGroup.group: toolGroup
                    onClicked: root.viewport.tool = modelData.tool
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: modelData.tip
                }
            }
        }

        RowLayout {
            visible: root.viewport.tool !== "navigate"
            spacing: 2
            ButtonGroup { id: modeGroup }
            Repeater {
                model: [
                    { mode: "replace", text: qsTr("New") },
                    { mode: "add", text: qsTr("Add") },
                    { mode: "subtract", text: qsTr("Remove") }
                ]
                delegate: ToolButton {
                    required property var modelData
                    text: modelData.text
                    checkable: true
                    checked: root.viewport.selectMode === modelData.mode
                    ButtonGroup.group: modeGroup
                    onClicked: root.viewport.selectMode = modelData.mode
                }
            }
        }

        RowLayout {
            visible: root.viewport.tool === "region"
            Label { text: qsTr("Max bend") }
            SpinBox {
                from: 1
                to: 90
                value: root.viewport.regionAngle
                editable: true
                textFromValue: (value, locale) => value + "°"
                valueFromText: (text, locale) => parseInt(text)
                onValueModified: root.viewport.regionAngle = value
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Largest angle between neighbouring triangles inside the region")
            }
        }

        RowLayout {
            visible: root.viewport.tool === "brush"
            Label { text: qsTr("Radius") }
            TextField {
                Layout.preferredWidth: 70
                text: root.viewport.brushRadius > 0 ? String(root.viewport.brushRadius) : ""
                placeholderText: qsTr("auto")
                validator: DoubleValidator { bottom: 0.01; decimals: 3; notation: DoubleValidator.StandardNotation }
                onEditingFinished: {
                    const v = Number.fromLocaleString(Qt.locale(), text)
                    if (v > 0)
                        root.viewport.brushRadius = v
                }
            }
            Label { text: qsTr("mm") }
            CheckBox {
                text: qsTr("Connected only")
                checked: root.viewport.brushConnected
                onToggled: root.viewport.brushConnected = checked
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Paint only the surface under the brush, not through thin walls")
            }
        }

        RowLayout {
            visible: root.viewport.tool === "lasso"
            CheckBox {
                text: qsTr("Select through")
                checked: root.viewport.lassoThrough
                onToggled: root.viewport.lassoThrough = checked
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Also select hidden triangles behind the visible surface")
            }
        }

        Label {
            visible: root.viewport.tool !== "navigate"
            text: qsTr("Shift + middle drag: orbit · middle drag: pan")
            opacity: 0.6
            font.pixelSize: 11
        }
    }
}
