// SPDX-License-Identifier: GPL-3.0-or-later
// Navigation bar at the bottom of the canvas, as in Autodesk Fusion: orbit / pan / zoom tools
// that map the left mouse button, fit, home and the camera projection.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: root

    /// "", "orbit", "pan" or "zoom" (see CameraController.leftDragMode).
    property string mode: ""
    property bool orthographic: false

    signal modeSelected(string mode)
    signal fitRequested()
    signal homeRequested()
    signal projectionRequested(bool orthographic)

    padding: 2
    background: Rectangle {
        color: palette.window
        border.color: palette.mid
        radius: 4
        opacity: 0.95
    }

    component NavButton: ToolButton {
        property string tip
        font.pixelSize: 12
        ToolTip.visible: hovered && tip.length > 0
        ToolTip.delay: 500
        ToolTip.text: tip
    }

    RowLayout {
        spacing: 0
        NavButton {
            text: qsTr("Orbit")
            tip: qsTr("Left drag orbits (Shift + middle drag always orbits)")
            checked: root.mode === "orbit"
            onClicked: root.modeSelected(root.mode === "orbit" ? "" : "orbit")
        }
        NavButton {
            text: qsTr("Pan")
            tip: qsTr("Left drag pans (middle drag always pans)")
            checked: root.mode === "pan"
            onClicked: root.modeSelected(root.mode === "pan" ? "" : "pan")
        }
        NavButton {
            text: qsTr("Zoom")
            tip: qsTr("Left drag zooms (the wheel always zooms)")
            checked: root.mode === "zoom"
            onClicked: root.modeSelected(root.mode === "zoom" ? "" : "zoom")
        }
        ToolSeparator {}
        NavButton {
            text: qsTr("Fit")
            tip: qsTr("Fit all (F6)")
            onClicked: root.fitRequested()
        }
        NavButton {
            text: qsTr("Home")
            tip: qsTr("Home view")
            onClicked: root.homeRequested()
        }
        ToolSeparator {}
        NavButton {
            text: root.orthographic ? qsTr("Orthographic") : qsTr("Perspective")
            tip: qsTr("Switch camera projection")
            onClicked: root.projectionRequested(!root.orthographic)
        }
    }
}
