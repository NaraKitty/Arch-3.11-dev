#!/usr/bin/env python3
"""clpcmp.py PORT.CLP REF.CLP - compares two .CLP files byte for byte, except the bytes real 3.1
cannot reproduce: what follows each name's terminator in the 79-byte name field (3.1 builds the
entry on its stack, so that is stack garbage) and a picture's hMF word (a handle value). Exit 0 if
equal; otherwise prints the first differences and exits 1."""
import struct
import sys


def masked(path):
    d = bytearray(open(path, 'rb').read())
    if len(d) < 4:
        return d
    magic, n = struct.unpack_from('<HH', d)
    for i in range(n):
        at = 4 + 89 * i
        if at + 89 > len(d):
            break
        fmt, ln, off = struct.unpack_from('<HII', d, at)
        name = d[at + 10:at + 89]
        end = name.find(0)
        if end >= 0:
            d[at + 10 + end:at + 89] = bytes(89 - 10 - end)
        if fmt == 3 and off + 8 <= len(d):        # CF_METAFILEPICT: mm, xExt, yExt, hMF
            d[off + 6:off + 8] = b'\0\0'
    return d


a, b = masked(sys.argv[1]), masked(sys.argv[2])
if a == b:
    print('same (%d bytes)' % len(a))
    sys.exit(0)
print('differ: %d vs %d bytes' % (len(a), len(b)))
shown = 0
for i in range(min(len(a), len(b))):
    if a[i] != b[i]:
        print('  offset %d: %02x vs %02x' % (i, a[i], b[i]))
        shown += 1
        if shown == 10:
            break
sys.exit(1)
