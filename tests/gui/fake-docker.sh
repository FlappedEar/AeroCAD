#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Stand-in for the docker CLI in GUI tests (AEROCAD_DOCKER): reports a running engine with the
# OpenFOAM image, and a "run" that only sleeps, so a wind tunnel run stays running without
# OpenFOAM. With FAKE_DOCKER_HISTORY=1 the run first writes 20 iterations of coefficient and
# force history into the mounted case, as a solver part-way through would.
case "$1" in
  info) echo 4 ;;
  image) echo sha256:0 ;;
  run)
    if [ -n "$FAKE_DOCKER_HISTORY" ]; then
      case_dir=""
      prev=""
      for arg in "$@"; do
        if [ "$prev" = "-v" ]; then
          case "$arg" in *:/case) case_dir="${arg%:/case}" ;; esac
        fi
        prev="$arg"
      done
      if [ -n "$case_dir" ]; then
        mkdir -p "$case_dir/postProcessing/forceCoeffs/0" "$case_dir/postProcessing/forces/0"
        echo solve > "$case_dir/aerocad.stage"
        {
          echo "# Time Cd Cl Cs"
          i=1; while [ $i -le 20 ]; do echo "$i 0.8 0.1 0.01"; i=$((i + 1)); done
        } > "$case_dir/postProcessing/forceCoeffs/0/coefficient.dat"
        {
          echo "# Time total_x total_y total_z pressure_x viscous_x"
          i=1; while [ $i -le 20 ]; do echo "$i 1.5 0.01 0.2 1.4 0.1"; i=$((i + 1)); done
        } > "$case_dir/postProcessing/forces/0/force.dat"
        # FAKE_DOCKER_FINISH=1 (CAD-205): a finished run whose last iteration differs from
        # the mean of the last 20 % (Cd 0.8 / 0.9, drag 1.5 / 1.7 N in turn).
        if [ -n "$FAKE_DOCKER_FINISH" ]; then
          {
            echo "# Time Cd Cl Cs"
            i=1; while [ $i -le 20 ]; do
              if [ $((i % 2)) -eq 0 ]; then echo "$i 0.9 0.1 0.01"; else echo "$i 0.8 0.1 0.01"; fi
              i=$((i + 1))
            done
          } > "$case_dir/postProcessing/forceCoeffs/0/coefficient.dat"
          {
            echo "# Time total_x total_y total_z pressure_x viscous_x"
            i=1; while [ $i -le 20 ]; do
              if [ $((i % 2)) -eq 0 ]; then echo "$i 1.7 0.01 0.2 1.6 0.1"; else echo "$i 1.5 0.01 0.2 1.4 0.1"; fi
              i=$((i + 1))
            done
          } > "$case_dir/postProcessing/forces/0/force.dat"
          echo finished > "$case_dir/aerocad.status"
          exit 0
        fi
      fi
    fi
    exec sleep 20 ;;
esac
exit 0
