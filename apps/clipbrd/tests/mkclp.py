#!/usr/bin/env python3
"""Writes the .CLP test files for the Clipboard Viewer tests (our own content, in the 3.1 .CLP
layout the viewer reads: WORD 0xC350, WORD count, then count 89-byte entries {WORD format, DWORD
length, DWORD offset, char name[79]} and the data). The same files go to the reference rig
(ref-run.ps1 -Files) and to the port's C: fixture, so both viewers open identical clipboards.

    mkclp.py OUTDIR      writes CBBMP.CLP CBMONO.CLP CBWMF.CLP CBTEXT.CLP CBSYLK.CLP CBREG.CLP
"""
import struct
import sys
import os

# the 16 VGA colours in VGA.DRV's index order (black, dark red, dark green, ...), as RGB
VGA = [(0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0), (0, 0, 128), (128, 0, 128), (0, 128, 128),
       (128, 128, 128), (192, 192, 192), (255, 0, 0), (0, 255, 0), (255, 255, 0), (0, 0, 255),
       (255, 0, 255), (0, 255, 255), (255, 255, 255)]


def clp(entries):
    """entries: [(format, name, bytes)]"""
    head = struct.pack('<HH', 0xC350, len(entries))
    off = 4 + 89 * len(entries)
    dirs, datas = b'', b''
    for fmt, name, data in entries:
        nm = name.encode('ascii')[:78]
        dirs += struct.pack('<HII', fmt, len(data), off) + nm + b'\0' * (79 - len(nm))
        datas += data
        off += len(data)
    return head + dirs + datas


def bitmap_planar(w, h, pix):
    """CF_BITMAP data: the 14-byte BITMAP (planes 4, 1 bit) and VGA.DRV's bits - each scan line's
    four planes one after the other, plane p = bit p of the pixel's colour index"""
    wb = (w + 15) // 16 * 2
    bits = bytearray()
    for y in range(h):
        for p in range(4):
            row = bytearray(wb)
            for x in range(w):
                if pix(x, y) >> p & 1:
                    row[x // 8] |= 0x80 >> (x & 7)
            bits += row
    return struct.pack('<hhhhBBI', 0, w, h, wb, 4, 1, 0) + bytes(bits)


def bitmap_mono(w, h, pix):
    wb = (w + 15) // 16 * 2
    bits = bytearray()
    for y in range(h):
        row = bytearray(wb)
        for x in range(w):
            if pix(x, y):
                row[x // 8] |= 0x80 >> (x & 7)
        bits += row
    return struct.pack('<hhhhBBI', 0, w, h, wb, 1, 1, 0) + bytes(bits)


def dib4(w, h, pix, colors):
    """CF_DIB: BITMAPINFOHEADER, 16 RGBQUADs, 4-bit rows bottom-up padded to 4 bytes"""
    hdr = struct.pack('<IiiHHIIiiII', 40, w, h, 1, 4, 0, 0, 0, 0, 0, 0)
    pal = b''.join(struct.pack('BBBB', b, g, r, 0) for r, g, b in colors)
    stride = ((w * 4 + 31) // 32) * 4
    bits = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray(stride)
        for x in range(w):
            v = pix(x, y) & 15
            row[x // 2] |= v << 4 if x % 2 == 0 else v
        bits += row
    return hdr + pal + bytes(bits)


def palette(cols):
    return struct.pack('<HH', 0x300, len(cols)) + b''.join(struct.pack('BBBB', r, g, b, 0) for r, g, b in cols)


def rec(fn, *params):
    return struct.pack('<IH', 3 + len(params), fn) + b''.join(struct.pack('<H', p & 0xFFFF) for p in params)


def metafile(records, nobj):
    body = b''.join(records) + rec(0)
    maxrec = max(struct.unpack_from('<I', r)[0] for r in records + [rec(0)])
    size = 9 + len(body) // 2
    return struct.pack('<HHHIHIH', 1, 9, 0x300, size, nobj, maxrec, 0) + body


def text_out(x, y, s):
    b = s.encode('ascii')
    if len(b) % 2:
        b += b'\0'
    words = struct.unpack('<%dH' % (len(b) // 2), b)
    return rec(0x0521, len(s), *words, y, x)


def main(out):
    os.makedirs(out, exist_ok=True)

    def stripes(x, y):
        if y < 20:
            return x // 4 % 16
        return 15 if (x // 8 + y // 5) % 2 else 9 + (y - 20) // 5 % 6

    dib_cols = VGA

    def bands(x, y):
        if x == 0 or y == 0 or x == 47 or y == 31:
            return 0
        return 1 + (y // 4) % 15 if x < 24 else 15 - (x // 3) % 16

    pal = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (0, 255, 255), (255, 0, 255),
           (128, 0, 0), (0, 128, 0), (0, 0, 128), (128, 128, 0), (0, 128, 128), (128, 0, 128),
           (255, 255, 255), (192, 192, 192), (128, 128, 128), (0, 0, 0),
           (255, 128, 0), (128, 255, 0), (0, 128, 255), (255, 0, 128), (64, 64, 64), (200, 100, 50)]
    with open(os.path.join(out, 'CBBMP.CLP'), 'wb') as f:
        f.write(clp([(2, '&Bitmap', bitmap_planar(64, 40, stripes)),
                     (8, '&DIB Bitmap', dib4(48, 32, bands, dib_cols)),
                     (9, 'Pal&ette', palette(pal))]))

    def ring(x, y):
        return ((x - 40) ** 2 + (y - 25) ** 2) // 60 % 2
    with open(os.path.join(out, 'CBMONO.CLP'), 'wb') as f:
        f.write(clp([(2, '&Bitmap', bitmap_mono(83, 51, ring))]))

    recs = [rec(0x020B, 0, 0), rec(0x020C, 200, 400),
            rec(0x02FA, 0, 1, 0, 0, 0),                         # pen: solid, 1, black
            rec(0x02FC, 0, 0x00FF, 0x0000, 0),                  # brush: solid red
            rec(0x012D, 0), rec(0x012D, 1),
            rec(0x041B, 120, 180, 20, 20),                      # Rectangle(20, 20, 180, 120)
            rec(0x02FC, 0, 0x0000, 0x00FF, 0),                  # brush: solid blue
            rec(0x012D, 2),
            rec(0x0418, 180, 380, 40, 200),                     # Ellipse(200, 40, 380, 180)
            rec(0x0214, 0, 0), rec(0x0213, 200, 400),           # MoveTo(0, 0) LineTo(400, 200)
            rec(0x0102, 1),                                     # SetBkMode(TRANSPARENT)
            text_out(30, 150, 'Picture')]
    with open(os.path.join(out, 'CBWMF.CLP'), 'wb') as f:
        f.write(clp([(3, '&Picture', struct.pack('<hhhH', 8, 4000, 2000, 0) + metafile(recs, 3))]))

    lines = ['Line %d of the clipboard text\tTab' % i for i in range(1, 41)]
    lines[2] = 'A long line that is wider than the window, so the viewer breaks it where it no longer fits.'
    text = ('\r\n'.join(lines) + '\r\n').encode('ascii') + b'\0'
    oem = b'OEM: \xc9\xcd\xcd\xbb \xb0\xb1\xb2\xdb \x81\x84\x94\r\n\xc8\xcd\xcd\xbc\0'
    with open(os.path.join(out, 'CBTEXT.CLP'), 'wb') as f:
        f.write(clp([(1, '&Text', text), (7, '&OEM Text', oem)]))

    with open(os.path.join(out, 'CBSYLK.CLP'), 'wb') as f:
        f.write(clp([(4, '&Sylk', b'ID;PWXL;N;E\r\nC;Y1;X1;K1\r\nE\r\n\0')]))

    with open(os.path.join(out, 'CBREG.CLP'), 'wb') as f:
        f.write(clp([(0xC123, 'arch311 test data', b'private bytes\0'),
                     (5, '&DIF', b'TABLE\r\n0,1\r\n""\r\n\0')]))

    # a text file for Notepad to copy from
    with open(os.path.join(out, 'CBNOTE.TXT'), 'wb') as f:
        f.write(b'Copied from Notepad.\r\nSecond line\twith a tab\r\n'
                b'A line long enough to be broken by the Clipboard Viewer at its right edge.\r\nEnd')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main(sys.argv[1])
