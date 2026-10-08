"""NE-aware 16-bit disassembler used as the *reference* for native ports.

    python3 -m arch311rip.disasm ASSETS/files/NOTEPAD.EXE --names ASSETS/files -o work/notepad.asm

* applies NE relocation chains, so far calls show up as  call far USER.CreateWindow
* names imports by ordinal using the export tables of the user's own KRNL386/USER/GDI/...
* splits code at function prologues / far-call targets and marks exported entry points
* annotates string literals in the data segment

The listing is a working document for whoever ports the app. It is derived from
Microsoft's binary, so it lives in work/ (gitignored) and is never committed.
Requires: pip install capstone
"""
import argparse
import os
import struct
import sys

from . import ne

try:
    import capstone
except ImportError:  # pragma: no cover
    capstone = None

# Modules whose file name differs from their module name
MODULE_FILES = {"KERNEL": ["KRNL386.EXE", "KRNL286.EXE"], "DISPLAY": ["VGA.DRV"],
                "SOUND": ["SOUND.DRV"]}

REL_SRC = {0: "lobyte", 2: "selector", 3: "farptr", 5: "offset", 11: "farptr48", 13: "offset32"}


class ImportNames:
    def __init__(self, files_dir):
        self.dir = files_dir
        self.cache = {}

    def name(self, module, ordinal):
        if module not in self.cache:
            names = {}
            cands = MODULE_FILES.get(module, []) + [module + ext for ext in (".EXE", ".DLL", ".DRV")]
            for c in cands:
                p = os.path.join(self.dir or "", c)
                if self.dir and os.path.exists(p):
                    try:
                        m = ne.NE(open(p, "rb").read())
                        names = {**m.nonresident_names, **m.resident_names}
                        break
                    except ne.NotNE:
                        pass
            self.cache[module] = names
        return self.cache[module].get(ordinal, "@%d" % ordinal)


def relocations(m, segno, imports):
    """-> {offset_in_segment: (kind, text)} with NE chains walked."""
    s = m.segments[segno - 1]
    if not s["has_relocs"] or not s["file_offset"]:
        return {}
    d = m.d
    p = s["file_offset"] + s["length"]
    seg = bytearray(m.segment_data(segno))
    count = struct.unpack_from("<H", d, p)[0]
    p += 2
    out = {}
    for _ in range(count):
        src, flags, off, a, b = struct.unpack_from("<BBHHH", d, p)
        p += 8
        typ = flags & 3
        additive = flags & 4
        if typ == 0:
            seg_i = a & 0xFF
            if seg_i == 0xFF:
                text = "entry#%d" % b
            else:
                text = "seg%d:%04X" % (seg_i, b)
        elif typ == 1:
            mod = m.imports[a - 1] if 0 < a <= len(m.imports) else "MOD%d" % a
            text = "%s.%s" % (mod, imports.name(mod, b))
        elif typ == 2:
            mod = m.imports[a - 1] if 0 < a <= len(m.imports) else "MOD%d" % a
            base = m.off + m.impname_off
            n = d[base + b]
            text = "%s.%s" % (mod, d[base + b + 1: base + b + 1 + n].decode("latin-1"))
        else:
            text = "OSFIXUP%d" % a
        kind = REL_SRC.get(src, "src%d" % src)
        if additive:
            out[off] = (kind, text)
        else:
            seen = set()
            while off != 0xFFFF and off < len(seg) - 1 and off not in seen:
                seen.add(off)
                out[off] = (kind, text)
                off = struct.unpack_from("<H", seg, off)[0]
    return out


def entry_points(m):
    """-> {(seg, off): ordinal}"""
    d = m.d
    p = m.off + m.entry_off
    end = p + m.entry_len
    ordv = 1
    out = {}
    while p < end:
        cnt, ind = d[p], d[p + 1]
        p += 2
        if cnt == 0:
            break
        for _ in range(cnt):
            if ind == 0:
                pass
            elif ind == 0xFF:
                _fl, _int3f, seg, off = struct.unpack_from("<BHBH", d, p)
                out[(seg, off)] = ordv
                p += 6
            else:
                _fl, off = struct.unpack_from("<BH", d, p)
                out[(ind, off)] = ordv
                p += 3
            ordv += 1
    return out


def data_strings(m):
    """Find printable C strings in the auto data segment: {offset: text}."""
    if not m.autodata:
        return {}
    data = m.segment_data(m.autodata)
    out = {}
    i = 0
    while i < len(data):
        j = i
        while j < len(data) and (32 <= data[j] < 127 or data[j] in (9, 10, 13)):
            j += 1
        if j - i >= 3 and j < len(data) and data[j] == 0:
            out[i] = data[i:j].decode("latin-1")
            i = j + 1
        else:
            i += 1
    return out


def disassemble(path, names_dir, out):
    if capstone is None:
        sys.exit("capstone is required: pip install capstone")
    m = ne.NE(open(path, "rb").read())
    imports = ImportNames(names_dir)
    entries = entry_points(m)
    names = {**m.resident_names, **m.nonresident_names}
    strs = data_strings(m)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
    w = out.write
    w("; %s  module=%s  %s\n" % (os.path.basename(path), m.module_name, m.description))
    w("; imports: %s\n" % ", ".join(m.imports))
    w("; entry CS:IP = seg%d:%04X  autodata = seg%d\n" % (m.cs, m.ip, m.autodata))
    w("; derived from a Microsoft binary - reference only, do not commit\n\n")
    stats = {"insns": 0, "api_calls": {}}
    for s in m.segments:
        if s["kind"] != "CODE":
            continue
        segno = s["index"]
        code = m.segment_data(segno)
        rel = relocations(m, segno, imports)
        w("\n;=============== seg%d  CODE  %d bytes\n" % (segno, len(code)))
        labels = {off: names.get(o, "export_%d" % o) for (sg, off), o in entries.items() if sg == segno}
        if segno == m.cs:
            labels.setdefault(m.ip, "start")
        off = 0
        while off < len(code):
            if off in labels:
                w("\n%s:  ; seg%d:%04X\n" % (labels[off], segno, off))
            insn = next(md.disasm(code[off:off + 16], off), None)
            if insn is None:
                w("    %04X  db %02Xh\n" % (off, code[off]))
                off += 1
                continue
            text = "%s %s" % (insn.mnemonic, insn.op_str)
            note = ""
            for k in range(off, off + insn.size):
                if k in rel:
                    kind, target = rel[k]
                    if insn.mnemonic in ("lcall", "call", "ljmp", "jmp") and kind == "farptr":
                        text = "%s far %s" % ("call" if "call" in insn.mnemonic else "jmp", target)
                        if "." in target:
                            stats["api_calls"][target] = stats["api_calls"].get(target, 0) + 1
                    else:
                        note = "  ; reloc %s -> %s" % (kind, target)
                    break
            if not note and m.autodata:
                for tok in insn.op_str.replace("[", " ").replace("]", " ").replace(",", " ").split():
                    if tok.startswith("0x"):
                        try:
                            v = int(tok, 16)
                        except ValueError:
                            continue
                        if v in strs:
                            note = '  ; "%s"' % strs[v][:60].replace("\n", "\\n")
                            break
            if insn.mnemonic in ("push",) and insn.op_str == "bp" and off + 3 < len(code) and \
                    code[off + 1:off + 3] in (b"\x8b\xec", b"\x89\xe5") and off not in labels:
                w("\nsub_%d_%04X:\n" % (segno, off))
            w("    %04X  %-40s%s\n" % (off, text, note))
            stats["insns"] += 1
            off += insn.size
    return m, stats


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--names", help="directory with the user's expanded system files (KRNL386.EXE, USER.EXE, ...)")
    ap.add_argument("-o", "--out")
    a = ap.parse_args(argv)
    names_dir = a.names or os.path.dirname(a.exe)
    fh = open(a.out, "w") if a.out else sys.stdout
    m, st = disassemble(a.exe, names_dir, fh)
    print("%s: %d instructions, %d distinct API imports" % (
        os.path.basename(a.exe), st["insns"], len(st["api_calls"])), file=sys.stderr)


if __name__ == "__main__":
    main()
