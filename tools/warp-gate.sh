#!/bin/sh
# warp-gate.sh -- read the WARP disarmed gate off a built core1 ELF, and say what it is.
#
# WHY THIS EXISTS, AND IT IS NOT A CONVENIENCE. Round 4 shipped `C62AE6E2` to metal, measured
# a -4.256 % disarm penalty on it, and scored gate G-R4.7 MISSED against it -- and a clean
# rebuild of round 4's own source produces a DIFFERENT run loop: 3 instructions and one load
# where the flown image has 4 and two, 2 188 bytes where the flown image has 2 372. The image
# that was measured is not what the source builds, proven byte-wise on the archived artifact
# (docs/WARP-ENGINE.md 0.15.2). §0.13.4 found the same class of defect one round earlier and
# attributed it to the wrong round.
#
# Nothing in the build would have said so. `-MMD -MP` writes the dependency data and round 4
# finally `-include`d it, but a tree whose objects are ALREADY stale still links happily, and
# an image is a container you cannot objdump. So the gate is read off the ELF, out loud, at
# the end of every image build -- and a round whose gate does not match its own design has to
# notice before it books a card rather than after it rolls one back.
#
# Usage:  tools/warp-gate.sh <path-to-Z3660_emu.elf> [...]
#         make lean && tools/warp-gate.sh z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/build-lean/Z3660_emu.elf
set -eu

OBJDUMP="${OBJDUMP:-arm-none-eabi-objdump}"
NM="${NM:-arm-none-eabi-nm}"
SYM='_Z15m68k_run_mmu040v'

for elf in "$@"; do
	if [ ! -f "$elf" ]; then
		echo "$elf: no such file" >&2
		exit 1
	fi
	size=$("$NM" -S "$elf" 2>/dev/null | awk -v s="$SYM" '$4==s {print strtonum("0x" $2)}')
	[ -n "$size" ] || { echo "$elf: $SYM not found" >&2; exit 1; }

	echo "=== $elf"
	echo "    m68k_run_mmu040 : $size B"

	# The disarmed gate is the run of instructions between the interpreter handler's `blx r3`
	# and the first conditional branch after it -- i.e. exactly what a DISARMED pass executes
	# that a pre-WARP loop did not. Print it, and count the loads in it, because "one load off
	# a live base" versus "two serially dependent loads" is the distinction two rounds of this
	# campaign turned on.
	#
	# A BUILD WITH NO GATE MUST SAY SO RATHER THAN PRINT SOMETHING. The ablation kit's levels
	# 3, 4 and 5 remove the hook, so the instruction after `blx r3` is ordinary interpreter
	# dispatch -- and printing eight lines of it under the heading "the gate" is exactly the
	# kind of plausible wrong answer this script exists to stop. The gate is recognised by its
	# SHAPE: a load, then a compare against zero, then a conditional branch.
	dis=$("$OBJDUMP" -d --disassemble="$SYM" "$elf" 2>/dev/null |
	      awk '/\tblx\tr3$/ && !s { s = 1; next }
	           s && n < 6 { print; n++ }')

	if ! printf '%s\n' "$dis" | head -3 |
	     awk 'NR==1 && /\tldr\t/ {a=1} NR==2 && /\tcmp\t.*#0$/ {b=1}
	          NR==3 && /\tb(eq|ne)\t/ {c=1} END {exit !(a && b && c)}'; then
		echo "    NO WARP GATE -- the run-loop hook is not in this build (ablation level >= 3)"
		continue
	fi

	printf '%s\n' "$dis" | head -3 | sed 's/^/    /'
	nl=$(printf '%s\n' "$dis" | head -3 | grep -c '	ldr	' || true)
	echo "    loads on the disarmed path: $nl"
done
