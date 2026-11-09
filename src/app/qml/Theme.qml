// SPDX-License-Identifier: GPL-3.0-or-later
// AeroCAD design tokens: colours (light and dark), typography, spacing, radii and icons.
// Every QML file takes its colours and sizes from here instead of hard-coding them, so the
// whole window switches between light and dark together. See docs/ui.md, "Design system".
pragma Singleton
import QtQuick
import AeroCAD

QtObject {
    id: theme

    // --- Mode -------------------------------------------------------------------------

    /// Preferences > Theme: "system", "light" or "dark".
    readonly property string mode: Preferences.theme
    /// The platform's appearance. Qt.styleHints.colorScheme exists from Qt 6.5
    /// (Qt::ColorScheme::Dark == 2); older Qt falls back to the platform palette.
    readonly property bool systemDark: Qt.styleHints.colorScheme !== undefined
        ? Qt.styleHints.colorScheme === 2
        : _systemPalette.window.hslLightness < 0.5
    readonly property bool dark: mode === "dark" || (mode !== "light" && systemDark)

    readonly property SystemPalette _systemPalette: SystemPalette { colorGroup: SystemPalette.Active }

    function pick(light, darkValue) { return theme.dark ? darkValue : light }

    // --- Colours ----------------------------------------------------------------------
    // Neutral greys with a slight blue tint; one accent colour for selection, focus,
    // checked tools and the timeline marker.

    /// Window chrome: toolbar, status bar, timeline.
    readonly property color chrome: pick("#eef0f3", "#1e2125")
    /// Panels (Browser, Properties) and floating palettes.
    readonly property color surface: pick("#f8f9fb", "#26292e")
    /// Slightly raised elements on a surface: section headers, chips.
    readonly property color surfaceRaised: pick("#ffffff", "#2e3238")
    /// Text fields and other inputs.
    readonly property color field: pick("#ffffff", "#1b1d21")
    readonly property color border: pick("#d9dde3", "#363a41")
    readonly property color borderStrong: pick("#c3c9d1", "#4a4f58")
    readonly property color separator: pick("#e3e6ea", "#30343a")

    readonly property color text: pick("#1c2026", "#e7e9ec")
    readonly property color textMuted: pick("#5c636d", "#a4abb4")
    readonly property color textFaint: pick("#8c939d", "#6f767f")
    readonly property color icon: pick("#3a4049", "#c9ced5")

    readonly property color accent: pick("#2463eb", "#4d8dff")
    readonly property color accentHover: pick("#1d55d1", "#6aa0ff")
    readonly property color accentText: "#ffffff"
    /// Background of checked tools and selected rows.
    readonly property color accentSoft: pick(Qt.rgba(0.14, 0.39, 0.92, 0.12), Qt.rgba(0.30, 0.55, 1.0, 0.20))
    readonly property color hover: pick(Qt.rgba(0, 0, 0, 0.055), Qt.rgba(1, 1, 1, 0.07))
    readonly property color pressed: pick(Qt.rgba(0, 0, 0, 0.10), Qt.rgba(1, 1, 1, 0.12))

    readonly property color danger: pick("#c5221f", "#f2766d")
    readonly property color dangerSoft: pick("#fbe3e1", "#4a2523")
    readonly property color success: pick("#1e7e34", "#4cc27a")
    readonly property color warning: pick("#b45309", "#f0a640")

    readonly property color tooltip: pick("#2b3038", "#f0f2f5")
    readonly property color tooltipText: pick("#ffffff", "#1c2026")
    readonly property color shadow: pick(Qt.rgba(0, 0, 0, 0.14), Qt.rgba(0, 0, 0, 0.45))

    // --- 3D view ----------------------------------------------------------------------

    readonly property color viewportTop: pick("#f9fafc", "#474d56")
    readonly property color viewportBottom: pick("#c9d1db", "#1f2226")
    readonly property color gridMinor: pick(Qt.rgba(0.20, 0.25, 0.32, 0.10), Qt.rgba(1, 1, 1, 0.06))
    readonly property color gridMajor: pick(Qt.rgba(0.20, 0.25, 0.32, 0.22), Qt.rgba(1, 1, 1, 0.13))
    readonly property color bodyColor: pick("#b5bcc5", "#aab1ba")
    readonly property color meshColor: pick("#c6cad0", "#b8bcc2")
    readonly property color edgeColor: pick("#242a31", "#14171b")
    readonly property color selectedColor: pick("#6ea4ff", "#78a9ff")
    readonly property color hoverColor: pick("#9cc0ff", "#9cc0ff")
    /// Laid over a part's colour under the pointer: lighter than the selection, still grey.
    readonly property color hoverTint: Qt.rgba(0.43, 0.64, 1.0, 0.4)
    readonly property color sketchColor: pick("#1f5fd6", "#62a0ff")
    readonly property color sketchProfile: Qt.rgba(1.0, 0.72, 0.38, 0.40)
    /// Construction planes: translucent orange squares, like Fusion.
    readonly property color planeFill: Qt.rgba(0.95, 0.62, 0.25, 0.22)
    readonly property color planeSelected: Qt.rgba(0.5, 0.7, 1.0, 0.3)
    readonly property color planeHover: Qt.rgba(0.6, 0.78, 1.0, 0.22)
    readonly property color planeEdge: pick("#c77a2a", "#e0954a")
    readonly property color regionColor: "#ff9a3c"
    /// Overlay palettes drawn over the 3D view (navigation bar, selection tools).
    readonly property color overlay: pick(Qt.rgba(1, 1, 1, 0.92), Qt.rgba(0.15, 0.16, 0.18, 0.92))
    /// Chart series (simulation histories), in order of use.
    readonly property color chart1: pick("#1f6fc4", "#5aa3f0")
    readonly property color chart2: pick("#c9302c", "#f07a6f")
    readonly property color chart3: pick("#2c8a3e", "#5fcb7a")
    readonly property color chart4: pick("#7b4bb8", "#b08de6")
    readonly property color chart5: pick("#d97706", "#f5ad4e")
    readonly property color chart6: pick("#0e8a99", "#4fc6d3")
    /// Chart grid lines.
    readonly property color chartGrid: pick(Qt.rgba(0, 0, 0, 0.12), Qt.rgba(1, 1, 1, 0.12))
    /// Rows of the two runs (or variants) being compared.
    readonly property color compareA: pick(Qt.rgba(0.14, 0.39, 0.92, 0.14), Qt.rgba(0.30, 0.55, 1.0, 0.22))
    readonly property color compareB: pick(Qt.rgba(0.85, 0.47, 0.02, 0.16), Qt.rgba(0.96, 0.68, 0.30, 0.22))

    // --- Typography -------------------------------------------------------------------
    // Pixel sizes; the platform UI font (SF Pro on macOS, Segoe UI on Windows, the desktop
    // font on Linux) is used throughout.

    readonly property int fontCaption: 10   // ribbon group captions, badges (upper case)
    readonly property int fontSmall: 11     // secondary text, ribbon labels, status bar
    readonly property int fontBody: 13      // default
    readonly property int fontTitle: 15     // dialog and panel titles
    readonly property real captionSpacing: 0.6

    // --- Spacing, sizes, radii --------------------------------------------------------
    // 4 px grid.

    readonly property int space1: 4
    readonly property int space2: 8
    readonly property int space3: 12
    readonly property int space4: 16
    readonly property int space6: 24

    readonly property int radiusSmall: 4
    readonly property int radius: 6
    readonly property int radiusLarge: 10

    readonly property int iconSmall: 16
    readonly property int iconMedium: 20
    readonly property int iconLarge: 24

    readonly property int controlHeight: 28
    readonly property int rowHeight: 30
    readonly property int panelHeaderHeight: 32
    readonly property int statusBarHeight: 26

    readonly property int animationFast: 90

    // --- Icons ------------------------------------------------------------------------

    /// URL of a monochrome SVG icon from src/app/icons; tint it with icon.color.
    function iconUrl(name) {
        return name ? "qrc:/qt/qml/AeroCAD/icons/" + name + ".svg" : ""
    }

    /// Icon for a toolbar command by its label, so commands added with only a text get a
    /// matching icon. New commands should set ToolCommand.iconName explicitly.
    readonly property var commandIcons: ({
        "undo": "undo", "redo": "redo", "import": "import", "open": "open", "save": "save",
        "to body": "mesh-to-body", "sketch": "sketch", "extrude": "extrude", "revolve": "revolve",
        "box": "box", "cylinder": "cylinder", "sphere": "sphere", "torus": "torus", "hole": "hole", "line": "line", "rectangle": "rectangle",
        "circle": "circle", "arc": "arc", "center rectangle": "center-rectangle", "center arc": "center-arc", "polygon": "polygon", "slot": "slot", "spline": "spline", "dimension": "dimension",
        "finish sketch": "finish-sketch", "press pull": "extrude", "fillet": "fillet", "chamfer": "chamfer", "shell": "shell",
        "combine": "combine", "split body": "section", "delete face": "delete", "draft": "shell", "parameters": "parameters", "show/hide": "visible", "delete": "delete",
        "selected": "export", "all": "export", "export": "export", "fit": "fit", "home": "home",
        "ortho": "ortho", "sweep": "sweep", "loft": "loft", "mirror": "mirror", "pattern": "pattern",
        "circular pattern": "circular-pattern", "plane": "plane", "midplane": "plane", "measure": "measure",
        "clearance": "clearance", "section": "section", "deviation": "deviation",
        "thickness": "thickness", "a/b": "variants", "new": "duplicate", "variant": "variants"
    })

    function commandIcon(label) {
        const key = String(label).toLowerCase().replace(/&/g, "")
        return commandIcons[key] || "tool"
    }

    /// One-line explanation of a toolbar command by its label, shown in its tooltip under
    /// the name, so the ribbon explains itself. Commands not listed show only their name.
    readonly property var commandHelps: ({
        "sketch": qsTr("Draw a 2D outline on a plane, a flat face, a surface or a scan."),
        "extrude": qsTr("Pull a closed sketch outline straight out into a solid, or cut with it."),
        "revolve": qsTr("Spin a closed sketch outline around an axis into a solid."),
        "hole": qsTr("Drill a round hole into a body at a point on a face."),
        "box": qsTr("Add a block whose size you type in."),
        "cylinder": qsTr("Add a cylinder whose size you type in."),
        "sphere": qsTr("Add a ball whose diameter you type in."),
        "torus": qsTr("Add a ring (doughnut) whose ring and tube diameters you type in."),
        "press pull": qsTr("Move faces of a body in or out by a distance; a curved face is offset (Fusion: Press Pull, Q)."),
        "fillet": qsTr("Round the selected edges of a body."),
        "chamfer": qsTr("Bevel (cut off at an angle) the selected edges of a body."),
        "shell": qsTr("Hollow out a body, leaving walls of the thickness you type."),
        "move/rotate": qsTr("Turn the selected part about an axis and move it by a distance (Fusion: Move/Copy, M)."),
        "combine": qsTr("Join, cut or intersect two bodies."),
        "midplane": qsTr("Add a construction plane halfway between two parallel faces or planes, e.g. a part's symmetry plane (Fusion: Midplane)."),
        "draft": qsTr("Tilt faces of a body by an angle from a fixed plane, e.g. mould walls (Fusion: Draft)."),
        "delete face": qsTr("Remove faces of a body (a hole, a fillet, a boss) and close the gap from the faces around them (Fusion: Delete)."),
        "split body": qsTr("Cut a body with a plane into two bodies, e.g. to print a part larger than the printer bed (Fusion: Split Body)."),
        "parameters": qsTr("Named values (e.g. wall = 2 mm) that dimensions can use; change one to update the design."),
        "delete": qsTr("Delete the selected part. Undo brings it back."),
        "import": qsTr("Open a scan or model: STL, OBJ, PLY, 3MF or STEP."),
        "to body": qsTr("Turn the selected scan or solid into an editable body in the timeline."),
        "selected": qsTr("Export the selected part to STL, 3MF or STEP."),
        "all visible": qsTr("Export every visible part to one file."),
        "a/b": qsTr("Switch between two design variants to compare them."),
        "new": qsTr("Start a new design variant from the current one."),
        "finish sketch": qsTr("Close the sketch and go back to the 3D design."),
        "line": qsTr("Click points to draw connected lines; click the first point to close the outline."),
        "rectangle": qsTr("Drag two corners."),
        "center rectangle": qsTr("Click the centre, then a corner; the rectangle is symmetric about the centre."),
        "center arc": qsTr("Click the centre, the start (sets the radius) and the end; the arc runs counter-clockwise."),
        "circle": qsTr("Click the centre, then the radius."),
        "arc": qsTr("Click start, end and a point on the arc."),
        "polygon": qsTr("Regular polygon: click the centre, then a corner."),
        "slot": qsTr("Rounded slot: click the two centres, then the width."),
        "spline": qsTr("Smooth curve through the points you click; Enter ends it, the first point closes it."),
        "dimension": qsTr("Click a line, circle or two points to fix a size; type the value."),
        "measure": qsTr("Measure distances and angles between points, edges and faces."),
        "section": qsTr("Cut the view with a plane to look inside."),
        "deviation": qsTr("Colour a part by its distance from the scan."),
        "thickness": qsTr("Colour a part by its wall thickness."),
        "clearance": qsTr("Colour where two parts come closer than a gap you choose.")
    })

    function commandHelp(label) {
        return commandHelps[String(label).toLowerCase().replace(/&/g, "")] || ""
    }

    /// Icon of a timeline feature type or Browser part kind.
    function featureIcon(type) {
        const map = {
            sketch: "sketch", extrude: "extrude", revolve: "revolve", fillet: "fillet",
            chamfer: "chamfer", shell: "shell", combine: "combine", box: "box", cylinder: "cylinder", sphere: "sphere", torus: "torus",
            hole: "hole", base_body: "mesh-to-body", sweep: "sweep", loft: "loft", mirror: "mirror",
            pattern: "pattern", rectangular_pattern: "pattern", circular_pattern: "circular-pattern",
            plane: "plane", body: "body", imported: "mesh", mesh: "mesh",
            part_on_surface: "surface", fin_on_surface: "surface", move: "move", press_pull: "extrude", split: "section", delete_faces: "delete", draft: "shell"
        }
        return map[type] || "tool"
    }
}
