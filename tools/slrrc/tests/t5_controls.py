#!/usr/bin/env python3
"""t5_controls.py - what milestone M5 is worth: one rule of the writer changed
at a time, and the number of oracle files and pools that are lost.

    python -I tests/t5_controls.py [NAME ...]          (about 7 s per control)

tests/t5_files.py says that the compiler reproduces 2,649 class files.  That
says little unless a wrong writer would reproduce fewer.  Each control below
is a small textual change to slrrc/write.py or slrrc/pool.py (the old text
must occur exactly once, so a control cannot go stale silently); the changed
module is compiled in memory, the oracle pairs are compiled with it, and the
bytes are compared with the shipped classes exactly as the milestone test
does.  A control that loses nothing marks a choice the corpus does not decide.

Nothing in the package is changed, and this script is not part of the
milestone runs.  The trees are those of milestone M4 in every control: only
the order of the pool, the indices and the layout can differ.
"""
import os
import sys
import time
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness              # noqa: E402
import oracle               # noqa: E402  lib
import t5_files             # noqa: E402

import slrrc                        # noqa: E402
from slrrc import decl              # noqa: E402
from slrrc import pool              # noqa: E402
from slrrc import write             # noqa: E402

RESOLVE_ORDER = (
    '    for r in rows.static_methods:\n'
    '        if r.tree != NONE:\n'
    '            resolve_tree(pool, trees[r.tree], True, pool.entries[r.desc].text, own_type)\n'
    '    for r in rows.instance_methods:\n'
    '        if r.tree != NONE:\n'
    '            resolve_tree(pool, trees[r.tree], False, pool.entries[r.desc].text, own_type)\n'
    '    for r in rows.static_fields:\n'
    '        if r.tree != NONE:\n'
    '            resolve_tree(pool, trees[r.tree], True, None, own_type)\n'
    '    for r in rows.instance_fields:\n'
    '        if r.tree != NONE:\n'
    '            resolve_tree(pool, trees[r.tree], False, None, own_type)\n')


def order(*names):
    """The body of resolve_pass with the four row vectors in another order."""
    part = {
        'sm': ('    for r in rows.static_methods:\n'
               '        if r.tree != NONE:\n'
               '            resolve_tree(pool, trees[r.tree], True, pool.entries[r.desc].text, '
               'own_type)\n'),
        'im': ('    for r in rows.instance_methods:\n'
               '        if r.tree != NONE:\n'
               '            resolve_tree(pool, trees[r.tree], False, pool.entries[r.desc].text, '
               'own_type)\n'),
        'sf': ('    for r in rows.static_fields:\n'
               '        if r.tree != NONE:\n'
               '            resolve_tree(pool, trees[r.tree], True, None, own_type)\n'),
        'if': ('    for r in rows.instance_fields:\n'
               '        if r.tree != NONE:\n'
               '            resolve_tree(pool, trees[r.tree], False, None, own_type)\n'),
    }
    return ''.join(part[n] for n in names)


# the four vectors merged into tree order: (tree, is_static, descriptor or None)
BY_TREE = (
    '    owners = []\n'
    '    for r in rows.static_methods:\n'
    '        owners.append((r.tree, True, pool.entries[r.desc].text))\n'
    '    for r in rows.instance_methods:\n'
    '        owners.append((r.tree, False, pool.entries[r.desc].text))\n'
    '    for r in rows.static_fields:\n'
    '        owners.append((r.tree, True, None))\n'
    '    for r in rows.instance_fields:\n'
    '        owners.append((r.tree, False, None))\n'
    '    owners.sort(key=lambda x: x[0])\n'
    '    for t, st, d in owners:\n'
    '        if t != NONE:\n'
    '            resolve_tree(pool, trees[t], st, d, own_type)\n')

EMIT_LOOP = ('    pool = build.pool\n'
             '    for tree in build.trees:\n'
             '        ops = tree.operands\n')

# (name, module, [(old text, new text)], what the change is)
CONTROLS = [
    ('resolve-in-tree-order', 'write', [(RESOLVE_ORDER, BY_TREE)],
     'step 4 visits the trees in tree order instead of by row vector'),
    ('resolve-instance-first', 'write', [(RESOLVE_ORDER, order('im', 'sm', 'sf', 'if'))],
     'step 4: instance methods before static methods'),
    ('resolve-fields-first', 'write', [(RESOLVE_ORDER, order('sf', 'if', 'sm', 'im'))],
     'step 4: field initialisers before methods'),
    ('resolve-instance-fields-first', 'write', [(RESOLVE_ORDER, order('sm', 'im', 'if', 'sf'))],
     'step 4: instance field initialisers before static ones'),
    ('this-interns-nothing', 'write', [
        ('        elif op == OP_LOCAL and has_this and nd.value == 0:\n'
         '            pool.utf8(own_type)\n', '')],
     'this as a value interns nothing'),
    ('no-receiver-class', 'write', [
        ("    if n >= 2 and type_desc[0] == 'L' and type_desc[n - 1] == ';':\n"
         "        pool.cls(type_desc[1:n - 1])\n", "    return\n")],
     'the class of a receiver is not interned before the lookup'),
    ('receiver-before-names-only', 'write', [
        ('                elif sop == OP_FIELD:\n                    if not first:\n'
         '                        touch_receiver(pool, reached)\n',
         '                elif sop == OP_FIELD:\n'),
        ('                elif sop == OP_METHOD:\n                    if not first:\n'
         '                        touch_receiver(pool, reached)\n',
         '                elif sop == OP_METHOD:\n')],
     'the class of a receiver is interned only before a bare name'),
    ('receiver-arrays-too', 'write', [
        ("    if n >= 2 and type_desc[0] == 'L' and type_desc[n - 1] == ';':\n"
         "        pool.cls(type_desc[1:n - 1])\n",
         "    if n >= 2 and type_desc[0] == 'L' and type_desc[n - 1] == ';':\n"
         "        pool.cls(type_desc[1:n - 1])\n"
         "    elif type_desc[:1] == '[':\n"
         "        pool.cls(classes.class_of_desc(type_desc))\n")],
     'the class of an array receiver is interned before the lookup too'),
    ('ref-nat-before-class', 'pool', [
        ('        c = self.cls(cls)\n        n = self.nat(name, desc)\n',
         '        n = self.nat(name, desc)\n        c = self.cls(cls)\n')],
     'inside a ref the nat before the class'),
    ('nat-descriptor-first', 'pool', [
        ('        n = self.utf8(name)\n        d = self.utf8(desc)\n',
         '        d = self.utf8(desc)\n        n = self.utf8(name)\n')],
     'inside a nat the descriptor before the name'),
    ('emit-by-row-vector', 'write', [
        (EMIT_LOOP,
         '    pool = build.pool\n'
         '    rows = build.rows\n'
         '    order = []\n'
         '    for vec in (rows.static_methods, rows.instance_methods, rows.static_fields,\n'
         '                rows.instance_fields):\n'
         '        for r in vec:\n'
         '            if r.tree != NONE:\n'
         '                order.append(build.trees[r.tree])\n'
         '    for tree in order:\n'
         '        ops = tree.operands\n')],
     'step 5 visits the trees by row vector, as step 4 does'),
    ('emit-before-resolve', 'write', [
        ('    resolve_pass(build)\n    build.pool_steps.append(len(build.pool.entries))\n'
         '    emit_pass(build)\n',
         '    emit_pass(build)\n    build.pool_steps.append(len(build.pool.entries))\n'
         '    resolve_pass(build)\n')],
     'the strings before the refs'),
    ('one-pass-per-tree', 'write', [
        ('        if op == OP_DECLARE:\n            # declare k, type T',
         '        if (op == OP_STR or op == OP_NAME or op == OP_GETFIELD or op == OP_CALL_ON\n'
         '                or op == OP_CALL_SUPER):\n'
         '            pool.utf8(ops[nd.value].text)\n'
         '        if op == OP_RID:\n'
         '            pool.rid(ops[nd.value].text, ops[nd.value].number)\n'
         '        if op == OP_DECLARE:\n            # declare k, type T')],
     'steps 4 and 5 in one pass per tree (the lone nodes; path segments still in step 5)'),
    ('emit-interns-types', 'write', [
        ('            if op == OP_STR:\n                pool.utf8(ops[nd.value].text)\n',
         '            if op == OP_STR or op == OP_TYPE:\n'
         '                pool.utf8(ops[nd.value].text)\n')],
     'step 5 interns type carriers too (they are in the pool since step 2)'),
    ('fild-always', 'write', [
        ('    if has_fild(rows):\n', '    if True:\n')],
     'FILD is written for a class without a field too'),
    ('rid-id-first', 'pool', [
        ('        return self._entry(RID, self.utf8(pack), local_id)',
         '        return self._entry(RID, local_id, self.utf8(pack))')],
     'a rid entry holds the local id before the pack name'),
]


def patched(module, edits):
    """The module with the edits applied to its source, compiled in memory."""
    with open(module.__file__, 'r', encoding='utf-8') as f:
        src = f.read()
    for old, new in edits:
        if src.count(old) != 1:
            raise SystemExit('the text of a control occurs %d times in %s:\n%s' % (
                src.count(old), os.path.basename(module.__file__), old))
        src = src.replace(old, new)
    mod = types.ModuleType(module.__name__)
    mod.__package__ = 'slrrc'
    mod.__file__ = module.__file__
    exec(compile(src, module.__file__, 'exec'), mod.__dict__)
    return mod


def passing(entries, prov, pk, table):
    """(oracle files that come out byte-identical, 'file#blob' with an exact
    CONS chunk)."""
    files = set()
    pools = set()
    for e in entries:
        text = harness.load_source(e)
        mode = table.get(e.cls, 'A')
        if mode == '-':
            mode = 'A'
        res = slrrc.compile_source(text, e.source, prov, pk,
                                   slrrc.Options(strict=True, float_mode=mode))
        if not res.ok or harness.surprises(e.source, res.diagnostics):
            continue
        theirs = oracle.load_class_bytes(e)
        if res.class_file == theirs:
            files.add(e.cls)
        same, _ = t5_files.cons_equal(res.class_file, theirs)
        for k in same:
            pools.add('%s#%d' % (e.cls, k))
    return files, pools


def main(argv):
    prov = harness.provider()
    pk = harness.packs()
    table = t5_files.float_table()
    entries = harness.oracle_pairs()
    names = [a for a in argv if not a.startswith('-')]
    for n in names:
        if n not in [c[0] for c in CONTROLS]:
            raise SystemExit('unknown control %s (one of: %s)' % (
                n, ' '.join(c[0] for c in CONTROLS)))
    t0 = time.time()
    base_files, base_pools = passing(entries, prov, pk, table)
    print('writer as it is: %d oracle files byte-identical, %d pools exact' % (
        len(base_files), len(base_pools)))
    # where the compiler takes the changed code from: compile_source calls
    # write.finish_pool and write.class_file; decl.declare_class makes the Pool
    real = {'write': (write, write, ('finish_pool', 'class_file')),
            'pool': (pool, decl, ('Pool',))}
    for name, which, edits, what in CONTROLS:
        if names and name not in names:
            continue
        module, holder, attrs = real[which]
        mod = patched(module, edits)
        saved = [(a, getattr(holder, a)) for a in attrs]
        for a in attrs:
            setattr(holder, a, getattr(mod, a))
        try:
            files, pools = passing(entries, prov, pk, table)
        finally:
            for a, v in saved:
                setattr(holder, a, v)
        lost = sorted(base_files - files)
        print('%-30s %5d files lost %5d pools lost   %s%s' % (
            name, len(lost), len(base_pools - pools), what,
            ('   e.g. ' + lost[0]) if lost else ''))
        sys.stdout.flush()
    print('%.0f s' % (time.time() - t0))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
