#!/usr/bin/env python3
"""check_tail.py - the pool model checked WITHOUT a source, on every blob of
the corpus (2,993), so that the 102 class files that ship without a source
(the runtime library under system/scripts among them) are covered too.

What can be said about a pool from the class file alone:

  T1  seed      pool starts with: class name utf8, class entry, superclass
                utf8, class entry, then utf8 + class entry per interface, in
                the order of the CLSS record
  T2  types     between the seed and the first member name stand only utf8
                entries spelled like a type descriptor; every type column of
                a FILD row points there
  T3  names     the names region is one contiguous block of utf8 entries,
                each named by a FILD / MTHD row (name or descriptor column),
                and nothing else is in it
  T4  names     the block is a merge of the four row vectors that keeps each
                vector in row order, a method giving descriptor then name
  T5  names     rows that own a tree appear in tree order (= source order)
                across the vectors
  T6  tail      everything behind the names region is exactly what steps 4
                (resolve) and 5 (emit) of the model intern, given the trees
  T7  walk      no member ref follows an unresolved name inside a path
  T8  types     the types region is what step 2 interns: there is an order of
                the members (each row vector in row order, rows with a tree
                in tree order) for which  field type, then the types of its
                initialiser / parameter types, return type, then the types
                of the body (a local's type behind its initialiser)  gives
                exactly the region, followed by "LI;" per interface and, in
                a blob that is not the first of its file, "LC;"

    python -I check_tail.py
"""
import os
import re
import sys
from collections import Counter, OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'lib')))
sys.path.insert(0, HERE)

import oracle      # noqa: E402
import tufa        # noqa: E402
import poolmodel   # noqa: E402

OUT = os.path.join(HERE, 'out')
NONE = tufa.NONE
RULES = ('T1', 'T2', 'T3', 'T4', 'T5', 'T6', 'T7', 'T8')
# "L;" (empty class name) is what an unresolvable class name compiles to
# (java.util.Thesaurus, field curlang)
TYPE_RE = re.compile(r'^\[*(?:[IFV]|L(?:[A-Za-z_$][\w$]*(?:\.[A-Za-z_$][\w$]*)*)?;)$')


VARIANT = 'default'     # set by variants(): one rule of T8 replaced


def tree_type_events(cc, nodes):
    """Type strings a tree made the parser intern, in the order of step 2:
    carriers in node order, except that the type of a local with an
    initialiser comes behind the carriers of that initialiser."""
    out = []
    pending = []
    code = [tufa.OP_FLAGS[x.op] for x in nodes]
    n = len(nodes)
    i = 0
    while i < n:
        c = code[i]
        if c == 0x1002 and i + 1 < n and code[i + 1] == 0x1006:
            T = cc[nodes[i + 1].imm][1]
            if i + 2 < n and code[i + 2] == 0x002C and \
                    VARIANT != 'local type before its initialiser':
                pending.append(T)
                i += 3
            else:
                out.append(T)
                i += 2
            continue
        if c == 0x002D and pending:
            out.append(pending.pop())
        elif c == 0x1006:
            out.append(cc[nodes[i].imm][1])
        i += 1
    out.extend(reversed(pending))
    return out


def dedupe(seq, seen):
    out = []
    s = set()
    for x in seq:
        if x not in seen and x not in s:
            s.add(x)
            out.append(x)
    return out


def check_types_region(b, cc, region, blob_index):
    """T8.  Returns (ok, detail)."""
    vectors = []
    for rows, is_m in ((b.static_fields, False), (b.instance_fields, False),
                       (b.static_methods, True), (b.instance_methods, True)):
        v = []
        for r in rows:
            tree = r.tree if is_m else r.init
            body = tree_type_events(cc, b.trees[tree]) if tree != NONE else []
            if is_m:
                d = b.utf8(r.desc)
                ev = poolmodel.split_params(d)
                ret = d[d.index(')') + 1:]
                if ret:
                    if VARIANT == 'return type before the parameters':
                        ev.insert(0, ret)
                    elif VARIANT == 'return type after the body':
                        body = body + [ret]
                    else:
                        ev.append(ret)
                ev = ev + body
            elif VARIANT == 'field type after its initialiser':
                ev = body + [b.utf8(r.type)]
            else:
                ev = [b.utf8(r.type)] + body
            v.append((ev, None if tree == NONE else tree))
        vectors.append(v)
    end = ['L' + i + ';' for i in b.interface_names]
    if VARIANT == 'no interface types at the end of the body':
        end = []
    own = ['L' + b.name + ';']
    if VARIANT == 'own type at the end of every body':
        end = end + own
    elif VARIANT == 'no own type at the end of a later class':
        pass
    elif blob_index > 0:
        if VARIANT == 'own type before the interface types':
            end = own + end
        else:
            end = end + own
    trees_sorted = sorted(t for v in vectors for ev, t in v if t is not None)
    tpos = {t: k for k, t in enumerate(trees_sorted)}
    lens = tuple(len(v) for v in vectors)
    # prefix unions per vector
    pre = []
    for v in vectors:
        acc = [frozenset()]
        for ev, t in v:
            acc.append(acc[-1] | frozenset(ev))
        pre.append(acc)
    # number of tree-bearing members consumed per vector prefix
    tcount = []
    for v in vectors:
        acc = [0]
        for ev, t in v:
            acc.append(acc[-1] + (t is not None))
        tcount.append(acc)
    failed = set()
    stack = [(0, 0, 0, 0)]
    best = (0, None)
    steps = 0
    while stack:
        state = stack.pop()
        if state in failed:
            continue
        steps += 1
        if steps > 400000:
            return None, 'search gave up'
        seen = pre[0][state[0]] | pre[1][state[1]] | pre[2][state[2]] | pre[3][state[3]]
        pos = len(seen & frozenset(region)) if False else None
        # position in the region = number of region strings already seen
        pos = 0
        while pos < len(region) and region[pos] in seen:
            pos += 1
        if any(x in seen for x in region[pos:]):
            failed.add(state)
            continue
        if state == lens:
            tail = region[pos:]
            want = dedupe(end, seen)
            if tail == want:
                return True, ''
            best = max(best, (pos, 'end of body: region has %r, model %r' % (tail, want)),
                       key=lambda x: x[0])
            failed.add(state)
            continue
        done_trees = sum(tcount[v][state[v]] for v in range(4))
        moved = False
        nxt = []
        for v in range(4):
            h = state[v]
            if h >= lens[v]:
                continue
            ev, t = vectors[v][h]
            if t is not None and tpos[t] != done_trees:
                continue
            new = dedupe(ev, seen)
            if region[pos:pos + len(new)] == new:
                ns = list(state)
                ns[v] += 1
                nxt.append(tuple(ns))
            else:
                if pos >= best[0]:
                    best = (pos, 'at region[%d]: member wants %r, region has %r' % (
                        pos, new[:4], region[pos:pos + 4]))
        if not nxt:
            failed.add(state)
            continue
        failed.add(state)       # visited
        stack.extend(nxt)
    return False, best[1] or 'no order of the members gives the region'


def check_blob(b, blob_index=0):
    """-> (dict rule -> True/False/None, details, model)"""
    res = OrderedDict((k, None) for k in RULES)
    det = []
    cc = poolmodel.content(b)
    # ---- T1 seed
    seed = [('utf8', b.name), ('class', b.name)]
    if b.clss.super != NONE:
        seed += [('utf8', b.super_name), ('class', b.super_name)]
    for i in b.interface_names:
        seed += [('utf8', i), ('class', i)]
    res['T1'] = cc[:len(seed)] == seed
    if not res['T1']:
        det.append('seed %r' % (cc[:len(seed)],))
    ns = len(seed)
    # ---- rows
    vectors = []
    for rows, is_m in ((b.static_fields, False), (b.instance_fields, False),
                       (b.static_methods, True), (b.instance_methods, True)):
        v = []
        for r in rows:
            if is_m:
                v.append(([r.desc, r.name], r.tree))
            else:
                v.append(([r.name], r.init))
        vectors.append(v)
    R = set()
    for v in vectors:
        for ents, _ in v:
            R.update(ents)
    typecols = set(f.type for _, _, f in b.fields())
    if not R:
        # no member at all: everything behind the seed is a type string
        rest = cc[ns:]
        res['T2'] = all(e[0] == 'utf8' and TYPE_RE.match(e[1]) for e in rest)
        if not res['T2']:
            det.append('entries behind the seed of a class without members: %r' % (rest[:4],))
        ok, why = check_types_region(b, cc, [e[1] for e in rest], blob_index)
        res['T8'] = ok
        if not ok:
            det.append('T8 ' + why)
        return res, det, None
    hi = max(R)
    lo = hi
    while lo - 1 >= ns and (lo - 1) in R:
        lo -= 1
    # a type string can double as the first member name only by accident;
    # the block must not swallow type strings: shrink it while its first
    # entry is a type column
    while lo <= hi and lo in typecols:
        lo += 1
    # ---- T3: contiguous block, only row names
    stray = sorted(i for i in R if i < lo)
    res['T3'] = all(cc[i][0] == 'utf8' for i in range(lo, hi + 1)) and not stray
    if stray:
        # a member name that equals a string interned earlier (a type
        # string or a seed name) is legitimate sharing; anything else is not
        res['T3'] = all(cc[i][0] == 'utf8' for i in stray)
        det.append('row names shared with entries in front of the block: %r'
                   % ([cc[i][1] for i in stray],))
    # ---- T2: types region
    region = cc[ns:lo]
    res['T2'] = all(e[0] == 'utf8' and TYPE_RE.match(e[1]) for e in region) and \
        all(ns <= i < lo for i in typecols)
    if not res['T2']:
        det.append('types region: %r' % ([e for e in region if not (e[0] == 'utf8' and TYPE_RE.match(e[1]))][:4],))
    # ---- T8: the types region is what step 2 interns
    ok, why = check_types_region(b, cc, [e[1] for e in region], blob_index)
    res['T8'] = ok
    if not ok:
        det.append('T8 ' + why)
    # ---- T4 / T5: the block is a merge of the vectors
    seen = set(range(lo))
    heads = [0, 0, 0, 0]
    first_pos = {}          # (vector, row) -> index of its first new entry
    ok4 = True
    for idx in range(lo, hi + 1):
        placed = False
        # drop rows that contribute nothing new
        for vi, v in enumerate(vectors):
            while heads[vi] < len(v) and all(x in seen for x in v[heads[vi]][0]):
                heads[vi] += 1
        cand = []
        for vi, v in enumerate(vectors):
            if heads[vi] >= len(v):
                continue
            ents, tree = v[heads[vi]]
            nxt = next(x for x in ents if x not in seen)
            if nxt == idx:
                cand.append((tree, vi))
        if cand:
            # two heads can ask for the same new string (two members of one
            # name in different vectors): the one declared first takes it
            tree, vi = min(cand)
            seen.add(idx)
            first_pos.setdefault((vi, heads[vi]), idx)
            placed = True
        if not placed:
            ok4 = False
            det.append('names block entry %d (%r) is not the next new entry of '
                       'any vector head' % (idx, cc[idx]))
            seen.add(idx)
    res['T4'] = ok4
    order = []
    for vi, v in enumerate(vectors):
        for ri, (ents, tree) in enumerate(v):
            if tree != NONE and (vi, ri) in first_pos:
                order.append((tree, first_pos[(vi, ri)]))
    order.sort()
    res['T5'] = all(order[k][1] < order[k + 1][1] for k in range(len(order) - 1))
    if not res['T5']:
        det.append('rows with trees are not in tree order in the names block')
    # ---- T6: the tail
    m = poolmodel.Model.from_blob(b)
    m.run_tail(hi + 1)
    res['T6'] = m.pool.entries == cc
    if not res['T6']:
        d = poolmodel.first_diff(m.pool.entries, cc)
        a = cc[d] if d < len(cc) else None
        p = m.pool.entries[d] if d < len(m.pool.entries) else None
        det.append('tail differs at %d: actual %r, model %r (%s)' % (
            d, a, p, m.pool.phase[d] if d < len(m.pool.phase) else 'end'))
    res['T7'] = m.stats.get('member ref after an unresolved name', 0) == 0
    return res, det, m


VARIANTS = ('return type before the parameters', 'return type after the body',
            'local type before its initialiser', 'field type after its initialiser',
            'no interface types at the end of the body',
            'no own type at the end of a later class',
            'own type at the end of every body',
            'own type before the interface types')


def variants(out):
    """T8 with one rule replaced: how many blobs then fail (the source-free
    counterpart of check_alternatives.py)."""
    global VARIANT
    idx = {e.cls: e for e in oracle.iter_entries() if e.cls}
    files = list(oracle.iter_class_files())
    out('T8 with one rule replaced: blobs whose types region is then not reproduced')
    out('   %-46s %10s %24s' % ('variant', 'all blobs', 'blobs without a source'))
    for v in VARIANTS:
        VARIANT = v
        bad = badn = 0
        ex = []
        for rel, blobs in files:
            e = idx.get(rel)
            nosrc = e is None or e.source is None
            for bi, b in enumerate(blobs):
                cc = poolmodel.content(b)
                ns = 2 + (2 if b.clss.super != NONE else 0) + 2 * len(b.clss.interfaces)
                R = set()
                for _, _, f in b.fields():
                    R.add(f.name)
                for _, _, m in b.methods():
                    R.update((m.name, m.desc))
                typecols = set(f.type for _, _, f in b.fields())
                if R:
                    hi = max(R)
                    lo = hi
                    while lo - 1 >= ns and (lo - 1) in R:
                        lo -= 1
                    while lo <= hi and lo in typecols:
                        lo += 1
                else:
                    lo = len(cc)
                ok, why = check_types_region(b, cc, [x[1] for x in cc[ns:lo]], bi)
                if ok is not True:
                    bad += 1
                    if nosrc:
                        badn += 1
                        if len(ex) < 2:
                            ex.append('%s#%d' % (rel.rsplit('/', 1)[1], bi))
        out('   %-46s %10d %24d   %s' % (v, bad, badn, ', '.join(ex)))
    VARIANT = 'default'


def twins(out):
    """Classes that ship twice: as a later class of a class file with
    several classes and as the only class of a file of their own (nine
    dialog classes of system/scripts/render/osd/dialog).  Where the two were
    compiled from the same code the pools must differ by exactly the own
    type "LC;" that the end of a later class body interns."""
    by_name = {}
    for rel, blobs in oracle.iter_class_files():
        for bi, b in enumerate(blobs):
            by_name.setdefault(b.name, []).append((rel, bi, len(blobs), b))
    out('classes shipped both as a later class of a file and as a file of their own:')
    n = same = 0
    for name in sorted(by_name):
        v = by_name[name]
        later = [x for x in v if x[1] > 0]
        alone = [x for x in v if x[2] == 1]
        if not later or not alone:
            continue
        n += 1
        a = poolmodel.content(later[0][3])
        c = poolmodel.content(alone[0][3])
        own = ('utf8', 'L' + name + ';')
        if [e for e in a if e != own] == c and own in a:
            same += 1
            out('   %-46s pool of %d entries = the stand-alone pool of %d + "LC;" at [%d]' % (
                name, len(a), len(c), a.index(own)))
        else:
            out('   %-46s compiled from different code (%d / %d entries), not comparable' % (
                name, len(a), len(c)))
    out('   %d such classes; %d comparable, all differ by the own type alone' % (n, same))


def unreferenced(out):
    """Every pool entry of the corpus that nothing refers to (2,537 entries,
    foundation.md rule P2), by where the model says it comes from."""
    import stats_decl
    c = Counter()
    nb = 0
    for rel, blobs in oracle.iter_class_files():
        for bi, b in enumerate(blobs):
            ref = stats_decl.referenced(b)
            un = [i for i in range(len(b.cons)) if i not in ref]
            if not un:
                continue
            nb += 1
            cc = poolmodel.content(b)
            R = set()
            for _, _, f in b.fields():
                R.add(f.name)
            for _, _, m in b.methods():
                R.update((m.name, m.desc))
            hi = max(R) if R else len(cc) - 1
            m = poolmodel.Model.from_blob(b)
            m.run_tail(hi + 1)
            for i in un:
                kind, s = cc[i][0], cc[i][1]
                if i > hi:
                    c['steps 4-5: %s (%s)' % (m.pool.phase[i], kind)] += 1
                elif kind != 'utf8':
                    c['UNEXPLAINED: %s entry in front of the names' % kind] += 1
                elif s == 'V':
                    c['step 2: "V" (a return type)'] += 1
                elif s in ('I', 'F'):
                    c['step 2: "I" / "F" (parameter or return type)'] += 1
                elif s.startswith('['):
                    c['step 2: array type (parameter or return type)'] += 1
                elif s == 'L' + b.name + ';':
                    c['step 2: "LC;" of the own class'] += 1
                elif s.startswith('L') and s[1:-1] in b.interface_names:
                    c['step 2: "LI;" of an implemented interface'] += 1
                elif s.startswith('L'):
                    c['step 2: class type (parameter or return type)'] += 1
                else:
                    c['UNEXPLAINED: %r' % s] += 1
    out('entries that nothing refers to: %d in %d blobs' % (sum(c.values()), nb))
    for k in sorted(c):
        out('   %-62s %6d' % (k, c[k]))


def main(argv):
    idx = {}
    for e in oracle.iter_entries():
        if e.cls:
            idx[e.cls] = e
    cats = OrderedDict((k, Counter()) for k in (
        'all blobs', 'blobs of pairs', 'blobs without a source',
        'blobs without a source, Build 940'))
    tot = Counter()
    fails = []
    walk = Counter()
    extra = Counter()
    for rel, blobs in oracle.iter_class_files():
        e = idx.get(rel)
        nosrc = e is None or e.source is None
        for bi, b in enumerate(blobs):
            res, det, m = check_blob(b, bi)
            if m is not None:
                for k, v in m.stats.items():
                    walk[k] += v
            keys = ['all blobs', 'blobs without a source' if nosrc else 'blobs of pairs']
            if nosrc and b.build == tufa.BUILD_940:
                keys.append('blobs without a source, Build 940')
            if nosrc:
                extra['blobs'] += 1
                if bi > 0:
                    extra['blobs that are not first in their file'] += 1
                    if ('utf8', 'L' + b.name + ';') in poolmodel.content(b):
                        extra['... holding "L<own class>;"'] += 1
                if b.interface_names:
                    extra['blobs that implement an interface'] += 1
            for key in keys:
                tot[key] += 1
                for r, v in res.items():
                    if v is None:
                        continue
                    cats[key][r + ' n'] += 1
                    cats[key][r + ' ok'] += bool(v)
            if any(v is False for v in res.values()):
                fails.append((rel, bi, b.name, [r for r, v in res.items() if v is False], det))
    lines = []
    out = lines.append
    for key, c in cats.items():
        out('%s: %d' % (key, tot[key]))
        for r in RULES:
            out('   %s  holds for %5d of %5d' % (r, c[r + ' ok'], c[r + ' n']))
    out('blobs without a source:')
    for k in sorted(extra):
        out('   %-46s %7d' % (k, extra[k]))
    out('counters of the resolution walk over all blobs:')
    for k in sorted(walk):
        out('   %-46s %7d' % (k, walk[k]))
    out('blobs that fail a rule: %d' % len(fails))
    for rel, bi, name, bad, det in fails:
        out('   %s#%d %s: %s' % (rel, bi, name, ' '.join(bad)))
        for d in det[:4]:
            out('      ' + d[:400])
    variants(out)
    twins(out)
    unreferenced(out)
    text = '\n'.join(lines) + '\n'
    sys.stdout.write(text)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'check_tail.txt'), 'w', encoding='utf-8') as f:
        f.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
