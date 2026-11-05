"""Convert Urban Crime's PVRTC textures for the Crusher add-on into the DDS (DXT5) the Mac game reads.

usage: python -I uc_crusher_tex.py <add-on\\crusher folder> <Urban Crime _packMac folder> [--png]

Reads car_update_texture_d.tga and car_update_texture_damage_d.tga from the Urban Crime pack (PVR v2,
PVRTC 2 bpp, twiddled), decodes the full-size level, builds a box-filtered mip chain down to 1x1 and
writes each as a DDS with DXT5 blocks under the same name into the add-on folder (the Mac pack keeps its
".tga" textures as DDS too).  --png also writes <name>.png previews of the decoded picture.
The files written are the user's own conversion of their own copy: never ship them.
"""
import os
import struct
import sys
import zlib

NAMES = ("car_update_texture_d.tga", "car_update_texture_damage_d.tga")


# ---- PVRTC 2 bpp decoding (after Imagination's PVRTDecompress) ----------------------------------

def twiddle(xsize, ysize, x, y):
    mind, maxv = xsize, y
    if ysize < xsize:
        mind, maxv = ysize, x
    tw, src, dst, shift = 0, 1, 1, 0
    while src < mind:
        if y & src:
            tw |= dst
        if x & src:
            tw |= dst << 1
        src <<= 1
        dst <<= 2
        shift += 1
    return tw | ((maxv >> shift) << (2 * shift))


def color_a(c):
    if c & 0x8000:
        return ((c & 0x7c00) >> 10, (c & 0x3e0) >> 5, (c & 0x1e) | ((c & 0x1e) >> 4), 0xf)
    return (((c & 0xf00) >> 7) | ((c & 0xf00) >> 11), ((c & 0xf0) >> 3) | ((c & 0xf0) >> 7),
            ((c & 0xe) << 1) | ((c & 0xe) >> 2), (c & 0x7000) >> 11)


def color_b(c):
    if c & 0x80000000:
        return ((c & 0x7c000000) >> 26, (c & 0x3e00000) >> 21, (c & 0x1f0000) >> 16, 0xf)
    return (((c & 0xf000000) >> 23) | ((c & 0xf000000) >> 27), ((c & 0xf00000) >> 19) | ((c & 0xf00000) >> 23),
            ((c & 0xf0000) >> 15) | ((c & 0xf0000) >> 19), (c & 0x70000000) >> 27)


def interpolate(P, Q, R, S):
    """8x4 upscale of four 5554 colours, P at the top-left texel: out[y][x] = (r, g, b, a) 8 bit."""
    out = [[None] * 8 for _ in range(4)]
    hP = [p * 8 for p in P]
    hR = [r * 8 for r in R]
    qp = [q - p for q, p in zip(Q, P)]
    sr = [s - r for s, r in zip(S, R)]
    for x in range(8):
        res = [4 * v for v in hP]
        dy = [r - p for r, p in zip(hR, hP)]
        for y in range(4):
            out[y][x] = ((res[0] >> 7) + (res[0] >> 2), (res[1] >> 7) + (res[1] >> 2),
                         (res[2] >> 7) + (res[2] >> 2), (res[3] >> 5) + (res[3] >> 1))
            res = [a + b for a, b in zip(res, dy)]
        hP = [a + b for a, b in zip(hP, qp)]
        hR = [a + b for a, b in zip(hR, sr)]
    return out


def unpack_mod(mod, col, ox, oy, vals, modes):
    mode = col & 1
    bits = mod
    if mode:
        if bits & 1:
            mode = 3 if bits & (1 << 20) else 2
            if bits & (1 << 21):
                bits |= 1 << 20
            else:
                bits &= ~(1 << 20)
        if bits & 2:
            bits |= 1
        else:
            bits &= ~1
        for y in range(4):
            for x in range(8):
                modes[x + ox][y + oy] = mode
                if ((x ^ y) & 1) == 0:
                    vals[x + ox][y + oy] = bits & 3
                    bits >>= 2
    else:
        for y in range(4):
            for x in range(8):
                modes[x + ox][y + oy] = 0
                vals[x + ox][y + oy] = 3 if bits & 1 else 0
                bits >>= 1


REP = (0, 3, 5, 8)


def mod_value(vals, modes, x, y):
    m = modes[x][y]
    if m == 0 or ((x ^ y) & 1) == 0:
        return REP[vals[x][y]]
    if m == 1:
        return (REP[vals[x][y - 1]] + REP[vals[x][y + 1]] + REP[vals[x - 1][y]] + REP[vals[x + 1][y]] + 2) // 4
    if m == 2:
        return (REP[vals[x - 1][y]] + REP[vals[x + 1][y]] + 1) // 2
    return (REP[vals[x][y - 1]] + REP[vals[x][y + 1]] + 1) // 2


def decode_pvrtc2(data, w, h):
    nx, ny = w // 8, h // 4
    words = struct.unpack_from("<%dI" % (nx * ny * 2), data)
    img = [[None] * w for _ in range(h)]
    for wy in range(ny):
        for wx in range(nx):
            idx = [(wx, wy), ((wx + 1) % nx, wy), (wx, (wy + 1) % ny), ((wx + 1) % nx, (wy + 1) % ny)]
            ws = []
            for (x, y) in idx:
                o = twiddle(nx, ny, x, y) * 2
                ws.append((words[o], words[o + 1]))
            A = interpolate(*[color_a(c) for _, c in ws])
            B = interpolate(*[color_b(c) for _, c in ws])
            vals = [[0] * 8 for _ in range(16)]
            modes = [[0] * 8 for _ in range(16)]
            for k, (ox, oy) in enumerate(((0, 0), (8, 0), (0, 4), (8, 4))):
                unpack_mod(ws[k][0], ws[k][1], ox, oy, vals, modes)
            for y in range(4):
                py = (wy * 4 + 2 + y) % h
                row = img[py]
                for x in range(8):
                    m = mod_value(vals, modes, x + 4, y + 2)
                    a, b = A[y][x], B[y][x]
                    row[(wx * 8 + 4 + x) % w] = tuple((a[i] * (8 - m) + b[i] * m) // 8 for i in range(4))
    return img


# ---- mips, DXT5 and DDS -----------------------------------------------------------------------

def half(img):
    h, w = len(img), len(img[0])
    if w == 1 and h == 1:
        return None
    nw, nh = max(1, w // 2), max(1, h // 2)
    out = []
    for y in range(nh):
        row = []
        for x in range(nw):
            px = [img[min(2 * y + j, h - 1)][min(2 * x + i, w - 1)] for j in (0, 1) for i in (0, 1)]
            row.append(tuple((sum(p[c] for p in px) + 2) // 4 for c in range(4)))
        out.append(row)
    return out


def to565(c):
    return ((c[0] * 31 + 127) // 255) << 11 | ((c[1] * 63 + 127) // 255) << 5 | ((c[2] * 31 + 127) // 255)


def from565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def dxt5_block(px):
    al = [p[3] for p in px]
    a0, a1 = max(al), min(al)
    if a0 == a1:
        abits = 0
    else:
        lv = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
        abits = 0
        for k, a in enumerate(al):
            best = min(range(8), key=lambda i: abs(lv[i] - a))
            abits |= best << (3 * k)
    lo = [min(p[c] for p in px) for c in range(3)]
    hi = [max(p[c] for p in px) for c in range(3)]
    inset = [(h_ - l_) // 16 for h_, l_ in zip(hi, lo)]
    lo = [min(255, l_ + i) for l_, i in zip(lo, inset)]
    hi = [max(0, h_ - i) for h_, i in zip(hi, inset)]
    c0, c1 = to565(hi), to565(lo)
    if c0 < c1:
        c0, c1 = c1, c0
    cbits = 0
    if c0 != c1:
        e0, e1 = from565(c0), from565(c1)
        pal = [e0, e1, tuple((2 * a + b) // 3 for a, b in zip(e0, e1)), tuple((a + 2 * b) // 3 for a, b in zip(e0, e1))]
        for k, p in enumerate(px):
            best = min(range(4), key=lambda i: sum((pal[i][c] - p[c]) ** 2 for c in range(3)))
            cbits |= best << (2 * k)
    return struct.pack("<BB", a0, a1) + abits.to_bytes(6, "little") + struct.pack("<HHI", c0, c1, cbits)


def dxt5(img):
    h, w = len(img), len(img[0])
    out = bytearray()
    for by in range(0, max(h, 4), 4):
        for bx in range(0, max(w, 4), 4):
            px = [img[min(by + y, h - 1)][min(bx + x, w - 1)] for y in range(4) for x in range(4)]
            out += dxt5_block(px)
    return bytes(out)


def dds(levels):
    h, w = len(levels[0]), len(levels[0][0])
    hdr = struct.pack("<4s7I44x2I4s5I5I", b"DDS ", 124, 0x21007, h, w, 0, 0, len(levels),
                      32, 4, b"DXT5", 0, 0, 0, 0, 0, 0x401008, 0, 0, 0, 0)
    return hdr + b"".join(dxt5(l) for l in levels)


def png(img, path):
    h, w = len(img), len(img[0])
    raw = b"".join(b"\0" + bytes(c for p in row for c in p) for row in img)
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    out_dir, uc_dir = sys.argv[1], sys.argv[2]
    for name in NAMES:
        d = open(os.path.join(uc_dir, name), "rb").read()
        hs, h, w, mips, flags, size, bpp = struct.unpack_from("<7I", d, 0)
        if d[44:48] != b"PVR!" or flags & 0xff != 0x18 or bpp != 2:
            sys.exit("%s: not a PVR v2 PVRTC 2 bpp texture (flags %#x, bpp %d)" % (name, flags, bpp))
        img = decode_pvrtc2(d[hs:], w, h)
        if "--png" in sys.argv:
            png(img, os.path.join(out_dir, name[:-4] + ".png"))
        levels = [img]
        while True:
            n = half(levels[-1])
            if n is None:
                break
            levels.append(n)
        open(os.path.join(out_dir, name), "wb").write(dds(levels))
        print("%s: %dx%d, %d mip levels -> DXT5 DDS" % (name, w, h, len(levels)))


if __name__ == "__main__":
    main()
