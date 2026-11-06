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
| Import (Open) | Ctrl+O | Cmd+O | yes (Open) |
| Undo | Ctrl+Z | Cmd+Z | yes |
| Redo | Ctrl+Y | Cmd+Shift+Z | yes |
| Toggle visibility of the selected part | V | V | yes |
| Delete the selected part | Del | Delete (Backspace) or Fn+Delete | yes (Del) |
| Fit all | F6 | F6 | Fusion lists F6 "Zoom to Fit" for the Electronics workspace |
| Show/hide ViewCube | Ctrl+Alt+V | Option+Cmd+V | yes |
| Show/hide Browser | Ctrl+Alt+B | Option+Cmd+B | yes |
| Show/hide navigation bar | Ctrl+Alt+N | Option+Cmd+N | yes |
| Reset to default layout | Ctrl+Alt+R | Option+Cmd+R | yes |
| Region selection: smooth region (click) | 1 | 1 | Fusion: Window selection |
| Region selection: lasso (freeform) | 2 | 2 | yes (Freeform selection) |
| Region selection: brush (paint) | 3 | 3 | yes (Paint selection) |
| Back to Navigate (leave a selection tool) | Esc | Esc | yes (Esc cancels) |
| Brush smaller / larger | [ / ] | [ / ] | no |

Single-letter keys follow Fusion so that they stay free for the matching tools later
(for example E = Extrude, F = Fillet, L = Line). 1/2/3 select the region selection tools,
see "Region selection" in [macos.md](macos.md).
