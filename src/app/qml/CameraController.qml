// SPDX-License-Identifier: GPL-3.0-or-later
// CAD camera input with the Autodesk Fusion default mouse scheme
// (https://www.autodesk.com/shortcuts/fusion-360, "Fusion 360 Camera Controls" in Autodesk help):
//   middle drag = pan, Shift + middle drag = orbit, wheel = zoom;
//   touchpad / Magic Mouse: two-finger drag = pan, Shift + two-finger drag = orbit, pinch = zoom.
// The left button is left free for selection; the navigation bar can temporarily map it to
// orbit, pan or zoom (`leftDragMode`).
// Preferences > Mouse switches the middle button to other common schemes:
//   SolidWorks: middle drag = orbit, Ctrl + middle = pan, Shift + middle = zoom;
//   Blender:    middle drag = orbit, Shift + middle = pan, Ctrl + middle = zoom.
import QtQuick
import QtQuick3D
import AeroCAD

Item {
    id: root

    /// Node the camera orbits around (position = pivot, eulerRotation = view direction).
    required property Node pivot
    /// Camera placed on the pivot's local +Z axis at `distance`.
    required property PerspectiveCamera camera
    property real distance: 300
    property real minDistance: 0.01
    property real maxDistance: 1e7
    property real orbitSpeed: 0.3 // degrees per pixel
    property real zoomStep: 1.0015 // per wheel angle unit (1/8 degree)
    property real dragZoomStep: 1.005 // per pixel of vertical drag

    property real pitch: -30
    property real yaw: 45

    /// What a plain left-button drag does: "" (nothing, left is for selection), "orbit", "pan", "zoom".
    property string leftDragMode: ""

    /// "fusion", "solidworks" or "blender" (see Preferences).
    property string mouseProfile: Preferences.mouseProfile
    property bool invertZoom: Preferences.invertZoom
    readonly property int panModifiers: mouseProfile === "solidworks" ? Qt.ControlModifier
                                        : mouseProfile === "blender" ? Qt.ShiftModifier : Qt.NoModifier
    readonly property int orbitModifiers: mouseProfile === "fusion" ? Qt.ShiftModifier : Qt.NoModifier
    readonly property int zoomModifiers: mouseProfile === "solidworks" ? Qt.ShiftModifier : Qt.ControlModifier

    function updateTransform() {
        pivot.eulerRotation = Qt.vector3d(pitch, yaw, 0)
        camera.z = distance
    }

    /// World units per screen pixel at the pivot depth.
    function worldPerPixel() {
        return 2 * distance * Math.tan(camera.fieldOfView * Math.PI / 360) / Math.max(1, height)
    }

    function pan(dx, dy) {
        const s = worldPerPixel()
        pivot.position = pivot.position.minus(pivot.right.times(dx * s)).plus(pivot.up.times(dy * s))
    }

    function orbit(dx, dy) {
        viewAnimation.stop()
        yaw -= dx * orbitSpeed
        pitch = Math.max(-90, Math.min(90, pitch - dy * orbitSpeed))
    }

    function zoomBy(factor) {
        distance = Math.max(minDistance, Math.min(maxDistance, distance * factor))
    }

    /// Rotates to the given view along the shortest path.
    function animateTo(targetPitch, targetYaw) {
        viewAnimation.stop()
        let delta = (targetYaw - yaw) % 360
        if (delta > 180)
            delta -= 360
        else if (delta < -180)
            delta += 360
        pitchAnimation.to = targetPitch
        yawAnimation.to = yaw + delta
        viewAnimation.start()
    }

    onPitchChanged: updateTransform()
    onYawChanged: updateTransform()
    onDistanceChanged: updateTransform()
    Component.onCompleted: updateTransform()

    ParallelAnimation {
        id: viewAnimation
        NumberAnimation { id: pitchAnimation; target: root; property: "pitch"; duration: 250; easing.type: Easing.InOutQuad }
        NumberAnimation { id: yawAnimation; target: root; property: "yaw"; duration: 250; easing.type: Easing.InOutQuad }
    }

    component DeltaDragHandler: DragHandler {
        target: null
        signal moved(real dx, real dy)
        property point last
        onActiveChanged: last = centroid.position
        onCentroidChanged: {
            if (!active)
                return
            const p = centroid.position
            moved(p.x - last.x, p.y - last.y)
            last = p
        }
    }

    DeltaDragHandler {
        acceptedButtons: Qt.MiddleButton
        acceptedModifiers: root.panModifiers
        onMoved: (dx, dy) => root.pan(dx, dy)
    }

    DeltaDragHandler {
        acceptedButtons: Qt.MiddleButton
        acceptedModifiers: root.orbitModifiers
        onMoved: (dx, dy) => root.orbit(dx, dy)
    }

    // Zoom by dragging (not in the Fusion scheme, which zooms with the wheel only).
    DeltaDragHandler {
        enabled: root.mouseProfile !== "fusion"
        acceptedButtons: Qt.MiddleButton
        acceptedModifiers: root.zoomModifiers
        onMoved: (dx, dy) => root.zoomBy(Math.pow(root.dragZoomStep, root.invertZoom ? -dy : dy))
    }

    // Navigation bar tools (Orbit / Pan / Zoom) map the left button temporarily.
    DeltaDragHandler {
        enabled: root.leftDragMode !== ""
        acceptedButtons: Qt.LeftButton
        onMoved: (dx, dy) => {
            if (root.leftDragMode === "orbit")
                root.orbit(dx, dy)
            else if (root.leftDragMode === "pan")
                root.pan(dx, dy)
            else if (root.leftDragMode === "zoom")
                root.zoomBy(Math.pow(root.dragZoomStep, dy))
        }
    }

    PinchHandler {
        target: null
        property real startDistance
        onActiveChanged: startDistance = root.distance
        onActiveScaleChanged: root.distance = Math.max(root.minDistance, Math.min(root.maxDistance, startDistance / activeScale))
    }

    WheelHandler {
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => {
            // Trackpads (and the Magic Mouse) deliver pixel deltas; classic wheels only angle deltas.
            const touchpad = (event.device && event.device.type === PointerDevice.TouchPad)
                || event.pixelDelta.x !== 0 || event.pixelDelta.y !== 0
            if (touchpad) {
                const d = event.pixelDelta.x !== 0 || event.pixelDelta.y !== 0
                    ? event.pixelDelta : Qt.point(event.angleDelta.x / 8, event.angleDelta.y / 8)
                if (event.modifiers & Qt.ShiftModifier)
                    root.orbit(-d.x, -d.y)
                else
                    root.pan(d.x, d.y)
                return
            }
            if (event.modifiers & Qt.ShiftModifier)
                return
            root.zoomBy(Math.pow(root.zoomStep, root.invertZoom ? event.angleDelta.y : -event.angleDelta.y))
        }
    }
}
