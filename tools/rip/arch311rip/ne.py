"""16-bit NE (New Executable) reader: header, segment table, resources, names, entry points.

Interface/format facts only; nothing here executes guest code.
"""
import struct

RT_NAMES = {
    1: "CURSOR", 2: "BITMAP", 3: "ICON", 4: "MENU", 5: "DIALOG", 6: "STRING",
    7: "FONTDIR", 8: "FONT", 9: "ACCELERATOR", 10: "RCDATA", 11: "MESSAGETABLE",
    12: "GROUP_CURSOR", 14: "GROUP_ICON", 15: "NAMETABLE", 16: "VERSION",
}


class NotNE(Exception):
    pass


class Resource:
    __slots__ = ("type", "name", "flags", "data")

    def __init__(self, rtype, name, flags, data):
        self.type, self.name, self.flags, self.data = rtype, name, flags, data

    @property
    def type_name(self):
        return RT_NAMES.get(self.type, str(self.type)) if isinstance(self.type, int) else self.type

    def __repr__(self):
        return "<Resource %s %r %d bytes>" % (self.type_name, self.name, len(self.data))


class NE:
    def __init__(self, data):
        if data[:2] != b"MZ" or len(data) < 0x40:
            raise NotNE("no MZ header")
        off = struct.unpack_from("<I", data, 0x3C)[0]
        if off + 0x40 > len(data) or data[off:off + 2] != b"NE":
            raise NotNE("no NE header")
        self.d = data
        self.off = off
        def u16(o):
            return struct.unpack_from("<H", data, off + o)[0]
        self.linker_ver, self.linker_rev = data[off + 2], data[off + 3]
        self.entry_off, self.entry_len = u16(0x04), u16(0x06)
        self.crc = struct.unpack_from("<I", data, off + 0x08)[0]
        self.flags, self.autodata, self.heap, self.stack = u16(0x0C), u16(0x0E), u16(0x10), u16(0x12)
        self.ip, self.cs, self.sp, self.ss = u16(0x14), u16(0x16), u16(0x18), u16(0x1A)
        self.nseg, self.nmod, self.nonres_len = u16(0x1C), u16(0x1E), u16(0x20)
        self.seg_off, self.res_off, self.resname_off = u16(0x22), u16(0x24), u16(0x26)
        self.modref_off, self.impname_off = u16(0x28), u16(0x2A)
        self.nonres_off = struct.unpack_from("<I", data, off + 0x2C)[0]
        self.nmovable, self.align, self.nres = u16(0x30), u16(0x32), u16(0x34)
        self.os, self.flags2 = data[off + 0x36], data[off + 0x37]
        self.expver = u16(0x3E)
        self.is_dll = bool(self.flags & 0x8000)
        self.segments = self._segments()
        self.module_name, self.resident_names = self._names(off + self.resname_off)
        self.description, self.nonresident_names = (
            self._names(self.nonres_off) if self.nonres_len else ("", {}))
        self.imports = self._imports()

    # -- tables
    def _segments(self):
        segs = []
        shift = self.align or 9
        for i in range(self.nseg):
            so, sl, fl, ma = struct.unpack_from("<HHHH", self.d, self.off + self.seg_off + i * 8)
            segs.append({
                "index": i + 1, "file_offset": so << shift, "length": sl or 0x10000,
                "flags": fl, "min_alloc": ma or 0x10000,
                "kind": "DATA" if fl & 1 else "CODE",
                "has_relocs": bool(fl & 0x100),
            })
        return segs

    def _names(self, pos):
        """Resident/non-resident name table -> (first name, {ordinal: name})."""
        first = None
        names = {}
        d = self.d
        while pos < len(d):
            n = d[pos]
            if n == 0:
                break
            s = d[pos + 1:pos + 1 + n].decode("latin-1")
            ordv = struct.unpack_from("<H", d, pos + 1 + n)[0]
            if first is None:
                first = s
            else:
                names[ordv] = s
            pos += 3 + n
        return first or "", names

    def _imports(self):
        mods = []
        base = self.off + self.impname_off
        for i in range(self.nmod):
            o = struct.unpack_from("<H", self.d, self.off + self.modref_off + i * 2)[0]
            n = self.d[base + o]
            mods.append(self.d[base + o + 1: base + o + 1 + n].decode("latin-1"))
        return mods

    def segment_data(self, seg):
        s = self.segments[seg - 1]
        if s["file_offset"] == 0:
            return b""
        return self.d[s["file_offset"]:s["file_offset"] + s["length"]]

    # -- resources
    def _resstr(self, base, o):
        n = self.d[base + o]
        return self.d[base + o + 1: base + o + 1 + n].decode("latin-1")

    def resources(self):
        if self.res_off == self.resname_off:
            return []
        base = self.off + self.res_off
        d = self.d
        shift = struct.unpack_from("<H", d, base)[0]
        p = base + 2
        out = []
        while True:
            tid = struct.unpack_from("<H", d, p)[0]
            if tid == 0:
                break
            count = struct.unpack_from("<H", d, p + 2)[0]
            p += 8
            rtype = tid & 0x7FFF if tid & 0x8000 else self._resstr(base, tid)
            for _ in range(count):
                o, ln, fl, rid = struct.unpack_from("<HHHH", d, p)
                p += 12
                name = rid & 0x7FFF if rid & 0x8000 else self._resstr(base, rid)
                start = o << shift
                out.append(Resource(rtype, name, fl, d[start:start + (ln << shift)]))
        return out
