// SPDX-License-Identifier: GPL-3.0-or-later
// Command dialog for timeline features (Fusion's tool dialogs: Extrude, Fillet, ...).
// Fields are described by `specs`; numbers accept expressions with user parameters.
// Selections (edges, faces, profiles) are made by clicking in the viewport while the
// dialog is open: Main forwards viewport picks to addPick().
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Pane {
    id: root

    required property AppController controller
    /// Feature type being created or edited ("" when closed).
    property string type: ""
    /// Feature id when editing an existing feature, 0 when creating one.
    property var editId: 0
    property var values: ({})
    property string error: ""
    /// Key of the selection field that receives viewport picks.
    property string pickKey: ""
    property var timeline: ({ features: [], bodies: [] })

    signal finished()

    /// Picked edge / face points ([[x, y, z], ...]) to mark in the viewport.
    readonly property var pickedPoints: {
        const out = []
        if (!spec)
            return out
        for (const f of spec.fields) {
            if (f.type === "pointPick" && values[f.key])
                out.push(values[f.key])
            if ((f.type === "edgePicks" || f.type === "facePicks") && values[f.key]) {
                for (const item of values[f.key]) {
                    if (item && item.point)
                        out.push(item.point)
                }
            }
        }
        return out
    }

    visible: type !== ""
    padding: 12
    width: 300

    readonly property var operations: [
        { value: "new_body", label: qsTr("New Body") }, { value: "join", label: qsTr("Join") },
        { value: "cut", label: qsTr("Cut") }, { value: "intersect", label: qsTr("Intersect") }
    ]

    readonly property var specs: ({
        extrude: { title: qsTr("Extrude"), command: "feature.extrude", fields: [
            { key: "sketch", type: "sketch", label: qsTr("Sketch") },
            { key: "points", type: "profilePicks", label: qsTr("Profiles"), hint: qsTr("All profiles (click inside profiles to pick)") },
            { key: "distance", type: "expr", label: qsTr("Distance"), init: 10 },
            { key: "extent", type: "choice", label: qsTr("Direction"), init: "one_side", options: [
                { value: "one_side", label: qsTr("One Side") }, { value: "symmetric", label: qsTr("Symmetric") },
                { value: "two_sides", label: qsTr("Two Sides") }] },
            { key: "distance2", type: "expr", label: qsTr("Distance 2"), showIf: v => v.extent === "two_sides" },
            { key: "flip", type: "bool", label: qsTr("Flip") },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "new_body", options: operations }] },
        revolve: { title: qsTr("Revolve"), command: "feature.revolve", fields: [
            { key: "sketch", type: "sketch", label: qsTr("Sketch") },
            { key: "points", type: "profilePicks", label: qsTr("Profiles"), hint: qsTr("All profiles (click inside profiles to pick)") },
            { key: "axis", type: "choice", label: qsTr("Axis"), init: "Z", options: [
                { value: "X", label: "X" }, { value: "Y", label: "Y" }, { value: "Z", label: "Z" }] },
            { key: "angle", type: "expr", label: qsTr("Angle (deg)"), init: 360 },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "new_body", options: operations }] },
        fillet: { title: qsTr("Fillet"), command: "feature.fillet", fields: [
            { key: "body", type: "body", label: qsTr("Body") },
            { key: "edges", type: "edgePicks", label: qsTr("Edges"), hint: qsTr("Click near edges of the body") },
            { key: "radius", type: "expr", label: qsTr("Radius"), init: 1 }] },
        chamfer: { title: qsTr("Chamfer"), command: "feature.chamfer", fields: [
            { key: "body", type: "body", label: qsTr("Body") },
            { key: "edges", type: "edgePicks", label: qsTr("Edges"), hint: qsTr("Click near edges of the body") },
            { key: "distance", type: "expr", label: qsTr("Distance"), init: 1 }] },
        shell: { title: qsTr("Shell"), command: "feature.shell", fields: [
            { key: "body", type: "body", label: qsTr("Body") },
            { key: "faces", type: "facePicks", label: qsTr("Faces to remove"), hint: qsTr("Click faces (none: closed hollow)") },
            { key: "thickness", type: "expr", label: qsTr("Thickness"), init: 1 }] },
        combine: { title: qsTr("Combine"), command: "feature.combine", fields: [
            { key: "target", type: "body", label: qsTr("Target body") },
            { key: "tools", type: "bodyList", label: qsTr("Tool body") },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "join", options: operations.slice(1) },
            { key: "keepTools", type: "bool", label: qsTr("Keep tools") }] },
        hole: { title: qsTr("Hole"), command: "feature.hole", fields: [
            { key: "body", type: "body", label: qsTr("Body") },
            { key: "point", type: "pointPick", label: qsTr("Position"), hint: qsTr("Click on a planar face") },
            { key: "diameter", type: "expr", label: qsTr("Diameter"), init: 5 },
            { key: "depth", type: "expr", label: qsTr("Depth (empty: through all)") }] },
        box: { title: qsTr("Box"), command: "feature.box", fields: [
            { key: "corner", type: "vec3", label: qsTr("Corner"), init: [0, 0, 0] },
            { key: "size", type: "vec3", label: qsTr("Size"), init: [20, 20, 20] },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "new_body", options: operations }] },
        cylinder: { title: qsTr("Cylinder"), command: "feature.cylinder", fields: [
            { key: "center", type: "vec3", label: qsTr("Base centre"), init: [0, 0, 0] },
            { key: "diameter", type: "expr", label: qsTr("Diameter"), init: 20 },
            { key: "height", type: "expr", label: qsTr("Height"), init: 20 },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "new_body", options: operations }] }
    })

    readonly property var spec: specs[type] || null

    function refreshTimeline() {
        const r = controller.execute("timeline.list")
        if (r.ok)
            timeline = r.result
    }

    function sketches() {
        return timeline.features.filter(f => f.type === "sketch" && f.status === "ok")
    }

    /// Opens the dialog to create a feature (`initial` pre-fills fields) or, with
    /// `featureId`, to edit an existing one with its current params.
    function open(featureType, initial, featureId) {
        refreshTimeline()
        const s = specs[featureType]
        if (!s)
            return false
        const v = {}
        for (const f of s.fields) {
            if (f.init !== undefined)
                v[f.key] = f.init
        }
        if (initial) {
            for (const k in initial)
                v[k] = initial[k]
        }
        if (!featureId) {
            if (s.fields.some(f => f.key === "sketch") && v.sketch === undefined) {
                const sk = sketches()
                if (sk.length > 0)
                    v.sketch = sk[sk.length - 1].id
            }
            const sketch = timeline.features.find(f => f.id === v.sketch)
            if (featureType === "extrude" && sketch && sketch.params.face && !(initial && initial.operation))
                v.operation = "join" // like Fusion: extruding from a face adds to that body
        }
        values = v
        editId = featureId || 0
        error = ""
        type = featureType
        const firstPick = s.fields.find(f => f.type.endsWith("Picks") || f.type === "pointPick")
        pickKey = firstPick ? firstPick.key : ""
        return true
    }

    function close() {
        type = ""
        pickKey = ""
        finished()
    }

    function setValue(key, value) {
        const v = Object.assign({}, values)
        if (value === undefined || value === "" || value === null)
            delete v[key]
        else
            v[key] = value
        values = v
    }

    function parseExpr(text) {
        const t = String(text).trim()
        if (t === "")
            return undefined
        return isNaN(Number(t)) ? t : Number(t)
    }

    /// Viewport pick while the dialog is open: `part` is controller.partInfo().
    function addPick(part, point) {
        if (!spec || !pickKey || !part || !part.id)
            return
        const field = spec.fields.find(f => f.key === pickKey)
        if (field.type === "pointPick") {
            if (part.kind !== "body")
                return
            const bodyField = spec.fields.find(f => f.type === "body")
            if (bodyField)
                setValue(bodyField.key, part.featureId)
            setValue(pickKey, [point.x, point.y, point.z].map(x => Math.round(x * 1e4) / 1e4))
            return
        }
        const list = (values[pickKey] || []).slice()
        if (field.type === "profilePicks") {
            if (part.kind !== "sketch")
                return
            const r = controller.execute("sketch.profiles", { sketch: part.featureId })
            if (!r.ok)
                return
            const pl = r.result.plane
            const d = [point.x - pl.origin[0], point.y - pl.origin[1], point.z - pl.origin[2]]
            const uv = [d[0] * pl.xDir[0] + d[1] * pl.xDir[1] + d[2] * pl.xDir[2],
                        d[0] * pl.yDir[0] + d[1] * pl.yDir[1] + d[2] * pl.yDir[2]]
            if (values.sketch !== part.featureId) {
                setValue("sketch", part.featureId)
                list.length = 0
            }
            list.push(uv.map(x => Math.round(x * 1e4) / 1e4))
        } else {
            if (part.kind !== "body")
                return
            const bodyKey = spec.fields.find(f => f.type === "body").key
            if (values[bodyKey] !== part.featureId) {
                setValue(bodyKey, part.featureId)
                list.length = 0
            }
            list.push({ point: [point.x, point.y, point.z].map(x => Math.round(x * 1e4) / 1e4) })
        }
        setValue(pickKey, list)
    }

    function apply() {
        const params = Object.assign({}, values)
        for (const f of spec.fields) {
            if (f.showIf && !f.showIf(values))
                delete params[f.key]
            if ((f.type.endsWith("Picks")) && params[f.key] && params[f.key].length === 0)
                delete params[f.key]
        }
        let r
        if (editId) {
            // Merge patch: fields that are no longer set must be removed explicitly.
            const feature = timeline.features.find(f => f.id === editId)
            const patch = Object.assign({}, params)
            if (feature) {
                for (const k in feature.params) {
                    if (!(k in params) && spec.fields.some(f => f.key === k))
                        patch[k] = null
                }
            }
            r = controller.execute("feature.edit", { id: editId, params: patch })
        } else {
            r = controller.execute(spec.command, params)
        }
        if (r.ok)
            close()
        else
            error = r.error
    }

    Keys.onEscapePressed: close()

    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 6

        Label {
            text: root.spec ? (root.editId ? qsTr("Edit %1").arg(root.spec.title) : root.spec.title) : ""
            font.bold: true
            font.pixelSize: 15
        }

        Repeater {
            model: root.spec ? root.spec.fields : []
            delegate: ColumnLayout {
                id: fieldItem
                required property var modelData
                readonly property var field: modelData
                Layout.fillWidth: true
                visible: !field.showIf || field.showIf(root.values)
                spacing: 2

                Label { text: fieldItem.field.label; opacity: 0.75 }

                Loader {
                    Layout.fillWidth: true
                    sourceComponent: {
                        switch (fieldItem.field.type) {
                        case "expr": return exprEditor
                        case "choice": return choiceEditor
                        case "bool": return boolEditor
                        case "sketch": return sketchEditor
                        case "body": return bodyEditor
                        case "bodyList": return bodyListEditor
                        case "vec3": return vec3Editor
                        default: return pickEditor
                        }
                    }
                    property var field: fieldItem.field
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.error !== ""
            text: root.error
            color: "#b3261e"
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Button { text: qsTr("Cancel"); onClicked: root.close() }
            Button { text: qsTr("OK"); highlighted: true; onClicked: root.apply() }
        }
    }

    Component {
        id: exprEditor
        TextField {
            property var field: parent ? parent.field : ({})
            text: root.values[field.key] !== undefined ? String(root.values[field.key]) : ""
            placeholderText: qsTr("number or expression")
            selectByMouse: true
            onEditingFinished: root.setValue(field.key, root.parseExpr(text))
            onAccepted: { root.setValue(field.key, root.parseExpr(text)); root.apply() }
        }
    }
    Component {
        id: choiceEditor
        ComboBox {
            property var field: parent ? parent.field : ({})
            model: field.options
            textRole: "label"
            valueRole: "value"
            currentIndex: Math.max(0, field.options.findIndex(o => o.value === root.values[field.key]))
            onActivated: root.setValue(field.key, currentValue)
        }
    }
    Component {
        id: boolEditor
        CheckBox {
            property var field: parent ? parent.field : ({})
            checked: !!root.values[field.key]
            onToggled: root.setValue(field.key, checked ? true : undefined)
        }
    }
    Component {
        id: sketchEditor
        ComboBox {
            property var field: parent ? parent.field : ({})
            readonly property var items: root.sketches()
            model: items
            textRole: "name"
            valueRole: "id"
            currentIndex: items.findIndex(s => s.id === root.values[field.key])
            onActivated: { root.setValue(field.key, currentValue); root.setValue("points", undefined) }
        }
    }
    Component {
        id: bodyEditor
        ComboBox {
            property var field: parent ? parent.field : ({})
            model: root.timeline.bodies
            textRole: "name"
            valueRole: "id"
            currentIndex: root.timeline.bodies.findIndex(b => b.id === root.values[field.key])
            onActivated: root.setValue(field.key, currentValue)
        }
    }
    Component {
        id: bodyListEditor
        ComboBox {
            property var field: parent ? parent.field : ({})
            model: root.timeline.bodies
            textRole: "name"
            valueRole: "id"
            currentIndex: root.values[field.key] ? root.timeline.bodies.findIndex(b => b.id === root.values[field.key][0]) : -1
            onActivated: root.setValue(field.key, [currentValue])
        }
    }
    Component {
        id: vec3Editor
        RowLayout {
            property var field: parent ? parent.field : ({})
            Repeater {
                model: 3
                TextField {
                    required property int index
                    Layout.fillWidth: true
                    text: root.values[field.key] ? String(root.values[field.key][index]) : "0"
                    selectByMouse: true
                    onEditingFinished: {
                        const v = (root.values[field.key] || [0, 0, 0]).slice()
                        const parsed = root.parseExpr(text)
                        v[index] = parsed === undefined ? 0 : parsed
                        root.setValue(field.key, v)
                    }
                }
            }
        }
    }
    Component {
        id: pickEditor
        RowLayout {
            property var field: parent ? parent.field : ({})
            readonly property var list: field.type === "pointPick" ? (root.values[field.key] ? [root.values[field.key]] : [])
                                                                   : (root.values[field.key] || [])
            Button {
                Layout.fillWidth: true
                checkable: true
                checked: root.pickKey === field.key
                text: list.length > 0 ? qsTr("%n selected", "", list.length) : field.hint
                onClicked: root.pickKey = field.key
            }
            ToolButton {
                text: "✕"
                visible: list.length > 0
                onClicked: root.setValue(field.key, undefined)
            }
        }
    }
}
