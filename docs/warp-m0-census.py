#!/usr/bin/env python3
"""Plan WARP ENGINE, M0 -- the zero-build censuses behind docs/WARP-ENGINE.md Sec 0.3.

Reads source only. Builds nothing, needs no toolchain, and reproduces every number the
freeze section quotes for censuses (a) and (b). Run it from anywhere in the repo:

    python3 docs/warp-m0-census.py

The point of committing it rather than quoting a one-off grep is the campaign's own
standard: a number in a pre-registration has to be re-derivable by whoever doubts it,
including after the code it counts has moved.
"""

import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
JIT = os.path.join(HERE, os.pardir, "z3660-firmware", "Z-TURN", "vitis_ide",
                   "Z3660_emu", "src", "uae", "jit")
JIT = os.path.normpath(JIT)

# The complete guest-memory API a per-opcode compiler may call. Defined in
# compemu_support_arm.cpp; every one of them funnels into the jnf_MEM_* layer.
# jnf_MOVE16 is here because it is the one per-opcode midfunc that reaches guest
# memory WITHOUT going through that API (see census (a)).
MEM_API = ["readbyte", "readword", "readlong",
           "writebyte", "writeword", "writelong",
           "writeword_clobber", "writelong_clobber",
           "get_n_addr", "get_n_addr_jmp",
           "jnf_MOVE16"]


def read(name):
    with open(os.path.join(JIT, name), "r", errors="replace") as fh:
        return fh.read()


def census_a():
    print("=" * 78)
    print("CENSUS (a) -- the jnf_MEM layer, and how much of the compile table depends on it")
    print("=" * 78)

    # --- (a1)/(a2) every midfunc, and which of them emit through R_MEMSTART ---
    lines = read("compemu_midfunc_arm2.cpp").splitlines()
    midfuncs, cur = [], None
    for i, line in enumerate(lines, 1):
        m = re.match(r"MIDFUNC\((\d+),([A-Za-z0-9_]+),", line)
        if m:
            cur = [m.group(2), i, int(m.group(1)), False]
            midfuncs.append(cur)
        if cur is not None and "R_MEMSTART" in line:
            cur[3] = True
        if line.startswith("MENDFUNC"):
            cur = None

    mem = [x for x in midfuncs if x[0].startswith("jnf_MEM_")]
    via = [x for x in midfuncs if x[3]]

    print("\n(a1) jnf_MEM_* midfuncs in compemu_midfunc_arm2.cpp: %d" % len(mem))
    for name, ln, nargs, ms in mem:
        print("     %-24s :%-6d args=%d  R_MEMSTART=%s"
              % (name, ln, nargs, "yes" if ms else "no (memory-bank table)"))

    print("\n(a2) midfuncs that emit through R_MEMSTART at all: %d" % len(via))
    for name, ln, _, _ in via:
        mark = "" if name.startswith("jnf_MEM_") else "   <-- OUTSIDE the jnf_MEM_ layer"
        print("     %-24s :%d%s" % (name, ln, mark))

    print("\n(a3) R_MEMSTART elsewhere in jit/:")
    for fname in ("codegen_arm.cpp", "compemu_midfunc_arm.cpp",
                  "compemu_support_arm.cpp", "compemu_arm.cpp",
                  "compemu_fpp_arm.cpp"):
        hits = [(i, l.strip()) for i, l in enumerate(read(fname).splitlines(), 1)
                if "R_MEMSTART" in l]
        if hits:
            print("     %s: %d line(s)" % (fname, len(hits)))
            for i, l in hits:
                print("       :%-6d %s" % (i, l))

    # --- (a4)/(a5) the compile table ---
    src = read("compemu_arm.cpp")
    starts = [(m.start(), m.group(1)) for m in re.finditer(
        r"uae_u32 REGPARAM2 (op_[0-9a-f]+_\d+_comp_(?:ff|nf))\(uae_u32 opcode\) \{", src)]
    bodies = {}
    for idx, (pos, name) in enumerate(starts):
        end = starts[idx + 1][0] if idx + 1 < len(starts) else len(src)
        bodies[name] = src[pos:end]

    api_re = re.compile(r"\b(" + "|".join(MEM_API) + r")\s*\(")
    touches = {n: set(api_re.findall(b)) for n, b in bodies.items()}
    calls = {n: len(api_re.findall(b)) for n, b in bodies.items()}

    print("\n(a4) per-opcode compilers defined in compemu_arm.cpp: %d" % len(bodies))

    tables, cur_t = collections.OrderedDict(), None
    for line in read("compstbl_arm.cpp").splitlines():
        m = re.match(r"extern const struct comptbl (\w+)\[\] = \{", line)
        if m:
            cur_t = m.group(1)
            tables[cur_t] = []
            continue
        if cur_t:
            m = re.match(r"\{\s*(NULL|op_[0-9a-f]+_\d+_comp_(?:ff|nf))\s*,.*?/\* (\S+) \*/",
                         line)
            if m:
                tables[cur_t].append((m.group(1), m.group(2)))
            if line.startswith("};"):
                cur_t = None

    for tname, ents in tables.items():
        nn = [e for e in ents if e[0] != "NULL"]
        reach = [e for e in nn if touches.get(e[0])]
        print("\n(a5) table %s" % tname)
        print("     entries                 : %d" % len(ents))
        print("     NULL (no compiler)      : %d  (%.2f %%)"
              % (len(ents) - len(nn), 100.0 * (len(ents) - len(nn)) / len(ents)))
        print("     compilers               : %d" % len(nn))
        print("     ... can reach memory    : %d  (%.2f %% of compilers)"
              % (len(reach), 100.0 * len(reach) / len(nn)))
        print("     ... pure register ops   : %d  (%.2f %%)"
              % (len(nn) - len(reach), 100.0 * (len(nn) - len(reach)) / len(nn)))

        fam, famr = collections.Counter(), collections.Counter()
        for fn, mn in nn:
            fam[mn] += 1
            if touches.get(fn):
                famr[mn] += 1
        pure = sorted(m for m in fam if famr[m] == 0)
        allm = sorted(m for m in fam if famr[m] == fam[m])
        mixed = sorted(m for m in fam if 0 < famr[m] < fam[m])
        print("     mnemonic families       : %d" % len(fam))
        print("       fully register-only   : %2d  %s" % (len(pure), " ".join(pure)))
        print("       fully memory-touching : %2d  %s" % (len(allm), " ".join(allm)))
        print("       mixed (per shape)     : %2d  %s" % (len(mixed), " ".join(mixed)))
        print("     generation-one candidates (Q7):")
        for m in ("MOVE", "MOVEA", "Bcc", "DBcc"):
            if m in fam:
                print("       %-6s entries %4d   reach memory %4d   pure %4d"
                      % (m, fam[m], famr[m], fam[m] - famr[m]))

    # --- (a6) faultable-site density, the denominator M1's cost measurement needs ---
    hist = collections.Counter(calls.values())
    total = sum(calls.values())
    withmem = sum(v for k, v in hist.items() if k > 0)
    print("\n(a6) faultable sites per compiler body (both tables, %d bodies)" % len(bodies))
    print("     total guest-memory call sites : %d" % total)
    print("     mean per body                 : %.2f" % (total / float(len(bodies))))
    print("     mean per memory-touching body : %.2f" % (total / float(withmem)))
    print("     distribution                  : %s"
          % ", ".join("%d site(s): %d" % (k, hist[k]) for k in sorted(hist)))

    agg = collections.Counter()
    for used in touches.values():
        for u in used:
            agg[u] += 1
    print("\n     API symbol usage (compiler bodies calling it, both tables):")
    for k in MEM_API:
        print("       %-20s %d" % (k, agg.get(k, 0)))


def census_b():
    print()
    print("=" * 78)
    print("CENSUS (b) -- where guest state lives during a block")
    print("=" * 78)

    hdr = read("compemu_arm.h")
    sup = read("compemu_support_arm.cpp")
    cg = read("codegen_arm.cpp")

    def find(text, pattern, label):
        for i, line in enumerate(text.splitlines(), 1):
            if re.search(pattern, line):
                print("     %-34s :%-6d %s" % (label, i, line.strip()))
                return
        print("     %-34s NOT FOUND" % label)

    print("\n(b1) the virtual register file")
    find(hdr, r"#define VREGS", "VREGS")
    for nm in ("PC_P", "FLAGX", "FLAGTMP", "S1", "S2", "S3"):
        find(hdr, r"#define %s\s+\d" % nm, nm)
    for nm in ("INMEM", "CLEAN", "DIRTY", "UNDEF", "ISCONST"):
        find(hdr, r"#define %s\s+\d" % nm, "status " + nm)

    print("\n(b2) virtual -> home-location binding (init_comp)")
    for pat, lbl in ((r"live\.state\[i\]\.mem = &regs\.regs\[i\]", "v0..v15 -> regs.regs[]"),
                     (r"live\.state\[PC_P\]\.mem", "PC_P -> regs.pc_p (HOST ptr)"),
                     (r"live\.state\[FLAGX\]\.mem", "FLAGX -> regs.ccrflags.x"),
                     (r"live\.state\[FLAGTMP\]\.mem", "FLAGTMP -> regs.ccrflags.nzcv"),
                     (r"live\.state\[i\]\.mem = &regs\.scratchregs", "S1..S3 -> scratchregs[]")):
        find(sup, pat, lbl)

    print("\n(b3) host register budget")
    find(cg, r"#define R_MEMSTART", "R_MEMSTART")
    find(cg, r"#define R_REGSTRUCT", "R_REGSTRUCT")
    find(cg, r"uae_s8 always_used\[\]", "always_used[] (locked regs)")
    find(hdr, r"#define N_REGS 11", "N_REGS (nominal)")
    m = re.search(r"uae_s8 always_used\[\]=\{([^}]*)\}", cg)
    locked = [t.strip() for t in m.group(1).split(",") if t.strip() not in ("-1",)]
    print("     -> locked: %s  => allocatable host regs: %d of r0-r12"
          % (", ".join(locked), 13 - len(locked)))

    print("\n(b4) the writeback / sync machinery")
    for pat, lbl in ((r"^void sync_m68k_pc", "sync_m68k_pc (lazy PC fold)"),
                     (r"^static void flags_to_stack", "flags_to_stack"),
                     (r"^STATIC_INLINE void clobber_flags\(void\)$", "clobber_flags"),
                     (r"^STATIC_INLINE void flush_flags", "flush_flags"),
                     (r"^static void tomem", "tomem (DIRTY -> home)"),
                     (r"^STATIC_INLINE void writeback_const", "writeback_const"),
                     (r"^static void evict", "evict"),
                     (r"^static void flush\(int save_regs\)", "flush(save_regs)"),
                     (r"^static void flush_all", "flush_all"),
                     (r"^static void prepare_for_call_1\(void\)$", "prepare_for_call_1"),
                     (r"^static void prepare_for_call_2\(void\)$", "prepare_for_call_2")):
        find(sup, pat, lbl)

    print("\n(b5) what the interpreter's boundary checkpoint needs, and whether jit/ has it")
    files = sorted(f for f in os.listdir(JIT)
                   if f.endswith((".c", ".cpp", ".h")))
    blob = "".join(read(f) for f in files)
    print("     (scanning all %d files in jit/: %s)" % (len(files), ", ".join(files)))
    for pat, note in ((r"regs\.regs\[", "16 data/address registers"),
                      (r"ccrflags", "regs.ccrflags (nzcv + x)"),
                      (r"regs\.pc_p\b", "regs.pc_p -- HOST pointer, not the guest PC"),
                      (r"regs\.pc\b(?!_)", "regs.pc -- the guest logical PC"),
                      (r"\binstruction_pc\b", "regs.instruction_pc -- the restart PC"),
                      (r"\bmmu_restart\b", "mmu_restart"),
                      (r"\bmmufixup\b", "mmufixup[0..1]")):
        n = len(re.findall(pat, blob))
        print("     %-46s %s (%d occurrence(s) in jit/)"
              % (note, "PRESENT" if n else "ABSENT ", n))

    # (b6) regs.pc IS written -- but only at block terminators with a runtime target.
    # Worth pinning precisely: it is the difference between "the primitive does not exist"
    # and "the primitive exists and the discipline does not".
    src = read("compemu_arm.cpp")
    starts = [(m.start(), m.group(1)) for m in re.finditer(
        r"uae_u32 REGPARAM2 (op_[0-9a-f]+_\d+_comp_(?:ff|nf))\(uae_u32 opcode\) \{", src)]
    mn = {}
    for line in read("compstbl_arm.cpp").splitlines():
        m = re.match(r"\{\s*(op_[0-9a-f]+_\d+_comp_(?:ff|nf))\s*,.*?/\* (\S+) \*/", line)
        if m:
            mn[m.group(1)] = m.group(2)
    hits, sites = collections.Counter(), 0
    for idx, (pos, name) in enumerate(starts):
        end = starts[idx + 1][0] if idx + 1 < len(starts) else len(src)
        n = len(re.findall(r"regs\.pc\b(?!_)", src[pos:end]))
        if n:
            hits[mn.get(name, "?")] += 1
            sites += n
    print("\n(b6) which per-opcode compilers write regs.pc (the guest logical PC)")
    print("     compiler bodies: %d   store sites: %d   (both tables)"
          % (sum(hits.values()), sites))
    for k in sorted(hits):
        print("       %-6s %d bodies" % (k, hits[k]))
    print("     -> every one is a block terminator whose target is not a compile-time")
    print("        constant. No intra-block instruction boundary writes it.")


def main():
    if not os.path.isdir(JIT):
        sys.stderr.write("jit/ not found at %s\n" % JIT)
        return 1
    census_a()
    census_b()
    print()
    print("census (c) checks single claims against single lines and is not scripted; "
          "its cites are in Sec 0.3.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
