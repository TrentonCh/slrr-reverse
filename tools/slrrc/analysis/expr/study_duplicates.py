#!/usr/bin/env python3
"""study_duplicates.py - nine dialog classes exist twice: as a blob of
system/scripts/render/osd/dialog/Dialog.class and as a class file of their own
in the same directory.  The two versions differ (other fields and methods).
Which one did the compiler see when it compiled the classes that use them?

Runs check_expr over all consistent pairs twice, once with the class database
preferring the stand-alone file (the default of classdb.py) and once
preferring the blob inside Dialog.class, and prints the member differences.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for p in (_LIB, _HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import oracle       # noqa: E402
import classdb      # noqa: E402
import check_expr   # noqa: E402

DIALOG = 'system/scripts/render/osd/dialog/Dialog.class'


def main(argv):
    db = classdb.load()
    inside = {}
    for b in oracle.load_class(DIALOG):
        inside[b.name] = classdb.info_from_blob(b)
    dup = [n for n in inside if db.classes[n].file != DIALOG]
    print('classes of Dialog.class that also have a file of their own: %d' % len(dup))
    for n in sorted(dup):
        a, b = db.classes[n], inside[n]
        fa, fb = set(a.fields), set(b.fields)
        ma = set((m.name, m.desc) for m in a.methods)
        mb = set((m.name, m.desc) for m in b.methods)
        print('  %-48s own file: %d fields %d methods; in Dialog.class: %d fields %d methods; '
              'fields only in one: %s; methods only in one: %s' % (
                  n, len(fa), len(ma), len(fb), len(mb), sorted(fa ^ fb),
                  sorted(x[0] + x[1] for x in ma ^ mb)))
    entries = check_expr.entries_of('940') + check_expr.entries_of('other')
    r1 = check_expr.run(entries, {}, quiet=True)
    saved = {}
    for n in dup:
        saved[n] = db.classes[n]
        ci = inside[n]
        ci.file = DIALOG
        db.classes[n] = ci
    r2 = check_expr.run(entries, {}, quiet=True)
    for n in dup:
        db.classes[n] = saved[n]
    print('database prefers the stand-alone file : %d of %d trees identical' % (
        r1['counts']['trees identical'], r1['counts']['trees']))
    print('database prefers the blob in Dialog.class: %d of %d trees identical' % (
        r2['counts']['trees identical'], r2['counts']['trees']))
    for m in r2['mismatches']:
        for ex in m['examples'][:4]:
            print('    %s tree %d %s L%d: %s | %s' % (ex[0], ex[2], ex[3], ex[4], ex[5][:80], ex[6][:80]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
