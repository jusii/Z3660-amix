#!/usr/bin/env python3
"""fixsum.py <in.raw> <out.rom> -- pad ztest to 512 KB and fix the Kickstart-style checksum.

The longword at ROM_END-$18 is set so that the add-with-carry sum of every longword is
$FFFFFFFF, the same rule a Kickstart ROM follows. Path 8 of the test program checks its own
image in 4 KB chunks against sums it takes at sizing; this whole-image rule lets the host
confirm the file before it goes to the card.
"""
import struct
import sys

SIZE = 0x80000
OFF = SIZE - 0x18


def kssum(b):
    s = 0
    for (w,) in struct.iter_unpack('>I', b):
        s += w
        if s > 0xFFFFFFFF:
            s = (s & 0xFFFFFFFF) + 1
    return s


def main():
    raw = open(sys.argv[1], 'rb').read()
    if len(raw) > SIZE:
        sys.exit('image larger than 512 KB: %d' % len(raw))
    b = bytearray(raw.ljust(SIZE, b'\0'))
    b[OFF:OFF + 4] = b'\0\0\0\0'
    s = kssum(bytes(b))
    b[OFF:OFF + 4] = struct.pack('>I', (~s) & 0xFFFFFFFF)
    assert kssum(bytes(b)) == 0xFFFFFFFF
    open(sys.argv[2], 'wb').write(b)


if __name__ == '__main__':
    main()
