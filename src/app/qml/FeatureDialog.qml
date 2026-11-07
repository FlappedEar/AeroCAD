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
    padding: Theme.space3
    width: 300
    background: Rectangle {
        color: Theme.overlay
        border.color: Theme.border
        radius: Theme.radius
    }

    readonly property var operations: [
        { value: "new_body", label: qsTr("New Body") }, { value: "join", label: qsTr("Join") },
        { value: "cut", label: qsTr("Cut") }, { value: "intersect", label: qsTr("Intersect") }
    ]

    readonly property var patternOperations: [
        { value: "join", label: qsTr("Join") }, { value: "new_body", label: qsTr("New Body") }
    ]
    readonly property var axes: [ { value: "X", label: "X" }, { value: "Y", label: "Y" }, { value: "Z", label: "Z" } ]
    readonly property var planes: [ { value: "YZ", label: "YZ" }, { value: "XZ", label: "XZ" }, { value: "XY", label: "XY" } ]

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
            { key: "type", type: "choice", label: qsTr("Chamfer type"), init: "equal_distance", options: [
                { value: "equal_distance", label: qsTr("Equal distance") },
                { value: "two_distances", label: qsTr("Two distances") },
                { value: "distance_angle", label: qsTr("Distance and angle") }] },
            { key: "distance", type: "expr", label: qsTr("Distance"), init: 1 },
            { key: "distance2", type: "expr", label: qsTr("Distance 2"), init: 1, showIf: v => v.type === "two_distances" },
            { key: "angle", type: "expr", label: qsTr("Angle (deg)"), init: 45, showIf: v => v.type === "distance_angle" },
            { key: "flip", type: "bool", label: qsTr("Flip"), showIf: v => v.type !== undefined && v.type !== "equal_distance" }] },
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
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "new_body", options: operations }] },
        loft: { title: qsTr("Loft"), command: "feature.loft", fields: [
            { key: "sections", type: "sketchList", label: qsTr("Profiles (in order)") },
            { key: "ruled", type: "bool", label: qsTr("Straight between profiles") },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "new_body", options: operations }] },
        sweep: { title: qsTr("Sweep"), command: "feature.sweep", fields: [
            { key: "sketch", type: "sketch", label: qsTr("Profile sketch") },
            { key: "points", type: "profilePicks", label: qsTr("Profiles"), hint: qsTr("All profiles (click inside profiles to pick)") },
            { key: "path", type: "pathSketch", label: qsTr("Path sketch") },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "new_body", options: operations }] },
        plane: { title: qsTr("Construction Plane"), command: "feature.plane", fields: [
            { key: "base", type: "planePick", label: qsTr("Base plane"), init: "XY", hint: qsTr("Origin plane, construction plane, or click a planar face") },
            { key: "offset", type: "expr", label: qsTr("Offset"), init: 10 },
            { key: "angle", type: "expr", label: qsTr("Angle (deg)"), init: 0 },
            { key: "axis", type: "choice", label: qsTr("Rotation axis"), init: "x", options: [
                { value: "x", label: qsTr("Plane X axis") }, { value: "y", label: qsTr("Plane Y axis") }],
              showIf: v => Number(v.angle) !== 0 }] },
        mirror: { title: qsTr("Mirror"), command: "feature.mirror", fields: [
            { key: "body", type: "body", label: qsTr("Body") },
            { key: "plane", type: "choice", label: qsTr("Mirror plane"), init: "YZ", options: planes },
            { key: "offset", type: "expr", label: qsTr("Plane offset"), init: 0 },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "join", options: patternOperations }] },
        rectangular_pattern: { title: qsTr("Rectangular Pattern"), command: "feature.rectangular_pattern", fields: [
            { key: "body", type: "body", label: qsTr("Body") },
            { key: "direction", type: "choice", label: qsTr("Direction"), init: "X", options: axes },
            { key: "count", type: "expr", label: qsTr("Quantity"), init: 3 },
            { key: "spacing", type: "expr", label: qsTr("Spacing"), init: 30 },
            { key: "direction2", type: "choice", label: qsTr("Direction 2"), init: "Y", options: axes },
            { key: "count2", type: "expr", label: qsTr("Quantity 2"), init: 1 },
            { key: "spacing2", type: "expr", label: qsTr("Spacing 2"), init: 30, showIf: v => Number(v.count2) !== 1 },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "join", options: patternOperations }] },
        circular_pattern: { title: qsTr("Circular Pattern"), command: "feature.circular_pattern", fields: [
            { key: "body", type: "body", label: qsTr("Body") },
            { key: "axis", type: "choice", label: qsTr("Axis"), init: "Z", options: axes },
            { key: "count", type: "expr", label: qsTr("Quantity"), init: 6 },
            { key: "angle", type: "expr", label: qsTr("Total angle (deg)"), init: 360 },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "join", options: patternOperations }] }
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
        if (featureType === "cylinder" && v.radius !== undefined) {
            // The dialog edits the diameter; a radius given through the API is converted
            // (and removed on apply) so that it does not compete with the default diameter.
            if (!(initial && initial.diameter !== undefined))
                v.diameter = typeof v.radius === "number" ? v.radius * 2 : "(" + v.radius + ") * 2"
            delete v.radius
        }
        if (!featureId) {
            if (s.fields.some(f => f.key === "sketch") && v.sketch === undefined) {
                const sk = sketches()
                if (sk.length > 0)
                    v.sketch = sk[sk.length - 1].id
            }
            const bodies = timeline.bodies || []
            for (const f of s.fields) {
                if (f.type === "body" && v[f.key] === undefined && bodies.length > 0)
                    v[f.key] = bodies[bodies.length - 1].id
            }
            const sketch = timeline.features.find(f => f.id === v.sketch)
            if (featureType === "extrude" && sketch && sketch.params.face && !(initial && initial.operation))
                v.operation = "join" // like Fusion: extruding from a face adds to that body
        }
        values = v
        editId = featureId || 0
        error = ""
        type = featureType
        const firstPick = s.fields.find(f => f.type.endsWith("Picks") || f.type === "pointPick" || f.type === "planePick")
        pickKey = firstPick ? firstPick.key : ""
        previewTimer.restart()
        return true
    }

    function close() {
        previewTimer.stop()
        undoPreview()
        // Cancel: the solids made editable for this dialog go back to what they were.
        for (; conversions > 0; --conversions)
            controller.execute("edit.undo")
        type = ""
        pickKey = ""
        finished()
    }

    // --- Live preview -------------------------------------------------------------------
    // Like Fusion, the model shows the result while the dialog is open: every change runs
    // the command, and the previous preview is undone first. OK keeps one undo step;
    // Cancel undoes the preview.

    property bool previewApplied: false
    property var previewFeatureId: -1
    /// The plane being created or edited must not catch the clicks meant for faces behind it.
    readonly property var ownPlane: type !== "plane" ? -1 : editId ? editId : previewFeatureId

    /// What viewport clicks should hit while the dialog waits for picks: profiles live on
    /// sketches, edges/faces/points on bodies (the preview body would hide the sketch).
    readonly property string pickKind: {
        const f = spec && pickKey ? spec.fields.find(x => x.key === pickKey) : null
        if (!f)
            return ""
        if (f.type === "planePick")
            return "" // faces of bodies and construction planes
        // "solid": bodies and exact solids outside the timeline, made editable when picked.
        return f.type === "profilePicks" ? "sketch" : "solid"
    }
    /// The sketch the feature uses stays visible while the dialog is open.
    readonly property var shownSketch: type !== "" && values.sketch !== undefined ? values.sketch : -1

    // The controller keeps previews out of the undo history (redo entries survive), and
    // any other document change (Undo, Delete, ...) ends the preview first.
    function undoPreview() {
        if (previewApplied) {
            controller.cancelPreview()
            previewApplied = false
        }
    }

    function runPreview() {
        if (!spec)
            return
        undoPreview()
        const r = submit(true)
        previewApplied = r.ok
        previewFeatureId = r.ok && r.result ? r.result.id : -1
        // Incomplete input (no edges picked yet, ...) is normal while editing: those errors
        // are shown when OK is pressed. A complete feature that fails (a fillet radius that
        // does not fit) is reported right away, like Fusion's dialog warning.
        if (r.ok)
            error = ""
        else if (/does not fit|failed/.test(r.error || ""))
            error = r.error
    }

    onValuesChanged: if (type !== "") previewTimer.restart()

    Timer {
        id: previewTimer
        interval: 250
        onTriggered: root.runPreview()
    }

    // Another change (Undo, Delete, a parameter edit) ended the preview: show it again on
    // top of the new state.
    Connections {
        target: root.controller
        function onHistoryChanged() {
            if (root.previewApplied && !root.controller.previewActive()) {
                root.previewApplied = false
                if (root.type !== "")
                    previewTimer.restart()
            }
        }
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

    /// The timeline body for a picked part: bodies as they are; an exact solid that is not
    /// in the timeline yet (a fin, a fitted solid, a STEP import) is made editable first,
    /// so fillets and cuts work on it in one step. null for anything else.
    function asBody(part) {
        if (part.kind === "body")
            return part
        if (part.kind === "sketch" || part.kind === "plane" || !part.hasBrep || !((part.solids ?? 0) > 0))
            return null
        const b = convertToBody(part.id)
        if (!b)
            error = qsTr("Could not make the solid editable")
        return b
    }

    /// Conversions made for this dialog (feature.base_body runs): Cancel undoes them, OK
    /// joins them with the feature into one undo step.
    property int conversions: 0

    /// Makes imported part `partId` a timeline body (feature.base_body) as part of this
    /// dialog. Returns { id, kind: "body", featureId } of the new body, or null.
    function convertToBody(partId) {
        undoPreview()
        const r = controller.execute("feature.base_body", { part: partId })
        const body = r.ok && r.result.bodies ? r.result.bodies.find(b => b.id === r.result.id) : null
        if (!body)
            return null
        conversions += 1
        previewTimer.restart()
        return { id: body.partId, kind: "body", featureId: body.id }
    }

    /// Viewport pick while the dialog is open: `part` is controller.partInfo().
    function addPick(part, point) {
        if (!spec || !pickKey || !part || !part.id)
            return
        const field = spec.fields.find(f => f.key === pickKey)
        if (field.type === "planePick") {
            if (part.kind === "plane")
                setValue(pickKey, { plane: part.featureId })
            else if ((part = asBody(part)))
                setValue(pickKey, { body: part.featureId, point: [point.x, point.y, point.z].map(x => Math.round(x * 1e4) / 1e4) })
            return
        }
        if (field.type === "pointPick") {
            if (!(part = asBody(part)))
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
            if (!(part = asBody(part)))
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

    /// Runs the feature command (create, or edit of `editId`) with the current values.
    function submit(quiet) {
        const run = (name, args) => quiet ? controller.preview(name, args) : controller.execute(name, args)
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
                if (type === "cylinder" && feature.params.radius !== undefined)
                    patch.radius = null
            }
            r = run("feature.edit", { id: editId, params: patch })
        } else {
            r = run(spec.command, params)
        }
        return r
    }

    function apply() {
        previewTimer.stop()
        undoPreview()
        const r = submit()
        if (r.ok) {
            if (conversions > 0)
                controller.mergeHistory(conversions + 1, spec.title)
            conversions = 0
            close()
        } else {
            error = r.error
        }
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
                        case "sketchList": return sketchListEditor
                        case "pathSketch": return pathSketchEditor
                        case "planePick": return planeEditor
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
            color: Theme.danger
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
        id: sketchListEditor
        // Loft sections: tick sketches in the order the loft should pass through them.
        ColumnLayout {
            property var field: parent ? parent.field : ({})
            spacing: 0
            Repeater {
                model: root.sketches()
                CheckBox {
                    required property var modelData
                    readonly property var picked: root.values[field.key] || []
                    readonly property int order: picked.indexOf(modelData.id)
                    text: modelData.name + (order >= 0 ? "  (" + (order + 1) + ")" : "")
                    checked: order >= 0
                    onToggled: {
                        const next = picked.filter(id => id !== modelData.id)
                        if (checked)
                            next.push(modelData.id)
                        root.setValue(field.key, next.length > 0 ? next : undefined)
                    }
                }
            }
        }
    }
    Component {
        id: planeEditor
        // Origin planes and construction planes; a face clicked in the view shows as an extra entry.
        ComboBox {
            property var field: parent ? parent.field : ({})
            readonly property var current: root.values[field.key]
            readonly property var items: {
                const out = [{ name: "XY", value: "XY" }, { name: "XZ", value: "XZ" }, { name: "YZ", value: "YZ" }]
                for (const f of root.timeline.features) {
                    if (f.type === "plane" && f.status === "ok" && f.id !== root.editId)
                        out.push({ name: f.name, value: { plane: f.id } })
                }
                if (current && typeof current === "object" && current.body !== undefined) {
                    const b = root.timeline.bodies.find(x => x.id === current.body)
                    out.push({ name: qsTr("Face of %1").arg(b ? b.name : current.body), value: current })
                }
                return out
            }
            model: items.map(i => i.name)
            currentIndex: items.findIndex(i => JSON.stringify(i.value) === JSON.stringify(current))
            onActivated: index => root.setValue(field.key, items[index].value)
        }
    }
    Component {
        id: pathSketchEditor
        ComboBox {
            property var field: parent ? parent.field : ({})
            readonly property var items: root.sketches()
            readonly property var current: root.values[field.key]
            model: items
            textRole: "name"
            valueRole: "id"
            currentIndex: current ? items.findIndex(s => s.id === current.sketch) : -1
            onActivated: root.setValue(field.key, { sketch: currentValue })
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
