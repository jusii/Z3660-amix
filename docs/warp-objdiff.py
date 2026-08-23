#!/usr/bin/env python3
"""Function-granularity object diff for two builds of the same firmware.

WHY THIS EXISTS. A switch that ships disarmed makes a claim -- "with the engine off this
binary does what the previous one did" -- and that claim is about OBJECT CODE, not about a
source diff. A source diff shows what changed; it says nothing about what the compiler did
with it, and this repository has already been bitten by the gap in both directions (a
`-Os/-flto` flag change alone once produced stale I-cache trampolines and a reboot).

Whole-image byte identity is the right test after a REVERT -- and it has been used as one
here, twice. It is the wrong test after an ADDITION: a new translation unit moves every
later function's address, so every branch encoding in the image changes and a byte compare
reports "everything differs" while telling you nothing. So this compares at FUNCTION
granularity, with addresses normalised away:

  * the address column and the raw instruction encoding are dropped;
  * a branch target `1234 <foo+0x8>` becomes `<foo+0x8>`, so a function that moved but was
    not recompiled compares equal;
  * literal-pool `.word` values are kept but their address operands normalised the same way;
  * functions present in only one build are reported separately from functions that differ.

The output is the number this round is scored on: how many functions changed, and which.

Usage:
    docs/warp-objdiff.py BASE.elf NEW.elf [--list] [--relaxed]

Requires arm-none-eabi-objdump on PATH.
"""
import re
import subprocess
import sys

FUNC_RE = re.compile(r'^([0-9a-f]+) <(.+)>:$')
INSN_RE = re.compile(r'^\s*[0-9a-f]+:\t([0-9a-f ]+)\t(.*)$')
# A branch/loadaddr target printed as "<addr> <symbol+0xoff>"; the address is position, the
# symbol is identity, so keep the second and drop the first.
TARGET_RE = re.compile(r'\b[0-9a-f]{4,8} (<[^>]+>)')
# A bare hex address inside a comment (objdump prints "@ 3001234 <foo>" and similar).
ADDR_RE = re.compile(r'\b[0-9a-f]{7,8}\b')
# THE ONE THAT MATTERS ON THIS TARGET. The build passes -DARMV6T2, so there is no literal
# pool: every address a function names -- including every .bss and .data object -- is baked
# into the instruction stream as a movw/movt immediate pair. Adding ONE global therefore
# rewrites the immediates in every function that touches any global placed after it, and a
# naive text compare reports thousands of "changes" that are pure relocation.
#
# So the pair is resolved back to the SYMBOL it names, through the ELF symbol table, and the
# comparison becomes "materialises the address of X" rather than "materialises 0x3061...".
# That is the right abstraction and it is also strictly stronger than dropping the
# immediates: a function that started naming a DIFFERENT object still compares unequal.
MOVW_RE = re.compile(r'^movw(\w*)\s+(\w+), #(\d+)')
MOVT_RE = re.compile(r'^movt(\w*)\s+(\w+), #(\d+)')
WORD_RE = re.compile(r'^\.word\s+0x([0-9a-f]{8})$')
# core1's DDR window: lscript.ld puts ORIGIN at 0x30000000 with LENGTH 0x0FF00000. A .word
# holding a value in here is an address, not data.
IMAGE_LO = 0x30000000
IMAGE_HI = 0x40000000


def symbol_table(path):
    """Sorted (address, size, name) for every defined symbol with a size.

    `nm -S` rather than `objdump -t`, because the SIZE is what makes the lookup exact: a
    nearest-symbol-below search resolves a floating-point constant in a literal pool to
    whatever object happens to precede it, which is worse than not resolving it at all.
    """
    out = subprocess.run(['arm-none-eabi-nm', '-S', '--defined-only', path],
                         capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 4:
            continue
        try:
            addr = int(parts[0], 16)
            size = int(parts[1], 16)
        except ValueError:
            continue
        syms.append((addr, size, parts[3]))
    syms.sort()
    return syms


def resolve(syms, addr):
    """addr -> '<symbol+0xoff>', or None when it is inside no sized symbol."""
    import bisect
    i = bisect.bisect_right(syms, (addr, 1 << 62, '')) - 1
    if i < 0:
        return None
    base, size, name = syms[i]
    if size == 0 or addr >= base + size:
        return None
    off = addr - base
    return '<%s+0x%x>' % (name, off) if off else '<%s>' % name


def disassemble(path, relaxed=False):
    out = subprocess.run(['arm-none-eabi-objdump', '-d', path],
                         capture_output=True, text=True, check=True).stdout
    syms = symbol_table(path)
    funcs = {}
    cur = None
    body = []
    pending = {}
    for line in out.splitlines():
        m = FUNC_RE.match(line)
        if m:
            if cur is not None:
                funcs[cur] = body
            cur = m.group(2)
            body = []
            pending = {}
            continue
        m = INSN_RE.match(line)
        if not m or cur is None:
            continue
        text = m.group(2)
        if text.startswith(('movw', 'movt')):
            text = text.split('@')[0].strip()

        if relaxed:
            # RELAXED MODE: canonicalise every movw/movt immediate outright. On this target
            # movw/movt is the ONLY way an address enters the instruction stream, so this
            # removes relocation entirely -- at the price of making a changed CONSTANT
            # invisible. What it then proves is that the instruction stream's shape and
            # every branch target are unchanged, which is the half of the claim a linked
            # image can honestly support once -flto has re-run whole-program optimisation.
            mw = MOVW_RE.match(text)
            mt = MOVT_RE.match(text)
            if mw:
                body.append('movw%s %s, #K' % (mw.group(1), mw.group(2)))
                continue
            if mt:
                body.append('movt%s %s, #K' % (mt.group(1), mt.group(2)))
                continue

        mw = MOVW_RE.match(text)
        if mw:
            # Remember WHERE the low half was emitted; the matching movt may be several
            # instructions later in predicated code, and patching the pair in place keeps
            # the instruction count -- and therefore the comparison -- honest.
            pending[(mw.group(1), mw.group(2))] = (len(body), int(mw.group(3)))
            body.append('movw%s %s, <lo>' % (mw.group(1), mw.group(2)))
            continue
        mt = MOVT_RE.match(text)
        if mt and (mt.group(1), mt.group(2)) in pending:
            idx, lo = pending.pop((mt.group(1), mt.group(2)))
            addr = (int(mt.group(3)) << 16) | lo
            sym = resolve(syms, addr)
            tag = sym if sym else 'ADDR'
            body[idx] = 'movw%s %s, %s' % (mt.group(1), mt.group(2), tag)
            body.append('movt%s %s, ^' % (mt.group(1), mt.group(2)))
            continue

        ww = WORD_RE.match(text.strip())
        if ww:
            val = int(ww.group(1), 16)
            sym = resolve(syms, val)
            if sym:
                body.append('.word %s' % sym)
            elif relaxed and IMAGE_LO <= val < IMAGE_HI:
                # An address word that names no SIZED symbol -- a static local, a string
                # literal, a jump-table entry. It is a relocation like any other and it
                # moves when anything before it does.
                body.append('.word ADDR')
            else:
                body.append(text.strip())
            continue

        text = TARGET_RE.sub(r'\1', text)
        text = ADDR_RE.sub('@', text)
        body.append(text.strip())
    if cur is not None:
        funcs[cur] = body
    return funcs


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    base, new = sys.argv[1], sys.argv[2]
    show = '--list' in sys.argv

    relaxed = '--relaxed' in sys.argv
    a = disassemble(base, relaxed)
    b = disassemble(new, relaxed)

    only_a = sorted(set(a) - set(b))
    only_b = sorted(set(b) - set(a))
    common = sorted(set(a) & set(b))
    changed = [f for f in common if a[f] != b[f]]

    print(f'base : {base}')
    print(f'new  : {new}')
    print(f'functions, base {len(a)}   new {len(b)}   common {len(common)}')
    print(f'CHANGED (normalised body differs) : {len(changed)}')
    print(f'only in base                      : {len(only_a)}')
    print(f'only in new                       : {len(only_b)}')
    if show or len(changed) <= 40:
        for f in changed:
            print(f'  ~ {f}   {len(a[f])} -> {len(b[f])} instructions')
    if show:
        for f in only_a:
            print(f'  - {f}')
        for f in only_b:
            print(f'  + {f}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
