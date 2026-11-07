#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sets up another machine as the AeroCAD wind tunnel engine (OpenFOAM in Docker, reached
# over SSH), so simulations do not load this computer. Run it on the computer with AeroCAD:
#
#   scripts/setup-remote-engine.sh user@host [port]
#
# It installs the docker command line tool here if needed (Homebrew), creates an SSH key
# and copies it to the other machine (asks for that machine's password once), checks that
# Docker runs there and that this user may use it (fixes PATH on macOS, starts Docker if it
# is stopped), downloads the OpenFOAM image there, runs a short OpenFOAM check and saves
# the engine in AeroCAD (~/.aerocad/cfd/engine.json). Safe to run again.
set -euo pipefail

IMAGE="opencfd/openfoam-default:2506"

if [ $# -lt 1 ] || [[ "$1" != *@* ]]; then
    echo "Usage: $0 user@host [port]" >&2
    exit 2
fi
TARGET="$1"
PORT="${2:-22}"
HOST_URL="ssh://$TARGET"
[ "$PORT" != "22" ] && HOST_URL="$HOST_URL:$PORT"

step() { printf '\n==> %s\n' "$*"; }
fail() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }
remote() { ssh -o BatchMode=yes -o ConnectTimeout=10 -p "$PORT" "$TARGET" "$@"; }

# 1. docker command line tool on this computer (no engine needed here).
step "docker command line tool on this computer"
if ! command -v docker >/dev/null 2>&1; then
    for d in /opt/homebrew/bin /usr/local/bin; do
        [ -x "$d/docker" ] && PATH="$PATH:$d"
    done
fi
if ! command -v docker >/dev/null 2>&1; then
    if command -v brew >/dev/null 2>&1; then
        brew install docker
    else
        fail "docker is not installed. macOS: install Homebrew (https://brew.sh), then brew install docker. Linux: install docker-ce-cli from your distribution."
    fi
fi
echo "ok: $(command -v docker)"

# 2. SSH key login.
step "SSH login to $TARGET"
KEY=""
for k in "$HOME/.ssh/id_ed25519" "$HOME/.ssh/id_ecdsa" "$HOME/.ssh/id_rsa"; do
    [ -f "$k.pub" ] && { KEY="$k"; break; }
done
if [ -z "$KEY" ]; then
    KEY="$HOME/.ssh/id_ed25519"
    mkdir -p "$HOME/.ssh" && chmod 700 "$HOME/.ssh"
    ssh-keygen -q -t ed25519 -N "" -C "aerocad@$(hostname -s 2>/dev/null || hostname)" -f "$KEY"
    echo "created $KEY"
fi
if ! remote true 2>/dev/null; then
    echo "Copying $KEY.pub to $TARGET (enter the password of $TARGET once)."
    # Accepts the host key on first contact; ssh asks to confirm its fingerprint.
    if command -v ssh-copy-id >/dev/null 2>&1; then
        ssh-copy-id -i "$KEY.pub" -p "$PORT" "$TARGET"
    else
        ssh -p "$PORT" "$TARGET" 'umask 077; mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys' < "$KEY.pub"
    fi
    remote true || fail "SSH login with the key still does not work: check sshd on $TARGET."
fi
echo "ok: key login works"

# 3. Docker on the other machine, usable by this user over a non-interactive SSH login.
step "Docker on $TARGET"
OS="$(remote uname -s)"
echo "system: $OS"
if ! remote command -v docker >/dev/null 2>&1; then
    if [ "$OS" = "Darwin" ]; then
        # Non-interactive SSH logins on macOS do not get Homebrew's / Docker Desktop's PATH.
        # shellcheck disable=SC2016 # Expanded on the other machine.
        DIR="$(remote 'for d in /opt/homebrew/bin /usr/local/bin /Applications/Docker.app/Contents/Resources/bin "$HOME/.orbstack/bin"; do [ -x "$d/docker" ] && { echo "$d"; break; }; done')"
        [ -n "$DIR" ] || fail "docker is not installed on $TARGET. Install it there (e.g. brew install docker colima && colima start), then run this script again."
        remote "grep -qs 'aerocad-docker-path' ~/.zshenv || printf '\nexport PATH=\"\$PATH:$DIR\" # aerocad-docker-path\n' >> ~/.zshenv"
        echo "added $DIR to PATH in ~/.zshenv on $TARGET"
    else
        fail "docker is not installed on $TARGET (or not on the PATH of SSH logins). Install Docker Engine there: https://docs.docker.com/engine/install/"
    fi
fi
if ! remote docker info >/dev/null 2>&1; then
    ERR="$(remote docker info 2>&1 >/dev/null || true)"
    if [[ "$ERR" == *"permission denied"* ]]; then
        echo "User ${TARGET%@*} may not use Docker on $TARGET. Adding it to the docker group (sudo password of $TARGET):"
        ssh -t -p "$PORT" "$TARGET" "sudo usermod -aG docker \$USER"
    elif [ "$OS" = "Darwin" ]; then
        echo "Docker is not running on $TARGET: starting it."
        remote 'if command -v colima >/dev/null 2>&1; then colima start; else open -a Docker; fi' || true
    else
        echo "Docker is not running on $TARGET: starting it (sudo password of $TARGET):"
        ssh -t -p "$PORT" "$TARGET" "sudo systemctl enable --now docker"
    fi
    for _ in $(seq 1 30); do
        remote docker info >/dev/null 2>&1 && break
        sleep 4
    done
    remote docker info >/dev/null 2>&1 || fail "Docker on $TARGET still does not answer: $(remote docker info 2>&1 | tail -1)"
fi
CPUS="$(remote docker info --format '{{.NCPU}}')"
echo "ok: Docker $(remote docker info --format '{{.ServerVersion}}') with $CPUS CPUs"

# 4. The OpenFOAM image there (about 0.4-0.5 GB download, once).
step "OpenFOAM image on $TARGET"
if remote docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "ok: $IMAGE is there"
else
    remote docker pull "$IMAGE"
fi

# 5. The way AeroCAD talks to it, and OpenFOAM itself.
step "Check: docker -H $HOST_URL and OpenFOAM"
docker -H "$HOST_URL" info --format '{{.Name}}' >/dev/null || fail "docker -H $HOST_URL info failed."
docker -H "$HOST_URL" run --rm "$IMAGE" -c 'simpleFoam -help >/dev/null && echo "ok: OpenFOAM $WM_PROJECT_VERSION runs"'

# 6. Save the engine in AeroCAD.
step "AeroCAD setting"
ROOT="${AEROCAD_CFD_DIR:-$HOME/.aerocad/cfd}"
mkdir -p "$ROOT"
printf '{\n  "remoteHost": "%s"\n}\n' "$HOST_URL" > "$ROOT/engine.json"
echo "ok: saved $HOST_URL in $ROOT/engine.json"

printf '\nDone. In AeroCAD press Check in the CFD panel: it shows "Remote engine %s (%s CPUs) is ready".\n' "$HOST_URL" "$CPUS"
