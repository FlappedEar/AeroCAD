// SPDX-License-Identifier: GPL-3.0-or-later
// Small line chart for simulation histories: several series over a shared x axis,
// optional logarithmic y axis (residuals), legend and nice tick values. Hovering shows the
// values of every series at the nearest recorded x (CAD-177).
import QtQuick
import QtQuick.Controls
import AeroCAD

Item {
    id: chart

    property string title
    property string xLabel: qsTr("Iteration")
    /// [{name, color, x: [...], y: [...]}]; null y values are gaps.
    property var series: []
    property bool logY: false
    implicitHeight: 170

    /// What the hover shows: {x, px, values: [{name, color, value, ownX, px, py}]}, or null.
    property var readout: null

    onSeriesChanged: refresh()
    onLogYChanged: refresh()
    onWidthChanged: refresh()
    onHeightChanged: refresh()
    function refresh() {
        readout = hover.hovered ? readoutAt(hover.point.position.x) : null
        canvas.requestPaint()
    }

    function niceStep(range, count) {
        const raw = range / Math.max(1, count)
        const mag = Math.pow(10, Math.floor(Math.log10(raw)))
        const n = raw / mag
        return (n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10) * mag
    }

    /// Plot area, axis ranges and mappings for a canvas of this size; null when too small,
    /// {empty: true, ...} without data.
    function layoutPlot(width, height) {
        const left = 52, right = 18, top = 4, bottom = 22
        const w = width - left - right, h = height - top - bottom
        if (w <= 10 || h <= 10)
            return null
        const tr = v => logY ? (v > 0 ? Math.log10(v) : NaN) : v
        let xmin = Infinity, xmax = -Infinity, ymin = Infinity, ymax = -Infinity
        for (const s of series) {
            for (let i = 0; i < s.x.length; ++i) {
                const y = s.y[i]
                if (y === null || y === undefined || !isFinite(tr(y)))
                    continue
                xmin = Math.min(xmin, s.x[i]); xmax = Math.max(xmax, s.x[i])
                ymin = Math.min(ymin, tr(y)); ymax = Math.max(ymax, tr(y))
            }
        }
        if (!isFinite(xmin))
            return { empty: true, left: left, top: top, w: w, h: h }
        if (xmax === xmin) xmax = xmin + 1
        if (ymax === ymin) { ymax += Math.abs(ymax) * 0.05 + 1e-9; ymin -= Math.abs(ymin) * 0.05 + 1e-9 }
        if (logY) { ymin = Math.floor(ymin); ymax = Math.ceil(ymax) }
        return { empty: false, left: left, top: top, w: w, h: h, xmin: xmin, xmax: xmax, ymin: ymin, ymax: ymax, tr: tr,
                 px: x => left + (x - xmin) / (xmax - xmin) * w,
                 py: y => top + h - (y - ymin) / (ymax - ymin) * h }
    }

    function valueText(v) {
        if (v === null || v === undefined || !isFinite(v))
            return "–"
        return logY ? v.toExponential(2) : fmt(v)
    }

    /// The values of every series at the recorded x nearest to the pixel column px. Series may
    /// be sampled at different x or end at different x (A/B histories, CAD-179): the readout is
    /// at the nearest x of any series; a series without a point there gives its nearest point
    /// with its own x (ownX), and one whose range does not reach that x gives no value.
    function readoutAt(px) {
        const p = layoutPlot(canvas.width, canvas.height)
        if (!p || p.empty || px < p.left - 4 || px > p.left + p.w + 4)
            return null
        const xv = p.xmin + (px - p.left) / p.w * (p.xmax - p.xmin)
        const nearest = s => {
            let best = -1
            for (let i = 0; i < s.x.length; ++i)
                if (best < 0 || Math.abs(s.x[i] - xv) < Math.abs(s.x[best] - xv))
                    best = i
            return best
        }
        let at = null
        for (const s of series) {
            const i = nearest(s)
            if (i >= 0 && (at === null || Math.abs(s.x[i] - xv) < Math.abs(at - xv)))
                at = s.x[i]
        }
        if (at === null)
            return null
        const values = []
        for (const s of series) {
            const lo = s.x.length ? Math.min(s.x[0], s.x[s.x.length - 1]) : NaN
            const hi = s.x.length ? Math.max(s.x[0], s.x[s.x.length - 1]) : NaN
            if (!(at >= lo && at <= hi)) {
                values.push({ name: s.name, color: s.color, value: null, ownX: null, px: NaN, py: NaN })
                continue
            }
            let i = s.x.indexOf(at)
            if (i < 0)
                i = nearest(s)
            const y = s.y[i]
            const t = y === null || y === undefined ? NaN : p.tr(y)
            values.push({ name: s.name, color: s.color, value: y, ownX: s.x[i] === at ? null : s.x[i],
                          px: p.px(s.x[i]), py: isFinite(t) ? p.py(t) : NaN })
        }
        return { x: at, px: p.px(at), values: values }
    }

    function fmt(v) {
        const a = Math.abs(v)
        if (a !== 0 && (a < 1e-3 || a >= 1e5))
            return v.toExponential(0)
        return Number(v.toPrecision(4)).toString()
    }

    Label {
        id: titleLabel
        text: chart.title
        font.bold: true
        font.pixelSize: 12
    }

    Row {
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 10
        Repeater {
            model: chart.series
            delegate: Row {
                required property var modelData
                spacing: 4
                Rectangle { width: 12; height: 3; color: modelData.color; anchors.verticalCenter: parent.verticalCenter }
                Label { text: modelData.name; font.pixelSize: 11 }
            }
        }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        anchors.topMargin: titleLabel.height + 4

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const p = chart.layoutPlot(width, height)
            if (!p)
                return
            const left = p.left, top = p.top, w = p.w, h = p.h
            const xmin = p.xmin, xmax = p.xmax, ymin = p.ymin, ymax = p.ymax, px = p.px, py = p.py, tr = p.tr
            ctx.font = "10px sans-serif"
            ctx.fillStyle = Theme.textMuted
            ctx.strokeStyle = Theme.chartGrid
            ctx.lineWidth = 1
            if (p.empty) {
                ctx.fillText(qsTr("No data yet"), left + 8, top + h / 2)
                return
            }

            // Grid and ticks.
            const yStep = chart.logY ? Math.max(1, Math.ceil((ymax - ymin) / 5)) : chart.niceStep(ymax - ymin, 4)
            for (let v = Math.ceil(ymin / yStep) * yStep; v <= ymax + yStep * 1e-6; v += yStep) {
                const y = py(v)
                ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(left + w, y); ctx.stroke()
                ctx.fillText(chart.logY ? "1e" + Math.round(v) : chart.fmt(v), 2, y + 3)
            }
            const xStep = chart.niceStep(xmax - xmin, 5)
            for (let v = Math.ceil(xmin / xStep) * xStep; v <= xmax + xStep * 1e-6; v += xStep) {
                const x = px(v)
                ctx.beginPath(); ctx.moveTo(x, top); ctx.lineTo(x, top + h); ctx.stroke()
                ctx.fillText(chart.fmt(v), x - 8, top + h + 14)
            }

            // Series.
            ctx.lineWidth = 1.6
            for (const s of chart.series) {
                ctx.strokeStyle = s.color
                ctx.beginPath()
                let pen = false
                for (let i = 0; i < s.x.length; ++i) {
                    const y = s.y[i]
                    const t = y === null || y === undefined ? NaN : tr(y)
                    if (!isFinite(t)) { pen = false; continue }
                    if (pen) ctx.lineTo(px(s.x[i]), py(t))
                    else { ctx.moveTo(px(s.x[i]), py(t)); pen = true }
                }
                ctx.stroke()
            }

            // Hover: a line at the nearest recorded x and a dot on each series.
            const r = chart.readout
            if (r) {
                ctx.strokeStyle = Theme.textMuted
                ctx.lineWidth = 1
                ctx.beginPath(); ctx.moveTo(r.px, top); ctx.lineTo(r.px, top + h); ctx.stroke()
                for (const v of r.values) {
                    if (!isFinite(v.py))
                        continue
                    ctx.fillStyle = v.color
                    ctx.beginPath(); ctx.arc(v.px, v.py, 3, 0, 2 * Math.PI); ctx.fill()
                }
            }
        }

        HoverHandler {
            id: hover
            onPointChanged: chart.readout = hovered ? chart.readoutAt(point.position.x) : null
            onHoveredChanged: chart.readout = hovered ? chart.readoutAt(point.position.x) : null
        }
    }
    onReadoutChanged: canvas.requestPaint()

    Rectangle {
        id: readoutBox
        objectName: "chartReadout"
        visible: !!chart.readout
        readonly property real gap: 10
        // Right of the line, or left of it near the right edge; always inside the chart.
        x: !chart.readout ? 0
           : chart.readout.px + gap + width <= chart.width ? chart.readout.px + gap
           : Math.max(0, chart.readout.px - gap - width)
        y: canvas.y + 4
        width: readoutColumn.implicitWidth + 12
        height: readoutColumn.implicitHeight + 8
        radius: 4
        color: Theme.surfaceRaised
        border.color: Theme.borderStrong
        Column {
            id: readoutColumn
            x: 6
            y: 4
            spacing: 1
            Label {
                objectName: "chartReadoutTitle"
                font.pixelSize: 11
                font.bold: true
                // The exact recorded x, not rounded like the axis ticks (iteration 12345, not 12350).
                text: chart.readout ? chart.xLabel + " " + (Number.isInteger(chart.readout.x) ? chart.readout.x : Number(chart.readout.x.toPrecision(8))) : ""
            }
            Repeater {
                model: chart.readout ? chart.readout.values : []
                delegate: Label {
                    required property var modelData
                    font.pixelSize: 11
                    textFormat: Text.StyledText
                    text: "<font color=\"" + modelData.color + "\">■</font> " + modelData.name + "  " + chart.valueText(modelData.value) +
                          (modelData.ownX !== null && modelData.ownX !== undefined ? " (" + chart.xLabel.toLowerCase() + " " + modelData.ownX + ")" : "")
                }
            }
        }
    }
}
