"""Build the link table the Win32 host needs to run the game image at a different base.

The game is a non-PIE i386 Mach-O linked at 0x1000. Windows already owns that range (WOW64
stacks/heaps sit in 0x10000..0x600000 before the process even runs), so the host maps the
image elsewhere and slides every absolute pointer. The binary has no rebase info, so this
tool reconstructs the set of absolute slots from structure + validated heuristics, and
flattens the dyld bind / lazy-bind opcode streams into a plain list.

usage: python -I genlink.py <macho> <out.bin> [--report report.txt]

out.bin layout (little endian):
    char magic[4] = "GMVL"; u32 version = 2
    u32 n_reloc; u32 n_bind; u32 names_size
    u32 reloc[n_reloc]                 vmaddr of a 32-bit slot that gets += slide
    struct { u32 slot; i32 addend; u16 ordinal; u8 flags; u8 pad; u32 name; } bind[n_bind]
        flags: 1 = weak import, 2 = from lazy bind stream; ordinal 0xFFFF = flat lookup
    char names[names_size]             NUL-terminated symbol names
Self-ordinal binds and weak binds are resolved here: the slot is written with the symbol's
vmaddr into patches.  Those come as an extra trailer:
    u32 n_set; struct { u32 slot; u32 value; } set[n_set]   value is a vmaddr (slide added)
Version 1 tables ignored the weak-bind stream and left ~1500 symbol-pointer slots unfilled;
the host refuses them.
"""
import bisect
import collections
import os
import struct
import sys

import capstone
from capstone import x86

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machoinfo import parse, symbols, INDIRECT_SYMBOL_LOCAL, INDIRECT_SYMBOL_ABS  # noqa: E402


def uleb(buf, i):
    v = s = 0
    while True:
        b = buf[i]
        i += 1
        v |= (b & 0x7F) << s
        s += 7
        if b < 0x80:
            return v, i


def sleb(buf, i):
    v = s = 0
    while True:
        b = buf[i]
        i += 1
        v |= (b & 0x7F) << s
        s += 7
        if b < 0x80:
            if b & 0x40:
                v -= 1 << s
            return v, i


def run_binds(buf, segs, lazy):
    """Yield (slot_vmaddr, name, ordinal, addend, weak) for one dyld bind opcode stream."""
    i = 0
    ordinal, name, weak, addend, addr = 0, "", False, 0, 0
    while i < len(buf):
        b = buf[i]
        i += 1
        op, imm = b & 0xF0, b & 0x0F
        if op == 0x00:          # DONE
            if not lazy:
                return
        elif op == 0x10:        # SET_DYLIB_ORDINAL_IMM
            ordinal = imm
        elif op == 0x20:        # SET_DYLIB_ORDINAL_ULEB
            ordinal, i = uleb(buf, i)
        elif op == 0x30:        # SET_DYLIB_SPECIAL_IMM
            ordinal = (imm | 0xF0) - 0x100 if imm else 0
        elif op == 0x40:        # SET_SYMBOL_TRAILING_FLAGS_IMM
            end = buf.index(b"\0", i)
            name = buf[i:end].decode("latin-1")
            weak = bool(imm & 1)
            i = end + 1
        elif op == 0x50:        # SET_TYPE_IMM
            if imm != 1:
                raise SystemExit("bind type %d not supported" % imm)
        elif op == 0x60:        # SET_ADDEND_SLEB
            addend, i = sleb(buf, i)
        elif op == 0x70:        # SET_SEGMENT_AND_OFFSET_ULEB
            off, i = uleb(buf, i)
            addr = segs[imm]["vmaddr"] + off
        elif op == 0x80:        # ADD_ADDR_ULEB
            d, i = uleb(buf, i)
            addr = (addr + d) & 0xFFFFFFFF
        elif op == 0x90:        # DO_BIND
            yield addr, name, ordinal, addend, weak
            addr += 4
        elif op == 0xA0:        # DO_BIND_ADD_ADDR_ULEB
            yield addr, name, ordinal, addend, weak
            d, i = uleb(buf, i)
            addr = (addr + 4 + d) & 0xFFFFFFFF
        elif op == 0xB0:        # DO_BIND_ADD_ADDR_IMM_SCALED
            yield addr, name, ordinal, addend, weak
            addr += 4 + imm * 4
        elif op == 0xC0:        # DO_BIND_ULEB_TIMES_SKIPPING_ULEB
            n, i = uleb(buf, i)
            skip, i = uleb(buf, i)
            for _ in range(n):
                yield addr, name, ordinal, addend, weak
                addr += 4 + skip
        else:
            raise SystemExit("bad bind opcode 0x%02x" % b)


class Image:
    def __init__(self, data, info):
        self.data, self.info = data, info
        self.segs = info["segments"]
        self.sects = {(seg["name"], s["name"]): s for seg in self.segs for s in seg["sections"]}
        self.lo = self.segs[1]["vmaddr"]
        self.hi = max(s["vmaddr"] + s["vmsize"] for s in self.segs if s["name"] != "__LINKEDIT")

    def sect(self, seg, name):
        return self.sects.get((seg, name))

    def bytes_of(self, s):
        if s["flags"] & 0xFF == 1:      # zerofill
            return b""
        return self.data[s["offset"]:s["offset"] + s["size"]]

    def u32(self, addr):
        for seg in self.segs:
            if seg["vmaddr"] <= addr < seg["vmaddr"] + seg["filesize"]:
                return struct.unpack_from("<I", self.data, seg["fileoff"] + addr - seg["vmaddr"])[0]
        return 0

    def section_at(self, addr):
        for (segname, name), s in self.sects.items():
            if s["addr"] <= addr < s["addr"] + s["size"]:
                return segname, name
        return None


def main():
    path, out = sys.argv[1], sys.argv[2]
    report = open(sys.argv[sys.argv.index("--report") + 1], "w") if "--report" in sys.argv else None

    def rep(*a):
        if report:
            print(*a, file=report)

    with open(path, "rb") as f:
        data, info = parse(f.read())
    img = Image(data, info)
    syms = symbols(data, info)
    defined = {}
    sym_addrs = set()
    for s in syms:
        if s["value"] and (s["type"] & 0x0E) == 0x0E:
            sym_addrs.add(s["value"])
            if s["type"] & 0x01:
                defined.setdefault(s["name"], s["value"])

    text = img.sect("__TEXT", "__text")
    coal = img.sect("__TEXT", "__const_coal")
    stubs = img.sect("__TEXT", "__symbol_stub")
    cstr = img.sect("__TEXT", "__cstring")
    ustr = img.sect("__TEXT", "__ustring")

    # ---- binds -----------------------------------------------------------------------
    dy = info["dyld_info"]
    binds = []
    for off, size, lazy in ((dy["bind_off"], dy["bind_size"], False),
                            (dy["lazy_off"], dy["lazy_size"], True)):
        binds += [(a, n, o, ad, w, lazy) for a, n, o, ad, w in
                  run_binds(data[off:off + size], img.segs, lazy)]
    bound_slots = {b[0] for b in binds}
    sets = []
    ext_binds = []
    for slot, name, ordinal, addend, weak, lazy in binds:
        if ordinal == 0 or ordinal == -1:
            if name not in defined:
                raise SystemExit("self bind to undefined %s" % name)
            sets.append((slot, defined[name] + addend))
        else:
            ext_binds.append((slot, name, ordinal, addend, weak, lazy))
    rep("binds: %d external, %d self-resolved" % (len(ext_binds), len(sets)))

    # ---- weak binds ------------------------------------------------------------------
    # dyld coalesces weak definitions at load and the main executable is searched first, so
    # each slot in this stream resolves to the game's own copy.  They are exact pointer
    # locations: the symbol-pointer slots of weak-def functions are filled by nothing else,
    # and the data words here need no guessing.  Names the game does not define are weak
    # references into libstdc++ and are already in the regular bind stream.
    weak_where = collections.Counter()
    for slot, name, _ordinal, addend, _weak in run_binds(
            data[dy["weak_off"]:dy["weak_off"] + dy["weak_size"]], img.segs, False):
        if name in defined:
            sets.append((slot, defined[name] + addend))
            weak_where[(img.section_at(slot) or ("?", "?"))[1]] += 1
        elif slot not in bound_slots:
            raise SystemExit("weak bind of undefined %s at 0x%x is not bound" % (name, slot))
    rep("weak binds resolved to the game's own definitions:", dict(weak_where))
    if len({s for s, _ in sets}) != len(sets):
        raise SystemExit("a slot is resolved twice")

    # ---- what a valid pointer target looks like ------------------------------------
    code = img.bytes_of(text)
    funcs = set(a for a in sym_addrs if text["addr"] <= a < text["addr"] + text["size"])
    for i in range(0, len(code) - 3, 16):
        if code[i:i + 3] == b"\x55\x89\xe5":
            funcs.add(text["addr"] + i)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    md.skipdata = True
    insns = []
    called = set()
    boundaries = set()
    framed = set()      # prologue right after a terminator: as good as a symbol
    enders = {"ret", "jmp", "hlt", "nop", "int3", "ud2"}
    for blob_sect in (text, coal):
        blob = img.bytes_of(blob_sect)
        prev = "ret"
        for ins in md.disasm(blob, blob_sect["addr"]):
            if ins.id == 0:
                prev = "nop"
                continue
            # functions are not always 16-aligned nor framed (vbase thunks): an instruction
            # boundary after ret/padding, or a frame prologue anywhere, may start one.  Code
            # after a plain jmp is usually just the next basic block, so it does not count.
            if (prev in enders and prev != "jmp" and ins.mnemonic not in ("nop", "int3")) or \
                    ins.bytes[:3] == b"\x55\x89\xe5":
                funcs.add(ins.address)
                if prev in enders and ins.bytes[:3] == b"\x55\x89\xe5":
                    framed.add(ins.address)
            if ins.id == x86.X86_INS_CALL and ins.bytes[0] == 0xE8:
                t = ins.operands[0].imm
                if t != ins.address + 5:
                    called.add(t)
            prev = ins.mnemonic
            boundaries.add(ins.address)
            insns.append(ins)
    funcs |= called
    rep("function starts: %d (%d call targets), instructions: %d" % (len(funcs), len(called), len(insns)))

    cbytes = img.bytes_of(cstr)
    str_starts = {cstr["addr"]} | {cstr["addr"] + i + 1 for i, b in enumerate(cbytes[:-1]) if b == 0}

    def text_like(v):
        return text["addr"] <= v < text["addr"] + text["size"] or \
            (coal and coal["addr"] <= v < coal["addr"] + coal["size"])

    mask_ops = {"and", "or", "xor", "test", "shl", "shr", "sar", "rol", "ror", "imul", "bt"}

    # vtable extents: inside a __ZTV object every text-pointing word is a method pointer
    data_syms = sorted((s["value"], s["name"]) for s in syms
                       if s["value"] and (s["type"] & 0x0E) == 0x0E and s["value"] >= img.sect("__DATA", "__data")["addr"])
    dsym_addr = [v for v, _ in data_syms]

    def in_vtable(a):
        i = bisect.bisect_right(dsym_addr, a) - 1
        return i >= 0 and data_syms[i][1].startswith(("__ZTV", "__ZTC"))

    def valid_target(v, ctx, mnem="", slot=0):
        """ctx: 'data' (aligned word in a data section), 'abs' (code memory operand with no
        base register -- it is dereferenced, so it is an address), 'imm' (code immediate)."""
        if ctx == "data" and text_like(v) and in_vtable(slot):
            return (v in boundaries), "vtbl"
        if v < text["addr"] or v >= img.hi:          # mach header / load commands / linkedit
            return False, "range"
        sec = img.section_at(v)
        if sec is None:
            return False, "gap"
        if text_like(v):
            if ctx == "imm" and mnem not in ("mov", "push"):
                return False, "func-op"
            # round numbers (0x30000, 0xea60...) are far more often counts than code
            # addresses; a missed function pointer is caught at run time by the host's
            # fault trap, a wrongly slid integer is silent corruption
            if v & 0xFF == 0 and v not in sym_addrs and v not in called and v not in framed:
                return False, "round"
            return (v in funcs), "func"
        if sec[1] == "__symbol_stub":
            return (v - stubs["addr"]) % 6 == 0, "stub"
        if sec[1] in ("__cstring", "__ustring"):     # tail-merged strings: any byte may be a start
            return True, "str"
        if sec[1] in ("__stub_helper", "__unwind_info"):
            return False, sec[1]
        if ctx == "abs" or v in sym_addrs:
            return True, "sym"
        if bin(v).count("1") <= 1:
            return False, "pow2"
        if ctx == "imm":
            return mnem not in mask_ops, "mask"
        return v % 4 == 0, "data"

    relocs = set()
    why = collections.Counter()

    # ---- exact structural sources ----------------------------------------------------
    for j in range(stubs["size"] // 6):
        a = stubs["addr"] + 6 * j
        relocs.add(a + 2)
    why["stubs"] = stubs["size"] // 6

    dyst = info["dysymtab"]
    ind = struct.unpack_from("<%dI" % dyst["nindirectsyms"], data, dyst["indirectsymoff"])
    for seg in img.segs:
        for s in seg["sections"]:
            if s["flags"] & 0xFF in (6, 7):
                for j in range(s["size"] // 4):
                    a = s["addr"] + 4 * j
                    if ind[s["reserved1"] + j] & INDIRECT_SYMBOL_LOCAL and \
                            not ind[s["reserved1"] + j] & INDIRECT_SYMBOL_ABS and a not in bound_slots:
                        if img.lo <= img.u32(a) < img.hi:
                            relocs.add(a)
                            why["local sym_ptr"] += 1

    for name in ("__mod_init_func", "__program_vars"):
        s = img.sect("__DATA", name)
        for a in range(s["addr"], s["addr"] + s["size"], 4):
            if img.lo <= img.u32(a) < img.hi:
                relocs.add(a)
                why[name] += 1

    # ---- data words ------------------------------------------------------------------
    rejected = collections.Counter()
    scan = [s for seg in img.segs if seg["name"] in ("__DATA", "__OBJC") for s in seg["sections"]
            if s["flags"] & 0xFF not in (1, 6, 7) and s["name"] not in ("__mod_init_func", "__program_vars")]
    for s in scan:
        blob = img.bytes_of(s)
        n = 0
        for j in range(0, len(blob) - 3, 4):
            a = s["addr"] + j
            if a in bound_slots:
                continue
            v = struct.unpack_from("<I", blob, j)[0]
            if not (img.lo <= v < img.hi):
                continue
            ok, kind = valid_target(v, "data", slot=a)
            if ok:
                relocs.add(a)
                n += 1
            else:
                rejected[kind] += 1
                rep("  data reject %-18s 0x%06x = 0x%06x (%s)" % (s["name"], a, v, kind))
        why["data " + s["name"]] = n
    rep("data rejects by kind:", dict(rejected))

    # ---- switch jump tables -----------------------------------------------------------
    # -mdynamic-no-pic code dispatches switches through absolute tables in __TEXT,__const:
    #     cmp idx, N-1 ; ja default ; jmp [table + idx*4]     (or mov r, [table+idx*4]; jmp r)
    # The entries are basic-block addresses, not function starts, so the pointer heuristics
    # reject them.  The cmp guarding the dispatch gives the exact entry count.
    jt = {}
    jt_unbounded = []
    for k, ins in enumerate(insns):
        if ins.id == x86.X86_INS_JMP and len(ins.operands) == 1:
            mop = ins.operands[0]
        elif ins.id == x86.X86_INS_MOV and len(ins.operands) == 2 and ins.operands[0].type == x86.X86_OP_REG:
            dst = ins.operands[0].reg
            if not any(n.id == x86.X86_INS_JMP and n.operands and n.operands[0].type == x86.X86_OP_REG
                       and n.operands[0].reg == dst for n in insns[k + 1:k + 3]):
                continue
            mop = ins.operands[1]
        else:
            continue
        if mop.type != x86.X86_OP_MEM or mop.mem.base != 0 or mop.mem.index == 0 or \
                mop.mem.scale != 4 or mop.mem.segment != 0:
            continue
        table, idx, count = mop.mem.disp & 0xFFFFFFFF, mop.mem.index, None
        for back in range(k - 1, max(k - 8, 0), -1):
            p, nx = insns[back], insns[back + 1]
            if p.id == x86.X86_INS_CMP and len(p.operands) == 2 and p.operands[0].type == x86.X86_OP_REG \
                    and p.operands[0].reg == idx and p.operands[1].type == x86.X86_OP_IMM:
                if nx.id == x86.X86_INS_JA:
                    count = p.operands[1].imm + 1
                elif nx.id == x86.X86_INS_JAE:
                    count = p.operands[1].imm
                break
        if count is None:
            jt_unbounded.append(table)
        elif 0 < count <= 4096:
            jt[table] = max(jt.get(table, 0), count)
    bounded = set(jt)
    for table in jt_unbounded:
        if table in jt:
            continue
        # no guard found: take entries while they are instruction starts, up to the next table
        n = 0
        while n < 4096 and (table + 4 * n) not in jt and img.u32(table + 4 * n) in boundaries:
            n += 1
        if n:
            jt[table] = n
    jt_bad = jt_entries = 0
    for table, count in jt.items():
        for j in range(count):
            a = table + 4 * j
            v = img.u32(a)
            if text_like(v):
                relocs.add(a)
                jt_entries += 1
                if v not in boundaries:
                    jt_bad += 1
                    rep("  jump table 0x%06x[%d] = 0x%06x is not an instruction start" % (table, j, v))
            else:
                jt_bad += 1
                rep("  jump table 0x%06x[%d] = 0x%08x is not code" % (table, j, v))
    why["jump table entries"] = jt_entries
    rep("jump tables: %d (%d without a cmp bound), %d entries, %d suspicious" % (
        len(jt), len(set(jt) - bounded), jt_entries, jt_bad))
    jt_slots = {t + 4 * j for t, c in jt.items() for j in range(c)}

    # ---- __TEXT,__const --------------------------------------------------------------
    # A non-PIE image never slides, so const pointer tables can sit here too: the weak-bind
    # stream proves ~200 of them and the jump tables above are here.  The data heuristics
    # are NOT applied: this section is full of packed u16 tables (0x00710070, 0x007b0000)
    # that validate as pointers into __common, and PIC objects keep label-difference
    # tables here that must not slide.  Count what would validate, don't slide it.
    tc = img.sect("__TEXT", "__const")
    blob = img.bytes_of(tc)
    tc_kinds = collections.Counter()
    for j in range(0, len(blob) - 3, 4):
        a = tc["addr"] + j
        v = struct.unpack_from("<I", blob, j)[0]
        if a in jt_slots or not (img.lo <= v < img.hi):
            continue
        ok, kind = valid_target(v, "data", slot=a)
        tc_kinds[("would " if ok else "rej ") + kind] += 1
        rep("  tconst %s %-6s 0x%06x = 0x%06x" % ("would" if ok else "rej  ", kind, a, v))
    rep("__TEXT,__const in-image words (NOT slid):", dict(tc_kinds))

    # ---- absolute references in code --------------------------------------------------
    code_rej = collections.Counter()
    for ins in insns:
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == 0 and ins.disp_size == 4:
                v = op.mem.disp & 0xFFFFFFFF
                if img.lo <= v < img.hi:
                    ok, kind = valid_target(v, "abs")
                    if ok:
                        relocs.add(ins.address + ins.disp_offset)
                        why["code [abs]"] += 1
                    else:
                        code_rej["abs/" + kind] += 1
                    rep("  code abs %s %-8s 0x%06x %s %s" % ("OK " if ok else "rej", kind, ins.address, ins.mnemonic, ins.op_str))
            elif op.type == x86.X86_OP_IMM and ins.imm_size == 4 and \
                    ins.id not in (x86.X86_INS_CALL, x86.X86_INS_JMP) and not ins.mnemonic.startswith("j"):
                v = op.imm & 0xFFFFFFFF
                if img.lo <= v < img.hi:
                    ok, kind = valid_target(v, "imm", ins.mnemonic)
                    if ok:
                        relocs.add(ins.address + ins.imm_offset)
                        why["code imm"] += 1
                    else:
                        code_rej["imm/" + kind] += 1
                    rep("  code imm %s %-8s 0x%06x %s %s" % ("OK " if ok else "rej", kind, ins.address, ins.mnemonic, ins.op_str))
    rep("code rejects by kind:", dict(code_rej))

    relocs -= bound_slots
    relocs -= {s for s, _ in sets}
    for k, v in why.items():
        rep("reloc source %-28s %d" % (k, v))
    rep("total relocs: %d" % len(relocs))

    # ---- write -----------------------------------------------------------------------
    names = bytearray()
    name_off = {}
    for _, n, *_ in ext_binds:
        if n not in name_off:
            name_off[n] = len(names)
            names += n.encode("latin-1") + b"\0"
    with open(out, "wb") as f:
        f.write(b"GMVL" + struct.pack("<4I", 2, len(relocs), len(ext_binds), len(names)))
        f.write(struct.pack("<%dI" % len(relocs), *sorted(relocs)))
        for slot, n, ordinal, addend, weak, lazy in ext_binds:
            f.write(struct.pack("<IiHBBI", slot, addend, ordinal & 0xFFFF,
                                (1 if weak else 0) | (2 if lazy else 0), 0, name_off[n]))
        f.write(names)
        f.write(struct.pack("<I", len(sets)))
        for slot, v in sets:
            f.write(struct.pack("<II", slot, v))
    print("relocs %d, external binds %d (%d unique), self binds %d -> %s" % (
        len(relocs), len(ext_binds), len(name_off), len(sets), out))
    print("sources:", dict(why))
    print("data rejects:", dict(rejected), " code rejects:", dict(code_rej))
    print("jump tables: %d, %d entries, %d suspicious;  __TEXT,__const words: %s" % (
        len(jt), jt_entries, jt_bad, dict(tc_kinds)))


if __name__ == "__main__":
    main()
