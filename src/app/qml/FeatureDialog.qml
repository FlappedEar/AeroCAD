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

    /// Extrude: the distance arrow on the profile, along the sketch normal (Viewport
    /// distanceArrow), or null. The distance is signed: negative means Flip.
    readonly property var distanceArrow: {
        if (type === "fillet" || type === "chamfer" || type === "shell" || type === "press_pull")
            return sizeArrow()
        if (type !== "extrude" || values.sketch === undefined || values.extent === "to_object")
            return null
        const d = Number(values.distance)
        if (!isFinite(d))
            return null // an expression: typed, not dragged
        const r = controller.execute("sketch.profiles", { sketch: values.sketch })
        if (!r.ok || r.result.profiles.length === 0)
            return null
        const pl = r.result.plane
        let base
        const picks = values.points || []
        if (picks.length > 0) {
            const uv = picks[0]
            base = [0, 1, 2].map(i => pl.origin[i] + pl.xDir[i] * uv[0] + pl.yDir[i] * uv[1])
        } else {
            // Area-weighted centre of all profiles.
            let a = 0
            base = [0, 0, 0]
            for (const p of r.result.profiles) {
                a += p.area
                for (let i = 0; i < 3; ++i)
                    base[i] += p.centroidWorld[i] * p.area
            }
            base = base.map(x => a > 0 ? x / a : 0)
        }
        return { base: base, direction: pl.normal, distance: values.flip ? -d : d }
    }

    /// The value the size arrow of Fillet / Chamfer / Shell drags.
    readonly property string sizeKey: type === "fillet" ? "radius" : type === "shell" ? "thickness" : type === "press_pull" ? "distance"
                                     : type === "chamfer" && (values.type === undefined || values.type === "equal_distance") ? "distance" : ""

    /// Fillet / Chamfer: an arrow on the first picked edge, into the corner between its faces;
    /// Shell: on the first removed face, into the body (Fusion's drag handles). Null without
    /// a pick or when the size is an expression.
    function sizeArrow() {
        if (sizeKey === "" || values.body === undefined)
            return null
        const size = Number(values[sizeKey])
        if (!isFinite(size))
            return null
        const pick = ((type === "shell" || type === "press_pull" ? values.faces : values.edges) || [])[0]
        if (!pick || !pick.point)
            return null
        // Where the arrow sits is found once per pick, on the body without the preview (with
        // it, the body already has the fillet or shell and its edges are not the picked ones).
        // The values just changed, so the preview is run again right after.
        const key = JSON.stringify([type, values.body, pick.point])
        if (sizeArrowPlace.key !== key) {
            sizeArrowPlace.key = key
            undoPreview()
            sizeArrowPlace.place = sizeArrowAt(pick.point)
        }
        const place = sizeArrowPlace.place
        return place ? { base: place.base, direction: place.direction, distance: size } : null
    }
    /// Cache of sizeArrow(): {key, place}. Mutated in place, not a binding dependency.
    readonly property var sizeArrowPlace: ({ key: "", place: null })

    /// Params of body.edges / body.faces for the picked body: when editing a feature, the
    /// body as it was before it (the picks were made there; Fusion rolls back the same way).
    function bodyQuery(extra) {
        const q = Object.assign({ body: values.body }, extra)
        if (editId)
            q.before = editId
        return q
    }

    /// {base, direction} of the size arrow for the pick at `point`, or null.
    function sizeArrowAt(point) {
        const v3 = a => Qt.vector3d(a[0], a[1], a[2])
        const arr = q => [q.x, q.y, q.z]
        if (type === "shell" || type === "press_pull") {
            // The face nearest the pick, with its normal at the pick (curved faces too): into
            // the body for Shell, out of it for Press Pull (its distance is signed).
            const r = controller.execute("body.faces", bodyQuery({ point: point }))
            if (!r.ok || !r.result.nearest || !r.result.nearest.normal)
                return null
            const n = v3(r.result.nearest.normal).normalized()
            return { base: point, direction: arr(type === "shell" ? n.times(-1) : n) }
        }
        const r = controller.execute("body.edges", bodyQuery({ point: point }))
        if (!r.ok || !r.result.nearest)
            return null
        // By its index: degenerate edges (a cone's apex) are not listed, so it is not the position.
        const edge = r.result.edges.find(e => e.index === r.result.nearest.index)
        if (!edge)
            return null
        const mid = v3(edge.middle)
        const along = v3(edge.end).minus(v3(edge.start))
        const dirAlong = along.length() > 1e-9 ? along.normalized() : Qt.vector3d(0, 0, 0)
        // The two faces whose planes hold the edge (nearest first): the arrow goes against
        // the sum of their normals, into the corner.
        const fr = controller.execute("body.faces", bodyQuery({}))
        const faces = fr.ok ? fr.result.faces : []
        const scale = Math.max(1e-3, along.length())
        const holding = faces.filter(f => {
            const n = v3(f.normal)
            return Math.abs(n.dotProduct(mid.minus(v3(f.center)))) < 1e-4 * Math.max(1, scale) && Math.abs(n.dotProduct(dirAlong)) < 1e-4
        }).sort((a, b) => v3(a.center).minus(mid).length() - v3(b.center).minus(mid).length())
        let dir = null
        if (holding.length >= 2) {
            const s = v3(holding[0].normal).plus(v3(holding[1].normal))
            if (s.length() > 1e-6)
                dir = s.times(-1).normalized()
        }
        if (!dir) {
            // Towards the middle of the body.
            let c = Qt.vector3d(0, 0, 0), a = 0
            for (const f of faces) { c = c.plus(v3(f.center).times(f.area)); a += f.area }
            const toC = a > 0 ? c.times(1 / a).minus(mid) : Qt.vector3d(0, 0, 1)
            dir = toC.minus(dirAlong.times(toC.dotProduct(dirAlong)))
            dir = dir.length() > 1e-9 ? dir.normalized() : Qt.vector3d(0, 0, 1)
        }
        return { base: edge.middle, direction: arr(dir) }
    }

    /// The distance arrow was dragged to signed distance `t` (mm): set Distance and Flip.
    function dragDistance(t) {
        if (type === "press_pull") {
            // Signed: out of the body or into it; never zero (Press Pull would refuse it).
            const d = Math.round(t * 1e4) / 1e4
            const v = Object.assign({}, values)
            v.distance = Math.abs(d) < 0.01 ? (t < 0 ? -0.01 : 0.01) : d
            values = v
            return
        }
        if (sizeKey !== "") {
            // A size is never negative or zero: the smallest drag step stays.
            const v = Object.assign({}, values)
            v[sizeKey] = Math.max(0.01, Math.round(Math.abs(t) * 1e4) / 1e4)
            values = v
            return
        }
        if (type !== "extrude")
            return
        const v = Object.assign({}, values)
        v.distance = Math.round(Math.abs(t) * 1e4) / 1e4
        if (t < 0)
            v.flip = true
        else
            delete v.flip
        values = v
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
            { key: "distance", type: "expr", label: qsTr("Distance"), init: 10, showIf: v => v.extent !== "to_object" },
            { key: "extent", type: "choice", label: qsTr("Extent"), init: "one_side", options: [
                { value: "one_side", label: qsTr("One Side") }, { value: "symmetric", label: qsTr("Symmetric") },
                { value: "two_sides", label: qsTr("Two Sides") }, { value: "to_object", label: qsTr("To Object") }] },
            { key: "to", type: "objectPick", label: qsTr("Object"), hint: qsTr("Click the face, body, surface or scan to extrude up to"),
              showIf: v => v.extent === "to_object" },
            { key: "distance2", type: "expr", label: qsTr("Distance 2"), showIf: v => v.extent === "two_sides" },
            { key: "taper", type: "expr", label: qsTr("Taper Angle (deg)"), init: 0, showIf: v => v.extent !== "to_object" },
            { key: "start_offset", type: "expr", label: qsTr("Start Offset"), init: 0 },
            { key: "flip", type: "bool", label: qsTr("Flip") },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "new_body", options: operations }] },
        revolve: { title: qsTr("Revolve"), command: "feature.revolve", fields: [
            { key: "sketch", type: "sketch", label: qsTr("Sketch") },
            { key: "points", type: "profilePicks", label: qsTr("Profiles"), hint: qsTr("All profiles (click inside profiles to pick)") },
            { key: "axis", type: "axisPick", label: qsTr("Axis"), init: "Z", hint: qsTr("Click a sketch line or a straight edge") },
            { key: "extent", type: "choice", label: qsTr("Direction"), init: "one_side", options: [
                { value: "one_side", label: qsTr("One Side") }, { value: "symmetric", label: qsTr("Symmetric") },
                { value: "two_sides", label: qsTr("Two Sides") }] },
            { key: "angle", type: "expr", label: qsTr("Angle (deg)"), init: 360 },
            { key: "angle2", type: "expr", label: qsTr("Angle 2 (deg)"), init: 90, showIf: v => v.extent === "two_sides" },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "new_body", options: operations }] },
        fillet: { title: qsTr("Fillet"), command: "feature.fillet", fields: [
            { key: "body", type: "body", label: qsTr("Body") },
            { key: "edges", type: "edgePicks", label: qsTr("Edges"), hint: qsTr("Click near edges of the body"), required: true },
            { key: "radius", type: "expr", label: qsTr("Radius"), init: 1 }] },
        chamfer: { title: qsTr("Chamfer"), command: "feature.chamfer", fields: [
            { key: "body", type: "body", label: qsTr("Body") },
            { key: "edges", type: "edgePicks", label: qsTr("Edges"), hint: qsTr("Click near edges of the body"), required: true },
            { key: "type", type: "choice", label: qsTr("Chamfer type"), init: "equal_distance", options: [
                { value: "equal_distance", label: qsTr("Equal distance") },
                { value: "two_distances", label: qsTr("Two distances") },
                { value: "distance_angle", label: qsTr("Distance and angle") }] },
            { key: "distance", type: "expr", label: qsTr("Distance"), init: 1 },
            { key: "distance2", type: "expr", label: qsTr("Distance 2"), init: 1, showIf: v => v.type === "two_distances" },
            { key: "angle", type: "expr", label: qsTr("Angle (deg)"), init: 45, showIf: v => v.type === "distance_angle" },
            { key: "flip", type: "bool", label: qsTr("Flip"), showIf: v => v.type !== undefined && v.type !== "equal_distance" }] },
        press_pull: { title: qsTr("Press Pull"), command: "feature.press_pull", fields: [
            { key: "body", type: "body", label: qsTr("Body") },
            { key: "faces", type: "facePicks", label: qsTr("Faces"), hint: qsTr("Click faces to move"), required: true },
            { key: "distance", type: "expr", label: qsTr("Distance (negative: push in)"), init: 5 }] },
        draft: { title: qsTr("Draft"), command: "feature.draft", fields: [
            { key: "body", type: "body", label: qsTr("Body") },
            { key: "faces", type: "facePicks", label: qsTr("Faces to tilt"), hint: qsTr("Click the side faces"), required: true },
            { key: "plane", type: "planePick", label: qsTr("Fixed plane"), init: "XY", hint: qsTr("Origin plane, construction plane, or click a planar face") },
            { key: "angle", type: "expr", label: qsTr("Angle (deg, negative: outwards)"), init: 3 }] },
        delete_faces: { title: qsTr("Delete Face"), command: "feature.delete_faces", fields: [
            { key: "body", type: "body", label: qsTr("Body") },
            { key: "faces", type: "facePicks", label: qsTr("Faces to delete"), hint: qsTr("Click every face of the hole, fillet or boss"), required: true }] },
        shell: { title: qsTr("Shell"), command: "feature.shell", fields: [
            { key: "body", type: "body", label: qsTr("Body") },
            { key: "faces", type: "facePicks", label: qsTr("Faces to remove"), hint: qsTr("Click faces (none: closed hollow)") },
            { key: "thickness", type: "expr", label: qsTr("Thickness"), init: 1 }] },
        combine: { title: qsTr("Combine"), command: "feature.combine", fields: [
            { key: "target", type: "targetPick", label: qsTr("Target body"), hint: qsTr("Click the body to change") },
            { key: "tools", type: "toolPicks", label: qsTr("Tool bodies"), hint: qsTr("Click bodies, a surface or the scan") },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "join", options: operations.slice(1) },
            { key: "flip", type: "bool", label: qsTr("Flip side"), showIf: v => (v.tools || []).some(t => root.isSurfaceTool(t)) },
            { key: "keepTools", type: "bool", label: qsTr("Keep tools"), showIf: v => (v.tools || []).some(t => typeof t !== "object") }] },
        hole: { title: qsTr("Hole"), command: "feature.hole", fields: [
            { key: "body", type: "body", label: qsTr("Body") },
            // Fusion: Placement. "At sketch points" drills one hole at every point of the sketch.
            { key: "placement", type: "choice", label: qsTr("Placement"), init: "face", local: true,
              options: [{ value: "face", label: qsTr("Click on face") }, { value: "sketch", label: qsTr("At sketch points") }] },
            { key: "point", type: "pointPick", label: qsTr("Position"), hint: qsTr("Click on a planar face"),
              showIf: v => v.placement !== "sketch" },
            { key: "sketch", type: "sketch", label: qsTr("Sketch with points"), showIf: v => v.placement === "sketch" },
            { key: "diameter", type: "expr", label: qsTr("Diameter"), init: 5 },
            { key: "depth", type: "expr", label: qsTr("Depth (empty: through all)") },
            { key: "flip", type: "bool", label: qsTr("Flip direction"), showIf: v => v.placement === "sketch" }] },
        box: { title: qsTr("Box"), command: "feature.box", fields: [
            { key: "corner", type: "vec3", label: qsTr("Corner"), init: [0, 0, 0] },
            { key: "size", type: "vec3", label: qsTr("Size"), init: [20, 20, 20] },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "new_body", options: operations }] },
        cylinder: { title: qsTr("Cylinder"), command: "feature.cylinder", fields: [
            { key: "center", type: "vec3", label: qsTr("Base centre"), init: [0, 0, 0] },
            { key: "diameter", type: "expr", label: qsTr("Diameter"), init: 20 },
            { key: "height", type: "expr", label: qsTr("Height"), init: 20 },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "new_body", options: operations }] },
        sphere: { title: qsTr("Sphere"), command: "feature.sphere", fields: [
            { key: "center", type: "vec3", label: qsTr("Centre"), init: [0, 0, 0] },
            { key: "diameter", type: "expr", label: qsTr("Diameter"), init: 20 },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "new_body", options: operations }] },
        torus: { title: qsTr("Torus"), command: "feature.torus", fields: [
            { key: "center", type: "vec3", label: qsTr("Centre"), init: [0, 0, 0] },
            { key: "diameter", type: "expr", label: qsTr("Ring diameter"), init: 40 },
            { key: "tube_diameter", type: "expr", label: qsTr("Tube diameter"), init: 10 },
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
        midplane: { title: qsTr("Midplane"), command: "feature.plane", fields: [
            { key: "base", type: "planePick", label: qsTr("First plane or face"), hint: qsTr("Click a planar face, or choose a plane"), required: true },
            { key: "base2", type: "planePick", label: qsTr("Second plane or face"), hint: qsTr("Click the opposite face, or choose a plane"), required: true }] },
        plane: { title: qsTr("Construction Plane"), command: "feature.plane", fields: [
            { key: "base", type: "planePick", label: qsTr("Base plane"), init: "XY", hint: qsTr("Origin plane, construction plane, or click a planar face") },
            { key: "offset", type: "expr", label: qsTr("Offset"), init: 10 },
            { key: "angle", type: "expr", label: qsTr("Angle (deg)"), init: 0 },
            { key: "axis", type: "choice", label: qsTr("Rotation axis"), init: "x", options: [
                { value: "x", label: qsTr("Plane X axis") }, { value: "y", label: qsTr("Plane Y axis") }],
              showIf: v => Number(v.angle) !== 0 }] },
        move: { title: qsTr("Move/Rotate"), command: "feature.move", fields: [
            { key: "body", type: "body", label: qsTr("Body") },
            { key: "axis", type: "choice", label: qsTr("Rotate about"), init: "Z", options: axes },
            { key: "angle", type: "expr", label: qsTr("Angle (deg)"), init: 90 },
            { key: "translate", type: "vec3", label: qsTr("Move by"), init: [0, 0, 0] },
            { key: "scale", type: "expr", label: qsTr("Scale (factor)"), init: 1 }] },
        scale: { title: qsTr("Scale"), command: "feature.scale", fields: [
            { key: "body", type: "body", label: qsTr("Body") },
            { key: "scaleType", type: "choice", label: qsTr("Type"), init: "uniform", local: true, options: [
                { value: "uniform", label: qsTr("Uniform") }, { value: "non_uniform", label: qsTr("Non-Uniform") }] },
            { key: "factor", type: "expr", label: qsTr("Scale factor"), init: 1, showIf: v => v.scaleType !== "non_uniform" },
            { key: "factor_x", type: "expr", label: qsTr("X factor"), init: 1, showIf: v => v.scaleType === "non_uniform" },
            { key: "factor_y", type: "expr", label: qsTr("Y factor"), init: 1, showIf: v => v.scaleType === "non_uniform" },
            { key: "factor_z", type: "expr", label: qsTr("Z factor"), init: 1, showIf: v => v.scaleType === "non_uniform" }] },
        split: { title: qsTr("Split Body"), command: "feature.split", fields: [
            { key: "body", type: "body", label: qsTr("Body to split") },
            { key: "plane", type: "planePick", label: qsTr("Splitting plane"), init: "YZ", hint: qsTr("Origin plane, construction plane, or click a planar face") },
            { key: "offset", type: "expr", label: qsTr("Offset"), init: 0 }] },
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
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "join", options: patternOperations }] },
        pipe: { title: qsTr("Pipe"), command: "feature.pipe", fields: [
            { key: "path", type: "pathSketch", label: qsTr("Path sketch") },
            { key: "section", type: "choice", label: qsTr("Section"), init: "circular", options: [
                { value: "circular", label: qsTr("Circular") }, { value: "square", label: qsTr("Square") }] },
            { key: "diameter", type: "expr", label: qsTr("Outer size (diameter / side)"), init: 10 },
            { key: "thickness", type: "expr", label: qsTr("Wall thickness (0: solid)"), init: 0 },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "new_body", options: operations }] },
        coil: { title: qsTr("Coil"), command: "feature.coil", fields: [
            { key: "diameter", type: "expr", label: qsTr("Outer diameter"), init: 30 },
            { key: "section_size", type: "expr", label: qsTr("Wire diameter (square: side)"), init: 4 },
            { key: "coilType", type: "choice", label: qsTr("Size by"), init: "revolutions_pitch", local: true, options: [
                { value: "revolutions_pitch", label: qsTr("Revolutions and pitch") }, { value: "revolutions_height", label: qsTr("Revolutions and height") },
                { value: "height_pitch", label: qsTr("Height and pitch") }] },
            { key: "revolutions", type: "expr", label: qsTr("Revolutions"), init: 5, showIf: v => v.coilType !== "height_pitch" },
            { key: "pitch", type: "expr", label: qsTr("Pitch"), init: 8, showIf: v => v.coilType !== "revolutions_height" },
            { key: "height", type: "expr", label: qsTr("Height"), init: 40, showIf: v => v.coilType !== "revolutions_pitch" },
            { key: "axis", type: "choice", label: qsTr("Axis"), init: "Z", options: [
                { value: "X", label: qsTr("X") }, { value: "Y", label: qsTr("Y") }, { value: "Z", label: qsTr("Z") }] },
            { key: "section", type: "choice", label: qsTr("Section"), init: "circular", options: [
                { value: "circular", label: qsTr("Circular") }, { value: "square", label: qsTr("Square") }] },
            { key: "handedness", type: "choice", label: qsTr("Handedness"), init: "right", options: [
                { value: "right", label: qsTr("Right") }, { value: "left", label: qsTr("Left") }] },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "new_body", options: operations }] },
        rib: { title: qsTr("Rib"), command: "feature.rib", fields: [
            { key: "path", type: "pathSketch", label: qsTr("Curve sketch") },
            { key: "thickness", type: "expr", label: qsTr("Thickness"), init: 3 },
            { key: "depth", type: "expr", label: qsTr("Height"), init: 10 },
            { key: "direction", type: "choice", label: qsTr("Direction"), init: "normal", options: [
                { value: "normal", label: qsTr("Normal to sketch plane") }, { value: "parallel", label: qsTr("Parallel to sketch plane") }] },
            { key: "flip", type: "bool", label: qsTr("Flip side"), init: false },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "new_body", options: operations }] },
        path_pattern: { title: qsTr("Pattern on Path"), command: "feature.path_pattern", fields: [
            { key: "body", type: "body", label: qsTr("Body") },
            { key: "path", type: "pathSketch", label: qsTr("Path sketch") },
            { key: "count", type: "expr", label: qsTr("Quantity"), init: 4 },
            { key: "distance_type", type: "choice", label: qsTr("Distance type"), init: "extent", options: [
                { value: "extent", label: qsTr("Extent") }, { value: "spacing", label: qsTr("Spacing") }] },
            { key: "distance", type: "expr", label: qsTr("Distance (mm; empty: whole path)"), init: undefined },
            { key: "orientation", type: "choice", label: qsTr("Orientation"), init: "identical", options: [
                { value: "identical", label: qsTr("Identical") }, { value: "path_direction", label: qsTr("Path direction") }] },
            { key: "flip", type: "bool", label: qsTr("Start from the other end") },
            { key: "operation", type: "choice", label: qsTr("Operation"), init: "join", options: patternOperations }] },
        // Built on a scan or surface: fitting the skin takes seconds, so no live preview.
        fit_selection: { title: qsTr("Fit Surface"), command: "surface.fit_selection", slow: true, fields: [
            { key: "id", type: "reference", label: qsTr("Source part") },
            { key: "tolerance", type: "expr", label: qsTr("Tolerance (mm)"), init: 0.25 },
            { key: "method", type: "choice", label: qsTr("Fit method"), init: "auto", options: [
                { value: "auto", label: qsTr("Automatic") }, { value: "plane", label: qsTr("Plane") },
                { value: "unfold", label: qsTr("Unfold") }] }] },
        fit_regions: { title: qsTr("Fit Patches"), command: "surface.fit_regions", slow: true, fields: [
            { key: "id", type: "reference", label: qsTr("Source part") },
            { key: "tolerance", type: "expr", label: qsTr("Tolerance (mm)"), init: 0.25 },
            { key: "creaseAngle", type: "expr", label: qsTr("Crease angle (deg)"), init: 30 },
            { key: "blendRings", type: "expr", label: qsTr("Blend rings"), init: 6 }] },
        trim_by_sketch: { title: qsTr("Trim Surface"), command: "surface.trim_by_sketch", slow: true, fields: [
            { key: "id", type: "reference", label: qsTr("Source surface") },
            { key: "sketch", type: "sketch", label: qsTr("Outline sketch") },
            { key: "keep", type: "choice", label: qsTr("Keep"), init: "inside", options: [
                { value: "inside", label: qsTr("Inside the outline") }, { value: "outside", label: qsTr("Outside the outline") }] },
            { key: "extend", type: "bool", label: qsTr("Extend fitted surface"), init: true }] },
        offset_surface: { title: qsTr("Offset Surface"), command: "surface.offset", slow: true, fields: [
            { key: "id", type: "reference", label: qsTr("Source surface") },
            { key: "distance", type: "expr", label: qsTr("Clearance (mm)"), init: 2 }] },
        thicken_surface: { title: qsTr("Thicken Surface"), command: "surface.thicken", slow: true, fields: [
            { key: "id", type: "reference", label: qsTr("Source surface") },
            { key: "thickness", type: "expr", label: qsTr("Thickness (mm)"), init: 3 },
            { key: "side", type: "choice", label: qsTr("Direction"), init: "outside", options: [
                { value: "outside", label: qsTr("Out of the surface") }, { value: "inside", label: qsTr("Into the surface") },
                { value: "both", label: qsTr("Both sides") }] }] },
        part_on_surface: { title: qsTr("Part on Surface"), command: "surface.part_from_sketch", slow: true, fields: [
            { key: "sketch", type: "sketch", label: qsTr("Outline sketch") },
            { key: "thickness", type: "expr", label: qsTr("Thickness"), init: 2 },
            { key: "side", type: "choice", label: qsTr("Direction"), init: "inside", options: [
                { value: "inside", label: qsTr("Into the surface") }, { value: "outside", label: qsTr("Out of the surface") },
                { value: "both", label: qsTr("Both sides") }] }] },
        fin_on_surface: { title: qsTr("Fin on Surface"), command: "surface.fin_from_sketch", slow: true, fields: [
            { key: "sketch", type: "sketch", label: qsTr("Fin sketch") },
            { key: "thickness", type: "expr", label: qsTr("Thickness"), init: 3 },
            { key: "rootFillet", type: "expr", label: qsTr("Root fillet"), init: 0 },
            { key: "count", type: "expr", label: qsTr("Number of fins"), init: 1 },
            // Hidden only for a plain 1: an expression count may be more, and a hidden field is dropped.
            { key: "spacing", type: "expr", label: qsTr("Spacing"), init: 40, showIf: v => !(Number(v.count) <= 1) },
            { key: "mirror", type: "bool", label: qsTr("Also on the other side") }] }
    })

    readonly property var spec: specs[type] || null

    function isSurfaceTool(ref) {
        if (typeof ref === "object" && ref) {
            const part = controller.partInfo(ref.part)
            return part && part.id && !(part.solids > 0)
        }
        // Use the pre-preview timeline snapshot: a consumed tool can disappear from
        // the document while the dialog is open, but Flip must remain editable.
        const body = (timeline.bodies || []).find(b => b.id === ref || b.name === ref)
        if (body && body.solids === 0)
            return true
        // An existing Combine may already have consumed its tool before the dialog
        // opens. Its evaluated report retains the tool's actual geometry kind.
        const feature = (timeline.features || []).find(f => f.id === editId)
        return !!(feature && feature.report && (feature.report.surfaceTools || []).some(
                      t => t.id === ref || t.name === ref))
    }

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
        // Earlier features may have changed the body since the arrow was last placed.
        sizeArrowPlace.key = ""
        sizeArrowPlace.place = null
        // A plane made halfway between two (Construct > Midplane) is edited in its own dialog.
        if (featureType === "plane" && featureId && initial && initial.base2 !== undefined)
            featureType = "midplane"
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
        // A hole made at sketch points is edited with that placement.
        if (featureType === "hole" && initial && initial.sketch !== undefined && initial.placement === undefined)
            v.placement = "sketch"
        // Sizing modes that are not stored as such are inferred from the stored parameters, so that
        // reopening a feature and pressing OK does not swap in the other mode's defaults.
        if (initial && featureType === "coil") {
            if (initial.height !== undefined && initial.revolutions === undefined)
                v.coilType = "height_pitch"
            else if (initial.height !== undefined && initial.pitch === undefined)
                v.coilType = "revolutions_height"
        }
        if (initial && featureType === "scale" && (initial.factor_x !== undefined || initial.factor_y !== undefined || initial.factor_z !== undefined))
            v.scaleType = "non_uniform"
        if (!featureId) {
            if (s.fields.some(f => f.key === "sketch") && v.sketch === undefined) {
                // A hole at sketch points starts from the latest sketch that has points.
                const withPoints = featureType === "hole" ? sketches().filter(f => (f.params.curves || []).some(c => c.type === "point")) : []
                const sk = withPoints.length > 0 ? withPoints : sketches()
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
        // The first selection still to make receives viewport picks (Combine with a target
        // already set starts with the tools).
        const pickFields = s.fields.filter(f => f.type.endsWith("Picks") || f.type === "pointPick" || f.type === "planePick"
                                                || f.type === "targetPick" || f.type === "axisPick" || f.type === "objectPick")
        const firstPick = pickFields.find(f => v[f.key] === undefined) || pickFields[0]
        pickKey = firstPick ? firstPick.key : ""
        previewTimer.restart()
        return true
    }

    function close() {
        previewTimer.stop()
        undoPreview()
        // Cancel: the solids made editable for this dialog go back to what they were, newest
        // first, as long as each is still the last step. A change made on top of one (by an
        // MCP client, a script or another command) is never undone for the dialog: those
        // conversions stay, and the status bar says so.
        let kept = 0
        for (let i = ownedSteps.length - 1; i >= 0; --i) {
            if (!controller.historyApplied(ownedSteps[i]))
                continue // already undone by the user, or the project was replaced
            if (kept > 0 || !controller.undoIfTop(ownedSteps[i]))
                kept += 1
        }
        ownedSteps = []
        if (kept > 0)
            controller.showStatus(kept === 1 ? qsTr("The solid made editable stays editable: other changes were made after it")
                                             : qsTr("%1 solids made editable stay editable: other changes were made after them").arg(kept))
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
    readonly property var ownPlane: type !== "plane" && type !== "midplane" ? -1 : editId ? editId : previewFeatureId

    /// What viewport clicks should hit while the dialog waits for picks: profiles live on
    /// sketches, edges/faces/points on bodies (the preview body would hide the sketch).
    readonly property string pickKind: {
        const f = spec && pickKey ? spec.fields.find(x => x.key === pickKey) : null
        if (!f)
            return ""
        if (f.type === "planePick")
            return "" // faces of bodies and construction planes
        if (f.type === "toolPicks")
            return "" // bodies, solids, surfaces and scans
        if (f.type === "axisPick")
            return "axis" // lines of sketches, straight edges of bodies other than the preview
        if (f.type === "objectPick")
            return "object" // bodies other than the preview, surfaces, solids and scans
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
        if (!spec || spec.slow)
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

    /// Revolve: the full-turn Angle was lowered by choosing Two Sides.
    property bool angleLowered: false

    function setValue(key, value) {
        const v = Object.assign({}, values)
        if (value === undefined || value === "" || value === null)
            delete v[key]
        else
            v[key] = value
        // Revolve Two Sides: both angles must fit in one turn, so the default full turn gives way
        // (and comes back when the direction changes again, unless the angle was edited).
        if (key === "angle")
            angleLowered = false
        if (key === "extent" && spec && spec.fields.some(f => f.key === "angle2")) {
            if (value === "two_sides" && (v.angle === undefined || Number(v.angle) >= 360)) {
                v.angle = 90
                angleLowered = true
            } else if (value !== "two_sides" && angleLowered) {
                v.angle = 360
                angleLowered = false
            }
        }
        values = v
        // Extrude "To Object" (Fusion): the object is picked next.
        if (key === "extent" && value === "to_object" && v.to === undefined && spec && spec.fields.some(f => f.key === "to"))
            pickKey = "to"
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

    /// Undo-step tokens (controller.historyTopId) of the conversions made for this dialog
    /// (feature.base_body runs): Cancel undoes them, OK joins them with the feature into one
    /// undo step, in both cases only while no one else's step is in between.
    property var ownedSteps: []

    /// Makes imported part `partId` a timeline body (feature.base_body) as part of this
    /// dialog. Returns { id, kind: "body", featureId } of the new body, or null.
    function convertToBody(partId) {
        undoPreview()
        const r = controller.execute("feature.base_body", { part: partId })
        const body = r.ok && r.result.bodies ? r.result.bodies.find(b => b.id === r.result.id) : null
        if (!body)
            return null
        ownedSteps = ownedSteps.concat([controller.historyTopId()])
        previewTimer.restart()
        return { id: body.partId, kind: "body", featureId: body.id }
    }

    /// Revolve axis from a click at view position (x, y) of `view` (the Viewport): a line of
    /// the dialog's sketch within a few pixels ({line: index}), else the nearest straight
    /// edge of the body under the cursor ({origin, direction}); the dialog's own preview
    /// body is looked through. Returns false when the click found neither.
    function pickAxisAt(view, x, y) {
        if (!spec || !pickKey)
            return false
        const field = spec.fields.find(f => f.key === pickKey)
        if (!field || field.type !== "axisPick")
            return false
        let axis = null
        if (values.sketch !== undefined) {
            const pr = controller.execute("sketch.profiles", { sketch: values.sketch })
            const cr = controller.execute("sketch.curves", { sketch: values.sketch })
            if (pr.ok && cr.ok) {
                const pl = pr.result.plane
                const v3 = a => Qt.vector3d(a[0], a[1], a[2])
                const w = view.planePoint(x, y, v3(pl.origin), v3(pl.normal))
                if (w) {
                    const d = w.minus(v3(pl.origin))
                    const uv = [d.dotProduct(v3(pl.xDir)), d.dotProduct(v3(pl.yDir))]
                    const tol = 8 * view.worldPerPixelAt(w)
                    let bestD = tol
                    for (const c of cr.result.curves) {
                        if (c.kind !== "line")
                            continue
                        const p0 = c.points[0], p1 = c.points[1]
                        const dx = p1[0] - p0[0], dy = p1[1] - p0[1]
                        const l2 = dx * dx + dy * dy
                        const t = l2 > 0 ? Math.max(0, Math.min(1, ((uv[0] - p0[0]) * dx + (uv[1] - p0[1]) * dy) / l2)) : 0
                        const dist = Math.hypot(uv[0] - p0[0] - t * dx, uv[1] - p0[1] - t * dy)
                        if (dist <= bestD) { axis = { line: c.index }; bestD = dist }
                    }
                }
            }
        }
        if (!axis) {
            // Only bodies (not the preview) take the click: planes, sketches and scans in
            // front are clicked through.
            const parts = controller.execute("document.parts")
            const ids = parts.ok ? parts.result.parts.filter(p => p.kind === "body" && p.featureId !== previewFeatureId).map(p => p.id) : []
            const hit = view.pickPartAmong(x, y, ids)
            const info = hit ? controller.partInfo(hit.partId) : null
            if (info && info.kind === "body")
                axis = edgeAxis(info.featureId, hit.point)
        }
        if (!axis) {
            controller.showStatus(qsTr("Click a line of the sketch or a straight edge of a body"))
            return false
        }
        setValue(pickKey, axis)
        return true
    }

    /// The straight edge of body `featureId` nearest to `point`, as {origin, direction}.
    function edgeAxis(featureId, point) {
        const er = controller.execute("body.edges", { body: featureId })
        if (!er.ok)
            return null
        let best = null, bestD = Infinity
        for (const e of er.result.edges) {
            if (e.type !== "line")
                continue
            const a = e.start, b = e.end
            const ab = [b[0] - a[0], b[1] - a[1], b[2] - a[2]]
            const ap = [point.x - a[0], point.y - a[1], point.z - a[2]]
            const l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2]
            const t = l2 > 0 ? Math.max(0, Math.min(1, (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2)) : 0
            const dist = Math.hypot(ap[0] - t * ab[0], ap[1] - t * ab[1], ap[2] - t * ab[2])
            if (dist < bestD) { best = e; bestD = dist }
        }
        if (!best)
            return null
        const r4 = x => Math.round(x * 1e4) / 1e4
        return { origin: best.start.map(r4), direction: [0, 1, 2].map(i => r4(best.end[i] - best.start[i])) }
    }

    /// Viewport pick while the dialog is open: `part` is controller.partInfo().
    function addPick(part, point) {
        if (!spec || !pickKey || !part || !part.id)
            return
        const field = spec.fields.find(f => f.key === pickKey)
        if (field.type === "targetPick") {
            if (!(part.solids > 0))
                return // Surfaces and scans can be tools, never the solid target.
            const b = asBody(part)
            if (!b)
                return
            setValue(pickKey, b.featureId)
            // Like Fusion: the tools are picked next.
            const tools = spec.fields.find(f => f.type === "toolPicks")
            if (tools)
                pickKey = tools.key
            return
        }
        if (field.type === "toolPicks") {
            // Bodies by id; anything else (a fin or STEP solid, a surface, the scan) as a part.
            let ref
            if (part.kind === "body")
                ref = part.featureId
            else if (part.kind === "imported")
                ref = { part: part.id }
            else
                return
            const target = spec.fields.find(f => f.type === "targetPick")
            if (target && ref === values[target.key])
                return
            const list = (values[pickKey] || []).filter(t => JSON.stringify(t) !== JSON.stringify(ref))
            if (list.length === (values[pickKey] || []).length)
                list.push(ref) // a second click on a picked tool removes it
            setValue(pickKey, list)
            return
        }
        if (field.type === "axisPick")
            return // handled by pickAxisAt
        if (field.type === "objectPick") {
            // A body: up to the clicked face of it; a surface, solid or scan: up to it.
            if (part.kind === "body" && part.featureId !== previewFeatureId)
                setValue(pickKey, { body: part.featureId, faces: [{ point: [point.x, point.y, point.z].map(x => Math.round(x * 1e4) / 1e4) }] })
            else if (part.kind === "imported")
                setValue(pickKey, { part: part.id })
            return
        }
        if (field.type === "planePick") {
            // The body the preview made (Split Body's front piece) is gone when the step is
            // recomputed, so a face of it cannot hold the plane.
            if (part.kind === "body" && part.featureId === previewFeatureId)
                return
            const key = pickKey
            if (part.kind === "plane")
                setValue(key, { plane: part.featureId })
            else if ((part = asBody(part)))
                setValue(key, { body: part.featureId, point: [point.x, point.y, point.z].map(x => Math.round(x * 1e4) / 1e4) })
            else
                return
            // Midplane: the next click fills the second plane.
            const next = spec.fields.find(f => f.type === "planePick" && f.key !== key && values[f.key] === undefined)
            if (next)
                pickKey = next.key
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
                if (values.axis && values.axis.line !== undefined)
                    setValue("axis", "Z")
            }
            list.push(uv.map(x => Math.round(x * 1e4) / 1e4))
            // Revolve (Fusion): the axis is picked next, unless one was clicked already.
            const axisField = spec.fields.find(f => f.type === "axisPick")
            if (axisField && list.length === 1 && typeof values[axisField.key] !== "object") {
                setValue(pickKey, list)
                pickKey = axisField.key
                return
            }
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
        // A field the command would otherwise default (Midplane's planes fall back to XY)
        // must be filled in: nothing is made until it is.
        const empty = f => values[f.key] === undefined || (Array.isArray(values[f.key]) && values[f.key].length === 0)
        const missing = spec.fields.find(f => f.required && empty(f))
        if (missing)
            return { ok: false, error: qsTr("%1: pick the %2").arg(spec.title).arg(missing.label.toLowerCase()) }
        const run = (name, args) => quiet ? controller.preview(name, args) : controller.execute(name, args)
        const params = Object.assign({}, values)
        for (const f of spec.fields) {
            if (f.local || (f.showIf && !f.showIf(values)))
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
                    // Values tied to the chosen sketch (a hole's sketch points) go with it.
                    if (!(k in params) && (spec.fields.some(f => f.key === k) || k === "sketch_points" || k === "deletedSketchPoints"))
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
            const steps = ownedSteps.filter(id => controller.historyApplied(id))
            if (steps.length > 0)
                controller.mergeHistoryIds(steps.concat([controller.historyTopId()]), spec.title)
            ownedSteps = []
            // A kept approximation (fit tolerance missed) or another caveat must not pass silently.
            const res = r.result || {}
            const warnings = res.warnings || (res.report && res.report.warnings) || []
            if (warnings.length > 0)
                controller.showStatus(qsTr("%1: %2").arg(spec.title).arg(warnings[0]))
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

                // A checkbox carries its own label (inline, like the rest of the app).
                Label { visible: fieldItem.field.type !== "bool"; text: fieldItem.field.label; opacity: 0.75 }

                Loader {
                    Layout.fillWidth: true
                    sourceComponent: {
                        switch (fieldItem.field.type) {
                        case "expr": return exprEditor
                        case "reference": return referenceEditor
                        case "choice": return choiceEditor
                        case "bool": return boolEditor
                        case "sketch": return sketchEditor
                        case "body": return bodyEditor
                        case "bodyList": return bodyListEditor
                        case "targetPick": return namedPickEditor
                        case "objectPick": return namedPickEditor
                        case "toolPicks": return namedPickEditor
                        case "sketchList": return sketchListEditor
                        case "pathSketch": return pathSketchEditor
                        case "planePick": return planeEditor
                        case "axisPick": return axisEditor
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
        id: referenceEditor
        Label {
            property var field: parent ? parent.field : ({})
            readonly property var source: controller.partInfo(Number(root.values[field.key] || 0))
            text: source && source.id ? source.name + " (#" + source.id + ")" : qsTr("Missing source")
            elide: Text.ElideRight
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
            text: field.label || ""
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
            onActivated: {
                root.setValue(field.key, currentValue)
                root.setValue("points", undefined)
                // Hole: the picked sketch points (curve indices) belong to the old sketch.
                root.setValue("sketch_points", undefined)
                root.setValue("deletedSketchPoints", undefined)
                // A picked sketch line belongs to the old sketch.
                if (root.values.axis && root.values.axis.line !== undefined)
                    root.setValue("axis", "Z")
            }
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
        // Origin planes and construction planes; a face clicked in the view shows as an extra
        // entry. "Pick in the view" sends the next click here, when the dialog also picks
        // faces or edges (Draft) or has two planes (Midplane).
        ColumnLayout {
            property var field: parent ? parent.field : ({})
            spacing: 2
            ComboBox {
            Layout.fillWidth: true
            readonly property var field: parent.field
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
            displayText: currentIndex < 0 ? (field.hint || "") : currentText
            onActivated: index => root.setValue(field.key, items[index].value)
            }
            Button {
                Layout.fillWidth: true
                visible: root.spec && root.spec.fields.filter(f => f.type.endsWith("Picks") || f.type.endsWith("Pick")).length > 1
                checkable: true
                checked: root.pickKey === field.key
                text: root.pickKey === field.key ? qsTr("Click a planar face or plane in the view") : qsTr("Pick in the view")
                onClicked: root.pickKey = field.key
            }
        }
    }
    Component {
        id: axisEditor
        // Revolve axis (Fusion: Axis selection): X / Y / Z, or a line / edge clicked in the view.
        ColumnLayout {
            property var field: parent ? parent.field : ({})
            readonly property var current: root.values[field.key]
            spacing: 2
            ComboBox {
                Layout.fillWidth: true
                readonly property var items: {
                    const out = [{ name: "X", value: "X" }, { name: "Y", value: "Y" }, { name: "Z", value: "Z" }]
                    if (current && typeof current === "object")
                        out.push({ name: current.line !== undefined ? qsTr("Sketch line %1").arg(current.line + 1)
                                                                   : qsTr("Picked edge"), value: current })
                    return out
                }
                model: items.map(i => i.name)
                currentIndex: items.findIndex(i => JSON.stringify(i.value) === JSON.stringify(current))
                onActivated: index => root.setValue(field.key, items[index].value)
            }
            Button {
                Layout.fillWidth: true
                checkable: true
                checked: root.pickKey === field.key
                text: root.pickKey === field.key ? field.hint : qsTr("Pick in the view")
                onClicked: root.pickKey = field.key
            }
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
    /// Name of a picked body (feature id) or part ({part: id}).
    function pickName(ref) {
        if (typeof ref === "object" && ref && ref.body !== undefined)
            return ref.faces ? qsTr("Face of %1").arg(pickName(ref.body)) : pickName(ref.body)
        if (typeof ref === "object" && ref)
            return controller.partInfo(ref.part).name || qsTr("Part %1").arg(ref.part)
        const b = (timeline.bodies || []).find(b => b.id === ref)
        if (b)
            return b.name
        const feature = (timeline.features || []).find(f => f.id === editId)
        const tool = feature && feature.report ? (feature.report.surfaceTools || []).find(
                         t => t.id === ref || t.name === ref) : null
        if (tool)
            return tool.name
        // A body made a moment ago (a solid made editable on the way): not in the timeline
        // snapshot yet.
        const r = controller.execute("document.parts")
        const p = r.ok ? r.result.parts.find(p => p.kind === "body" && p.featureId === ref) : null
        return p ? p.name : String(ref)
    }

    Component {
        id: namedPickEditor
        // Bodies picked in the viewport, by name (Combine target and tools).
        RowLayout {
            property var field: parent ? parent.field : ({})
            readonly property var picked: {
                const v = root.values[field.key]
                // Timeline JSON lists can be Qt sequence objects, not JS Arrays.
                return v === undefined ? [] : field.type === "toolPicks" ? v : [v]
            }
            Button {
                Layout.fillWidth: true
                checkable: true
                checked: root.pickKey === field.key
                text: picked.length > 0 ? picked.map(r => root.pickName(r)).join(", ") : field.hint
                onClicked: root.pickKey = field.key
            }
            ToolButton {
                text: "✕"
                visible: picked.length > 0
                onClicked: root.setValue(field.key, undefined)
            }
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
