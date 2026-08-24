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
#     * m68k_run_mmu040_warp does NOT contain a gate, or reaches warp_on through a
#       materialised address instead of one load off a live base;
#     * the selector m68k_run_mmu040 is missing.
#
# and the image targets in vitis_ide/Makefile invoke it, so a violating image cannot be built.
#
# WHAT THE SHAPE TEST GOT WRONG, AND IT IS A TOOLCHAIN STORY (2026-08-24). The matcher used
# to require the gate on three FIXED lines -- ldr, then cmp, then the branch, at offsets 1, 2
# and 3 after the handler's `blx r3`. That is not the gate, it is one compiler's SCHEDULE of
# the gate. gcc 14.2.1 sinks the `z3660_warp_step` argument load below the branch; gcc 12.2.0
# hoists it between the load and the compare, so the cmp lands on line 3, the branch on line 4,
# and the matcher reported NO GATE on a loop that plainly calls the engine two instructions
# later. Every image the WARP campaign ever archived was built by the HOST arm-none-eabi-gcc
# 14.2.1; the first build through the Vitis docker (gcc 12.2.0) failed the gate on the default
# variant AND on lean, which is how a compiler difference got read as a defect in one variant.
#
# So the gate is now recognised by its DATAFLOW: a compare against zero whose register was
# loaded earlier in the window, immediately followed by a conditional branch. Instructions
# scheduled into the middle of it no longer hide it. Two things keep that from being a
# loosening. The disarmed loop is still refused if it carries the same dataflow, and it is
# still checked -- independently, and more strongly than any shape test -- for naming a
# z3660_warp symbol at all. And the "one load" rule, which existed to catch round 3's
# arrangement (materialise `&warp_on` from the literal pool, then load through it, at
# -4.056 %), is now stated as the property it always meant: the load that feeds the compare
# must come off a LIVE BASE, not off a register some earlier load in the window produced.
# A round-3 regression still fails. An argument load the scheduler moved does not.
#
# AND THE PRODUCER IS PRINTED, because the whole incident was one invisible variable. A
# manifest row that does not say which compiler built it cannot be compared with one that
# was built by another.
#
# Usage:  tools/warp-gate.sh <path-to-Z3660_emu.elf> [...]
#         make lean && tools/warp-gate.sh z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/build-lean/Z3660_emu.elf
#         make gate     (re-gates every core1 build directory that exists, without rebuilding)
set -eu

OBJDUMP="${OBJDUMP:-arm-none-eabi-objdump}"
NM="${NM:-arm-none-eabi-nm}"
READELF="${READELF:-arm-none-eabi-readelf}"
SEL='_Z15m68k_run_mmu040v'
NOWARP='_Z22m68k_run_mmu040_nowarpv'
WARP='_Z20m68k_run_mmu040_warpv'

rc=0

symsize() {
	"$NM" -S "$1" 2>/dev/null | awk -v s="$2" '$4==s {print strtonum("0x" $2)}'
}

# Which compiler produced this ELF. Several may appear -- the app objects and the prebuilt
# Xilinx/newlib libraries need not agree -- so all of them are printed, deduplicated.
builtby() {
	"$READELF" -p .comment "$1" 2>/dev/null |
	awk '{ i = index($0, "GCC: ")
	       if (i) { s = substr($0, i)
	                if (!(s in seen)) { seen[s] = 1; out = out (out ? "  +  " : "") s } } }
	     END { print out }'
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
# its DATAFLOW: a load, a compare of THAT REGISTER against zero, and a conditional branch on
# the compare -- wherever in the window the scheduler put them.
after_blx() {
	"$OBJDUMP" -d --disassemble="$2" "$1" 2>/dev/null |
	awk '/\tblx\tr3$/ && !s { s = 1; next }
	     s && n < 6 { print; n++ }'
}

# gate_facts <window>. Prints TAB-separated facts and exits 0 when the window holds a gate:
#     LOAD  <asm>   the load whose value the compare consumes
#     CMP   <asm>   cmp <that register>, #0
#     BR    <asm>   the conditional branch on it, immediately after the compare
#     LOADS <n>     ldr count from the top of the window through the branch
#     DEP   <0|1>   1 = the load's base register is itself produced by an earlier load in
#                   the window (or is pc) -- i.e. an address was materialised first
#     OFF   <imm>   the load's displacement, empty if it is not [rN,#imm]
# Exits non-zero, printing nothing, when there is no gate.
gate_facts() {
	printf '%s\n' "$1" |
	awk '
	{
		ln[NR] = $0; cum[NR] = nldr
		if (match($0, /[ \t]ldr[ \t]+([a-z0-9]+), \[([a-z0-9]+)(, #([0-9]+))?\]/, m)) {
			dst[NR] = m[1]; base[NR] = m[2]; off[NR] = m[4]
			cum[NR] = ++nldr
		}
		if (match($0, /[ \t]cmp[ \t]+([a-z0-9]+), #0$/, c)) cmpreg[NR] = c[1]
		if ($0 ~ /[ \t]b(eq|ne)[ \t]/)                      isbr[NR]   = 1
	}
	END {
		for (i = 1; i <= NR; i++) {
			if (!(i in cmpreg) || !((i + 1) in isbr)) continue
			j = 0
			for (k = i - 1; k >= 1; k--)
				if ((k in dst) && dst[k] == cmpreg[i]) { j = k; break }
			if (j == 0) continue                    # compared a value nothing loaded here
			dep = (base[j] == "pc") ? 1 : 0
			for (k = j - 1; k >= 1 && !dep; k--)
				if ((k in dst) && dst[k] == base[j]) dep = 1
			printf "LOAD\t%s\n",  ln[j]
			printf "CMP\t%s\n",   ln[i]
			printf "BR\t%s\n",    ln[i + 1]
			printf "LOADS\t%d\n", cum[i + 1]
			printf "DEP\t%d\n",   dep
			printf "OFF\t%s\n",   off[j]
			exit 0
		}
		exit 1
	}'
}

# The VALUE is everything after the first tab -- a disassembly line has tabs of its own, so
# splitting on every tab keeps only the address column and prints a plausible nothing.
gate_field() {
	printf '%s\n' "$1" |
	awk -v k="$2" '{ t = index($0, "\t") }
	     t && substr($0, 1, t - 1) == k { print substr($0, t + 1); exit }'
}

gate_show() {
	printf '%s\n' "$1" |
	awk -v pad="$2" '{ t = index($0, "\t"); h = substr($0, 1, t - 1) }
	     h == "LOAD" || h == "CMP" || h == "BR" { print pad substr($0, t + 1) }'
}

# ---- THE MATCHER'S OWN SELF-TEST -------------------------------------------------------
# The matcher is the part of this script that was wrong, and nothing checked it: the gate was
# demonstrated once, by hand, on one compiler, and then rotted silently against another. So
# it is now checkable on demand. The fixtures are windows lifted VERBATIM off real builds --
# one per toolchain -- plus the two arrangements the gate exists to refuse. No assembler, no
# ELF, no build: text in, verdict out, so it costs nothing and cannot itself go stale.
selftest_case() {
	if facts=$(gate_facts "$5"); then
		got=GATE; gotl=$(gate_field "$facts" LOADS); gotd=$(gate_field "$facts" DEP)
	else
		got=NONE; gotl=-; gotd=-
	fi
	if [ "$got" = "$2" ] && [ "$gotl" = "$3" ] && [ "$gotd" = "$4" ]; then
		echo "    ok    $1  ($got loads=$gotl dep=$gotd)"
	else
		echo "    FAIL  $1  want $2 loads=$3 dep=$4 -- got $got loads=$gotl dep=$gotd"
		rc=1
	fi
}

if [ "${1:-}" = "--selftest" ]; then
	echo "=== warp-gate.sh gate matcher, self-test"

	# The armed gate as gcc 14.2.1 schedules it: the warp_step argument load sinks BELOW
	# the branch, so the gate occupies three consecutive lines. (build-lean, 9AECF99D.)
	selftest_case "armed gate, gcc 14.2.1 schedule" GATE 1 0 '302462e4:	e59431d0 	ldr	r3, [r4, #464]	@ 0x1d0
302462e8:	e3530000 	cmp	r3, #0
302462ec:	0a00000a 	beq	3024631c <_Z20m68k_run_mmu040_warpv+0x98>
302462f0:	e5940058 	ldr	r0, [r4, #88]	@ 0x58
302462f4:	ebf964e5 	bl	3009f690 <z3660_warp_step>
302462f8:	e2503000 	subs	r3, r0, #0'

	# The same gate as gcc 12.2.0 schedules it: the argument load is HOISTED between the
	# load and the compare. This is the arrangement that used to be reported as no gate.
	selftest_case "armed gate, gcc 12.2.0 schedule" GATE 2 0 '30258208:	e59531d0 	ldr	r3, [r5, #464]	@ 0x1d0
3025820c:	e5950058 	ldr	r0, [r5, #88]	@ 0x58
30258210:	e3530000 	cmp	r3, #0
30258214:	0a000009 	beq	30258240 <_Z20m68k_run_mmu040_warpv+0x98>
30258218:	ebf943cc 	bl	300a9150 <z3660_warp_step>
3025821c:	e2503000 	subs	r3, r0, #0'

	# Round 3's arrangement, at -4.056 %: the address is materialised out of the literal
	# pool and the flag is loaded THROUGH it. Two serially dependent loads -- must fail.
	selftest_case "materialised address (round 3)" GATE 2 1 '30246300:	e59f3790 	ldr	r3, [pc, #1936]	@ 30246a98 <_Z20m68k_run_mmu040_warpv+0x814>
30246304:	e5933000 	ldr	r3, [r3]
30246308:	e3530000 	cmp	r3, #0
3024630c:	0a00000a 	beq	3024633c <_Z20m68k_run_mmu040_warpv+0x98>'

	# The disarmed loop, verbatim off the shipping lean. Loads and a store, no compare
	# against zero anywhere -- six lines of ordinary dispatch must not read as a gate.
	selftest_case "disarmed loop, no gate" NONE - - '30246b6c:	e59f6790 	ldr	r6, [pc, #1936]	@ 30247304 <_Z22m68k_run_mmu040_nowarpv+0x7f8>
30246b70:	e3e03000 	mvn	r3, #0
30246b74:	e59f778c 	ldr	r7, [pc, #1932]	@ 30247308 <_Z22m68k_run_mmu040_nowarpv+0x7fc>
30246b78:	e59fc774 	ldr	ip, [pc, #1908]	@ 302472f4 <_Z22m68k_run_mmu040_nowarpv+0x7e8>
30246b7c:	e1c630b0 	strh	r3, [r6]
30246b80:	e5943058 	ldr	r3, [r4, #88]	@ 0x58'

	# A compare and a branch on a register NOTHING in the window loaded. That is not a
	# read of warp_on, and calling it one is the plausible wrong answer to avoid.
	selftest_case "compare fed by no load" NONE - - '30246400:	e3530000 	cmp	r3, #0
30246404:	0a00000a 	beq	30246434 <_Z20m68k_run_mmu040_warpv+0x98>
30246408:	e5940058 	ldr	r0, [r4, #88]	@ 0x58'

	if [ "$rc" -eq 0 ]; then
		echo "    all five cases as expected"
	fi
	exit $rc
fi

for elf in "$@"; do
	if [ ! -f "$elf" ]; then
		echo "$elf: no such file" >&2
		exit 1
	fi
	echo "=== $elf"
	echo "    built by: $(builtby "$elf")"

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
		if facts=$(gate_facts "$dis"); then
			gate_show "$facts" "    "
			echo "    loads on the disarmed path: $(gate_field "$facts" LOADS)"
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
	if facts=$(gate_facts "$dis"); then
		echo "    FAIL: the DISARMED loop carries a gate -- the dormant tax is back (G-R6.6)"
		gate_show "$facts" "        "
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
	if facts=$(gate_facts "$dis"); then
		gate_show "$facts" "        "
		echo "        loads in the gate window: $(gate_field "$facts" LOADS)"
		# G-R6.9's whole risk is that a member added below warp_on pushes it past ldr's
		# imm12 and the compiler goes back to materialising an address -- which is round
		# 3's arrangement, at -4.056 %. THAT is what fails here; a surplus load the
		# scheduler moved into the window is not it, and used to be confused with it.
		if [ "$(gate_field "$facts" DEP)" -ne 0 ]; then
			echo "    FAIL: the gate reaches warp_on through a materialised address, not one"
			echo "          load off a live base -- round 3's arrangement is back (G-R6.9)"
			rc=1
		else
			echo "        warp_on read in one load off a live base, zero materialisations"
		fi
		off=$(gate_field "$facts" OFF)
		if [ -n "$off" ]; then
			echo "        regs.warp_on displacement: $off (imm12 limit 4095)"
		fi
	else
		echo "    FAIL: the ARMED loop has no gate -- it cannot engage the engine (G-R6.6)"
		rc=1
	fi
done

exit $rc
