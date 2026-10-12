#!/usr/bin/env python3
"""check_destruct.py - every tree of the corpus through destruct.py.

No source is used: each of the 9,772 trees must split into the statement
forms of notes/statements.md.  Reported per group of class files:

    oracle        class has a source, Build 940, consistent
    other pairs   class has a source (stale or older build)
    no source     the 102 class files that ship without a source

For the trees that stmtgen.py reproduces from source, the role this script
gives each jump is compared with the role the model gave it: two independent
readings of the same nodes.

Also checked here, from the node side: the fifth word of a method row equals
the highest number of locals the structure walk sees live (foundation M5).

    python -I check_destruct.py [--show N] [--only KEY]

Writes out/check_destruct.txt and out/check_destruct.json.
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

import align       # noqa: E402
import check_stmt  # noqa: E402
import destruct    # noqa: E402
import disasm      # noqa: E402
import oracle      # noqa: E402
import sparse      # noqa: E402
import tufa        # noqa: E402

# engine/include/vm_case_map.inc kVmStmtCaseMap: tag -> sub-body of op 0x1008
STMT_CASE = (0, 1, 1, 1, 1, 1, 1, 1, 1, 1,
             1, 1, 11, 1, 1, 1, 1, 1, 1, 1,
             2, 2, 2, 3, 4, 3, 4, 5, 6, 11,
             7, 8, 9, 9, 11, 10, 10, 10, 10, 10,
             10, 10, 10, 10, 10, 10, 10)

ROLE_MAP = {
    'for.jump_false': 'loop.jump_false', 'while.jump_false': 'loop.jump_false',
    'for.jump_back': 'loop.jump_back', 'while.jump_back': 'loop.jump_back',
}


def tree_info(blob):
    """tree index -> (kind, is_ctor, is_static, nargs, ret_void, label, row)"""
    out = {}
    for st, i, f in blob.fields():
        if f.init != tufa.NONE:
            out[f.init] = ('field', False, st, 0, True,
                           'field %s' % blob.utf8(f.name), f)
    for st, i, m in blob.methods():
        if m.tree == tufa.NONE:
            continue
        name = blob.utf8(m.name)
        desc = blob.utf8(m.desc)
        params, ret = disasm.split_descriptor(desc)
        out[m.tree] = ('method', name == '<init>', st, len(params),
                       ret in ('', 'V'), '%s%s' % (name, desc), m)
    return out


def main(argv):
    show = 0
    only = None
    it = iter(argv)
    for a in it:
        if a == '--show':
            show = int(next(it))
        elif a == '--only':
            only = next(it)
    entries = {e.cls: e for e in oracle.iter_entries(('pair_940', 'pair_other'))}
    tot = defaultdict(Counter)
    forms = defaultdict(Counter)
    roles = defaultdict(Counter)
    fails = defaultdict(list)
    agree = Counter()
    stmt_tags = defaultdict(Counter)
    disagree = Counter()
    disagree_eg = {}
    notes = []
    shown = 0
    only_cls = oracle.entry(only).cls if only else None
    for rel, blobs in oracle.iter_class_files():
        if only_cls and rel != only_cls:
            continue
        e = entries.get(rel)
        if e is None:
            grp = 'no source'
        elif e.category == 'pair_940' and e.consistent:
            grp = 'oracle'
        else:
            grp = 'other pairs'
        unit = sparse.parse(oracle.load_source(e)) if e is not None else None
        tot[grp]['class files'] += 1
        for bi, b in enumerate(blobs):
            tot[grp]['blobs'] += 1
            info = tree_info(b)
            preds = {}
            if unit is not None and len(unit.classes) == len(blobs):
                try:
                    al = align.align(unit.classes[bi], b)
                except align.AlignError:
                    al = []
                for t, kind, node, row, st in al:
                    if t is None:
                        continue
                    r = check_stmt.run_tree(unit, b, t, kind, node, row, st)
                    if r.ok_struct:
                        preds[t] = r.gen.out
            for t, nodes in enumerate(b.trees):
                kind, is_ctor, st, nargs, ret_void, label, row = info[t]
                tot[grp]['trees'] += 1
                tot[grp]['nodes'] += len(nodes)
                njump = sum(1 for x in nodes if tufa.OP_FLAGS[x.op] in
                            (0x1014, 0x1015, 0x1016, 0x1017))
                for x in nodes:
                    if tufa.OP_FLAGS[x.op] == 0x1008:
                        stmt_tags[grp][x.imm] += 1
                tot[grp]['jumps'] += njump
                d = destruct.Destruct(b, nodes, kind, is_ctor, st, nargs, ret_void)
                try:
                    d.run()
                except destruct.Fail as x:
                    tot[grp]['trees NOT split into statement forms'] += 1
                    fails[grp].append((rel, bi, t, label, x.why, x.pos))
                    if shown < show:
                        shown += 1
                        print('--- %s blob %d tree %d %s: %s (node %s)' % (rel, bi, t, label, x.why, x.pos))
                        lm = disasm.local_names(b, st, row) if kind == 'method' else None
                        lines = disasm.tree_lines(b, t, locals_map=lm)
                        a = max(0, (x.pos or 0) - 30)
                        z = min(len(lines), (x.pos or 0) + 12)
                        print('\n'.join(lines[a:z]))
                    continue
                tot[grp]['trees split into statement forms'] += 1
                tot[grp]['nodes in those trees'] += len(nodes)
                tot[grp]['jumps given a role'] += len(d.roles)
                for f, c in d.forms.items():
                    forms[grp][f] += c
                for i, r in d.roles.items():
                    roles[grp][r] += 1
                for n in d.notes:
                    notes.append('%s blob %d tree %d %s: %s' % (rel, bi, t, label, n))
                if kind == 'method':
                    tot[grp]['method trees split'] += 1
                    want = d.max_live if nodes else 0
                    if row.locals == want:
                        tot[grp]['locals word = most locals live in the walk'] += 1
                    else:
                        tot[grp]['locals word differs'] += 1
                        notes.append('%s blob %d tree %d %s: locals word %d, walk %d'
                                     % (rel, bi, t, label, row.locals, want))
                pred = preds.get(t)
                if pred is not None:
                    for i, r in d.roles.items():
                        m = pred[i].cat
                        m = ROLE_MAP.get(m, m)
                        if m == r:
                            agree[r] += 1
                        else:
                            disagree[(m, r)] += 1
                            disagree_eg.setdefault((m, r), '%s blob %d tree %d node %d' % (rel, bi, t, i))
    L = []
    w = L.append
    w('check_destruct.py: statement structure read back from the nodes alone')
    w('')
    groups = ['oracle', 'other pairs', 'no source']
    for grp in groups:
        if grp not in tot:
            continue
        w('group %s' % grp)
        for k in sorted(tot[grp]):
            w('  %-50s %d' % (k, tot[grp][k]))
        w('')
    for grp in groups:
        if not fails[grp]:
            continue
        w('trees that do not split, group %s: %d' % (grp, len(fails[grp])))
        by = Counter(x[4] for x in fails[grp])
        for why, c in by.most_common():
            w('  %4d  %s' % (c, why))
        for rel, bi, t, label, why, pos in fails[grp][:40]:
            w('        %s blob %d tree %d %s: %s (node %s)' % (rel, bi, t, label, why, pos))
        w('')
    allforms = sorted(set(f for g in forms.values() for f in g))
    w('statement forms found (trees that split)')
    w('  %-46s %9s %9s %9s' % ('form', 'oracle', 'other', 'no source'))
    for f in allforms:
        w('  %-46s %9d %9d %9d' % (f, forms['oracle'][f], forms['other pairs'][f],
                                   forms['no source'][f]))
    w('')
    w('0x1008 `stmt` nodes by expression kind and by the sub-body of the interpreter')
    w('(kVmStmtCaseMap of engine/include/vm_case_map.inc; old exe byte_423B04)')
    import opcodes
    w('  %-5s %-12s %-8s %9s %9s %9s' % ('tag', 'kind', 'sub-body', 'oracle', 'other', 'no source'))
    alltags = sorted(set(t for g in stmt_tags.values() for t in g))
    for tg in alltags:
        ti = opcodes.tag_info(tg)
        w('  0x%02X  %-12s %-8s %9d %9d %9d' % (
            tg, ti.name if ti else '?', STMT_CASE[tg] if tg < len(STMT_CASE) else '-',
            stmt_tags['oracle'][tg], stmt_tags['other pairs'][tg], stmt_tags['no source'][tg]))
    w('  total 0x1008 nodes: %d' % sum(sum(g.values()) for g in stmt_tags.values()))
    w('')
    allroles = sorted(set(r for g in roles.values() for r in g))
    w('jump roles')
    w('  %-46s %9s %9s %9s' % ('role', 'oracle', 'other', 'no source'))
    for r in allroles:
        w('  %-46s %9d %9d %9d' % (r, roles['oracle'][r], roles['other pairs'][r],
                                   roles['no source'][r]))
    w('')
    w('jump roles against the model compiler (trees stmtgen.py reproduces)')
    w('  same role: %d' % sum(agree.values()))
    for r in sorted(agree):
        w('    %-30s %d' % (r, agree[r]))
    w('  other role: %d' % sum(disagree.values()))
    for (m, r), c in sorted(disagree.items()):
        w('    model %-22s here %-22s %5d   e.g. %s' % (m, r, c, disagree_eg[(m, r)]))
    w('')
    w('notes: %d' % len(notes))
    for n in notes[:40]:
        w('  ' + n)
    text = '\n'.join(L) + '\n'
    if not only:
        with open(os.path.join(_HERE, 'out', 'check_destruct.txt'), 'w', newline='\n') as f:
            f.write(text)
        with open(os.path.join(_HERE, 'out', 'check_destruct.json'), 'w', newline='\n') as f:
            json.dump({'totals': {g: dict(c) for g, c in tot.items()},
                       'forms': {g: dict(c) for g, c in forms.items()},
                       'roles': {g: dict(c) for g, c in roles.items()},
                       'stmt_tags': {g: {'0x%02X' % t: n for t, n in c.items()}
                                     for g, c in stmt_tags.items()},
                       'role_agreement': dict(agree),
                       'role_disagreement': {'%s|%s' % k: v for k, v in disagree.items()},
                       'failures': {g: [list(x) for x in v] for g, v in fails.items()},
                       'notes': notes}, f, indent=1, sort_keys=True)
            f.write('\n')
    sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
