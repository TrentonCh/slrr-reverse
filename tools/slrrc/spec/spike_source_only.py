#!/usr/bin/env python3
"""spike_source_only.py - compile the 635 sources that have no class file with
the composition spike, write the class files, and check them without an oracle.

    python -I spike_source_only.py [--out DIR]

Output directory (default spec/out/source_only_classes): one class file per
source at <relative class directory>/<Name>.class, the layout a cache
directory of the host would have.  Nothing is written into the install.

Checks, per class file (there is no shipped class to compare with):
    strict   lib/tufa.py reads it back, validate() passes, it serialises to
             the same bytes
    stmt     every tree splits into the statement forms of notes/statements.md
             (analysis/stmt/destruct.py: jump targets, release indices,
             epilogue, locals word)
    expr     every tree is accepted by the expression shape grammar of
             notes/expressions.md (analysis/expr/decomp.py)
    twin     where a sibling in the same directory HAS a shipped class and its
             source differs from this one only in literals and names, the two
             class files have the same node shape (opcode and line sequence)
"""
import hashlib
import os
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import spike_compiler as sc   # noqa: E402  (sets up sys.path)
import oracle                 # noqa: E402
import tufa                   # noqa: E402
import disasm                 # noqa: E402
import destruct               # noqa: E402
import decomp                 # noqa: E402
import srclex                 # noqa: E402
import check_opcodes          # noqa: E402


def tree_info(blob):
    out = {}
    for st, i, f in blob.fields():
        if f.init != tufa.NONE:
            out[f.init] = ('field', False, st, 0, True)
    for st, i, m in blob.methods():
        if m.tree == tufa.NONE:
            continue
        name = blob.utf8(m.name)
        params, ret = disasm.split_descriptor(blob.utf8(m.desc))
        out[m.tree] = ('method', name == '<init>', st, len(params), ret in ('', 'V'))
    return out


def shape_key(text):
    """Token kinds of a source with identifiers and literals blanked: two
    sources with the same key differ only in names and literal values."""
    out = []
    for t in srclex.tokenize(text):
        if t.kind in ('id', 'int', 'float', 'str', 'rid'):
            out.append(t.kind)
        else:
            out.append(t.text)
    return tuple(out)


def node_shape(blobs):
    return [[[(n.op,) for n in tree] for tree in b.trees] for b in blobs]


# operands that depend on the names and literals of the source, not on its shape
POOL_OR_LITERAL = (0x1006, 0x4009, 0x100e, 0x4012, 0x1019, 0x101a, 0x101b, 0x401c,
                   0x4025, 0x4026, 0x200a, 0x100b)


def node_shape_strict(blobs):
    """Opcode, line and every operand that the shape of the source fixes
    (local index, count, jump offset, expression kind)."""
    out = []
    for b in blobs:
        for tree in b.trees:
            out.append([(n.op, n.slot, None if tufa.OP_FLAGS[n.op] in POOL_OR_LITERAL else n.imm)
                        for n in tree])
    return out


def line_key(text):
    return tuple(t.line for t in srclex.tokenize(text))


def simulate_tree(blob, nodes, kind, is_static, nargs, rules, where):
    """The stack simulation of lib/check_opcodes.py (rules X1 to X6) on one tree."""
    codes = [tufa.OP_FLAGS[x.op] for x in nodes]
    eaten = disasm.consumed_nodes(nodes)
    base = (nargs + (0 if is_static else 1)) if kind == 'method' else 0
    check_opcodes.simulate(nodes, codes, eaten, base, rules, where, defaultdict(Counter))


def main(argv):
    out_dir = argv[argv.index('--out') + 1] if '--out' in argv else \
        os.path.join(_HERE, 'out', 'source_only_classes')
    env = sc.Env()
    c = Counter()
    fails = defaultdict(list)
    shas = []
    xrules = tuple(check_opcodes.Rule(r, r) for r in ('X1', 'X2', 'X3', 'X4', 'X5', 'X6'))
    # shipped siblings by directory and shape key
    sib = defaultdict(dict)
    sib_strict = defaultdict(dict)
    for e in oracle.iter_oracle_pairs():
        txt = oracle.load_source(e)
        k = shape_key(txt)
        sib[e.dir].setdefault(k, e)
        sib_strict[e.dir].setdefault((k, line_key(txt)), e)
    for e in oracle.iter_entries('source_only'):
        c['sources'] += 1
        text = oracle.load_source(e)
        try:
            data, info = sc.compile_source(text, env, 'A', name=e.source)
        except Exception as x:   # noqa
            fails['compile'].append('%s: %s' % (e.source, x))
            continue
        c['compiled'] += 1
        path = os.path.join(out_dir, e.dir.replace('/', os.sep), e.name + '.class')
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(data)
        c['bytes'] += len(data)
        shas.append('%s  %s/%s.class' % (hashlib.sha256(data).hexdigest(), e.dir, e.name))
        try:
            blobs = tufa.parse(data)
            for b in blobs:
                tufa.validate(b)
            assert tufa.serialise(blobs) == data
            c['strict'] += 1
        except Exception as x:   # noqa
            fails['strict'].append('%s: %s' % (e.source, x))
            continue
        ok_s = ok_e = True
        for b in blobs:
            c['classes'] += 1
            ti = tree_info(b)
            for t, nodes in enumerate(b.trees):
                c['trees'] += 1
                c['nodes'] += len(nodes)
                if not nodes:
                    c['empty trees'] += 1
                    continue
                kind, is_ctor, st, nargs, ret_void = ti[t]
                try:
                    d = destruct.Destruct(b, nodes, kind, is_ctor, st, nargs, ret_void)
                    d.run()
                    c['stmt trees'] += 1
                except Exception as x:   # noqa
                    ok_s = False
                    fails['stmt'].append('%s tree %d: %s' % (e.source, t, x))
                try:
                    decomp.decompile(b, nodes)
                    c['expr trees'] += 1
                except Exception as x:   # noqa
                    ok_e = False
                    fails['expr'].append('%s tree %d: %s' % (e.source, t, x))
                simulate_tree(b, nodes, kind, st, nargs, xrules, '%s tree %d' % (e.source, t))
        c['stmt'] += ok_s
        c['expr'] += ok_e
        tw = sib.get(e.dir, {}).get(shape_key(text))
        if tw is not None:
            c['with twin'] += 1
            theirs = oracle.load_class(tw)
            if node_shape(blobs) == node_shape(theirs):
                c['twin same opcodes'] += 1
            else:
                fails['twin'].append('%s against %s' % (e.source, tw.cls))
        tw = sib_strict.get(e.dir, {}).get((shape_key(text), line_key(text)))
        if tw is not None:
            # same tokens on the same lines: opcode, line and every operand
            # that does not name a literal or a pool entry must be equal
            c['with strict twin'] += 1
            theirs = oracle.load_class(tw)
            if node_shape_strict(blobs) == node_shape_strict(theirs):
                c['strict twin equal'] += 1
            else:
                fails['strict twin'].append('%s against %s' % (e.source, tw.cls))
    print('sources without a class file: %d' % c['sources'])
    print('  compiled                                   %d (%d classes, %d trees of which %d empty, %d nodes, %d bytes)'
          % (c['compiled'], c['classes'], c['trees'], c['empty trees'], c['nodes'], c['bytes']))
    print('  strict reader, validate, round trip        %d of %d files' % (c['strict'], c['compiled']))
    print('  every tree splits into statement forms     %d of %d files (%d of %d trees with nodes)'
          % (c['stmt'], c['strict'], c['stmt trees'], c['trees'] - c['empty trees']))
    print('  every tree fits the expression shapes      %d of %d files (%d of %d trees with nodes)'
          % (c['expr'], c['strict'], c['expr trees'], c['trees'] - c['empty trees']))
    print('  stack simulation of lib/check_opcodes.py     %s'
          % ', '.join('%s %d of %d' % (r.rid, r.ok, r.total) for r in xrules))
    for r in xrules:
        if r.ok != r.total:
            fails['simulation ' + r.rid] = list(r.bad)
    print('  sources with a shipped twin of the same token shape in their directory: %d; '
          'same opcode sequence as the twin\'s shipped class: %d'
          % (c['with twin'], c['twin same opcodes']))
    print('  of these, twins with every token on the same line: %d; opcode, line, local index, '
          'count, jump offset and expression kind of every node equal: %d'
          % (c['with strict twin'], c['strict twin equal']))
    for k, v in fails.items():
        print('  FAILED %s: %d' % (k, len(v)))
        for x in v[:8]:
            print('      %s' % x[:300])
    with open(os.path.join(_HERE, 'out', 'spike_source_only.sha256'), 'w', encoding='utf-8',
              newline='\n') as f:
        f.write('\n'.join(sorted(shas, key=lambda x: x.split('  ')[1].lower())) + '\n')
    print('written to %s' % out_dir)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
