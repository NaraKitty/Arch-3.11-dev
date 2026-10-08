"""Microsoft KWAJ / SZDD decompressors (the formats used by Windows 3.x setup disks).

Pure Python, no dependencies. Written from the publicly documented formats
(libmspack's format notes). Only decompression is implemented.

KWAJ methods:
  0 = stored, 1 = XOR 0xFF, 2 = SZDD-style LZSS, 3 = LZ + Huffman ("LZH"), 4 = MS-ZIP
Windows 3.11 floppies use method 3 for every compressed file.
"""
import struct
import zlib

KWAJ_SIG = b"KWAJ\x88\xf0\x27\xd1"
SZDD_SIG = b"SZDD\x88\xf0\x27\x33"


class FormatError(Exception):
    pass


# --------------------------------------------------------------------------- bit reader
class _Bits:
    """MSB-first bit reader. Reading past the end yields zero bits and sets .eof
    (mirrors libmspack, which pads the stream with zeros and stops after the
    token that hit the end)."""

    def __init__(self, data, pos):
        self.data = data
        self.pos = pos
        self.buf = 0
        self.n = 0
        self.eof = False

    def _fill(self, need):
        while self.n < need:
            if self.pos < len(self.data):
                self.buf = (self.buf << 8) | self.data[self.pos]
                self.pos += 1
            else:
                self.buf <<= 8
                self.eof = True
            self.n += 8

    def peek(self, k):
        self._fill(k)
        return (self.buf >> (self.n - k)) & ((1 << k) - 1)

    def skip(self, k):
        self.n -= k
        self.buf &= (1 << self.n) - 1

    def read(self, k):
        if k == 0:
            return 0
        v = self.peek(k)
        self.skip(k)
        return v


class _Huff:
    """Canonical Huffman table with a flat lookup of 2**maxlen entries."""

    def __init__(self, lens):
        self.maxlen = max(lens) if lens else 0
        if self.maxlen == 0:
            raise FormatError("empty huffman table")
        size = 1 << self.maxlen
        table = [None] * size
        code = 0
        for L in range(1, self.maxlen + 1):
            for sym, l in enumerate(lens):
                if l != L:
                    continue
                span = 1 << (self.maxlen - L)
                start = code << (self.maxlen - L)
                if start + span > size:
                    raise FormatError("over-subscribed huffman table")
                for i in range(start, start + span):
                    table[i] = (sym, L)
                code += 1
            code <<= 1
        self.table = table

    def decode(self, bits):
        e = self.table[bits.peek(self.maxlen)]
        if e is None:
            raise FormatError("bad huffman code")
        bits.skip(e[1])
        return e[0]


def _read_lens(bits, kind, numsyms):
    if kind == 0:
        c = {16: 4, 32: 5, 64: 6, 256: 8}[numsyms]
        return [c] * numsyms
    lens = [0] * numsyms
    if kind == 1:
        c = bits.read(4)
        lens[0] = c
        for i in range(1, numsyms):
            if bits.read(1) == 0:
                lens[i] = c
            elif bits.read(1) == 0:
                c += 1
                lens[i] = c
            else:
                c = bits.read(4)
                lens[i] = c
    elif kind == 2:
        c = bits.read(4)
        lens[0] = c
        for i in range(1, numsyms):
            sel = bits.read(2)
            if sel == 3:
                c = bits.read(4)
            else:
                c = (c + sel - 1) & 0xFF
            lens[i] = c
    elif kind == 3:
        for i in range(numsyms):
            lens[i] = bits.read(4)
    else:
        raise FormatError("bad length-table type %d" % kind)
    return lens


def _kwaj_lzh(data, pos, outlen):
    bits = _Bits(data, pos)
    # six 4-bit table types are stored (the 6th is padding for byte alignment)
    types = [bits.read(4) for _ in range(6)]
    ml1 = _Huff(_read_lens(bits, types[0], 16))
    ml2 = _Huff(_read_lens(bits, types[1], 16))
    litlen = _Huff(_read_lens(bits, types[2], 32))
    offs = _Huff(_read_lens(bits, types[3], 64))
    lit = _Huff(_read_lens(bits, types[4], 256))

    out = bytearray()
    window = bytearray(b" " * 4096)
    wpos = 0
    lit_run = False
    while not bits.eof and (outlen is None or len(out) < outlen):
        n = (ml2 if lit_run else ml1).decode(bits)
        if n > 0:
            n += 2
            lit_run = False
            off = (offs.decode(bits) << 6) | bits.read(6)
            for _ in range(n):
                b = window[(wpos - off) & 4095]
                window[wpos] = b
                out.append(b)
                wpos = (wpos + 1) & 4095
        else:
            n = litlen.decode(bits) + 1
            lit_run = n != 32
            for _ in range(n):
                b = lit.decode(bits)
                window[wpos] = b
                out.append(b)
                wpos = (wpos + 1) & 4095
    if outlen is not None:
        if len(out) < outlen:
            raise FormatError("truncated stream (%d of %d bytes)" % (len(out), outlen))
        del out[outlen:]
    return bytes(out)


def _lzss(data, pos, outlen, start=4096 - 16):
    """SZDD / KWAJ method 2 LZSS."""
    out = bytearray()
    window = bytearray(b" " * 4096)
    wpos = start
    n = len(data)
    while pos < n:
        ctrl = data[pos]
        pos += 1
        for bit in range(8):
            if pos >= n:
                break
            if ctrl & (1 << bit):
                b = data[pos]
                pos += 1
                window[wpos] = b
                out.append(b)
                wpos = (wpos + 1) & 4095
            else:
                if pos + 1 >= n:
                    pos = n
                    break
                mpos = data[pos] | ((data[pos + 1] & 0xF0) << 4)
                mlen = (data[pos + 1] & 0x0F) + 3
                pos += 2
                for _ in range(mlen):
                    b = window[mpos]
                    window[wpos] = b
                    out.append(b)
                    mpos = (mpos + 1) & 4095
                    wpos = (wpos + 1) & 4095
    if outlen is not None:
        del out[outlen:]
    return bytes(out)


def is_compressed(data):
    return data[:8] in (KWAJ_SIG, SZDD_SIG)


def expand(data):
    """Return (bytes, original_name_or_None). Raises FormatError on bad input.
    Uncompressed input is returned unchanged."""
    if data[:8] == SZDD_SIG:
        if data[8:9] != b"A":
            raise FormatError("unknown SZDD method")
        missing = chr(data[9]) if data[9] else None
        outlen = struct.unpack_from("<I", data, 10)[0]
        return _lzss(data, 14, outlen), missing
    if data[:8] != KWAJ_SIG:
        return data, None

    method, dataoff, flags = struct.unpack_from("<HHH", data, 8)
    p = 14
    outlen = None
    name = ext = None
    if flags & 1:
        outlen = struct.unpack_from("<I", data, p)[0]
        p += 4
    if flags & 2:
        p += 2
    if flags & 4:
        p += 2 + struct.unpack_from("<H", data, p)[0]
    if flags & 8:
        e = data.index(b"\0", p)
        name = data[p:e].decode("latin-1")
        p = e + 1
    if flags & 16:
        e = data.index(b"\0", p)
        ext = data[p:e].decode("latin-1")
        p = e + 1
    full = None
    if name is not None:
        full = name + ("." + ext if ext else "")

    if method == 0:
        out = data[dataoff:]
    elif method == 1:
        out = bytes(b ^ 0xFF for b in data[dataoff:])
    elif method == 2:
        out = _lzss(data, dataoff, outlen, start=4096 - 18)
    elif method == 3:
        out = _kwaj_lzh(data, dataoff, outlen)
    elif method == 4:
        out = _mszip(data, dataoff)
    else:
        raise FormatError("unknown KWAJ method %d" % method)
    if outlen is not None:
        out = out[:outlen]
    return out, full


def _mszip(data, pos):
    out = bytearray()
    hist = b""
    while pos + 4 <= len(data):
        blen = struct.unpack_from("<H", data, pos)[0]
        blk = data[pos + 2: pos + 2 + blen]
        pos += 2 + blen
        if blk[:2] != b"CK":
            break
        d = zlib.decompressobj(-15, zdict=hist) if hist else zlib.decompressobj(-15)
        chunk = d.decompress(blk[2:])
        out += chunk
        hist = bytes(out[-32768:])
    return bytes(out)
