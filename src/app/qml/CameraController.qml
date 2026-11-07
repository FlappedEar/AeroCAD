// SPDX-License-Identifier: GPL-3.0-or-later
// CAD camera input with the Autodesk Fusion default mouse scheme
// (https://www.autodesk.com/shortcuts/fusion-360, "Fusion 360 Camera Controls" in Autodesk help):
//   middle drag = pan, Shift + middle drag = orbit, wheel = zoom towards the pointer.
// Added for mice without a middle button (Magic Mouse, most Mac mice) and touchpads:
//   right drag = orbit, Shift + right drag = pan (a right click without dragging still
//   opens the context menu).
// Touchpad / Magic Mouse: two-finger scroll = zoom towards the pointer (Preferences can
//   make it pan), Shift + scroll = orbit, Ctrl/Cmd + scroll = the other of zoom and pan,
//   pinch = zoom.
// Orbiting by dragging turns the view around the point of the model under the pointer,
// zooming keeps the point under the pointer where it is.
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
    property bool invertOrbit: Preferences.invertOrbit
    readonly property int panModifiers: mouseProfile === "solidworks" ? Qt.ControlModifier
                                        : mouseProfile === "blender" ? Qt.ShiftModifier : Qt.NoModifier
    readonly property int orbitModifiers: mouseProfile === "fusion" ? Qt.ShiftModifier : Qt.NoModifier
    property bool touchpadScrollPans: Preferences.touchpadScrollPans
    readonly property int zoomModifiers: mouseProfile === "solidworks" ? Qt.ShiftModifier : Qt.ControlModifier

    /// function(x, y, onModelOnly) -> scene position of what is under view position (x, y):
    /// the model surface, else (unless onModelOnly) the point at the pivot's depth; or null.
    /// Set by the viewport, which owns picking.
    property var pointUnder: null

    /// A right click that did not move (a right drag orbits): open the context menu.
    signal contextClicked(real x, real y)
    property bool contextMenuEnabled: true

    /// Point the current drag orbits around (scene space), when `orbitAroundPoint`.
    property vector3d orbitCenter
    property bool orbitAroundPoint: false

    // Zoom anchor cache: a wheel gesture keeps the point under a still pointer, so the
    // ray cast runs once per gesture instead of once per wheel event.
    property var _zoomAnchor: null
    property point _zoomAnchorAt
    property double _zoomAnchorTime: 0

    function updateTransform() {
        pivot.eulerRotation = Qt.vector3d(pitch, yaw, 0)
        camera.z = distance
    }

    /// World units per screen pixel at the pivot depth.
    function worldPerPixel() {
        return 2 * distance * Math.tan(camera.fieldOfView * Math.PI / 360) / Math.max(1, height)
    }

    function pan(dx, dy) {
        _zoomAnchor = null
        const s = worldPerPixel()
        pivot.position = pivot.position.minus(pivot.right.times(dx * s)).plus(pivot.up.times(dy * s))
    }

    function orbit(dx, dy) {
        viewAnimation.stop()
        _zoomAnchor = null
        // A copy: reading a value-type property gives a reference that follows the property.
        const r = pivot.rotation
        const before = Qt.quaternion(r.scalar, r.x, r.y, r.z)
        // The model follows the pointer; the preference turns that around.
        const s = invertOrbit ? -orbitSpeed : orbitSpeed
        yaw -= dx * s
        pitch = Math.max(-90, Math.min(90, pitch - dy * s))
        if (orbitAroundPoint) {
            // Turn the whole camera rig (pivot + camera) by the same rotation around the
            // centre, so the point under the pointer stays put.
            const rel = before.inverted().times(pivot.position.minus(orbitCenter))
            pivot.position = orbitCenter.plus(pivot.rotation.times(rel))
        }
    }

    /// Starts orbiting around the model point under view position (x, y), or around the
    /// pivot when nothing is there.
    function beginOrbitAt(x, y) {
        const p = pointUnder ? pointUnder(x, y, true) : null
        orbitAroundPoint = !!p
        if (p)
            orbitCenter = p
    }

    function endOrbit() {
        orbitAroundPoint = false
    }

    function zoomBy(factor) {
        distance = Math.max(minDistance, Math.min(maxDistance, distance * factor))
    }

    /// Zooms by `factor` (< 1 = closer) keeping the point under view position (x, y) under it.
    function zoomAt(factor, x, y) {
        const oldDistance = distance
        const newDistance = Math.max(minDistance, Math.min(maxDistance, distance * factor))
        if (!(newDistance > 0) || newDistance === oldDistance)
            return
        const now = Date.now()
        if (!_zoomAnchor || Math.abs(x - _zoomAnchorAt.x) > 2 || Math.abs(y - _zoomAnchorAt.y) > 2
                || now - _zoomAnchorTime > 600) {
            _zoomAnchor = pointUnder ? pointUnder(x, y, false) : null
            _zoomAnchorAt = Qt.point(x, y)
        }
        _zoomAnchorTime = now
        const f = newDistance / oldDistance
        // Scaling the camera rig about the anchor keeps the anchor on the same view ray.
        if (_zoomAnchor)
            pivot.position = _zoomAnchor.plus(pivot.position.minus(_zoomAnchor).times(f))
        distance = newDistance
    }

    /// Wheel angle (1/8 degree units, positive = away from the user) to a zoom factor.
    function wheelZoomFactor(angle) {
        return Math.pow(zoomStep, invertZoom ? angle : -angle)
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
        onStarted: root._zoomAnchor = null
    }

    component DeltaDragHandler: DragHandler {
        target: null
        signal moved(real dx, real dy)
        signal started(real x, real y)
        signal finished()
        property point last
        onActiveChanged: {
            last = centroid.position
            if (active)
                started(centroid.pressPosition.x, centroid.pressPosition.y)
            else
                finished()
        }
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
        onStarted: (x, y) => root.beginOrbitAt(x, y)
        onFinished: root.endOrbit()
        onMoved: (dx, dy) => root.orbit(dx, dy)
    }

    // Right button: drag = orbit (Shift: pan), click = context menu. PointHandlers only
    // watch (passive grab), so one handler sees press, moves and release and can tell a
    // click from a drag; a DragHandler here took the press away from any click handler.
    component RightDragWatcher: PointHandler {
        acceptedButtons: Qt.RightButton
        signal started(real x, real y)
        signal moved(real dx, real dy)
        signal clicked(real x, real y)
        signal finished()
        property point pressedAt
        property point last
        property bool dragging: false
        onActiveChanged: {
            if (active) {
                pressedAt = point.position
                last = pressedAt
                dragging = false
            } else if (dragging) {
                dragging = false
                finished()
            } else {
                clicked(pressedAt.x, pressedAt.y)
            }
        }
        onPointChanged: {
            if (!active)
                return
            const p = point.position
            if (!dragging) {
                if (Math.hypot(p.x - pressedAt.x, p.y - pressedAt.y) <= Application.styleHints.startDragDistance)
                    return
                dragging = true
                started(pressedAt.x, pressedAt.y)
            }
            moved(p.x - last.x, p.y - last.y)
            last = p
        }
    }

    RightDragWatcher {
        acceptedModifiers: Qt.NoModifier
        onStarted: (x, y) => root.beginOrbitAt(x, y)
        onFinished: root.endOrbit()
        onMoved: (dx, dy) => root.orbit(dx, dy)
        onClicked: (x, y) => { if (root.contextMenuEnabled) root.contextClicked(x, y) }
    }

    RightDragWatcher {
        acceptedModifiers: Qt.ShiftModifier
        onMoved: (dx, dy) => root.pan(dx, dy)
    }

    // Navigation bar tools (Orbit / Pan / Zoom) map the left button temporarily.
    DeltaDragHandler {
        enabled: root.leftDragMode !== ""
        acceptedButtons: Qt.LeftButton
        onStarted: (x, y) => { if (root.leftDragMode === "orbit") root.beginOrbitAt(x, y) }
        onFinished: root.endOrbit()
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
        property real lastScale: 1
        onActiveChanged: lastScale = 1
        onActiveScaleChanged: {
            if (!(activeScale > 0))
                return
            root.zoomAt(lastScale / activeScale, centroid.position.x, centroid.position.y)
            lastScale = activeScale
        }
    }

    WheelHandler {
        id: wheel
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => {
            // A device with smooth (pixel) scrolling: touchpad or Magic Mouse. Qt marks those
            // as TouchPad on macOS; a plain mouse wheel stays Mouse there too, even though
            // macOS also reports pixel deltas for it, so pixelDelta alone says nothing.
            const touchpad = !!event.device && event.device.type === PointerDevice.TouchPad
            // event.x / event.y are not in this item's coordinates in every Qt version;
            // the handler's point is.
            const x = wheel.point.position.x, y = wheel.point.position.y
            if (!touchpad) {
                // Shift + wheel is horizontal scrolling on most systems: ignore it.
                if (event.modifiers & Qt.ShiftModifier)
                    return
                const a = event.angleDelta.y !== 0 ? event.angleDelta.y : event.angleDelta.x
                // `inverted`: macOS "natural scrolling" turned the sign around; zoom follows
                // the physical wheel direction.
                root.zoomAt(root.wheelZoomFactor(event.inverted ? -a : a), x, y)
                return
            }
            const d = event.pixelDelta.x !== 0 || event.pixelDelta.y !== 0
                ? event.pixelDelta : Qt.point(event.angleDelta.x / 8, event.angleDelta.y / 8)
            const momentum = event.phase === Qt.ScrollMomentum
            if (event.modifiers & Qt.ShiftModifier) {
                if (!momentum)
                    root.orbit(d.x, d.y) // as with the mouse and two-finger pan: the model follows
                return
            }
            const pans = root.touchpadScrollPans !== !!(event.modifiers & Qt.ControlModifier)
            if (pans) {
                root.pan(d.x, d.y)
                return
            }
            if (momentum)
                return // a flick would keep zooming after the fingers lift
            const a = event.angleDelta.y !== 0 ? event.angleDelta.y : event.pixelDelta.y * 2
            root.zoomAt(root.wheelZoomFactor(event.inverted ? -a : a), x, y)
        }
    }
}
