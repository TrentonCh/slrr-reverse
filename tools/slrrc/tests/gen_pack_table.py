#!/usr/bin/env python3
"""gen_pack_table.py - print the pack name table of tests/pack_oracle.py.

    python -I tests/gen_pack_table.py

The pool of a class spells the pack of a resource id in one way per pack over
all 2,993 shipped classes (notes/exe_reads.md 2: the spelling of the pack's
first registration in the session).  This script reads every pool of the
corpus and prints those spellings as the lines of TABLE_940, sorted without
regard to letter case.  The table is a test oracle, not an input of the
compiler: tests/t4_trees.py checks that it still equals what the pools hold,
and that the replayed pack table of the package ends up with every one of its
spellings.

It also says how the table relates to the pack files of the install.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness  # noqa: E402
import oracle   # noqa: E402  lib


def main():
    spell = {}
    for e in oracle.iter_entries():
        if e.cls is None:
            continue
        for blob in oracle.load_class(e):
            for ent in blob.cons:
                if ent[0] == 'rid':
                    p = blob.utf8(ent[1])
                    spell.setdefault(p.lower(), set()).add(p)
    several = sorted(k for k, v in spell.items() if len(v) > 1)
    table = sorted((sorted(v)[0] for v in spell.values()), key=lambda s: s.lower())
    print('TABLE_940 = (')
    for p in table:
        if p.endswith('\\') or "'" in p:
            raise SystemExit('cannot write %r as a raw string' % p)
        print("    r'%s'," % p)
    print(')')
    files = dict((f.lower(), f) for f in harness.pack_files())
    sys.stderr.write('%d packs in the pools; packs with more than one spelling: %d\n' % (
        len(table), len(several)))
    sys.stderr.write('pack files in the corpus: %d\n' % len(files))
    sys.stderr.write('in the table, without a file: %s\n' % (
        [p for p in table if p.lower() not in files] or 'none'))
    sys.stderr.write('in the table, spelled otherwise than the file: %s\n' % (
        [(p, files[p.lower()]) for p in table
         if p.lower() in files and files[p.lower()] != p] or 'none'))
    sys.stderr.write('files no pool names: %d\n' % len(
        [f for f in files if f not in spell]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
