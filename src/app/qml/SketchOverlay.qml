// SPDX-License-Identifier: GPL-3.0-or-later
// Drawing tools of the sketch environment (Fusion: Line L, 2-point Rectangle R, Center
// Diameter Circle C, 3-point Arc, Polygon, Slot). Clicks are projected onto the sketch
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

    readonly property bool active: sketch !== null && tool !== ""
    /// With no tool (Fusion's select mode): curves picked with the mouse, indices of
    /// sketch.curves, and the curve under the cursor (-1 none).
    property var selectedCurves: []
    property int hoverCurve: -1
    readonly property bool selecting: sketch !== null && tool === ""
    /// Clicked points of the curve being drawn, [[u, v], ...].
    property var points: []
    property var hover: null
    property bool hoverSnapped: false
    readonly property real snapPixels: 10
    property int polygonSides: 6

    enabled: sketch !== null
    visible: sketch !== null

    onToolChanged: { reset(); clearSelection() }
    onSketchChanged: { reset(); clearSelection() }
    // The curves changed (a delete, an undo): drop picks that no longer exist.
    onSketchCurvesChanged: {
        const kept = selectedCurves.filter(i => i < sketchCurves.length)
        if (kept.length !== selectedCurves.length)
            selectedCurves = kept
        hoverCurve = -1
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

    /// Ends the curve being drawn (a line chain) or the constraint pick, as Esc does.
    function endCurrent() {
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
        const now = constraintLayer.pointCandidates().find(c => c.point.curve === ref.curve && c.point.point === ref.point)
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
        case "arc":
        case "slot": return 3
        default: return 2
        }
    }

    // --- Building curves ------------------------------------------------------------------

    function curveFrom(pts, dim) {
        const a = pts[0], b = pts[1]
        switch (tool) {
        case "line":
            return { type: "line", start: a, end: b }
        case "rectangle": {
            let w = Math.abs(b[0] - a[0]), h = Math.abs(b[1] - a[1])
            const corner = [Math.min(a[0], b[0]), Math.min(a[1], b[1])]
            if (w < 1e-9 || h < 1e-9)
                return null
            return { type: "rectangle", corner: corner, width: dim && dim.length > 0 ? dim[0] : w,
                     height: dim && dim.length > 1 ? dim[1] : h }
        }
        case "circle": {
            const d = dist(a, b) * 2
            if (d < 1e-9)
                return null
            return { type: "circle", center: a, diameter: dim && dim.length > 0 ? dim[0] : round6(d) }
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

    function emitCurve(curve) {
        if (curve !== null)
            curveDrawn(curve)
    }

    function click(p) {
        const pts = points.concat([p])
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
        if (tool === "line") {
            const len = Number(values[0])
            if (!(len > 0))
                return
            const d = dist(a, hover)
            const dir = d > 1e-9 ? [(hover[0] - a[0]) / d, (hover[1] - a[1]) / d] : [1, 0]
            click([round6(a[0] + dir[0] * len), round6(a[1] + dir[1] * len)])
        } else if (tool === "rectangle" || tool === "circle" || tool === "polygon") {
            const b = hover
            if (tool === "rectangle" && values.length < 2)
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

    /// Draws a solved sketch curve (sketch.curves format) as a polyline on the canvas.
    function strokeCurve(ctx, c) {
        let uv = []
        if (c.kind === "line" || c.kind === "spline") {
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
        readonly property bool dragTool: root.tool === "rectangle" || root.tool === "circle" || root.tool === "polygon"
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
            if (root.tool === "trim") {
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
            cancelPointDrag()
            reset()
            constraintLayer.reset()
            event.accepted = true
        } else if (active && !constraintTool && points.length > 0 && /^[0-9.(\-]$/.test(event.text)) {
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
            // Select mode: the curve under the cursor and the selected curves, highlighted.
            for (const c of root.sketchCurves) {
                const sel = root.selectedCurves.indexOf(c.index) >= 0
                if (!sel && c.index !== root.hoverCurve)
                    continue
                ctx.lineWidth = sel ? 3.5 : 3
                ctx.strokeStyle = Theme.accent
                ctx.globalAlpha = sel ? 1 : 0.45
                root.strokeCurve(ctx, c)
            }
            ctx.globalAlpha = 1
            ctx.lineWidth = 1.5
            ctx.strokeStyle = Theme.sketchColor
            ctx.fillStyle = Theme.sketchColor
            const pts = root.hover ? root.points.concat([root.hover]) : root.points
            const s = pts.map(p => root.toScreen(p))
            if (root.active && pts.length >= 2) {
                const curve = root.curveFrom(pts.length >= 3 || root.clicksNeeded() === 2 ? pts : pts, null)
                ctx.beginPath()
                if (root.tool === "line" || (pts.length === 2 && (root.tool === "arc" || root.tool === "slot"))) {
                    ctx.moveTo(s[0].x, s[0].y)
                    ctx.lineTo(s[1].x, s[1].y)
                } else if (root.tool === "rectangle") {
                    const a = pts[0], b = pts[1]
                    const c = [[a[0], a[1]], [b[0], a[1]], [b[0], b[1]], [a[0], b[1]]].map(p => root.toScreen(p))
                    ctx.moveTo(c[0].x, c[0].y)
                    for (let i = 1; i <= 4; ++i)
                        ctx.lineTo(c[i % 4].x, c[i % 4].y)
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
                if (root.tool === "line" || root.tool === "arc" || root.tool === "slot")
                    text = Preferences.formatLength(root.dist(a, b), Preferences.lengthUnit)
                else if (root.tool === "rectangle")
                    text = Preferences.formatLength(Math.abs(b[0] - a[0]), Preferences.lengthUnit) + " × " + Preferences.formatLength(Math.abs(b[1] - a[1]), Preferences.lengthUnit)
                else if (root.tool === "circle")
                    text = "⌀ " + Preferences.formatLength(2 * root.dist(a, b), Preferences.lengthUnit)
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
                    : root.tool === "trim" ? qsTr("Trim: click the piece of a curve to remove (up to where other curves cross it)")
                    : root.tool === "line" ? qsTr("Line: click points; Esc (or right-click › End Curve) ends the chain")
                    : root.tool === "rectangle" ? qsTr("Rectangle: drag or click two corners; type width, height")
                    : root.tool === "circle" ? qsTr("Circle: click centre and a point; type the diameter")
                    : root.tool === "arc" ? qsTr("Arc: click start, end, then a point on the arc")
                    : root.tool === "polygon" ? qsTr("Polygon: click centre and a corner; type the radius")
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
                visible: root.active && !root.constraintTool && root.tool !== "trim"
                Label { text: root.tool === "rectangle" ? qsTr("Width, height") : root.tool === "line" ? qsTr("Length") : qsTr("Size") }
                TextField {
                    id: dimension
                    Layout.preferredWidth: 140
                    placeholderText: qsTr("e.g. 40 or w/2")
                    onAccepted: { root.applyDimension(text); root.forceActiveFocus() }
                    Keys.onEscapePressed: { root.reset(); root.forceActiveFocus() }
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
                        text: modelData.type.replace(/_/g, " ") + (modelData.curve !== undefined ? " #" + modelData.curve
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
