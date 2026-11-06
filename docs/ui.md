# User interface and shortcuts

AeroCAD's window follows the layout of common CAD applications, Autodesk Fusion in particular:

- **Toolbar** (top): commands grouped with captions (EDIT, INSERT, MODIFY, VIEW).
- **Browser** (left): the parts of the document.
- **Canvas** (centre): the 3D view, with the **ViewCube** in the top-right corner and the
  **navigation bar** at the bottom centre.
- **Properties** (right) and the **status bar** (bottom).

Keyboard shortcuts and mouse navigation use Fusion's defaults so that Fusion users feel at home.
Sources: [Autodesk Fusion keyboard shortcuts](https://www.autodesk.com/shortcuts/fusion-360),
[Fusion 360 Camera Controls](https://help.autodesk.com/cloudhelp/2019/ENU/MSHMXR/files/GUID-61EE8C7A-F896-4C26-86B9-450247945FD1.htm),
[ViewCube reference](https://help.autodesk.com/cloudhelp/ENU/Fusion-GetStarted/files/GS-VIEWCUBE.htm),
[Fusion navigation (touchpad)](https://help.autodesk.com/cloudhelp/ENU/Fusion-GetStarted/files/GUID-7B742BB2-65B3-4ADA-9B11-9D57E1E31292.htm).

## Mouse and touchpad

| Action | Mouse | Touchpad / Magic Mouse |
|---|---|---|
| Pan | Middle drag | Two-finger swipe |
| Orbit | Shift + middle drag | Shift + two-finger swipe |
| Zoom | Wheel | Pinch |
| Select | Left click | Click |

The left button is reserved for selection. The navigation bar's **Orbit**, **Pan** and **Zoom**
tools temporarily map left drag to that navigation (useful without a middle button); click the
tool again to turn it off.

Not implemented yet: Fusion's "orbit around point" (Shift + click + middle drag) and the
alternative navigation schemes in Fusion's preferences.

## ViewCube

- Click a **face** for that orthographic standard view (Front, Back, Left, Right, Top, Bottom).
- Click an **edge** or a **corner** for the view between the adjacent faces (corners give
  isometric views).
- **Drag** the cube to orbit.
- The **house** icon goes to the Home view (default: isometric from front-right-top), framed to fit.
- **Right-click**: Go Home, Orthographic / Perspective, Set current view as Home, Reset Home.

Model space is Z-up: Front looks along +Y, Right along -X, Top along -Z.

## Keyboard shortcuts

Qt maps Ctrl to Command and Alt to Option on macOS, so "Ctrl+Alt+V" is Option+Command+V on a Mac.

| Command | Windows / Linux | macOS | Fusion default? |
|---|---|---|---|
| New Design | Ctrl+N | Cmd+N | yes |
| Open (projects and 3D files) | Ctrl+O | Cmd+O | yes |
| Save | Ctrl+S | Cmd+S | yes (Save Version) |
| Recovery Save (write the autosave copy now) | Ctrl+Shift+S | Cmd+Shift+S | yes |
| Undo | Ctrl+Z | Cmd+Z | yes |
| Redo | Ctrl+Y | Cmd+Shift+Z | yes |
| Toggle visibility of the selected part | V | V | yes |
| Delete the selected part | Del | Delete (Backspace) or Fn+Delete | yes (Del) |
| Fit all | F6 | F6 | Fusion lists F6 "Zoom to Fit" for the Electronics workspace |
| Show/hide ViewCube | Ctrl+Alt+V | Option+Cmd+V | yes |
| Show/hide Browser | Ctrl+Alt+B | Option+Cmd+B | yes |
| Show/hide navigation bar | Ctrl+Alt+N | Option+Cmd+N | yes |
| Reset to default layout | Ctrl+Alt+R | Option+Cmd+R | yes |

Modelling tools (single letters are Fusion's defaults; they do not fire while a text field
has focus):

| Command | Key | Fusion default? |
|---|---|---|
| Extrude | E | yes |
| Fillet | F | yes |
| Hole | H | yes |
| Line (in a sketch) | L | yes |
| 2-point Rectangle (in a sketch) | R | yes |
| Center Diameter Circle (in a sketch) | C | yes |
| Cancel the current curve / dialog, then leave the sketch | Esc | yes |

Save As, Import and the variant commands (Modify > Duplicate as New Variant, Toggle A/B)
have no shortcut (Fusion's list has no Save As).

Not mapped yet: Press Pull (Q), Move (M), Trim (T), Offset (O), Project (P),
Sketch Dimension (D), Construction (X), Measure (I) and selection modes 1/2/3.

## Modelling

See [cad.md](cad.md) for the timeline and the commands behind the tools.

- **Create Sketch**: click a planar face of a body, or pick XY / XZ / YZ. The view turns to
  look at the sketch. Tools: Line (chains; right-click or Esc ends), Rectangle, Circle, Arc
  (start, end, point on arc), Polygon, Slot. Points snap to existing points and to a grid
  that follows the zoom. While drawing, type a number (or `width, height` for rectangles)
  and press Enter to set the size; expressions such as `w / 2` keep the sketch parametric.
  **Finish Sketch** or Esc leaves the sketch.
- **Extrude (E)**, Revolve, Box, Cylinder, **Fillet (F)**, Chamfer, Shell, Combine open a
  dialog in the top-left corner. Selections are made in the view while the dialog is open:
  click inside sketch profiles (Extrude/Revolve; default: all profiles), near edges
  (Fillet/Chamfer), on faces (Shell) or at the hole position (Hole). Numbers accept expressions.
- **Timeline** (bottom): features in order; double-click to edit (sketches reopen in the
  sketch environment), right-click for Rename, Suppress, Roll Back To Here and Delete; the
  playback buttons move the marker. Failed features are red with the reason in the tooltip.
- **Parameters**: Modify > Change Parameters, user parameters used in expressions.
- Command scripts: `AeroCAD --run script.json` runs a JSON array of `{name, params}`
  commands at start-up, the same calls an AI agent makes through MCP.
