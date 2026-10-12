#!/usr/bin/env python3
"""check_stmt.py - prove the statement rules against the corpus.

For every source/class pair: parse the source (sparse.py), attribute every
tree of the class to its declaration (align.py), let the model compiler
(stmtgen.py) emit the tree and compare it with the class node for node:
opcode, immediate (jump offsets, local indices, counts, expression kinds) and
source line.

    python -I check_stmt.py                 all 2,819 pairs
    python -I check_stmt.py --only KEY      one entry (oracle.entry syntax)
    python -I check_stmt.py --show N        print N failing trees in full
    python -I check_stmt.py --lib           parse with lib/parser.py (through
                                            libast.py) instead of sparse.py;
                                            writes out/check_stmt_lib.txt/.json

Writes out/check_stmt.txt and out/check_stmt.json.
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

import align     # noqa: E402
import disasm    # noqa: E402
import opcodes   # noqa: E402
import oracle    # noqa: E402
import sparse    # noqa: E402
import stmtgen   # noqa: E402
import tufa      # noqa: E402


def group_of(e):
    """Corpus group of an entry: the oracle (Build 940, consistent), the
    Build 940 stale pairs, the older builds."""
    if e.category == 'pair_940':
        return '940' if e.consistent else '940 stale'
    return 'older' if e.consistent else 'older stale'


class TreeResult(object):
    __slots__ = ('ok_struct', 'ok_lines', 'why', 'pos', 'gen', 'bad_lines')


def run_tree(unit, blob, t, kind, node, row, is_static):
    """Emit one tree and compare.  Returns (TreeResult, Gen)."""
    actual = blob.trees[t]
    g = stmtgen.Gen(unit, blob, actual)
    r = TreeResult()
    r.gen = g
    r.bad_lines = []
    r.pos = None
    try:
        if kind == 'field':
            g.field_init(node)
        else:
            g.method(node)
        g.finish()
    except stmtgen.Mismatch as x:
        r.ok_struct = False
        r.ok_lines = False
        r.why = 'oracle window: ' + _generalise(x.why)
        r.pos = x.pos
        return r
    out = g.out
    n = min(len(out), len(actual))
    for i in range(n):
        a = actual[i]
        p = out[i]
        ac = tufa.OP_FLAGS[a.op]
        if ac != p.code:
            r.ok_struct = False
            r.ok_lines = False
            r.why = 'opcode: rule %s [%s], class %s' % (
                _mn(p.code, p.imm), p.cat, _mn(ac, a.imm))
            r.pos = i
            return r
        if a.imm != p.imm:
            r.ok_struct = False
            r.ok_lines = False
            r.why = 'immediate of %s [%s]' % (_mn(p.code, None), p.cat)
            r.pos = i
            return r
    if len(out) != len(actual):
        r.ok_struct = False
        r.ok_lines = False
        if len(out) > len(actual):
            r.why = 'rule emits more nodes: next %s [%s]' % (
                _mn(out[n].code, out[n].imm), out[n].cat)
        else:
            a = actual[n]
            r.why = 'class has more nodes: next %s' % _mn(tufa.OP_FLAGS[a.op], a.imm)
        r.pos = n
        return r
    r.ok_struct = True
    for i in range(n):
        if actual[i].slot != out[i].ln:
            r.bad_lines.append(i)
    r.ok_lines = not r.bad_lines
    r.why = None
    return r


def _mn(code, imm):
    raw = stmtgen.RAW_OF.get(code)
    if raw is None:
        return '0x%04X' % code
    o = opcodes.OPS[raw]
    if o.imm == opcodes.IMM_TAG and imm is not None:
        return opcodes.mnemonic(raw, imm)
    return o.name


def _generalise(why):
    """Strip numbers and names from a Mismatch text so that it groups."""
    import re
    w = re.sub(r"'[^']*'", "'..'", why)
    w = re.sub(r'\b\d+\b', 'N', w)
    w = re.sub(r'name [\w.]+', 'name X', w)
    w = re.sub(r'call [\w.]+', 'call X', w)
    w = re.sub(r'says \w+ is', 'says X is', w)
    return w


def method_label(blob, row):
    return '%s%s' % (blob.utf8(row.name), blob.utf8(row.desc))


def dump_tree(e, unit, blob, bi, t, r, kind, row, lines_only=False, ctx=None):
    """Readable side-by-side of a tree: source, class nodes, predicted nodes."""
    L = []
    actual = blob.trees[t]
    own = align.owners(blob).get(t)
    lm = None
    if own and own[0] == 'method':
        lm = disasm.local_names(blob, own[1], own[2])
    al = disasm.tree_lines(blob, t, locals_map=lm)
    out = r.gen.out
    src = oracle.load_source(e).split('\n')
    lo = min([n.slot for n in actual] + [x.ln for x in out if x.ln] or [1])
    hi = max([n.slot for n in actual] + [x.ln for x in out if x.ln] or [1])
    L.append('--- %s blob %d tree %d (%s %s)' % (
        e.cls, bi, t, kind, blob.utf8(row.name)))
    if r.why:
        L.append('    first difference at node %s: %s' % (r.pos, r.why))
    a0, a1 = 0, max(len(al), len(out))
    if ctx is not None and r.pos is not None:
        a0, a1 = max(0, r.pos - ctx), min(a1, r.pos + ctx + 1)
        lines = [actual[i].slot for i in range(a0, min(a1, len(actual)))]
        if lines:
            lo, hi = min(lines), max(lines)
    for ln in range(max(1, lo - 1), min(len(src), hi + 1) + 1):
        L.append('    %5d  %s' % (ln, src[ln - 1].rstrip('\r').expandtabs(4)))
    for i in range(a0, a1):
        left = al[i] if i < len(al) else '%6d  (none)' % i
        if i < len(out):
            p = out[i]
            imm = '' if p.imm is None else (' %d' % p.imm if p.imm < 0x80000000
                                             else ' %d' % (p.imm - (1 << 32)))
            right = 'L%-5s %s%s [%s]' % (p.ln, _mn(p.code, p.imm), imm if p.code not in (0x1007, 0x1008) else '', p.cat)
        else:
            right = '(none)'
        mark = '  '
        if i < len(actual) and i < len(out):
            if tufa.OP_FLAGS[actual[i].op] != out[i].code or actual[i].imm != out[i].imm:
                mark = '!!'
            elif actual[i].slot != out[i].ln:
                mark = 'L!'
        else:
            mark = '!!'
        L.append('%s %-78s | %s' % (mark, left[:78], right))
    return L


def main(argv):
    only = None
    show = 0
    show_lines = 0
    use_lib = False
    it = iter(argv)
    for a in it:
        if a == '--lib':
            use_lib = True
        elif a == '--only':
            only = next(it)
        elif a == '--show':
            show = int(next(it))
        elif a == '--show-lines':
            show_lines = int(next(it))
    entries = [e for e in oracle.iter_entries(('pair_940', 'pair_other'))]
    if only:
        entries = [oracle.entry(only)]
    tot = defaultdict(Counter)          # group -> counter
    why = defaultdict(Counter)          # group -> reason -> count
    why_eg = {}
    line_cat = defaultdict(Counter)     # cat -> {'ok','bad'}
    line_eg = defaultdict(list)
    alt_cat = defaultdict(Counter)      # (group, cat) -> rival line rules
    forms = defaultdict(Counter)        # group -> statement form -> count (in fully matched trees)
    forms_all = defaultdict(Counter)    # group -> form -> count (all trees emitted)
    notes = Counter()
    notes_eg = {}
    align_fail = []
    shown = []
    shown_lines = []
    files_ok = defaultdict(Counter)
    for e in entries:
        grp = group_of(e)
        if use_lib:
            import libast
            unit = libast.parse(oracle.load_source(e), e.source)
        else:
            unit = sparse.parse(oracle.load_source(e))
        blobs = oracle.load_class(e)
        tot[grp]['files'] += 1
        file_struct = True
        file_lines = True
        if len(unit.classes) != len(blobs):
            align_fail.append((e.cls, 'class count %d / %d' % (len(unit.classes), len(blobs))))
            continue
        for bi, (cls, blob) in enumerate(zip(unit.classes, blobs)):
            try:
                al = align.align(cls, blob)
            except align.AlignError as x:
                align_fail.append((e.cls, 'blob %d: %s' % (bi, x)))
                file_struct = file_lines = False
                continue
            for t, kind, node, row, st in al:
                if t is None:
                    tot[grp]['members without a tree'] += 1
                    continue
                r = run_tree(unit, blob, t, kind, node, row, st)
                what = 'field initialiser' if kind == 'field' else (
                    'constructor' if node.ctor else 'method')
                tot[grp]['trees'] += 1
                tot[grp]['trees: ' + what] += 1
                tot[grp]['nodes'] += len(blob.trees[t])
                for f, c in r.gen.stats.items():
                    forms_all[grp][f] += c
                if r.ok_struct and kind == 'method':
                    want = r.gen.max_locals if blob.trees[t] else 0
                    tot[grp]['method trees: locals word checked'] += 1
                    if row.locals == want:
                        tot[grp]['method trees: locals word = high-water mark of the scope model'] += 1
                if r.ok_struct:
                    tot[grp]['trees reproduced (opcodes and immediates)'] += 1
                    tot[grp]['reproduced: ' + what] += 1
                    tot[grp]['nodes in reproduced trees'] += len(blob.trees[t])
                    for f, c in r.gen.stats.items():
                        forms[grp][f] += c
                    actual = blob.trees[t]
                    bad = set(r.bad_lines)
                    for i, p in enumerate(r.gen.out):
                        if p.alts:
                            lines = [p.ln] + [x[1] for x in p.alts]
                            if len(set(lines)) > 1:
                                ac = alt_cat[(grp, p.cat)]
                                ac['nodes where the rules differ'] += 1
                                ac['RULE' if actual[i].slot == p.ln else 'rule fails'] += 1
                                for lab, ln in p.alts:
                                    if ln != p.ln:
                                        ac['%s differs' % lab] += 1
                                        if ln == actual[i].slot:
                                            ac['%s fits' % lab] += 1
                        if i in bad:
                            line_cat[(grp, p.cat)]['bad'] += 1
                            d = actual[i].slot - p.ln
                            line_cat[(grp, p.cat)]['off %+d' % d if abs(d) < 3 else 'off far'] += 1
                            if len(line_eg[p.cat]) < 6 and grp == '940':
                                line_eg[p.cat].append('%s blob %d tree %d node %d: class L%d rule L%d'
                                                      % (e.cls, bi, t, i, actual[i].slot, p.ln))
                        else:
                            line_cat[(grp, p.cat)]['ok'] += 1
                    if r.ok_lines:
                        tot[grp]['trees reproduced with every line'] += 1
                        tot[grp]['nodes in trees reproduced with every line'] += len(actual)
                    else:
                        file_lines = False
                        tot[grp]['nodes with another line'] += len(r.bad_lines)
                        if len(shown_lines) < show_lines and grp == '940':
                            shown_lines.append(dump_tree(e, unit, blob, bi, t, r, kind, row))
                    for cat, pos, text in r.gen.notes:
                        notes[(grp, cat)] += 1
                        notes_eg.setdefault((grp, cat), [])
                        if len(notes_eg[(grp, cat)]) < 8:
                            notes_eg[(grp, cat)].append('%s blob %d tree %d node %d: %s'
                                                        % (e.cls, bi, t, pos, text))
                else:
                    file_struct = file_lines = False
                    why[grp][r.why] += 1
                    why_eg.setdefault((grp, r.why), [])
                    if len(why_eg[(grp, r.why)]) < 5:
                        why_eg[(grp, r.why)].append('%s blob %d tree %d node %s (%s %s)' % (
                            e.cls, bi, t, r.pos, kind, blob.utf8(row.name)))
                    if len(shown) < show and grp == '940':
                        shown.append(dump_tree(e, unit, blob, bi, t, r, kind, row, ctx=14))
        if file_struct:
            tot[grp]['files reproduced (opcodes and immediates)'] += 1
        if file_lines:
            tot[grp]['files reproduced with every line'] += 1
    # ---- report ---------------------------------------------------------
    L = []
    w = L.append
    w('check_stmt.py: the statement model against the corpus%s'
      % (' (trees of lib/parser.py through libast.py)' if use_lib else ''))
    w('')
    groups = ['940', '940 stale', 'older', 'older stale']
    for grp in groups:
        if grp not in tot:
            continue
        w('group %s' % grp)
        for k in sorted(tot[grp]):
            w('  %-52s %d' % (k, tot[grp][k]))
        w('')
    w('alignment failures: %d' % len(align_fail))
    for a in align_fail:
        w('  %s: %s' % a)
    w('')
    for grp in groups:
        if not why[grp]:
            continue
        w('trees not reproduced, group %s, by first difference' % grp)
        for k, c in why[grp].most_common():
            w('  %5d  %s' % (c, k))
            for eg in why_eg[(grp, k)][:3]:
                w('           %s' % eg)
        w('')
    w('statement forms in reproduced trees / in all trees (group 940)')
    for f in sorted(set(forms['940']) | set(forms_all['940'])):
        w('  %-36s %7d / %7d' % (f, forms['940'][f], forms_all['940'][f]))
    w('')
    w('line of each node category, reproduced trees (ok / other), per group')
    cats = sorted(set(c for g, c in line_cat))
    for cat in cats:
        parts = []
        for grp in groups:
            c = line_cat.get((grp, cat))
            if not c:
                continue
            extra = ' '.join('%s:%d' % (k, v) for k, v in sorted(c.items())
                             if k not in ('ok', 'bad'))
            parts.append('%s %d/%d%s' % (grp, c['ok'], c['bad'], (' (' + extra + ')') if extra else ''))
        w('  %-28s %s' % (cat, ' ; '.join(parts)))
        for eg in line_eg.get(cat, [])[:3]:
            w('        %s' % eg)
    w('')
    w('line rules against their rivals, group 940: only the nodes for which the')
    w('rule and at least one rival give different lines')
    for (grp, cat), c in sorted(alt_cat.items()):
        if grp != '940':
            continue
        labs = sorted(k[:-8] for k in c if k.endswith(' differs'))
        w('  %-28s differ %-6d rule right %-6d rule wrong %-4d | %s' % (
            cat, c['nodes where the rules differ'], c['RULE'], c['rule fails'],
            ' ; '.join('%s %d of %d' % (lab, c[lab + ' fits'], c[lab + ' differs'])
                       for lab in labs)))
    w('')
    w('soft findings (the oracle window showed something the checks question)')
    for (grp, cat), c in sorted(notes.items()):
        w('  %-12s %-12s %d' % (grp, cat, c))
        for eg in notes_eg[(grp, cat)][:6]:
            w('        %s' % eg)
    text = '\n'.join(L) + '\n'
    if not only:
        os.makedirs(os.path.join(_HERE, 'out'), exist_ok=True)
        stem = 'check_stmt_lib' if use_lib else 'check_stmt'
        with open(os.path.join(_HERE, 'out', stem + '.txt'), 'w', newline='\n') as f:
            f.write(text)
        doc = {
            'totals': {g: dict(c) for g, c in tot.items()},
            'not_reproduced': {g: dict(c) for g, c in why.items()},
            'forms_reproduced': {g: dict(c) for g, c in forms.items()},
            'forms_all': {g: dict(c) for g, c in forms_all.items()},
            'lines': {'%s|%s' % k: dict(v) for k, v in line_cat.items()},
            'line_rivals': {'%s|%s' % k: dict(v) for k, v in alt_cat.items()},
            'soft_findings': {'%s|%s' % k: v for k, v in notes.items()},
            'align_failures': align_fail,
        }
        with open(os.path.join(_HERE, 'out', stem + '.json'), 'w', newline='\n') as f:
            json.dump(doc, f, indent=1, sort_keys=True)
            f.write('\n')
    sys.stdout.write(text)
    for d in shown:
        print('\n'.join(d))
    for d in shown_lines:
        print('\n'.join(d))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
