#!/usr/bin/env python3
"""universe.py - the set of classes the compiler can see, and the resolution
of a simple class name to its fully qualified name.

The universe is built from the corpus: every class blob (its dotted name) and
every top-level type declared by a source (package + name).  It is cached in
analysis/decl/out/universe.json.

    import universe
    U = universe.load()
    U.packages['java.game']            -> set of simple names
    U.resolve('Vector3', package, imports) -> 'java.lang.Vector3' or None
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'lib')))
sys.path.insert(0, HERE)

import oracle     # noqa: E402

OUT = os.path.join(HERE, 'out')
CACHE = os.path.join(OUT, 'universe.json')


class Universe(object):
    def __init__(self, packages, origin):
        self.packages = {k: set(v) for k, v in packages.items()}
        self.origin = origin          # fq name -> list of 'class:path' / 'source:path'
        self.mode = 'pkg-first'

    def has(self, fq):
        pkg, _, name = fq.rpartition('.')
        return name in self.packages.get(pkg, ())

    def candidates(self, simple, package, imports, local=()):
        """All fully qualified names a simple name could mean, in the order
        of the resolution rule: classes of the same source, the own package,
        the on-demand imports in source order, java.lang."""
        out = []
        if simple in local:
            out.append((package + '.' if package else '') + simple)
        scopes = [package] + [i for i in imports] + ['java.lang']
        for pkg in scopes:
            if pkg is None:
                continue
            if simple in self.packages.get(pkg, ()):
                fq = pkg + '.' + simple
                if fq not in out:
                    out.append(fq)
        return out

    def resolve(self, simple, package, imports, local=()):
        if '.' in simple:
            return simple
        c = self.candidates(simple, package, imports, local)
        return c[0] if c else None


def build():
    packages = {}
    origin = {}
    for rel, blobs in oracle.iter_class_files():
        for b in blobs:
            pkg, _, name = b.name.rpartition('.')
            packages.setdefault(pkg, set()).add(name)
            origin.setdefault(b.name, []).append('class:' + rel)
    for e in oracle.iter_entries():
        if e.source is None or not e.declared:
            continue
        pkg = e.package or ''
        for d in e.declared:
            packages.setdefault(pkg, set()).add(d['name'])
            origin.setdefault((pkg + '.' if pkg else '') + d['name'], []).append(
                'source:' + e.source)
    return Universe(packages, origin)


def load(rebuild=False):
    if not rebuild and os.path.exists(CACHE):
        with open(CACHE, 'r', encoding='utf-8') as f:
            d = json.load(f)
        return Universe(d['packages'], d['origin'])
    u = build()
    os.makedirs(OUT, exist_ok=True)
    with open(CACHE, 'w', encoding='utf-8') as f:
        json.dump({'packages': {k: sorted(v) for k, v in sorted(u.packages.items())},
                   'origin': u.origin}, f, indent=1)
    return u


if __name__ == '__main__':
    u = load(rebuild=True)
    print('%d packages, %d classes' % (len(u.packages),
                                       sum(len(v) for v in u.packages.values())))
    for k in sorted(u.packages):
        print('  %-40s %d' % (k, len(u.packages[k])))
    dup = {}
    for pkg, names in u.packages.items():
        for n in names:
            dup.setdefault(n, []).append(pkg)
    amb = {n: p for n, p in dup.items() if len(p) > 1}
    print('%d simple names exist in more than one package' % len(amb))
    for n in sorted(amb)[:60]:
        print('  ', n, sorted(amb[n]))
