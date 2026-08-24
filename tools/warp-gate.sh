#!/bin/sh
# warp-gate.sh -- read the WARP run loops off a built core1 ELF, say what they are, and
#                 REFUSE the build if the disarmed one carries a hook.
#
# WHY THIS EXISTS, AND IT IS NOT A CONVENIENCE. Round 4 shipped `C62AE6E2` to metal, measured
# a -4.256 % disarm penalty on it, and scored gate G-R4.7 MISSED against it -- and a clean
# rebuild of round 4's own source produces a DIFFERENT run loop: 3 instructions and one load
# where the flown image has 4 and two, 2 188 bytes where the flown image has 2 372. The image
# that was measured is not what the source builds, proven byte-wise on the archived artifact
# (docs/WARP-ENGINE.md 0.15.2). 0.13.4 found the same class of defect one round earlier and
# attributed it to the wrong round.
#
# Nothing in the build would have said so. `-MMD -MP` writes the dependency data and round 4
# finally `-include`d it, but a tree whose objects are ALREADY stale still links happily, and
# an image is a container you cannot objdump. So the loops are read off the ELF, out loud, at
# the end of every image build -- and a round whose loops do not match their own design has
# to notice before it books a card rather than after it rolls one back.
#
# WHAT ROUND 6 ADDED, AND WHY IT IS NOW A GATE RATHER THAN A REPORT (0.17.3 G-R6.6/G-R6.12).
# The dormant tax's largest named layer was L-HOOK -- 1.967 pp of 3.445 -- so 0.17.1 split the
# loop in two and gave the disarmed image one with no hook COMPILED IN. That claim is worth
# exactly as much as its enforcement: a `#if` that silently stopped selecting, or a future
# edit that put a warp reference back into the plain body, would reintroduce the whole tax
# with nothing to say so. So this script now EXITS NON-ZERO when
#
#     * m68k_run_mmu040_nowarp contains a gate, or names any z3660_warp symbol at all;
#     * m68k_run_mmu040_warp does NOT contain exactly one gate with exactly one load;
#     * the selector m68k_run_mmu040 is missing.
#
# and the image targets in vitis_ide/Makefile invoke it, so a violating image cannot be built.
#
# Usage:  tools/warp-gate.sh <path-to-Z3660_emu.elf> [...]
#         make lean && tools/warp-gate.sh z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/build-lean/Z3660_emu.elf
set -eu

OBJDUMP="${OBJDUMP:-arm-none-eabi-objdump}"
NM="${NM:-arm-none-eabi-nm}"
SEL='_Z15m68k_run_mmu040v'
NOWARP='_Z22m68k_run_mmu040_nowarpv'
WARP='_Z20m68k_run_mmu040_warpv'

rc=0

symsize() {
	"$NM" -S "$1" 2>/dev/null | awk -v s="$2" '$4==s {print strtonum("0x" $2)}'
}

# The disarmed gate is the run of instructions between the interpreter handler's `blx r3`
# and the first conditional branch after it -- i.e. exactly what a DISARMED pass executes
# that a pre-WARP loop did not. Print it, and count the loads in it, because "one load off
# a live base" versus "two serially dependent loads" is the distinction two rounds of this
# campaign turned on.
#
# A BUILD WITH NO GATE MUST SAY SO RATHER THAN PRINT SOMETHING. Since round 6 the plain loop
# has no gate BY DESIGN, and the ablation kit's levels 3+ have no hooked loop at all -- so
# printing six lines of ordinary interpreter dispatch under the heading "the gate" is exactly
# the kind of plausible wrong answer this script exists to stop. The gate is recognised by
# its SHAPE: a load, then a compare against zero, then a conditional branch.
after_blx() {
	"$OBJDUMP" -d --disassemble="$2" "$1" 2>/dev/null |
	awk '/\tblx\tr3$/ && !s { s = 1; next }
	     s && n < 6 { print; n++ }'
}

has_gate() {
	printf '%s\n' "$1" | head -3 |
	awk 'NR==1 && /\tldr\t/ {a=1} NR==2 && /\tcmp\t.*#0$/ {b=1}
	     NR==3 && /\tb(eq|ne)\t/ {c=1} END {exit !(a && b && c)}'
}

for elf in "$@"; do
	if [ ! -f "$elf" ]; then
		echo "$elf: no such file" >&2
		exit 1
	fi
	echo "=== $elf"

	sz_nowarp=$(symsize "$elf" "$NOWARP")
	sz_warp=$(symsize "$elf" "$WARP")
	sz_sel=$(symsize "$elf" "$SEL")

	# PRE-ROUND-6 ELFs -- one loop, named m68k_run_mmu040. The archive holds several and
	# they are still the reference rows, so the script reads them rather than refusing
	# them; there is nothing to gate, because the design it gates did not exist yet.
	if [ -z "$sz_nowarp" ]; then
		[ -n "$sz_sel" ] || { echo "    $SEL not found" >&2; rc=1; continue; }
		echo "    m68k_run_mmu040 : $sz_sel B   (pre-round-6 single-loop image)"
		dis=$(after_blx "$elf" "$SEL")
		if has_gate "$dis"; then
			printf '%s\n' "$dis" | head -3 | sed 's/^/    /'
			echo "    loads on the disarmed path: $(printf '%s\n' "$dis" | head -3 | grep -c '	ldr	' || true)"
		else
			echo "    NO WARP GATE -- the run-loop hook is not in this build"
		fi
		continue
	fi

	# ---- ROUND 6 AND LATER: two loops and a selector ----------------------------------
	[ -n "$sz_sel" ] || { echo "    FAIL: the selector $SEL is missing"; rc=1; continue; }
	echo "    selector m68k_run_mmu040        : $sz_sel B"
	echo "    disarmed m68k_run_mmu040_nowarp : $sz_nowarp B"

	dis=$(after_blx "$elf" "$NOWARP")
	if has_gate "$dis"; then
		echo "    FAIL: the DISARMED loop carries a gate -- the dormant tax is back (G-R6.6)"
		printf '%s\n' "$dis" | head -3 | sed 's/^/        /'
		rc=1
	else
		echo "        no gate after the handler returns -- as designed"
	fi

	# Stronger than the shape test and independent of it: the plain body must not NAME
	# the engine anywhere, by call or by address. A hook that moved would still be caught.
	refs=$("$OBJDUMP" -d --disassemble="$NOWARP" "$elf" 2>/dev/null |
	       grep -c 'z3660_warp' || true)
	if [ "$refs" -ne 0 ]; then
		echo "    FAIL: the DISARMED loop names z3660_warp* in $refs place(s) (G-R6.6)"
		rc=1
	else
		echo "        names no z3660_warp symbol at all"
	fi

	if [ -z "$sz_warp" ]; then
		# Ablation levels 3+ build no hooked loop on purpose (z3660_warp_ablate.h).
		echo "    armed  m68k_run_mmu040_warp     : ABSENT -- ablation level >= 3, arming is unreachable"
		continue
	fi

	echo "    armed  m68k_run_mmu040_warp     : $sz_warp B"
	dis=$(after_blx "$elf" "$WARP")
	if has_gate "$dis"; then
		printf '%s\n' "$dis" | head -3 | sed 's/^/        /'
		nl=$(printf '%s\n' "$dis" | head -3 | grep -c '	ldr	' || true)
		echo "        loads on the armed loop's dispatch path: $nl"
		[ "$nl" -eq 1 ] || { echo "    FAIL: expected exactly one load in the gate"; rc=1; }
		# The regstruct displacement, printed because G-R6.9's whole risk is that a member
		# added below warp_on pushes it past ldr's imm12 and the compiler goes back to
		# materialising an address -- which is round 3's arrangement, at -4.056 %.
		off=$(printf '%s\n' "$dis" | head -1 | sed -n 's/.*\[r[0-9a-z]*, #\([0-9]*\)\].*/\1/p')
		[ -n "$off" ] && echo "        regs.warp_on displacement: $off (imm12 limit 4095)"
	else
		echo "    FAIL: the ARMED loop has no gate -- it cannot engage the engine (G-R6.6)"
		rc=1
	fi
done

exit $rc
