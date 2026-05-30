#!/usr/bin/env bash
# Build the host gencpu tool against THIS tree's headers (Phase 0, decision #9).
#
# Usage:  ./build_gencpu.sh <path-to-amiberry-or-winuae-src> [out-binary]
#   e.g.  ./build_gencpu.sh /tmp/amiberry-560/src ./gencpu
#
# Needs from the source checkout: gencpu.cpp.  Uses THIS tree's cpudefs.cpp,
# readcpu.cpp and headers so the generated tables match this tree's opcode set.
# Only external symbol is ua() (Amiberry charset) -> charset_stub.cpp.
set -euo pipefail

SRC="${1:?usage: build_gencpu.sh <amiberry/winuae src dir> [out]}"
OUT="${2:-./gencpu}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE="$HERE/../.."          # z3660-firmware/.../Z3660_emu
UAE="$TREE/src/uae"

INCL=(-I"$TREE/test/host/stubs" -I"$UAE/include" -I"$UAE/include/uae" -I"$TREE/src" -I"$UAE")
DEFS=(-DSDL_strcasecmp=strcasecmp -D_vsnprintf=vsnprintf -Wno-write-strings -Wno-narrowing)

echo "==> building gencpu from $SRC/gencpu.cpp against tree headers"
g++ -O1 -std=c++11 -fno-strict-aliasing "${DEFS[@]}" "${INCL[@]}" \
    "$SRC/gencpu.cpp" "$UAE/cpudefs.cpp" "$UAE/readcpu.cpp" "$HERE/charset_stub.cpp" \
    -o "$OUT"
echo "==> built $OUT"
echo "    run it in an empty dir; it emits cpuemu_{0,4,11,13,40,44}.cpp, cpustbl.cpp, cputbl.h"
echo "    NOTE: Amiberry v5.6.0's gencpu.cpp aborts on 68060 HALT/PULSE/LPSTOP;"
echo "          use a WinUAE-lineage gencpu that matches this tree's cpustbl casts."
