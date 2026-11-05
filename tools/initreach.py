"""Which imports can the static initializers reach?  Static analysis only.

Walks the code reachable from every __mod_init_func entry by recursive descent: direct
calls and jumps are followed, calls through symbol stubs or non-lazy pointers are recorded
as imports, and indirect calls through registers/memory (virtual calls, function pointers)
are counted as unknowns, since their targets are not visible statically.

usage: python -I initreach.py <macho> [--report report.txt]
"""
import collections
import os
import struct
import sys

import capstone
from capstone import x86

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machoinfo import parse, symbols, INDIRECT_SYMBOL_LOCAL, INDIRECT_SYMBOL_ABS  # noqa: E402

ENDERS = {x86.X86_INS_RET, x86.X86_INS_RETF, x86.X86_INS_HLT, x86.X86_INS_UD2, x86.X86_INS_INT3}


def main():
    path = sys.argv[1]
    report = open(sys.argv[sys.argv.index("--report") + 1], "w") if "--report" in sys.argv else None
    with open(path, "rb") as f:
        data, info = parse(f.read())
    segs = info["segments"]
    sects = {(seg["name"], s["name"]): s for seg in segs for s in seg["sections"]}
    syms = symbols(data, info)
    dy = info["dysymtab"]
    ind = struct.unpack_from("<%dI" % dy["nindirectsyms"], data, dy["indirectsymoff"])

    def u32(a):
        for seg in segs:
            if seg["vmaddr"] <= a < seg["vmaddr"] + seg["filesize"]:
                return struct.unpack_from("<I", data, seg["fileoff"] + a - seg["vmaddr"])[0]
        return None

    # import name for each stub and each non-lazy pointer slot
    imp_at = {}
    for (segname, sname), s in sects.items():
        kind = s["flags"] & 0xFF
        if kind not in (6, 7, 8):
            continue
        stride = 4 if kind != 8 else s["reserved2"]
        for j in range(s["size"] // stride):
            idx = ind[s["reserved1"] + j]
            if idx & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS):
                continue
            sym = syms[idx]
            if (sym["type"] & 0x0E) == 0x0E:    # the game's own (weak) definition
                imp_at[s["addr"] + j * stride] = ("self", sym["value"])
            else:
                imp_at[s["addr"] + j * stride] = ("import", sym["name"])

    code_secs = [s for s in sects.values() if s["flags"] & 0x80000400 and s["name"] != "__symbol_stub"]
    stubs = sects[("__TEXT", "__symbol_stub")]

    def code_bytes(a):
        for s in code_secs:
            if s["addr"] <= a < s["addr"] + s["size"]:
                o = s["offset"] + a - s["addr"]
                return data[o:o + 16]
        return None

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    init = sects[("__DATA", "__mod_init_func")]
    roots = [u32(a) for a in range(init["addr"], init["addr"] + init["size"], 4)]

    imports = collections.defaultdict(set)      # import name -> initializers reaching it
    unknown_calls = collections.Counter()       # initializer -> indirect calls seen
    funcs_done = {}                             # function start -> (imports, callees, indirect)
    bad = []

    def walk_function(start):
        """Intra-procedural recursive descent; returns (imports, callees, n_indirect)."""
        if start in funcs_done:
            return funcs_done[start]
        imps, callees, nind = set(), set(), 0
        seen, todo = set(), [start]
        while todo:
            a = todo.pop()
            while a not in seen:
                b = code_bytes(a)
                if b is None:
                    if stubs["addr"] <= a < stubs["addr"] + stubs["size"]:
                        kind, v = imp_at.get(a, ("?", None))
                        if kind == "import":
                            imps.add(v)
                        elif kind == "self":
                            callees.add(v)
                    else:
                        bad.append((start, a))
                    break
                seen.add(a)
                ins = next(md.disasm(b, a), None)
                if ins is None:
                    bad.append((start, a))
                    break
                nxt = a + ins.size
                op = ins.operands[0] if ins.operands else None
                # loads through a non-lazy pointer: data imports (or a function's address)
                if ins.id != x86.X86_INS_CALL:
                    for o in ins.operands:
                        if o.type == x86.X86_OP_MEM and o.mem.base == 0 and o.mem.index == 0:
                            k = imp_at.get(o.mem.disp & 0xFFFFFFFF)
                            if k and k[0] == "import":
                                imps.add(k[1])
                if ins.id == x86.X86_INS_CALL:
                    if op.type == x86.X86_OP_IMM:
                        t = op.imm & 0xFFFFFFFF
                        if t != nxt:            # call $+5 is the PIC idiom, not a call
                            if stubs["addr"] <= t < stubs["addr"] + stubs["size"]:
                                kind, v = imp_at.get(t, ("?", None))
                                if kind == "import":
                                    imps.add(v)
                                elif kind == "self":
                                    callees.add(v)
                            else:
                                callees.add(t)
                    elif op.type == x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and \
                            ("import" == imp_at.get(op.mem.disp & 0xFFFFFFFF, ("",))[0]):
                        imps.add(imp_at[op.mem.disp & 0xFFFFFFFF][1])
                    else:
                        nind += 1
                    a = nxt
                elif ins.id == x86.X86_INS_JMP:
                    if op.type == x86.X86_OP_IMM:
                        t = op.imm & 0xFFFFFFFF
                        if stubs["addr"] <= t < stubs["addr"] + stubs["size"]:
                            kind, v = imp_at.get(t, ("?", None))
                            if kind == "import":
                                imps.add(v)
                            elif kind == "self":
                                callees.add(v)
                        else:
                            todo.append(t)      # tail call or intra-function jump: same walk
                    else:
                        nind += 1               # switch tables / tail calls through pointers
                    break
                elif ins.group(capstone.CS_GRP_JUMP) and op is not None and op.type == x86.X86_OP_IMM:
                    todo.append(op.imm & 0xFFFFFFFF)
                    a = nxt
                elif ins.id in ENDERS:
                    break
                else:
                    a = nxt
        funcs_done[start] = (imps, callees, nind)
        return funcs_done[start]

    reach_size = collections.Counter()
    for r in roots:
        seen_f, todo = set(), [r]
        while todo:
            f = todo.pop()
            if f in seen_f:
                continue
            seen_f.add(f)
            imps, callees, nind = walk_function(f)
            for i in imps:
                imports[i].add(r)
            unknown_calls[r] += nind
            todo.extend(callees)
        reach_size[r] = len(seen_f)

    by_lib = collections.Counter()
    libs = {}
    for s in syms:
        if s["name"] in imports and not (s["type"] & 0x0E) == 0x0E:
            o = (s["desc"] >> 8) & 0xFF
            dl = [d for k, d in info["dylibs"] if k == "load"]
            libs[s["name"]] = dl[o - 1].rsplit("/", 1)[-1] if 0 < o <= len(dl) else "?"
    for name in imports:
        by_lib[libs.get(name, "?")] += 1

    total_funcs = len(funcs_done)
    pure = sum(1 for r in roots if unknown_calls[r] == 0 and not any(r in v for v in imports.values()))
    print("initializers: %d, functions reached: %d, imports reached: %d, decode problems: %d" % (
        len(roots), total_funcs, len(imports), len(bad)))
    print("initializers touching no import and no indirect call: %d" % pure)
    print("indirect calls reached (targets unknown statically): %d" % sum(unknown_calls.values()))
    print("imports by library:", dict(by_lib.most_common()))
    out = report or sys.stdout
    print("\nimport, library, number of initializers reaching it:", file=out)
    for name in sorted(imports, key=lambda n: (libs.get(n, "?"), n)):
        print("  %-24s %-60s %d" % (libs.get(name, "?"), name, len(imports[name])), file=out)
    print("\nlargest initializers (functions reached, indirect calls):", file=out)
    for r, n in reach_size.most_common(15):
        print("  0x%06x %6d %6d" % (r, n, unknown_calls[r]), file=out)
    for start, a in bad[:20]:
        print("  decode problem in function 0x%06x at 0x%06x" % (start, a), file=out)


if __name__ == "__main__":
    main()
