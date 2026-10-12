#!/usr/bin/env python3
"""spike_parsers.py - the composition spike gives the same bytes whether the
models read the tree of lib/parser.py (through their adapters) or the trees of
the three analysis parsers they were developed on.

    python -I spike_parsers.py

Compiles every source of the corpus both ways (float setting A, pack name
table) and compares the class file bytes.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import spike_compiler as sc   # noqa: E402  (sets up sys.path)
import oracle                 # noqa: E402


def main(argv):
    env = sc.Env()
    n = same = 0
    bad = []
    for e in oracle.iter_entries():
        if not e.source:
            continue
        text = oracle.load_source(e)
        a, _ = sc.compile_source(text, env, 'A', name=e.source, parsers='lib')
        b, _ = sc.compile_source(text, env, 'A', name=e.source, parsers='analysis')
        n += 1
        same += a == b
        if a != b and len(bad) < 10:
            bad.append(e.source)
    print('sources %d, identical output under both parser sets %d' % (n, same))
    for x in bad:
        print('  differs: %s' % x)
    return 0 if n == same else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
