#!/usr/bin/env python3
"""check_members.py - proof that the declarations the parser finds are the
declarations the game's compiler found.

For each of the 2,819 source/class pairs the source is parsed (parser.py),
turned into class models (symbols.py) and compared with the class file
(tufa.py): the classes of the file, and per class the superclass, the
interfaces, every field row and every method row.

    python -I check_members.py [--json FILE] [--corpus DIR]

Prints one line per rule: "holds for N of M" and the exceptions.  Exit status
0 when every rule holds without exception.
"""
import json
import os
import sys
import time
from collections import Counter, OrderedDict

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import lexer    # noqa: E402
import oracle   # noqa: E402
import parser   # noqa: E402
import symbols  # noqa: E402
import tufa     # noqa: E402

RULES = OrderedDict([
    ('U1', 'the source parses'),
    ('U2', 'number of top-level types = number of blobs in the class file'),
    ('C1', 'blob name = package + "." + declared name, in source order'),
    ('C2', 'interface flag of CLSS = declared with the keyword interface'),
    ('C3', 'superclass = the class named after extends, resolved; java.lang.Object without extends; none for an interface'),
    ('C4', 'interface list = the names after implements, resolved, in source order'),
    ('F1', 'number of static fields and of instance fields (one row per declarator)'),
    ('F2', 'field names, in source order inside each vector'),
    ('F3', 'field flags = mask of the modifier keywords'),
    ('F4', 'field type descriptor (primitive, class resolved through package and imports, "[" per dimension)'),
    ('F5', 'a field owns an initialiser tree exactly when its declarator has "= expression"'),
    ('F6', 'index of the initialiser tree = position among the tree owners in source order'),
    ('M1', 'number of static methods and of instance methods'),
    ('M2', 'method names in source order inside each vector; a constructor is <init>'),
    ('M3', 'method flags = mask of the modifier keywords (static 0x8, native 0x40, ...)'),
    ('M4', 'number of parameters in the descriptor = number of declared parameters'),
    ('M5', 'method descriptor: parameter types and return type resolved; a constructor has no return part'),
    ('M6', 'index of the method tree = position among the tree owners in source order (none for abstract)'),
    ('M7', 'the tree is empty exactly for ";" bodies (native or not) and for "{}" of a method; a constructor with "{}" has code'),
    ('M8', 'locals word = this + parameters + deepest nesting of declared locals; 0 without code'),
    ('M9', 'no constructor is added: the class file has <init> rows exactly for the declared constructors'),
    ('T1', 'number of trees of the blob = fields with initialiser + methods that are not abstract'),
    ('D1', 'every descriptor of every blob of the corpus (pairs or not) is built from I, F, V, L<class>; and [ with at most one ['),
])


class Tally(object):
    def __init__(self):
        self.good = Counter()
        self.total = Counter()
        self.bad = {}

    def check(self, rule, ok, where, detail=''):
        self.total[rule] += 1
        if ok:
            self.good[rule] += 1
        else:
            self.bad.setdefault(rule, []).append('%s: %s' % (where, detail))
        return ok


def run(root=None):
    t = Tally()
    uni = symbols.Universe.from_index()
    stats = Counter()
    resolve_steps = Counter()
    ambiguous = []
    unresolved = []
    body_forms = Counter()
    scope_models = Counter(dict((m, 0) for m in symbols.SCOPE_MODELS[1:]))
    by_cat = {}
    for e in oracle.iter_pairs(build=None):
        stats['pairs'] += 1
        cat = e.category + ('' if e.consistent else ' (stale)')
        before = (sum(t.good.values()), sum(t.total.values()))
        text = oracle.load_source(e, root)
        try:
            unit = parser.parse(text, e.source)
        except (parser.ParseError, lexer.LexError) as x:
            t.check('U1', False, e.key, str(x))
            continue
        t.check('U1', True, e.key)
        blobs = oracle.load_class(e, root)
        res = symbols.Resolver(unit, uni)
        models = symbols.class_models(unit, res)
        if not t.check('U2', len(models) == len(blobs), e.key,
                       '%d declared, %d blobs' % (len(models), len(blobs))):
            continue
        for m, b in zip(models, blobs):
            stats['classes'] += 1
            w = '%s (%s)' % (e.key, m.simple_name)
            t.check('C1', m.name == b.name, w, '%s / %s' % (m.name, b.name))
            t.check('C2', m.interface == bool(b.is_interface), w)
            t.check('C3', m.super == b.super_name, w, '%s / %s' % (m.super, b.super_name))
            t.check('C4', m.interfaces == b.interface_names, w,
                    '%s / %s' % (m.interfaces, b.interface_names))
            t.check('T1', m.tree_count == len(b.trees), w,
                    '%d / %d' % (m.tree_count, len(b.trees)))
            declared = sum(1 for r in m.methods() if r.name == '<init>')
            in_class = sum(1 for _, _, f in b.methods() if b.utf8(f.name) == '<init>')
            t.check('M9', declared == in_class, w, '%d / %d' % (declared, in_class))
            stats['classes with %s' % ('no constructor' if declared == 0 else
                                       '1 constructor' if declared == 1 else
                                       'several constructors')] += 1
            # type names of the declarations, for the resolution statistics
            names = []
            cls = m.node
            if cls.extends:
                names.append(cls.extends)
            names.extend(cls.implements)
            for mem in cls.members:
                if mem.kind == 'FieldDecl':
                    if not mem.type.primitive:
                        names.extend([mem.type.name] * len(mem.declarators))
                elif mem.kind == 'MethodDecl':
                    for p in mem.params:
                        if not p.type.primitive:
                            names.append(p.type.name)
                    if mem.type is not None and not mem.type.primitive:
                        names.append(mem.type.name)
            for n in names:
                stats['type names in declarations'] += 1
                if '.' in n:
                    stats['type names written with dots'] += 1
                    continue
                c = res.candidates(n)
                if not c:
                    unresolved.append('%s: %s' % (w, n))
                    continue
                resolve_steps[c[0][0]] += 1
                if len(set(full for _, full in c)) > 1:
                    ambiguous.append('%s: %s -> %s' % (w, n, c))
            # fields
            for label, mine, theirs in (('static', m.static_fields, b.static_fields),
                                        ('instance', m.instance_fields, b.instance_fields)):
                if not t.check('F1', len(mine) == len(theirs), w,
                               '%s fields %d / %d' % (label, len(mine), len(theirs))):
                    continue
                for a, f in zip(mine, theirs):
                    stats['field rows'] += 1
                    stats['field rows ' + label] += 1
                    fw = '%s.%s line %d' % (w, a.name, a.declarator.line)
                    if not t.check('F2', a.name == b.utf8(f.name), fw, b.utf8(f.name)):
                        continue
                    t.check('F3', a.flags == f.flags, fw, '0x%X / 0x%X' % (a.flags, f.flags))
                    t.check('F4', a.desc == b.utf8(f.type), fw,
                            '%s / %s' % (a.desc, b.utf8(f.type)))
                    t.check('F5', (a.init is not None) == (f.init != tufa.NONE), fw)
                    t.check('F6', a.tree == f.init, fw, '%d / %d' % (a.tree, f.init))
                    if a.init is not None:
                        stats['fields with initialiser'] += 1
            # methods
            for label, mine, theirs in (('static', m.static_methods, b.static_methods),
                                        ('instance', m.instance_methods, b.instance_methods)):
                if not t.check('M1', len(mine) == len(theirs), w,
                               '%s methods %d / %d' % (label, len(mine), len(theirs))):
                    continue
                for a, f in zip(mine, theirs):
                    stats['method rows'] += 1
                    stats['method rows ' + label] += 1
                    mw = '%s.%s line %d' % (w, a.name, a.decl.line)
                    if not t.check('M2', a.name == b.utf8(f.name), mw, b.utf8(f.name)):
                        continue
                    if a.name == '<init>':
                        stats['constructors'] += 1
                    if a.flags & 0x40:
                        stats['native methods'] += 1
                    t.check('M3', a.flags == f.flags, mw, '0x%X / 0x%X' % (a.flags, f.flags))
                    desc = b.utf8(f.desc)
                    ps, ret = symbols.descriptor_params(desc)
                    t.check('M4', a.nparams == len(ps), mw, '%d / %s' % (a.nparams, desc))
                    t.check('M5', a.desc == desc, mw, '%s / %s' % (a.desc, desc))
                    t.check('M6', a.tree == f.tree, mw, '%d / %d' % (a.tree, f.tree))
                    if f.tree != tufa.NONE and f.tree < len(b.trees):
                        nodes = len(b.trees[f.tree])
                        want_code = symbols.has_code(a.decl)
                        t.check('M7', want_code == (nodes > 0), mw,
                                'body %s, %d nodes' % (a.body, nodes))
                        kind = 'constructor' if a.decl.constructor else (
                            'native method' if a.flags & 0x40 else 'method')
                        form = {'none': '";"', 'empty': '"{}"', 'block': '"{ ... }"'}[a.body]
                        body_forms['%s with %s -> %s' % (
                            kind, form, 'code' if nodes else 'empty tree')] += 1
                    t.check('M8', a.locals == f.locals, mw, '%d / %d' % (a.locals, f.locals))
                    if f.locals:
                        stats['methods with code'] += 1
                        for model in symbols.SCOPE_MODELS[1:]:
                            if symbols.locals_word(a.decl, model) != f.locals:
                                scope_models[model] += 1
        after = (sum(t.good.values()), sum(t.total.values()))
        c = by_cat.setdefault(cat, [0, 0, 0])
        c[0] += 1
        c[1] += after[1] - before[1]
        c[2] += (after[1] - before[1]) - (after[0] - before[0])
    # D1: all class files, with or without source
    letters = Counter()
    for rel, blobs in oracle.iter_class_files():
        for b in blobs:
            descs = []
            for _, _, f in b.fields():
                descs.append(b.utf8(f.type))
            for _, _, mrow in b.methods():
                ps, ret = symbols.descriptor_params(b.utf8(mrow.desc))
                descs.extend(ps)
                if ret:
                    descs.append(ret)
            for tree in b.trees:
                for nd in tree:
                    if nd.code == 0x1006:
                        descs.append(b.utf8(nd.imm))
            ok = True
            for d in descs:
                base = d.lstrip('[')
                letters[base[0]] += 1
                if len(d) - len(base) > 1:
                    letters['more than one ['] += 1
                    ok = False
                if base[0] not in 'IFVL' or (base[0] != 'L' and len(base) != 1) or \
                        (base[0] == 'L' and not base.endswith(';')):
                    ok = False
            t.check('D1', ok, '%s (%s)' % (rel, b.name))
    return {
        'descriptors': dict(letters),
        'tally': t, 'stats': stats, 'resolve_steps': resolve_steps,
        'ambiguous': ambiguous, 'unresolved': unresolved,
        'body_forms': body_forms, 'by_category': by_cat, 'scope_models': scope_models,
    }


def main(argv):
    root = None
    jpath = None
    i = 1
    while i < len(argv):
        if argv[i] == '--corpus':
            root = argv[i + 1]
            i += 2
        elif argv[i] == '--json':
            jpath = argv[i + 1]
            i += 2
        else:
            print(__doc__)
            return 2
    t0 = time.time()
    r = run(root)
    t = r['tally']
    s = r['stats']
    print('check_members: declarations of the parser against the class files')
    print('corpus %s' % (root or oracle.corpus_root()))
    print()
    print('pairs %d, classes %d, field rows %d (static %d, instance %d), '
          'method rows %d (static %d, instance %d; %d constructors, %d native)'
          % (s['pairs'], s['classes'], s['field rows'], s['field rows static'],
             s['field rows instance'], s['method rows'], s['method rows static'],
             s['method rows instance'], s['constructors'], s['native methods']))
    print()
    failed = 0
    for rule, text in RULES.items():
        good, total = t.good[rule], t.total[rule]
        mark = 'ok  ' if good == total else 'FAIL'
        if good != total:
            failed += 1
        print('%s %-3s %6d of %-6d %s' % (mark, rule, good, total, text))
        for x in t.bad.get(rule, [])[:25]:
            print('          exception: %s' % x)
        if len(t.bad.get(rule, [])) > 25:
            print('          ... %d more' % (len(t.bad[rule]) - 25))
    print()
    print('Per category (pairs, comparisons, failed comparisons):')
    for cat in sorted(r['by_category']):
        c = r['by_category'][cat]
        print('  %-22s %5d pairs  %7d comparisons  %d failed' % (cat, c[0], c[1], c[2]))
    print()
    print('Constructors per class (M9): none %d, one %d, several %d' % (
        s['classes with no constructor'], s['classes with 1 constructor'],
        s['classes with several constructors']))
    print()
    print('Scopes (M8): methods whose locals word a different scoping gets wrong,')
    print('of %d methods with a non-zero locals word:' % s['methods with code'])
    for model in symbols.SCOPE_MODELS[1:]:
        print('  %6d  %s' % (r['scope_models'][model], model))
    print()
    print('Descriptors of all class files (D1), by first letter: %s' % ', '.join(
        '%s %d' % (k, v) for k, v in sorted(r['descriptors'].items())))
    print()
    print('Body form against tree (M7):')
    for k in sorted(r['body_forms']):
        print('  %6d  %s' % (r['body_forms'][k], k))
    print()
    print('Type names in declarations (extends, implements, field, parameter and '
          'return types): %d' % s['type names in declarations'])
    print('  written with dots: %d' % s['type names written with dots'])
    for k in ('file', 'package', 'import', 'java.lang'):
        print('  first found in %-10s %d' % (k + ':', r['resolve_steps'][k]))
    print('  with candidates in more than one package: %d' % len(r['ambiguous']))
    for x in r['ambiguous'][:10]:
        print('      %s' % x)
    print('  unknown: %d' % len(r['unresolved']))
    for x in r['unresolved'][:10]:
        print('      %s' % x)
    print()
    total = sum(t.total.values())
    good = sum(t.good.values())
    print('%d of %d comparisons agree; %d of %d rules hold without exception (%.1f s)'
          % (good, total, len(RULES) - failed, len(RULES), time.time() - t0))
    if jpath:
        doc = {
            'rules': [{'id': k, 'text': v, 'holds': t.good[k], 'of': t.total[k],
                       'exceptions': t.bad.get(k, [])} for k, v in RULES.items()],
            'stats': dict(s), 'resolve_steps': dict(r['resolve_steps']),
            'ambiguous': r['ambiguous'], 'unresolved': r['unresolved'],
            'body_forms': dict(r['body_forms']),
            'scope_models': dict(r['scope_models']),
            'descriptors': r['descriptors'],
            'by_category': r['by_category'],
            'comparisons': total, 'agree': good,
        }
        with open(jpath, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(doc, f, indent=1, sort_keys=True)
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
