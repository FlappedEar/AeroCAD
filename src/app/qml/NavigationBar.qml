// SPDX-License-Identifier: GPL-3.0-or-later
// Navigation bar at the bottom of the canvas, as in Autodesk Fusion: orbit / pan / zoom tools
// that map the left mouse button, fit, home and the camera projection.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Pane {
    id: root

    /// "", "orbit", "pan" or "zoom" (see CameraController.leftDragMode).
    property string mode: ""
    property bool orthographic: false
    property bool gridVisible: true

    signal modeSelected(string mode)
    signal fitRequested()
    signal homeRequested()
    signal projectionRequested(bool orthographic)
    signal gridToggled(bool shown)

    padding: 3
    background: Rectangle {
        color: Theme.overlay
        border.color: Theme.border
        radius: Theme.radius
    }

    component NavButton: IconButton {
        iconSize: Theme.iconMedium
        implicitWidth: 32
        implicitHeight: 30
    }
    component NavSeparator: Rectangle {
        implicitWidth: 1
        implicitHeight: 18
        color: Theme.separator
        Layout.leftMargin: 3
        Layout.rightMargin: 3
    }

    RowLayout {
        spacing: 1
        NavButton {
            iconName: "orbit"
            tip: qsTr("Orbit: left drag orbits (Shift + middle drag always orbits)")
            checked: root.mode === "orbit"
            onClicked: root.modeSelected(root.mode === "orbit" ? "" : "orbit")
        }
        NavButton {
            iconName: "pan"
            tip: qsTr("Pan: left drag pans (middle drag always pans)")
            checked: root.mode === "pan"
            onClicked: root.modeSelected(root.mode === "pan" ? "" : "pan")
        }
        NavButton {
            iconName: "zoom"
            tip: qsTr("Zoom: left drag zooms (the wheel always zooms)")
            checked: root.mode === "zoom"
            onClicked: root.modeSelected(root.mode === "zoom" ? "" : "zoom")
        }
        NavSeparator {}
        NavButton {
            iconName: "fit"
            tip: qsTr("Fit all (F6)")
            onClicked: root.fitRequested()
        }
        NavButton {
            iconName: "home"
            tip: qsTr("Home view")
            onClicked: root.homeRequested()
        }
        NavSeparator {}
        NavButton {
            iconName: root.orthographic ? "ortho" : "perspective"
            tip: root.orthographic ? qsTr("Orthographic (click for perspective)") : qsTr("Perspective (click for orthographic)")
            onClicked: root.projectionRequested(!root.orthographic)
        }
        NavButton {
            iconName: "grid"
            tip: root.gridVisible ? qsTr("Hide grid") : qsTr("Show grid")
            checked: root.gridVisible
            onClicked: root.gridToggled(!root.gridVisible)
        }
    }
}
