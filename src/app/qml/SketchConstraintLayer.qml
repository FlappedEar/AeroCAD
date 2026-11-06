// SPDX-License-Identifier: GPL-3.0-or-later
// Sketch constraints and dimensions (Fusion: Sketch > Constraints, Sketch Dimension D).
// Shows the constraints of the active sketch as small glyphs and dimension labels, and
// implements the constraint tools: the user clicks curves or points, and a finished
// selection is reported with constraintAdded() in the format of sketch.add_constraints.
import QtQuick
import QtQuick.Controls

Item {
    id: root

    /// The SketchOverlay (coordinate helpers).
    required property Item overlay
    /// Solved curves of the sketch, as returned by sketch.curves.
    property var curves: []
    /// Constraints of the sketch, as returned by sketch.curves (with "resolved" values).
    property var constraints: []
    property string tool: ""
    signal constraintAdded(var constraint)

    /// Picked items of the current tool: { point: ref, pos: [u, v] } or { curve: index }.
    property var picks: []
    property var hoverPick: null
    /// Dimension waiting for its value: { constraint, value, at: [u, v] }.
    property var pending: null
    readonly property real pickPixels: 8

    readonly property var toolNames: ["dimension", "horizontal", "vertical", "coincident", "tangent",
        "equal", "parallel", "perpendicular", "concentric", "midpoint", "collinear", "fix"]

    onToolChanged: reset()
    onCurvesChanged: canvas.requestPaint()
    onConstraintsChanged: canvas.requestPaint()

    function reset() {
        picks = []
        pending = null
        valueField.text = ""
        canvas.requestPaint()
    }

    function hint() {
        switch (tool) {
        case "dimension": return qsTr("Dimension: click a line, circle, arc, two points or two lines; type the value")
        case "horizontal": return qsTr("Horizontal: click a line (or two points)")
        case "vertical": return qsTr("Vertical: click a line (or two points)")
        case "coincident": return qsTr("Coincident: click a point, then a point or a curve")
        case "tangent": return qsTr("Tangent: click a line or arc, then an arc or circle")
        case "equal": return qsTr("Equal: click two lines or two circles/arcs")
        case "parallel": return qsTr("Parallel: click two lines")
        case "perpendicular": return qsTr("Perpendicular: click two lines")
        case "concentric": return qsTr("Concentric: click two circles or arcs")
        case "midpoint": return qsTr("Midpoint: click a point, then a line")
        case "collinear": return qsTr("Collinear: click two lines")
        case "fix": return qsTr("Fix: click a point or a curve")
        }
        return ""
    }

    // --- Geometry of the solved curves ----------------------------------------------------

    function dist(a, b) { return Math.hypot(b[0] - a[0], b[1] - a[1]) }

    function curveCenter(c) {
        return c.kind === "circle" ? c.points[0] : c.center
    }

    function curveRadius(c) {
        return c.kind === "circle" ? c.radius : dist(c.points[0], c.center)
    }

    function isCirc(i) {
        const c = curves[i]
        return !!c && (c.kind === "circle" || c.kind === "arc")
    }

    function isLine(i) { return !!curves[i] && curves[i].kind === "line" }

    /// Points that constraints can refer to.
    function pointCandidates() {
        const out = []
        for (const c of curves) {
            if (c.kind === "line" || c.kind === "spline") {
                out.push({ point: { curve: c.index, point: "start" }, pos: c.points[0] })
                out.push({ point: { curve: c.index, point: "end" }, pos: c.points[c.points.length - 1] })
            } else if (c.kind === "arc") {
                out.push({ point: { curve: c.index, point: "start" }, pos: c.points[0] })
                out.push({ point: { curve: c.index, point: "end" }, pos: c.points[2] })
                out.push({ point: { curve: c.index, point: "center" }, pos: c.center })
            } else if (c.kind === "circle") {
                out.push({ point: { curve: c.index, point: "center" }, pos: c.points[0] })
            }
        }
        out.push({ point: "origin", pos: [0, 0] })
        return out
    }

    function segmentDistance(p, a, b) {
        const dx = b[0] - a[0], dy = b[1] - a[1]
        const l2 = dx * dx + dy * dy
        const t = l2 > 0 ? Math.max(0, Math.min(1, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2)) : 0
        return dist(p, [a[0] + t * dx, a[1] + t * dy])
    }

    function curveDistance(p, c) {
        if (c.kind === "line")
            return segmentDistance(p, c.points[0], c.points[1])
        if (c.kind === "circle")
            return Math.abs(dist(p, curveCenter(c)) - curveRadius(c))
        if (c.kind === "arc") {
            // Only the drawn sweep counts (start -> through the middle point -> end); outside
            // it the nearest point of the arc is one of its ends.
            const ctr = curveCenter(c)
            const angle = q => Math.atan2(q[1] - ctr[1], q[0] - ctr[0])
            const a0 = angle(c.points[0])
            const span = q => { const d = (angle(q) - a0) % (2 * Math.PI); return d < 0 ? d + 2 * Math.PI : d }
            const sEnd = span(c.points[2])
            const sP = span(p)
            const counterClockwise = span(c.points[1]) < sEnd
            if (counterClockwise ? sP <= sEnd : sP >= sEnd)
                return Math.abs(dist(p, ctr) - curveRadius(c))
            return Math.min(dist(p, c.points[0]), dist(p, c.points[2]))
        }
        let best = Infinity
        for (let i = 0; i + 1 < c.points.length; ++i)
            best = Math.min(best, segmentDistance(p, c.points[i], c.points[i + 1]))
        return best
    }

    /// Which kinds of item the tool accepts for the next pick.
    function wantsPoints() {
        switch (tool) {
        case "coincident":
        case "midpoint": return picks.length === 0 || tool === "coincident"
        case "dimension":
        case "fix":
        case "horizontal":
        case "vertical": return true
        default: return false
        }
    }

    function wantsCurves() {
        switch (tool) {
        case "coincident": return picks.length === 1
        case "midpoint": return picks.length === 1
        default: return true
        }
    }

    /// The item under the cursor: points win over curves, as in Fusion.
    function pickAt(x, y) {
        const p = overlay.screenToLocal(x, y)
        const q = overlay.screenToLocal(x + 1, y)
        if (p === null || q === null)
            return null
        const tol = pickPixels * Math.max(1e-9, dist(p, q))
        if (wantsPoints()) {
            let best = null, bestD = tol
            for (const c of pointCandidates()) {
                const d = dist(p, c.pos)
                if (d <= bestD) { best = c; bestD = d }
            }
            if (best)
                return best
        }
        if (wantsCurves()) {
            let best = null, bestD = tol
            for (const c of curves) {
                const d = curveDistance(p, c)
                if (d <= bestD) { best = { curve: c.index }; bestD = d }
            }
            return best
        }
        return null
    }

    function hoverAt(x, y) {
        hoverPick = pickAt(x, y)
        canvas.requestPaint()
    }

    // --- Tools ----------------------------------------------------------------------------

    function lineDir(i) {
        const c = curves[i]
        return [c.points[1][0] - c.points[0][0], c.points[1][1] - c.points[0][1]]
    }

    function round2(v) { return Math.round(v * 100) / 100 }

    /// Dimension for the current picks, or null if more picks are needed.
    function dimensionFor(ps) {
        const a = ps[0], b = ps[1]
        if (ps.length === 1 && a.curve !== undefined) {
            const c = curves[a.curve]
            if (c.kind === "line")
                return { constraint: { type: "length", curve: a.curve }, value: dist(c.points[0], c.points[1]) }
            if (c.kind === "circle")
                return { constraint: { type: "diameter", curve: a.curve }, value: 2 * c.radius }
            if (c.kind === "arc")
                return { constraint: { type: "radius", curve: a.curve }, value: curveRadius(c) }
            return null
        }
        if (ps.length !== 2)
            return null
        if (a.point !== undefined && b.point !== undefined)
            return { constraint: { type: "distance", points: [a.point, b.point] }, value: dist(a.pos, b.pos) }
        const pt = a.point !== undefined ? a : b.point !== undefined ? b : null
        const cu = a.curve !== undefined ? a : b.curve !== undefined ? b : null
        if (pt && cu && isLine(cu.curve)) {
            const c = curves[cu.curve]
            const d = lineDir(cu.curve), l = Math.hypot(d[0], d[1])
            const off = Math.abs(d[0] * (pt.pos[1] - c.points[0][1]) - d[1] * (pt.pos[0] - c.points[0][0])) / l
            return { constraint: { type: "distance", point: pt.point, curve: cu.curve }, value: off }
        }
        if (a.curve !== undefined && b.curve !== undefined && isLine(a.curve) && isLine(b.curve)) {
            const d1 = lineDir(a.curve), d2 = lineDir(b.curve)
            const ang = Math.abs(Math.atan2(d1[0] * d2[1] - d1[1] * d2[0], d1[0] * d2[0] + d1[1] * d2[1])) * 180 / Math.PI
            return { constraint: { type: "angle", curves: [a.curve, b.curve] }, value: ang }
        }
        return null
    }

    function midpointOf(ps) {
        let u = 0, v = 0, n = 0
        for (const p of ps) {
            const pos = p.pos || anchorOfCurve(p.curve)
            u += pos[0]; v += pos[1]; ++n
        }
        return [u / n, v / n]
    }

    function emit(constraint) {
        constraintAdded(constraint)
        picks = []
        canvas.requestPaint()
    }

    function click(x, y) {
        const p = pickAt(x, y)
        if (p === null)
            return
        const ps = picks.concat([p])
        const curvesPicked = ps.filter(q => q.curve !== undefined).map(q => q.curve)
        const pointsPicked = ps.filter(q => q.point !== undefined).map(q => q.point)
        switch (tool) {
        case "dimension": {
            const dim = dimensionFor(ps)
            picks = ps.length >= 2 ? [] : ps
            if (dim) {
                pending = { constraint: dim.constraint, value: dim.value, at: midpointOf(ps) }
                valueField.text = String(round2(dim.value))
                valueField.forceActiveFocus()
                valueField.selectAll()
            } else if (ps.length >= 2) {
                pending = null
            }
            break
        }
        case "horizontal":
        case "vertical":
            if (p.curve !== undefined && picks.length === 0) {
                if (isLine(p.curve))
                    emit({ type: tool, curve: p.curve })
            } else if (pointsPicked.length === 2) {
                emit({ type: tool, points: pointsPicked })
            } else if (p.point !== undefined) {
                picks = ps
            }
            break
        case "fix":
            emit(p.curve !== undefined ? { type: "fix", curves: [p.curve] } : { type: "fix", points: [p.point] })
            break
        case "coincident":
            if (ps.length === 2) {
                if (pointsPicked.length === 2)
                    emit({ type: "coincident", points: pointsPicked })
                else if (pointsPicked.length === 1 && curvesPicked.length === 1)
                    emit({ type: "point_on_curve", point: pointsPicked[0], curve: curvesPicked[0] })
                else
                    picks = []
            } else {
                picks = ps
            }
            break
        case "midpoint":
            if (ps.length === 2 && pointsPicked.length === 1 && isLine(curvesPicked[0]))
                emit({ type: "midpoint", point: pointsPicked[0], curve: curvesPicked[0] })
            else
                picks = ps.length >= 2 ? [] : ps
            break
        default:
            // Two-curve constraints.
            if (curvesPicked.length === 2) {
                const [i, j] = curvesPicked
                const ok = tool === "equal" ? (isLine(i) && isLine(j)) || (isCirc(i) && isCirc(j))
                         : tool === "tangent" ? isCirc(i) || isCirc(j)
                         : tool === "concentric" ? isCirc(i) && isCirc(j)
                         : isLine(i) && isLine(j)
                if (ok && i !== j)
                    emit({ type: tool, curves: [i, j] })
                else
                    picks = []
            } else {
                picks = ps
            }
        }
        canvas.requestPaint()
    }

    function applyValue(text) {
        if (pending === null)
            return
        const t = text.trim()
        if (t.length === 0)
            return
        const c = Object.assign({}, pending.constraint)
        c.value = isNaN(Number(t)) ? t : Number(t)
        pending = null
        emit(c)
    }

    // --- Drawing --------------------------------------------------------------------------

    function anchorOfCurve(i) {
        const c = curves[i]
        if (!c)
            return [0, 0]
        if (c.kind === "line")
            return [(c.points[0][0] + c.points[1][0]) / 2, (c.points[0][1] + c.points[1][1]) / 2]
        if (c.kind === "circle")
            return [c.points[0][0] + c.radius * 0.707, c.points[0][1] + c.radius * 0.707]
        if (c.kind === "arc")
            return c.points[1]
        return c.points[Math.floor(c.points.length / 2)]
    }

    function pointPos(ref) {
        if (ref === "origin")
            return [0, 0]
        for (const c of pointCandidates()) {
            if (c.point !== "origin" && c.point.curve === ref.curve && c.point.point === (ref.point || "start"))
                return c.pos
        }
        return null
    }

    function glyph(c) {
        const v = c.resolved !== undefined ? Number(c.resolved).toFixed(2) : ""
        const expr = typeof c.value === "string" ? c.value + " = " : ""
        switch (c.type) {
        case "horizontal": return "H"
        case "vertical": return "V"
        case "parallel": return "∥"
        case "perpendicular": return "⊥"
        case "collinear": return "≡"
        case "tangent": return "T"
        case "equal": return "="
        case "concentric": return "◎"
        case "coincident":
        case "point_on_curve": return "o"
        case "midpoint": return "M"
        case "symmetric": return "⇋"
        case "fix": return "⚓"
        case "diameter": return "⌀" + expr + v
        case "radius": return "R" + expr + v
        case "angle": return "∠" + expr + v + "°"
        default: return expr + v
        }
    }

    function anchorOf(c) {
        const curvesOf = c.curves !== undefined ? c.curves : c.curve !== undefined ? [c.curve] : []
        const pointsOf = c.points !== undefined ? c.points : c.point !== undefined ? [c.point] : []
        if (c.type === "distance" && pointsOf.length === 2) {
            const a = pointPos(pointsOf[0]), b = pointPos(pointsOf[1])
            if (a && b)
                return [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2]
        }
        if (pointsOf.length > 0 && (curvesOf.length === 0 || c.type === "coincident"))
            return pointPos(pointsOf[0])
        return curvesOf.length > 0 ? anchorOfCurve(curvesOf[0]) : null
    }

    function drawCurve(ctx, c) {
        ctx.beginPath()
        if (c.kind === "circle" || c.kind === "arc") {
            const ctr = curveCenter(c), r = curveRadius(c)
            let a0 = 0, sweep = 2 * Math.PI
            if (c.kind === "arc") {
                const ang = p => Math.atan2(p[1] - ctr[1], p[0] - ctr[0])
                const norm = a => (a % (2 * Math.PI) + 2 * Math.PI) % (2 * Math.PI)
                a0 = ang(c.points[0])
                sweep = norm(ang(c.points[2]) - a0)
                if (norm(ang(c.points[1]) - a0) > sweep)
                    sweep -= 2 * Math.PI
            }
            for (let i = 0; i <= 48; ++i) {
                const a = a0 + sweep * i / 48
                const q = overlay.toScreen([ctr[0] + r * Math.cos(a), ctr[1] + r * Math.sin(a)])
                if (i === 0) ctx.moveTo(q.x, q.y); else ctx.lineTo(q.x, q.y)
            }
        } else {
            c.points.forEach((p, i) => {
                const q = overlay.toScreen(p)
                if (i === 0) ctx.moveTo(q.x, q.y); else ctx.lineTo(q.x, q.y)
            })
        }
        ctx.stroke()
    }

    function drawPick(ctx, p, color) {
        ctx.strokeStyle = color
        ctx.fillStyle = color
        if (p.curve !== undefined && curves[p.curve]) {
            ctx.lineWidth = 3
            drawCurve(ctx, curves[p.curve])
        } else if (p.pos) {
            const s = overlay.toScreen(p.pos)
            ctx.beginPath()
            ctx.arc(s.x, s.y, 5, 0, 2 * Math.PI)
            ctx.fill()
        }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            if (root.overlay.sketch === null)
                return
            // Picks of the current tool.
            if (root.tool !== "") {
                for (const p of root.picks)
                    root.drawPick(ctx, p, "#e07000")
                if (root.hoverPick)
                    root.drawPick(ctx, root.hoverPick, "#2f8f2f")
            }
            // Sketch origin.
            const o = root.overlay.toScreen([0, 0])
            ctx.strokeStyle = "#7a3db8"
            ctx.lineWidth = 1.5
            ctx.beginPath()
            ctx.moveTo(o.x - 6, o.y); ctx.lineTo(o.x + 6, o.y)
            ctx.moveTo(o.x, o.y - 6); ctx.lineTo(o.x, o.y + 6)
            ctx.stroke()
            // Constraint glyphs and dimension labels.
            ctx.font = "11px sans-serif"
            const stacked = {}
            for (const c of root.constraints) {
                const at = root.anchorOf(c)
                if (!at)
                    continue
                const s = root.overlay.toScreen(at)
                const key = Math.round(s.x / 6) + ":" + Math.round(s.y / 6)
                const k = stacked[key] || 0
                stacked[key] = k + 1
                const text = root.glyph(c)
                const w = ctx.measureText(text).width + 6
                const x = s.x + 6 + k * (w + 2)
                const y = s.y - 16
                const dim = c.value !== undefined
                ctx.fillStyle = dim ? "rgba(255,255,255,0.9)" : "rgba(30,60,120,0.85)"
                ctx.fillRect(x, y, w, 14)
                ctx.fillStyle = dim ? "#123a75" : "white"
                ctx.fillText(text, x + 3, y + 11)
            }
        }
    }

    // Value of a dimension being placed (Fusion shows it in place).
    TextField {
        id: valueField
        visible: root.pending !== null
        width: 110
        x: root.pending ? root.overlay.toScreen(root.pending.at).x + 10 : 0
        y: root.pending ? root.overlay.toScreen(root.pending.at).y + 6 : 0
        selectByMouse: true
        placeholderText: qsTr("value or w/2")
        onAccepted: { root.applyValue(text); root.overlay.forceActiveFocus() }
        Keys.onEscapePressed: { root.reset(); root.overlay.forceActiveFocus() }
    }
}
