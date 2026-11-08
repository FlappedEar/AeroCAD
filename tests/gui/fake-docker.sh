#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Stand-in for the docker CLI in GUI tests (AEROCAD_DOCKER): reports a running engine with the
# OpenFOAM image, and a "run" that only sleeps, so a wind tunnel run stays running without
# OpenFOAM.
case "$1" in
  info) echo 4 ;;
  image) echo sha256:0 ;;
  run) exec sleep 20 ;;
esac
exit 0
