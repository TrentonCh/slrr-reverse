#!/usr/bin/env python3
"""probe_names.py - class names: uniqueness, packages, imports, resolution.

Questions answered (printed, and written to --json FILE):
  - is a fully qualified class name unique in the corpus?  (12 are not)
  - which packages exist and in how many directories
  - which import forms the sources use
  - for every (source file, simple class name) the model resolves: is the
    resolution ambiguous (more than one of: own package, an imported package,
    java.lang holds a class of that name), and how many resolve only through
    java.lang without an import of it

    python -I probe_names.py [--json FILE]
"""
import json
import os
import sys
from collections import defaultdict, Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for p in (_LIB, _HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import oracle    # noqa: E402
import classdb   # noqa: E402
import jparse    # noqa: E402


def main(argv):
    by_name = defaultdict(list)
    for e in oracle.iter_entries():
        if e.blobs:
            for b in e.blobs:
                by_name[b['name']].append((e.dir, e.name, 'class'))
        elif e.declared:
            for nm in e.declared_names:
                by_name[nm].append((e.dir, e.name, 'src'))
    dups = {k: v for k, v in by_name.items() if len(v) > 1}
    print('distinct class names %d, occurring more than once %d' % (len(by_name), len(dups)))
    for k, v in sorted(dups.items()):
        print('  %-50s %d times, e.g. %s and %s' % (k, len(v), '/'.join(v[0][:2]),
                                                  '/'.join(v[1][:2])))
    pd = defaultdict(set)
    for e in oracle.iter_entries():
        if e.package:
            pd[e.package].add(e.dir)
    print('packages named by sources: %d' % len(pd))

    db = classdb.load()
    imports = Counter()
    uses = ambiguous = via_lang = 0
    amb_ex = []
    lang_names = Counter()
    for e in oracle.iter_entries():
        if not e.source:
            continue
        u = jparse.parse(oracle.load_source(e))
        for name, star in u.imports:
            imports['%s%s' % (name, '.*' if star else '')] += 1
        r = classdb.Resolver(db, u.package, u.imports)
        names = set()
        for nd in jparse.walk(u):
            if nd.k == 'type' and nd.name not in classdb.PRIMS and '.' not in nd.name:
                names.add(nd.name)
            elif nd.k == 'name':
                names.add(nd.id)
        for nm in names:
            c = r.candidates(nm)
            if not c:
                continue
            uses += 1
            if len(c) > 1:
                ambiguous += 1
                if len(amb_ex) < 10:
                    amb_ex.append('%s: %s -> %s' % (e.source, nm, c))
            elif c[0].startswith('java.lang.') and 'java.lang' not in r.star and \
                    u.package != 'java.lang' and nm not in r.single:
                via_lang += 1
                lang_names[nm] += 1
    star = sum(1 for k in imports if k.endswith('.*'))
    odd = [k for k in imports if ':' in k]
    print('distinct imports: %d (%d of the form a.b.*; with a colon: %s)' % (
        len(imports), star, odd))
    print('(source, simple class name) resolutions: %d; ambiguous: %d; found only through '
          'java.lang without an import: %d' % (uses, ambiguous, via_lang))
    for x in amb_ex:
        print('   ' + x)
    print('   java.lang names used without import:', lang_names.most_common(12))
    if '--json' in argv:
        with open(argv[argv.index('--json') + 1], 'w', encoding='utf-8') as f:
            json.dump({'class_names': len(by_name), 'duplicate_names': len(dups),
                       'packages': len(pd), 'imports': dict(imports), 'star_imports': star,
                       'colon_imports': odd, 'resolutions': uses, 'ambiguous': ambiguous,
                       'via_java_lang': via_lang, 'java_lang_names': dict(lang_names)}, f,
                      indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
