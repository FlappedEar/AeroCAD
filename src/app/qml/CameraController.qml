// SPDX-License-Identifier: GPL-3.0-or-later
// CAD-style camera input: orbit around a pivot, pan in the view plane, zoom with the wheel.
import QtQuick
import QtQuick3D

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

    property real pitch: -30
    property real yaw: 45

    function updateTransform() {
        pivot.eulerRotation = Qt.vector3d(pitch, yaw, 0)
        camera.z = distance
    }

    function pan(dx, dy) {
        // World units per pixel at the pivot depth.
        const worldPerPixel = 2 * distance * Math.tan(camera.fieldOfView * Math.PI / 360) / Math.max(1, height)
        const right = pivot.right
        const up = pivot.up
        pivot.position = pivot.position.minus(right.times(dx * worldPerPixel)).plus(up.times(dy * worldPerPixel))
    }

    onPitchChanged: updateTransform()
    onYawChanged: updateTransform()
    onDistanceChanged: updateTransform()
    Component.onCompleted: updateTransform()

    DragHandler {
        id: orbitHandler
        target: null
        acceptedButtons: Qt.LeftButton
        acceptedModifiers: Qt.NoModifier
        property point last
        onActiveChanged: last = centroid.position
        onCentroidChanged: {
            if (!active)
                return
            const p = centroid.position
            root.yaw -= (p.x - last.x) * root.orbitSpeed
            root.pitch = Math.max(-90, Math.min(90, root.pitch - (p.y - last.y) * root.orbitSpeed))
            last = p
        }
    }

    DragHandler {
        id: panHandler
        target: null
        acceptedButtons: Qt.MiddleButton | Qt.RightButton
        property point last
        onActiveChanged: last = centroid.position
        onCentroidChanged: {
            if (!active)
                return
            const p = centroid.position
            root.pan(p.x - last.x, p.y - last.y)
            last = p
        }
    }

    DragHandler {
        id: shiftPanHandler
        target: null
        acceptedButtons: Qt.LeftButton
        acceptedModifiers: Qt.ShiftModifier
        property point last
        onActiveChanged: last = centroid.position
        onCentroidChanged: {
            if (!active)
                return
            const p = centroid.position
            root.pan(p.x - last.x, p.y - last.y)
            last = p
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
            const factor = Math.pow(root.zoomStep, -event.angleDelta.y)
            root.distance = Math.max(root.minDistance, Math.min(root.maxDistance, root.distance * factor))
        }
    }
}
