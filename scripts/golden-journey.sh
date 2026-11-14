#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# CAD-294: run the golden journey (tests/gui/golden_journey.ui) against a build and print one
# line per step: PASS, GAP <issue> (not possible yet) or FAIL. Exit status 1 on any FAIL.
# Usage: scripts/golden-journey.sh [build-dir] [scan]   (default: build/release, the dome scan)
# On Linux it needs a display: use DISPLAY=:99 with Xvfb, or run it under xvfb-run -a.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build/release}
scan=${2:-$root/tests/data/models/dome_scan.stl}
export LIBGL_ALWAYS_SOFTWARE=${LIBGL_ALWAYS_SOFTWARE:-1}
out=$(mktemp)
timeout 300 "$build/bin/AeroCAD" "$scan" --ui-script "$root/tests/gui/golden_journey.ui" >"$out" 2>&1
status=$?
grep -E '^(GJ |ui-script: FAIL)' "$out"
rm -f "$out"
exit "$status"
