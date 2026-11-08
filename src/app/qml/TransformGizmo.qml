// SPDX-License-Identifier: GPL-3.0-or-later
// Move / Rotate / Scale manipulator drawn over the 3D view (Fusion: Move/Copy). Arrows move
// along the X, Y, Z axes, rings turn about them, the square in the middle scales evenly.
// Everything previews live; Viewport.finishTransform() applies it as one undo step.
// Shift while dragging snaps: 15 degrees for rings, whole millimetres for arrows.
import QtQuick
import AeroCAD

Item {
    id: root

    /// The Viewport (modelToScreen, worldPerPixelAt, transform state).
    required property var viewport
    readonly property bool active: !!viewport.transformTarget

    /// Arrow length on screen, pixels.
    property real arrowLength: 110
    readonly property var axes: [Qt.vector3d(1, 0, 0), Qt.vector3d(0, 1, 0), Qt.vector3d(0, 0, 1)]
    readonly property var axisColors: ["#e5484d", "#30a46c", "#3e63dd"]

    /// "" or "move0".."move2", "rotate0".."rotate2", "scale".
    property string hovered: ""
    property string dragging: ""
    /// Shown next to the pointer while dragging ("12.5 mm", "30°", "120 %").
    property string feedback: ""
    property point feedbackAt

    // Drag state.
    property point _last
    property real _accum: 0 // mm or degrees since the drag started, before snapping
    property real _applied: 0 // what has been applied of _accum (snapped)
    // Ring drags: screen direction (unit) and pixels per radian of the ring where it was
    // grabbed; pointer motion along it turns the ring, so the grabbed point follows.
    property point _tangent
    property real _pxPerRadian: 0

    visible: active
    opacity: viewport.transformPickMode !== "" ? 0.3 : 1

    function center() {
        const t = viewport.transformTarget
        return t ? t.center.plus(viewport.transformMove) : Qt.vector3d(0, 0, 0)
    }

    /// Screen geometry of the handles: {c, ends: [3 points], rings: [3 polylines], len (mm)}.
    function layout() {
        const c3 = center()
        const len = arrowLength * viewport.worldPerPixelAt(c3)
        const c = viewport.modelToScreen(c3)
        const ends = axes.map(a => viewport.modelToScreen(c3.plus(a.times(len))))
        const rings = []
        const r = len * 0.72
        for (let i = 0; i < 3; ++i) {
            // Two unit vectors perpendicular to axis i.
            const u = axes[(i + 1) % 3], v = axes[(i + 2) % 3]
            const pts = []
            for (let k = 0; k <= 64; ++k) {
                const t = k / 64 * 2 * Math.PI
                pts.push(viewport.modelToScreen(c3.plus(u.times(Math.cos(t) * r)).plus(v.times(Math.sin(t) * r))))
            }
            rings.push(pts)
        }
        return { c: c, ends: ends, rings: rings, len: len }
    }

    function distToSegment(p, a, b) {
        const dx = b.x - a.x, dy = b.y - a.y
        const l2 = dx * dx + dy * dy
        const t = l2 > 0 ? Math.max(0, Math.min(1, ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2)) : 0
        return Math.hypot(p.x - a.x - t * dx, p.y - a.y - t * dy)
    }

    /// Handle under view position p, or "".
    function handleAt(p) {
        // While Lay on Face / Face Front waits for a face, clicks go through to the part.
        if (!active || viewport.transformPickMode !== "")
            return ""
        const g = layout()
        if (viewport.transformTarget.canScale && Math.abs(p.x - g.c.x) <= 9 && Math.abs(p.y - g.c.y) <= 9)
            return "scale"
        let best = "", bestD = 9
        for (let i = 0; i < 3; ++i) {
            const d = distToSegment(p, g.c, g.ends[i])
            if (d < bestD && Math.hypot(g.ends[i].x - g.c.x, g.ends[i].y - g.c.y) > 12) {
                best = "move" + i
                bestD = d
            }
        }
        if (best)
            return best
        bestD = 7
        for (let i = 0; i < 3; ++i) {
            const pts = g.rings[i]
            for (let k = 1; k < pts.length; ++k) {
                const d = distToSegment(p, pts[k - 1], pts[k])
                if (d < bestD) {
                    best = "rotate" + i
                    bestD = d
                }
            }
        }
        return best
    }

    function fmt(v, decimals) {
        return Number(v).toLocaleString(Qt.locale(), "f", decimals)
    }

    /// Where a ring was grabbed: the ring point under p nearest to the viewer (the near
    /// side, where a ring seen at a slant overlaps itself), and the ring's screen tangent
    /// there. Sets _tangent / _pxPerRadian; 0 when the ring is seen edge-on there.
    function grabRing(i, p) {
        const c3 = center()
        const g = layout()
        const r = g.len * 0.72
        const u = axes[(i + 1) % 3], v = axes[(i + 2) % 3]
        const at = t => c3.plus(u.times(Math.cos(t) * r)).plus(v.times(Math.sin(t) * r))
        let best = -1, bestDepth = Infinity, bestD = Infinity
        const n = 256
        for (let k = 0; k < n; ++k) {
            const t = k / n * 2 * Math.PI
            const q = viewport.modelToScreen(at(t))
            const d = Math.hypot(q.x - p.x, q.y - p.y)
            if (d > 9)
                continue
            // Towards the viewer = the ring point's direction from the centre faces the eye.
            const depth = viewport.axisTowardsViewer(at(t).minus(c3), c3) ? 0 : 1
            if (depth < bestDepth || (depth === bestDepth && d < bestD)) {
                best = t
                bestDepth = depth
                bestD = d
            }
        }
        if (best < 0)
            best = Math.atan2(p.y - g.c.y, p.x - g.c.x) // not on the ring: any tangent will do
        // d(screen)/d(angle), right-hand about axis i (u x v = axis i).
        const e = 1e-3
        const q0 = viewport.modelToScreen(at(best - e)), q1 = viewport.modelToScreen(at(best + e))
        const tx = (q1.x - q0.x) / (2 * e), ty = (q1.y - q0.y) / (2 * e)
        const len = Math.hypot(tx, ty)
        const screenR = Math.max(1, arrowLength * 0.72)
        if (len < 1e-6) {
            _pxPerRadian = 0
            return
        }
        _tangent = Qt.point(tx / len, ty / len)
        // Seen nearly edge-on the tangent shrinks: keep a sensible speed (at least a third
        // of the ring's radius per radian).
        _pxPerRadian = Math.max(len, screenR / 3)
    }

    function beginDrag(handle, p) {
        if (handle.startsWith("rotate"))
            grabRing(Number(handle.charAt(6)), p)
        dragging = handle
        _last = p
        _accum = 0
        _applied = 0
        feedbackAt = p
        feedback = ""
    }

    function dragTo(p, snap) {
        const g = layout()
        const h = dragging
        if (h.startsWith("move")) {
            const i = Number(h.charAt(4))
            const e = g.ends[i]
            const sx = e.x - g.c.x, sy = e.y - g.c.y
            const pxLen = Math.hypot(sx, sy)
            if (pxLen < 1e-6)
                return
            // Pointer motion along the arrow on screen, in millimetres.
            _accum += ((p.x - _last.x) * sx + (p.y - _last.y) * sy) / pxLen / (pxLen / g.len)
            const target = snap ? Math.round(_accum) : _accum
            viewport.transformMove = viewport.transformMove.plus(axes[i].times(target - _applied))
            _applied = target
            feedback = (target >= 0 ? "+" : "") + fmt(target, snap ? 0 : 2) + " mm"
        } else if (h.startsWith("rotate")) {
            const i = Number(h.charAt(6))
            if (_pxPerRadian > 0) {
                // Pointer motion along the ring where it was grabbed (Fusion: the grabbed
                // point follows the pointer, whatever the angle the ring is seen at).
                const along = (p.x - _last.x) * _tangent.x + (p.y - _last.y) * _tangent.y
                _accum += along / _pxPerRadian * 180 / Math.PI
            } else {
                // Ring seen exactly edge-on where grabbed: turn with the angle about the centre.
                const a0 = Math.atan2(_last.y - g.c.y, _last.x - g.c.x)
                const a1 = Math.atan2(p.y - g.c.y, p.x - g.c.x)
                let d = a1 - a0
                if (d > Math.PI) d -= 2 * Math.PI
                if (d < -Math.PI) d += 2 * Math.PI
                // Screen y points down: a positive screen angle is clockwise; seen from the
                // axis tip, a right-hand turn is anticlockwise.
                const towardsViewer = viewport.axisTowardsViewer(axes[i], center())
                _accum += -d * 180 / Math.PI * (towardsViewer ? 1 : -1)
            }
            const target = snap ? Math.round(_accum / 15) * 15 : _accum
            viewport.rotateTransform(axes[i], target - _applied)
            _applied = target
            feedback = (target >= 0 ? "+" : "") + fmt(target, snap ? 0 : 1) + "°"
        } else if (h === "scale") {
            _accum += (p.x - _last.x) - (p.y - _last.y)
            const factor = Math.exp(_accum * 0.005)
            const target = snap ? Math.max(0.05, Math.round(factor * 20) / 20) : factor
            viewport.transformScale = viewport.transformScale * target / (_applied || 1)
            _applied = target
            feedback = fmt(viewport.transformScale * 100, 0) + " %"
        }
        _last = p
        feedbackAt = p
    }

    function endDrag() {
        dragging = ""
        feedback = ""
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            if (!root.active)
                return
            const g = root.layout()
            const lit = h => root.dragging === h || (!root.dragging && root.hovered === h)
            // Rings first, arrows on top.
            for (let i = 0; i < 3; ++i) {
                const h = "rotate" + i
                ctx.strokeStyle = root.axisColors[i]
                ctx.globalAlpha = lit(h) ? 1 : 0.7
                ctx.lineWidth = lit(h) ? 4 : 2
                ctx.beginPath()
                const pts = g.rings[i]
                ctx.moveTo(pts[0].x, pts[0].y)
                for (let k = 1; k < pts.length; ++k)
                    ctx.lineTo(pts[k].x, pts[k].y)
                ctx.stroke()
            }
            for (let i = 0; i < 3; ++i) {
                const h = "move" + i
                const e = g.ends[i]
                const dx = e.x - g.c.x, dy = e.y - g.c.y
                const l = Math.hypot(dx, dy)
                if (l < 12)
                    continue // pointing at the viewer: no usable arrow
                const ux = dx / l, uy = dy / l
                ctx.globalAlpha = 1
                ctx.strokeStyle = root.axisColors[i]
                ctx.fillStyle = root.axisColors[i]
                ctx.lineWidth = lit(h) ? 5 : 3
                ctx.beginPath()
                ctx.moveTo(g.c.x, g.c.y)
                ctx.lineTo(e.x - ux * 12, e.y - uy * 12)
                ctx.stroke()
                const head = lit(h) ? 9 : 7
                ctx.beginPath()
                ctx.moveTo(e.x, e.y)
                ctx.lineTo(e.x - ux * 16 - uy * head, e.y - uy * 16 + ux * head)
                ctx.lineTo(e.x - ux * 16 + uy * head, e.y - uy * 16 - ux * head)
                ctx.closePath()
                ctx.fill()
                ctx.font = "bold 12px sans-serif"
                ctx.fillText(["X", "Y", "Z"][i], e.x + ux * 8 - 4, e.y + uy * 8 + 4)
            }
            if (root.viewport.transformTarget.canScale) {
                ctx.globalAlpha = 1
                ctx.fillStyle = lit("scale") ? Theme.accent : "#ffffff"
                ctx.strokeStyle = "#3a414b"
                ctx.lineWidth = 1.5
                ctx.fillRect(g.c.x - 7, g.c.y - 7, 14, 14)
                ctx.strokeRect(g.c.x - 7, g.c.y - 7, 14, 14)
            }
        }
    }

    // Repaint when the view or the transform changes.
    Connections {
        target: root.viewport
        function onViewKeyChanged() { canvas.requestPaint() }
        function onTransformTargetChanged() { canvas.requestPaint() }
        function onTransformMoveChanged() { canvas.requestPaint() }
        function onTransformRotationChanged() { canvas.requestPaint() }
        function onTransformScaleChanged() { canvas.requestPaint() }
    }
    onHoveredChanged: canvas.requestPaint()
    onDraggingChanged: canvas.requestPaint()

    // Pointer handlers, enabled only over a handle: anywhere else presses go straight to
    // selection and navigation underneath.
    readonly property bool overHandle: active && (hovered !== "" || dragging !== "")

    HoverHandler {
        enabled: root.active
        cursorShape: root.dragging ? Qt.ClosedHandCursor : root.hovered ? Qt.OpenHandCursor : Qt.ArrowCursor
        onPointChanged: if (!root.dragging) root.hovered = root.handleAt(point.position)
        onHoveredChanged: if (!hovered && !root.dragging) root.hovered = ""
    }

    DragHandler {
        id: handleDrag
        target: null
        enabled: root.overHandle
        acceptedButtons: Qt.LeftButton
        dragThreshold: 1
        onActiveChanged: {
            if (active) {
                const h = root.handleAt(centroid.pressPosition)
                if (h) {
                    root.beginDrag(h, centroid.pressPosition)
                    root.dragTo(centroid.position, (centroid.modifiers & Qt.ShiftModifier) !== 0)
                }
            } else {
                root.endDrag()
                root.hovered = root.handleAt(centroid.position)
            }
        }
        onCentroidChanged: {
            if (active && root.dragging)
                root.dragTo(centroid.position, (centroid.modifiers & Qt.ShiftModifier) !== 0)
        }
    }

    // A click on a handle without dragging does nothing (it is not a click on the view).
    TapHandler {
        enabled: root.overHandle
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.ReleaseWithinBounds
    }

    // Live value next to the pointer while dragging.
    Rectangle {
        visible: root.feedback !== ""
        x: root.feedbackAt.x + 16
        y: root.feedbackAt.y + 12
        width: feedbackText.implicitWidth + 12
        height: feedbackText.implicitHeight + 6
        radius: 4
        color: Theme.surfaceRaised
        border.color: Theme.border
        Text {
            id: feedbackText
            anchors.centerIn: parent
            text: root.feedback
            color: Theme.text
            font.pixelSize: Theme.fontSmall
        }
    }
}
