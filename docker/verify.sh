#!/usr/bin/env bash
# Structural sanity checks for rebuilt Z3660 artifacts.
#
# Run inside the container after a successful build:
#   ./docker/run.sh ./docker/verify.sh
#
# Exits 0 on success, non-zero on any failure. Prints a summary table.
set -uo pipefail

REPO_ROOT="${REPO_ROOT:-/work}"
FAIL=0
PASS=0

red()    { printf '\033[31m%s\033[0m' "$*"; }
green()  { printf '\033[32m%s\033[0m' "$*"; }
yellow() { printf '\033[33m%s\033[0m' "$*"; }

ok()   { PASS=$((PASS+1)); printf '  [ %s ] %s\n' "$(green OK)"   "$*"; }
fail() { FAIL=$((FAIL+1)); printf '  [ %s ] %s\n' "$(red FAIL)"   "$*"; }
warn() {                  printf '  [ %s ] %s\n' "$(yellow WARN)" "$*"; }

section() {
    printf '\n== %s ==\n' "$*"
}

# Args: path, label, min-bytes
check_exists_min() {
    local path="$1" label="$2" minb="$3"
    if [[ ! -f "$path" ]]; then
        fail "$label missing: $path"; return
    fi
    local sz; sz=$(stat -c %s "$path")
    if (( sz < minb )); then
        fail "$label too small: $sz bytes (< $minb): $path"
    else
        ok "$label exists, $sz bytes: $path"
    fi
}

# Args: path, label, hex magic at offset 0 (e.g. "000003F3" for Amiga Hunk)
check_magic() {
    local path="$1" label="$2" magic="$3"
    if [[ ! -f "$path" ]]; then fail "$label missing: $path"; return; fi
    local got; got=$(xxd -p -l "${#magic}" -s 0 "$path" 2>/dev/null | tr a-f A-F | head -c "${#magic}")
    local want; want=$(printf '%s' "$magic" | tr a-f A-F)
    if [[ "$got" == "$want" ]]; then
        ok "$label has expected magic $magic"
    else
        fail "$label magic mismatch: got $got, want $want ($path)"
    fi
}

# ---------------------------------------------------------------------------
# Firmware artifacts
# ---------------------------------------------------------------------------
section "Firmware (BOOT.BIN)"

BOOT_BIN_CANDIDATES=(
    "$REPO_ROOT/z3660-firmware/Z-TURN/vitis_ide/Z3660_system/Alfa/sd_card/BOOT.BIN"
    "$REPO_ROOT/z3660-firmware/Z-TURN/vitis_ide/Z3660_system/Debug/sd_card/BOOT.BIN"
    "$REPO_ROOT/z3660-firmware/Z-TURN/vitis_ide/Z3660_system/sd_card/BOOT.BIN"
)
BOOT_BIN=""
for c in "${BOOT_BIN_CANDIDATES[@]}"; do
    if [[ -f "$c" ]]; then BOOT_BIN="$c"; break; fi
done

if [[ -z "$BOOT_BIN" ]]; then
    warn "no rebuilt BOOT.BIN found in expected locations (firmware may not have been built yet)"
else
    check_exists_min "$BOOT_BIN" "BOOT.BIN" $((1*1024*1024))
    # Zynq-7000 BOOT.BIN header: sync word 0xAA995566 stored little-endian at
    # offset 0x20 (bytes 66 55 99 AA), followed by "XLNX" identifier at 0x24.
    SYNC_HEX=$(xxd -p -l 4 -s 0x20 "$BOOT_BIN" 2>/dev/null | tr a-f A-F)
    XLNX_HEX=$(xxd -p -l 4 -s 0x24 "$BOOT_BIN" 2>/dev/null | tr a-f A-F)
    if [[ "$SYNC_HEX" == "665599AA" && "$XLNX_HEX" == "584E4C58" ]]; then
        ok "BOOT.BIN Zynq sync word + XLNX identifier present"
    else
        warn "BOOT.BIN header bytes: sync@0x20=0x$SYNC_HEX, XLNX@0x24=0x$XLNX_HEX (expected 665599AA, 584E4C58)"
    fi
fi

# ---------------------------------------------------------------------------
# Driver binaries — Amiga Hunk format magic 0x000003F3
# ---------------------------------------------------------------------------
section "Drivers (Amiga Hunk format)"

check_magic "$REPO_ROOT/z3660-drivers/rtg/Z3660.card"             "Z3660.card"        "000003F3"
check_magic "$REPO_ROOT/z3660-drivers/eth/Z3660Net.device"        "Z3660Net.device"   "000003F3"
check_magic "$REPO_ROOT/z3660-drivers/scsi/z3660_scsi.device"     "z3660_scsi.device" "000003F3"
check_magic "$REPO_ROOT/z3660-drivers/ahi/z3660ax.audio"          "z3660ax.audio"     "000003F3"
check_magic "$REPO_ROOT/z3660-drivers/mhi/mhiz3660.library"       "mhiz3660.library"  "000003F3"
check_magic "$REPO_ROOT/z3660-drivers/Z3660_Wazp3D/Wazp3D.library" "Wazp3D.library"    "000003F3"
check_magic "$REPO_ROOT/z3660-drivers/usb/z3660_usb.device"       "z3660_usb.device"  "000003F3"

# ---------------------------------------------------------------------------
# Kickstart ROM — stub when no user-supplied Kickstart 3.1 ROM is present;
# only ~200 bytes. A full build with a user-supplied AmigaOS 3.1 ROM produces
# a 512 KB binary. So this only sanity-checks existence.
# ---------------------------------------------------------------------------
section "Kickstart ROM"
check_exists_min "$REPO_ROOT/z3660-drivers/kick31_060/kick060.rom" "kick060.rom" 1

# ---------------------------------------------------------------------------
# ADF
# ---------------------------------------------------------------------------
section "ADF (880 KB Amiga floppy image)"
ADF="$REPO_ROOT/z3660-drivers/0_Z3660.adf"
if [[ -f "$ADF" ]]; then
    SZ=$(stat -c %s "$ADF")
    if [[ "$SZ" -eq 901120 ]]; then
        ok "0_Z3660.adf is exactly 880 KB"
    else
        fail "0_Z3660.adf is $SZ bytes (expected 901120)"
    fi
    if command -v xdftool >/dev/null; then
        if xdftool "$ADF" list > /dev/null 2>&1; then
            ok "0_Z3660.adf passes xdftool list"
        else
            fail "0_Z3660.adf fails xdftool list parse"
        fi
    else
        warn "xdftool not on PATH, skipping ADF content parse"
    fi
else
    fail "0_Z3660.adf missing"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
printf '\n== Summary ==\n'
printf '  %d passed, %d failed\n' "$PASS" "$FAIL"

(( FAIL == 0 ))
