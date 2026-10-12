#!/usr/bin/env python3
"""check_expr.py - compare the model of gen.py with every tree of the corpus.

    python -I check_expr.py [--set NAME] [--only SUBSTR] [--show N] [--opt k=v]
                            [--rules] [--json FILE]

    --set 940        the 2,651 consistent Build 940 pairs (default)
    --set other      the 153 consistent pairs of older builds
    --set consistent both of the above (2,804 pairs)
    --set stale      the 15 stale pairs (source edited after compile)
    --set all        all 2,819 pairs
    --only SUBSTR    only entries whose dir/name contains SUBSTR
    --show N         print the first N mismatches with context
    --opt k=v        model option (see gen.py: ref_class, callee, ifaces, ...)
    --rules          print the count of every rule application
    --json FILE      write the result (counts, rules, examples) as JSON

For every pair the source is parsed, gen.Model predicts the nodes of every
tree, and the prediction is compared with the class file node by node in
symbolic form (symbolic.py).  Statement-level control nodes are filtered from
the class side.  A rule application counts as "held" when it lies in a tree
that is reproduced completely.

As a library:  for rec in check_expr.iter_trees(entries, model): ...
"""
import json
import os
import pickle
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for p in (_LIB, _HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import oracle     # noqa: E402
import tufa       # noqa: E402
import classdb    # noqa: E402
import gen        # noqa: E402
import jparse     # noqa: E402
import symbolic   # noqa: E402


def tree_order(blob):
    """tree index -> (kind, name, desc/type, static)"""
    own = {}
    for st, i, f in blob.fields():
        if f.init != tufa.NONE:
            own[f.init] = ('field', blob.utf8(f.name), blob.utf8(f.type), st)
    for st, i, m in blob.methods():
        if m.tree != tufa.NONE:
            own[m.tree] = ('method', blob.utf8(m.name), blob.utf8(m.desc), st)
    return own


def _callee_form(seq, i, get):
    """(name, descriptor or None, nodes used) when seq[i:] starts with the last
    segment of a call path: a method ref, a name, or `path 1; name`."""
    if i >= len(seq):
        return None
    mn, op = get(i)
    if mn == 'method':
        return op[1], op, 1
    if mn == 'name':
        return op, None, 1
    if mn == 'path' and op == 1 and i + 1 < len(seq):
        m2, o2 = get(i + 1)
        if m2 == 'name':
            return o2, None, 2
    return None


def compare_tree(pred, act_sym, keep, exceptions=None):
    """Walk predicted and actual (filtered) nodes together.  Returns
    (ok, index in pred, index in keep of the first mismatch, alignment) where
    alignment[k] is the keep-index the k-th predicted node was matched at.

    When `exceptions` is a list, a call whose last segment differs only in
    kind or descriptor (method ref against name: the callee class has changed
    since this class was compiled) is appended to it as (pred index, predicted
    form, actual form) and the walk goes on behind it."""
    pk = ak = 0
    align = []
    np_, na = len(pred), len(keep)
    while pk < np_ and ak < na:
        used = symbolic.node_match(pred[pk], act_sym, keep, ak)
        if not used:
            if exceptions is not None:
                pf = _callee_form(pred, pk, lambda i: pred[i])
                af = _callee_form(keep, ak, lambda i: act_sym[keep[i]])
                if pf and af and pf[0] == af[0]:
                    exceptions.append((pk, pf, af))
                    for _ in range(pf[2]):
                        align.append(ak)
                    pk += pf[2]
                    ak += af[2]
                    continue
            return False, pk, ak, align
        align.append(ak)
        pk += 1
        ak += used
    if pk != np_ or ak != na:
        return False, pk, ak, align
    return True, -1, -1, align


class Rec(object):
    """One tree of one pair: prediction and class side."""
    __slots__ = ('entry', 'text', 'unit', 'blob', 'bi', 'ti', 'pt', 'nodes',
                 'sym', 'keep', 'ok', 'pk', 'ak', 'align', 'problem', 'exc')


def entries_of(name):
    if name == '940':
        return list(oracle.iter_oracle_pairs())
    if name == 'other':
        return [e for e in oracle.iter_pairs(build=None, consistent=True)
                if e.builds != [oracle.BUILD_940]]
    if name == 'consistent':
        return list(oracle.iter_pairs(build=None, consistent=True))
    if name == 'stale':
        return list(oracle.iter_pairs(build=None, consistent=False))
    if name == 'all':
        return list(oracle.iter_pairs(build=None))
    raise ValueError(name)


def iter_trees(entries, model, problems=None):
    """Yield a Rec for every tree of every entry.  Structural problems (other
    class names, another tree count, another owner) are appended to
    `problems` as (entry key, text) and the trees concerned are skipped."""
    for e in entries:
        text = oracle.load_source(e)
        unit = jparse.parse(text)
        blobs = oracle.load_class(e)
        ucs = model.compile_unit(unit)
        if [u.name for u in ucs] != [b.name for b in blobs]:
            if problems is not None:
                problems.append((e.key, 'class names differ'))
            continue
        for bi, (uc, b) in enumerate(zip(ucs, blobs)):
            own = tree_order(b)
            if len(uc.trees) != len(b.trees):
                if problems is not None:
                    problems.append((e.key, '%s: %d trees predicted, class has %d' % (
                        b.name, len(uc.trees), len(b.trees))))
                continue
            for ti, (pt, nodes) in enumerate(zip(uc.trees, b.trees)):
                o = own.get(ti)
                if o is None or o[0] != pt.kind or o[1] != pt.owner.split(' ', 1)[1]:
                    if problems is not None:
                        problems.append((e.key, '%s tree %d: owner differs' % (b.name, ti)))
                    continue
                r = Rec()
                r.entry, r.text, r.unit, r.blob, r.bi, r.ti = e, text, unit, b, bi, ti
                r.pt, r.nodes = pt, nodes
                r.sym = symbolic.sym_nodes(b, nodes)
                r.keep = symbolic.filter_nodes(r.sym)
                r.exc = []
                r.ok, r.pk, r.ak, r.align = compare_tree(pt.nodes, r.sym, r.keep, r.exc)
                yield r


def run(entries, options, show=0, only=None, captures=None, quiet=False):
    db = classdb.load()
    model = gen.Model(db, **options)
    st = Counter()
    mism = Counter()
    examples = defaultdict(list)
    bad_files = {}
    rules_ok = Counter()
    rules_bad = Counter()
    rule_ex = {}
    problems = []
    seen_files = set()
    line_bad = Counter()
    line_ex = []
    callee_exc = []
    if only:
        entries = [e for e in entries if only.lower() in e.key.lower()]
    st['files'] = len(entries)
    for r in iter_trees(entries, model, problems):
        e, b, ti, pt, nodes, sym, keep = r.entry, r.blob, r.ti, r.pt, r.nodes, r.sym, r.keep
        seen_files.add(e.key)
        st['trees'] += 1
        st['nodes (class, all)'] += len(nodes)
        st['nodes (class, filtered)'] += len(keep)
        if captures is not None and r.ok:
            for k, pn in enumerate(pt.nodes):
                if pn[0].endswith('?'):
                    j = r.align[k]
                    act = sym[keep[j]]
                    if act[0] == 'path':
                        act = sym[keep[j + 1]]
                    captures.append((e.key, b.name, ti, pn, act, nodes[keep[j]].line))
        if r.ok:
            excl = set()
            for xpk, pf, af in r.exc:
                excl.update(range(xpk, xpk + pf[2]))
                callee_exc.append({
                    'file': e.key, 'class': b.name, 'tree': ti, 'owner': pt.owner,
                    'line': nodes[keep[r.align[xpk]]].line,
                    'predicted': symbolic.fmt(pt.nodes[xpk + pf[2] - 1]),
                    'actual': symbolic.fmt(sym[keep[r.align[xpk] + af[2] - 1]])})
            if r.exc:
                st['trees identical but for a callee'] += 1
                bad_files[e.key] = bad_files.get(e.key, 0) + 1
            else:
                st['trees identical'] += 1
            st['nodes in identical trees'] += len(keep) - sum(af[2] for _, pf, af in r.exc)
            lines_ok = True
            for k, pl in enumerate(pt.lines):
                if pl is None or k in excl:
                    continue
                st['nodes with a predicted line'] += 1
                j = r.align[k]
                if nodes[keep[j]].line == pl:
                    st['lines identical'] += 1
                else:
                    lines_ok = False
                    role = pt.anchor[k][1]
                    line_bad['%s: %s' % (role, pt.nodes[k][0])] += 1
                    if len(line_ex) < 12:
                        line_ex.append('%s tree %d: %s predicted line %d, class has %d' % (
                            e.key, ti, symbolic.fmt(pt.nodes[k]), pl, nodes[keep[j]].line))
            if lines_ok and not r.exc:
                st['trees identical with lines'] += 1
            for key, ast, a, z in pt.rules:
                if excl and any(x in excl for x in range(a, z)):
                    rules_bad[key] += 1
                    continue
                rules_ok[key] += 1
                n = z - a
                cur = rule_ex.get(key)
                # keep a short, single-line example
                one_line = e.category == 'pair_940' and ast is not None and \
                    r.unit.toks[ast.t0].line == r.unit.toks[max(ast.t0, ast.t1 - 1)].line
                score = (0 if one_line else 1, n if n >= 1 else 99)
                if cur is not None and score[:2] > cur[0][:2]:
                    continue
                if ast is not None:
                    # ... on a short source line that holds a whole statement
                    ll = getattr(r.unit, '_linelen', None)
                    if ll is None:
                        ll = r.unit._linelen = []
                        for x in r.text.split('\n'):
                            x = x.split('//')[0].strip()
                            whole = x.endswith((';', '{', '}', ')'))
                            ll.append((0 if whole else 1, len(x)))
                    score = score + ll[ast.line - 1]
                if cur is None or score < cur[0]:
                    lo = keep[r.align[a]] if a < len(r.align) else None
                    hi = keep[r.align[z - 1]] if z - 1 < len(r.align) and z > a else lo
                    rule_ex[key] = (score, e.key, r.bi, ti, ast.line if ast else 0, lo, hi)
            continue
        for key, ast, a, z in pt.rules:
            rules_bad[key] += 1
        bad_files[e.key] = bad_files.get(e.key, 0) + 1
        pk, ak = r.pk, r.ak
        p = pt.nodes[pk] if pk < len(pt.nodes) else ('<end>', None)
        a = sym[keep[ak]] if ak < len(keep) else ('<end>', None)
        key = (p[0] if p[0] not in ('expr', 'stmt') else '%s %s' % p,
               a[0] if a[0] not in ('expr', 'stmt') else '%s %s' % a)
        mism[key] += 1
        line = nodes[keep[ak]].line if ak < len(keep) else (
            nodes[-1].line if nodes else pt.line)
        ex = (e.key, b.name, ti, pt.owner, line, symbolic.fmt(p), symbolic.fmt(a))
        if len(examples[key]) < 6:
            examples[key].append(ex)
        if show and st['shown'] < show:
            st['shown'] += 1
            print('MISMATCH %s tree %d (%s) line %d: predicted %s, class has %s' % (
                e.key, ti, pt.owner, line, symbolic.fmt(p), symbolic.fmt(a)))
            for d in range(-6, 6):
                k, j = pk + d, ak + d
                if k < 0 or j < 0:
                    continue
                pp = symbolic.fmt(pt.nodes[k]) if k < len(pt.nodes) else ''
                aa = symbolic.fmt(sym[keep[j]]) if j < len(keep) else ''
                al = nodes[keep[j]].line if j < len(keep) else 0
                if not pp and not aa:
                    continue
                print('   %s %4d  %-60s | L%-5d %s' % (
                    '>>' if d == 0 else '  ', k, pp[:60], al, aa[:70]))
    for key, what in problems:
        bad_files[key] = bad_files.get(key, 0) + 1
    st['files identical'] = st['files'] - len(bad_files)
    st['structural problems'] = len(problems)
    result = {
        'counts': dict(st),
        'mismatches': [{'predicted': k[0], 'actual': k[1], 'trees': n,
                        'examples': examples[k]} for k, n in mism.most_common()],
        'bad_files': bad_files,
        'line_mismatches': dict(line_bad),
        'callee_exceptions': callee_exc,
        'line_examples': line_ex,
        'problems': problems,
        'rules': {k: {'held': rules_ok[k], 'not_held': rules_bad[k],
                      'example': rule_ex.get(k, (None,))[1:]}
                  for k in sorted(set(rules_ok) | set(rules_bad))},
    }
    if not quiet:
        for k in ('files', 'files identical', 'structural problems', 'trees',
                  'trees identical', 'trees identical but for a callee',
                  'trees identical with lines', 'nodes (class, all)',
                  'nodes (class, filtered)', 'nodes in identical trees',
                  'nodes with a predicted line', 'lines identical'):
            print('%-36s %d' % (k, st[k]))
        for k, v in sorted(line_bad.items()):
            print('  line mismatch %-30s %d' % (k, v))
        for x in line_ex:
            print('    ' + x)
        for key, what in problems[:20]:
            print('  PROBLEM %s: %s' % (key, what))
        for x in callee_exc:
            print('  CALLEE %s tree %d (%s) line %d: predicted %s, class has %s' % (
                x['file'], x['tree'], x['owner'], x['line'], x['predicted'], x['actual']))
        print('first mismatches by (predicted, actual):')
        for key, n in mism.most_common(60):
            print('  %5d  %-28s %-28s' % (n, key[0], key[1]))
            for ex in examples[key][:3]:
                print('           %s tree %d %s L%d: %s | %s' % (
                    ex[0], ex[2], ex[3], ex[4], ex[5][:70], ex[6][:90]))
    return result


def main(argv):
    options = {}
    show = 0
    only = None
    which = '940'
    out_json = None
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == '--show':
            show = int(argv[i + 1])
            i += 1
        elif a == '--only':
            only = argv[i + 1]
            i += 1
        elif a == '--set':
            which = argv[i + 1]
            i += 1
        elif a == '--json':
            out_json = argv[i + 1]
            i += 1
        elif a == '--opt':
            k, _, v = argv[i + 1].partition('=')
            options[k] = v
            i += 1
        i += 1
    entries = entries_of(which)
    captures = [] if options.get('callee') == 'wild' else None
    res = run(entries, options, show=show, only=only, captures=captures)
    if '--rules' in argv:
        print('rule applications (held / not held):')
        for k, v in res['rules'].items():
            print('  %7d %5d  %s' % (v['held'], v['not_held'], k))
    if captures is not None and not only:
        with open(os.path.join(_HERE, '_captures.pickle'), 'wb') as f:
            pickle.dump(captures, f, pickle.HIGHEST_PROTOCOL)
        print('captures: %d' % len(captures))
    if out_json:
        with open(out_json, 'w', encoding='utf-8') as f:
            json.dump(res, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
