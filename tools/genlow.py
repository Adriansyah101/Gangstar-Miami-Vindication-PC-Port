"""Build the patch table that lets the game's lowest code run from a copy.

The host maps the game at its own addresses (see layout.h in host/games/<game>).  Windows owns everything
below 0x10000 and the exe's PE header owns 0x10000..0x11000, so the game bytes
[0x1000, low_end) run from a copy at LOWCOPY_BASE instead.  low_end is the first function
start at or above 0x11000, so no function is split between the two places.  This tool
decodes that code and lists every patch the copy needs; nothing is guessed from data
values, and the host checks each patch's expected bytes before applying it.

usage: python -I genlow.py <macho> <out.bin> [--report report.txt]

out.bin layout (little endian):
    char magic[4] = "GMVP"; u32 version = 1
    u32 macho_size; u32 text_crc32          crc32 of the __TEXT segment's file bytes
    u32 low_start; u32 low_end              game range that runs from the copy
    u32 n_patch
    struct { u32 addr; u8 kind; u8 reg; u16 pad; } patch[n_patch]     sorted by addr
kinds (addr is a game vmaddr; "copy" means addr + LOW_DELTA):
    1 REL32_OUT  copy has a rel32 field at addr branching out of the range: field -= delta
    2 PIC_POP    copy has "call $+5; pop r" at addr: becomes "mov r, addr+5; nop"
    3 REL32_IN   game code outside the range has a rel32 call/jmp field at addr whose
                 target is a function start inside the range: field += delta
    4 PTR_IN     u32 at addr (outside the range) is an exact pointer slot holding a code
                 address inside the range: value += delta
reg: x86 register number (eax=0 .. edi=7) for PIC_POP, else 0.
Anything that still reaches the range at run time (function pointers built in code, PIC
jump tables) faults there and the host's exception handler redirects it into the copy.
"""
import bisect
import collections
import os
import struct
import sys
import zlib

import capstone
from capstone import x86

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machoinfo import parse, symbols  # noqa: E402
from genlink import run_binds  # noqa: E402

LOW_START = 0x1000
PE_HEADER_END = 0x11000
LOWCOPY_CAP = 0x20000
REL32_OUT, PIC_POP, REL32_IN, PTR_IN = 1, 2, 3, 4
REGNUM = {x86.X86_REG_EAX: 0, x86.X86_REG_ECX: 1, x86.X86_REG_EDX: 2, x86.X86_REG_EBX: 3,
          x86.X86_REG_ESP: 4, x86.X86_REG_EBP: 5, x86.X86_REG_ESI: 6, x86.X86_REG_EDI: 7}
BRANCH_GROUPS = (capstone.CS_GRP_JUMP, capstone.CS_GRP_CALL)


def main():
    path, out = sys.argv[1], sys.argv[2]
    report = open(sys.argv[sys.argv.index("--report") + 1], "w") if "--report" in sys.argv else None

    def rep(*a):
        if report:
            print(*a, file=report)

    with open(path, "rb") as f:
        raw = f.read()
    data, info = parse(raw)
    if len(data) != len(raw):
        raise SystemExit("fat binaries are not supported by the host")
    segs = info["segments"]
    sects = {(seg["name"], s["name"]): s for seg in segs for s in seg["sections"]}
    textseg = next(seg for seg in segs if seg["name"] == "__TEXT")
    text = sects[("__TEXT", "__text")]
    coal = sects.get(("__TEXT", "__const_coal"))

    def u32(a):
        for seg in segs:
            if seg["vmaddr"] <= a < seg["vmaddr"] + seg["filesize"]:
                return struct.unpack_from("<I", data, seg["fileoff"] + a - seg["vmaddr"])[0]
        return None

    def bytes_at(a, n):
        o = text["offset"] + a - text["addr"]
        return data[o:o + n]

    syms = symbols(data, info)
    text_syms = sorted({s["value"] for s in syms if s["value"] and (s["type"] & 0x0E) == 0x0E
                        and text["addr"] <= s["value"] < text["addr"] + text["size"]})

    # ---- decode all code once ----------------------------------------------------------
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    md.skipdata = True
    insns, undecodable = [], []
    for s in (text, coal):
        if not s:
            continue
        for ins in md.disasm(data[s["offset"]:s["offset"] + s["size"]], s["addr"]):
            if ins.id == 0:
                undecodable.append(ins.address)
            else:
                insns.append(ins)
    starts = {i.address for i in insns}
    # function starts: named symbols (local ones are stripped), framed prologues after a
    # terminator, and the targets of direct calls
    funcs = set(text_syms)
    prev = None
    for ins in insns:
        if ins.bytes[:3] == b"\x55\x89\xe5" and prev in ("ret", "jmp", "nop", "int3", "hlt", "ud2"):
            funcs.add(ins.address)
        if ins.id == x86.X86_INS_CALL and ins.bytes[0] == 0xE8:
            t = ins.operands[0].imm & 0xFFFFFFFF
            if t != ins.address + 5 and t in starts:
                funcs.add(t)
        prev = ins.mnemonic

    # ---- the range: up to the first named function at or above the PE header end ------
    i = bisect.bisect_left(text_syms, PE_HEADER_END)
    low_end = text_syms[i]
    # Local symbols are stripped in some builds, so the first named function can lie past what the low
    # copy holds (GMV_LOWCOPY_CAP, layout.h).  Then a direct-call target that follows a terminator or
    # padding will do: nothing falls through into it, so no function is split.
    if low_end - LOW_START > LOWCOPY_CAP:
        by_addr = {ins.address: k for k, ins in enumerate(insns)}
        called = set()
        for ins in insns:
            if ins.id == x86.X86_INS_CALL and ins.bytes[0] == 0xE8:
                called.add(ins.operands[0].imm & 0xFFFFFFFF)
        for f in sorted(funcs):
            if f < PE_HEADER_END or f >= low_end:
                continue
            k = by_addr.get(f)
            if k is None or k == 0 or f not in called:
                continue
            if insns[k - 1].mnemonic in ("ret", "jmp", "nop", "int3", "hlt", "ud2") and \
                    insns[k - 1].address + insns[k - 1].size == f:
                low_end = f
                break
    if low_end not in starts:
        raise SystemExit("low_end 0x%x is not an instruction start" % low_end)
    crossing = [ins for ins in insns if ins.address < low_end < ins.address + ins.size]
    if crossing:
        raise SystemExit("an instruction crosses low_end 0x%x" % low_end)
    rep("low range 0x%x..0x%x (%d bytes of code from 0x%x), copy size 0x%x" % (
        LOW_START, low_end, low_end - text["addr"], text["addr"], low_end - LOW_START))

    def inside(a):
        return LOW_START <= a < low_end

    patches = {}
    problems = []

    def add(addr, kind, reg=0):
        if addr in patches and patches[addr] != (kind, reg):
            problems.append("conflicting patches at 0x%x" % addr)
        patches[addr] = (kind, reg)

    # ---- code inside the range ---------------------------------------------------------
    low = [ins for ins in insns if text["addr"] <= ins.address < low_end]
    low_undec = [a for a in undecodable if a < low_end]
    for a in low_undec:
        rep("  undecodable byte in range at 0x%x: %s" % (a, bytes_at(a, 8).hex()))
    stats = collections.Counter()
    for k, ins in enumerate(low):
        is_branch = any(ins.group(g) for g in BRANCH_GROUPS)
        if is_branch and len(ins.operands) == 1 and ins.operands[0].type == x86.X86_OP_IMM:
            tgt = ins.operands[0].imm & 0xFFFFFFFF
            if ins.id == x86.X86_INS_CALL and tgt == ins.address + 5:
                nxt = low[k + 1] if k + 1 < len(low) else None
                if nxt and nxt.id == x86.X86_INS_POP and nxt.address == tgt and \
                        nxt.operands[0].type == x86.X86_OP_REG and nxt.size == 1:
                    add(ins.address, PIC_POP, REGNUM[nxt.operands[0].reg])
                    stats["pic call/pop"] += 1
                else:
                    problems.append("call $+5 at 0x%x not followed by a 1-byte pop" % ins.address)
                continue
            if inside(tgt):
                stats["branch within range"] += 1
                continue
            if ins.imm_size != 4:
                problems.append("short branch out of range at 0x%x -> 0x%x" % (ins.address, tgt))
                continue
            add(ins.address + ins.imm_offset, REL32_OUT)
            stats["rel32 out (%s)" % ("call" if ins.id == x86.X86_INS_CALL else "jmp/jcc")] += 1
        elif is_branch and ins.id in (x86.X86_INS_JMP, x86.X86_INS_CALL):
            stats["indirect " + ins.mnemonic] += 1
    # get_pc_thunk style helpers (mov r, [esp]; ret) called from inside the range would hand
    # back a copy address; the game uses call/pop, so just prove there are none
    for ins in low:
        if ins.id == x86.X86_INS_CALL and ins.operands[0].type == x86.X86_OP_IMM:
            t = ins.operands[0].imm & 0xFFFFFFFF
            b = bytes_at(t, 4) if text["addr"] <= t < text["addr"] + text["size"] else b""
            if len(b) == 4 and b[0] == 0x8B and (b[1] & 0xC7) == 0x04 and b[2] == 0x24 and b[3] == 0xC3:
                problems.append("get_pc_thunk call at 0x%x" % ins.address)
    # absolute references from range code to the range itself stay as they are (the copy
    # keeps original addresses for every pointer it hands out); just count them
    for ins in low:
        for op in ins.operands:
            v = op.imm if op.type == x86.X86_OP_IMM else op.mem.disp if op.type == x86.X86_OP_MEM and op.mem.base == 0 else None
            if v is not None and not any(ins.group(g) for g in BRANCH_GROUPS) and inside(v & 0xFFFFFFFF) \
                    and (v & 0xFFFFFFFF) >= text["addr"]:
                stats["abs ref to range (left as is)"] += 1
    rep("range code: %d instructions, %d undecodable bytes, %s" % (len(low), len(low_undec), dict(stats)))

    # ---- direct calls and jumps into the range from everywhere else -------------------
    into = collections.Counter()
    for ins in insns:
        if ins.address < low_end or ins.id not in (x86.X86_INS_CALL, x86.X86_INS_JMP):
            continue
        if ins.imm_size != 4 or ins.bytes[0] not in (0xE8, 0xE9) or ins.size != 5:
            continue
        tgt = ins.operands[0].imm & 0xFFFFFFFF
        if not inside(tgt):
            continue
        if tgt in funcs:
            add(ins.address + 1, REL32_IN)
            into[ins.mnemonic + " to function start"] += 1
        else:
            into[ins.mnemonic + " elsewhere (left to the fault handler)"] += 1
            rep("  direct %s at 0x%x -> 0x%x is not a known function start" % (ins.mnemonic, ins.address, tgt))
    rep("direct branches into range:", dict(into))

    # ---- exact pointer slots holding range code addresses -----------------------------
    dy = info["dyld_info"]
    bound = set()
    for off, size, lazy in ((dy["bind_off"], dy["bind_size"], False), (dy["lazy_off"], dy["lazy_size"], True)):
        bound |= {b[0] for b in run_binds(data[off:off + size], segs, lazy)}
    slots = collections.Counter()

    def ptr_slot(a, why):
        v = u32(a)
        if v is not None and inside(v) and v >= text["addr"] and a not in bound:
            if v not in starts:
                problems.append("%s slot 0x%x -> 0x%x is not an instruction start" % (why, a, v))
                return
            add(a, PTR_IN)
            slots[why] += 1

    for seg in segs:
        for s in seg["sections"]:
            if s["flags"] & 0xFF in (6, 7):
                for a in range(s["addr"], s["addr"] + s["size"], 4):
                    ptr_slot(a, s["name"])
    s = sects[("__DATA", "__mod_init_func")]
    for a in range(s["addr"], s["addr"] + s["size"], 4):
        ptr_slot(a, "__mod_init_func")
    # weak binds: exact slots resolved to the game's own definitions
    for slot, name, _o, addend, _w in run_binds(data[dy["weak_off"]:dy["weak_off"] + dy["weak_size"]], segs, False):
        ptr_slot(slot, "weak bind")
    # vtables: inside a __ZTV/__ZTC object, a word equal to a function start is a method
    data_syms = sorted((s["value"], s["name"]) for s in syms
                       if s["value"] and (s["type"] & 0x0E) == 0x0E and s["value"] >= sects[("__DATA", "__data")]["addr"])
    for j, (va, name) in enumerate(data_syms):
        if not name.startswith(("__ZTV", "__ZTC")):
            continue
        end = data_syms[j + 1][0] if j + 1 < len(data_syms) else va
        for a in range(va, min(end, va + 0x4000), 4):
            v = u32(a)
            if v is not None and inside(v) and v in funcs:
                ptr_slot(a, "vtable")
    # ObjC 1.0 method lists, reached through the module -> symtab -> class/category chain;
    # each list is {obsolete, count, {SEL, types, IMP}[count]}
    objc = [s for (sg, _n), s in sects.items() if sg == "__OBJC"]
    objc_lo = min(s["addr"] for s in objc)
    objc_hi = max(s["addr"] + s["size"] for s in objc)

    def method_list(ml):
        if not ml:
            return
        count = u32(ml + 4)
        if not (objc_lo <= ml and ml + 8 + 12 * count <= objc_hi):
            problems.append("objc method list 0x%x (count %s) outside __OBJC" % (ml, count))
            return
        for m in range(count):
            ptr_slot(ml + 8 + 12 * m + 8, "objc IMP")

    mod = sects[("__OBJC", "__module_info")]
    for m in range(mod["addr"], mod["addr"] + mod["size"], 16):
        symtab = u32(m + 12)
        if not symtab or u32(symtab + 8) is None:      # modules without definitions
            continue
        ncls, ncat = u32(symtab + 8) & 0xFFFF, u32(symtab + 8) >> 16
        for d in range(ncls + ncat):
            obj = u32(symtab + 12 + 4 * d)
            if d < ncls:
                method_list(u32(obj + 28))           # instance methods
                method_list(u32(u32(obj) + 28))      # metaclass: class methods
            else:
                method_list(u32(obj + 8))
                method_list(u32(obj + 12))
    rep("pointer slots into range:", dict(slots))

    # ---- anything referring to the Mach-O header / load commands ---------------------
    hdr_refs = 0
    for ins in insns:
        for op in ins.operands:
            v = op.imm if op.type == x86.X86_OP_IMM else op.mem.disp if op.type == x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 else None
            if v is not None and LOW_START <= (v & 0xFFFFFFFF) < text["addr"] and not any(ins.group(g) for g in BRANCH_GROUPS):
                hdr_refs += 1
                rep("  header ref 0x%x %s %s" % (ins.address, ins.mnemonic, ins.op_str))
    rep("code operands pointing at the Mach-O header area: %d (mostly small integers)" % hdr_refs)

    for p in problems:
        rep("PROBLEM " + p)
    if problems:
        print("\n".join(problems[:20]))
        raise SystemExit("%d problems, no table written" % len(problems))

    crc = zlib.crc32(data[textseg["fileoff"]:textseg["fileoff"] + textseg["filesize"]]) & 0xFFFFFFFF
    kinds = collections.Counter(k for k, _ in patches.values())
    with open(out, "wb") as f:
        f.write(b"GMVP" + struct.pack("<6I", 1, len(raw), crc, LOW_START, low_end, len(patches)))
        for a in sorted(patches):
            k, r = patches[a]
            f.write(struct.pack("<IBBH", a, k, r, 0))
    print("range 0x%x..0x%x; %d patches: REL32_OUT %d, PIC_POP %d, REL32_IN %d, PTR_IN %d -> %s" % (
        LOW_START, low_end, len(patches), kinds[REL32_OUT], kinds[PIC_POP], kinds[REL32_IN], kinds[PTR_IN], out))
    print("range code:", dict(stats))
    print("into range:", dict(into), " pointer slots:", dict(slots))
    print("undecodable bytes in range: %d, header-area operands: %d" % (len(low_undec), hdr_refs))


if __name__ == "__main__":
    main()
