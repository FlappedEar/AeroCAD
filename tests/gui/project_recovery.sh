#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# CAD-226: crash recovery end to end. A first AeroCAD saves "wing", changes it, isolates a part,
# writes the recovery copy and is killed; a second one must offer the copy by design name and
# time (not the copy's path) and bring it back with the camera and the isolation.
# Usage: project_recovery.sh <AeroCAD binary> <source dir> <work dir>
set -u
app=$1
src=$2
work=$3
rm -rf "$work"
mkdir -p "$work/data"
cd "$work" || exit 1
XDG_DATA_HOME="$work/data"
XDG_CONFIG_HOME="$work/data"
export XDG_DATA_HOME XDG_CONFIG_HOME
"$app" "$src/tests/data/box_200x80x60.stl" --ui-script "$src/tests/gui/project_recovery_crash.ui" >crash.log 2>&1 &
pid=$!
i=0
while [ $i -lt 120 ]; do
    grep -q "AUTOSAVED" crash.log && break
    sleep 0.5
    i=$((i + 1))
done
kill -9 "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
if ! grep -q "AUTOSAVED true" crash.log; then
    cat crash.log
    echo "project_recovery: no recovery copy was written"
    exit 1
fi
exec "$app" --ui-script "$src/tests/gui/project_recovery_check.ui"
