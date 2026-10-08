"""Pixel comparison of a libw16 port screenshot against a real-3.11 reference (no PIL needed).

  compare.py diff REF.png PORT.png OUT.png [--ignore x0,y0,x1,y1 ...]
      prints the number of differing pixels and their bounding boxes; writes REF | PORT | DIFF
      (differences red over a dimmed reference)
  compare.py runs FILE.png row Y [x0 x1]      colour runs along a row  (x0-x1 #rrggbb)
  compare.py runs FILE.png col X [y0 y1]      colour runs along a column
  compare.py crop FILE.png x0 y0 x1 y1 SCALE OUT.png
"""
import struct
import sys
import zlib


def read_png(path):
    d = open(path, 'rb').read()
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(path + ': not a PNG')
    i, idat, w, h, ctype = 8, b'', 0, 0, 2
    while i < len(d):
        n, t = struct.unpack('>I4s', d[i:i + 8])
        c = d[i + 8:i + 8 + n]
        i += 12 + n
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', c[:10])
            if depth != 8 or ctype not in (2, 6):
                raise ValueError(path + ': only 8-bit RGB/RGBA PNGs')
        elif t == b'IDAT':
            idat += c
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    s = w * bpp
    prev = bytearray(s)
    px = bytearray(w * h * 3)
    for y in range(h):
        f = raw[y * (s + 1)]
        line = bytearray(raw[y * (s + 1) + 1:(y + 1) * (s + 1)])
        for x in range(s):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        for x in range(w):
            px[(y * w + x) * 3:(y * w + x) * 3 + 3] = line[x * bpp:x * bpp + 3]
    return w, h, px


def write_png(path, w, h, px):
    raw = b''.join(b'\0' + bytes(px[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(t, c):
        return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def color(px, w, x, y):
    i = (y * w + x) * 3
    return '#%02x%02x%02x' % (px[i], px[i + 1], px[i + 2])


def boxes(mask, w, h, gap=4):
    """Group differing pixels into bounding boxes (simple flood over a coarse grid)."""
    cell = gap
    gw, gh = (w + cell - 1) // cell, (h + cell - 1) // cell
    grid = [[False] * gw for _ in range(gh)]
    for y in range(h):
        for x in range(w):
            if mask[y * w + x]:
                grid[y // cell][x // cell] = True
    seen = [[False] * gw for _ in range(gh)]
    out = []
    for gy in range(gh):
        for gx in range(gw):
            if grid[gy][gx] and not seen[gy][gx]:
                stack, x0, y0, x1, y1 = [(gx, gy)], gx, gy, gx, gy
                seen[gy][gx] = True
                while stack:
                    cx, cy = stack.pop()
                    x0, y0, x1, y1 = min(x0, cx), min(y0, cy), max(x1, cx), max(y1, cy)
                    for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                        if 0 <= nx < gw and 0 <= ny < gh and grid[ny][nx] and not seen[ny][nx]:
                            seen[ny][nx] = True
                            stack.append((nx, ny))
                out.append((x0 * cell, y0 * cell, min(w, (x1 + 1) * cell), min(h, (y1 + 1) * cell)))
    return out


def cmd_diff(args):
    ref, port, outp = args[0], args[1], args[2]
    ignore = []
    rest = args[3:]
    while rest:
        if rest[0] == '--ignore':
            ignore.append(tuple(int(v) for v in rest[1].split(',')))
            rest = rest[2:]
        else:
            raise SystemExit('unknown option ' + rest[0])
    w, h, a = read_png(ref)
    w2, h2, b = read_png(port)
    if (w, h) != (w2, h2):
        raise SystemExit('size differs: %dx%d vs %dx%d' % (w, h, w2, h2))
    mask = bytearray(w * h)
    n = 0
    for y in range(h):
        for x in range(w):
            if any(x0 <= x < x1 and y0 <= y < y1 for x0, y0, x1, y1 in ignore):
                continue
            i = (y * w + x) * 3
            if a[i:i + 3] != b[i:i + 3]:
                mask[y * w + x] = 1
                n += 1
    out = bytearray(w * 3 * h * 3)
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 3
            o = (y * w * 3 + x) * 3
            out[o:o + 3] = a[i:i + 3]
            out[o + w * 3:o + w * 3 + 3] = b[i:i + 3]
            if mask[y * w + x]:
                out[o + w * 6:o + w * 6 + 3] = b'\xff\x00\x00'
            else:
                out[o + w * 6:o + w * 6 + 3] = bytes(128 + v // 2 for v in a[i:i + 3])
    write_png(outp, w * 3, h, out)
    print('%d differing pixels (%.2f%%)' % (n, 100.0 * n / (w * h)))
    for x0, y0, x1, y1 in boxes(mask, w, h)[:40]:
        print('  box x=%d..%d y=%d..%d' % (x0, x1, y0, y1))


def cmd_runs(args):
    w, h, px = read_png(args[0])
    kind, pos = args[1], int(args[2])
    lo = int(args[3]) if len(args) > 3 else 0
    hi = int(args[4]) if len(args) > 4 else (w if kind == 'row' else h)
    cur, start = None, lo
    for t in range(lo, hi + 1):
        c = None if t == hi else (color(px, w, t, pos) if kind == 'row' else color(px, w, pos, t))
        if c != cur:
            if cur is not None:
                print('%4d-%-4d %s (%d)' % (start, t - 1, cur, t - start))
            cur, start = c, t


def cmd_crop(args):
    w, h, px = read_png(args[0])
    x0, y0, x1, y1, k = (int(v) for v in args[1:6])
    out = bytearray()
    for y in range(y0, y1):
        line = bytearray()
        for x in range(x0, x1):
            i = (y * w + x) * 3
            line += bytes(px[i:i + 3]) * k
        out += line * k
    write_png(args[6], (x1 - x0) * k, (y1 - y0) * k, out)


if __name__ == '__main__':
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    {'diff': cmd_diff, 'runs': cmd_runs, 'crop': cmd_crop}[sys.argv[1]](sys.argv[2:])
