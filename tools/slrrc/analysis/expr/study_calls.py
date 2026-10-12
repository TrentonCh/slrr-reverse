#!/usr/bin/env python3
"""study_calls.py - the call sites of the corpus, by form and by how the
callee was resolved.

Uses the model of gen.py over the consistent pairs (all builds) and keeps only
the trees the class file confirms.  For every call through a name path
(`f(...)` and `a.b.f(...)`; 24,000 of them) it knows the receiver class, the
form and type of every argument and whether the class file ends the path in a
method ref or in a name.

Tables printed (and written to --json FILE):
  1. call sites by shape and outcome
  2. argument forms: under how many method refs / names each form stands, among
     the calls whose receiver class has a method of that name and argument
     count (so a ref was possible)
  3. calls with typed arguments only: outcome against what the method lookup
     returns
  4. per argument of a resolved call: relation of argument type to parameter
     type, with the score it contributes
  5. overloads: how many candidates the lookup had to choose from
"""
import json
import os
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for p in (_LIB, _HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import classdb      # noqa: E402
import check_expr   # noqa: E402
import gen          # noqa: E402

FORM_TEXT = {
    'lit-int': 'int literal', 'lit-float': 'float literal', 'lit-str': 'string literal',
    'lit-rid': 'resource id literal', 'lit-null': 'null', 'lit-bool': 'true / false',
    'local': 'local or parameter', 'field1': 'field by simple name', 'this': 'this',
    'path': 'dotted name a.b', 'getfield': 'X.f with computed X', 'new': 'new T(...)',
    'newarray': 'new T[n]', 'call': 'call', 'index': 'a[i]', 'cast': 'cast',
    'cond': 'c ? a : b', 'instanceof': 'instanceof',
}


def form_text(k):
    p = ''
    if k.startswith('paren:'):
        p = 'parenthesised '
        k = k[6:]
    if k in FORM_TEXT:
        return p + FORM_TEXT[k]
    for pre, txt in (('binary', 'binary '), ('unary', 'unary '), ('assign', 'assignment '),
                     ('postfix', 'postfix ')):
        if k.startswith(pre):
            return p + txt + k[len(pre):]
    return p + k


def relation(model, par, arg):
    if par == arg:
        return 'identical type', 0
    sc = model.type_score(par, arg)
    if arg in ('null', 'L;'):
        return 'null for an object or array parameter', sc
    if arg == 'I' and par == 'F':
        return 'int for a float parameter', sc
    if arg == 'F' and par == 'I':
        return 'float for an int parameter', sc
    if arg in ('I', 'F') and par[0] == 'L':
        return '%s for a %s parameter' % ('int' if arg == 'I' else 'float',
                                          par[1:-1].rpartition('.')[2]), sc
    if arg[0] == '[':
        return 'array for an Object parameter', sc
    acls, pcls = arg[1:-1], par[1:-1]
    d = 0
    for ci in model.db.chain(acls):
        if ci.name == pcls:
            return 'subclass for a superclass parameter, %d step%s up' % (d, '' if d == 1 else 's'), sc
        if pcls in ci.interfaces:
            return 'class for a parameter of an interface it implements', sc
        d += 1
    return 'other', sc


def main(argv):
    db = classdb.load()
    model = gen.Model(db)
    entries = check_expr.entries_of('940') + check_expr.entries_of('other')
    shape = Counter()
    forms = defaultdict(Counter)
    typed_tab = Counter()
    rel = Counter()
    rel_ex = {}
    over = Counter()
    sites = 0
    for r in check_expr.iter_trees(entries, model):
        if not r.ok:
            continue
        excl = set(x[0] + x[1][2] - 1 for x in r.exc)
        for k, pn in enumerate(r.pt.nodes):
            src = r.pt.src[k]
            if not isinstance(src, tuple):
                continue
            e, info = src
            if k in excl:
                shape['(callee class changed since compile: not counted)'] += 1
                continue
            sites += 1
            out = pn[0]                     # 'method' or 'name'
            shape['%s, ends in %s' % ('f(...)' if info['lone'] else 'a.b.f(...)',
                                      'a method ref' if out == 'method' else 'a name')] += 1
            argc = len(info['argt'])
            same = [m for m in info['cands'] if len(classdb.split_desc(m.desc)[0]) == argc]
            if same:
                if argc == 0:
                    forms['(no arguments)'][out] += 1
                for fk in set(info['argk']):
                    forms[form_text(fk)][out] += 1
            if info['typed']:
                found = info['found'] is not None
                typed_tab['typed arguments only; lookup finds %s; class file has a %s' % (
                    'a method' if found else 'nothing' if same else 'nothing (no method of that name and count)',
                    'method ref' if out == 'method' else 'name')] += 1
            else:
                typed_tab['an argument of another form; class file has a %s' % (
                    'method ref' if out == 'method' else 'name')] += 1
            if out == 'method':
                m = info['found']
                ps = classdb.split_desc(m.desc)[0]
                for p_, a_ in zip(ps, info['argt']):
                    what, sc = relation(model, p_, a_)
                    key = '%s (score %d)' % (what, sc)
                    rel[key] += 1
                    rel_ex.setdefault(key, '%s line %d: %s.%s %s' % (
                        r.entry.key, e.line, info['cls'].rpartition('.')[2], info['name'], m.desc))
                descs = set(c.desc for c in same)
                over['%d method%s of that name and argument count' % (
                    min(len(descs), 5), '' if len(descs) == 1 else 's')
                    + (' or more' if len(descs) >= 5 else '')] += 1
    print('call sites through a name path: %d' % sites)
    for k, v in sorted(shape.items()):
        print('  %7d  %s' % (v, k))
    print('argument forms (calls for which a method of that name and count exists):')
    print('  %-36s %8s %8s' % ('form', 'method', 'name'))
    for k, c in sorted(forms.items(), key=lambda kv: -sum(kv[1].values())):
        print('  %-36s %8d %8d' % (k, c['method'], c['name']))
    print('typed arguments against the lookup:')
    for k, v in sorted(typed_tab.items()):
        print('  %7d  %s' % (v, k))
    print('argument against parameter, per argument of a resolved call:')
    for k, v in sorted(rel.items(), key=lambda kv: -kv[1]):
        print('  %7d  %-62s %s' % (v, k, rel_ex[k]))
    print('candidates of a resolved call:')
    for k, v in sorted(over.items()):
        print('  %7d  %s' % (v, k))
    if '--json' in argv:
        with open(argv[argv.index('--json') + 1], 'w', encoding='utf-8') as f:
            json.dump({'sites': sites, 'shape': dict(shape),
                       'forms': {k: dict(v) for k, v in forms.items()},
                       'typed': dict(typed_tab), 'relations': dict(rel),
                       'relation_examples': rel_ex, 'overloads': dict(over)}, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
