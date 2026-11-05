"""Cross-check a GMVL link table against the Mach-O it was generated from.

Nothing here executes game code; it only reads both files and reports what the host would
leave unhandled: symbol-pointer slots nobody fills, relocs that overlap or land in zerofill,
binds outside __DATA, and what the weak-bind stream would have changed.

usage: python -I checklink.py <macho> <gmv_link.bin>
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machoinfo import parse, symbols, INDIRECT_SYMBOL_LOCAL, INDIRECT_SYMBOL_ABS  # noqa: E402
from genlink import uleb, sleb  # noqa: E402


def read_link(path):
    with open(path, "rb") as f:
        b = f.read()
    if b[:4] != b"GMVL":
        raise SystemExit("bad magic %r" % b[:4])
    ver, nrel, nbind, nsz = struct.unpack_from("<4I", b, 4)
    o = 20
    relocs = struct.unpack_from("<%dI" % nrel, b, o)
    o += 4 * nrel
    raw = [struct.unpack_from("<IiHBBI", b, o + 16 * i) for i in range(nbind)]
    o += 16 * nbind
    names = b[o:o + nsz]
    o += nsz
    nset = struct.unpack_from("<I", b, o)[0]
    sets = [struct.unpack_from("<II", b, o + 4 + 8 * i) for i in range(nset)]
    o += 4 + 8 * nset
    binds = []
    for slot, addend, ordinal, flags, _pad, noff in raw:
        name = names[noff:names.index(b"\0", noff)].decode("latin-1")
        binds.append((slot, addend, ordinal, flags, name))
    return ver, relocs, binds, sets, len(b) - o


def weak_binds(buf, segs):
    """Decode the weak-bind stream: (slot, name) entries plus strong-definition markers."""
    i, name, addr, out, strong = 0, "", 0, [], []
    while i < len(buf):
        b = buf[i]
        i += 1
        op, imm = b & 0xF0, b & 0x0F
        if op == 0x00:
            break
        elif op == 0x40:
            end = buf.index(b"\0", i)
            name = buf[i:end].decode("latin-1")
            i = end + 1
            if imm & 8:                 # BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION
                strong.append(name)
        elif op == 0x50:
            pass
        elif op == 0x60:
            _, i = sleb(buf, i)
        elif op == 0x70:
            off, i = uleb(buf, i)
            addr = segs[imm]["vmaddr"] + off
        elif op == 0x80:
            d, i = uleb(buf, i)
            addr = (addr + d) & 0xFFFFFFFF
        elif op == 0x90:
            out.append((addr, name))
            addr += 4
        elif op == 0xA0:
            out.append((addr, name))
            d, i = uleb(buf, i)
            addr = (addr + 4 + d) & 0xFFFFFFFF
        elif op == 0xB0:
            out.append((addr, name))
            addr += 4 + imm * 4
        elif op == 0xC0:
            n, i = uleb(buf, i)
            skip, i = uleb(buf, i)
            for _ in range(n):
                out.append((addr, name))
                addr += 4 + skip
        else:
            raise SystemExit("bad weak bind opcode 0x%02x at %d" % (b, i - 1))
    return out, strong


def main():
    macho, link = sys.argv[1], sys.argv[2]
    with open(macho, "rb") as f:
        data, info = parse(f.read())
    segs = info["segments"]
    sects = [(seg["name"], s) for seg in segs for s in seg["sections"]]
    ver, relocs, binds, sets, trailing = read_link(link)
    print("GMVL v%d: %d relocs, %d binds, %d sets, %d trailing bytes" % (
        ver, len(relocs), len(binds), len(sets), trailing))
    problems = (ver != 2) + (trailing != 0)

    def where(a):
        for segname, s in sects:
            if s["addr"] <= a < s["addr"] + s["size"]:
                return segname, s
        return None, None

    def u32(a):
        for seg in segs:
            if seg["vmaddr"] <= a < seg["vmaddr"] + seg["filesize"]:
                return struct.unpack_from("<I", data, seg["fileoff"] + a - seg["vmaddr"])[0]
        return None

    lo = segs[1]["vmaddr"]
    hi = max(s["vmaddr"] + s["vmsize"] for s in segs if s["name"] != "__LINKEDIT")

    # ---- relocs ----------------------------------------------------------------------
    rs = sorted(relocs)
    overlap = [(a, b) for a, b in zip(rs, rs[1:]) if b - a < 4]
    bad_place = collections.Counter()
    out_of_range = 0
    for a in rs:
        segname, s = where(a)
        if s is None or s["addr"] + s["size"] < a + 4:
            bad_place["outside any section"] += 1
            continue
        if s["flags"] & 0xFF == 1:
            bad_place["zerofill " + s["name"]] += 1
            continue
        if s["name"] in ("__stub_helper", "__unwind_info") or segname == "__LINKEDIT":
            bad_place[s["name"]] += 1
        v = u32(a)
        if v is None or not (lo <= v < hi):
            out_of_range += 1
    print("relocs: %d overlapping pairs, misplaced %s, value outside image %d" % (
        len(overlap), dict(bad_place) or "none", out_of_range))
    problems += len(overlap) + sum(bad_place.values()) + out_of_range
    for a, b in overlap[:10]:
        print("   overlap 0x%06x / 0x%06x" % (a, b))

    # ---- binds -----------------------------------------------------------------------
    slot_set = collections.Counter(b[0] for b in binds)
    dup = [s for s, n in slot_set.items() if n > 1]
    not_data = [b for b in binds if where(b[0])[0] not in ("__DATA", "__OBJC")]
    ords = collections.Counter(b[2] for b in binds)
    libs = [d for kind, d in info["dylibs"] if kind == "load"]
    print("binds: %d slots bound twice, %d outside __DATA/__OBJC" % (len(dup), len(not_data)))
    print("bind ordinals:", ", ".join("%s=%d" % (
        libs[o - 1].rsplit("/", 1)[-1] if 0 < o <= len(libs) else "ord%d" % o, n)
        for o, n in sorted(ords.items())))
    flags = collections.Counter(b[3] for b in binds)
    print("bind flags (1 weak, 2 lazy):", dict(flags))
    addends = [b for b in binds if b[1]]
    print("binds with addend: %d %s" % (len(addends), [(hex(b[0]), b[4], b[1]) for b in addends[:5]]))
    problems += len(not_data)
    relset = set(relocs)
    both = [b for b in binds if b[0] in relset]
    print("binds that are also relocs (would be double-patched): %d" % len(both))
    problems += len(both)

    # ---- sets (self / weak binds resolved to the game's own definitions) --------------
    set_bad = collections.Counter()
    bound_slots = {b[0] for b in binds}
    for slot, v in sets:
        segname, s = where(slot)
        if s is None or s["flags"] & 0xFF == 1:
            set_bad["slot not file-backed"] += 1
        if not (lo <= v < hi):
            set_bad["value outside image"] += 1
        if slot in relset:
            set_bad["also a reloc"] += 1
        if slot in bound_slots:
            set_bad["also bound"] += 1
    by_sect = collections.Counter((where(slot)[1] or {"name": "?"})["name"] for slot, _ in sets)
    print("sets by section:", dict(by_sect), "problems:", dict(set_bad) or "none")
    problems += sum(set_bad.values())

    # ---- every symbol pointer / stub slot ---------------------------------------------
    syms = symbols(data, info)
    dy = info["dysymtab"]
    ind = struct.unpack_from("<%dI" % dy["nindirectsyms"], data, dy["indirectsymoff"])
    bound = {b[0] for b in binds}
    setslots = {s for s, _ in sets}
    cover = collections.Counter()
    uncovered = []
    for segname, s in sects:
        kind = s["flags"] & 0xFF
        if kind not in (6, 7, 8):
            continue
        stride = 4 if kind != 8 else s["reserved2"]
        for j in range(s["size"] // stride):
            a = s["addr"] + j * stride
            idx = ind[s["reserved1"] + j]
            if kind == 8:
                # stub: jmp *[slot]; the slot itself must be filled by someone
                tgt = u32(a + 2)
                ok = (a + 2) in relset and tgt is not None and (tgt in bound or tgt in relset or tgt in setslots)
                cover["stub ok" if ok else "stub BAD"] += 1
                if not ok:
                    uncovered.append((a, s["name"], idx, tgt))
                continue
            if a in bound:
                cover[s["name"] + " bound"] += 1
            elif a in setslots:
                cover[s["name"] + " set"] += 1
            elif a in relset:
                cover[s["name"] + " reloc"] += 1
            elif idx & INDIRECT_SYMBOL_ABS:
                cover[s["name"] + " abs"] += 1
            else:
                cover[s["name"] + " NONE"] += 1
                uncovered.append((a, s["name"], idx, u32(a)))
    for k in sorted(cover):
        print("   %-28s %d" % (k, cover[k]))
    kinds = collections.Counter()
    for a, sname, idx, v in uncovered:
        if idx & INDIRECT_SYMBOL_LOCAL:
            kinds["local"] += 1
            continue
        sym = syms[idx]
        defined = sym["type"] & 0x0E == 0x0E
        weakdef = bool(sym["desc"] & 0x80)
        kinds["%s%s value=%s" % ("defined" if defined else "undef",
                                 " weak-def" if weakdef else "",
                                 "match" if v == sym["value"] else ("0" if v == 0 else "other"))] += 1
    print("uncovered slots: %d -> %s" % (len(uncovered), dict(kinds)))
    for a, sname, idx, v in uncovered[:12]:
        name = "<local>" if idx & INDIRECT_SYMBOL_LOCAL else syms[idx]["name"]
        sv = syms[idx]["value"] if not idx & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS) else 0
        print("   0x%06x %-16s now=0x%08x sym=0x%08x %s" % (a, sname, v or 0, sv, name[:90]))
    problems += len(uncovered)

    # ---- weak binding ----------------------------------------------------------------
    d = info["dyld_info"]
    wb, strong = weak_binds(data[d["weak_off"]:d["weak_off"] + d["weak_size"]], segs)
    wslots = collections.Counter()
    for a, name in wb:
        segname, s = where(a)
        tag = (s["name"] if s else "?") + (" (reloc)" if a in relset else "") + (" (bound)" if a in bound else "")
        wslots[tag] += 1
    print("weak-bind: %d slot entries %s, %d strong (non-weak) definitions" % (len(wb), dict(wslots), len(strong)))
    for n in strong[:20]:
        print("   strong def:", n)

    # ---- program vars and init funcs --------------------------------------------------
    for name in ("__program_vars", "__mod_init_func"):
        s = next(s for _, s in sects if s["name"] == name)
        miss = [a for a in range(s["addr"], s["addr"] + s["size"], 4) if a not in relset and (u32(a) or 0) >= lo]
        print("%s: %d words, %d pointer-looking words not relocated" % (name, s["size"] // 4, len(miss)))
        problems += len(miss)

    print("entry eip 0x%06x is %s" % (info["entry"], where(info["entry"])[1]["name"]))
    print("PROBLEMS: %d" % problems)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
