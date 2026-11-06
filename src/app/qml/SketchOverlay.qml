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
    signal curveDrawn(var curve)

    readonly property bool active: sketch !== null && tool !== ""
    /// Clicked points of the curve being drawn, [[u, v], ...].
    property var points: []
    property var hover: null
    property bool hoverSnapped: false
    readonly property real snapPixels: 10
    property int polygonSides: 6

    enabled: sketch !== null
    visible: sketch !== null

    onToolChanged: reset()
    onSketchChanged: reset()

    function reset() {
        points = []
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

    function toScreen(p) {
        return viewport.viewPoint(toWorld(p))
    }

    function screenToLocal(x, y) {
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

    // --- Input ----------------------------------------------------------------------------

    MouseArea {
        anchors.fill: parent
        enabled: root.active
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: root.active ? Qt.CrossCursor : Qt.ArrowCursor
        onPositionChanged: mouse => {
            root.hover = root.snap(mouse.x, mouse.y)
            canvas.requestPaint()
        }
        onClicked: mouse => {
            if (mouse.button === Qt.RightButton) {
                root.reset()
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
            reset()
            event.accepted = true
        } else if (active && points.length > 0 && /^[0-9.(\-]$/.test(event.text)) {
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
            ctx.lineWidth = 1.5
            ctx.strokeStyle = "#1f5fbf"
            ctx.fillStyle = "#1f5fbf"
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
                    text = Preferences.formatLength(root.dist(a, b))
                else if (root.tool === "rectangle")
                    text = Preferences.formatLength(Math.abs(b[0] - a[0])) + " × " + Preferences.formatLength(Math.abs(b[1] - a[1]))
                else if (root.tool === "circle")
                    text = "⌀ " + Preferences.formatLength(2 * root.dist(a, b))
                else if (root.tool === "polygon")
                    text = "R " + Preferences.formatLength(root.dist(a, b))
                ctx.font = "12px sans-serif"
                ctx.fillStyle = "#123a75"
                const last = s[s.length - 1]
                ctx.fillText(text, last.x + 12, last.y - 10)
            }
        }
    }

    // --- Sketch palette -------------------------------------------------------------------

    Pane {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 8
        padding: 8
        opacity: 0.95
        visible: root.sketch !== null

        ColumnLayout {
            spacing: 4
            Label {
                text: root.tool === "" ? qsTr("Sketch: pick a tool (L line, R rectangle, C circle)")
                    : root.tool === "line" ? qsTr("Line: click points; right-click or Esc ends the chain")
                    : root.tool === "rectangle" ? qsTr("Rectangle: click two corners; type width, height")
                    : root.tool === "circle" ? qsTr("Circle: click centre and a point; type the diameter")
                    : root.tool === "arc" ? qsTr("Arc: click start, end, then a point on the arc")
                    : root.tool === "polygon" ? qsTr("Polygon: click centre and a corner; type the radius")
                    : qsTr("Slot: click both centres, then the width")
                font.bold: true
            }
            RowLayout {
                visible: root.active
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
        }
    }
}
