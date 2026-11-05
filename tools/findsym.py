"""Look up defined symbols in a Mach-O by regex, or the symbol nearest an address.

usage: python -I findsym.py <macho> <regex | 0xADDR>
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machoinfo import parse, symbols  # noqa: E402


def main():
    with open(sys.argv[1], "rb") as f:
        data, info = parse(f.read())
    syms = sorted((s["value"], s["name"]) for s in symbols(data, info)
                  if s["value"] and (s["type"] & 0x0E) == 0x0E)
    q = sys.argv[2]
    if q.startswith("0x"):
        a = int(q, 16)
        best = max((v, n) for v, n in syms if v <= a)
        print("0x%06x = %s+0x%x" % (a, best[1], a - best[0]))
        return
    rx = re.compile(q)
    for v, n in syms:
        if rx.search(n):
            print("0x%06x %s" % (v, n))


if __name__ == "__main__":
    main()
