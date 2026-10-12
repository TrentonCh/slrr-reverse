#!/usr/bin/env python3
"""study_class_dirs.py - where do the classes a source names live?

A compiler has to find the class behind every class name of a source (to
write its full name into the pool and to look up its members).  The exe finds
classes through nine classpath directories (docs/ENGINE.md 4.2):

    java.lang java.net java.io java.util java.render java.sound
                                  -> system\\scripts\\{lang,net,io,util,render,sound}
    java.game                     -> sl\\scripts\\game
    java.game.parts               -> parts\\scripts
    java.game.parts.accessories   -> parts\\accessories\\scripts

with sub-packages in sub-directories.  This script measures how far that
gets: for every source and every distinct class name it uses (type names, and
identifiers that resolve to a class through the package, the imports and
java.lang), is the class in the classpath directory of its package, in the
directory of the source, or somewhere else?

    python -I study_class_dirs.py [--json FILE]
"""
import json
import os
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import oracle   # noqa: E402
import parser   # noqa: E402
import symbols  # noqa: E402

ROOTS = [
    ('java.lang', 'system/scripts/lang'),
    ('java.net', 'system/scripts/net'),
    ('java.io', 'system/scripts/io'),
    ('java.util', 'system/scripts/util'),
    ('java.render', 'system/scripts/render'),
    ('java.sound', 'system/scripts/sound'),
    ('java.game.parts.accessories', 'parts/accessories/scripts'),
    ('java.game.parts', 'parts/scripts'),
    ('java.game', 'sl/scripts/game'),
]

KINDS = ['same file', 'classpath directory of its package',
         'directory of the source (not on the classpath)',
         'another directory (not on the classpath)']


def classpath_dir(pkg):
    best = None
    for pre, d in ROOTS:
        if pkg == pre or pkg.startswith(pre + '.'):
            if best is None or len(pre) > len(best[0]):
                best = (pre, d)
    if best is None:
        return None
    rest = pkg[len(best[0]):].strip('.')
    return (best[1] + ('/' + rest.replace('.', '/') if rest else '')).lower()


def run(root=None):
    dirs = defaultdict(set)              # class -> directories holding it
    pkgdirs = defaultdict(set)           # package -> directories
    for e in oracle.iter_entries():
        if e.blobs:
            names = [b['name'] for b in e.blobs]
        else:
            names = [e.package + '.' + d['name'] for d in (e.declared or [])]
        for n in names:
            dirs[n].add(e.dir.lower())
            pkgdirs[n.rpartition('.')[0]].add(e.dir.lower())
    uni = symbols.Universe.from_index()
    c = Counter()
    per_cat = defaultdict(Counter)
    examples = defaultdict(list)
    targets = Counter()
    for e in oracle.iter_entries():
        if e.source is None:
            continue
        u = parser.parse(oracle.load_source(e, root), e.source)
        res = symbols.Resolver(u, uni)
        names = set()
        for cls in u.types:
            if cls.extends:
                names.add(cls.extends)
            names.update(cls.implements)
        for n in u.walk():
            if n.kind == 'Type' and not n.primitive:
                names.add(n.name)
            elif n.kind == 'Name':
                names.add(n.name)
        here = e.dir.lower()
        own = set(t.name for t in u.types)
        for nm in sorted(names):
            full = res.resolve(nm)
            if full is None:
                continue
            ds = dirs[full]
            cp = classpath_dir(full.rpartition('.')[0])
            if nm in own:
                k = KINDS[0]
            elif cp in ds:
                k = KINDS[1]
            elif here in ds:
                k = KINDS[2]
            else:
                k = KINDS[3]
                targets[full] += 1
                if len(examples[k]) < 6 and not any(x.startswith(e.source) for x in examples[k]):
                    examples[k].append('%s names %s, which lives in %s' % (
                        e.source, full, sorted(ds)[0]))
            c['references'] += 1
            c[k] += 1
            per_cat[e.category][k] += 1
    on_cp = sorted(p for p, ds in pkgdirs.items() if ds == {classpath_dir(p)})
    off_cp = sorted(p for p in pkgdirs if p not in on_cp)
    return {'counts': c, 'per_cat': per_cat, 'examples': examples, 'targets': targets,
            'dirs': dirs,
            'packages_on_classpath': on_cp, 'packages_off_classpath': off_cp,
            'pkgdirs': pkgdirs}


def main(argv):
    jpath = None
    if len(argv) == 3 and argv[1] == '--json':
        jpath = argv[2]
    elif len(argv) != 1:
        print(__doc__)
        return 2
    r = run()
    c = r['counts']
    print('study_class_dirs: where the classes named by a source live')
    print()
    print('distinct class names per source, all 3,454 sources: %d' % c['references'])
    for k in KINDS:
        print('  %6d  %-50s %s' % (c[k], k, '  '.join(
            '%s %d' % (cat, r['per_cat'][cat][k]) for cat in sorted(r['per_cat']))))
    print()
    print('packages whose classes all sit in the classpath directory of the package: %d of %d'
          % (len(r['packages_on_classpath']), len(r['pkgdirs'])))
    print('packages elsewhere (%d):' % len(r['packages_off_classpath']))
    for p in r['packages_off_classpath']:
        ds = sorted(r['pkgdirs'][p])
        print('  %-60s %d director%s, e.g. %s' % (p, len(ds), 'y' if len(ds) == 1 else 'ies', ds[0]))
    print()
    print('classes named from another directory that is not on the classpath (%d distinct):'
          % len(r['targets']))
    for full, n in r['targets'].most_common(12):
        print('  %5d sources name %s' % (n, full))
    several = sorted((k, len(v)) for k, v in r['dirs'].items() if len(v) > 1)
    print()
    print('classes declared under one full name in several directories (%d):' % len(several))
    for k, n in several:
        print('  %5d directories declare %s' % (n, k))
    for x in r['examples'][KINDS[3]]:
        print('  e.g. %s' % x)
    if jpath:
        sizes = Counter()
        for name in r['dirs']:
            sizes[name.rpartition('.')[0]] += 1
        doc = {'counts': dict(c), 'per_category': {k: dict(v) for k, v in r['per_cat'].items()},
               'package_sizes': dict(sizes),
               'classes_in_several_directories': dict(
                   (k, len(v)) for k, v in r['dirs'].items() if len(v) > 1),
               'examples': dict(r['examples']), 'targets': dict(r['targets']),
               'packages_on_classpath': r['packages_on_classpath'],
               'packages_off_classpath': r['packages_off_classpath'],
               'package_count': len(r['pkgdirs'])}
        with open(jpath, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(doc, f, indent=1, sort_keys=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
