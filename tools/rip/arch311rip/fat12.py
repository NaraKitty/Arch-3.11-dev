"""Minimal read-only FAT12/FAT16 floppy image reader (root dir + subdirs, 8.3 names)."""
import struct


class Fat:
    def __init__(self, data):
        self.d = data
        bps, spc, rsv, nfat, nroot, tot16, _media, spf = struct.unpack_from("<HBHBHHBH", data, 11)
        if bps not in (512, 1024, 2048, 4096) or spc == 0 or nfat == 0:
            raise ValueError("not a FAT floppy image")
        tot = tot16 or struct.unpack_from("<I", data, 32)[0]
        self.bps, self.spc = bps, spc
        self.fat_off = rsv * bps
        self.root_off = (rsv + nfat * spf) * bps
        self.root_len = nroot * 32
        self.data_off = self.root_off + ((nroot * 32 + bps - 1) // bps) * bps
        nclusters = (tot - self.data_off // bps) // spc
        self.fat16 = nclusters >= 4085
        self.label = None

    def _next(self, c):
        if self.fat16:
            return struct.unpack_from("<H", self.d, self.fat_off + c * 2)[0]
        v = struct.unpack_from("<H", self.d, self.fat_off + c * 3 // 2)[0]
        return v >> 4 if c & 1 else v & 0xFFF

    def _chain(self, c):
        end = 0xFFF8 if self.fat16 else 0xFF8
        seen = set()
        while 2 <= c < end and c not in seen:
            seen.add(c)
            yield c
            c = self._next(c)

    def _read_chain(self, c, size=None):
        cs = self.bps * self.spc
        out = bytearray()
        for cl in self._chain(c):
            o = self.data_off + (cl - 2) * cs
            out += self.d[o:o + cs]
        return bytes(out if size is None else out[:size])

    def _entries(self, raw, prefix):
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if len(e) < 32 or e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] == 0x0F:
                continue
            attr = e[11]
            name = e[0:8].decode("latin-1").rstrip()
            ext = e[8:11].decode("latin-1").rstrip()
            full = name + ("." + ext if ext else "")
            if attr & 0x08:
                self.label = (name + ext).strip()
                continue
            if full in (".", ".."):
                continue
            clus = struct.unpack_from("<H", e, 26)[0]
            size = struct.unpack_from("<I", e, 28)[0]
            if attr & 0x10:
                yield from self._entries(self._read_chain(clus), prefix + full + "/")
            else:
                yield prefix + full, clus, size

    def files(self):
        """Yield (path, bytes) for every file on the image."""
        root = self.d[self.root_off:self.root_off + self.root_len]
        for path, clus, size in list(self._entries(root, "")):
            yield path, (self._read_chain(clus, size) if size else b"")
