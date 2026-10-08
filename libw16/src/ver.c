/* VER.DLL: VerFindFile, VerInstallFile, GetFileVersionInfoSize, GetFileVersionInfo, VerQueryValue,
 * ported from the 3.11 VER.DLL (seg3:0906-1596). Paths are DOS paths on libw16's drives.
 *
 * VerInstallFile reads its source the way it does in 3.1, through LZEXPAND: LZOpenFile tries the
 * compressed name ("FILE.EX_") when the file is not found (LZEXPAND seg2:030A), and LZCopy expands
 * files compressed with COMPRESS.EXE (SZDD). TODO: KWAJ-compressed sources (LZEXPAND handles them
 * too; the WfW 3.11 floppies use KWAJ) are not expanded: they fail as unreadable (UNTESTED either
 * way). */
#include "w16int.h"
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#define LZERROR_BADINHANDLE (-1)
#define LZERROR_BADOUTHANDLE (-2)
#define LZERROR_READ (-3)
#define LZERROR_WRITE (-4)
#define LZERROR_GLOBALLOC (-5)
#define LZERROR_UNKNOWNALG (-8)

/* ------------------------------------------------------------------ seg3:1192 (export 17) */
/* dir + "\" + name (no separator after "\" or ":"); returns the length of the directory part */
static int MakePath(LPCSTR dir, LPCSTR name, LPSTR out)
{
    lstrcpy(out, dir);
    int n = lstrlen(out);
    if (n && out[n - 1] != '\\' && out[n - 1] != ':') out[n++] = '\\';
    lstrcpy(out + n, name);
    return n;
}

/* ------------------------------------------------------------------ seg3:0AC2 */
/* INT 21h 4300h: 0 if the file exists (*ro = the read-only attribute), else -1 */
static int FileAttr(LPCSTR dos, int *ro)
{
    char host[2048];
    struct stat st;
    if (w16_dos_to_host(dos, host, sizeof host) || stat(host, &st)) return -1;
    if (ro) *ro = access(host, W_OK) != 0;
    return 0;
}

/* ------------------------------------------------------------------ seg3:0B42 */
/* the DOS error of the last file operation as VIF bits: access denied, sharing violation */
static DWORD DosErrToVif(int err) { return err == 5 ? VIF_ACCESSVIOLATION : err == 0x20 ? VIF_SHARINGVIOLATION : 0; }

/* seg3:0B94: OpenFile(OF_DELETE); returns < 0 on failure with *err the DOS error */
static int DeleteFile(LPCSTR dos, int *err)
{
    OFSTRUCT of;
    int r = OpenFile(dos, &of, OF_DELETE);
    if (err) *err = of.nErrCode;
    return r;
}

/* seg3:0BD6: INT 21h AH=56h; 0 or the DOS error */
static int RenameFile(LPCSTR from, LPCSTR to)
{
    char a[2048], b[2048];
    if (w16_dos_to_host(from, a, sizeof a) || w16_dos_to_host(to, b, sizeof b)) return 3;
    if (!rename(a, b)) return 0;
    return errno == EACCES || errno == EPERM ? 5 : errno == ENOENT ? 2 : 5;
}

/* seg3:113A: the file is a module in use (3.1: GetModuleHandle + GetModuleFileName); arch311 runs
 * no 16-bit modules, the drivers held open by the installable-driver layer count */
static BOOL InUse(LPCSTR name, LPCSTR path)
{
    (void)path;
    return w16_driver_in_use(name);
}

/* ------------------------------------------------------------------ LZEXPAND, as VerInstallFile uses it */
typedef struct { uint8_t *d; size_t n; } Src;

/* LZEXPAND seg2:030A LZOpenFile(OF_READ): the file, else its compressed name ("X.DRV" -> "X.DR_") */
static int LzOpen(LPCSTR path, Src *s)
{
    char name[300], host[2048];
    OFSTRUCT of;
    snprintf(name, sizeof name, "%s", path);
    HFILE h = OpenFile(name, &of, OF_READ);
    if (h == HFILE_ERROR && of.nErrCode == 2) {
        char *base = name;
        for (char *c = name; *c; c++)
            if (*c == '\\' || *c == ':') base = c + 1;
        char *dot = strrchr(base, '.');
        size_t n = strlen(name);
        if (!dot) snprintf(name + n, sizeof name - n, "._");
        else if (strlen(dot) < 4) snprintf(name + n, sizeof name - n, "_");
        else name[n - 1] = '_';
        h = OpenFile(name, &of, OF_READ);
    }
    if (h == HFILE_ERROR) return 0;
    _lclose(h);
    if (w16_dos_to_host(of.szPathName, host, sizeof host)) return 0;
    FILE *f = fopen(host, "rb");
    if (!f) return 0;
    s->d = NULL;
    s->n = 0;
    uint8_t buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        uint8_t *t = realloc(s->d, s->n + got);
        if (!t) break;
        s->d = t;
        memcpy(s->d + s->n, buf, got);
        s->n += got;
    }
    fclose(f);
    return 1;
}

/* LZCopy (LZEXPAND seg4:015C): the source, expanded when it is compressed, into hOut; the bytes
 * written or an LZERROR_ code */
static long LzCopy(const Src *s, HFILE hOut)
{
    static const uint8_t szdd[8] = {'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33};
    static const uint8_t kwaj[8] = {'K', 'W', 'A', 'J', 0x88, 0xF0, 0x27, 0xD1};
    if (hOut == HFILE_ERROR) return LZERROR_BADOUTHANDLE;
    if (s->n >= 8 && !memcmp(s->d, kwaj, 8)) return LZERROR_UNKNOWNALG;   /* TODO: KWAJ */
    if (s->n < 14 || memcmp(s->d, szdd, 8)) {
        if (s->n && _lwrite(hOut, s->d, (UINT)s->n) != (UINT)s->n) return LZERROR_WRITE;
        return (long)s->n;
    }
    if (s->d[8] != 'A') return LZERROR_UNKNOWNALG;
    /* COMPRESS.EXE's LZSS: a 4096-byte window of blanks written from 4096-16; each control byte's
     * bits (lowest first) mark a literal (1) or a 12-bit position + 4-bit length - 3 pair (0) */
    size_t size = s->d[10] | s->d[11] << 8 | (size_t)s->d[12] << 16 | (size_t)s->d[13] << 24, o = 0;
    uint8_t *out = malloc(size ? size : 1), win[4096];
    if (!out) return LZERROR_GLOBALLOC;
    memset(win, ' ', sizeof win);
    int pos = 4096 - 16;
    size_t i = 14;
    while (o < size && i < s->n) {
        int ctl = s->d[i++];
        for (int bit = 0; bit < 8 && o < size && i < s->n; bit++) {
            if (ctl & (1 << bit)) {
                out[o++] = win[pos] = s->d[i++];
                pos = (pos + 1) & 4095;
            } else {
                if (i + 1 >= s->n) break;
                int at = s->d[i] | (s->d[i + 1] & 0xF0) << 4, len = (s->d[i + 1] & 0x0F) + 3;
                i += 2;
                while (len-- && o < size) {
                    out[o++] = win[pos] = win[at];
                    at = (at + 1) & 4095;
                    pos = (pos + 1) & 4095;
                }
            }
        }
    }
    long r = o < size ? LZERROR_READ : (long)size;
    if (r > 0 && _lwrite(hOut, out, (UINT)size) != (UINT)size) r = LZERROR_WRITE;
    free(out);
    return r;
}

/* ------------------------------------------------------------------ version resources */
/* the VS_VERSION_INFO resource (RT_VERSION 16, id 1) of an NE file; malloc'd, NULL if none */
static uint8_t *VersionResource(LPCSTR file, DWORD *len)
{
    char host[2048];
    OFSTRUCT of;
    if (OpenFile(file, &of, OF_EXIST) == HFILE_ERROR || w16_dos_to_host(of.szPathName, host, sizeof host)) return NULL;
    FILE *f = fopen(host, "rb");
    if (!f) return NULL;
    uint8_t h[0x40], *res = NULL;
    if (fread(h, 1, 0x40, f) != 0x40 || h[0] != 'M' || h[1] != 'Z') goto out;
    long ne = h[0x3C] | h[0x3D] << 8 | (long)h[0x3E] << 16 | (long)h[0x3F] << 24;
    if (fseek(f, ne, SEEK_SET) || fread(h, 1, 0x40, f) != 0x40 || h[0] != 'N' || h[1] != 'E') goto out;
    long rt = ne + (h[0x24] | h[0x25] << 8);
    if ((h[0x24] | h[0x25] << 8) == (h[0x26] | h[0x27] << 8) || fseek(f, rt, SEEK_SET)) goto out;
    uint8_t w[12];
    if (fread(w, 1, 2, f) != 2) goto out;
    int shift = w[0] | w[1] << 8;
    for (;;) {
        if (fread(w, 1, 8, f) != 8) goto out;
        int type = w[0] | w[1] << 8, count = w[2] | w[3] << 8;
        if (!type) goto out;
        for (int i = 0; i < count; i++) {
            if (fread(w, 1, 12, f) != 12) goto out;
            if (type != (0x8000 | 16) || (w[6] | w[7] << 8) != (0x8000 | 1)) continue;
            long off = (long)(w[0] | w[1] << 8) << shift;
            DWORD n = (DWORD)(w[2] | w[3] << 8) << shift;
            res = malloc(n ? n : 1);
            if (res && (fseek(f, off, SEEK_SET) || fread(res, 1, n, f) != n)) { free(res); res = NULL; }
            if (res && len) *len = n;
            goto out;
        }
    }
out:
    fclose(f);
    return res;
}

/* seg3:086E */
DWORD GetFileVersionInfoSize(LPCSTR file, DWORD *handle)
{
    DWORD n = 0;
    uint8_t *r = VersionResource(file, &n);
    if (handle) *handle = 0;
    free(r);
    return r ? n : 0;
}

/* seg3:0906 */
BOOL GetFileVersionInfo(LPCSTR file, DWORD handle, DWORD len, void *data)
{
    DWORD n = 0;
    (void)handle;
    uint8_t *r = VersionResource(file, &n);
    if (!r) return FALSE;
    memcpy(data, r, n < len ? n : len);
    free(r);
    return TRUE;
}

/* seg3:0943: a block is WORD cbBlock, WORD cbValue, its key, the value and the child blocks, each
 * part DWORD-aligned; "\" is the root's value (VS_FIXEDFILEINFO), "\A\B" walks the keys */
BOOL VerQueryValue(const void *block, LPCSTR sub, void **buf, UINT *len)
{
    const uint8_t *b = block;
    char part[128];
    for (;;) {
        while (*sub == '\\') sub++;
        unsigned cb = b[0] | b[1] << 8, cv = b[2] | b[3] << 8;
        size_t key = strlen((const char *)b + 4);
        const uint8_t *val = b + 4 + ((key + 4) & ~3u);
        if (!*sub) {
            if (buf) *buf = (void *)val;
            if (len) *len = cv;
            return TRUE;
        }
        size_t n = strcspn(sub, "\\");
        snprintf(part, sizeof part, "%.*s", (int)n, sub);
        sub += n;
        const uint8_t *c = val + ((cv + 3) & ~3u), *end = b + cb;
        while (c + 4 <= end) {
            unsigned ccb = c[0] | c[1] << 8;
            if (!ccb) return FALSE;
            if (!lstrcmpi((const char *)c + 4, part)) break;
            c += (ccb + 3) & ~3u;
        }
        if (c + 4 > end) return FALSE;
        b = c;
    }
}

/* seg3:0C34: the version information of a file in a buffer (NULL if it has none) */
static uint8_t *VersionInfo(LPCSTR file)
{
    DWORD n = GetFileVersionInfoSize(file, NULL);
    uint8_t *p = n ? malloc(n) : NULL;
    if (p && !GetFileVersionInfo(file, 0, n, p)) { free(p); p = NULL; }
    return p;
}

static DWORD dw(const uint8_t *p) { return p[0] | p[1] << 8 | (DWORD)p[2] << 16 | (DWORD)p[3] << 24; }

/* ------------------------------------------------------------------ seg3:0C9E */
DWORD VerInstallFile(UINT flags, LPCSTR szSrcFileName, LPCSTR szDestFileName, LPCSTR szSrcDir,
                     LPCSTR szDestDir, LPCSTR szCurDir, LPSTR szTmpFile, UINT *lpuTmpFileLen)
{
    char srcPath[300], destPath[300], tmpPath[300], oldPath[300];
    DWORD vif = 0;
    Src src = {NULL, 0};
    int ro = 0, err = 0, n;
    MakePath(szSrcDir, szSrcFileName, srcPath);
    if (!LzOpen(srcPath, &src)) return VIF_CANNOTREADSRC;
    int dirLen = MakePath(szDestDir, szDestFileName, destPath);
    lstrcpy(tmpPath, destPath);
    BOOL destExists = FileAttr(destPath, &ro) == 0;
    if (destExists) {
        if (ro) { vif |= VIF_WRITEPROT; goto close; }
        if (InUse(szDestFileName, destPath)) { vif |= VIF_FILEINUSE; goto close; }
    }
    /* a forced install may reuse the temporary file of an earlier call */
    if ((flags & VIFF_FORCEINSTALL) && *szTmpFile) {
        MakePath(szDestDir, szTmpFile, tmpPath);
        if (FileAttr(tmpPath, NULL) == 0) goto have_tmp;
    }
    /* a free name temp.000 .. temp.FFF next to the destination */
    lstrcpy(tmpPath + dirLen, "temp.");
    for (n = 0;; n++) {
        if (n > 0xFFF) { vif |= VIF_CANNOTCREATE; goto close; }
        wsprintf(tmpPath + dirLen + 5, "%03X", n);
        if (FileAttr(tmpPath, NULL)) break;
    }
    {
        OFSTRUCT of;
        HFILE h = OpenFile(tmpPath, &of, OF_CREATE);
        if (h == HFILE_ERROR) { vif |= DosErrToVif(of.nErrCode) | VIF_CANNOTCREATE; goto close; }
        long r = LzCopy(&src, h);
        _lclose(h);
        if (r < 0) {
            /* seg3:0E58: the LZERROR_ codes as VIF bits */
            vif |= r == LZERROR_WRITE || r == LZERROR_BADOUTHANDLE ? VIF_OUTOFSPACE
                 : r == LZERROR_GLOBALLOC || r == -6 ? VIF_OUTOFMEMORY : VIF_CANNOTREADSRC;
            DeleteFile(tmpPath, NULL);
            goto close;
        }
    }
have_tmp:
    if (destExists) {
        if (!(flags & VIFF_FORCEINSTALL)) {
            uint8_t *dst = VersionInfo(destPath);
            if (dst) {
                uint8_t *s = VersionInfo(tmpPath);
                if (!s)
                    vif |= VIF_MISMATCH | VIF_SRCOLD;
                else {
                    /* VS_FIXEDFILEINFO at +14h: file version +1Ch/+20h, type +38h, subtype +3Ch */
                    DWORD dms = dw(dst + 0x1C), dls = dw(dst + 0x20), sms = dw(s + 0x1C), sls = dw(s + 0x20);
                    if (dms > sms || (dms == sms && dls > sls)) vif |= VIF_MISMATCH | VIF_SRCOLD;
                    if (dw(dst + 0x38) != dw(s + 0x38) || dw(dst + 0x3C) != dw(s + 0x3C)) vif |= VIF_MISMATCH | VIF_DIFFTYPE;
                    void *pd, *ps;
                    UINT cd, cs;
                    if (VerQueryValue(dst, "\\VarFileInfo\\Translation", &pd, &cd) &&
                        VerQueryValue(s, "\\VarFileInfo\\Translation", &ps, &cs)) {
                        const uint8_t *td = pd, *ts = ps;
                        for (UINT i = 0; i < cd / 4; i++) {
                            UINT j;
                            for (j = 0; j < cs / 4; j++) {
                                WORD dl = td[4 * i] | td[4 * i + 1] << 8, dc = td[4 * i + 2] | td[4 * i + 3] << 8;
                                WORD sl = ts[4 * j] | ts[4 * j + 1] << 8, sc = ts[4 * j + 2] | ts[4 * j + 3] << 8;
                                if (dl != sl) continue;
                                if ((dc == 0 && sc != 0x4B0) || dc == sc) break;
                            }
                            if (j == cs / 4) vif |= VIF_MISMATCH | VIF_DIFFLANG;
                        }
                    }
                    free(s);
                }
                free(dst);
            }
        }
        if (!vif && DeleteFile(destPath, &err) < 0) {
            vif |= DosErrToVif(err) | VIF_CANNOTDELETE;
            DeleteFile(tmpPath, NULL);
            goto close;
        }
    }
    if (vif) {
        /* problems: the new file stays as the temporary file, for a forced retry */
        n = lstrlen(tmpPath + dirLen);
        if ((UINT)n < *lpuTmpFileLen) {
            lstrcpy(szTmpFile, tmpPath + dirLen);
            vif |= VIF_TEMPFILE;
        } else {
            vif |= VIF_BUFFTOOSMALL;
            DeleteFile(tmpPath, NULL);
        }
        *lpuTmpFileLen = n + 1;
        goto close;
    }
    /* the copy found elsewhere (szCurDir) goes, unless VIFF_DONTDELETEOLD */
    if (!(flags & VIFF_DONTDELETEOLD) && szCurDir && *szCurDir && lstrcmpi(szCurDir, szDestDir)) {
        MakePath(szCurDir, szDestFileName, oldPath);
        err = 0;
        if (FileAttr(oldPath, NULL) == 0 && (InUse(szDestFileName, oldPath) || DeleteFile(oldPath, &err) < 0))
            vif |= DosErrToVif(err) | VIF_CANNOTDELETECUR;
    }
    if ((err = RenameFile(tmpPath, destPath)) != 0) {
        vif |= DosErrToVif(err) | VIF_CANNOTRENAME;
        DeleteFile(tmpPath, NULL);
    }
close:
    free(src.d);
    return vif;
}

/* ------------------------------------------------------------------ seg3:1200 */
/* the directory (no trailing "\" except a root) where OpenFile finds `file` in dirs[] (NULL-ended),
 * else on its own search path; returns its length, 0 if not found */
static int FindFile(LPCSTR file, LPSTR out, int cb, LPCSTR const *dirs)
{
    char path[300];
    OFSTRUCT of;
    int first = 1, flen = lstrlen(file);
    for (;; dirs++) {
        if (*dirs) {
            if (lstrlen(*dirs) + flen >= 0x103) continue;
            MakePath(*dirs, file, path);
        } else if (first) {
            first = 0;
            lstrcpy(path, file);
            dirs--;
        } else
            return 0;
        if (OpenFile(path, &of, OF_EXIST) == HFILE_ERROR) {
            if (!first) return 0;
            continue;
        }
        int last = 0;
        for (int i = 0; of.szPathName[i]; i++)
            if (of.szPathName[i] == '\\') last = i;
        if (last <= 3) last++;
        int k = last < cb - 1 ? last : cb - 1;
        of.szPathName[k] = 0;
        OemToAnsi(of.szPathName, out);
        return last;
    }
}

/* ------------------------------------------------------------------ seg3:1319 */
UINT VerFindFile(UINT flags, LPCSTR szFileName, LPCSTR szWinDir, LPCSTR szAppDir, LPSTR szCurDir,
                 UINT *lpuCurDirLen, LPSTR szDestDir, UINT *lpuDestDirLen)
{
    static char shared[0x104];       /* [0x52]: where shared files go, found once */
    static int cchShared;            /* [0x38] */
    char winDir[0x104], sysDir[0x104], curDir[0x104];
    UINT r = 0;
    (void)szWinDir;                  /* 3.1 replaces it with GetWindowsDirectory */
    GetWindowsDirectory(winDir, sizeof winDir);
    if (!GetSystemDirectory(sysDir, sizeof sysDir)) lstrcpy(sysDir, winDir);
    LPCSTR dirs[4] = {szAppDir, winDir, sysDir, NULL};
    if (flags & VFFF_ISSHAREDFILE) { dirs[0] = winDir; dirs[1] = sysDir; dirs[2] = szAppDir; }
    int n = FindFile(szFileName, curDir, sizeof curDir, dirs);
    if (!n) curDir[0] = 0;
    if (*lpuCurDirLen > (UINT)n) lstrcpy(szCurDir, curDir);
    else r |= VFF_BUFFTOOSMALL;
    *lpuCurDirLen = n + 1;
    if (szDestDir) {
        LPCSTR dest;
        int len;
        if (flags & VFFF_ISSHAREDFILE) {
            if (!cchShared) {
                /* the system directory when it lies inside the Windows directory, else the latter */
                int w = lstrlen(winDir);
                BOOL inside = FALSE;
                if (w && winDir[w - 1] == '\\') inside = sysDir[w - 1] == '\\' && !strncasecmp(winDir, sysDir, w);
                else inside = sysDir[w] == '\\' && !strncasecmp(winDir, sysDir, w);
                lstrcpy(shared, inside ? sysDir : winDir);
                cchShared = lstrlen(shared);
            }
            dest = shared;
            len = cchShared;
        } else {
            dest = szAppDir;
            len = lstrlen(szAppDir);
        }
        if (*lpuDestDirLen > (UINT)len) {
            lstrcpy(szDestDir, dest);
            int l = lstrlen(szDestDir);
            if (l && szDestDir[l - 1] == '\\') szDestDir[l - 1] = 0;
            if (lstrcmpi(szDestDir, szCurDir)) r |= VFF_CURNEDEST;
        } else
            r |= VFF_BUFFTOOSMALL;
        *lpuDestDirLen = len + 1;
    }
    if (curDir[0]) {
        MakePath(curDir, szFileName, sysDir);
        if (InUse(szFileName, sysDir)) r |= VFF_FILEINUSE;
    }
    return r;
}
