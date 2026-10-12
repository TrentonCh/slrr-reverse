#!/usr/bin/env python3
"""t4_controls.py - what milestone M4 is worth: one rule of the emitter changed
at a time, and the number of oracle trees that are lost.

    python -I tests/t4_controls.py [NAME ...]          (about 10 s per control)

tests/t4_trees.py says that the emitter reproduces 7,568 trees.  That says
little unless a wrong emitter would reproduce fewer.  Each control below is a
small textual change to slrrc/emit.py or slrrc/classes.py (the old text must
occur exactly once, so a control cannot go stale silently); the changed module
is compiled in memory, the oracle pairs are emitted with it, and the trees are
compared with the shipped classes exactly as the milestone test does.

A control that loses no tree marks a choice the corpus does not decide
(notes/statements.md 8.1, "not ruled out").  Nothing in the package is
changed, and this script is not part of the milestone runs.
"""
import os
import sys
import time
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness              # noqa: E402
import oracle               # noqa: E402  lib
import t4_trees             # noqa: E402

import slrrc                        # noqa: E402
from slrrc import classes           # noqa: E402
from slrrc import emit              # noqa: E402

# (name, module, [(old text, new text)], what the change is)
CONTROLS = [
    ('params-forward', 'emit', [
        ('        k = len(m.items)\n        while k > 0:                            # the last parameter first\n            k -= 1\n            p = m.items[k]\n',
         '        k = 0\n        while k < len(m.items):\n            k += 1\n            p = m.items[k - 1]\n')],
     'parameters numbered in source order instead of in reverse'),
    ('outer-local-wins', 'emit', [
        ('        k = len(self.local_names)\n        while k > 0:\n            k -= 1\n            if self.local_names[k] == name:\n                return k\n        return NONE',
         '        k = 0\n        while k < len(self.local_names):\n            if self.local_names[k] == name:\n                return k\n            k += 1\n        return NONE')],
     'the outermost local of a name wins instead of the innermost'),
    ('break-without-releases', 'emit', [
        ('            f = self.flow[len(self.flow) - 1]\n'
         '            self.releases(f.mark, s.tok1, False)\n',
         '            f = self.flow[len(self.flow) - 1]\n')],
     'break does not release the locals it leaves behind'),
    ('return-closes-scope', 'emit', [
        ('        self.releases(0, tok, False)\n        self.put(OP_RET_VALUE, tok)',
         '        self.releases(0, tok, True)\n        self.put(OP_RET_VALUE, tok)')],
     'a return ends the scope of the locals it releases'),
    ('assign-target-first', 'emit', [
        ('            self.expr(e.b, OP_EXPR)\n            self.expr(e.a, OP_EXPR)\n            self.put_imm(top, ASSIGN_KIND[op], e.tok1 + 1)',
         '            self.expr(e.a, OP_EXPR)\n            self.expr(e.b, OP_EXPR)\n            self.put_imm(top, ASSIGN_KIND[op], e.tok1 + 1)')],
     'the target of an assignment before its value'),
    ('index-array-first', 'emit', [
        ('            self.expr(e.b, OP_EXPR)\n            self.expr(e.a, OP_EXPR)\n            self.put(OP_INDEX, e.tok1)',
         '            self.expr(e.a, OP_EXPR)\n            self.expr(e.b, OP_EXPR)\n            self.put(OP_INDEX, e.tok1)')],
     'the array before the index'),
    ('field-declaring-class', 'emit', [
        ('hit.owner if f.is_static else self.cls,', 'hit.owner,'),
        ('hit.owner if f.is_static else cls,', 'hit.owner,')],
     'a field ref always names the class that declares the field'),
    ('field-receiver-class', 'emit', [
        ('hit.owner if f.is_static else self.cls,', 'self.cls,'),
        ('hit.owner if f.is_static else cls,', 'cls,')],
     'a field ref always names the type of the receiver'),
    ('lookup-first-fit', 'classes', [
        ('            if score == 0:\n                hit.method = m\n',
         '            if score >= 0:\n                hit.method = m\n')],
     'the method lookup takes the first method that accepts the arguments'),
    ('lookup-last-of-ties', 'classes', [
        ('            if score < hit.score:\n', '            if score <= hit.score:\n')],
     'of equal scores the method met last stays (no call site has a tie)'),
    ('typed-in-parentheses', 'emit', [
        ('        k = a.kind\n        return k == LITERAL or k == NAME or k == THIS',
         '        k = strip_parens(a).kind\n        return k == LITERAL or k == NAME or k == THIS'),
        ('        None when the name is no variable."""\n        k = a.kind',
         '        None when the name is no variable."""\n        a = strip_parens(a)\n        k = a.kind')],
     'a parenthesised literal or name counts as a typed argument (no shipped call '
     'that could be resolved has one)'),
    ('if-end-last-token', 'emit', [
        ('                if s.c is None:\n                    return s.tok1 + 1\n',
         '                if s.c is None:\n                    return s.tok1 + 1 if dual else s.tok1\n')],
     'an if without else costs no lookahead token of its own'),
    ('end-without-context', 'emit', [
        ('                return s.tok1 + 1 if dual else s.tok1', '                return s.tok1')],
     'END(statement) without the dual context (a plain yacc grammar)'),
    ('else-not-dual', 'emit', [
        ('                self.stmt(s.c, dual)', '                self.stmt(s.c, False)')],
     'the else-branch does not inherit the dual context'),
    ('binary-line-last-token', 'emit', [
        ('                self.put_imm(top, BINARY_KIND[op], behind)',
         '                self.put_imm(top, BINARY_KIND[op], e.tok1)')],
     'a binary operator carries the line of the last token of its right operand'),
    ('assign-line-last-token', 'emit', [
        ('            self.put_imm(top, ASSIGN_KIND[op], e.tok1 + 1)',
         '            self.put_imm(top, ASSIGN_KIND[op], e.tok1)')],
     'an assignment carries the line of the last token of its value (not ruled out)'),
    ('call-path-line-rparen', 'emit', [
        ('                self.put_imm(OP_PATH, n + 1, e.name_tok)',
         '                self.put_imm(OP_PATH, n + 1, close)')],
     'the path header of a call carries the line of the ")"'),
    ('new-type-line-rparen', 'emit', [
        ('classes.descriptor(self.scope, e.a, 0), e.a.tok1 + 1)',
         'classes.descriptor(self.scope, e.a, 0), e.tok1)')],
     'the type carrier of new carries the line of the ")"'),
    ('implicit-super-line-brace', 'emit', [
        ('            first = body.tok0 + 1\n', '            first = body.tok0\n')],
     'the implicit super call carries the line of the "{"'),
    ('group-release-line-last', 'emit', [
        ('            self.releases(mark, g.tok1 + 1, True)',
         '            self.releases(mark, g.tok1, True)')],
     'the releases of a case group carry the line of its last token'),
    ('declare-line-name', 'emit', [
        ('            tok = d.name_tok + 1 + 2 * d.dims()\n', '            tok = d.name_tok\n')],
     'the nodes of a declaration carry the line of the declared name (not ruled out)'),
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


def passing(entries, prov, pk, modes):
    """The oracle trees that come out equal: set of 'class file#blob#tree'."""
    ok = set()
    for e in entries:
        text = harness.load_source(e)
        mode = modes.get(e.cls, 'A')
        res = slrrc.emit_trees(text, e.source, prov, pk,
                               slrrc.Options(strict=True, float_mode=mode))
        if not res.ok or harness.surprises(e.source, res.diagnostics):
            continue
        for bi, ti, why in t4_trees.compare(res, oracle.load_class(e)):
            if why is None:
                ok.add('%s#%d#%d' % (e.cls, bi, ti))
    return ok


def main(argv):
    prov = harness.provider()
    pk = harness.packs()
    modes = t4_trees.float_table()
    entries = harness.oracle_pairs()
    names = [a for a in argv if not a.startswith('-')]
    for n in names:
        if n not in [c[0] for c in CONTROLS]:
            raise SystemExit('unknown control %s (one of: %s)' % (
                n, ' '.join(c[0] for c in CONTROLS)))
    t0 = time.time()
    base = passing(entries, prov, pk, modes)
    print('emitter as it is: %d oracle trees node for node' % len(base))
    real = {'emit': (emit, ('emit_class',)),
            'classes': (classes, ('lookup_method', 'find_method', 'find_field', 'has_method'))}
    for name, which, edits, what in CONTROLS:
        if names and name not in names:
            continue
        module, attrs = real[which]
        mod = patched(module, edits)
        saved = [(a, getattr(module, a)) for a in attrs]
        for a in attrs:
            setattr(module, a, getattr(mod, a))
        try:
            now = passing(entries, prov, pk, modes)
        finally:
            for a, v in saved:
                setattr(module, a, v)
        lost = sorted(base - now)
        print('%-26s %5d trees lost   %s%s' % (
            name, len(lost), what, ('   e.g. ' + lost[0]) if lost else ''))
        sys.stdout.flush()
    print('%.0f s' % (time.time() - t0))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
