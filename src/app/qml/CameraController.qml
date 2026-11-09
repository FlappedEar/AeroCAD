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

    /// The view is a parallel projection (same pose; the scale matches the pivot's depth).
    property bool orthographic: false

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
    /// Middle button double click (Fusion: zoom to fit).
    signal fitAllRequested()
    property bool contextMenuEnabled: true

    /// Point the current drag orbits around (scene space), when `orbitAroundPoint`.
    property vector3d orbitCenter
    property bool orbitAroundPoint: false

    // Zoom anchor cache: a wheel gesture keeps the point under a still pointer, so the
    // ray cast runs once per gesture instead of once per wheel event.
    property var _zoomAnchor: null
    property point _zoomAnchorAt
    property double _zoomAnchorTime: 0
    // The camera as the last zoomAt left it: any other change (Fit, a view restore, a
    // standard view) means the cached anchor is no longer under the pointer.
    property var _zoomAnchorCamera: null

    /// Forgets the cached zoom anchor (call when the projection changes).
    function invalidateZoomAnchor() {
        _zoomAnchor = null
    }

    function _cameraKey() {
        const p = pivot.position
        return [p.x, p.y, p.z, distance, pitch, yaw]
    }

    function _sameCamera(a, b) {
        if (!a || !b)
            return false
        for (let i = 0; i < a.length; ++i)
            if (Math.abs(a[i] - b[i]) > 1e-9 * Math.max(1, Math.abs(a[i])))
                return false
        return true
    }

    function updateTransform() {
        pivot.eulerRotation = Qt.vector3d(pitch, yaw, 0)
        camera.z = distance
    }

    /// World units per screen pixel at the pivot depth.
    function worldPerPixel() {
        return 2 * distance * Math.tan(camera.fieldOfView * Math.PI / 360) / Math.max(1, height)
    }

    /// World units per pixel of the pan being dragged (0 = none: use the pivot's depth).
    property real _panScale: 0

    /// Starts a pan that keeps the point under view position (x, y) under the pointer: the
    /// model there, else the point at the pivot's depth. With perspective a near point moves
    /// fewer world units per pixel than a far one, so the scale is taken at its depth.
    function beginPanAt(x, y) {
        _panScale = 0
        if (orthographic)
            return
        const p = pointUnder ? pointUnder(x, y, false) : null
        if (!p)
            return
        const depth = p.minus(camera.scenePosition).dotProduct(camera.forward)
        if (depth > 0)
            _panScale = 2 * depth * Math.tan(camera.fieldOfView * Math.PI / 360) / Math.max(1, height)
    }

    function endPan() {
        _panScale = 0
    }

    function pan(dx, dy) {
        _zoomAnchor = null
        const s = _panScale > 0 ? _panScale : worldPerPixel()
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
        // Free orbit: no stop at the top or the bottom, the view goes over the pole. Upside
        // down the world's up points down on screen, so turning about it goes the other way.
        yaw -= dx * s * (upright() ? 1 : -1)
        pitch = wrapAngle(pitch - dy * s)
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
        if (!_zoomAnchor || !_sameCamera(_zoomAnchorCamera, _cameraKey()) || Math.abs(x - _zoomAnchorAt.x) > 2 || Math.abs(y - _zoomAnchorAt.y) > 2
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
        _zoomAnchorCamera = _cameraKey()
    }

    /// Wheel angle (1/8 degree units, positive = away from the user) to a zoom factor.
    function wheelZoomFactor(angle) {
        return Math.pow(zoomStep, invertZoom ? angle : -angle)
    }

    /// A restored project camera supersedes a view animation started in the old context.
    function stopAnimation() {
        viewAnimation.stop()
    }

    /// Angle in (-180, 180].
    function wrapAngle(a) {
        let w = ((a + 180) % 360 + 360) % 360 - 180
        return w === -180 ? 180 : w
    }

    /// False while the view is upside down (orbited over the top or the bottom).
    function upright() {
        return Math.cos(pitch * Math.PI / 180) >= -1e-9
    }

    /// Turns the view by 90-degree steps as the ViewCube arrows do: dPitch < 0 looks more
    /// from above (over the top when already there), dYaw > 0 looks more from the right.
    function stepView(dPitch, dYaw) {
        viewAnimation.stop()
        _zoomAnchor = null
        pitch = wrapAngle(pitch)
        pitchAnimation.to = pitch + dPitch
        yawAnimation.to = yaw + dYaw * (upright() ? 1 : -1)
        viewAnimation.start()
    }

    /// Rotates to the given view along the shortest path.
    function animateTo(targetPitch, targetYaw) {
        viewAnimation.stop()
        pitch = wrapAngle(pitch)
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
            // From the press: the movement before the drag threshold counts too, so the
            // point grabbed stays under the pointer.
            last = active ? centroid.pressPosition : centroid.position
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
        onStarted: (x, y) => root.beginPanAt(x, y)
        onFinished: root.endPan()
        onMoved: (dx, dy) => root.pan(dx, dy)
    }

    DeltaDragHandler {
        acceptedButtons: Qt.MiddleButton
        acceptedModifiers: root.orbitModifiers
        onStarted: (x, y) => root.beginOrbitAt(x, y)
        onFinished: root.endOrbit()
        onMoved: (dx, dy) => root.orbit(dx, dy)
    }

    // Middle button double click fits everything, as in Fusion. A tap takes no grab, so the
    // middle drag handlers above still pan / orbit.
    TapHandler {
        acceptedButtons: Qt.MiddleButton
        gesturePolicy: TapHandler.DragThreshold
        grabPermissions: PointerHandler.ApprovesTakeOverByAnything
        onDoubleTapped: root.fitAllRequested()
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
        onStarted: (x, y) => root.beginPanAt(x, y)
        onFinished: root.endPan()
        onMoved: (dx, dy) => root.pan(dx, dy)
    }

    /// A mouse button is held over the view (orbit, pan, a drag or a click on its way).
    readonly property bool buttonHeld: anyButton.active
    PointHandler {
        id: anyButton
        acceptedButtons: Qt.AllButtons
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
        onStarted: (x, y) => {
            if (root.leftDragMode === "orbit")
                root.beginOrbitAt(x, y)
            else if (root.leftDragMode === "pan")
                root.beginPanAt(x, y)
        }
        onFinished: {
            root.endOrbit()
            root.endPan()
        }
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

    /// One wheel or scroll event. `pos` is the pointer in this item's coordinates.
    function handleWheel(event, pos) {
        // A device with smooth (pixel) scrolling: touchpad or Magic Mouse. Qt marks those
        // as TouchPad on macOS; a plain mouse wheel stays Mouse there too, even though
        // macOS also reports pixel deltas for it, so pixelDelta alone says nothing.
        const touchpad = !!event.device && event.device.type === PointerDevice.TouchPad
        // event.x / event.y are not in this item's coordinates in every Qt version;
        // the handler's point is.
        const x = pos.x, y = pos.y
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

    // Two WheelHandlers: an inactive WheelHandler only takes events along its orientation
    // (default vertical), so a gesture that starts sideways (touchpad pan, Shift + swipe to
    // orbit) would never arrive. The horizontal one takes only what the vertical one does
    // not get: purely sideways events while the vertical one is not in a gesture.
    WheelHandler {
        id: wheel
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => root.handleWheel(event, wheel.point.position)
    }

    WheelHandler {
        id: sidewaysWheel
        target: null
        orientation: Qt.Horizontal
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => {
            if (wheel.active || event.angleDelta.y !== 0 || event.pixelDelta.y !== 0)
                return
            root.handleWheel(event, sidewaysWheel.point.position)
        }
    }
}
