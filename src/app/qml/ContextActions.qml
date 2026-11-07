// SPDX-License-Identifier: GPL-3.0-or-later
// What can be done with the current selection, in words a new user understands.
// One registry used by the step-by-step guide (GuidePanel, top of Properties), the
// context bar over the 3D view (ContextBar) and the right-click menu on parts, so a
// command is described once and offered the same way everywhere. See docs/ui.md,
// "Guided workflow".
//
// An action is { id, text, icon, tip, enabled, reason, params, primary }:
//   tip     one or two sentences on what it does (shown in tooltips and the guide),
//   reason  why it is disabled (shown instead of the tip while disabled),
//   params  [{ key, label, type: "number" | "choice" | "sketch", value, unit, options, help }]
//           asked in a small popup (ActionPopup) before the command runs.
// run(id, values) runs it; a command that creates a part selects that part, so the next
// step of the guide is offered right away.
import QtQuick
import AeroCAD

QtObject {
    id: root

    required property AppController controller
    /// Main.qml's window: selectPart, startCreateSketch, createSketch, openFeature,
    /// openExport, editFeature, setSelectionTool.
    required property var host

    /// Changes whenever the document changes, so bindings that call guide() / actions()
    /// re-evaluate.
    property int revision: 0
    readonly property Connections _watch: Connections {
        target: root.controller
        function onPartChanged() { root.revision++ }
        function onHistoryChanged() { root.revision++ }
    }

    // --- Classification ----------------------------------------------------------------

    /// "mesh" (scan), "surface" (fitted NURBS surface), "solid" (imported exact solid),
    /// "body" (timeline body), "sketch", or "" (nothing selected).
    function role(info) {
        if (!info || !info.id)
            return ""
        if (info.kind === "sketch")
            return info.kind
        if (info.hasBrep && (info.faces ?? 0) > 0 && (info.solids ?? 0) === 0)
            return "surface"
        if (info.kind === "body")
            return "body"
        if (info.hasBrep && (info.solids ?? 0) > 0)
            return "solid"
        if (info.hasBrep && (info.faces ?? 0) > 0)
            return "surface"
        return "mesh"
    }

    /// A body, or an exact solid that features make editable on the way (Main.openFeature).
    function editableSolid(info) {
        const r = role(info)
        return r === "body" || r === "solid"
    }

    function roleName(r) {
        return ({ mesh: qsTr("Scan (mesh)"), surface: qsTr("Surface"), solid: qsTr("Solid"),
                  body: qsTr("Body"), sketch: qsTr("Sketch") })[r] || ""
    }

    function roleIcon(r) {
        return ({ mesh: "mesh", surface: "surface", solid: "body", body: "body", sketch: "sketch" })[r] || "tool"
    }

    /// Sketches of the document, newest first: [{ name, featureId, partId }].
    function sketches() {
        void revision
        const r = controller.execute("document.parts")
        const list = []
        for (const p of (r.ok ? r.result.parts : []))
            if (p.kind === "sketch")
                list.unshift({ name: p.name, featureId: p.featureId, partId: p.id })
        return list
    }

    /// The part a sketch was drawn on (sketch.create {surface}), or 0.
    function sketchSurface(featureId) {
        if (!(featureId > 0))
            return 0 // no sketch (the toolbar asks without a part)
        const r = controller.execute("sketch.profiles", { sketch: featureId })
        return r.ok && r.result.onSurface ? (r.result.onSurface.part ?? 0) : 0
    }

    /// True for a sketch drawn across a surface (a fin).
    function sketchAcross(featureId) {
        if (!(featureId > 0))
            return false // no sketch (the toolbar asks without a part)
        const r = controller.execute("sketch.profiles", { sketch: featureId })
        return r.ok && !!r.result.onSurface && !!r.result.onSurface.across
    }

    function sketchHasProfiles(featureId) {
        if (!(featureId > 0))
            return false // no sketch (the toolbar asks without a part)
        const r = controller.execute("sketch.profiles", { sketch: featureId })
        return r.ok && (r.result.profiles ?? []).length > 0
    }

    // --- Actions -----------------------------------------------------------------------

    /// The timeline feature that made a body (several fins share one), or null.
    function ownFeature(info) {
        void revision
        if (!info || info.kind !== "body")
            return null
        const r = controller.execute("timeline.list")
        const id = info.featureId % 1099511627776 // 2^40: see cad::featureOfBody
        return r.ok ? (r.result.features.find(f => f.id === id) ?? null) : null
    }

    /// True for a surface piece made by "Cut out by sketch" (its next step is Thicken).
    function isCutOut(info) {
        return String(info.source ?? "").startsWith("surface.trim_by_sketch")
    }

    /// The scan a fitted surface or solid was made from (surface.fit_selection /
    /// surface.fit_regions), or 0 when it is gone.
    function fitSource(info) {
        const m = /^surface\.fit_(selection|regions) of part (\d+)$/.exec(String(info.source ?? ""))
        if (!m)
            return 0
        const id = Number(m[2])
        return controller.parts.rowOf(id) >= 0 ? id : 0
    }

    function _num(key, label, value, unit, help) {
        return { key: key, label: label, type: "number", value: value, unit: unit || "", help: help || "" }
    }

    function _choice(key, label, options, help) {
        return { key: key, label: label, type: "choice", options: options, value: options[0].value, help: help || "" }
    }

    /// The action `id` for the part `info` (from controller.partInfo).
    function action(id, info) {
        void revision
        const selected = (info.selectedTriangles ?? 0) > 0
        switch (id) {
        // Start
        case "import":
            return { id: id, text: qsTr("Import a scan or model"), icon: "import", enabled: true,
                     tip: qsTr("Open an STL, OBJ, PLY, 3MF or STEP file. You can also drop a file onto the 3D view.") }
        case "newSketch":
            return { id: id, text: qsTr("New sketch"), icon: "sketch", enabled: true,
                     tip: qsTr("Draw a 2D outline on a plane or on a face, then extrude or revolve it into a solid.") }
        case "box":
            return { id: id, text: qsTr("Box"), icon: "box", enabled: true,
                     tip: qsTr("Start from a simple block whose size you type in.") }

        // Scan
        case "regionTool":
            return { id: id, text: qsTr("Click an area"), icon: "select-click", enabled: true,
                     tip: qsTr("Click the scan: the smooth area around the click is selected, up to sharp edges. Shortcut 1.") }
        case "lassoTool":
            return { id: id, text: qsTr("Draw around"), icon: "lasso", enabled: true,
                     tip: qsTr("Drag a loop around the area to select. Shortcut 2.") }
        case "brushTool":
            return { id: id, text: qsTr("Paint"), icon: "brush", enabled: true,
                     tip: qsTr("Paint over the area with a brush; [ and ] change its size. Shortcut 3.") }
        case "growSelection":
            return { id: id, text: qsTr("Grow"), icon: "grow", enabled: selected,
                     reason: qsTr("Select an area first."), tip: qsTr("Adds one ring of triangles around the selection.") }
        case "shrinkSelection":
            return { id: id, text: qsTr("Shrink"), icon: "shrink", enabled: selected,
                     reason: qsTr("Select an area first."), tip: qsTr("Removes one ring of triangles from the edge of the selection.") }
        case "clearSelection":
            return { id: id, text: qsTr("Clear"), icon: "close", enabled: selected,
                     reason: qsTr("Nothing is selected."), tip: qsTr("Deselects the area.") }
        case "fitSurface":
            return { id: id, text: qsTr("Fit smooth surface"), icon: "surface", enabled: selected, primary: true,
                     reason: qsTr("Select an area of the scan first (step 1)."),
                     tip: qsTr("Turns the selected area into one smooth, exact (NURBS) surface you can design on. Holes in the area are cut out. The result is a new part; the scan is kept."),
                     params: [_num("tolerance", qsTr("Tolerance"), 0.25, "mm",
                                   qsTr("How far the surface may deviate from the scan. Smaller follows the scan more closely but may be bumpier."))] }
        case "fitWhole":
            return { id: id, text: selected ? qsTr("Fit patches to selection") : qsTr("Whole scan to solid"),
                     icon: "body", enabled: true,
                     tip: qsTr("Splits the scan at its sharp edges, fits a smooth surface to each patch and sews them together. A closed scan becomes a solid."),
                     params: [_num("tolerance", qsTr("Tolerance"), 0.25, "mm",
                                   qsTr("How far the surfaces may deviate from the scan."))] }
        case "fitDeviation": {
            const scan = fitSource(info)
            return { id: id, text: qsTr("Show deviation from the scan"), icon: "deviation", enabled: scan > 0,
                     reason: qsTr("The scan this was fitted to is gone."),
                     tip: qsTr("Colours the scan by its distance to this part: green within the tolerance, red outside, blue inside. This part is hidden meanwhile; Inspect > Clear Analyses turns the colours off."),
                     params: [_num("tolerance", qsTr("Tolerance"), 0.25, "mm",
                                   qsTr("Deviations up to this are green; the colours reach full red or blue at twice this."))] }
        }
        case "sketchOnScan":
            return { id: id, text: qsTr("Sketch on the scan"), icon: "sketch", enabled: true, primary: !selected,
                     tip: qsTr("Starts a sketch on a plane fitted to the scan where you click (or over the selected area). Draw the outline of the part, then click Finish Sketch.") }
        case "sketchFin":
            return { id: id, text: qsTr("Sketch a fin"), icon: "sketch", enabled: true,
                     tip: qsTr("Starts a sketch standing across the surface where you click, for a fin such as a canard or a winglet. The orange line shows the surface; draw the fin with its root a little past it.") }
        case "toBody":
            return { id: id, text: qsTr("Convert to body"), icon: "mesh-to-body", enabled: true,
                     tip: qsTr("Uses the whole mesh as a solid body in the timeline, so fillets, holes and cuts work on it. Best for clean, closed meshes; for a raw scan fit a surface instead.") }
        case "repair":
            return { id: id, text: qsTr("Repair mesh"), icon: "repair", enabled: true,
                     tip: qsTr("Removes duplicate and degenerate triangles and merges vertices at the same position. Fixes many \"not watertight\" export warnings.") }
        case "simplify":
            return { id: id, text: qsTr("Simplify"), icon: "simplify", enabled: true,
                     tip: qsTr("Reduces the number of triangles so the scan is faster to work with; the shape changes very little."),
                     params: [_num("targetTriangles", qsTr("Triangles"),
                                   Math.max(1000, Math.round((info.triangles ?? 200000) / 4)), "",
                                   qsTr("How many triangles to keep at most."))] }
        case "scale":
            return { id: id, text: qsTr("Scale"), icon: "scale", enabled: info.kind === "imported",
                     reason: qsTr("Scale the imported source instead."),
                     tip: qsTr("Makes the part bigger or smaller, e.g. when a scan came in the wrong unit."),
                     params: [_num("percent", qsTr("Scale"), 100, "%", qsTr("100 % keeps the size, 1000 % turns cm into mm."))] }

        // Surface
        case "sketchOnSurface":
            return { id: id, text: qsTr("Sketch the outline"), icon: "sketch", enabled: true,
                     primary: sketches().length === 0,
                     tip: qsTr("Starts a sketch on a plane fitted to this surface. Draw the closed outline of the part (e.g. an aileron), then click Finish Sketch.") }
        case "trim": {
            const sk = sketches()
            return { id: id, text: qsTr("Cut out by sketch"), icon: "section", enabled: sk.length > 0,
                     primary: sk.length > 0 && !isCutOut(info),
                     reason: qsTr("Draw a closed outline in a sketch first (step 1)."),
                     tip: qsTr("Projects the sketch outline onto this curved surface and keeps the piece inside it, as a new surface that follows the curvature exactly."),
                     params: [{ key: "sketch", label: qsTr("Sketch"), type: "sketch", value: sk.length ? sk[0].featureId : 0,
                                options: sk.map(s => ({ value: s.featureId, label: s.name })),
                                help: qsTr("The outline to cut out. The newest sketch is preselected.") },
                              _choice("keep", qsTr("Keep"), [{ value: "inside", label: qsTr("Inside the outline") },
                                                             { value: "outside", label: qsTr("Outside the outline") }])] }
        }
        case "thicken":
            return { id: id, text: qsTr("Thicken to solid"), icon: "thickness", enabled: true,
                     primary: isCutOut(info),
                     tip: qsTr("Gives the surface a thickness, so it becomes a closed solid you can export and print."),
                     params: [_num("thickness", qsTr("Thickness"), 2, "mm"),
                              _choice("side", qsTr("Direction"), [{ value: "outside", label: qsTr("Outwards (along the normal)") },
                                                                  { value: "inside", label: qsTr("Inwards") },
                                                                  { value: "both", label: qsTr("Both sides, centred") }],
                                      qsTr("Which side of the surface the material goes."))] }
        case "offset":
            return { id: id, text: qsTr("Offset copy"), icon: "plane", enabled: true,
                     tip: qsTr("A copy of the surface moved along its normal, e.g. to leave a gap for paint or a clearance."),
                     params: [_num("distance", qsTr("Distance"), 2, "mm", qsTr("Negative values go to the other side."))] }

        // Sketch
        case "finOnSurface": {
            const ok = sketchHasProfiles(info.featureId)
            return { id: id, text: qsTr("Fin on surface"), icon: "thickness", enabled: ok, primary: true,
                     reason: qsTr("The sketch needs a closed outline."),
                     tip: qsTr("Gives the fin outline a thickness (half each side of the sketch) and cuts its root to the surface, so it sits flush on it."),
                     params: [_num("thickness", qsTr("Thickness"), 2, "mm"),
                              _num("rootFillet", qsTr("Root fillet"), 2, "mm", qsTr("Rounds the joint between the fin and the surface; 0 for a sharp joint.")),
                              _num("count", qsTr("Fins"), 1, "", qsTr("More than one: copies stacked across the sketch, each set on the surface where it lands.")),
                              _num("spacing", qsTr("Spacing"), 40, "mm", qsTr("Distance between the fins; negative puts the copies on the other side.")),
                              _choice("mirror", qsTr("Sides"), [{ value: "no", label: qsTr("This side only") },
                                                               { value: "auto", label: qsTr("Both, centre plane of the scan") },
                                                               { value: "yz", label: qsTr("Both, YZ plane") },
                                                               { value: "xz", label: qsTr("Both, XZ plane") }],
                                      qsTr("Also makes the fins on the other side of the car, mirrored and set on the surface there."))] }
        }
        case "editStep": {
            const f = ownFeature(info)
            return { id: id, text: qsTr("Edit step"), icon: f ? Theme.featureIcon(f.type) : "tool", enabled: !!f,
                     reason: qsTr("This body has no step of its own in the timeline."),
                     tip: qsTr("Changes the settings of the timeline step that made this body; the model is rebuilt.") }
        }
        case "editSketch":
            return { id: id, text: qsTr("Edit sketch"), icon: "sketch", enabled: true,
                     tip: qsTr("Opens the sketch again to change curves and dimensions.") }
        case "partOnSurface": {
            const ok = sketchHasProfiles(info.featureId)
            return { id: id, text: qsTr("Part on surface"), icon: "thickness", enabled: ok, primary: true,
                     reason: qsTr("The sketch needs a closed outline."),
                     tip: qsTr("In one step: cuts the outline out of the surface it was drawn on and thickens it into a solid that follows the curvature."),
                     params: [_num("thickness", qsTr("Thickness"), 2, "mm"),
                              _choice("side", qsTr("Direction"), [{ value: "inside", label: qsTr("Into the surface") },
                                                                  { value: "outside", label: qsTr("Out of the surface") },
                                                                  { value: "both", label: qsTr("Both sides, centred") }])] }
        }
        case "extrude": {
            const ok = info.kind !== "sketch" || sketchHasProfiles(info.featureId)
            return { id: id, text: qsTr("Extrude"), icon: "extrude", enabled: ok, primary: !(info.kind === "sketch" && sketchSurface(info.featureId) > 0),
                     reason: qsTr("The sketch needs a closed outline."),
                     tip: qsTr("Pulls the closed outline straight out into a solid (or cuts with it). Shortcut E.") }
        }
        case "revolve": {
            const ok = info.kind !== "sketch" || sketchHasProfiles(info.featureId)
            return { id: id, text: qsTr("Revolve"), icon: "revolve", enabled: ok,
                     reason: qsTr("The sketch needs a closed outline."),
                     tip: qsTr("Spins the outline around an axis into a round solid.") }
        }

        // Body / solid
        case "sketchOnFace":
            return { id: id, text: qsTr("Sketch on a face"), icon: "sketch", enabled: true, primary: info.kind === "body",
                     tip: qsTr("Click a flat face of the body to draw on it, e.g. the outline of a pocket or a boss.") }
        case "fillet":
            return { id: id, text: qsTr("Round edges"), icon: "fillet", enabled: editableSolid(info),
                     reason: qsTr("Only solids have edges to round."),
                     tip: qsTr("Fillet: rounds the edges you click. Shortcut F.") }
        case "chamfer":
            return { id: id, text: qsTr("Bevel edges"), icon: "chamfer", enabled: editableSolid(info),
                     reason: qsTr("Only solids have edges to bevel."),
                     tip: qsTr("Chamfer: cuts the edges you click, at 45° or with two distances or an angle.") }
        case "shell":
            return { id: id, text: qsTr("Hollow out"), icon: "shell", enabled: editableSolid(info),
                     reason: qsTr("Only solids can be hollowed out."),
                     tip: qsTr("Shell: removes the faces you click and leaves walls of the given thickness.") }
        case "hole":
            return { id: id, text: qsTr("Hole"), icon: "hole", enabled: editableSolid(info),
                     reason: qsTr("Only solids can be drilled."), tip: qsTr("Drills a hole where you click. Shortcut H.") }
        case "combine":
            return { id: id, text: qsTr("Combine"), icon: "combine", enabled: editableSolid(info),
                     reason: qsTr("Only solids can be combined."),
                     tip: qsTr("Joins this solid with other bodies, cuts them out of it, or keeps only the overlap. A surface or the scan as the tool cuts it to that shape.") }
        case "cutWithThis":
            return { id: id, text: qsTr("Cut a part to this shape"), icon: "combine", enabled: true,
                     tip: qsTr("Combine with this as the tool: click the part, and what is behind this surface (inside the scan) is cut away, so the part sits flush on it.") }
        case "solidToBody":
            return { id: id, text: qsTr("Make editable"), icon: "mesh-to-body", enabled: true,
                     tip: qsTr("Adds the solid to the timeline as a body, so fillets, holes and cuts work on it.") }

        // Any part
        case "export":
            return { id: id, text: qsTr("Export…"), icon: "export", enabled: info.kind !== "sketch",
                     // A finished solid is usually exported next, but by Export for printing.
                     primary: false,
                     reason: qsTr("Sketches are not exported."),
                     tip: qsTr("Saves this part as STL or 3MF (3D printing) or STEP (CAD).") }
        case "exportForPrint":
            return { id: id, text: qsTr("Export for printing…"), icon: "export", enabled: role(info) === "solid",
                     primary: role(info) === "solid",
                     reason: qsTr("Only solids can be printed."),
                     tip: qsTr("Writes every visible solid (e.g. all the fins) to a folder you choose, each as its own STL for the printer and STEP for CAD. Hide what you do not want printed.") }
        case "moveRotate": {
            const r = role(info)
            // Shape role determines which modelling tools fit; ownership determines
            // whether a transform is a timeline feature or an imported-part edit.
            if (info.kind === "body")
                return { id: id, text: qsTr("Move / Rotate…"), icon: "move", enabled: true,
                         tip: qsTr("Turns the part about an axis and moves it by a distance. Added to the timeline, so it can be edited later. Shortcut M.") }
            return { id: id, text: qsTr("Move / Rotate…"), icon: "move", enabled: r !== "sketch" && r !== "" && info.kind !== "plane",
                     reason: qsTr("Move the solid the sketch belongs to instead."),
                     tip: qsTr("Turns the part about an axis through its centre and moves it by a distance. Shortcut M."),
                     params: [_choice("axis", qsTr("Rotate about"), [{ value: "z", label: qsTr("Z axis (up)") },
                                                                      { value: "x", label: qsTr("X axis") },
                                                                      { value: "y", label: qsTr("Y axis") }]),
                              _num("angle", qsTr("Angle"), 90, "°", qsTr("Positive turns anticlockwise looking down the axis. 0 only moves.")),
                              _num("dx", qsTr("Move X"), 0, "mm"),
                              _num("dy", qsTr("Move Y"), 0, "mm"),
                              _num("dz", qsTr("Move Z"), 0, "mm")] }
        }
        case "zoomTo":
            return { id: id, text: qsTr("Zoom to this"), icon: "fit", enabled: true,
                     tip: qsTr("Fills the view with this part. F6 fits everything.") }
        case "hide":
            return { id: id, text: info.visible ? qsTr("Hide") : qsTr("Show"), icon: info.visible ? "hidden" : "visible",
                     enabled: true, tip: qsTr("Hides or shows the part in the view. Shortcut V.") }
        case "finishSketch":
            return { id: id, text: qsTr("Finish Sketch"), icon: "finish-sketch", enabled: true, primary: true,
                     tip: qsTr("Closes the sketch. Select it afterwards (Browser) to turn the outline into a part.") }
        case "delete":
            return { id: id, text: qsTr("Delete"), icon: "delete", enabled: true,
                     tip: qsTr("Removes the part (timeline parts: removes the feature that made them). Undo brings it back.") }
        }
        return { id: id, text: id, icon: "tool", enabled: false, tip: "" }
    }

    /// Step-by-step guide for the selection: { title, intro, sections: [{ title, steps:
    /// [{ title, text, done, actions: [action] }], actions: [action] }] }.
    function guide(info) {
        const g = _guide(info)
        for (const s of g.sections) {
            s.steps = s.steps || []
            s.actions = s.actions || []
            s.title = s.title || ""
        }
        return g
    }

    function _guide(info) {
        void revision
        const a = id => action(id, info)
        const r = role(info)
        const selected = (info.selectedTriangles ?? 0) > 0
        // A surface or solid fitted to a scan can show how far the scan is from it.
        const fitted = r !== "mesh" && fitSource(info) > 0 ? [a("fitDeviation")] : []
        if (host.activeSketch) {
            return { title: qsTr("Drawing a sketch"), icon: "sketch", plainTitle: true,
                     intro: qsTr("You are drawing in a sketch. The tools are on the SKETCH tab at the top."),
                     sections: [{ title: qsTr("Draw the outline"), steps: [
                                      { title: qsTr("Draw a closed outline"), done: false,
                                        text: qsTr("Line: click the points, click the first point again to close it. Rectangle: drag two corners. Closed outlines are filled orange."),
                                        actions: [] },
                                      { title: qsTr("Finish"), done: false,
                                        text: qsTr("Click Finish Sketch (or Esc). Then select the sketch to make a part from it."),
                                        actions: [a("finishSketch")] }] }] }
        }
        if (r === "") {
            return { title: qsTr("Getting started"),
                     intro: qsTr("Nothing is selected. Import a scan to design on it, or start a new design from a sketch. Click a part in the view or in the Browser to see what you can do with it."),
                     sections: [{ title: "", steps: [], actions: [a("import"), a("newSketch"), a("box")] }] }
        }
        if (r === "mesh") {
            return { title: roleName(r),
                     intro: qsTr("A scan is made of many small triangles. To design on it, draw the outline of the new part on the scan, or turn an area into a smooth surface first."),
                     sections: [
                         { title: qsTr("Part on the scan (e.g. an aileron)"), steps: [
                               { title: qsTr("Draw the outline on the scan"), done: false,
                                 text: qsTr("Click the button, then click the scan where the part goes. Draw a closed outline and click Finish Sketch."),
                                 actions: [a("sketchOnScan")] },
                               { title: qsTr("Create the part"), done: false,
                                 text: qsTr("Select the sketch and choose Part on surface: it is cut out of the scan's shape and thickened."),
                                 actions: [] }] },
                         { title: qsTr("Fin on the scan (e.g. a canard)"), steps: [
                               { title: qsTr("Sketch the fin"), done: false,
                                 text: qsTr("Click the button, then click the scan where the fin stands. Draw its outline with the root a little past the orange surface line and click Finish Sketch."),
                                 actions: [a("sketchFin")] },
                               { title: qsTr("Create the fin"), done: false,
                                 text: qsTr("Set the thickness and click Create: the root is cut to the scan's shape."),
                                 actions: [] }] },
                         { title: qsTr("Smooth surface from an area"), steps: [
                               { title: qsTr("Select the area"), done: selected,
                                 text: selected
                                       ? qsTr("%1 triangles, %2 mm² selected. Grow or shrink it, or click again to add more.")
                                             .arg(Number(info.selectedTriangles).toLocaleString(Qt.locale(), "f", 0))
                                             .arg(Number(info.selectedArea ?? 0).toLocaleString(Qt.locale(), "f", 1))
                                       : qsTr("Pick a selection tool, then click or drag on the scan. Esc goes back to navigating."),
                                 actions: selected ? [a("growSelection"), a("shrinkSelection"), a("clearSelection")]
                                                   : [a("regionTool"), a("lassoTool"), a("brushTool")] },
                               { title: qsTr("Fit a smooth surface"), done: false,
                                 text: qsTr("Creates a new surface part from the selected area. It is selected for the next steps."),
                                 actions: [a("fitSurface")] }] },
                         { title: qsTr("More"), steps: [],
                           actions: [a("fitWhole"), a("cutWithThis"), a("toBody"), a("repair"), a("simplify"), a("scale"), a("export")] }] }
        }
        if (r === "surface") {
            const cut = isCutOut(info)
            const hasSketch = cut || sketches().length > 0
            return { title: roleName(r),
                     intro: qsTr("A smooth, exact surface fitted to the scan. Cut the part you need out of it and give it a thickness."),
                     sections: [
                         { title: qsTr("Make a part from this surface"), steps: [
                               { title: qsTr("Draw the outline"), done: hasSketch,
                                 text: hasSketch ? qsTr("A sketch exists. Draw another one if you need a different outline.")
                                                 : qsTr("Draw the closed outline of the part on the surface and click Finish Sketch."),
                                 actions: [a("sketchOnSurface")] },
                               { title: qsTr("Cut it out"), done: cut,
                                 text: cut ? qsTr("This surface is already a cut-out piece. Cut again to make it smaller.")
                                           : qsTr("Keeps only the piece of the surface inside the outline. The piece is a new part and is selected."),
                                 actions: [a("trim")] },
                               { title: qsTr("Thicken"), done: false,
                                 text: qsTr("Turns the piece into a solid with the thickness you type."),
                                 actions: [a("thicken")] }] },
                         { title: qsTr("More"), steps: [], actions: [...fitted, a("sketchFin"), a("offset"), a("cutWithThis"),
                               info.kind === "body" ? a("editStep") : a("scale"), a("export"), a("hide"), a("delete")] }] }
        }
        if (r === "sketch") {
            const onSurface = sketchSurface(info.featureId) > 0
            const fin = onSurface && sketchAcross(info.featureId)
            const steps = []
            if (fin)
                steps.push({ title: qsTr("Create the fin on the surface"), done: false,
                             text: qsTr("The sketch stands across a surface or scan. This gives the outline a thickness and cuts its root to the surface."),
                             actions: [a("finOnSurface")] })
            else if (onSurface)
                steps.push({ title: qsTr("Create the part on the surface"), done: false,
                             text: qsTr("The sketch was drawn on a surface or scan. This cuts the outline out of it and thickens it in one step."),
                             actions: [a("partOnSurface")] })
            steps.push({ title: onSurface ? qsTr("Or make a flat solid") : qsTr("Make a solid"), done: false,
                         text: qsTr("Extrude pulls the outline straight out; Revolve spins it around an axis."),
                         actions: [a("extrude"), a("revolve")] })
            return { title: roleName(r),
                     intro: qsTr("A 2D drawing. Closed outlines (profiles) can become solids."),
                     sections: [{ title: qsTr("Next step"), steps: steps, actions: [] },
                                { title: qsTr("More"), steps: [], actions: [a("editSketch"), a("hide"), a("delete")] }] }
        }
        // body / solid
        const editable = r === "body"
        return { title: roleName(r),
                 intro: editable ? qsTr("A solid in the design timeline. Add features to it; every step can be edited later in the timeline at the bottom.")
                                 : qsTr("An exact solid from a file or a fit. Make it editable to add fillets, holes and cuts, or export it as it is."),
                 sections: editable
                     ? [{ title: qsTr("Change the shape"), steps: [],
                          actions: [a("editStep"), a("sketchOnFace"), a("fillet"), a("chamfer"), a("shell"), a("hole"), a("combine")] },
                        { title: qsTr("More"), steps: [], actions: [a("export"), a("hide"), a("delete")] }]
                     : [{ title: qsTr("Use it"), steps: [
                              { title: qsTr("Export for printing or CAD"), done: false,
                                text: qsTr("STL or 3MF for a 3D printer, STEP for other CAD programs. For printing, every visible solid gets its own files."),
                                actions: [a("exportForPrint"), a("export")] },
                              { title: qsTr("Or keep designing"), done: false,
                                text: qsTr("Round or bevel its edges right away, or make it editable for holes and cuts; the changes appear in the timeline."),
                                actions: [a("fillet"), a("chamfer"), a("combine"), a("solidToBody")] }] },
                        { title: qsTr("More"), steps: [], actions: [...fitted, a("scale"), a("hide"), a("delete")] }] }
    }

    /// Every action for the selection, flat (context bar and right-click menu).
    function actions(info) {
        const g = guide(info)
        const out = []
        for (const s of g.sections) {
            for (const st of s.steps)
                for (const x of st.actions) out.push(x)
            for (const x of s.actions) out.push(x)
        }
        return out
    }

    /// The recommended next action: the first enabled primary one.
    function nextAction(info) {
        for (const x of actions(info))
            if (x.primary && x.enabled)
                return x
        return null
    }

    // --- Running -----------------------------------------------------------------------

    /// Hides the part a new result was made from: they overlap exactly, so the result
    /// would otherwise be hidden inside it. The Browser's eye icon shows it again.
    function _hideSource(r, pid) {
        if (r.ok && r.result && r.result.id && r.result.id !== pid)
            controller.execute("part.set_visible", { id: pid, visible: false })
    }

    function _selectResult(r) {
        if (r.ok && r.result && r.result.id && controller.parts.rowOf(r.result.id) >= 0)
            host.selectPart(r.result.id)
        return r
    }

    /// Runs action `id` on part `info` with the popup's values ({key: value}).
    function run(id, info, values) {
        const v = values || {}
        const pid = info.id
        switch (id) {
        case "import": return host.openImport()
        case "newSketch": return host.startCreateSketch()
        case "box": return host.openFeature("box")
        case "regionTool": return host.setSelectionTool("region")
        case "lassoTool": return host.setSelectionTool("lasso")
        case "brushTool": return host.setSelectionTool("brush")
        case "growSelection": return controller.execute("selection.grow", { id: pid, steps: 1 })
        case "shrinkSelection": return controller.execute("selection.grow", { id: pid, steps: -1 })
        case "clearSelection": return controller.execute("selection.clear", { id: pid })
        case "fitSurface": {
            const r = controller.execute("surface.fit_selection", { id: pid, tolerance: v.tolerance > 0 ? v.tolerance : 0.25 })
            if (r.ok) {
                // The area is done: back to navigating, and the scan's highlight no longer
                // covers the new surface.
                host.setSelectionTool("navigate")
                controller.execute("selection.clear", { id: pid })
                _hideSource(r, pid)
                const f = r.result.fit
                controller.showStatus(qsTr("Surface \"%1\" fitted: max deviation %2 mm (%3). The scan is hidden. Draw the outline of the part and click Finish Sketch.")
                    .arg(r.result.name).arg(Number(f.meshToSurfaceMax).toFixed(2))
                    .arg(f.withinTolerance ? qsTr("within tolerance") : qsTr("more than the tolerance")))
            }
            _selectResult(r)
            // The next step is always the outline: start the sketch on the new surface.
            if (r.ok && r.result.id)
                host.createSketch({ surface: { part: r.result.id } })
            return r
        }
        case "fitWhole": {
            const r = controller.execute("surface.fit_regions", { id: pid, tolerance: v.tolerance > 0 ? v.tolerance : 0.25,
                                                                   useSelection: (info.selectedTriangles ?? 0) > 0 })
            if (r.ok) {
                const f = r.result.fit
                let msg = qsTr("\"%1\": %2 patches, %3, max deviation %4 mm (%5).").arg(r.result.name).arg(f.faces)
                    .arg(f.solid ? qsTr("closed solid") : qsTr("open shell with %1 free edges").arg(f.freeEdges))
                    .arg(Number(f.meshToSurfaceMax).toFixed(2))
                    .arg(f.withinTolerance ? qsTr("within tolerance") : qsTr("MORE THAN THE TOLERANCE"))
                if (f.smoothSeams > 0)
                    msg += " " + qsTr("Smooth seams within %1°.").arg(Number(f.seamAngleMaxDeg).toFixed(2))
                if ((r.result.warnings ?? []).length > 0)
                    msg += " " + r.result.warnings.join(" ")
                controller.showStatus(msg)
            }
            return _selectResult(r)
        }
        case "fitDeviation": {
            const scan = fitSource(info)
            const tol = v.tolerance > 0 ? v.tolerance : 0.25
            // Scan points beyond a few tolerances are not on this fit (e.g. the rest of the
            // scan around a fitted area): they stay uncoloured.
            const r = controller.execute("inspect.deviation", { measured: scan, reference: pid, tolerance: tol,
                                                                range: 2 * tol, maxDistance: Math.max(1, 8 * tol) })
            if (r.ok) {
                // The coloured scan and the fit overlap exactly: show the scan, hide the fit.
                controller.execute("part.set_visible", { id: scan, visible: true })
                controller.execute("part.set_visible", { id: pid, visible: false })
                controller.showStatus(qsTr("Scan coloured by its distance to \"%1\" (hidden; V shows it again): %2% of the points within ±%3 mm, largest %4 mm. Inspect > Clear Analyses removes the colours.")
                    .arg(info.name).arg(Number(r.result.withinTolerance ?? 0).toFixed(1)).arg(tol)
                    .arg(Number(Math.max(Math.abs(r.result.stats.min), Math.abs(r.result.stats.max))).toFixed(3)))
            }
            return r
        }
        case "sketchOnScan":
            if ((info.selectedTriangles ?? 0) > 0)
                return host.createSketch({ surface: { part: pid, useSelection: true } })
            return host.startCreateSketch(false)
        case "sketchOnSurface":
            return host.createSketch({ surface: { part: pid } })
        case "sketchFin":
            return host.startCreateSketch(true)
        case "toBody":
        case "solidToBody": {
            const r = controller.execute("feature.base_body", { part: pid })
            if (r.ok && r.result.bodies && r.result.bodies.length)
                host.selectBodyOfFeature(r.result.id)
            return r
        }
        case "repair": return controller.execute("mesh.repair", { id: pid })
        case "simplify": return controller.execute("mesh.decimate", { id: pid, targetTriangles: Math.max(4, Math.round(v.targetTriangles)) })
        case "scale": return v.percent > 0 && v.percent !== 100 ? controller.execute("part.scale", { id: pid, factor: v.percent / 100 }) : null
        case "trim": {
            const r = controller.execute("surface.trim_by_sketch", { id: pid, sketch: v.sketch, keep: v.keep || "inside" })
            _hideSource(r, pid)
            if (r.ok)
                controller.showStatus(qsTr("Cut out \"%1\" (%2 cm²); the full surface is hidden. Next: thicken it.").arg(r.result.name)
                                      .arg((r.result.area / 100).toFixed(2)))
            return _selectResult(r)
        }
        case "thicken": {
            const r = controller.execute("surface.thicken", { id: pid, thickness: v.thickness, side: v.side || "outside" })
            _hideSource(r, pid)
            if (r.ok)
                controller.showStatus(qsTr("Solid \"%1\": %2 mm thick, volume %3 cm³; the surface piece is hidden. Next: export it.")
                                      .arg(r.result.name).arg(v.thickness).arg((r.result.volume / 1000).toFixed(2)))
            return _selectResult(r)
        }
        case "offset": return _selectResult(controller.execute("surface.offset", { id: pid, distance: v.distance }))
        case "partOnSurface": {
            const r = controller.execute("surface.part_from_sketch", { sketch: info.featureId, thickness: v.thickness, side: v.side || "inside" })
            if (r.ok)
                controller.showStatus(qsTr("Part \"%1\" created on the surface. Hide the scan (eye icon in the Browser) to see it on its own. Next: export it.")
                                      .arg(r.result.name))
            return _selectResult(r)
        }
        case "finOnSurface": {
            const r = controller.execute("surface.fin_from_sketch", { sketch: info.featureId, thickness: v.thickness, rootFillet: v.rootFillet || 0,
                                                                          count: Math.max(1, Math.round(v.count || 1)), spacing: v.spacing || 40,
                                                                          mirror: !!v.mirror && v.mirror !== "no", mirrorPlane: v.mirror && v.mirror !== "no" ? v.mirror : "auto" })
            if (r.ok && r.result.warnings.length > 0)
                controller.showStatus(qsTr("Fin \"%1\": %2").arg(r.result.name).arg(r.result.warnings[0]))
            else if (r.ok)
                controller.showStatus(qsTr("Fin \"%1\": %2 mm thick, %3 cm³. Next: export it.")
                                      .arg(r.result.name).arg(v.thickness).arg((r.result.volume / 1000).toFixed(1)))
            return _selectResult(r)
        }
        case "finishSketch": return host.finishSketch(true)
        case "editSketch": return host.editFeature({ type: "sketch", id: info.featureId })
        case "editStep": {
            const f = ownFeature(info)
            return f ? host.editFeature(f) : null
        }
        case "extrude": return host.openFeature("extrude")
        case "revolve": return host.openFeature("revolve")
        case "sketchOnFace": return host.startCreateSketch()
        case "fillet": return host.openFeature("fillet")
        case "chamfer": return host.openFeature("chamfer")
        case "shell": return host.openFeature("shell")
        case "hole": return host.openFeature("hole")
        case "combine": return host.openFeature("combine")
        case "cutWithThis": return host.openFeature("combine")
        case "export": return host.openExport([pid])
        case "exportForPrint": return host.openPrintExport()
        case "moveRotate": {
            if (info.kind === "body")
                return host.openFeature("move")
            const params = { id: pid }
            const angle = Number(v.angle) || 0
            if (angle !== 0) {
                params.axis = v.axis === "x" ? [1, 0, 0] : v.axis === "y" ? [0, 1, 0] : [0, 0, 1]
                params.angle = angle
            }
            const t = [Number(v.dx) || 0, Number(v.dy) || 0, Number(v.dz) || 0]
            if (t.some(c => c !== 0))
                params.translate = t
            return params.angle !== undefined || params.translate ? controller.execute("part.transform", params) : null
        }
        case "zoomTo": return host.zoomToPart(pid)
        case "hide": return controller.execute("part.set_visible", { id: pid, visible: !info.visible })
        case "delete": return host.deleteSelected()
        }
        return null
    }
}
