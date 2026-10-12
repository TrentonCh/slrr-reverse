#!/usr/bin/env python3
"""census_forms.py - which statement forms occur where.

Counts every statement form of every source, split by corpus category:

    oracle        pair, Build 940, consistent   (the forms check_stmt.py proves)
    other pairs   stale or older build
    no class      the 635 sources that have no .class file

The point of the split: a form that occurs only in sources WITHOUT a class has
never been seen compiled, so its emission rule is a guess.  The last section
of the output lists exactly those.

Writes out/census_forms.txt and out/census_forms.json.
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

import oracle   # noqa: E402
import sparse   # noqa: E402

N = sparse.N


def cat_of(e):
    if e.category == 'pair_940' and e.consistent:
        return 'oracle'
    if e.category in ('pair_940', 'pair_other'):
        return 'other pairs'
    return 'no class'


class Census(object):
    def __init__(self, c, eg, where):
        self.c = c
        self.eg = eg
        self.where = where
        self.toks = None

    def hit(self, form, node=None):
        self.c[form] += 1
        if form not in self.eg and node is not None:
            self.eg[form] = '%s:%d' % (self.where, self.toks[node.t0].line)

    # ---- expressions (only what matters for statements) ----------------
    def expr(self, e, top=False):
        k = e.k
        # every expression kind, so that "only in sources without a class"
        # covers the syntax as a whole and not just the statements
        if k in ('bin', 'un', 'post', 'assign'):
            self.hit('expression: %s %s' % ({'bin': 'binary', 'un': 'prefix', 'post': 'postfix',
                                             'assign': 'assignment'}[k], e.op), e)
        elif k == 'lit':
            self.hit('expression: literal %s' % e.lk, e)
        elif k == 'name':
            self.hit('expression: name of %s' % ('1 identifier' if len(e.parts) == 1 else
                                                 '2 identifiers' if len(e.parts) == 2 else
                                                 '3 or more identifiers'), e)
        elif k == 'call':
            self.hit('expression: call through a name of %s' % (
                '1 identifier' if len(e.name.parts) == 1 else
                '2 identifiers' if len(e.name.parts) == 2 else '3 or more identifiers'), e)
        elif k == 'cast':
            self.hit('expression: cast to %s' % ('int/float' if e.type.parts[0] in ('int', 'float')
                                                 and len(e.type.parts) == 1 else 'a class'), e)
            if e.type.dims:
                self.hit('expression: cast to an array type', e)
        else:
            self.hit('expression: %s' % {'this': 'this', 'field': 'member of a computed value (x).name',
                                         'mcall': 'call on a computed value (x).f(..)',
                                         'scall': 'super.f(..)', 'ctor': 'this(..) / super(..)',
                                         'index': 'array element a[i]', 'new': 'new T(..)',
                                         'newarr': 'new T[n]', 'instanceof': 'instanceof',
                                         'cond': '?:', 'paren': '( .. )', 'super': 'bare super'}.get(k, k), e)
        if k == 'paren':
            self.hit('parenthesised expression', e)
            if top:
                self.hit('expression statement in parentheses', e)
            return self.expr(e.e, top)
        if k == 'cond':
            self.hit('conditional expression ?:', e)
        if k == 'bin' and e.op in ('&&', '||'):
            self.hit('short-circuit %s' % e.op, e)
        if k == 'assign' and not top:
            self.hit('assignment used as a value', e)
        if k in ('post', 'un') and e.op in ('++', '--') and not top:
            self.hit('++ / -- used as a value', e)
        if k == 'new' and top:
            self.hit('expression statement: new T(..)', e)
        if k == 'newarr' and len(e.sizes) != 1:
            self.hit('new array with %d sizes' % len(e.sizes), e)
        if k == 'ctor' and not top:
            self.hit('this(..)/super(..) not as a statement', e)
        for a, v in e.__dict__.items():
            if a in ('k', 't0', 't1'):
                continue
            if isinstance(v, N) and v.k not in ('type',):
                self.expr(v)
            elif isinstance(v, list):
                for x in v:
                    if isinstance(x, N):
                        self.expr(x)

    # ---- statements -----------------------------------------------------
    def stmts(self, lst, flow, where):
        seen_exit = False
        for s in lst:
            if seen_exit:
                self.hit('statement behind return/break/continue in the same list', s)
                seen_exit = False
            self.stmt(s, flow, where)
            if s.k in ('return', 'break', 'continue'):
                seen_exit = True

    def stmt(self, s, flow, where):
        """flow: list of enclosing 'for' 'while' 'do' 'switch'; where: what
        the statement is the direct child of."""
        k = s.k
        if k == 'block':
            self.hit('block', s)
            if not s.stmts:
                self.hit('empty block {}', s)
            if where in ('block', 'body', 'group'):
                self.hit('block directly inside a block', s)
            self.stmts(s.stmts, flow, 'block')
            return
        if k == 'empty':
            self.hit('empty statement ;', s)
            self.hit('empty statement ; as %s' % where, s)
            return
        if k == 'local':
            self.hit('local declaration', s)
            if where not in ('block', 'body', 'body-first', 'group', 'for-init'):
                self.hit('local declaration as %s (no block)' % where, s)
            if s.final:
                self.hit('local declaration with final', s)
            if len(s.decls) > 1:
                self.hit('local declaration with several declarators', s)
            for d in s.decls:
                self.hit('declarator with initialiser' if d.init is not None
                         else 'declarator without initialiser', s)
                if d.dims:
                    self.hit('declarator with [] behind the name', s)
                if d.init is not None:
                    self.expr(d.init)
            if s.type.dims:
                self.hit('local of array type', s)
            return
        if k == 'expr':
            e = s.e
            while e.k == 'paren':
                e = e.e
            kind = e.k if e.k not in ('un', 'post', 'assign') else '%s %s' % (e.k, e.op)
            self.hit('expression statement', s)
            self.hit('expression statement: %s' % kind, s)
            if e.k == 'ctor' and where != 'body-first':
                self.hit('this(..)/super(..) not first in a constructor body', s)
            self.expr(s.e, top=True)
            return
        if k == 'if':
            self.hit('if-else' if s.els is not None else 'if', s)
            if where == 'then':
                self.hit('if directly as the then-branch of an if', s)
                if s.els is not None:
                    self.hit('if-else directly as the then-branch of an if', s)
            if where == 'else':
                self.hit('else if', s)
            if where in ('for-body', 'while-body'):
                self.hit('if directly as a loop body', s)
            self.expr(s.cond)
            self.stmt(s.then, flow, 'then')
            if s.els is not None:
                self.stmt(s.els, flow, 'else')
            return
        if k == 'while':
            self.hit('while', s)
            if where == 'then':
                self.hit('loop directly as the then-branch of an if', s)
            self.expr(s.cond)
            self.stmt(s.body, flow + ['while'], 'while-body')
            return
        if k == 'do':
            self.hit('do-while', s)
            if s.semi is None:
                self.hit('do-while without the final ;', s)
            self.stmt(s.body, flow + ['do'], 'do-body')
            self.expr(s.cond)
            return
        if k == 'for':
            self.hit('for', s)
            if where == 'then':
                self.hit('loop directly as the then-branch of an if', s)
            if s.init is None:
                self.hit('for without init', s)
            elif isinstance(s.init, list):
                self.hit('for with expression init', s)
                if len(s.init) > 1:
                    self.hit('for with several init expressions', s)
                for x in s.init:
                    self.expr(x, top=True)
            else:
                self.hit('for with declaration init', s)
                self.stmt(s.init, flow, 'for-init')
                self.c['local declaration'] -= 1
            if s.cond is None:
                self.hit('for without condition', s)
            else:
                self.expr(s.cond)
            if not s.update:
                self.hit('for without update', s)
            if len(s.update) > 1:
                self.hit('for with several update expressions', s)
            for x in s.update:
                self.expr(x, top=True)
            self.stmt(s.body, flow + ['for'], 'for-body')
            return
        if k == 'switch':
            self.hit('switch', s)
            if not s.groups:
                self.hit('switch without any case', s)
            self.expr(s.subj)
            has_default = False
            for gi, g in enumerate(s.groups):
                self.hit('case group', g)
                if len(g.labels) > 1:
                    self.hit('case group with several labels', g)
                for c in g.labels:
                    if c.k == 'case':
                        self.hit('case label', c)
                        self.expr(c.e)
                        ce = c.e
                        while ce.k == 'paren':
                            ce = ce.e
                        self.hit('case value: %s' % (ce.k if ce.k != 'lit' else 'literal ' + ce.lk), c)
                    else:
                        self.hit('default label', c)
                        has_default = True
                        if gi != len(s.groups) - 1 or c is not g.labels[-1]:
                            self.hit('default label not last', c)
                if not g.stmts:
                    self.hit('case group without statements', g)
                last = g.stmts[-1] if g.stmts else None
                if last is None or last.k not in ('break', 'return', 'continue'):
                    if gi != len(s.groups) - 1:
                        self.hit('case group that falls through into the next', g)
                    else:
                        self.hit('last case group without break', g)
                if any(x.k == 'local' for x in g.stmts):
                    self.hit('case group that declares locals', g)
                self.stmts(g.stmts, flow + ['switch'], 'group')
            if not has_default:
                self.hit('switch without default', s)
            return
        if k == 'break':
            self.hit('break', s)
            if not flow:
                self.hit('break outside loop and switch', s)
            else:
                self.hit('break in a %s' % flow[-1], s)
            return
        if k == 'continue':
            self.hit('continue', s)
            loops = [f for f in flow if f != 'switch']
            if not loops:
                self.hit('continue outside a loop', s)
            else:
                self.hit('continue in a %s' % loops[-1], s)
                if flow[-1] == 'switch':
                    self.hit('continue inside a switch inside a loop', s)
            return
        if k == 'return':
            self.hit('return expr' if s.e is not None else 'return', s)
            if flow:
                self.hit('return inside a loop or switch', s)
            if s.e is not None:
                self.expr(s.e)
            return
        self.hit('UNKNOWN statement kind %s' % k, s)

    def member(self, m):
        if m.k == 'field':
            for d in m.decls:
                if d.init is not None:
                    self.hit('field initialiser', d)
                    self.hit('field initialiser (%s)' % ('static' if 'static' in m.mods else 'instance'), d)
                    self.expr(d.init)
            return
        if m.k != 'method':
            self.hit('member kind %s' % m.k, m)
            return
        what = 'constructor' if m.ctor else 'method'
        if 'native' in m.mods:
            what = 'native method'
        if 'abstract' in m.mods:
            what = 'abstract method'
        if m.body is None:
            self.hit('%s with body ;' % what, m)
            return
        if not m.body.stmts:
            self.hit('%s with body {}' % what, m)
            return
        self.hit('%s with statements' % what, m)
        if all(x.k in ('empty',) or (x.k == 'block' and not x.stmts) for x in m.body.stmts):
            self.hit('%s whose body holds only empty statements' % what, m)
        if 'static' in m.mods:
            self.hit('static method with statements', m)
        first = m.body.stmts[0]
        if m.ctor:
            if first.k == 'expr' and first.e.k == 'ctor':
                self.hit('constructor starting with %s(..)' % first.e.which, m)
            else:
                self.hit('constructor without this(..)/super(..)', m)
        last = m.body.stmts[-1]
        void = m.ctor or (m.ret.parts == ['void'] and not m.ret.dims)
        if not void and last.k != 'return':
            self.hit('value method not ending in a return', m)
        if void and last.k == 'return':
            self.hit('void method / constructor ending in a return', m)
        if m.ctor and any(self._has_return(x) for x in m.body.stmts):
            self.hit('constructor containing a return', m)
        self.stmts([first], [], 'body-first')
        self.c['statement behind return/break/continue in the same list'] += 0
        seen_exit = first.k in ('return', 'break', 'continue')
        for s in m.body.stmts[1:]:
            if seen_exit:
                self.hit('statement behind return/break/continue in the same list', s)
                seen_exit = False
            self.stmt(s, [], 'body')
            if s.k in ('return', 'break', 'continue'):
                seen_exit = True

    def _has_return(self, s):
        if s.k == 'return':
            return True
        for a, v in s.__dict__.items():
            if isinstance(v, N) and v.k not in ('type',):
                if self._has_return(v):
                    return True
            elif isinstance(v, list):
                for x in v:
                    if isinstance(x, N) and self._has_return(x):
                        return True
        return False


def main():
    counts = defaultdict(Counter)
    egs = defaultdict(dict)
    files = Counter()
    for e in oracle.iter_entries():
        if not e.source:
            continue
        cat = cat_of(e)
        files[cat] += 1
        u = sparse.parse(oracle.load_source(e))
        cs = Census(counts[cat], egs[cat], e.source)
        cs.toks = u.toks
        for c in u.classes:
            if c.kind == 'interface':
                counts[cat]['interface'] += 1
            for m in c.members:
                cs.member(m)
    cats = ['oracle', 'other pairs', 'no class']
    forms = sorted(set(f for c in counts.values() for f in c))
    L = []
    w = L.append
    w('census_forms.py: statement forms per corpus category')
    w('sources: ' + ', '.join('%s %d' % (c, files[c]) for c in cats))
    w('')
    w('%-62s %8s %8s %8s' % ('form', 'oracle', 'other', 'no class'))
    for f in forms:
        w('%-62s %8d %8d %8d' % (f, counts['oracle'][f], counts['other pairs'][f],
                                 counts['no class'][f]))
    w('')
    w('forms that occur ONLY in sources without a class (never seen compiled):')
    only = [f for f in forms if counts['no class'][f] and not counts['oracle'][f]
            and not counts['other pairs'][f]]
    if not only:
        w('  none')
    for f in only:
        w('  %-58s %6d   e.g. %s' % (f, counts['no class'][f], egs['no class'].get(f, '')))
    w('')
    w('forms that occur in the oracle pairs fewer than 10 times:')
    for f in forms:
        if 0 < counts['oracle'][f] < 10:
            w('  %-58s %6d   e.g. %s' % (f, counts['oracle'][f], egs['oracle'].get(f, '')))
    text = '\n'.join(L) + '\n'
    with open(os.path.join(_HERE, 'out', 'census_forms.txt'), 'w', newline='\n') as f:
        f.write(text)
    with open(os.path.join(_HERE, 'out', 'census_forms.json'), 'w', newline='\n') as f:
        json.dump({'files': dict(files), 'counts': {c: dict(v) for c, v in counts.items()},
                   'examples': {c: dict(v) for c, v in egs.items()}}, f, indent=1, sort_keys=True)
        f.write('\n')
    sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
