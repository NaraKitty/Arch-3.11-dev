/* LZEXPAND.DLL: the file calls MAIN.CPL's INF reader (seg23) makes. The ripped files are expanded
 * already, so a handle reads the file as it is.
 * TODO: files still compressed with COMPRESS.EXE (SZDD) are not expanded on the fly as 3.1's
 * LZOpenFile does (UNTESTED: no program here reads one yet). */
#include "w16int.h"

HFILE LZOpenFile(LPCSTR name, OFSTRUCT *of, UINT style)
{
    return OpenFile(name, of, style);
}

LONG LZSeek(HFILE f, LONG off, int origin)
{
    return _llseek(f, off, origin);
}

int LZRead(HFILE f, void *buf, int cb)
{
    if (cb <= 0) return 0;
    UINT n = _lread(f, buf, (UINT)cb);
    return n == (UINT)-1 ? LZERROR_READ : (int)n;
}

void LZClose(HFILE f)
{
    _lclose(f);
}
