// Window pixel grabber for the reference rig: PrintWindow into a DIB (works without focus and
// when the window is covered), plus a minimal PNG writer so no System.Drawing is needed.
using System;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;

public static class WinCap
{
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)]
    struct BITMAPINFOHEADER
    {
        public int biSize, biWidth, biHeight; public short biPlanes, biBitCount;
        public int biCompression, biSizeImage, biXPelsPerMeter, biYPelsPerMeter, biClrUsed, biClrImportant;
    }
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleBitmap(IntPtr dc, int w, int h);
    [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr o);
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr o);
    [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern int GetDIBits(IntPtr dc, IntPtr bmp, uint start, uint lines, byte[] bits, ref BITMAPINFOHEADER bi, uint usage);

    /// <summary>Client area as packed RGB rows (no padding); null when the window has no area.</summary>
    public static byte[] Grab(IntPtr h, out int w, out int hh)
    {
        RECT r; GetClientRect(h, out r);
        w = r.R - r.L; hh = r.B - r.T;
        if (w <= 0 || hh <= 0) return null;
        IntPtr sdc = GetDC(h), mdc = CreateCompatibleDC(sdc), bmp = CreateCompatibleBitmap(sdc, w, hh);
        IntPtr old = SelectObject(mdc, bmp);
        PrintWindow(h, mdc, 1 | 2); // PW_CLIENTONLY | PW_RENDERFULLCONTENT
        SelectObject(mdc, old);
        var bi = new BITMAPINFOHEADER { biSize = 40, biWidth = w, biHeight = -hh, biPlanes = 1, biBitCount = 24 };
        int stride = (w * 3 + 3) & ~3;
        byte[] bits = new byte[stride * hh];
        GetDIBits(mdc, bmp, 0, (uint)hh, bits, ref bi, 0);
        DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(h, sdc);
        byte[] rgb = new byte[w * 3 * hh];
        for (int y = 0; y < hh; y++)
            for (int x = 0; x < w; x++)
            {
                int s = y * stride + x * 3, d = (y * w + x) * 3;
                rgb[d] = bits[s + 2]; rgb[d + 1] = bits[s + 1]; rgb[d + 2] = bits[s];
            }
        return rgb;
    }

    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] static extern uint MapVirtualKey(uint code, uint type);

    /// <summary>Posts a key press or release to the window (no focus needed). SDL reads the scan
    /// code from lParam, so it is filled in from the virtual key.</summary>
    public static void Key(IntPtr h, int vk, bool down, bool extended)
    {
        uint scan = MapVirtualKey((uint)vk, 0);
        long lp = 1 | ((long)scan << 16) | (extended ? 1L << 24 : 0);
        if (!down) lp |= (1L << 30) | (1L << 31);
        bool sys = vk == 0x12; // VK_MENU goes as WM_SYSKEYDOWN/UP, like a real Alt press
        uint msg = down ? (sys ? 0x104u : 0x100u) : (sys ? 0x105u : 0x101u);
        PostMessage(h, msg, (IntPtr)vk, (IntPtr)lp);
    }

    /// <summary>Number of pixels that differ between two RGB frames of the same size.</summary>
    public static int DiffCount(byte[] a, byte[] b)
    {
        if (a == null || b == null || a.Length != b.Length) return int.MaxValue;
        int n = 0;
        for (int i = 0; i < a.Length; i += 3)
            if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2]) n++;
        return n;
    }

    static uint[] crcTable;
    static uint Crc(byte[] b, uint c)
    {
        if (crcTable == null)
        {
            crcTable = new uint[256];
            for (uint i = 0; i < 256; i++) { uint k = i; for (int j = 0; j < 8; j++) k = (k & 1) != 0 ? 0xEDB88320 ^ (k >> 1) : k >> 1; crcTable[i] = k; }
        }
        foreach (byte x in b) c = crcTable[(c ^ x) & 0xFF] ^ (c >> 8);
        return c;
    }
    static byte[] BE(int v) { return new byte[] { (byte)(v >> 24), (byte)(v >> 16), (byte)(v >> 8), (byte)v }; }
    static void Chunk(Stream s, string type, byte[] data)
    {
        s.Write(BE(data.Length), 0, 4);
        byte[] t = System.Text.Encoding.ASCII.GetBytes(type);
        s.Write(t, 0, 4); s.Write(data, 0, data.Length);
        s.Write(BE((int)(Crc(data, Crc(t, 0xFFFFFFFF)) ^ 0xFFFFFFFF)), 0, 4);
    }
    public static void SavePng(string path, byte[] rgb, int w, int h)
    {
        byte[] raw = new byte[(w * 3 + 1) * h];
        for (int y = 0; y < h; y++) Array.Copy(rgb, y * w * 3, raw, y * (w * 3 + 1) + 1, w * 3);
        var z = new MemoryStream();
        using (var zs = new ZLibStream(z, CompressionLevel.Optimal, true)) zs.Write(raw, 0, raw.Length);
        using (var f = File.Create(path))
        {
            f.Write(new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A }, 0, 8);
            byte[] ihdr = new byte[13];
            Array.Copy(BE(w), 0, ihdr, 0, 4); Array.Copy(BE(h), 0, ihdr, 4, 4);
            ihdr[8] = 8; ihdr[9] = 2;
            Chunk(f, "IHDR", ihdr); Chunk(f, "IDAT", z.ToArray()); Chunk(f, "IEND", new byte[0]);
        }
    }
}
