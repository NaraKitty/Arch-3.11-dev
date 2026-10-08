"""Decoders for Win16 resource formats -> plain data (dicts) and standard files.

Everything here turns *the user's own* binaries into data at install time.
Output is never committed to the repo.
"""
import io
import struct

# ---------------------------------------------------------------- strings / helpers


def _sz(d, p):
    e = d.index(b"\0", p)
    return d[p:e].decode("cp1252", "replace"), e + 1


def _sz_or_ord(d, p):
    """Win16 'name or ordinal': 0xFF + u16 ordinal, or NUL-terminated string."""
    if p < len(d) and d[p] == 0xFF:
        return struct.unpack_from("<H", d, p + 1)[0], p + 3
    return _sz(d, p)


# ---------------------------------------------------------------- string tables


def string_block(block_id, data):
    """RT_STRING block -> {string_id: text}. Block N holds ids (N-1)*16 .. (N-1)*16+15."""
    out = {}
    p = 0
    for i in range(16):
        if p >= len(data):
            break
        n = data[p]
        if n:
            out[(block_id - 1) * 16 + i] = data[p + 1:p + 1 + n].decode("cp1252", "replace")
        p += 1 + n
    return out


# ---------------------------------------------------------------- menus

MF_GRAYED, MF_DISABLED, MF_CHECKED, MF_POPUP = 0x01, 0x02, 0x08, 0x10
MF_MENUBARBREAK, MF_MENUBREAK, MF_END, MF_HELP = 0x20, 0x40, 0x80, 0x4000


def menu(data):
    """RT_MENU (Win16 standard menu template) -> list of items (nested)."""
    _ver, hdr = struct.unpack_from("<HH", data, 0)
    pos = [4 + hdr]

    def parse():
        items = []
        while pos[0] < len(data):
            flags = struct.unpack_from("<H", data, pos[0])[0]
            pos[0] += 2
            item = {"flags": flags}
            if not flags & MF_POPUP:
                item["id"] = struct.unpack_from("<H", data, pos[0])[0]
                pos[0] += 2
            text, pos[0] = _sz(data, pos[0])
            item["text"] = text
            if not text and not flags & MF_POPUP and item.get("id", 0) == 0:
                item["separator"] = True
            if flags & MF_POPUP:
                item["items"] = parse()
            items.append(item)
            if flags & MF_END:
                break
        return items

    return parse()


# ---------------------------------------------------------------- dialogs

DS_SETFONT = 0x40
CLASS_NAMES = {0x80: "BUTTON", 0x81: "EDIT", 0x82: "STATIC", 0x83: "LISTBOX",
               0x84: "SCROLLBAR", 0x85: "COMBOBOX"}


def dialog(data):
    """RT_DIALOG (Win16 DLGTEMPLATE) -> dict. Units are dialog base units."""
    style, n, x, y, cx, cy = struct.unpack_from("<IBHHHH", data, 0)
    p = 13
    menu_name, p = _sz_or_ord(data, p)
    cls, p = _sz_or_ord(data, p)
    caption, p = _sz(data, p)
    dlg = {"style": style, "x": x, "y": y, "cx": cx, "cy": cy,
           "menu": menu_name or None, "class": cls or None, "caption": caption,
           "font": None, "items": []}
    if style & DS_SETFONT:
        pt = struct.unpack_from("<H", data, p)[0]
        face, p = _sz(data, p + 2)
        dlg["font"] = {"size": pt, "face": face}
    for _ in range(n):
        ix, iy, icx, icy, iid, istyle = struct.unpack_from("<hhhhHI", data, p)
        p += 14
        c = data[p]
        if c & 0x80:
            klass = CLASS_NAMES.get(c, "0x%02X" % c)
            p += 1
        else:
            klass, p = _sz(data, p)
        text, p = _sz_or_ord(data, p)
        extra = data[p]
        p += 1 + extra
        dlg["items"].append({"x": ix, "y": iy, "cx": icx, "cy": icy, "id": iid,
                             "style": istyle, "class": klass, "text": text})
    return dlg


# ---------------------------------------------------------------- accelerators

VIRT_KEY_NAMES = {
    0x08: "VK_BACK", 0x09: "VK_TAB", 0x0D: "VK_RETURN", 0x1B: "VK_ESCAPE", 0x20: "VK_SPACE",
    0x21: "VK_PRIOR", 0x22: "VK_NEXT", 0x23: "VK_END", 0x24: "VK_HOME", 0x25: "VK_LEFT",
    0x26: "VK_UP", 0x27: "VK_RIGHT", 0x28: "VK_DOWN", 0x2D: "VK_INSERT", 0x2E: "VK_DELETE",
    **{0x70 + i: "VK_F%d" % (i + 1) for i in range(16)},
}


def accelerators(data):
    out = []
    for p in range(0, len(data) - 4, 5):
        fl, key, cmd = struct.unpack_from("<BHH", data, p)
        out.append({"virtkey": bool(fl & 1), "noinvert": bool(fl & 2), "shift": bool(fl & 4),
                    "control": bool(fl & 8), "alt": bool(fl & 16), "key": key, "cmd": cmd})
        if fl & 0x80:
            break
    return out


# ---------------------------------------------------------------- version


def version(data):
    def node(p):
        cb, cbv = struct.unpack_from("<HH", data, p)
        end = p + cb
        key, q = _sz(data, p + 4)
        q = (q + 3) & ~3
        val = data[q:q + cbv]
        q = (q + cbv + 3) & ~3
        kids = []
        while q < end and q + 4 <= len(data):
            k, q2 = node(q)
            kids.append(k)
            if q2 <= q:
                break
            q = q2
        n = {"key": key}
        if key == "VS_VERSION_INFO" and len(val) >= 52:
            fv = struct.unpack_from("<IIII", val, 8)
            n["file_version"] = "%d.%d.%d.%d" % (fv[0] >> 16, fv[0] & 0xFFFF, fv[1] >> 16, fv[1] & 0xFFFF)
            n["product_version"] = "%d.%d.%d.%d" % (fv[2] >> 16, fv[2] & 0xFFFF, fv[3] >> 16, fv[3] & 0xFFFF)
        elif cbv and kids == []:
            n["value"] = val.split(b"\0")[0].decode("cp1252", "replace")
        if kids:
            n["children"] = kids
        return n, (end + 3) & ~3

    root, _ = node(0)
    flat = {}

    def walk(n):
        if "value" in n:
            flat[n["key"]] = n["value"]
        for k in n.get("children", []):
            walk(k)
    walk(root)
    return {"file_version": root.get("file_version"),
            "product_version": root.get("product_version"), "strings": flat}


# ---------------------------------------------------------------- bitmaps / icons / cursors


def dib_to_bmp(dib):
    """Raw DIB (as stored in RT_BITMAP) -> .bmp file bytes."""
    hsz = struct.unpack_from("<I", dib, 0)[0]
    if hsz == 12:
        bpp = struct.unpack_from("<H", dib, 10)[0]
        ncol = (1 << bpp) if bpp <= 8 else 0
        palsz = ncol * 3
    else:
        bpp = struct.unpack_from("<H", dib, 14)[0]
        clr = struct.unpack_from("<I", dib, 32)[0]
        comp = struct.unpack_from("<I", dib, 16)[0]
        ncol = clr or ((1 << bpp) if bpp <= 8 else 0)
        palsz = ncol * 4 + (12 if comp == 3 and hsz == 40 else 0)
    off = 14 + hsz + palsz
    return b"BM" + struct.pack("<IHHI", 14 + len(dib), 0, 0, off) + dib


def _dib_dims(dib):
    hsz = struct.unpack_from("<I", dib, 0)[0]
    if hsz == 12:
        w, h, _pl, bpp = struct.unpack_from("<HHHH", dib, 4)
        return w, h, bpp, 0
    w, h, _pl, bpp = struct.unpack_from("<iiHH", dib, 4)
    clr = struct.unpack_from("<I", dib, 32)[0]
    return w, h, bpp, clr


def build_group(group_data, images, cursor=False):
    """GROUP_ICON / GROUP_CURSOR + {id: RT_ICON/RT_CURSOR data} -> .ico/.cur bytes."""
    _res, typ, count = struct.unpack_from("<HHH", group_data, 0)
    entries = []
    for i in range(count):
        p = 6 + i * 14
        if cursor:
            w, h2, planes, bpp, size, rid = struct.unpack_from("<HHHHIH", group_data, p)
            entries.append((rid, w, h2 // 2, 0))
        else:
            w, h, colors, _r, planes, bpp, size, rid = struct.unpack_from("<BBBBHHIH", group_data, p)
            entries.append((rid, w, h, colors))
    blobs = []
    head = struct.pack("<HHH", 0, 2 if cursor else 1, 0)
    dirs = b""
    for rid, w, h, colors in entries:
        img = images.get(rid)
        if img is None:
            continue
        hx = hy = 0
        if cursor:
            hx, hy = struct.unpack_from("<HH", img, 0)
            img = img[4:]
        dw, dh, bpp, clr = _dib_dims(img)
        blobs.append((dw, abs(dh) // 2, bpp, clr, hx, hy, img))
    head = struct.pack("<HHH", 0, 2 if cursor else 1, len(blobs))
    off = 6 + 16 * len(blobs)
    body = b""
    for dw, dh, bpp, clr, hx, hy, img in blobs:
        ncol = clr or ((1 << bpp) if bpp < 8 else 0)
        a, b = (hx, hy) if cursor else (1, bpp)
        dirs += struct.pack("<BBBBHHII", dw & 0xFF, dh & 0xFF, ncol & 0xFF, 0, a, b, len(img), off + len(body))
        body += img
    return head + dirs + body


def image_to_png(filebytes, kind):
    """Render a .bmp/.ico/.cur to PNG with Pillow (optional dependency)."""
    try:
        from PIL import Image
    except ImportError:
        return None
    try:
        im = Image.open(io.BytesIO(filebytes))
        if kind in ("ico", "cur"):
            sizes = getattr(im, "ico", None) or getattr(im, "info", {})
            try:
                best = max(im.ico.sizes()) if hasattr(im, "ico") else None
                if best:
                    im.size = best
            except Exception:
                pass
        im.load()
        out = io.BytesIO()
        im.save(out, "PNG")
        return out.getvalue()
    except Exception:
        return None


# ---------------------------------------------------------------- fonts (.FNT)


def fnt_info(data):
    (ver, size) = struct.unpack_from("<HI", data, 0)
    copyright_ = data[6:66].split(b"\0")[0].decode("cp1252", "replace")
    (dtype, points, vres, hres, ascent, ileading, eleading, italic, underline, strike, weight,
     charset, pixw, pixh, pitchfam, avgw, maxw, first, last, default, brk, widthbytes,
     device, face, bitsptr, bitsoff) = struct.unpack_from("<HHHHHHHBBBHBHHBHHBBBBHIIII", data, 66)
    facename = ""
    if face and face < len(data):
        facename = data[face:data.index(b"\0", face)].decode("cp1252", "replace")
    return {"version": ver, "size": size, "copyright": copyright_, "vector": bool(dtype & 1),
            "points": points, "vres": vres, "hres": hres, "ascent": ascent,
            "internal_leading": ileading, "external_leading": eleading, "italic": bool(italic),
            "underline": bool(underline), "strikeout": bool(strike), "weight": weight,
            "charset": charset, "pix_width": pixw, "pix_height": pixh, "pitch_family": pitchfam,
            "avg_width": avgw, "max_width": maxw, "first_char": first, "last_char": last,
            "default_char": default, "break_char": brk, "face": facename}


def fnt_glyphs(data):
    """Raster .FNT -> (info, {char: (width, rows[list of int bitmasks MSB=left])})."""
    info = fnt_info(data)
    if info["vector"]:
        return info, {}
    h = info["pix_height"]
    v3 = info["version"] >= 0x300
    table = 148 if v3 else 118
    entsz = 6 if v3 else 4
    glyphs = {}
    for i, ch in enumerate(range(info["first_char"], info["last_char"] + 2)):
        p = table + i * entsz
        if v3:
            w, off = struct.unpack_from("<HI", data, p)
        else:
            w, off = struct.unpack_from("<HH", data, p)
        cols = (w + 7) // 8
        rows = []
        for y in range(h):
            v = 0
            for c in range(cols):
                v = (v << 8) | data[off + c * h + y]
            rows.append(v >> (cols * 8 - w) if w else 0)
        glyphs[ch] = (w, rows)
    return info, glyphs


def fnt_sheet_png(data):
    try:
        from PIL import Image
    except ImportError:
        return None
    info, glyphs = fnt_glyphs(data)
    if not glyphs:
        return None
    h = info["pix_height"]
    cw = max(w for w, _ in glyphs.values()) + 2
    im = Image.new("L", (16 * cw, 16 * (h + 2)), 255)
    px = im.load()
    for ch, (w, rows) in glyphs.items():
        if ch > 255:
            continue
        ox, oy = (ch % 16) * cw + 1, (ch // 16) * (h + 2) + 1
        for y, r in enumerate(rows):
            for x in range(w):
                if r & (1 << (w - 1 - x)):
                    px[ox + x, oy + y] = 0
    out = io.BytesIO()
    im.save(out, "PNG")
    return out.getvalue()


# ---------------------------------------------------------------- .rc text (human readable)

_STYLE_BITS = [
    (0x80000000, "WS_POPUP"), (0x40000000, "WS_CHILD"), (0x20000000, "WS_MINIMIZE"),
    (0x10000000, "WS_VISIBLE"), (0x08000000, "WS_DISABLED"), (0x00C00000, "WS_CAPTION"),
    (0x00800000, "WS_BORDER"), (0x00400000, "WS_DLGFRAME"), (0x00200000, "WS_VSCROLL"),
    (0x00100000, "WS_HSCROLL"), (0x00080000, "WS_SYSMENU"), (0x00040000, "WS_THICKFRAME"),
    (0x00020000, "WS_GROUP"), (0x00010000, "WS_TABSTOP"),
]


def _style_str(s):
    parts = []
    for bit, name in _STYLE_BITS:
        if s & bit == bit:
            parts.append(name)
            s &= ~bit
    if s:
        parts.append("0x%X" % s)
    return " | ".join(parts) or "0"


def _q(s):
    if isinstance(s, int):
        return str(s)
    return '"' + s.replace('"', '""').replace("\t", "\\t") + '"'


def dialog_rc(name, d):
    out = ["%s DIALOG %d, %d, %d, %d" % (name, d["x"], d["y"], d["cx"], d["cy"]),
           "STYLE " + _style_str(d["style"])]
    if d["caption"]:
        out.append("CAPTION " + _q(d["caption"]))
    if d["font"]:
        out.append("FONT %d, %s" % (d["font"]["size"], _q(d["font"]["face"])))
    if d["menu"]:
        out.append("MENU " + _q(d["menu"]))
    out.append("BEGIN")
    for it in d["items"]:
        out.append("    CONTROL %s, %d, %s, %s, %d, %d, %d, %d" % (
            _q(it["text"]), it["id"], _q(it["class"]), _style_str(it["style"]),
            it["x"], it["y"], it["cx"], it["cy"]))
    out.append("END")
    return "\n".join(out)


def menu_rc(name, items):
    out = ["%s MENU" % name, "BEGIN"]

    def emit(items, ind):
        for it in items:
            flags = []
            for bit, nm in ((MF_GRAYED, "GRAYED"), (MF_CHECKED, "CHECKED"), (MF_HELP, "HELP"),
                            (MF_MENUBARBREAK, "MENUBARBREAK"), (MF_MENUBREAK, "MENUBREAK")):
                if it["flags"] & bit:
                    flags.append(nm)
            fl = (", " + ", ".join(flags)) if flags else ""
            if "items" in it:
                out.append(" " * ind + "POPUP %s%s" % (_q(it["text"]), fl))
                out.append(" " * ind + "BEGIN")
                emit(it["items"], ind + 4)
                out.append(" " * ind + "END")
            elif it.get("separator"):
                out.append(" " * ind + "MENUITEM SEPARATOR")
            else:
                out.append(" " * ind + "MENUITEM %s, %d%s" % (_q(it["text"]), it["id"], fl))
    emit(items, 4)
    out.append("END")
    return "\n".join(out)
