"""Quick probes of the game's code model: PIC or absolute addressing, stub format.

usage: python -I codeprobe.py <macho>
"""
import collections
import os
import sys

import capstone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machoinfo import parse  # noqa: E402


def main():
    with open(sys.argv[1], "rb") as f:
        data, info = parse(f.read())
    text = next(s for seg in info["segments"] for s in seg["sections"] if s["name"] == "__text")
    stubs = next(s for seg in info["segments"] for s in seg["sections"] if s["name"] == "__symbol_stub")
    code = data[text["offset"]:text["offset"] + text["size"]]
    lo = info["segments"][1]["vmaddr"]
    hi = max(s["vmaddr"] + s["vmsize"] for s in info["segments"])

    print("stub size:", stubs["reserved2"])
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for ins in md.disasm(data[stubs["offset"]:stubs["offset"] + 24], stubs["addr"]):
        print("   0x%x %s %s" % (ins.address, ins.mnemonic, ins.op_str))

    pic_pops = code.count(b"\xe8\x00\x00\x00\x00")
    print("call $+5 sequences:", pic_pops)

    # Count 32-bit little-endian words in code that land inside the image (absolute refs).
    abs_refs = 0
    for i in range(0, len(code) - 3):
        v = int.from_bytes(code[i:i + 4], "little")
        if lo <= v < hi:
            abs_refs += 1
    print("raw in-image dwords in __text (upper bound on absolute refs): %d" % abs_refs)

    # Disassemble the first chunk linearly and tally operand styles.
    md.detail = True
    tally = collections.Counter()
    n = 0
    for ins in md.disasm(code[:0x40000], text["addr"]):
        n += 1
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_IMM and lo <= op.imm < hi and ins.mnemonic not in ("call", "jmp") and not ins.mnemonic.startswith("j"):
                tally["imm->image"] += 1
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and lo <= op.mem.disp < hi:
                tally["[abs]->image"] += 1
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base != 0 and lo <= op.mem.disp < hi:
                tally["[reg+abs]->image"] += 1
    print("first 256KB: %d insns, %s" % (n, dict(tally)))

    print("code below 0x10000: 0x%x bytes" % (0x10000 - text["addr"]))


if __name__ == "__main__":
    main()
