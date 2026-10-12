#!/usr/bin/env python3
"""t4_trees.py - milestone M4: the trees, node for node.

    python -I tests/t4_trees.py [--only TEXT] [-v] [--update]

slrrc.emit_trees() is given the source text, the CorpusProvider (the other
classes of the corpus) and the pack names of the corpus; it returns, per class
and per tree, the node list with symbolic operands (an operand that names a
pool entry is kept by content).  The class file of the source is opened only
to compare.

A tree passes when it has the same number of nodes as the shipped tree and
every node agrees in
  a  the opcode,
  b  the line,
  c  the operand: a number (local index, count, int value, float bits, kind of
     expression, jump offset) must be equal; a pool operand is compared
     through what it names (text; class name; class, member name and
     descriptor; pack file name and local id).
The order of the pool is not part of this milestone (M5).

Every source is compiled in strict mode: a diagnostic of any kind fails all
its trees, so a pass also says that the source holds no construct outside the
proven subset.  (The one exception is written down in harness.py: the two
W_PACK warnings of TimeAttackTest, which must come.)

Float literals: four conversions occur in the shipped classes and which one
the game uses is an open question (notes/compiler_spec.md 1.4).  The oracle
pairs are compared twice: with the setting tests/baseline/m5_float.tsv names
for the file (A for a file it does not list), and with setting A for all.

Besides the oracle pairs:
  older    the pairs written by older builds (setting A)
  stale    the pairs whose source was edited after the compile: a control,
           most of their trees must NOT come out equal
  spike    every source of the corpus, the 635 without a class included:
           the trees equal the ones of spec/spike_compiler.py (the composition
           of the analysis models), node for node, setting A
  locals   the high-water mark of the emitter equals the locals word of the
           method row (slrrc.decl.locals_word) for every method tree
  floats   slrrc.floatlit equals spec/float_closed_forms.py for every float
           literal of the corpus under each of the four settings
  packs    the table of slrrc.packs equals the spellings of the shipped pools

Headline:
  M4 trees         7568/7570 node for node (float setting per file; 7396/7570
  with setting A alone); older builds 154/154; stale 3/18; equal to the spike
  3454/3454 sources
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness              # noqa: E402
import oracle               # noqa: E402  lib
import tufa                 # noqa: E402  lib
import float_closed_forms   # noqa: E402  spec
import spike_compiler       # noqa: E402  spec

import slrrc                        # noqa: E402
from slrrc import decl              # noqa: E402
from slrrc import dump              # noqa: E402
from slrrc import emit              # noqa: E402
from slrrc import floatlit          # noqa: E402
from slrrc import packs             # noqa: E402
from slrrc import parser as ps      # noqa: E402
from slrrc import scanner as sc     # noqa: E402

MODES = 'ABCD'


def float_table():
    """class file -> float setting, from tests/baseline/m5_float.tsv."""
    out = {}
    for line in harness.Baseline('m5_float.tsv').load() or []:
        cls, mode = line.split('\t')
        out[cls] = 'A' if mode == '-' else mode
    return out


def expected_failures():
    """class file -> reason, for the oracle pairs that cannot be reproduced
    from the install (tests/baseline/expected_fail.tsv; the stale pairs in
    that file are not oracle pairs)."""
    out = {}
    for line in harness.Baseline('expected_fail.tsv').load() or []:
        cols = line.split('\t')
        if cols[1] != 'stale':
            out[cols[0]] = cols[3]
    return out


def node_differs(tree, n, blob, b):
    """None when node n of an emitted tree equals node b of a shipped blob,
    else what differs."""
    if n.op != b.op:
        return 'opcode %s / %s' % (emit.OP_NAMES.get(n.op, n.op), emit.OP_NAMES.get(b.op, b.op))
    if n.line != b.slot:
        return 'line %d / %d' % (n.line, b.slot)
    k = n.kind
    if k == emit.K_NONE:
        if b.imm is not None:
            return 'no operand / %r' % (b.imm,)
        return None
    if b.imm is None:
        return 'operand / none'
    if k == emit.K_IMM:
        if n.value != b.imm:
            return 'operand %d / %d' % (n.value, b.imm)
        return None
    o = tree.operands[n.value]
    try:
        if k == emit.K_UTF8:
            theirs = blob.utf8(b.imm)
            mine = o.text
        elif k == emit.K_CLASS:
            theirs = blob.class_name(b.imm)
            mine = o.text
        elif k == emit.K_REF:
            theirs = blob.ref(b.imm)
            mine = (o.text, o.name, o.desc)
        elif k == emit.K_RID:
            theirs = blob.rid(b.imm)
            mine = (o.text, o.number)
        else:
            return 'operand kind %d left in the tree' % k
    except tufa.TufaError as x:
        return 'operand %s / %s' % (dump.operand_text(tree, n), x)
    if mine != theirs:
        return 'operand %r / %r' % (mine, theirs)
    return None


def tree_differs(tree, blob, nodes):
    """None when an emitted tree equals a shipped one, else the first
    difference."""
    k = 0
    for n, b in zip(tree.nodes, nodes):
        why = node_differs(tree, n, blob, b)
        if why is not None:
            return 'node %d line %d: %s' % (k, b.slot, why)
        k += 1
    if len(tree.nodes) != len(nodes):
        return '%d nodes / %d' % (len(tree.nodes), len(nodes))
    return None


def compare(res, blobs):
    """[(blob number, tree number, None or difference)] for every tree of the
    shipped file."""
    out = []
    for bi, blob in enumerate(blobs):
        cb = res.classes[bi] if res.ok and bi < len(res.classes) else None
        for ti, nodes in enumerate(blob.trees):
            if cb is None or ti >= len(cb.trees):
                out.append((bi, ti, 'not emitted'))
            else:
                out.append((bi, ti, tree_differs(cb.trees[ti], blob, nodes)))
        if cb is not None and len(cb.trees) > len(blob.trees):
            out.append((bi, len(blob.trees), '%d trees emitted / %d' % (
                len(cb.trees), len(blob.trees))))
    if res.ok and len(res.classes) > len(blobs):
        out.append((len(blobs), 0, '%d classes emitted / %d' % (len(res.classes), len(blobs))))
    return out


def locals_differ(res):
    """Trees whose high-water mark is not the locals word of their row."""
    bad = []
    for cb in res.classes:
        for ti, (owner, tree) in enumerate(zip(cb.rows.owners, cb.trees)):
            want = 0 if owner.is_field else decl.locals_word(owner.node)
            if tree.max_locals != want:
                bad.append('%s tree %d: high-water mark %d, locals word %d' % (
                    cb.info.name, ti, tree.max_locals, want))
    return bad


def census(res, c):
    """Constructs the corpus tests on few occurrences (compiler_spec.md 5)."""
    ast = res.ast
    for n in ast.nodes:
        k = n.kind
        if k == ps.DO:
            c['do'] = c.get('do', 0) + 1
        elif k == ps.CONTINUE:
            c['continue'] = c.get('continue', 0) + 1
        elif k == ps.COND:
            c['?:'] = c.get('?:', 0) + 1
        elif k == ps.DEFAULT_LABEL:
            c['default'] = c.get('default', 0) + 1
        elif k == ps.CTOR_CALL and ast.toks[n.tok0].kind == sc.KW_THIS:
            c['this(..)'] = c.get('this(..)', 0) + 1
        elif k == ps.LITERAL and ast.toks[n.tok0].kind in (sc.KW_TRUE, sc.KW_FALSE):
            c['true / false'] = c.get('true / false', 0) + 1
        elif k == ps.BINARY or k == ps.ASSIGN or k == ps.UNARY:
            op = ast.toks[n.op_tok].kind
            if op in (sc.P_XOR, sc.P_BITNOT, sc.P_AND_ASSIGN, sc.P_XOR_ASSIGN, sc.P_SHL,
                      sc.P_SHR, sc.P_DIV_ASSIGN):
                t = sc.FIXED_TEXT[op]
                c[t] = c.get(t, 0) + 1
        elif k == ps.FOR and n.a is not None and not n.items:
            c['for with condition, without update'] = \
                c.get('for with condition, without update', 0) + 1
        elif k == ps.METHOD_DECL and (n.flags & ps.ACC_STATIC) and decl.has_code(n):
            c['static method with code'] = c.get('static method with code', 0) + 1
        elif k == ps.RETURN and n.a is None:
            c['return;'] = c.get('return;', 0) + 1


def spike_trees(env, text, name):
    """The trees of the composition spike: [tufa.Blob], or the exception."""
    try:
        data, info = spike_compiler.compile_source(text, env, 'A', 'table', name=name)
        return tufa.parse(data), None
    except Exception as x:      # noqa: the spike raises for everything
        return None, '%s: %s' % (type(x).__name__, str(x)[:120])


def check_floats(texts):
    """Float literal texts whose bits differ from spec/float_closed_forms.py."""
    bad = []
    for tx in sorted(texts):
        ip, fp = sc.float_digits(tx)
        for mode in MODES:
            mine = floatlit.float_bits(mode, ip, fp)
            ref = float_closed_forms.CONV[mode](tx)
            if mine != ref:
                bad.append('float %s setting %s: %08X / %08X' % (tx, mode, mine, ref))
    return bad


def check_packs(prov):
    """The table of the package against the spellings of the shipped pools."""
    bad = []
    mine = sorted(packs.TABLE_940)
    theirs = sorted(prov.pack_spellings.values())
    if mine != theirs:
        bad.append('pack table: %d entries, the shipped pools spell %d packs; first '
                   'difference %r' % (len(mine), len(theirs),
                                      sorted(set(mine) ^ set(theirs))[:1]))
    return bad


def run(opts):
    oc = harness.Outcome('M4 trees')
    prov = harness.provider()
    pk = harness.packs()
    modes = float_table()
    expected = expected_failures()
    expected_seen = []
    env = spike_compiler.Env()
    oracle_keys = set(e.key for e in harness.oracle_pairs())
    older_keys = set(e.key for e in harness.older_pairs())
    c = {}
    thin = {}
    float_texts = set()
    passed = []             # oracle trees, float setting per file
    passed_other = []       # trees of older-build and stale pairs
    passed_spike = []       # sources equal to the spike
    n_sources = 0
    kinds = {}

    def count(key, n=1):
        c[key] = c.get(key, 0) + n

    def emit_trees(text, name, mode):
        return slrrc.emit_trees(text, name, prov, pk,
                                slrrc.Options(strict=True, float_mode=mode))

    for e in harness.selected(harness.all_sources(), opts):
        n_sources += 1
        text = harness.load_source(e)
        res = emit_trees(text, e.source, 'A')
        odd = harness.surprises(e.source, res.diagnostics)
        if not res.ok or odd:
            oc.failures.append('%s: %s' % (e.source, odd[0] if odd else 'not ok'))
        if res.ok:
            for x in locals_differ(res):
                oc.failures.append('%s: %s' % (e.source, x))
                count('locals differ')
            src = res.ast.source
            for t in res.ast.toks:
                if t.kind == sc.T_FLOAT:
                    float_texts.add(src[t.pos:t.pos + t.length])
        clean = res.ok and not odd
        # ---- the spike, every source
        blobs, why = spike_trees(env, text, e.source)
        if blobs is None:
            oc.failures.append('%s: the spike does not compile it: %s' % (e.source, why))
        else:
            diffs = compare(res, blobs) if clean else [(0, 0, 'diagnostics')]
            if clean and len(blobs) != len(res.classes):
                diffs.append((0, 0, '%d classes / spike %d' % (len(res.classes), len(blobs))))
            bad = [d for d in diffs if d[2] is not None]
            count('spike trees', sum(len(b.trees) for b in blobs))
            count('spike trees ok', len(diffs) - len(bad) if clean else 0)
            if not bad:
                passed_spike.append(e.source)
            else:
                bi, ti, why = bad[0]
                oc.failures.append('%s: differs from the spike: blob %d tree %d: %s' % (
                    e.source, bi, ti, why))
        if not e.is_pair:
            continue
        # ---- the shipped class
        if e.key in oracle_keys:
            grp = 'oracle'
        elif e.key in older_keys:
            grp = 'older'
        else:
            grp = 'stale'
        shipped = oracle.load_class(e)
        if grp == 'oracle' and clean:
            census(res, thin)
        diffs_a = compare(res, shipped) if clean else \
            [(bi, ti, 'diagnostics') for bi, b in enumerate(shipped) for ti in range(len(b.trees))]
        mode = modes.get(e.cls, 'A')
        diffs = diffs_a
        if grp == 'oracle' and mode != 'A' and clean:
            res2 = emit_trees(text, e.source, mode)
            diffs = compare(res2, shipped) if res2.ok and not harness.surprises(
                e.source, res2.diagnostics) else diffs_a
        if grp == 'oracle':
            count('mode ' + mode)
        for (bi, ti, why), (_, _, why_a) in zip(diffs, diffs_a):
            count(grp + ' trees')
            item = '%s#%d#%d' % (e.cls, bi, ti)
            if why_a is None:
                count(grp + ' trees ok under A')
            if why is None:
                count(grp + ' trees ok')
                if grp == 'oracle':
                    passed.append(item)
                    if clean and bi < len(res.classes) and ti < len(res.classes[bi].rows.owners):
                        o = res.classes[bi].rows.owners[ti]
                        kd = 'field initialisers' if o.is_field else (
                            'constructors' if o.node.flags & ps.F_CTOR else 'methods')
                        kinds[kd] = kinds.get(kd, 0) + 1
                else:
                    passed_other.append(item)
            elif grp == 'oracle' and e.cls in expected and clean:
                expected_seen.append('%s blob %d tree %d: %s' % (e.cls, bi, ti, why))
            elif grp != 'stale':
                oc.failures.append('%s blob %d tree %d: %s' % (e.cls, bi, ti, why))
    for src, diags in prov.failed:
        d = diags[0]
        oc.failures.append('%s (declared for the provider): line %d: %s %s' % (
            src, d.line, d.code, d.text))
    bad_floats = check_floats(float_texts)
    bad_packs = check_packs(prov)
    oc.failures.extend(bad_floats[:20])
    oc.failures.extend(bad_packs)

    def g(key):
        return c.get(key, 0)

    oc.text = ('%d/%d node for node (float setting per file; %d/%d with setting A alone); '
               'older builds %d/%d; stale %d/%d; equal to the spike %d/%d sources' % (
                   g('oracle trees ok'), g('oracle trees'),
                   g('oracle trees ok under A'), g('oracle trees'),
                   g('older trees ok'), g('older trees'),
                   g('stale trees ok'), g('stale trees'),
                   len(passed_spike), n_sources))
    oc.ok = (g('oracle trees') == 7570 and g('oracle trees ok') == 7568 and
             g('oracle trees ok under A') == 7396 and
             g('older trees ok') == g('older trees') == 154 and
             g('stale trees') == 18 and g('stale trees ok') == 3 and
             len(passed_spike) == n_sources == 3454 and
             g('locals differ') == 0 and not bad_floats and not bad_packs and
             not prov.failed)
    oc.passed['m4.pass'] = passed
    oc.passed['m4_other.pass'] = passed_other
    oc.passed['m4_spike.pass'] = passed_spike
    for x in expected_seen:
        oc.notes.append('expected (compiled against another version of the class it '
                        'calls): %s' % x)
    oc.notes.append('oracle trees that pass, by owner: %s' % ', '.join(
        '%s %d' % (k, v) for k, v in sorted(kinds.items())))
    oc.notes.append('oracle files by float setting: %s' % ', '.join(
        '%s %d' % (m, g('mode ' + m)) for m in MODES))
    oc.notes.append('trees equal to the spike: %d/%d; float literal texts checked under '
                    'four settings: %d; pack table: %d entries' % (
                        g('spike trees ok'), g('spike trees'), len(float_texts),
                        len(packs.TABLE_940)))
    oc.notes.append('rules the oracle sources exercise fewer than ten times: %s' % (
        ', '.join('%s %d' % (k, v) for k, v in sorted(thin.items()) if v < 10) or 'none'))
    oc.notes.append('other thin rules: %s' % ', '.join(
        '%s %d' % (k, v) for k, v in sorted(thin.items()) if v >= 10))
    return oc


if __name__ == '__main__':
    sys.exit(harness.main_single(run, sys.argv[1:]))
