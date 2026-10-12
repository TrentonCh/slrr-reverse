#!/usr/bin/env python3
"""probe_source_only.py - run the model on the 635 sources without a class.

Nothing can be compared here; the question is whether the rules proven on the
pairs are ENOUGH to compile these sources: does every name resolve, does every
construct fall under a counted rule?  Prints the notes the model leaves
(unresolved names, unknown types, unknown members) and the rule applications
that have no counterpart in the pairs.

    python -I probe_source_only.py [--json FILE]
"""
import json
import os
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for p in (_LIB, _HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import oracle       # noqa: E402
import classdb      # noqa: E402
import check_expr   # noqa: E402
import gen          # noqa: E402
import jparse       # noqa: E402


def main(argv):
    db = classdb.load()
    model = gen.Model(db)
    # rules seen in the pairs
    base = check_expr.run(check_expr.entries_of('940') + check_expr.entries_of('other'),
                          {}, quiet=True)
    known = set(k for k, v in base['rules'].items() if v['held'])
    # side counts that describe types, scores and clashes are not forms
    side = ('types.', 'method ref score.', 'method ref kind.', 'method ref.',
            'method ref tie.', 'call name clash.', 'name context.')
    files = trees = nodes = 0
    notes = Counter()
    note_ex = defaultdict(list)
    rules = Counter()
    new_rules = Counter()
    new_ex = defaultdict(list)
    clean_files = 0
    for e in oracle.iter_entries('source_only'):
        files += 1
        text = oracle.load_source(e)
        unit = jparse.parse(text)
        clean = True
        for uc in model.compile_unit(unit):
            for t in uc.trees:
                trees += 1
                nodes += len(t.nodes)
                for n in t.notes:
                    kind = n.split(' ')[0] + ' ' + n.split(' ')[1]
                    notes[kind] += 1
                    clean = False
                    if len(note_ex[kind]) < 5:
                        note_ex[kind].append('%s: %s' % (e.source, n))
                for key, ast, a, z in t.rules:
                    if key.startswith(side):
                        if key.startswith(('method ref tie.', 'call name clash.')):
                            notes['side: ' + key] += 1
                            clean = False
                        continue
                    rules[key] += 1
                    if key not in known:
                        new_rules[key] += 1
                        if len(new_ex[key]) < 3:
                            new_ex[key].append('%s:%d' % (e.source, ast.line))
        if clean:
            clean_files += 1
    print('sources without a class: %d, trees predicted: %d, nodes predicted: %d' % (
        files, trees, nodes))
    print('files the model compiles without a note: %d of %d' % (clean_files, files))
    print('notes:')
    for k, v in notes.most_common():
        print('  %6d  %s' % (v, k))
        for x in note_ex[k]:
            print('            ' + x)
    print('rule applications: %d in %d distinct rules; rules not seen in any pair: %d' % (
        sum(rules.values()), len(rules), len(new_rules)))
    for k, v in new_rules.most_common():
        print('  %6d  %s   %s' % (v, k, ', '.join(new_ex[k])))
    if '--json' in argv:
        with open(argv[argv.index('--json') + 1], 'w', encoding='utf-8') as f:
            json.dump({'files': files, 'trees': trees, 'nodes': nodes,
                       'clean_files': clean_files, 'notes': dict(notes),
                       'note_examples': dict(note_ex), 'rules': dict(rules),
                       'new_rules': dict(new_rules), 'new_examples': dict(new_ex)}, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
