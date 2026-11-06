// SPDX-License-Identifier: GPL-3.0-or-later
// Small line chart for simulation histories: several series over a shared x axis,
// optional logarithmic y axis (residuals), legend and nice tick values.
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

    onSeriesChanged: canvas.requestPaint()
    onLogYChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    function niceStep(range, count) {
        const raw = range / Math.max(1, count)
        const mag = Math.pow(10, Math.floor(Math.log10(raw)))
        const n = raw / mag
        return (n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10) * mag
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
            const left = 52, right = 18, top = 4, bottom = 22
            const w = width - left - right, h = height - top - bottom
            if (w <= 10 || h <= 10)
                return
            const tr = v => chart.logY ? (v > 0 ? Math.log10(v) : NaN) : v
            let xmin = Infinity, xmax = -Infinity, ymin = Infinity, ymax = -Infinity
            for (const s of chart.series) {
                for (let i = 0; i < s.x.length; ++i) {
                    const y = s.y[i]
                    if (y === null || y === undefined || !isFinite(tr(y)))
                        continue
                    xmin = Math.min(xmin, s.x[i]); xmax = Math.max(xmax, s.x[i])
                    ymin = Math.min(ymin, tr(y)); ymax = Math.max(ymax, tr(y))
                }
            }
            ctx.font = "10px sans-serif"
            ctx.fillStyle = Theme.textMuted
            ctx.strokeStyle = Theme.chartGrid
            ctx.lineWidth = 1
            if (!isFinite(xmin)) {
                ctx.fillText(qsTr("No data yet"), left + 8, top + h / 2)
                return
            }
            if (xmax === xmin) xmax = xmin + 1
            if (ymax === ymin) { ymax += Math.abs(ymax) * 0.05 + 1e-9; ymin -= Math.abs(ymin) * 0.05 + 1e-9 }
            if (chart.logY) { ymin = Math.floor(ymin); ymax = Math.ceil(ymax) }
            const px = x => left + (x - xmin) / (xmax - xmin) * w
            const py = y => top + h - (y - ymin) / (ymax - ymin) * h

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
        }
    }
}
