#!/usr/bin/env python3
"""check_source_only.py - what the model says about the 635 sources that ship
without a class file (the classes a compiler has to produce).

For them only steps 1 to 3 of the model can run (seed, types, names) and the
CLSS / FILD / MTHD chunks can be predicted in full.  The script checks

  S1  every class name used in a source resolves (own source, own package,
      on-demand imports, java.lang)
  S2  the rules the corpus leaves undetermined (see check_alternatives.py)
      never matter: every alternative gives the same pool prefix and rows
  S3  no source uses a declaration form that no pair exercises (abstract or
      interface methods, protected, synchronized, array brackets behind a
      name, qualified type names, several classes in one source)
  S4  no class name used in a source is found in more than one of the
      packages it can come from, so the search order is never decisive

    python -I check_source_only.py
"""
import os
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'lib')))
sys.path.insert(0, HERE)

import oracle      # noqa: E402
import srcparse    # noqa: E402
import universe    # noqa: E402
import poolmodel   # noqa: E402

OUT = os.path.join(HERE, 'out')

UNDETERMINED = [
    ('field type after its initialiser', {'field': 'post'}),
    ('local type after each declarator', {'local': 'each'}),
    ('type of new T[n] after the size', {'newarray': 'post'}),
    ('type of (int)x after x', {'cast': 'post'}),
    ('type of instanceof before x', {'instanceof': 'pre'}),
    ('own type before the interface types', {'class_end': 'own,ifaces'}),
]


def prefix(unit, cd, uni, opts=None):
    m = poolmodel.Model(unit, cd, None, uni, opts)
    m.run()
    rows = m.member_rows()
    return m, (tuple(m.pool.entries), repr(rows))


def main(argv):
    uni = universe.load()
    lines = []
    out = lines.append
    for label, cats in (('sources without a class file', ('source_only',)),
                        ('all sources', ('source_only', 'pair_940', 'pair_other'))):
        c = Counter()
        unresolved = Counter()
        differs = Counter()
        forms = Counter()
        scope = Counter()
        direction = Counter()
        for e in oracle.iter_entries(cats):
            unit = srcparse.parse(oracle.load_source(e))
            c['sources'] += 1
            if len(unit.classes) != 1:
                forms['sources with more than one class'] += 1
            for i in unit.imports:
                if i.get('odd'):
                    forms['imports of the form "parts:part.root.BodyPart.*"'] += 1
                if not i.star:
                    forms['single-type imports'] += 1
            for cd in unit.classes:
                c['classes'] += 1
                m, base = prefix(unit, cd, uni)
                c['pool entries of steps 1-3'] += len(m.pool.entries)
                sf, inf, sm, im, clss, ntrees = m.member_rows()
                c['field rows'] += len(sf) + len(inf)
                c['method rows'] += len(sm) + len(im)
                c['trees'] += ntrees
                if m.unresolved:
                    c['classes with an unresolved class name'] += 1
                    for u in set(m.unresolved):
                        unresolved[u] += 1
                names = [x.name for x in unit.classes]
                for name, sc, k in m.class_name_census():
                    scope[sc] += 1
                    c['class name uses'] += 1
                    if k > 1:
                        c['ambiguous uses'] += 1
                    if sc == 'same source':
                        j = names.index(name)
                        i = names.index(cd.name)
                        direction['the class itself' if j == i else
                                  'a class declared earlier in the source' if j < i
                                  else 'a class declared LATER in the source'] += 1
                for text, opts in UNDETERMINED:
                    _, alt = prefix(unit, cd, uni, opts)
                    if alt != base:
                        differs[text] += 1
                if cd.kind == 'interface':
                    forms['interfaces'] += 1
                    if cd.members:
                        forms['interfaces with members'] += 1
                for x in cd.members:
                    for mod in x.mods:
                        if mod in ('protected', 'synchronized'):
                            forms['modifier ' + mod] += 1
                    if x.k != 'field' and 'abstract' in x.mods:
                        forms['abstract methods'] += 1
                    if x.k == 'field' and x.dims:
                        forms['fields with [] behind the name'] += 1
                    for n in srcparse.walk(x):
                        if n.k == 'type' and '.' in n.base:
                            forms['qualified type names'] += 1
                        if n.k in ('param', 'decl') and n.dims:
                            forms['[] behind a parameter or local name'] += 1
        out('%s: %d sources, %d classes' % (label, c['sources'], c['classes']))
        for k in ('field rows', 'method rows', 'trees', 'pool entries of steps 1-3'):
            out('   %-58s %7d' % (k, c[k]))
        out('   S1 classes with an unresolved class name                   %7d' % c['classes with an unresolved class name'])
        for k, v in unresolved.most_common(20):
            out('         %-40s %d' % (k, v))
        out('   class names written in the sources (once per occurrence: extends, implements,')
        out('   every type that names a class), by the scope that resolves them:')
        for k, v in scope.most_common():
            out('         %-40s %d' % (k, v))
        for k, v in sorted(direction.items()):
            out('            same source: %-37s %d' % (k, v))
        out('   S4 class name uses found in more than one visible package  %7d of %d' % (
            c['ambiguous uses'], c['class name uses']))
        out('   S2 classes whose prediction changes under an undetermined rule:')
        for text, opts in UNDETERMINED:
            out('         %-40s %d' % (text, differs[text]))
        out('   S3 declaration forms:')
        for k in ('sources with more than one class', 'interfaces', 'interfaces with members',
                  'abstract methods', 'modifier protected', 'modifier synchronized',
                  'fields with [] behind the name', '[] behind a parameter or local name',
                  'qualified type names', 'single-type imports',
                  'imports of the form "parts:part.root.BodyPart.*"'):
            out('         %-52s %d' % (k, forms[k]))
        out('')
    text = '\n'.join(lines)
    sys.stdout.write(text)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'check_source_only.txt'), 'w', encoding='utf-8') as f:
        f.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
