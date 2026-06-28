#!/usr/bin/env bash
# Run a command in the Z3660 build container.
#
# Usage:
#   ./docker/run.sh                            # interactive shell in /work
#   ./docker/run.sh make                       # build firmware -> BOOT.BIN
#   ./docker/run.sh make -C z3660-drivers all adf   # build drivers + ADF
#   ./docker/run.sh ./docker/verify.sh          # run verification
#
# Env:
#   IMAGE   Image tag to run.   Default: z3660-build:latest
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE="${IMAGE:-z3660-build:latest}"

DOCKER_FLAGS=( --rm -v "$REPO_ROOT:/work" -w /work )

if [[ -t 0 && -t 1 ]]; then
    DOCKER_FLAGS+=( -it )
fi

if [[ $# -eq 0 ]]; then
    exec docker run "${DOCKER_FLAGS[@]}" "$IMAGE" bash -l
fi

# Wrap the command in a login shell so /etc/profile.d/xilinx.sh is sourced
# (Vitis settings64.sh puts arm-none-eabi-gcc, bootgen etc. on PATH).
# `printf %q` quotes each argument safely for re-evaluation inside the shell.
QUOTED=
for arg in "$@"; do
    QUOTED+=" $(printf '%q' "$arg")"
done
exec docker run "${DOCKER_FLAGS[@]}" "$IMAGE" bash -lc "${QUOTED# }"
