"""Dump the structure of an i386 Mach-O: segments, sections, entry, dyld info, imports.

usage: python -I machoinfo.py <macho> [--imports] [--json out.json]
"""
import json
import struct
import sys

LC_SEGMENT = 0x1
LC_SYMTAB = 0x2
LC_UNIXTHREAD = 0x5
LC_DYSYMTAB = 0xB
LC_LOAD_DYLIB = 0xC
LC_ID_DYLIB = 0xD
LC_LOAD_DYLINKER = 0xE
LC_DYLD_INFO = 0x22
LC_DYLD_INFO_ONLY = 0x80000022
LC_LOAD_WEAK_DYLIB = 0x80000018
LC_REEXPORT_DYLIB = 0x8000001F

INDIRECT_SYMBOL_LOCAL = 0x80000000
INDIRECT_SYMBOL_ABS = 0x40000000

S_NON_LAZY_SYMBOL_POINTERS = 0x6
S_LAZY_SYMBOL_POINTERS = 0x7
S_SYMBOL_STUBS = 0x8


def cstr(b):
    return b.split(b"\0", 1)[0].decode("latin-1")


def thin_i386(data):
    """Return the i386 slice of a (possibly fat) Mach-O."""
    magic = struct.unpack_from(">I", data, 0)[0]
    if magic == 0xCAFEBABE:
        n = struct.unpack_from(">I", data, 4)[0]
        for i in range(n):
            cpu, _sub, off, size, _align = struct.unpack_from(">5I", data, 8 + 20 * i)
            if cpu == 7:
                return data[off:off + size]
        raise SystemExit("fat binary has no i386 slice")
    return data


def parse(data):
    data = thin_i386(data)
    magic, cpu, _sub, ftype, ncmds, _sz, flags = struct.unpack_from("<7I", data, 0)
    if magic != 0xFEEDFACE:
        raise SystemExit("not a 32-bit little-endian Mach-O (magic %08x)" % magic)
    info = {"cputype": cpu, "filetype": ftype, "flags": flags, "segments": [], "dylibs": []}
    off = 28
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<2I", data, off)
        if cmd == LC_SEGMENT:
            name = cstr(data[off + 8:off + 24])
            vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, sflags = \
                struct.unpack_from("<8I", data, off + 24)
            seg = {"name": name, "vmaddr": vmaddr, "vmsize": vmsize, "fileoff": fileoff,
                   "filesize": filesize, "initprot": initprot, "sections": []}
            so = off + 56
            for _ in range(nsects):
                sect = cstr(data[so:so + 16])
                addr, ssize, soff, align, reloff, nreloc, sfl, r1, r2 = \
                    struct.unpack_from("<9I", data, so + 32)
                seg["sections"].append({"name": sect, "addr": addr, "size": ssize, "offset": soff,
                                        "flags": sfl, "reserved1": r1, "reserved2": r2,
                                        "nreloc": nreloc})
                so += 68
            info["segments"].append(seg)
        elif cmd == LC_UNIXTHREAD:
            # flavor, count, then i386_thread_state_t: eax..gs; eip is the 11th field
            regs = struct.unpack_from("<16I", data, off + 16)
            info["entry"] = regs[10]
        elif cmd in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY):
            f = struct.unpack_from("<10I", data, off + 8)
            info["dyld_info"] = dict(zip(
                ["rebase_off", "rebase_size", "bind_off", "bind_size", "weak_off", "weak_size",
                 "lazy_off", "lazy_size", "export_off", "export_size"], f))
        elif cmd == LC_SYMTAB:
            symoff, nsyms, stroff, strsize = struct.unpack_from("<4I", data, off + 8)
            info["symtab"] = {"symoff": symoff, "nsyms": nsyms, "stroff": stroff, "strsize": strsize}
        elif cmd == LC_DYSYMTAB:
            f = struct.unpack_from("<18I", data, off + 8)
            info["dysymtab"] = {"iundefsym": f[4], "nundefsym": f[5],
                                "indirectsymoff": f[12], "nindirectsyms": f[13],
                                "extreloff": f[14], "nextrel": f[15],
                                "locreloff": f[16], "nlocrel": f[17]}
        elif cmd in (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_ID_DYLIB):
            noff = struct.unpack_from("<I", data, off + 8)[0]
            kind = "id" if cmd == LC_ID_DYLIB else "load"
            info["dylibs"].append((kind, cstr(data[off + noff:off + size])))
        off += size
    return data, info


def symbols(data, info):
    st = info["symtab"]
    strtab = data[st["stroff"]:st["stroff"] + st["strsize"]]
    out = []
    for i in range(st["nsyms"]):
        strx, ntype, sect, desc, value = struct.unpack_from("<IBBhI", data, st["symoff"] + 12 * i)
        name = cstr(strtab[strx:]) if strx < len(strtab) else ""
        out.append({"name": name, "type": ntype, "sect": sect, "desc": desc, "value": value})
    return out


def imports(data, info):
    """Map every symbol-pointer / stub slot to the imported symbol it resolves to."""
    syms = symbols(data, info)
    dy = info["dysymtab"]
    ind = struct.unpack_from("<%dI" % dy["nindirectsyms"], data, dy["indirectsymoff"])
    slots = []
    for seg in info["segments"]:
        for s in seg["sections"]:
            kind = s["flags"] & 0xFF
            if kind in (S_NON_LAZY_SYMBOL_POINTERS, S_LAZY_SYMBOL_POINTERS):
                stride = 4
            elif kind == S_SYMBOL_STUBS:
                stride = s["reserved2"]
            else:
                continue
            for j in range(s["size"] // stride):
                idx = ind[s["reserved1"] + j]
                if idx & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS):
                    continue
                slots.append({"addr": s["addr"] + j * stride, "section": s["name"],
                              "kind": {6: "nl_ptr", 7: "la_ptr", 8: "stub"}[kind],
                              "symbol": syms[idx]["name"],
                              "lib_ordinal": (syms[idx]["desc"] >> 8) & 0xFF})
    return slots


def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        data, info = parse(f.read())
    print("cputype=%d filetype=%d flags=0x%x entry=0x%08x" % (
        info["cputype"], info["filetype"], info["flags"], info.get("entry", 0)))
    for kind, d in info["dylibs"]:
        print("  dylib[%s] %s" % (kind, d))
    for seg in info["segments"]:
        print("%-16s vm 0x%08x-0x%08x file 0x%08x+0x%x prot %d" % (
            seg["name"], seg["vmaddr"], seg["vmaddr"] + seg["vmsize"], seg["fileoff"],
            seg["filesize"], seg["initprot"]))
        for s in seg["sections"]:
            print("    %-18s 0x%08x size 0x%-8x flags 0x%08x nreloc %d" % (
                s["name"], s["addr"], s["size"], s["flags"], s["nreloc"]))
    for k in ("dyld_info", "symtab", "dysymtab"):
        if k in info:
            print(k, info[k])
    slots = imports(data, info) if "dysymtab" in info else []
    uniq = sorted({s["symbol"] for s in slots})
    print("import slots: %d, unique imported symbols: %d" % (len(slots), len(uniq)))
    if "--imports" in sys.argv:
        for name in uniq:
            print("  ", name)
    if "--json" in sys.argv:
        out = sys.argv[sys.argv.index("--json") + 1]
        with open(out, "w") as f:
            json.dump({"info": info, "imports": slots}, f, indent=1)


if __name__ == "__main__":
    main()
