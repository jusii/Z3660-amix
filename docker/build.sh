#!/usr/bin/env bash
# Build the Z3660 build-environment image.
#
# Usage:
#   ./docker/build.sh                 # full image (drivers + Vitis), needs XILINX_DIR
#   ./docker/build.sh --drivers-only  # small image, no Vitis, no Xilinx blob needed
#
# Env:
#   XILINX_DIR   Where the Xilinx SFD + Update tarballs live. Default: docker/xilinx
#   IMAGE        Image tag to produce.        Default: z3660-build:latest
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TARGET=full
IMAGE="${IMAGE:-z3660-build:latest}"
XILINX_DIR="${XILINX_DIR:-$SCRIPT_DIR/xilinx}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --drivers-only) TARGET=drivers-only ;;
        --full)         TARGET=full ;;
        --tag)          IMAGE="$2"; shift ;;
        --xilinx-dir)   XILINX_DIR="$2"; shift ;;
        -h|--help)
            sed -n '2,10p' "$0"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 2
            ;;
    esac
    shift
done

UID_HOST="$(id -u)"
GID_HOST="$(id -g)"

echo "==> Building $IMAGE (target=$TARGET) as UID=$UID_HOST GID=$GID_HOST"

BUILDX_ARGS=(
    --target "$TARGET"
    --tag    "$IMAGE"
    --build-arg "UID=$UID_HOST"
    --build-arg "GID=$GID_HOST"
)

if [[ "$TARGET" == "full" ]]; then
    if [[ ! -d "$XILINX_DIR" ]]; then
        cat >&2 <<EOF
==> ERROR: XILINX_DIR=$XILINX_DIR does not exist.

The 'full' target needs the Xilinx Unified 2023.2 installer (and the 2023.2.2
update if you want to match what the project's FPGA IPs were last regenerated
with). Stage either form:

  EXTRACTED (preferred, no extra work for the build):
    \$XILINX_DIR/FPGAs_AdaptiveSoCs_Unified_2023.2_*/   <- contains xsetup
    \$XILINX_DIR/Vivado_Vitis_Update_2023.2.2_*/       <- contains xsetup

  TARBALLS (build will extract them — slow, ~100 GB tmpfs needed):
    \$XILINX_DIR/FPGAs_AdaptiveSoCs_Unified_2023.2_*.tar.gz
    \$XILINX_DIR/Vivado_Vitis_Update_2023.2.2_*.tar(.gz)

Or pass --drivers-only to skip Vitis entirely.
EOF
        exit 1
    fi

    BASE=$(find "$XILINX_DIR" -maxdepth 3 \
        \( -path '*FPGAs_AdaptiveSoCs_Unified_2023.2*/xsetup' \
        -o -path '*Xilinx_Unified_2023.2*/xsetup' \
        -o -path '*Unified_2023.2_1013*/xsetup' \) \
        -type f 2>/dev/null | head -n1)
    if [[ -z "$BASE" ]]; then
        echo "==> ERROR: no base 2023.2 xsetup found under $XILINX_DIR" >&2
        echo "    Looked for: */FPGAs_AdaptiveSoCs_Unified_2023.2*/xsetup, */Xilinx_Unified_2023.2*/xsetup" >&2
        ls -la "$XILINX_DIR" >&2
        exit 1
    fi
    echo "==> Found base installer:  $BASE"

    UPDATE=$(find "$XILINX_DIR" -maxdepth 3 \
        -path '*Vivado_Vitis_Update_2023.2.2*/xsetup' \
        -type f 2>/dev/null | head -n1)
    if [[ -n "$UPDATE" ]]; then
        echo "==> Found update 2023.2.2: $UPDATE"
    else
        echo "==> No Update 2023.2.2 found; will install base only."
    fi

    BUILDX_ARGS+=( --build-context "xilinx=$XILINX_DIR" )
fi

cd "$SCRIPT_DIR"

DOCKER_BUILDKIT=1 exec docker buildx build "${BUILDX_ARGS[@]}" .
