// SPDX-License-Identifier: GPL-3.0-or-later
// Colour scale of the active heat map (deviation or thickness), shown in the viewport.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Pane {
    id: root

    required property AppController controller
    readonly property var heat: controller.inspectRevision >= 0 ? controller.inspectInfo().heatMap : ({ enabled: false })
    readonly property bool deviation: heat.kind === "deviation"

    visible: heat.enabled
    padding: 8
    opacity: 0.94

    function f(v) {
        return (v > 0 && deviation ? "+" : "") + Number(v).toFixed(3)
    }

    ColumnLayout {
        spacing: 4
        Label {
            text: root.deviation ? qsTr("Deviation (mm)") : qsTr("Thickness (mm)")
            font.bold: true
        }
        RowLayout {
            spacing: 6
            Rectangle {
                width: 16
                height: 160
                border.color: "#80000000"
                // Top = high values. Deviation: red (outside) at the top; thickness: blue (thick).
                gradient: Gradient {
                    GradientStop { position: 0.0; color: root.controller.heatColor(root.deviation ? 1.0 : 0.0) }
                    GradientStop { position: 0.25; color: root.controller.heatColor(root.deviation ? 0.75 : 0.25) }
                    GradientStop { position: 0.5; color: root.controller.heatColor(0.5) }
                    GradientStop { position: 0.75; color: root.controller.heatColor(root.deviation ? 0.25 : 0.75) }
                    GradientStop { position: 1.0; color: root.controller.heatColor(root.deviation ? 0.0 : 1.0) }
                }
            }
            Item {
                width: 64
                height: 160
                Label { anchors.top: parent.top; text: root.heat.enabled ? root.f(root.heat.rangeMax) : "" }
                Label { anchors.verticalCenter: parent.verticalCenter; text: root.heat.enabled ? root.f((root.heat.rangeMin + root.heat.rangeMax) / 2) : "" }
                Label { anchors.bottom: parent.bottom; text: root.heat.enabled ? root.f(root.heat.rangeMin) : "" }
            }
        }
        Label {
            visible: root.heat.enabled
            opacity: 0.75
            text: root.heat.enabled ? qsTr("min %1 · max %2").arg(root.f(root.heat.min)).arg(root.f(root.heat.max)) : ""
        }
    }
}
