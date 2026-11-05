"""Dump the legacy (ObjC 1.0, i386) runtime metadata: classes, methods, categories, class refs, selectors.

usage: python -I objc1dump.py <macho>
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machoinfo import parse  # noqa: E402


class Image:
    def __init__(self, data, info):
        self.data = data
        self.segs = info["segments"]
        self.sects = {s["name"]: s for seg in self.segs for s in seg["sections"]
                      if seg["name"] == "__OBJC"}

    def off(self, addr):
        for seg in self.segs:
            if seg["vmaddr"] <= addr < seg["vmaddr"] + seg["filesize"]:
                return seg["fileoff"] + addr - seg["vmaddr"]
        return None

    def u32(self, addr):
        o = self.off(addr)
        return struct.unpack_from("<I", self.data, o)[0] if o is not None else 0

    def cstr(self, addr):
        o = self.off(addr)
        if o is None:
            return "<0x%x>" % addr
        return self.data[o:self.data.index(b"\0", o)].decode("latin-1")


def methods(img, mlist):
    out = []
    while mlist:
        # objc_method_list: obsolete next, count, then {SEL name, char *types, IMP}
        count = img.u32(mlist + 4)
        for i in range(count):
            e = mlist + 8 + 12 * i
            out.append((img.cstr(img.u32(e)), img.cstr(img.u32(e + 4)), img.u32(e + 8)))
        mlist = 0  # method lists in a static image are not chained
    return out


def dump_class(img, cls, label):
    sup, name = img.u32(cls + 4), img.u32(cls + 8)
    info, isize, ivars, mlists = img.u32(cls + 16), img.u32(cls + 20), img.u32(cls + 24), img.u32(cls + 28)
    meta = img.u32(cls)
    print("%s %s : %s  (info 0x%x, instance_size %d)" % (
        label, img.cstr(name), img.cstr(sup) if sup else "-", info, isize))
    if ivars:
        n = img.u32(ivars)
        for i in range(n):
            e = ivars + 4 + 12 * i
            print("    ivar %-24s %-18s @%d" % (img.cstr(img.u32(e)), img.cstr(img.u32(e + 4)), img.u32(e + 8)))
    for sel, types, imp in methods(img, mlists):
        print("    - %-48s %-16s 0x%06x" % (sel, types, imp))
    if meta:
        for sel, types, imp in methods(img, img.u32(meta + 28)):
            print("    + %-48s %-16s 0x%06x" % (sel, types, imp))


def main():
    with open(sys.argv[1], "rb") as f:
        data, info = parse(f.read())
    img = Image(data, info)
    mod = img.sects["__module_info"]
    # objc_module: version, size, name, symtab
    for m in range(mod["addr"], mod["addr"] + mod["size"], 16):
        symtab = img.u32(m + 12)
        if not symtab or img.off(symtab) is None:
            continue
        # objc_symtab: sel_ref_cnt, refs, cls_def_cnt(u16), cat_def_cnt(u16), defs[]
        cls_cnt, cat_cnt = struct.unpack_from("<HH", data, img.off(symtab + 8))
        defs = [img.u32(symtab + 12 + 4 * i) for i in range(cls_cnt + cat_cnt)]
        for cls in defs[:cls_cnt]:
            dump_class(img, cls, "@class")
        for cat in defs[cls_cnt:]:
            # objc_category: name, class_name, instance_methods, class_methods, protocols
            print("@category %s (%s)" % (img.cstr(img.u32(cat + 4)), img.cstr(img.u32(cat))))
            for sel, types, imp in methods(img, img.u32(cat + 8)):
                print("    - %-48s %-16s 0x%06x" % (sel, types, imp))
            for sel, types, imp in methods(img, img.u32(cat + 12)):
                print("    + %-48s %-16s 0x%06x" % (sel, types, imp))
    refs = img.sects["__cls_refs"]
    print("class refs:", ", ".join(sorted({img.cstr(img.u32(a)) for a in range(refs["addr"], refs["addr"] + refs["size"], 4)})))
    msgs = img.sects["__message_refs"]
    print("selectors used (%d):" % (msgs["size"] // 4))
    print("   " + " ".join(sorted({img.cstr(img.u32(a)) for a in range(msgs["addr"], msgs["addr"] + msgs["size"], 4)})))


if __name__ == "__main__":
    main()
