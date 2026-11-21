# Troubleshooting

| Problem | What to do |
|---|---|
| I cannot see the model. | Press ++f6++. Check the eye icon in the Browser. |
| The scan is tiny or huge. | The file has no unit. Use **It is in metres (×1000)** (or the matching button) in the guide, or **Scale**. See [step 1](step-1-scan.md#check-the-size-units). |
| Left drag does not rotate. | By design: the left button selects. Right drag or ++shift++ + middle drag orbits, or click **Orbit** in the navigation bar. |
| A single-letter shortcut does nothing. | A text field has focus. Click in the 3D view first. |
| **Sketch on the scan** is disabled or nothing happens when I click. | Select the scan first. In the *Create Sketch* panel click the scan or part itself, not an empty place. |
| The **Part on surface** panel did not appear (or I clicked *Not now*). | The outline must be closed and the sketch drawn on a scan or surface. Select the sketch or the scan and click **Part on surface** in the guide. |
| I cannot see the part I created. | The scan and the part overlap. Hide the scan (eye icon, or ++v++ on it) or give the part a colour with ++a++. Make the part thicker or choose **Outward**. |
| *Approximation: the surface is up to X mm from the scan...* | See [step 3](step-3-part.md#the-approximation-warning). The part is kept; check the 99 % figure and the outline. |
| The part looks wrong at the edge of the scan. | The outline reaches past the scan. Edit the sketch (double-click it in the timeline) and keep the outline on the scan. |
| A step in the timeline is red. | Hover it for the reason. A later step broke after an earlier change; edit the earlier step or suppress the red one (right-click). |
| *Sketch on a face* does not start a sketch. | Click the part (the body), not the scan behind it, after pressing the button. |
| Fillet says a radius does not fit. | The message names the largest radius that works; use a smaller one. |
| **Run CFD** is disabled, or the panel says Docker is not running. | Press **Check**. On a Mac use **Install for me** until the dot is green, or run `colima start`. On Linux start Docker. See [Install and run](install.md#the-wind-tunnel-engine-docker). |
| The arrows of the air hit the back of the car. | Change **Air flows** in the panel until the arrows hit the front. |
| The panel warns about open edges of the part. | An open shell is not a solid. Close it with **MESH > Scan Tools > Repair** (increase *Fill holes up to*), or run only the closed part. |
| The results say *checks: warning*. | Read the lines under the badge. A coarse mesh often is *not converged to the preset targets*; use it for A/B comparison, not for absolute numbers. Try the medium mesh. |
| A run was stopped when I closed the program. | Runs continue in the background and are picked up at the next start. A queued run that never started is reported as stopped. |
| **Export for printing** shows *Check before export*. | See [step 7](step-7-export.md#the-export-check). **Show Thin Walls** shows where; **Export Anyway** writes the files as they are. A part that is not a closed solid blocks the print export (no *Export Anyway*): close it first. |
| The file asks me to *Recover unsaved changes?* at start. | The program did not close properly. Recover brings back the autosaved copy. |

## Still stuck?

- Select the part: the guide in Properties says what is possible and why a button is disabled.
- The status bar and the tooltip of a toolbar button explain the command.
- Reference pages: [Scans](../scans.md), [CAD](../cad.md), [Wind tunnel](../cfd.md).
- Report a problem at [github.com/flappedear/aerocad/issues](https://github.com/flappedear/aerocad/issues).
