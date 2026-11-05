"""Inspect the code that sits below 0x10000 (Windows cannot map it) and everything that touches it.

usage: python -I lowcode.py <macho> [--dump]
"""
import collections
import os
import sys

import capstone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machoinfo import parse, symbols  # noqa: E402

LOW_END = 0x10000


def main():
    with open(sys.argv[1], "rb") as f:
        data, info = parse(f.read())
    text = next(s for seg in info["segments"] for s in seg["sections"] if s["name"] == "__text")
    base = text["addr"]
    code = data[text["offset"]:text["offset"] + text["size"]]

    syms = sorted((s["value"], s["name"]) for s in symbols(data, info)
                  if s["value"] and (s["type"] & 0x0E) == 0x0E and s["sect"] == 1)
    low_syms = [(v, n) for v, n in syms if v < LOW_END]
    print("named symbols in __text: %d, below 0x10000: %d" % (len(syms), len(low_syms)))
    for v, n in low_syms[:40]:
        print("   0x%06x %s" % (v, n))

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.skipdata = True
    stats = collections.Counter()
    ins_low = []
    for ins in md.disasm(code[:LOW_END - base], base):
        ins_low.append(ins)
        if ins.address + ins.size > LOW_END:
            break
    for i, ins in enumerate(ins_low):
        b = ins.bytes
        if ins.mnemonic == "call" and b[0] == 0xE8:
            tgt = int(ins.op_str, 16)
            if tgt == ins.address + 5:
                stats["call $+5"] += 1
            elif tgt >= LOW_END:
                stats["call out"] += 1
            else:
                stats["call in"] += 1
        elif (ins.mnemonic == "jmp" or ins.mnemonic.startswith("j")) and ins.op_str.startswith("0x"):
            tgt = int(ins.op_str, 16)
            stats["jcc/jmp out" if tgt >= LOW_END else "jcc/jmp in"] += 1
        elif ins.mnemonic == "jmp" and "[" not in ins.op_str and ins.op_str in ("eax", "ebx", "ecx", "edx", "esi", "edi"):
            stats["jmp reg (jump table?)"] += 1
            if "--dump" in sys.argv:
                for p in ins_low[max(0, i - 6):i + 1]:
                    print("      0x%06x %-6s %s" % (p.address, p.mnemonic, p.op_str))
                print()
        elif ins.mnemonic == ".byte":
            stats["undecodable"] += 1
    print("instructions below 0x10000: %d -> %s" % (len(ins_low), dict(stats)))

    # Who, in the rest of __text, branches into the low region?
    into = collections.Counter()
    for ins in md.disasm(code[LOW_END - base:], LOW_END):
        if ins.op_str.startswith("0x") and (ins.mnemonic == "call" or ins.mnemonic.startswith("j")):
            tgt = int(ins.op_str, 16)
            if base <= tgt < LOW_END:
                into[ins.mnemonic if ins.mnemonic in ("call", "jmp") else "jcc"] += 1
    print("direct branches from high code into low code:", dict(into))

    # Data words anywhere that look like pointers into low code.
    lo_ptr = 0
    for seg in info["segments"]:
        if seg["name"] in ("__DATA", "__OBJC"):
            blob = data[seg["fileoff"]:seg["fileoff"] + seg["filesize"]]
            for j in range(0, len(blob) - 3, 4):
                v = int.from_bytes(blob[j:j + 4], "little")
                if base <= v < LOW_END:
                    lo_ptr += 1
    print("aligned data words in [0x%x,0x10000): %d (some are plain integers)" % (base, lo_ptr))


if __name__ == "__main__":
    main()
