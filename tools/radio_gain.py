"""Raise the iPhone version's radio songs to the Mac's level for add-on\\radio.

usage: py -3 -I radio_gain.py <the iPhone's _packMac folder> <add-on\\radio folder> [dB, default 3]

The Mac's copies of the songs both versions share are 3.0 dB louder than the iPhone's (17 of the 18; electro_05
is 5.8 dB), so the iPhone's nine extra songs sound quieter than the Mac's on the same station.  Each song is
decoded (IMA ADPCM, fmt 17), scaled and encoded again block by block with the same format and sizes, so only the
sample data changes.  A stereo block is a 4-byte header per channel, then 4 bytes (8 samples) of the left, 4 of
the right, and so on; low nibble first.  Encoding again adds a little noise: the report gives the signal-to-noise ratio and how many samples hit full
scale.  Reads the originals each time, so running it again does not add up.  Then pack the folder with
uc_crusher_pack.py.  Never ship the result.
"""
import concurrent.futures
import math
import os
import struct
import sys

SONGS = (
    "m_radio_electro_stompbox.wav", "m_radio_electro_jerkit.wav",
    "m_radio_hiphop_bump.wav", "m_radio_hiphop_volcano.wav", "m_radio_hiphop_jerkit.wav",
    "m_radio_hiphop_boombapcats.wav",
    "m_radio_rock_policesign.wav", "m_radio_rock_bigbadwolf.wav", "m_radio_rock_notime.wav",
)

STEPS = (7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
         107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
         876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
         4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
         24623, 27086, 29794, 32767)
ADJ = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)


def data_chunk(d):
    """(block align, data offset, data length) of a stereo IMA ADPCM wav."""
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        raise ValueError("not a wav")
    o, ch, align, fmt = 12, 0, 0, 0
    while o + 8 <= len(d):
        n = struct.unpack_from("<I", d, o + 4)[0]
        if d[o:o + 4] == b"fmt ":
            fmt, ch = struct.unpack_from("<HH", d, o + 8)
            align = struct.unpack_from("<H", d, o + 20)[0]
        elif d[o:o + 4] == b"data":
            if fmt != 17 or ch != 2 or not align or (align - 8) % 8:
                raise ValueError("not stereo IMA ADPCM in whole words")
            return align, o + 8, min(n, len(d) - o - 8)
        o += 8 + n + (n & 1)
    raise ValueError("no data chunk")


def one_channel(src, base, words, c, gain, steps, adj):
    """One channel of one block: its nibbles written in place in src, with the error totals."""
    head = base + 4 * c
    pred = struct.unpack_from("<h", src, head)[0]
    idx = min(src[head + 2], 88)
    out = pred * gain
    out = 32767 if out > 32767 else -32768 if out < -32768 else int(round(out))
    struct.pack_into("<h", src, head, out)
    # the steps rise about 10% an index: the block starts where the louder signal's steps would be
    eidx = idx + int(round(math.log(gain) / math.log(1.1)))
    eidx = 0 if eidx < 0 else 88 if eidx > 88 else eidx
    src[head + 2] = eidx
    src[head + 3] = 0
    enc = out
    err2 = sig2 = 0.0
    clip = 0
    for k in range(words):
        word = base + 8 + k * 8 + c * 4
        for s in range(8):
            at = word + (s >> 1)
            b = src[at]
            v = b >> 4 if s & 1 else b & 15
            # the original's next sample
            st = steps[idx]
            df = st >> 3
            if v & 4:
                df += st
            if v & 2:
                df += st >> 1
            if v & 1:
                df += st >> 2
            pred += -df if v & 8 else df
            pred = 32767 if pred > 32767 else -32768 if pred < -32768 else pred
            idx += adj[v]
            idx = 0 if idx < 0 else 88 if idx > 88 else idx
            # scaled
            want = pred * gain
            if want > 32767:
                want = 32767.0
                clip += 1
            elif want < -32768:
                want = -32768.0
                clip += 1
            # its nibble
            st = steps[eidx]
            delta = want - enc
            n = 0
            if delta < 0:
                n = 8
                delta = -delta
            if delta >= st:
                n |= 4
                delta -= st
            if delta >= st / 2:
                n |= 2
                delta -= st / 2
            if delta >= st / 4:
                n |= 1
            df = st >> 3
            if n & 4:
                df += st
            if n & 2:
                df += st >> 1
            if n & 1:
                df += st >> 2
            enc += -df if n & 8 else df
            enc = 32767 if enc > 32767 else -32768 if enc < -32768 else enc
            eidx += adj[n]
            eidx = 0 if eidx < 0 else 88 if eidx > 88 else eidx
            src[at] = ((b & 15) | n << 4) if s & 1 else ((b & 0xF0) | n)
            e = want - enc
            err2 += e * e
            sig2 += want * want
    return err2, sig2, clip


def one_song(args):
    in_dir, out_dir, name, gain = args
    with open(os.path.join(in_dir, name), "rb") as f:
        d = bytearray(f.read())
    align, at, n = data_chunk(d)
    words = (align - 8) // 8
    err2 = sig2 = 0.0
    clip = samples = 0
    for b in range(at, at + n - align + 1, align):
        for c in range(2):
            e, s, k = one_channel(d, b, words, c, gain, STEPS, ADJ)
            err2 += e
            sig2 += s
            clip += k
        samples += words * 16
    tmp = os.path.join(out_dir, name + ".tmp")
    with open(tmp, "wb") as f:
        f.write(d)
    os.replace(tmp, os.path.join(out_dir, name))
    return name, 10 * math.log10(sig2 / err2) if err2 else 999.0, 100.0 * clip / samples


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    in_dir, out_dir = sys.argv[1], sys.argv[2]
    db = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0
    if os.path.abspath(in_dir) == os.path.abspath(out_dir):
        sys.exit("the output folder must not be the originals' folder")
    os.makedirs(out_dir, exist_ok=True)
    gain = 10 ** (db / 20)
    jobs = [(in_dir, out_dir, s, gain) for s in SONGS]
    with concurrent.futures.ProcessPoolExecutor() as ex:
        for name, snr, clipped in ex.map(one_song, jobs):
            print(f"{name:36s} +{db} dB  signal/noise {snr:5.1f} dB  at full scale {clipped:.3f}%")


if __name__ == "__main__":
    main()
