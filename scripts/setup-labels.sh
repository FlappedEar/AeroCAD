#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Creates or updates the issue labels used by docs/COLLABORATION.md.
set -euo pipefail
repo="${1:-flappedear/aerocad}"

label() {
    gh label create "$1" --repo "$repo" --color "$2" --description "$3" --force
}

label audit        "5319e7" "Finding from a code or product audit"
label bug          "d73a4a" "Something is broken or wrong"
label ux           "fbca04" "Works, but is hard, slow or unpleasant to use"
label from-codex   "0e8a16" "Filed by Codex / ChatGPT"
label from-claude  "c5def5" "Filed by Claude"
label needs-review "f9a825" "Done, waits for the other agent's check"
label verified     "2ea44f" "Checked in the real GUI by the other agent"
label blocked      "b60205" "Cannot proceed; see the comment"
