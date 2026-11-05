"""Group a Mach-O's imported symbols by the dylib they bind to.

usage: python -I imports_by_lib.py <analysis.json> [--names]
"""
import collections
import json
import sys


def main():
    with open(sys.argv[1]) as f:
        j = json.load(f)
    libs = [d for kind, d in j["info"]["dylibs"] if kind == "load"]
    groups = collections.defaultdict(set)
    for s in j["imports"]:
        o = s["lib_ordinal"]
        name = libs[o - 1].rsplit("/", 1)[-1] if 0 < o <= len(libs) else "ordinal%d" % o
        groups[name].add(s["symbol"])
    for name, syms in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print("%-28s %4d" % (name, len(syms)))
        if "--names" in sys.argv:
            for s in sorted(syms):
                print("    " + s)


if __name__ == "__main__":
    main()
