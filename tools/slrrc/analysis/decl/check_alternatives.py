#!/usr/bin/env python3
"""check_alternatives.py - how firmly does the corpus pin each rule of the
pool model?

The model of poolmodel.py reproduces every pool.  This script switches ONE
rule at a time to an alternative and counts the classes whose pool is then
predicted wrongly.  A count of 0 means the corpus cannot tell the two rules
apart (the rule is "undetermined"); a positive count is the number of
classes that witness the rule.

It also prints the accounting of the default run: how many pool entries each
step and rule interned, and the counters of the resolution walk.

    python -I check_alternatives.py
"""
import json
import os
import sys
from collections import Counter, OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'lib')))
sys.path.insert(0, HERE)

import oracle      # noqa: E402
import srcparse    # noqa: E402
import universe    # noqa: E402
import poolmodel   # noqa: E402

OUT = os.path.join(HERE, 'out')

# (step, description of the alternative, options)
ALTERNATIVES = [
    ('1', 'seed order: superclass before the class itself', {'seed': 'super,name,ifaces'}),
    ('1', 'seed order: interfaces before the superclass', {'seed': 'name,ifaces,super'}),
    ('2', 'return type interned BEFORE the parameters', {'ret': 'before'}),
    ('2', 'return type interned after the method body', {'ret': 'end'}),
    ('2', 'parameter types not interned while parsing', {'param_types': False}),
    ('2', 'field type interned AFTER its initialiser', {'field': 'post'}),
    ('2', 'local type interned BEFORE its initialisers', {'local': 'pre'}),
    ('2', 'local type interned after EACH declarator', {'local': 'each'}),
    ('2', 'type of new T(args) interned AFTER the arguments', {'new': 'post'}),
    ('2', 'type of new T[n] interned AFTER the size', {'newarray': 'post'}),
    ('2', 'type of (int)x / (float)x interned AFTER x', {'cast': 'post'}),
    ('2', 'a cast to a class interns "LT;" too', {'cast_class_type': True}),
    ('2', 'the class entry of a cast is interned while parsing', {'cast_class_entry': 'parse'}),
    ('2', 'type of x instanceof T interned BEFORE x', {'instanceof': 'pre'}),
    ('2', 'no "LI;" per interface at the end of the body', {'iface_types': False}),
    ('2', 'no "LC;" for a class that is not first in its file', {'own_type': 'never'}),
    ('2', '"LC;" at the end of EVERY class body', {'own_type': 'always'}),
    ('2', 'end of body: own type before the interface types', {'class_end': 'own,ifaces'}),
    ('3', 'names by row vector (static fields, fields, static methods, methods)', {'names': 'vectors'}),
    ('3', 'method: name before descriptor', {'method_name': 'name,desc'}),
    ('4', 'trees resolved in tree (source) order', {'ref_order': 'all'}),
    ('4', 'methods in source order, then field initialisers in source order', {'ref_order': 'm,f'}),
    ('4', 'instance methods before static methods', {'ref_order': 'im,sm,sf,if'}),
    ('4', 'instance field initialisers before static ones', {'ref_order': 'sm,im,if,sf'}),
    ('4', 'field initialisers before methods', {'ref_order': 'sf,if,sm,im'}),
    ('4', '"this" does not intern the own type', {'this_type': False}),
    ('4', 'receiver class not interned before a lookup', {'touch': 'none'}),
    ('4', 'receiver class interned only before an unresolved name', {'touch': 'name'}),
    ('4', 'receiver class interned only before a resolved member', {'touch': 'member'}),
    ('4', 'receiver class interned for array receivers too', {'touch_array': True}),
    ('4', 'member ref: nat before class', {'ref_inner': 'nat,class'}),
    ('4', 'nat: descriptor before name', {'nat_inner': 'desc,name'}),
    ('5', 'strings interned in the order of step 4 (row vectors)', {'emit_order': 'vectors'}),
    ('4+5', 'one pass: refs and strings of a tree together, row vector order', {'merge_45': True}),
    ('4+5', 'one pass: refs and strings together, tree order', {'merge_45': True, 'ref_order': 'all'}),
]


def load_all():
    data = []
    for e in oracle.iter_pairs(None):
        unit = srcparse.parse(oracle.load_source(e))
        blobs = oracle.load_class(e)
        data.append((e, unit, blobs))
    return data


def run(data, uni, opts):
    """-> (failing classes, failing pairs, failing oracle classes, models)"""
    fc = fp = fo = 0
    models = []
    first = []
    for e, unit, blobs in data:
        bad = False
        for c, b in zip(unit.classes, blobs):
            m = poolmodel.Model(unit, c, b, uni, opts)
            m.run()
            ok = m.pool.entries == m.cons_content
            models.append((e, m, ok))
            if not ok:
                fc += 1
                bad = True
                if e.builds == [oracle.BUILD_940] and e.consistent:
                    fo += 1
                if len(first) < 3:
                    first.append('%s (%s)' % (e.key, c.name))
        fp += bad
    return fc, fp, fo, models, first


def main(argv):
    uni = universe.load()
    data = load_all()
    nclasses = sum(len(b) for _, _, b in data)
    lines = []
    out = lines.append
    fc, fp, fo, models, _ = run(data, uni, None)
    out('default model: %d of %d classes predicted exactly (%d pairs)' % (
        nclasses - fc, nclasses, len(data)))
    out('')
    # ---- accounting of the default run
    phase = Counter()
    kinds = Counter()
    stats = Counter()
    scope = Counter()
    for e, m, ok in models:
        for ent, ph in zip(m.pool.entries, m.pool.phase):
            phase[ph] += 1
            kinds[ent[0]] += 1
        for k, v in m.stats.items():
            stats[k] += v
        for name, sc, k in m.class_name_census():
            scope[sc] += 1
    out('pool entries of the %d classes by the rule that interned them:' % nclasses)
    for k in sorted(phase):
        out('   %-46s %7d' % (k, phase[k]))
    out('   %-46s %7d' % ('total', sum(phase.values())))
    out('by kind: ' + ', '.join('%s %d' % (k, kinds[k]) for k in sorted(kinds)))
    out('')
    out('counters of the resolution walk:')
    for k in sorted(stats):
        out('   %-46s %7d' % (k, stats[k]))
    out('')
    out('class names written in the sources of the pairs (once per occurrence: extends,')
    out('implements and every type that names a class), by the scope that resolves them:')
    for k, v in scope.most_common():
        out('   %-46s %7d' % (k, v))
    out('')
    out('one rule changed at a time: classes whose pool is then wrong')
    out('   %-4s %-72s %8s %8s %8s' % ('step', 'alternative', 'classes', 'pairs', 'oracle'))
    table = []
    for step, text, opts in ALTERNATIVES:
        fc, fp, fo, _, first = run(data, uni, opts)
        out('   %-4s %-72s %8d %8d %8d   %s' % (step, text, fc, fp, fo,
                                             ('e.g. ' + ', '.join(first[:2])) if first else 'UNDETERMINED'))
        table.append(OrderedDict(step=step, alternative=text, options=opts,
                                 classes_wrong=fc, pairs_wrong=fp,
                                 oracle_classes_wrong=fo, examples=first))
    text = '\n'.join(lines) + '\n'
    sys.stdout.write(text)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'check_alternatives.txt'), 'w', encoding='utf-8') as f:
        f.write(text)
    with open(os.path.join(OUT, 'check_alternatives.json'), 'w', encoding='utf-8') as f:
        json.dump({'classes': nclasses, 'pairs': len(data),
                   'entries_by_rule': dict(phase), 'entries_by_kind': dict(kinds),
                   'walk_counters': dict(stats), 'scopes': dict(scope),
                   'alternatives': table}, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
