"""Disassemble N instructions of a Mach-O at a virtual address.

usage: python -I disasm.py <macho> 0xADDR [count]
"""
import os
import sys

import capstone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machoinfo import parse  # noqa: E402


def main():
    with open(sys.argv[1], "rb") as f:
        data, info = parse(f.read())
    addr = int(sys.argv[2], 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    seg = next(s for s in info["segments"] if s["vmaddr"] <= addr < s["vmaddr"] + s["filesize"])
    off = seg["fileoff"] + addr - seg["vmaddr"]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for ins in md.disasm(data[off:off + count * 15], addr, count):
        print("0x%06x  %-24s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))


if __name__ == "__main__":
    main()
