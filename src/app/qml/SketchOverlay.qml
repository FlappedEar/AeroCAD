// SPDX-License-Identifier: GPL-3.0-or-later
// Drawing tools of the sketch environment (Fusion: Line L, 2-point Rectangle R, Center
// Diameter Circle C, 2-Point and 3-Point Circle, 3-point Arc, Polygon, Slot, Fit Point
// Spline, Center Rectangle, 3-Point Rectangle, Center Point Arc). Clicks are projected onto the sketch
// plane and snapped to existing points or a grid; a finished curve is reported with
// curveDrawn() in sketch coordinates (mm). Typing a value + Enter sets the dimension of
// the curve being drawn (length, diameter, "width, height"); expressions with user
// parameters are kept as expressions, so the sketch stays parametric.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Item {
    id: root

    required property Item viewport
    property var sketch: null
    property string tool: ""
    /// Existing points to snap to, [[u, v], ...].
    property var snapPoints: []
    /// Solved curves, constraints and degrees of freedom of the sketch (sketch.curves).
    property var sketchCurves: []
    property var sketchConstraints: []
    property int degreesOfFreedom: -1
    signal curveDrawn(var curve)
    signal constraintAdded(var constraint)
    signal constraintEdited(int index, var value)
    signal constraintRemoved(int index)
    /// Delete was asked for the selected curves (indices of sketch.curves).
    signal curvesDeleted(var indices)
    /// Construction (X) was asked for the selected curves: `on` true turns them into
    /// construction geometry, false back to normal.
    signal constructionToggled(var indices, bool on)
    /// Trim tool (T): the piece of curve `curve` under `at` (sketch coordinates) is to go.
    signal trimRequested(int curve, var at)
    /// Extend tool: the end of line or arc `curve` nearer `at` (sketch coordinates) is to
    /// reach the next curve.
    signal extendRequested(int curve, var at)
    /// Break tool: curve `curve` is to be split where other curves cross it, around `at`.
    signal breakRequested(int curve, var at)
    /// Mirror tool: `curves` are to be mirrored about line `line`.
    signal mirrorRequested(var curves, int line)
    /// Fillet tool: the corner of lines `curves` (two indices) is to be rounded.
    signal filletRequested(var curves, real radius)
    /// Radius the next sketch fillet starts with (the last one used); its dimension can be
    /// edited afterwards like any other.
    property real filletRadius: 5
    /// Project tool: the edge of body `body` (feature id) nearest `point` (model mm) is to
    /// be projected onto the sketch.
    signal projectRequested(int body, var point)
    /// Offset tool: `curves` (one chain) are to be offset by `distance`.
    signal offsetRequested(var curves, real distance)
    /// Curves the Offset tool offsets (the selection when it started).
    property var offsetCurves: []
    /// Curves the Mirror tool copies (the selection when it started).
    property var mirrorCurves: []
    /// Move/Copy tool (Fusion: Sketch > Move/Copy, M): `curves` are to be moved by dx, dy
    /// and turned by `angle` (degrees) about their middle; with `copy`, copies are added.
    signal moveRequested(var curves, real dx, real dy, real angle, bool copy)
    /// Curves the Move/Copy tool moves (the selection when it started), the turn it adds
    /// and whether it copies.
    property var moveCurves: []
    property real moveAngle: 0
    property bool moveCopy: false
    /// A point is being dragged in select mode (CAD-123): `point` is a constraint point
    /// reference, `to` where it is now (sketch coordinates); `done` on release.
    signal pointDragged(var point, var to, bool done)
    /// The point drag was cancelled (Esc): its live preview is to be undone.
    signal pointDragCancelled()
    /// A point is being dragged in select mode.
    readonly property bool pointDragging: input.pointDragging
    /// A palette button asks for another sketch tool.
    signal toolRequested(string tool)

    readonly property bool constraintTool: constraintLayer.toolNames.indexOf(tool) >= 0
    /// The constraint glyph and pick layer (for tests and tools that place constraints).
    readonly property alias constraintGlyphs: constraintLayer

    readonly property bool active: sketch !== null && tool !== ""
    /// With no tool (Fusion's select mode): curves picked with the mouse, indices of
    /// sketch.curves, and the curve under the cursor (-1 none).
    property var selectedCurves: []
    property int hoverCurve: -1
    /// Extend / Break: what the click would make under the cursor ({kind, points, center}) or null.
    property var piecePreview: null
    readonly property bool selecting: sketch !== null && tool === ""
    /// Clicked points of the curve being drawn, [[u, v], ...].
    property var points: []
    /// Center Point Arc: the radius typed after the centre (a number or an expression with
    /// user parameters), kept for the finished arc; null when the radius is drawn.
    property var typedRadius: null
    /// 3-Point Rectangle: the height typed after the first side (number or expression,
    /// signed to the side of the cursor), kept for the finished rectangle.
    property var typedHeight: null
    onPointsChanged: if (points.length === 0) { typedRadius = null; typedHeight = null; tangentFrom = null }
    /// Tangent Arc: the curve end it starts from, { ref: {curve, point}, pos, dir }.
    property var tangentFrom: null
    property var hover: null
    property bool hoverSnapped: false
    readonly property real snapPixels: 10
    property int polygonSides: 6

    enabled: sketch !== null
    visible: sketch !== null

    onToolChanged: {
        reset()
        clearSelection()
        if (tool !== "mirror")
            mirrorCurves = []
        if (tool !== "offset")
            offsetCurves = []
        if (tool !== "move") {
            moveCurves = []
            moveAngle = 0
        }
        piecePreview = null
    }
    onSketchChanged: { reset(); clearSelection() }
    // The curves changed (a delete, an undo): drop picks that no longer exist.
    onSketchCurvesChanged: {
        const kept = selectedCurves.filter(i => i < sketchCurves.length)
        if (kept.length !== selectedCurves.length)
            selectedCurves = kept
        hoverCurve = -1
        piecePreview = null
        canvas.requestPaint()
    }
    // Previews and the surface line are projected points: redraw them when the view moves.
    Connections {
        target: root.viewport
        function onViewKeyChanged() { canvas.requestPaint() }
    }

    /// A curve or constraint pick is under way (the sketch menu offers to end it).
    readonly property bool drawing: points.length > 0 || constraintLayer.picks.length > 0 || constraintLayer.pending !== null

    /// Cancels a point drag (Esc): nothing is kept, and the release that follows does
    /// nothing either.
    function cancelPointDrag() {
        if (!input.pointDragging && input.dragPoint === null)
            return false
        const was = input.pointDragging
        input.pointDragging = false
        input.dragPoint = null
        if (was)
            pointDragCancelled()
        return true
    }

    /// Ends the curve being drawn (a line chain) or the constraint pick, as Esc does. A
    /// spline is kept as an open spline through the points clicked so far.
    function endCurrent() {
        if (tool === "spline")
            finishSpline(false)
        reset()
        constraintLayer.reset()
    }

    function clearSelection() {
        if (selectedCurves.length === 0 && hoverCurve < 0)
            return
        selectedCurves = []
        hoverCurve = -1
        canvas.requestPaint()
    }

    /// Deletes the selected curves (Delete key, sketch menu). False when none is selected.
    function deleteSelection() {
        if (selectedCurves.length === 0)
            return false
        const list = selectedCurves.slice().sort((a, b) => a - b)
        selectedCurves = []
        hoverCurve = -1
        curvesDeleted(list)
        canvas.requestPaint()
        return true
    }

    /// Construction (Fusion: X): the selected curves become construction geometry, or
    /// normal again when all of them already are. False when none is selected.
    function toggleConstruction() {
        if (selectedCurves.length === 0)
            return false
        const list = selectedCurves.slice().sort((a, b) => a - b)
        const all = list.every(i => { const c = sketchCurves.find(x => x.index === i); return c && c.construction })
        constructionToggled(list, !all)
        return true
    }

    /// Mirror (Fusion: Sketch > Mirror): the selected curves are kept for the tool, which
    /// then waits for the mirror line. False when nothing is selected.
    function startMirror() {
        if (selectedCurves.length === 0)
            return false
        const list = selectedCurves.slice()
        toolRequested("mirror")
        mirrorCurves = list
        canvas.requestPaint()
        return true
    }

    /// Fillet tool: the two lines meeting at the corner nearest (x, y) are rounded, with
    /// the last radius used (smaller when the lines are short).
    function filletAt(x, y) {
        const p = pointAt(x, y)
        if (p === null)
            return false
        const lines = []
        for (const c of sketchCurves) {
            if (c.kind !== "line")
                continue
            if (dist(c.points[0], p.pos) < 1e-6 || dist(c.points[1], p.pos) < 1e-6)
                lines.push(c)
        }
        if (lines.length !== 2)
            return false
        const shortest = Math.min(dist(lines[0].points[0], lines[0].points[1]), dist(lines[1].points[0], lines[1].points[1]))
        let r = filletRadius
        if (r > 0.4 * shortest)
            r = Number((0.25 * shortest).toPrecision(2))
        filletRequested([lines[0].index, lines[1].index], r)
        return true
    }

    /// Offset (Fusion: Sketch > Offset): the selected chain is kept for the tool, which
    /// then asks for the distance. False when nothing is selected.
    function startOffset() {
        if (selectedCurves.length === 0)
            return false
        const list = selectedCurves.slice()
        toolRequested("offset")
        offsetCurves = list
        dimension.text = ""
        dimension.forceActiveFocus()
        canvas.requestPaint()
        return true
    }

    /// Move/Copy (Fusion: Sketch > Move/Copy): the selection is kept for the tool; two
    /// clicks (from, to) or typed "dx, dy" move it, with the turn and Copy from the panel.
    function startMove() {
        if (selectedCurves.length === 0)
            return false
        const list = selectedCurves.slice()
        toolRequested("move")
        moveCurves = list
        moveAngle = 0
        dimension.text = ""
        canvas.requestPaint()
        return true
    }

    /// Ends Move/Copy with the given offset (the turn and Copy from the panel).
    function applyMove(dx, dy) {
        const list = moveCurves.slice()
        const angle = moveAngle, copy = moveCopy
        if (list.length === 0 || (dx === 0 && dy === 0 && angle === 0))
            return false
        toolRequested("")
        moveRequested(list, dx, dy, angle, copy)
        return true
    }

    /// The middle of the curves' bounds (the turn's centre, as sketch.move_curves takes it).
    function curvesMiddle(list) {
        let lo = [Infinity, Infinity], hi = [-Infinity, -Infinity]
        for (const c of sketchCurves) {
            if (list.indexOf(c.index) < 0)
                continue
            const r = c.kind === "circle" ? c.radius : 0
            for (const p of c.points) {
                lo = [Math.min(lo[0], p[0] - r), Math.min(lo[1], p[1] - r)]
                hi = [Math.max(hi[0], p[0] + r), Math.max(hi[1], p[1] + r)]
            }
        }
        return lo[0] <= hi[0] ? [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2] : [0, 0]
    }

    /// A sketch curve (sketch.curves format) moved by `d` after turning by `angle` about `pivot`.
    function movedCurve(c, d, angle, pivot) {
        const a = angle * Math.PI / 180, ca = Math.cos(a), sa = Math.sin(a)
        const f = p => [pivot[0] + ca * (p[0] - pivot[0]) - sa * (p[1] - pivot[1]) + d[0],
                        pivot[1] + sa * (p[0] - pivot[0]) + ca * (p[1] - pivot[1]) + d[1]]
        const out = Object.assign({}, c)
        out.points = c.points.map(f)
        if (c.center)
            out.center = f(c.center)
        if (c.samples)
            out.samples = c.samples.map(f)
        return out
    }

    /// Index of the sketch curve within a few pixels of (x, y), or -1.
    function curveAt(x, y) {
        const p = screenToLocal(x, y)
        const q = screenToLocal(x + 1, y)
        if (p === null || q === null)
            return -1
        let best = -1, bestD = constraintLayer.pickPixels * Math.max(1e-9, dist(p, q))
        for (const c of sketchCurves) {
            const d = constraintLayer.curveDistance(p, c)
            if (d <= bestD) { best = c.index; bestD = d }
        }
        return best
    }

    /// Point (end, centre) of a sketch curve within the snap distance of (x, y):
    /// {point: {curve, point}, pos: [u, v]}, or null.
    function pointAt(x, y) {
        let best = null, bestD = snapPixels
        for (const c of constraintLayer.pointCandidates()) {
            if (c.point === "origin" || !c.pos)
                continue
            const s = toScreen(c.pos)
            const d = Math.hypot(s.x - x, s.y - y)
            if (d <= bestD) { best = c; bestD = d }
        }
        return best
    }

    /// Where a dragged point lands: another point within the snap distance, else the grid.
    /// The dragged point itself (`ref`, where it was pressed and where the live preview has
    /// it now, with the ends joined to it there) is not a snap target.
    function dragTarget(x, y, from, ref) {
        const p = screenToLocal(x, y)
        if (p === null)
            return null
        const self = [from]
        const now = constraintLayer.pointCandidates().find(c => c.point.curve === ref.curve && c.point.point === ref.point
                                                               && c.point.index === ref.index)
        if (now)
            self.push(now.pos)
        for (const c of snapPoints.concat([[0, 0]])) {
            if (self.some(q => dist(c, q) < 1e-6))
                continue
            const s = toScreen(c)
            if (Math.hypot(s.x - x, s.y - y) <= snapPixels)
                return [c[0], c[1]]
        }
        const step = gridStep(x, y)
        return [round6(Math.round(p[0] / step) * step), round6(Math.round(p[1] / step) * step)]
    }

    /// Click in select mode: a curve replaces the selection, or with Shift / Ctrl (Cmd)
    /// is added or removed; empty space clears it.
    function selectAt(x, y, modifiers) {
        const i = curveAt(x, y)
        const add = (modifiers & (Qt.ShiftModifier | Qt.ControlModifier)) !== 0
        if (i < 0) {
            if (!add)
                clearSelection()
            return
        }
        if (add)
            selectedCurves = selectedCurves.indexOf(i) >= 0 ? selectedCurves.filter(j => j !== i) : selectedCurves.concat([i])
        else
            selectedCurves = [i]
        canvas.requestPaint()
    }

    function reset() {
        points = []
        hover = null
        dimension.text = ""
        canvas.requestPaint()
    }

    // --- Coordinates ------------------------------------------------------------------------

    function v3(a) { return Qt.vector3d(a[0], a[1], a[2]) }

    function toWorld(p) {
        return v3(sketch.origin).plus(v3(sketch.xDir).times(p[0])).plus(v3(sketch.yDir).times(p[1]))
    }

    function toLocal(w) {
        const d = w.minus(v3(sketch.origin))
        return [d.dotProduct(v3(sketch.xDir)), d.dotProduct(v3(sketch.yDir))]
    }

    /// Where a sketch point is drawn: on a sketch over a surface, on the surface (where the
    /// finished curves are shown), so previews, hover markers and snapping match them.
    function toScreen(p) {
        const w = toWorld(p)
        if (sketch.surfacePart && !sketch.across)
            return viewport.viewPoint(viewport.controller.dropOntoPart(sketch.surfacePart, w, v3(sketch.normal)))
        return viewport.viewPoint(w)
    }

    function screenToLocal(x, y) {
        // Over a curved surface: the point of the surface under the cursor, seen along the
        // sketch normal, so drawing lands where the cursor is on the surface from any view.
        if (sketch.surfacePart && !sketch.across) {
            const s = viewport.pickOnPart(x, y, sketch.surfacePart)
            if (s !== null)
                return toLocal(s)
        }
        const w = viewport.planePoint(x, y, v3(sketch.origin), v3(sketch.normal))
        return w === null ? null : toLocal(w)
    }

    /// Grid step: a 1-2-5 value close to 12 pixels on screen.
    function gridStep(x, y) {
        const a = screenToLocal(x, y)
        const b = screenToLocal(x + 12, y)
        if (a === null || b === null)
            return 1
        const raw = Math.max(1e-6, Math.hypot(b[0] - a[0], b[1] - a[1]))
        const e = Math.pow(10, Math.floor(Math.log10(raw)))
        for (const m of [1, 2, 5, 10]) {
            if (m * e >= raw)
                return m * e
        }
        return 10 * e
    }

    function round6(v) { return Math.round(v * 1e6) / 1e6 }

    /// Snaps to existing points (and the current curve's points) within a few pixels,
    /// otherwise to the grid.
    function snap(x, y) {
        const p = screenToLocal(x, y)
        if (p === null)
            return null
        const candidates = snapPoints.concat(points).concat([[0, 0]])
        for (const c of candidates) {
            const s = toScreen(c)
            if (Math.hypot(s.x - x, s.y - y) <= snapPixels) {
                hoverSnapped = true
                return [c[0], c[1]]
            }
        }
        hoverSnapped = false
        const step = gridStep(x, y)
        return [round6(Math.round(p[0] / step) * step), round6(Math.round(p[1] / step) * step)]
    }

    function dist(a, b) { return Math.hypot(b[0] - a[0], b[1] - a[1]) }

    /// Number of clicks a tool needs.
    function clicksNeeded() {
        switch (tool) {
        case "tangent_arc": return 2
        case "arc":
        case "center_arc":
        case "circle_3point":
        case "rectangle_3point":
        case "slot": return 3
        case "spline": return Infinity // ended with Enter, a double-click or the first point
        default: return 2
        }
    }

    // --- Building curves ------------------------------------------------------------------

    /// True when size `i` of a curve is usable: the typed value (a positive number or an
    /// expression) when there is one, else the drawn size `drawn`.
    function sizeGiven(dim, i, drawn) {
        if (dim && dim.length > i)
            return typeof dim[i] === "string" ? dim[i].length > 0 : dim[i] > 0
        return drawn >= 1e-9
    }

    /// The value of a typed number or expression (user parameters, units); NaN with the
    /// error in the status bar when the expression does not evaluate.
    function evaluateTyped(value, what) {
        if (typeof value !== "string")
            return Number(value)
        const ev = root.viewport.controller.execute("parameter.evaluate", { expression: value })
        if (!ev.ok) {
            root.viewport.controller.showStatus(qsTr("%1: %2").arg(what).arg(ev.error))
            return NaN
        }
        return ev.result.value
    }

    /// Signed distance of `p` from the line a -> b, positive to its left.
    function leftOf(a, b, p) {
        const len = dist(a, b)
        return len < 1e-12 ? 0 : ((b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])) / len
    }

    /// Circle through three points as {center, radius}, or null when they are (nearly)
    /// on one line.
    function circleThrough(p1, p2, p3) {
        const [x1, y1] = p1, [x2, y2] = p2, [x3, y3] = p3
        const d = 2 * (x1 * (y2 - y3) + x2 * (y3 - y1) + x3 * (y1 - y2))
        const scale = Math.max(dist(p1, p2), dist(p2, p3), dist(p1, p3))
        if (scale < 1e-9 || Math.abs(d) < 1e-9 * scale * scale)
            return null
        const ux = ((x1 * x1 + y1 * y1) * (y2 - y3) + (x2 * x2 + y2 * y2) * (y3 - y1) + (x3 * x3 + y3 * y3) * (y1 - y2)) / d
        const uy = ((x1 * x1 + y1 * y1) * (x3 - x2) + (x2 * x2 + y2 * y2) * (x1 - x3) + (x3 * x3 + y3 * y3) * (x2 - x1)) / d
        return { center: [ux, uy], radius: Math.hypot(x1 - ux, y1 - uy) }
    }

    /// The end of a line or arc at `p` (a snapped point) with the direction leaving the curve
    /// there, as Tangent Arc starts from it; null when no line or arc ends at `p`.
    function tangentStartAt(p) {
        for (const c of sketchCurves) {
            if (c.kind !== "line" && c.kind !== "arc")
                continue
            for (const which of ["end", "start"]) {
                const pos = which === "end" ? c.points[c.points.length - 1] : c.points[0]
                if (dist(pos, p) > 1e-6)
                    continue
                let dir
                if (c.kind === "line") {
                    const a = c.points[0], b = c.points[1]
                    dir = which === "end" ? [b[0] - a[0], b[1] - a[1]] : [a[0] - b[0], a[1] - b[1]]
                } else {
                    // Direction of travel along the arc (counter-clockwise: the radius turned
                    // left), reversed at its start.
                    const q = c.points, ctr = c.center
                    const ccw = (q[1][0] - q[0][0]) * (q[2][1] - q[0][1]) - (q[1][1] - q[0][1]) * (q[2][0] - q[0][0]) > 0
                    const r = [pos[0] - ctr[0], pos[1] - ctr[1]]
                    dir = ccw ? [-r[1], r[0]] : [r[1], -r[0]]
                    if (which === "start")
                        dir = [-dir[0], -dir[1]]
                }
                if (Math.hypot(dir[0], dir[1]) > 1e-12)
                    return { ref: { curve: c.index, point: which }, pos: pos, dir: dir }
            }
        }
        return null
    }

    /// Preview of a tangent arc from `from` to `end`: { center, radius, a0, sweep }, or null when
    /// the end lies on the tangent line. sketch.add_curves builds the real arc.
    function tangentArc(from, end) {
        const t = from.dir, tl = Math.hypot(t[0], t[1])
        const d = [end[0] - from.pos[0], end[1] - from.pos[1]], dd = d[0] * d[0] + d[1] * d[1]
        const n = [-t[1] / tl, t[0] / tl]
        const side = n[0] * d[0] + n[1] * d[1]
        if (dd < 1e-18 || Math.abs(side) < 1e-9 * Math.sqrt(dd))
            return null
        const k = dd / (2 * side)
        const ctr = [from.pos[0] + n[0] * k, from.pos[1] + n[1] * k]
        const a0 = Math.atan2(from.pos[1] - ctr[1], from.pos[0] - ctr[0])
        let sweep = Math.atan2(end[1] - ctr[1], end[0] - ctr[0]) - a0
        if (side > 0)
            while (sweep <= 0) sweep += 2 * Math.PI
        else
            while (sweep >= 0) sweep -= 2 * Math.PI
        return { center: ctr, radius: Math.abs(k), a0: a0, sweep: sweep }
    }

    function curveFrom(pts, dim) {
        const a = pts[0], b = pts[1]
        switch (tool) {
        case "line":
            return { type: "line", start: a, end: b }
        case "rectangle": {
            let w = Math.abs(b[0] - a[0]), h = Math.abs(b[1] - a[1])
            const corner = [Math.min(a[0], b[0]), Math.min(a[1], b[1])]
            // A typed size counts, not the cursor offset (the cursor may be level with the
            // first corner).
            if (!sizeGiven(dim, 0, w) || !sizeGiven(dim, 1, h))
                return null
            return { type: "rectangle", corner: corner, width: dim && dim.length > 0 ? dim[0] : w,
                     height: dim && dim.length > 1 ? dim[1] : h }
        }
        case "center_rectangle": {
            // Centre, then a corner: the rectangle is symmetric about the centre.
            const w = 2 * Math.abs(b[0] - a[0]), h = 2 * Math.abs(b[1] - a[1])
            if (!sizeGiven(dim, 0, w) || !sizeGiven(dim, 1, h))
                return null
            return { type: "center_rectangle", center: a, width: dim && dim.length > 0 ? dim[0] : round6(w),
                     height: dim && dim.length > 1 ? dim[1] : round6(h) }
        }
        case "center_arc": {
            // Centre, start (sets the radius), end: counter-clockwise from start to end.
            if (pts.length < 3)
                return null
            const r = dist(a, b)
            if (r < 1e-9 || dist(a, pts[2]) < 1e-9)
                return null
            const deg = p => round6(Math.atan2(p[1] - a[1], p[0] - a[0]) * 180 / Math.PI)
            const s = deg(b), e = deg(pts[2])
            if (Math.abs(s - e) < 1e-6)
                return null
            return { type: "center_arc", center: a, radius: dim && dim.length > 0 ? dim[0] : typedRadius !== null ? typedRadius : round6(r),
                     startAngle: s, endAngle: e }
        }
        case "circle": {
            const d = dist(a, b) * 2
            if (d < 1e-9)
                return null
            return { type: "circle", center: a, diameter: dim && dim.length > 0 ? dim[0] : round6(d) }
        }
        case "rectangle_3point": {
            // One side from the first to the second click at any angle; the third click
            // (or a typed height) sets the other side.
            if (pts.length < 3 || dist(a, b) < 1e-9)
                return null
            const h = leftOf(a, b, pts[2])
            if (Math.abs(h) < 1e-9)
                return null
            return { type: "three_point_rectangle", start: a, end: b,
                     height: typedHeight !== null ? typedHeight : round6(h) }
        }
        case "circle_2point": {
            // The two points are the ends of a diameter; a typed diameter keeps the first
            // point and the direction to the second.
            const d = dist(a, b)
            if (d < 1e-9)
                return null
            if (dim && dim.length > 0) {
                // An expression is evaluated to place the centre and kept as the diameter.
                const typed = evaluateTyped(dim[0], qsTr("Diameter"))
                if (!(typed > 0))
                    return null
                const h = typed / 2 / d
                return { type: "circle", center: [round6(a[0] + (b[0] - a[0]) * h), round6(a[1] + (b[1] - a[1]) * h)], diameter: dim[0] }
            }
            return { type: "circle", center: [round6((a[0] + b[0]) / 2), round6((a[1] + b[1]) / 2)], diameter: round6(d) }
        }
        case "circle_3point": {
            if (pts.length < 3)
                return null
            const c = circleThrough(a, b, pts[2])
            if (!c)
                return null
            return { type: "circle", center: [round6(c.center[0]), round6(c.center[1])], diameter: round6(2 * c.radius) }
        }
        case "polygon": {
            const r = dist(a, b)
            if (r < 1e-9)
                return null
            return { type: "polygon", center: a, radius: dim && dim.length > 0 ? dim[0] : round6(r),
                     sides: polygonSides, angle: round6(Math.atan2(b[1] - a[1], b[0] - a[0]) * 180 / Math.PI) }
        }
        case "arc":
            if (pts.length < 3)
                return null
            return { type: "arc", start: a, through: pts[2], end: b }
        case "tangent_arc":
            if (!tangentFrom || !tangentArc(tangentFrom, b))
                return null
            return { type: "tangent_arc", from: tangentFrom.ref, end: b }
        case "slot": {
            if (pts.length < 3)
                return null
            // Width from the distance of the third point to the centre line.
            const dx = b[0] - a[0], dy = b[1] - a[1]
            const len = Math.hypot(dx, dy)
            if (len < 1e-9)
                return null
            const off = Math.abs((pts[2][0] - a[0]) * dy - (pts[2][1] - a[1]) * dx) / len
            return { type: "slot", start: a, end: b, width: dim && dim.length > 0 ? dim[0] : round6(2 * off) }
        }
        }
        return null
    }

    /// Ends the spline being drawn: an open one needs 2 fit points, a closed one 3.
    function finishSpline(closed) {
        if (points.length >= (closed ? 3 : 2))
            emitCurve({ type: "spline", points: points, closed: closed })
        points = []
        canvas.requestPaint()
    }

    /// Smooth preview through fit points (centripetal Catmull-Rom); the finished spline
    /// is drawn from the real curve.
    function splinePreview(pts, closed) {
        const n = pts.length
        if (n < 3)
            return pts
        const at = i => closed ? pts[(i + n) % n] : pts[Math.max(0, Math.min(n - 1, i))]
        const out = []
        const spans = closed ? n : n - 1
        for (let i = 0; i < spans; ++i) {
            const p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2)
            const knot = (a, b) => Math.max(1e-9, Math.sqrt(dist(a, b)))
            const t1 = knot(p0, p1), t2 = t1 + knot(p1, p2), t3 = t2 + knot(p2, p3)
            const lerp = (a, b, ta, tb, t) => [(a[0] * (tb - t) + b[0] * (t - ta)) / (tb - ta),
                                              (a[1] * (tb - t) + b[1] * (t - ta)) / (tb - ta)]
            for (let k = 0; k < 16; ++k) {
                const t = t1 + (t2 - t1) * k / 16
                const a1 = lerp(p0, p1, 0, t1, t), a2 = lerp(p1, p2, t1, t2, t), a3 = lerp(p2, p3, t2, t3, t)
                const b1 = lerp(a1, a2, 0, t2, t), b2 = lerp(a2, a3, t1, t3, t)
                out.push(lerp(b1, b2, t1, t2, t))
            }
        }
        out.push(closed ? pts[0] : pts[n - 1])
        return out
    }

    function emitCurve(curve) {
        if (curve !== null)
            curveDrawn(curve)
    }

    function click(p) {
        if (tool === "move") {
            // From a base point to where it goes.
            if (points.length === 0) {
                points = [p]
                canvas.requestPaint()
            } else {
                const from = points[0]
                points = []
                applyMove(p[0] - from[0], p[1] - from[1])
            }
            return
        }
        if (tool === "point") {
            // One click places a sketch point (Fusion: Point).
            emitCurve({ type: "point", at: p })
            points = []
            canvas.requestPaint()
            return
        }
        const pts = points.concat([p])
        if (tool === "spline") {
            // As in Fusion: a click on the first point closes the spline; a second click
            // on the last point (a double-click) ends it open.
            if (points.length >= 3 && dist(points[0], p) < 1e-9)
                finishSpline(true)
            else if (points.length > 0 && dist(points[points.length - 1], p) < 1e-9)
                finishSpline(false)
            else
                points = pts
            canvas.requestPaint()
            return
        }
        if (tool === "tangent_arc" && points.length === 0) {
            // It starts from the end of a line or arc (Fusion: Tangent Arc).
            const from = tangentStartAt(p)
            if (!from) {
                root.viewport.controller.showStatus(qsTr("Tangent arc: click the end of a line or arc"))
                return
            }
            points = [from.pos]
            tangentFrom = from
            canvas.requestPaint()
            return
        }
        if (tool === "line" && pts.length === 2) {
            if (dist(pts[0], pts[1]) < 1e-9)
                return
            emitCurve(curveFrom(pts, null))
            // Lines chain like in Fusion; clicking an existing point closes the chain.
            points = hoverSnapped && snapPoints.some(c => dist(c, p) < 1e-9) ? [] : [p]
        } else if (pts.length >= clicksNeeded()) {
            emitCurve(curveFrom(pts, null))
            points = []
        } else {
            points = pts
        }
        canvas.requestPaint()
    }

    /// Applies a typed dimension to the curve being drawn.
    function applyDimension(text) {
        const parts = text.split(",").map(s => s.trim()).filter(s => s.length > 0)
        if (parts.length === 0 || points.length === 0 || hover === null)
            return
        // Plain numbers stay numbers; anything else is an expression for the timeline.
        const values = parts.map(s => isNaN(Number(s)) ? s : Number(s))
        const a = points[0]
        if (tool === "rectangle_3point" && points.length === 2) {
            // The typed height goes to the side of the cursor.
            const h = evaluateTyped(values[0], qsTr("Height"))
            if (!(h > 0))
                return
            const side = leftOf(points[0], points[1], hover) < 0 ? -1 : 1
            const len = dist(a, points[1])
            const n = [-(points[1][1] - a[1]) / len, (points[1][0] - a[0]) / len]
            typedHeight = side > 0 ? values[0] : (typeof values[0] === "string" ? "-(" + values[0] + ")" : -values[0])
            click([round6(points[1][0] + n[0] * h * side), round6(points[1][1] + n[1] * h * side)])
        } else if (tool === "line" || (tool === "rectangle_3point" && points.length === 1)) {
            // A line length, or the first side of a 3-point rectangle, towards the cursor.
            const len = evaluateTyped(values[0], qsTr("Length"))
            if (!(len > 0))
                return
            const d = dist(a, hover)
            const dir = d > 1e-9 ? [(hover[0] - a[0]) / d, (hover[1] - a[1]) / d] : [1, 0]
            click([round6(a[0] + dir[0] * len), round6(a[1] + dir[1] * len)])
        } else if (tool === "center_arc" && points.length === 1) {
            // The typed radius places the start point towards the cursor; an expression is
            // evaluated for that and kept as the arc's radius.
            const r = evaluateTyped(values[0], qsTr("Radius"))
            if (!(r > 0))
                return
            const d = dist(a, hover)
            const dir = d > 1e-9 ? [(hover[0] - a[0]) / d, (hover[1] - a[1]) / d] : [1, 0]
            click([round6(a[0] + dir[0] * r), round6(a[1] + dir[1] * r)])
            typedRadius = values[0]
        } else if (tool === "rectangle" || tool === "center_rectangle" || tool === "circle" || tool === "circle_2point" || tool === "polygon") {
            const b = hover
            if ((tool === "rectangle" || tool === "center_rectangle") && values.length < 2)
                return
            const curve = curveFrom([a, dist(a, b) > 1e-9 ? b : [a[0] + 1, a[1] + 1]], values)
            if (tool === "rectangle" && curve) {
                // Keep the corner where the drawing started.
                curve.corner = a
                const sx = b[0] >= a[0] ? 1 : -1, sy = b[1] >= a[1] ? 1 : -1
                if (sx < 0 && !isNaN(Number(values[0]))) curve.corner = [round6(a[0] - Number(values[0])), curve.corner[1]]
                if (sy < 0 && !isNaN(Number(values[1]))) curve.corner = [curve.corner[0], round6(a[1] - Number(values[1]))]
            }
            emitCurve(curve)
            points = []
        }
        dimension.text = ""
        canvas.requestPaint()
    }

    /// Extend and Break hover: the curve under the cursor and what the click would make
    /// (Extend: the lengthened curve; Break: the piece it splits off), asked of the tool's
    /// read-only preview command.
    function pieceHover(x, y) {
        const i = curveAt(x, y)
        const at = screenToLocal(x, y)
        let preview = null
        if (i >= 0 && at !== null && sketch) {
            const extend = tool === "extend"
            const r = viewport.controller.execute(extend ? "sketch.extend_preview" : "sketch.break_preview", { sketch: sketch.id, curve: i, at: at })
            if (r.ok && (extend ? r.result.extendable : r.result.breakable)) {
                preview = { kind: r.result.kind, points: r.result.points }
                if (preview.kind === "arc")
                    preview.center = circumcenter(preview.points[0], preview.points[1], preview.points[2])
            }
        }
        hoverCurve = i
        piecePreview = preview
        canvas.requestPaint()
    }

    /// The centre of the circle through a, b and c (null when they are in line).
    function circumcenter(a, b, c) {
        const d = 2 * (a[0] * (b[1] - c[1]) + b[0] * (c[1] - a[1]) + c[0] * (a[1] - b[1]))
        if (Math.abs(d) < 1e-12)
            return null
        const a2 = a[0] * a[0] + a[1] * a[1], b2 = b[0] * b[0] + b[1] * b[1], c2 = c[0] * c[0] + c[1] * c[1]
        return [(a2 * (b[1] - c[1]) + b2 * (c[1] - a[1]) + c2 * (a[1] - b[1])) / d,
                (a2 * (c[0] - b[0]) + b2 * (a[0] - c[0]) + c2 * (b[0] - a[0])) / d]
    }

    /// Draws a solved sketch curve (sketch.curves format) as a polyline on the canvas.
    function strokeCurve(ctx, c) {
        if (c.kind === "point") {
            // A sketch point: a dot, ringed when highlighted.
            const q = toScreen(c.points[0])
            ctx.beginPath()
            ctx.arc(q.x, q.y, Math.max(3, ctx.lineWidth + 1), 0, 2 * Math.PI)
            ctx.stroke()
            return
        }
        let uv = []
        if (c.kind === "spline") {
            uv = c.samples || c.points
        } else if (c.kind === "line") {
            uv = c.points
        } else if (c.kind === "circle") {
            const r = c.radius
            for (let i = 0; i <= 72; ++i)
                uv.push([c.points[0][0] + r * Math.cos(2 * Math.PI * i / 72), c.points[0][1] + r * Math.sin(2 * Math.PI * i / 72)])
        } else if (c.kind === "arc") {
            const ctr = c.center
            const r = dist(c.points[0], ctr)
            const ang = q => Math.atan2(q[1] - ctr[1], q[0] - ctr[0])
            const norm = a => (a % (2 * Math.PI) + 2 * Math.PI) % (2 * Math.PI)
            const a0 = ang(c.points[0])
            let sweep = norm(ang(c.points[2]) - a0)
            if (norm(ang(c.points[1]) - a0) > sweep)
                sweep -= 2 * Math.PI
            for (let i = 0; i <= 48; ++i)
                uv.push([ctr[0] + r * Math.cos(a0 + sweep * i / 48), ctr[1] + r * Math.sin(a0 + sweep * i / 48)])
        }
        ctx.setLineDash(c.construction ? [6, 4] : []) // construction geometry stays dashed
        ctx.beginPath()
        for (let i = 0; i < uv.length; ++i) {
            const q = toScreen(uv[i])
            if (i === 0) ctx.moveTo(q.x, q.y); else ctx.lineTo(q.x, q.y)
        }
        ctx.stroke()
        ctx.setLineDash([])
    }

    // --- Input ----------------------------------------------------------------------------

    MouseArea {
        id: input
        anchors.fill: parent
        enabled: root.active || root.selecting
        hoverEnabled: true
        // Left only: right drag orbits and a right click opens the sketch menu (Viewport),
        // as everywhere else in the view.
        acceptedButtons: Qt.LeftButton
        cursorShape: root.active ? Qt.CrossCursor : Qt.ArrowCursor
        // Rectangle, circle and polygon also draw with press-drag-release (CAD-114); two
        // clicks keep working. A drag starts past a few pixels so a shaky click stays a click.
        property point pressAt
        property bool dragging: false
        property bool dragEnded: false
        /// Select mode: the point pressed on ({point, pos}) and whether it is being dragged.
        property var dragPoint: null
        property bool pointDragging: false
        readonly property bool dragTool: root.tool === "rectangle" || root.tool === "center_rectangle" || root.tool === "circle"
                                         || root.tool === "circle_2point" || root.tool === "polygon"
        onPressed: mouse => {
            if (root.selecting) {
                // Select mode: a curve under the cursor is picked here; anything else goes
                // on to the view (a click on empty space also clears the curve selection).
                // A press on a point of a curve may also start dragging it (CAD-123).
                pressAt = Qt.point(mouse.x, mouse.y)
                pointDragging = false
                dragPoint = root.pointAt(mouse.x, mouse.y)
                root.selectAt(mouse.x, mouse.y, mouse.modifiers)
                if (dragPoint === null && root.curveAt(mouse.x, mouse.y) < 0)
                    mouse.accepted = false
                root.forceActiveFocus()
                return
            }
            pressAt = Qt.point(mouse.x, mouse.y)
            dragging = false
            dragEnded = false
        }
        onPositionChanged: mouse => {
            if (root.selecting) {
                if (pressed && dragPoint !== null) {
                    if (!pointDragging && Math.hypot(mouse.x - pressAt.x, mouse.y - pressAt.y) > 4)
                        pointDragging = true
                    if (pointDragging) {
                        const to = root.dragTarget(mouse.x, mouse.y, dragPoint.pos, dragPoint.point)
                        if (to !== null)
                            root.pointDragged(dragPoint.point, to, false)
                    }
                    return
                }
                const i = root.curveAt(mouse.x, mouse.y)
                if (i !== root.hoverCurve) {
                    root.hoverCurve = i
                    canvas.requestPaint()
                }
                return
            }
            if (root.constraintTool) {
                constraintLayer.hoverAt(mouse.x, mouse.y)
                return
            }
            if (root.tool === "extend" || root.tool === "break") {
                root.pieceHover(mouse.x, mouse.y)
                return
            }
            if (root.tool === "trim" || root.tool === "mirror") {
                const i = root.curveAt(mouse.x, mouse.y)
                if (i !== root.hoverCurve) {
                    root.hoverCurve = i
                    canvas.requestPaint()
                }
                return
            }
            if (pressed && !dragging && dragTool && root.points.length === 0
                    && Math.hypot(mouse.x - pressAt.x, mouse.y - pressAt.y) > 8) {
                const first = root.snap(pressAt.x, pressAt.y)
                if (first !== null) {
                    root.points = [first]
                    dragging = true
                }
            }
            root.hover = root.snap(mouse.x, mouse.y)
            canvas.requestPaint()
        }
        // The press was taken away before the release: another window took focus, a menu or
        // dialog opened, or the window system cancelled the grab. A point drag is dropped as
        // with Esc (its preview would otherwise stay uncommitted and the drag stuck), and a
        // drawing drag stops.
        onCanceled: {
            root.cancelPointDrag()
            dragging = false
            dragEnded = false
        }
        onReleased: mouse => {
            if (root.selecting) {
                if (pointDragging) {
                    const to = root.dragTarget(mouse.x, mouse.y, dragPoint.pos, dragPoint.point)
                    root.pointDragged(dragPoint.point, to !== null ? to : dragPoint.pos, true)
                }
                pointDragging = false
                dragPoint = null
                return
            }
            if (!dragging)
                return
            dragging = false
            dragEnded = true // the click that follows this release is the drag's end, not a corner
            const p = root.snap(mouse.x, mouse.y)
            if (p !== null)
                root.click(p)
        }
        onClicked: mouse => {
            if (root.selecting)
                return
            if (root.tool === "mirror") {
                const i = root.curveAt(mouse.x, mouse.y)
                const c = root.sketchCurves.find(x => x.index === i)
                if (c && c.kind === "line" && root.mirrorCurves.length > 0) {
                    const list = root.mirrorCurves.filter(j => j !== i)
                    root.toolRequested("")
                    if (list.length > 0)
                        root.mirrorRequested(list, i)
                }
                root.forceActiveFocus()
                return
            }
            if (root.tool === "project") {
                // Only bodies take the click (the sketch's own plane and other sketches are
                // clicked through).
                const parts = root.viewport.controller.execute("document.parts")
                const bodies = parts.ok ? parts.result.parts.filter(p => p.kind === "body") : []
                const hit = root.viewport.pickPartAmong(mouse.x, mouse.y, bodies.map(p => p.id))
                if (hit) {
                    const b = bodies.find(p => p.id === hit.partId)
                    root.projectRequested(b.featureId, [hit.point.x, hit.point.y, hit.point.z])
                }
                root.forceActiveFocus()
                return
            }
            if (root.tool === "fillet") {
                root.filletAt(mouse.x, mouse.y)
                root.forceActiveFocus()
                return
            }
            if (root.tool === "extend" || root.tool === "break") {
                const i = root.curveAt(mouse.x, mouse.y)
                const at = root.screenToLocal(mouse.x, mouse.y)
                if (i >= 0 && at !== null) {
                    if (root.tool === "extend")
                        root.extendRequested(i, at)
                    else
                        root.breakRequested(i, at)
                }
                root.piecePreview = null
                root.forceActiveFocus()
                return
            }
            if (root.tool === "trim") {
                const i = root.curveAt(mouse.x, mouse.y)
                const at = root.screenToLocal(mouse.x, mouse.y)
                if (i >= 0 && at !== null)
                    root.trimRequested(i, at)
                root.forceActiveFocus()
                return
            }
            if (dragEnded) {
                dragEnded = false
                root.forceActiveFocus()
                return
            }
            if (root.constraintTool) {
                constraintLayer.click(mouse.x, mouse.y)
                return
            }
            const p = root.snap(mouse.x, mouse.y)
            if (p !== null)
                root.click(p)
            // Keep keyboard focus here so tool shortcuts (L, R, C, E) keep working;
            // typing a number moves to the dimension field (Keys.onPressed below).
            root.forceActiveFocus()
        }
    }

    // Typing a number starts the dimension input, as in Fusion.
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Escape) {
            if (!cancelPointDrag())
                endCurrent()
            event.accepted = true
        } else if (tool === "spline" && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
            finishSpline(false)
            event.accepted = true
        } else if (active && !constraintTool && tool !== "spline" && tool !== "tangent_arc" && (points.length > 0 || tool === "offset" || tool === "move") && /^[0-9.(\-]$/.test(event.text)) {
            dimension.text = event.text
            dimension.forceActiveFocus()
            event.accepted = true
        }
    }

    // --- Preview --------------------------------------------------------------------------

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            if (root.sketch === null)
                return
            // Across a surface (a fin): where the sketch plane cuts the surface, drawn on
            // top because the surface hides it.
            const line = root.sketch.surfaceLine || []
            if (line.length >= 2) {
                ctx.lineWidth = 2
                ctx.strokeStyle = Theme.warning
                ctx.beginPath()
                for (let i = 0; i + 1 < line.length; i += 2) {
                    const a = root.viewport.viewPoint(root.toWorld(line[i]))
                    const b = root.viewport.viewPoint(root.toWorld(line[i + 1]))
                    ctx.moveTo(a.x, a.y)
                    ctx.lineTo(b.x, b.y)
                }
                ctx.stroke()
            }
            // Sketch points, as dots (the 3D view draws them as small crosses).
            ctx.fillStyle = Theme.sketchColor
            for (const c of root.sketchCurves) {
                if (c.kind !== "point")
                    continue
                const q = root.toScreen(c.points[0])
                ctx.globalAlpha = c.construction ? 0.5 : 1
                ctx.beginPath()
                ctx.arc(q.x, q.y, 3.5, 0, 2 * Math.PI)
                ctx.fill()
            }
            ctx.globalAlpha = 1
            // Select mode: the curve under the cursor and the selected curves, highlighted.
            for (const c of root.sketchCurves) {
                const sel = root.selectedCurves.indexOf(c.index) >= 0 || root.mirrorCurves.indexOf(c.index) >= 0
                            || root.offsetCurves.indexOf(c.index) >= 0 || root.moveCurves.indexOf(c.index) >= 0
                if (!sel && c.index !== root.hoverCurve)
                    continue
                ctx.lineWidth = sel ? 3.5 : 3
                ctx.strokeStyle = Theme.accent
                ctx.globalAlpha = sel ? 1 : 0.45
                root.strokeCurve(ctx, c)
            }
            ctx.globalAlpha = 1
            // Move/Copy: where the curves go (from the base point to the cursor, turned).
            if (root.tool === "move" && root.moveCurves.length > 0) {
                const d = root.points.length === 1 && root.hover ? [root.hover[0] - root.points[0][0], root.hover[1] - root.points[0][1]] : [0, 0]
                if (d[0] !== 0 || d[1] !== 0 || root.moveAngle !== 0) {
                    const pivot = root.curvesMiddle(root.moveCurves)
                    ctx.lineWidth = 2
                    ctx.strokeStyle = Theme.accent
                    ctx.globalAlpha = 0.8
                    for (const c of root.sketchCurves) {
                        if (root.moveCurves.indexOf(c.index) >= 0)
                            root.strokeCurve(ctx, root.movedCurve(c, d, root.moveAngle, pivot))
                    }
                    ctx.setLineDash([])
                    ctx.globalAlpha = 1
                }
            }
            // Extend: the lengthened curve the click would make; Break: the piece it splits off.
            if ((root.tool === "extend" || root.tool === "break") && root.piecePreview && (root.piecePreview.kind === "line" || root.piecePreview.center)) {
                ctx.lineWidth = 2.5
                ctx.strokeStyle = Theme.accent
                root.strokeCurve(ctx, root.piecePreview)
                ctx.setLineDash([])
            }
            ctx.lineWidth = 1.5
            ctx.strokeStyle = Theme.sketchColor
            ctx.fillStyle = Theme.sketchColor
            const pts = root.hover ? root.points.concat([root.hover]) : root.points
            const s = pts.map(p => root.toScreen(p))
            if (root.active && pts.length >= 2) {
                const curve = root.curveFrom(pts.length >= 3 || root.clicksNeeded() === 2 ? pts : pts, null)
                ctx.beginPath()
                if (root.tool === "spline") {
                    // Snapped onto the first point (3 or more clicked): the closed spline.
                    const closing = root.points.length >= 3 && root.hover !== null && root.dist(root.points[0], root.hover) < 1e-9
                    const uv = root.splinePreview(closing ? root.points : pts, closing)
                    for (let i = 0; i < uv.length; ++i) {
                        const q = root.toScreen(uv[i])
                        if (i === 0) ctx.moveTo(q.x, q.y); else ctx.lineTo(q.x, q.y)
                    }
                } else if (root.tool === "line" || (pts.length === 2 && (root.tool === "arc" || root.tool === "slot" || root.tool === "circle_3point"
                                                                          || root.tool === "rectangle_3point"))) {
                    ctx.moveTo(s[0].x, s[0].y)
                    ctx.lineTo(s[1].x, s[1].y)
                } else if (root.tool === "rectangle" || root.tool === "center_rectangle") {
                    const b = pts[1]
                    // A centre rectangle mirrors the corner about the centre.
                    const a = root.tool === "rectangle" ? pts[0] : [2 * pts[0][0] - b[0], 2 * pts[0][1] - b[1]]
                    const c = [[a[0], a[1]], [b[0], a[1]], [b[0], b[1]], [a[0], b[1]]].map(p => root.toScreen(p))
                    ctx.moveTo(c[0].x, c[0].y)
                    for (let i = 1; i <= 4; ++i)
                        ctx.lineTo(c[i % 4].x, c[i % 4].y)
                } else if (root.tool === "tangent_arc") {
                    const arc = root.tangentFrom ? root.tangentArc(root.tangentFrom, pts[1]) : null
                    if (arc) {
                        for (let i = 0; i <= 64; ++i) {
                            const t = arc.a0 + arc.sweep * i / 64
                            const q = root.toScreen([arc.center[0] + arc.radius * Math.cos(t), arc.center[1] + arc.radius * Math.sin(t)])
                            if (i === 0) ctx.moveTo(q.x, q.y); else ctx.lineTo(q.x, q.y)
                        }
                    }
                } else if (root.tool === "center_arc") {
                    // Radius line, then the arc counter-clockwise from the start to the cursor.
                    const c = pts[0], r = root.dist(c, pts[1])
                    const a0 = Math.atan2(pts[1][1] - c[1], pts[1][0] - c[0])
                    let sweep = 2 * Math.PI
                    if (pts.length >= 3) {
                        sweep = Math.atan2(pts[2][1] - c[1], pts[2][0] - c[0]) - a0
                        if (sweep <= 0)
                            sweep += 2 * Math.PI
                    }
                    ctx.moveTo(s[0].x, s[0].y)
                    ctx.lineTo(s[1].x, s[1].y)
                    if (pts.length >= 3) {
                        for (let i = 0; i <= 64; ++i) {
                            const q = root.toScreen([c[0] + r * Math.cos(a0 + sweep * i / 64), c[1] + r * Math.sin(a0 + sweep * i / 64)])
                            if (i === 0) ctx.moveTo(q.x, q.y); else ctx.lineTo(q.x, q.y)
                        }
                    }
                } else if (root.tool === "rectangle_3point") {
                    // The first side, then the far side through the cursor.
                    const h = root.leftOf(pts[0], pts[1], pts[2]), len = root.dist(pts[0], pts[1])
                    if (len > 1e-9) {
                        const up = [-(pts[1][1] - pts[0][1]) / len * h, (pts[1][0] - pts[0][0]) / len * h]
                        const c = [pts[0], pts[1], [pts[1][0] + up[0], pts[1][1] + up[1]], [pts[0][0] + up[0], pts[0][1] + up[1]]].map(p => root.toScreen(p))
                        ctx.moveTo(c[0].x, c[0].y)
                        for (let i = 1; i <= 4; ++i)
                            ctx.lineTo(c[i % 4].x, c[i % 4].y)
                    }
                } else if (root.tool === "circle_2point" || root.tool === "circle_3point") {
                    // The circle through the points so far and the cursor.
                    if (curve) {
                        const r = curve.diameter / 2
                        for (let i = 0; i <= 64; ++i) {
                            const q = root.toScreen([curve.center[0] + r * Math.cos(2 * Math.PI * i / 64), curve.center[1] + r * Math.sin(2 * Math.PI * i / 64)])
                            if (i === 0) ctx.moveTo(q.x, q.y); else ctx.lineTo(q.x, q.y)
                        }
                    }
                } else if (root.tool === "circle" || root.tool === "polygon") {
                    const r = root.dist(pts[0], pts[1])
                    const n = root.tool === "polygon" ? root.polygonSides : 64
                    const a0 = Math.atan2(pts[1][1] - pts[0][1], pts[1][0] - pts[0][0])
                    for (let i = 0; i <= n; ++i) {
                        const a = a0 + 2 * Math.PI * i / n
                        const q = root.toScreen([pts[0][0] + r * Math.cos(a), pts[0][1] + r * Math.sin(a)])
                        if (i === 0) ctx.moveTo(q.x, q.y); else ctx.lineTo(q.x, q.y)
                    }
                } else if (root.tool === "arc" && pts.length >= 3) {
                    // Sample the circle through start, end, through.
                    const [x1, y1] = pts[0], [x2, y2] = pts[1], [x3, y3] = pts[2]
                    const d = 2 * (x1 * (y2 - y3) + x2 * (y3 - y1) + x3 * (y1 - y2))
                    if (Math.abs(d) > 1e-12) {
                        const ux = ((x1 * x1 + y1 * y1) * (y2 - y3) + (x2 * x2 + y2 * y2) * (y3 - y1) + (x3 * x3 + y3 * y3) * (y1 - y2)) / d
                        const uy = ((x1 * x1 + y1 * y1) * (x3 - x2) + (x2 * x2 + y2 * y2) * (x1 - x3) + (x3 * x3 + y3 * y3) * (x2 - x1)) / d
                        const r = Math.hypot(x1 - ux, y1 - uy)
                        const ang = p => Math.atan2(p[1] - uy, p[0] - ux)
                        const norm = a => (a % (2 * Math.PI) + 2 * Math.PI) % (2 * Math.PI)
                        const a1 = ang(pts[0])
                        let sweep = norm(ang(pts[1]) - a1)
                        if (norm(ang(pts[2]) - a1) > sweep)
                            sweep = sweep - 2 * Math.PI // through point is on the other side
                        for (let i = 0; i <= 64; ++i) {
                            const a = a1 + sweep * i / 64
                            const q = root.toScreen([ux + r * Math.cos(a), uy + r * Math.sin(a)])
                            if (i === 0) ctx.moveTo(q.x, q.y); else ctx.lineTo(q.x, q.y)
                        }
                    }
                } else if (root.tool === "slot" && pts.length >= 3 && curve) {
                    const a = pts[0], b = pts[1], w = curve.width / 2
                    const len = root.dist(a, b)
                    const n = [-(b[1] - a[1]) / len * w, (b[0] - a[0]) / len * w]
                    const q = [[a[0] + n[0], a[1] + n[1]], [b[0] + n[0], b[1] + n[1]],
                               [b[0] - n[0], b[1] - n[1]], [a[0] - n[0], a[1] - n[1]]].map(p => root.toScreen(p))
                    ctx.moveTo(q[0].x, q[0].y); ctx.lineTo(q[1].x, q[1].y)
                    ctx.moveTo(q[2].x, q[2].y); ctx.lineTo(q[3].x, q[3].y)
                }
                ctx.stroke()
            }
            // Clicked points and the cursor (filled when snapped to a point).
            for (let i = 0; i < s.length; ++i) {
                const snapped = i === s.length - 1 && root.hoverSnapped
                ctx.beginPath()
                ctx.arc(s[i].x, s[i].y, snapped ? 4.5 : 3, 0, 2 * Math.PI)
                if (snapped) ctx.fill(); else ctx.stroke()
            }
            // Live dimension next to the cursor.
            if (root.active && pts.length >= 2) {
                const a = pts[0], b = pts[1]
                let text = ""
                if (root.tool === "line" || root.tool === "arc" || root.tool === "slot" || (root.tool === "rectangle_3point" && pts.length === 2))
                    text = Preferences.formatLength(root.dist(a, b), Preferences.lengthUnit)
                else if (root.tool === "rectangle_3point")
                    text = Preferences.formatLength(root.dist(a, b), Preferences.lengthUnit) + " × " + Preferences.formatLength(Math.abs(root.leftOf(a, b, pts[2])), Preferences.lengthUnit)
                else if (root.tool === "rectangle")
                    text = Preferences.formatLength(Math.abs(b[0] - a[0]), Preferences.lengthUnit) + " × " + Preferences.formatLength(Math.abs(b[1] - a[1]), Preferences.lengthUnit)
                else if (root.tool === "center_rectangle")
                    text = Preferences.formatLength(2 * Math.abs(b[0] - a[0]), Preferences.lengthUnit) + " × " + Preferences.formatLength(2 * Math.abs(b[1] - a[1]), Preferences.lengthUnit)
                else if (root.tool === "center_arc")
                    text = "R " + Preferences.formatLength(root.dist(a, b), Preferences.lengthUnit)
                else if (root.tool === "tangent_arc") {
                    const arc = root.tangentFrom ? root.tangentArc(root.tangentFrom, b) : null
                    text = arc ? "R " + Preferences.formatLength(arc.radius, Preferences.lengthUnit) : ""
                }
                else if (root.tool === "circle")
                    text = "⌀ " + Preferences.formatLength(2 * root.dist(a, b), Preferences.lengthUnit)
                else if (root.tool === "circle_2point")
                    text = "⌀ " + Preferences.formatLength(root.dist(a, b), Preferences.lengthUnit)
                else if (root.tool === "circle_3point" && pts.length >= 3) {
                    const c = root.circleThrough(a, b, pts[2])
                    text = c ? "⌀ " + Preferences.formatLength(2 * c.radius, Preferences.lengthUnit) : ""
                }
                else if (root.tool === "polygon")
                    text = "R " + Preferences.formatLength(root.dist(a, b), Preferences.lengthUnit)
                ctx.font = "12px sans-serif"
                ctx.fillStyle = Theme.text
                const last = s[s.length - 1]
                ctx.fillText(text, last.x + 12, last.y - 10)
            }
        }
    }

    SketchConstraintLayer {
        id: constraintLayer
        anchors.fill: parent
        overlay: root
        curves: root.sketchCurves
        constraints: root.sketchConstraints
        tool: root.constraintTool ? root.tool : ""
        onConstraintAdded: c => root.constraintAdded(c)
    }

    // --- Sketch palette -------------------------------------------------------------------

    Pane {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 8
        padding: Theme.space3
        visible: root.sketch !== null
        background: Rectangle {
            color: Theme.overlay
            border.color: Theme.border
            radius: Theme.radius
        }

        ColumnLayout {
            spacing: 4
            Label {
                text: root.constraintTool ? constraintLayer.hint()
                    : root.tool === "" ? (root.selectedCurves.length > 0
                        ? qsTr("%n curve(s) selected: Delete removes, X construction, Esc clears", "", root.selectedCurves.length)
                        : qsTr("Click curves to select (Shift adds), drag points to move them; or pick a tool (L line, R rectangle, C circle, D dimension)"))
                    : root.tool === "project" ? qsTr("Project: click edges of bodies to bring them into the sketch (fixed)")
                    : root.tool === "offset" ? qsTr("Offset: type the distance and press Enter (negative goes inwards)")
                    : root.tool === "move" ? (root.points.length === 0
                        ? qsTr("Move/Copy %n curve(s): click a base point, or type dx, dy and press Enter", "", root.moveCurves.length)
                        : qsTr("Move/Copy: click where the base point goes"))
                    : root.tool === "fillet" ? qsTr("Fillet: click a corner of two lines; then edit its radius under Constraints")
                    : root.tool === "mirror" ? qsTr("Mirror: click the line to mirror the %n selected curve(s) about", "", root.mirrorCurves.length)
                    : root.tool === "trim" ? qsTr("Trim: click the piece of a curve to remove (up to where other curves cross it)")
                    : root.tool === "extend" ? qsTr("Extend: click near the end of a line or arc to lengthen it to the next curve")
                    : root.tool === "break" ? qsTr("Break: click a curve to split it where other curves cross it")
                    : root.tool === "point" ? qsTr("Point: click to place a sketch point (it snaps to curve ends and centres)")
                    : root.tool === "line" ? qsTr("Line: click points; Esc (or right-click › End Curve) ends the chain")
                    : root.tool === "rectangle" ? qsTr("Rectangle: drag or click two corners; type width, height")
                    : root.tool === "center_rectangle" ? qsTr("Center rectangle: click the centre, then a corner; type width, height")
                    : root.tool === "center_arc" ? qsTr("Center point arc: click the centre, the start, then the end (counter-clockwise); type the radius")
                    : root.tool === "tangent_arc" ? qsTr("Tangent arc: click the end of a line or arc, then the end of the arc")
                    : root.tool === "circle" ? qsTr("Circle: click centre and a point; type the diameter")
                    : root.tool === "rectangle_3point" ? qsTr("3-point rectangle: click both ends of one side (any angle), then the far side; type the length, then the height")
                    : root.tool === "circle_2point" ? qsTr("2-point circle: click both ends of a diameter; type the diameter")
                    : root.tool === "circle_3point" ? qsTr("3-point circle: click three points on the circle")
                    : root.tool === "arc" ? qsTr("Arc: click start, end, then a point on the arc")
                    : root.tool === "polygon" ? qsTr("Polygon: click centre and a corner; type the radius")
                    : root.tool === "spline" ? qsTr("Spline: click fit points; Enter, a double-click or Esc ends it, the first point closes it")
                    : qsTr("Slot: click both centres, then the width")
                font.weight: Font.DemiBold
            }
            // Constraint tools, like Fusion's sketch palette.
            Flow {
                Layout.preferredWidth: 300
                spacing: 2
                Repeater {
                    model: [
                        { tool: "horizontal", text: "H", tip: qsTr("Horizontal") },
                        { tool: "vertical", text: "V", tip: qsTr("Vertical") },
                        { tool: "coincident", text: "●", tip: qsTr("Coincident / point on curve") },
                        { tool: "tangent", text: "T", tip: qsTr("Tangent") },
                        { tool: "equal", text: "=", tip: qsTr("Equal") },
                        { tool: "parallel", text: "∥", tip: qsTr("Parallel") },
                        { tool: "perpendicular", text: "⊥", tip: qsTr("Perpendicular") },
                        { tool: "concentric", text: "◎", tip: qsTr("Concentric") },
                        { tool: "midpoint", text: "M", tip: qsTr("Midpoint") },
                        { tool: "collinear", text: "≡", tip: qsTr("Collinear") },
                        { tool: "symmetric", text: "⇋", tip: qsTr("Symmetric") },
                        { tool: "fix", text: "⚓", tip: qsTr("Fix") }
                    ]
                    delegate: IconButton {
                        required property var modelData
                        iconName: modelData.tool
                        iconSize: Theme.iconMedium
                        implicitWidth: 30
                        implicitHeight: 30
                        tip: modelData.tip
                        checked: root.tool === modelData.tool
                        onClicked: root.toolRequested(modelData.tool)
                    }
                }
            }
            RowLayout {
                visible: root.active && !root.constraintTool && root.tool !== "trim" && root.tool !== "extend" && root.tool !== "break" && root.tool !== "point" && root.tool !== "mirror" && root.tool !== "fillet" && root.tool !== "project" && root.tool !== "spline"
                         && root.tool !== "tangent_arc"
                         && root.tool !== "circle_3point"
                Label { text: root.tool === "rectangle" || root.tool === "center_rectangle" ? qsTr("Width, height")
                            : root.tool === "center_arc" ? qsTr("Radius") : root.tool === "line" ? qsTr("Length")
                            : root.tool === "circle_2point" ? qsTr("Diameter")
                            : root.tool === "rectangle_3point" ? (root.points.length >= 2 ? qsTr("Height") : qsTr("Length"))
                            : root.tool === "offset" ? qsTr("Distance")
                            : root.tool === "move" ? qsTr("dx, dy") : qsTr("Size") }
                TextField {
                    id: dimension
                    objectName: "sketchDimension"
                    Layout.preferredWidth: 140
                    placeholderText: qsTr("e.g. 40 or w/2")
                    onAccepted: {
                        if (root.tool === "move") {
                            const v = text.split(/[,;\s]+/).filter(t => t.length > 0).map(Number)
                            if (v.length >= 1 && v.length <= 2 && v.every(isFinite))
                                root.applyMove(v[0], v.length > 1 ? v[1] : 0)
                        } else if (root.tool === "offset") {
                            const d = Number(text)
                            const list = root.offsetCurves.slice()
                            if (isFinite(d) && d !== 0 && list.length > 0) {
                                root.toolRequested("")
                                root.offsetRequested(list, d)
                            }
                        } else {
                            root.applyDimension(text)
                        }
                        root.forceActiveFocus()
                    }
                    Keys.onEscapePressed: { root.reset(); root.forceActiveFocus() }
                }
                // Move/Copy: the turn about the curves' middle, and Copy (Fusion: Create Copy).
                Label { visible: root.tool === "move"; text: qsTr("Angle") }
                TextField {
                    id: moveAngleField
                    objectName: "sketchMoveAngle"
                    visible: root.tool === "move"
                    Layout.preferredWidth: 60
                    text: root.moveAngle !== 0 ? String(root.moveAngle) : ""
                    placeholderText: "0"
                    onTextEdited: { const a = Number(text); root.moveAngle = isFinite(a) ? a : 0; canvas.requestPaint() }
                    // Enter with no offset typed: just the turn.
                    onAccepted: { if (dimension.text.trim().length === 0) root.applyMove(0, 0); root.forceActiveFocus() }
                }
                CheckBox {
                    objectName: "sketchMoveCopy"
                    visible: root.tool === "move"
                    text: qsTr("Copy")
                    checked: root.moveCopy
                    onToggled: root.moveCopy = checked
                }
                Label { visible: root.tool === "polygon"; text: qsTr("Sides") }
                SpinBox {
                    visible: root.tool === "polygon"
                    from: 3
                    to: 64
                    value: root.polygonSides
                    onValueModified: root.polygonSides = value
                }
            }
            Label {
                visible: root.degreesOfFreedom >= 0
                text: root.degreesOfFreedom === 0 ? qsTr("Fully constrained")
                    : qsTr("%n degree(s) of freedom", "", root.degreesOfFreedom)
                color: root.degreesOfFreedom === 0 ? Theme.success : Theme.textMuted
            }
            Label {
                visible: root.sketchConstraints.length > 0
                text: qsTr("Constraints")
                font.bold: true
            }
            ListView {
                id: constraintList
                visible: root.sketchConstraints.length > 0
                Layout.preferredWidth: 300
                Layout.preferredHeight: Math.min(180, contentHeight)
                clip: true
                model: root.sketchConstraints
                delegate: RowLayout {
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    Label {
                        Layout.fillWidth: true
                        // Symmetric names its two items and the line: "symmetric #5, #6 about #0".
                        text: modelData.type === "symmetric" && modelData.points !== undefined
                              ? qsTr("symmetric %1 about #%2").arg(modelData.points.map(p => typeof p === "object" ? "#" + p.curve : String(p)).join(", ")).arg(modelData.curve)
                              : modelData.type.replace(/_/g, " ") + (modelData.curve !== undefined ? " #" + modelData.curve
                              : modelData.curves !== undefined ? " #" + modelData.curves.join(", #") : "")
                        elide: Text.ElideRight
                    }
                    TextField {
                        visible: modelData.value !== undefined
                        Layout.preferredWidth: 90
                        text: modelData.value !== undefined ? String(modelData.value) : ""
                        selectByMouse: true
                        onEditingFinished: {
                            if (text !== String(modelData.value) && text.trim().length > 0)
                                root.constraintEdited(index, isNaN(Number(text)) ? text.trim() : Number(text))
                        }
                    }
                    ToolButton {
                        text: "✕"
                        onClicked: root.constraintRemoved(index)
                    }
                }
            }
        }
    }
}
