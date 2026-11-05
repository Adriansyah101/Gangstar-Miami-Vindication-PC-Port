"""Pack an add-on folder (crusher, radio) into one masked file, or unpack it again.

usage: python -I uc_crusher_pack.py pack   <add-on\\crusher folder> <add-on\\crusher.pak>
       python -I uc_crusher_pack.py unpack <add-on\\crusher.pak> <folder>

The game reads add-on\\crusher.pak in place of the folder (ucveh.c, gmv_pak_unpack) and unpacks it to
cache\\uc\\crusher; add-on\\radio.pak the same way to cache\\radio\\songs (radiosongs.c).  Layout: "GMVP",
u32 version 1, then every byte from offset 8 on xored with mask(offset): u32 count, u32 FNV-1a of the
files' bytes, count x {u8 name length, name, u32 size}, then the files' bytes in that order.  The mask
only keeps names and contents from reading as they are.  The files are the user's own copies of Urban
Crime's and of the iPhone version's: never ship a pack.
"""
import os
import struct
import sys

MAGIC, VERSION = b"GMVP", 1


def mask_bytes(start, n):
    out = bytearray(n)
    for i in range(n):
        x = (((start + i) * 2654435761) & 0xFFFFFFFF) ^ 0x5A17C3E9
        x ^= x >> 13
        x = (x * 0x5BD1E995) & 0xFFFFFFFF
        x ^= x >> 15
        out[i] = x & 0xFF
    return out


def xor_from(buf, start):
    m = mask_bytes(start, len(buf) - start)
    for i in range(start, len(buf)):
        buf[i] ^= m[i - start]


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def pack(folder, out):
    names = sorted(n for n in os.listdir(folder) if os.path.isfile(os.path.join(folder, n)))
    if not names:
        sys.exit("no files in " + folder)
    blobs = [open(os.path.join(folder, n), "rb").read() for n in names]
    data = b"".join(blobs)
    buf = bytearray(MAGIC + struct.pack("<III", VERSION, len(names), fnv1a(data)))
    for n, b in zip(names, blobs):
        e = n.encode("ascii")
        if len(e) > 255:
            sys.exit("name too long: " + n)
        buf += bytes([len(e)]) + e + struct.pack("<I", len(b))
    buf += data
    xor_from(buf, 8)
    open(out, "wb").write(buf)
    print("packed %d files (%d bytes) into %s" % (len(names), len(buf), out))


def unpack(pak, folder):
    buf = bytearray(open(pak, "rb").read())
    if buf[:4] != MAGIC or struct.unpack_from("<I", buf, 4)[0] != VERSION:
        sys.exit(pak + " is not an add-on pack")
    xor_from(buf, 8)
    cnt, s = struct.unpack_from("<II", buf, 8)
    o, entries = 16, []
    for _ in range(cnt):
        l = buf[o]
        name = buf[o + 1:o + 1 + l].decode("ascii")
        entries.append((name, struct.unpack_from("<I", buf, o + 1 + l)[0]))
        o += 1 + l + 4
    if o + sum(sz for _, sz in entries) != len(buf) or fnv1a(buf[o:]) != s:
        sys.exit(pak + " is damaged")
    os.makedirs(folder, exist_ok=True)
    for name, sz in entries:
        if os.path.basename(name) != name or name in ("", ".", ".."):
            sys.exit("bad name in pack: " + name)
        open(os.path.join(folder, name), "wb").write(buf[o:o + sz])
        o += sz
    print("unpacked %d files into %s" % (cnt, folder))


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] not in ("pack", "unpack"):
        sys.exit(__doc__)
    (pack if sys.argv[1] == "pack" else unpack)(sys.argv[2], sys.argv[3])
