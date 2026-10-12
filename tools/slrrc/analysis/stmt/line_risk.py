#!/usr/bin/env python3
"""line_risk.py - how well is each line rule tested, and does it matter for
the sources that still have to be compiled?

Every node carries a source line.  check_stmt.py reproduces the line of all
nodes of the oracle pairs, but a rule is only TESTED by the nodes for which a
rival rule would give another line (a one-line statement cannot tell "line of
the last token" from "line of the next token").  check_stmt.py records, per
node category and rival, how many nodes tell the two apart.

This script runs the model compiler in free mode (no class to look at) over
the 635 sources that have no class file, and counts the nodes for which a
rival that the oracle never contradicted would give another line.  Those are
the nodes whose line is predicted by reasoning about the grammar rather than
by evidence.

Needs out/check_stmt.json (run check_stmt.py first).
Writes out/line_risk.txt and out/line_risk.json.
"""
import json
import os
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for _p in (_LIB, _HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import oracle    # noqa: E402
import sparse    # noqa: E402
import stmtgen   # noqa: E402


def base_cat(cat):
    if cat in ('name.local', 'name.field'):
        return 'name (single node)'
    if cat.startswith('name.path.seg'):
        return 'name.path segment'
    if cat.startswith('call.path.seg'):
        return 'call.path segment'
    if cat in ('cast.class', 'cast.type'):
        return 'cast carrier'
    if cat.startswith('lit.'):
        return 'literal carrier'
    return cat


def free_nodes(unit):
    """Every node the model emits for a parsed source, in free mode."""
    for c in unit.classes:
        for m in c.members:
            if m.k == 'field':
                for d in m.decls:
                    if d.init is not None:
                        g = stmtgen.Gen(unit, None, None)
                        g.field_init(d)
                        g.finish()
                        for x in g.out:
                            yield x
            elif m.k == 'method' and 'abstract' not in m.mods:
                g = stmtgen.Gen(unit, None, None)
                g.method(m)
                g.finish()
                for x in g.out:
                    yield x


def main():
    with open(os.path.join(_HERE, 'out', 'check_stmt.json')) as f:
        doc = json.load(f)
    tested = defaultdict(Counter)      # base cat -> counters
    nodes_940 = Counter()
    for key, c in doc['line_rivals'].items():
        grp, cat = key.split('|', 1)
        if grp != '940':
            continue
        b = base_cat(cat)
        for k, v in c.items():
            tested[b][k] += v
    for key, c in doc['lines'].items():
        grp, cat = key.split('|', 1)
        if grp == '940':
            nodes_940[base_cat(cat)] += c.get('ok', 0) + c.get('bad', 0)
    target = defaultdict(Counter)
    target_nodes = Counter()
    files = 0
    eg = {}
    for e in oracle.iter_entries('source_only'):
        files += 1
        unit = sparse.parse(oracle.load_source(e))
        for x in free_nodes(unit):
            b = base_cat(x.cat)
            target_nodes[b] += 1
            if not x.alts:
                continue
            for lab, ln in x.alts:
                if ln != x.ln:
                    target[b][lab] += 1
                    eg.setdefault((b, lab), '%s line %d (rival: %d)' % (e.source, x.ln, ln))
    cats = sorted(set(tested) | set(target) | set(target_nodes) | set(nodes_940))
    L = []
    w = L.append
    w('line_risk.py: line rules, their rivals, and the %d sources without a class' % files)
    w('')
    w('columns: nodes of the category in the oracle pairs; per rival rule the number')
    w('of oracle nodes that tell it from the rule ("refuted by"), and the number of')
    w('nodes in the class-less sources for which that rival gives another line.')
    w('A rival that was never told apart AND differs in the class-less sources is')
    w('marked AT RISK.')
    w('')
    risk = []
    rows = []
    for b in cats:
        labs = sorted(set(k[:-8] for k in tested[b] if k.endswith(' differs')) | set(target[b]))
        w('%-26s oracle nodes %-7d class-less nodes %d' % (b, nodes_940[b], target_nodes[b]))
        for lab in labs:
            d = tested[b].get(lab + ' differs', 0)
            fits = tested[b].get(lab + ' fits', 0)
            t = target[b].get(lab, 0)
            flag = ''
            if d == 0 and t > 0:
                flag = '   AT RISK'
                risk.append((b, lab, t))
            elif fits:
                flag = '   !! rival fits %d oracle nodes' % fits
            w('    rival %-16s refuted by %-6d differs in class-less sources: %d%s'
              % (lab, d - fits, t, flag))
            rows.append({'category': b, 'rival': lab, 'refuted_by': d - fits,
                         'rival_fits': fits, 'classless_differs': t})
        if not labs:
            w('    (no rival gives another line anywhere)')
    w('')
    w('AT RISK: %d (category, rival) pairs, %d nodes' % (len(risk), sum(r[2] for r in risk)))
    for b, lab, t in risk:
        w('  %-26s rival %-16s %5d nodes   e.g. %s' % (b, lab, t, eg.get((b, lab), '')))
    text = '\n'.join(L) + '\n'
    with open(os.path.join(_HERE, 'out', 'line_risk.txt'), 'w', newline='\n') as f:
        f.write(text)
    with open(os.path.join(_HERE, 'out', 'line_risk.json'), 'w', newline='\n') as f:
        json.dump({'files': files, 'rows': rows,
                   'at_risk': [list(r) for r in risk]}, f, indent=1, sort_keys=True)
        f.write('\n')
    sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
