#!/usr/bin/env bash
# Phase 0 non-MMU baseline gate (decision #9): diff this tree's checked-in
# cpuemu_*/cpustbl against Amiberry v5.6.0's pre-generated files. Expected result
# is the known-edit set documented in README.md (cpuemu_4/11/13/44 == identical;
# cpuemu_0=14, cpuemu_40=104, cpustbl=1712). Any *other* divergence is a finding.
#
# Usage: ./baseline_diff.sh <path-to-amiberry-v5.6.0/src>
#   (clone:  git clone --depth 1 --branch v5.6.0 \
#            https://github.com/BlitterStudio/amiberry /tmp/amiberry-560)
set -euo pipefail
A="${1:?usage: baseline_diff.sh <amiberry-v5.6.0/src dir>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UAE="$HERE/../../src/uae"

printf "%-12s %-14s %s\n" file changed-lines status
for f in cpuemu_0 cpuemu_4 cpuemu_11 cpuemu_13 cpuemu_40 cpuemu_44 cpustbl; do
	if [[ -f "$A/$f.cpp" ]]; then
		d=$(diff "$UAE/$f.cpp" "$A/$f.cpp" 2>/dev/null | grep -c '^[<>]' || true)
		case "$f:$d" in
			cpuemu_4:0|cpuemu_11:0|cpuemu_13:0|cpuemu_44:0|cpuemu_0:14|cpuemu_40:104|cpustbl:1712)
				st="OK (known)";;
			*) st="!! UNEXPECTED — investigate";;
		esac
		printf "%-12s %-14s %s\n" "$f" "$d" "$st"
	else
		printf "%-12s %-14s %s\n" "$f" "-" "missing in reference"
	fi
done
