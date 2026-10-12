#!/usr/bin/env python3
r"""t_review.py - one check, at least, for every defect the review of the
reference compiler found (notes/impl_fix.md lists the findings by number).

    python -I tests/t_review.py [-v] [--update]
    python -I tests/run_tests.py review

The milestone tests say that the compiler reproduces the shipped classes; the
review asked what a C++ port, a host and a person at the command line would
trip over.  Each group below holds the checks for the defects of one kind; a
check has a name, and the names of the checks that pass are the baseline
(tests/baseline/review.pass), so a check that stops passing is named.

  results      (finding 1)  what a result hands out is copied into it: the
               lists of a FrontResult do not grow when the emitter runs
               afterwards, and the result of the whole compilation holds
               what the emitter consulted, missed and reported
  provider     (4, 18, 19)  the contract of ClassProvider: the exact name,
               the grammar of every descriptor, what a ClassInfo has to
               hold, re-entry, one question per name and compilation
  parser       (2, 15, 16)  where the private exceptions are raised and
               caught; the error paths that rely on fail() not returning;
               every node closed exactly once, after its items; the number
               of every node of two sources written out by hand; where the
               nesting limit is reported
  reader       (5)   no bounds check adds a length from the file to a
               position; lengths that wrap in 32 bits are rejected
  numbers      (14)  no negative number and no counter that has to go below
               zero anywhere in the core, two listed places apart
  floats       (6)   the self test of floatlit against an exact model, and
               the model under ten wrong builds: each is caught; the default
               is the conversion of the exe (B) with its two exits (".5" is
               0.0, 1,024 fraction digits)
  diagnostics  (3, 11, 13, 17)  a pack without a file, a class that is its
               own superclass, an instance field in static code, control
               characters in a string, the order of the methods, a caller
               deep in the stack; a resource id with a low word of 0 or with
               a high word, as the resource engine resolves it
  driver       (7, 8, 9, 12, 16)  the command line on small trees: a run that
               fails part-way, manifests of other shapes, output that the
               code page cannot encode, an unreadable class file, nesting
  gaps         (10)  the rules six surviving mutants changed, each pinned by
               a case written out by hand; the six mutants, applied in
               memory, are each caught; and every source of the corpus
               rewritten with one token per line compiles to the bytes of
               the composition spike
  readme       (20)  the README names what a new person needs, and its
               library example runs

Headline:
  review           results N/N; provider N/N; parser N/N; reader N/N; numbers
                   N/N; floats N/N; diagnostics N/N; driver N/N; gaps N/N,
                   6/6 mutants caught, one token per line 3454/3454 sources
                   equal to the spike; readme N/N
"""
import ast as pyast
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness              # noqa: E402
import oracle               # noqa: E402  lib
import tufa                 # noqa: E402  lib
import spike_compiler       # noqa: E402  spec
import floatmodel as fm     # noqa: E402
import t6_source_only as t6     # noqa: E402  (run_cli, put, get, snapshot)
import t_extra              # noqa: E402
import t_extra4             # noqa: E402

import slrrc                            # noqa: E402
from slrrc import classes               # noqa: E402
from slrrc import classfile             # noqa: E402
from slrrc import cli                   # noqa: E402
from slrrc import decl                  # noqa: E402
from slrrc import dump                  # noqa: E402
from slrrc import emit                  # noqa: E402
from slrrc import floatlit              # noqa: E402
from slrrc import install               # noqa: E402
from slrrc import packs                 # noqa: E402
from slrrc import parser as ps          # noqa: E402
from slrrc import scanner as sc         # noqa: E402
from slrrc.diag import Diagnostics      # noqa: E402

PKG = os.path.join(harness.ROOT, 'slrrc')
CORE = ('__init__.py', 'classes.py', 'classfile.py', 'decl.py', 'diag.py', 'dump.py',
        'emit.py', 'floatlit.py', 'packs.py', 'parser.py', 'pool.py', 'scanner.py', 'write.py')
O = 'java.lang.Object'
STR = 'Ljava.lang.String;'
mk = t_extra4.make_class


class Group(object):
    """The checks of one kind: (name, None or what went wrong)."""

    def __init__(self, name):
        self.name = name
        self.results = []

    def check(self, what, cond, detail=''):
        self.results.append((what, None if cond else (str(detail) or 'failed')))

    def passed(self):
        return [w for w, why in self.results if why is None]


def table_packs():
    return harness.table_packs()


def compile_(text, prov, strict=False, pk=None):
    return slrrc.compile_source(text, 'case.java', prov, pk or table_packs(),
                                slrrc.Options(strict=strict))


def codes(res):
    return [d.code for d in res.diagnostics]


def at(res):
    return [(d.code, d.line, d.column) for d in res.diagnostics]


def position(text, needle, nth=0):
    """(line, column) of the nth occurrence of needle in text."""
    pos = 0
    for _ in range(nth + 1):
        pos = text.index(needle, pos) + 1
    pos -= 1
    return text.count('\n', 0, pos) + 1, pos - text.rfind('\n', 0, pos)


def module_text(name):
    with open(os.path.join(PKG, name), 'r', encoding='utf-8') as f:
        return f.read()


class Counting(classes.ClassProvider):
    """A provider that remembers every question."""

    def __init__(self, inner):
        self.inner = inner
        self.calls = []

    def find(self, fqn):
        self.calls.append(fqn)
        return self.inner.find(fqn)


class CountingPacks(packs.PackNames):
    def __init__(self, inner):
        self.inner = inner
        self.calls = []

    def canonical(self, written_path):
        self.calls.append(written_path)
        return self.inner.canonical(written_path)


class Listed(classes.ClassProvider):
    """The classes of t_extra4.MemberProvider and a few more."""

    def __init__(self, more=()):
        self.known = dict(t_extra4.MemberProvider().known)
        for info in more:
            self.known[info.name] = info

    def find(self, fqn):
        return self.known.get(fqn)


# ---------------------------------------------------------------------------
# results (finding 1)
# ---------------------------------------------------------------------------

USES = ('package q;\n'
        'import p.*;\n'
        'class C extends T {\n'
        '\tint g(int a) {\n'
        '\t\treturn a + S2.conv(a);\n'
        '\t}\n'
        '}\n')


def g_results(g):
    prov = Listed()
    pk = table_packs()
    fr = slrrc.front(USES, 'case.java', prov)
    n_c, n_m, n_d = len(fr.consulted), len(fr.missed), len(fr.diagnostics)
    first_c, first_m = list(fr.consulted), list(fr.missed)
    g.check('front() hands out lists of its own',
            fr.ok and fr.consulted is not fr.scope.consulted and
            fr.missed is not fr.scope.missed and fr.diagnostics is not fr.scope.diags.items)
    # the emitter, run by hand on the scope front() left behind
    for cb in fr.classes:
        emit.emit_class(fr.scope, cb, pk, 'A')
    g.check('the emitter goes on consulting and missing classes',
            len(fr.scope.consulted) > n_c and len(fr.scope.missed) > n_m,
            '%d -> %d consulted, %d -> %d missed' % (
                n_c, len(fr.scope.consulted), n_m, len(fr.scope.missed)))
    g.check('... and the lists of the FrontResult do not grow with it',
            (len(fr.consulted), len(fr.missed), len(fr.diagnostics)) == (n_c, n_m, n_d))
    em = slrrc.emit_trees(USES, 'case.java', prov, pk)
    g.check('emit_trees() holds what the emitter consulted and missed',
            em.ok and em.consulted == fr.scope.consulted and em.missed == fr.scope.missed and
            em.consulted is not em.scope.consulted and len(em.consulted) > n_c,
            '%r' % (em.consulted,))
    res = slrrc.compile_source(USES, 'case.java', prov, pk)
    g.check('compile_source() holds the same lists',
            res.ok and res.consulted == em.consulted and res.missed == em.missed and
            'p.S2' in res.consulted and 'p.S2' not in first_c and
            'q.S2' in res.missed and 'q.S2' not in first_m,
            '%r / %r' % (res.consulted, res.missed))
    bad = t_extra4.S('x = nowhere;')
    fr = slrrc.front(bad, 'case.java', prov)
    em = slrrc.emit_trees(bad, 'case.java', prov, pk)
    res = slrrc.compile_source(bad, 'case.java', prov, pk)
    g.check('an error of the emitter reaches the result',
            fr.ok and fr.diagnostics == [] and not em.ok and codes(em) == ['E_NAME'] and
            not res.ok and codes(res) == ['E_NAME'] and res.class_file == b'' and
            fr.diagnostics == [])
    res = slrrc.compile_source(t_extra4.S('x = a += 1;'), 'case.java', prov, pk)
    g.check('a warning of the emitter reaches the result',
            res.ok and codes(res) == ['W_UNVERIFIED'])
    # two compilations through one provider do not share a list
    a = slrrc.compile_source(USES, 'a.java', prov, pk)
    keep = (list(a.consulted), list(a.missed), list(a.diagnostics))
    slrrc.compile_source(t_extra4.S('t.m();\nx = nowhere;'), 'b.java', prov, pk)
    g.check('the lists of a result do not change when another source is compiled',
            (a.consulted, a.missed, a.diagnostics) == keep)
    # on shipped sources
    cprov = harness.provider()
    cpk = harness.packs()
    n = bad_n = grew = 0
    why = ''
    for e in harness.all_sources()[::60]:
        text = harness.load_source(e)
        fr = slrrc.front(text, e.source, cprov)
        res = slrrc.compile_source(text, e.source, cprov, cpk)
        n += 1
        grew += len(res.consulted) > len(fr.consulted)
        ok = (res.ok and res.consulted[:len(fr.consulted)] == fr.consulted and
              len(set(res.consulted)) == len(res.consulted) and
              len(set(res.missed)) == len(res.missed) and
              not set(res.consulted) & set(res.missed) and
              all(cprov.find(x) is not None for x in res.consulted) and
              all(cprov.find(x) is None for x in res.missed))
        if not ok:
            bad_n += 1
            why = e.source
    g.check('shipped sources: the result holds every class of the front half and more, '
            'none twice, each found or missed as the provider says',
            n > 50 and bad_n == 0 and grew >= 5,
            '%d of %d wrong, e.g. %s; more classes than the front half for %d' % (
                bad_n, n, why, grew))


# ---------------------------------------------------------------------------
# provider (findings 4, 18, 19)
# ---------------------------------------------------------------------------

DESC_KINDS = [
    ('I', classes.DESC_TYPE), ('F', classes.DESC_TYPE), ('Lp.T;', classes.DESC_TYPE),
    ('L;', classes.DESC_TYPE), ('[I', classes.DESC_TYPE), ('[[Lp.T;', classes.DESC_TYPE),
    ('', classes.DESC_BAD), ('V', classes.DESC_BAD), ('L', classes.DESC_BAD),
    ('Lp.T', classes.DESC_BAD), ('[', classes.DESC_BAD), ('[[', classes.DESC_BAD),
    ('II', classes.DESC_BAD), ('Lp.T;x', classes.DESC_BAD), ('X', classes.DESC_BAD),
    ('()', classes.DESC_CTOR), ('(I)', classes.DESC_CTOR), ('(ILp.T;[F)', classes.DESC_CTOR),
    ('()V', classes.DESC_METHOD), ('(I)I', classes.DESC_METHOD),
    ('(L;)[Lp.T;', classes.DESC_METHOD), ('([[I)F', classes.DESC_METHOD),
    ('(', classes.DESC_BAD), ('(I', classes.DESC_BAD), ('(L', classes.DESC_BAD),
    ('([', classes.DESC_BAD), ('(Lp.T)V', classes.DESC_BAD), ('(V)V', classes.DESC_BAD),
    ('(I)VV', classes.DESC_BAD), ('(I)X', classes.DESC_BAD), ('(I)II', classes.DESC_BAD),
    ('I)V', classes.DESC_BAD), ('(I)[', classes.DESC_BAD),
]

# what is wrong -> a ClassInfo named p.B that has it
BAD_CLASSES = [
    ('a field with an empty descriptor', lambda: mk('p.B', O, [], [('bf', '', False)], [])),
    ('a field of type void', lambda: mk('p.B', O, [], [('bf', 'V', False)], [])),
    ('a field descriptor "L"', lambda: mk('p.B', O, [], [('bf', 'L', False)], [])),
    ('a field descriptor without ";"', lambda: mk('p.B', O, [], [('bf', 'Lp.T', False)], [])),
    ('a field descriptor "["', lambda: mk('p.B', O, [], [('bf', '[', False)], [])),
    ('a field descriptor with a tail', lambda: mk('p.B', O, [], [('bf', 'II', False)], [])),
    ('a field with an empty name', lambda: mk('p.B', O, [], [('', 'I', False)], [])),
    ('a method descriptor "(L"', lambda: mk('p.B', O, [], [], [('m', '(L', False)])),
    ('a method descriptor "(["', lambda: mk('p.B', O, [], [], [('m', '([', False)])),
    ('a method descriptor without ")"', lambda: mk('p.B', O, [], [], [('m', '(I', False)])),
    ('a method descriptor without "("', lambda: mk('p.B', O, [], [], [('m', 'I)V', False)])),
    ('a void parameter', lambda: mk('p.B', O, [], [], [('m', '(V)V', False)])),
    ('a method that is no descriptor at all', lambda: mk('p.B', O, [], [], [('m', '', False)])),
    ('a method without a return part', lambda: mk('p.B', O, [], [], [('m', '(I)', False)])),
    ('a constructor that is no descriptor', lambda: mk('p.B', O, [], [],
                                                       [('<init>', '(I', False)])),
    ('a method with an empty name', lambda: mk('p.B', O, [], [], [('', '()V', False)])),
    ('an empty superclass name', lambda: mk('p.B', '', [], [], [])),
    ('an empty interface name', lambda: mk('p.B', O, [''], [], [])),
    ('another name than the one asked for', lambda: mk('p.Bb', O, [], [], [])),
]

GOOD_CLASSES = [
    ('the type that resolved nowhere', lambda: mk('p.B', O, [], [('f', 'L;', False)],
                                                  [('m', '(L;)V', False)])),
    ('arrays of arrays', lambda: mk('p.B', O, [], [('f', '[[I', True)],
                                    [('m', '([[Lp.T;I)[F', True)])),
    ('constructors', lambda: mk('p.B', O, [], [], [('<init>', '()', False),
                                                   ('<init>', '(ILp.T;)', False)])),
    ('a constructor with a return part, as the loader of the engine holds it',
     lambda: mk('p.B', O, [], [], [('<init>', '(I)V', False)])),
    ('an empty class, as a provider answers while it declares a source',
     lambda: classes.ClassInfo('p.B')),
    ('an interface', lambda: mk('p.B', None, [], [('K', 'I', True)], [], True)),
]


class OneMore(classes.ClassProvider):
    """java.lang.Object and one class, whatever name it is asked by."""

    def __init__(self, info):
        self.info = info

    def find(self, fqn):
        if fqn == O:
            return classes.ClassInfo(O)
        if fqn == 'p.B':
            return self.info
        return None


class Sloppy(classes.ClassProvider):
    """Answers a name without regard to case, as a lookup by file name on a
    file system without case would."""

    def __init__(self, infos):
        self.known = dict((i.name.lower(), i) for i in infos)

    def find(self, fqn):
        return self.known.get(fqn.lower())


class SourceProvider(classes.ClassProvider):
    """Classes that exist as source only, declared on demand as rule 3 of
    the contract of ClassProvider says."""

    def __init__(self, sources):
        self.sources = sources          # full name -> text
        self.classes = {O: classes.ClassInfo(O)}
        self.in_progress = {}
        self.depth = 0
        self.max_depth = 0
        self.stubs = 0

    def find(self, fqn):
        info = self.classes.get(fqn)
        if info is not None:
            return info
        info = self.in_progress.get(fqn)
        if info is not None:
            self.stubs += 1
            return info
        text = self.sources.get(fqn)
        if text is None:
            return None
        names = slrrc.declared_names(text)
        for n in names:
            self.in_progress[n] = classes.ClassInfo(n)
        self.depth += 1
        if self.depth > self.max_depth:
            self.max_depth = self.depth
        res = slrrc.declare_source(text, fqn, self)
        self.depth -= 1
        for n in names:
            del self.in_progress[n]
        if not res.ok:
            return None
        for ci in res.infos:
            self.classes[ci.name] = ci
        return self.classes.get(fqn)


def g_provider(g):
    # ---- descriptors
    bad = [d for d, k in DESC_KINDS if classes.desc_kind(d) != k]
    g.check('desc_kind tells %d descriptors and non-descriptors apart' % len(DESC_KINDS),
            not bad, repr(bad))
    g.check('split_params', classes.split_params('(ILp.T;[F)V') == ['I', 'Lp.T;', '[F'] and
            classes.split_params('()') == [] and
            classes.split_params('([[I[Lp.T;L;)I') == ['[[I', '[Lp.T;', 'L;'])
    got = [classes.class_of_desc(d) for d in ('Lp.T;', '[I', '[Lp.T;', '[[F', 'I', 'F', '', None,
                                             'L;', '[L;')]
    g.check('class_of_desc', got == ['p.T', '[I', '[p.T', '[[F', None, None, None, None, '', '['],
            repr(got))
    # ---- a class that breaks the contract: reported, treated as not found
    user = 'package p;\nclass C {\n\tB b;\n\tvoid f() { b = null; }\n}\n'
    for what, make in BAD_CLASSES:
        info = make()
        why = classes.check_class_info(info, 'p.B')
        try:
            res = compile_(user, OneMore(info))
            strict = compile_(user, OneMore(info), strict=True)
            ok = (why != '' and res.ok and at(res)[0] == ('W_CLASS', 1, 1) and
                  codes(res) == ['W_CLASS', 'W_TYPE'] and 'p.B' in res.missed and
                  'p.B' not in res.consulted and b'Lp.B;' not in res.class_file and
                  not strict.ok and codes(strict) == ['W_CLASS', 'E_TYPE'])
            detail = '%r %r %r' % (why, at(res), codes(strict))
        except Exception as x:                  # the compiler must not raise
            ok = False
            detail = 'exception %r' % (x,)
        g.check('refused: ' + what, ok, detail)
    for what, make in GOOD_CLASSES:
        info = make()
        res = compile_(user, OneMore(info), strict=True)
        g.check('accepted: ' + what,
                classes.check_class_info(info, 'p.B') == '' and res.ok and
                res.diagnostics == [] and 'p.B' in res.consulted, repr(at(res)))
    # a damaged descriptor used in a call: the reference raised IndexError here
    info = mk('p.B', O, [], [('bf', '', False)], [('m', '(I)V', False)])
    try:
        res = compile_('package p;\nclass C extends B { void f() { m(bf); } }\n', OneMore(info))
        g.check('a damaged descriptor in the superclass ends in diagnostics, not in an '
                'exception', not res.ok and codes(res) == ['W_CLASS', 'E_TYPE'], repr(at(res)))
    except Exception as x:
        g.check('a damaged descriptor in the superclass ends in diagnostics, not in an '
                'exception', False, repr(x))
    # ---- the exact name
    sloppy = Sloppy([classes.ClassInfo(O), mk('p.Base', O, [], [], [])])
    res = compile_('package p;\nclass C extends base {\n}\n', sloppy)
    g.check('a class answered under another spelling of its name is not taken (superclass)',
            not res.ok and codes(res) == ['W_CLASS', 'E_TYPE'] and res.class_file == b'',
            repr(at(res)))
    res = compile_('package p;\nclass C {\n\tbase b;\n}\n', sloppy)
    g.check('... (field type): the name the source wrote does not reach the class file',
            res.ok and codes(res) == ['W_CLASS', 'W_TYPE'] and b'p.base' not in res.class_file
            and b'p.Base' not in res.class_file and b'L;' in res.class_file, repr(at(res)))
    # ---- what a ClassInfo has to hold
    iface = mk('p.I', None, [], [], [], True)
    user_cls = mk('p.U', O, [], [], [('take', '(Lp.I;)V', False)])
    text = 'package p;\nclass C {\n\tvoid f(U u, T t) { u.take(t); }\n}\n'

    def call_line(impl):
        res = slrrc.emit_trees(text, 'case.java', Listed([iface, impl, user_cls]),
                               table_packs())
        return dump.dump_tree(res.classes[0].trees[0]).split('\n')[5]

    with_i = call_line(mk('p.T', O, ['p.I'], [], []))
    without = call_line(mk('p.T', O, [], [], []))
    g.check('the interfaces of a class decide a method ref',
            with_i == '5 L3 method p.U.take:(Lp.I;)V' and without == '5 L3 name "take"',
            '%r / %r' % (with_i, without))
    text = 'package p;\nclass C {\n\tint f(D d) { return d.S; }\n}\n'

    def field_line(is_static):
        base = mk('p.B', O, [], [('S', 'I', is_static)], [])
        res = slrrc.emit_trees(text, 'case.java', Listed([base, mk('p.D', 'p.B', [], [], [])]),
                               table_packs())
        return dump.dump_tree(res.classes[0].trees[0]).split('\n')[2]

    g.check('the static flag of a field decides the class of its ref',
            field_line(True) == '2 L3 field p.B.S:I' and field_line(False) == '2 L3 field p.D.S:I',
            '%r / %r' % (field_line(True), field_line(False)))
    a = classes.declaration_text(mk('p.K', O, [], [], [('<init>', '(I)', False),
                                                       ('m', '(I)V', False)]))
    b = classes.declaration_text(mk('p.K', O, [], [], [('<init>', '(I)V', False),
                                                       ('m', '(I)V', False)]))
    g.check('the declaration text holds a constructor as the pool does, whatever return part '
            'the provider gave it',
            a == b == 'class p.K\nsuper java.lang.Object\ninterface 0\nmethod i <init> (I)\n'
                      'method i m (I)V\n', '%r / %r' % (a, b))
    # the same class with "(..)V" constructors: the same bytes for a source that uses it
    text = 'package p;\nclass C extends K {\n\tC() { super(1); }\n\tK k = new K(2);\n}\n'
    one = compile_(text, Listed([mk('p.K', O, [], [], [('<init>', '(I)', False)])]), True)
    two = compile_(text, Listed([mk('p.K', O, [], [], [('<init>', '(I)V', False)])]), True)
    g.check('... and the bytes do not depend on it either',
            one.ok and two.ok and one.class_file == two.class_file and
            one.diagnostics == [] and two.diagnostics == [], '%r %r' % (at(one), at(two)))
    doc = (classes.ClassProvider.__doc__ or '') + (classes.ClassProvider.find.__doc__ or '')
    g.check('the contract is written at the interface',
            all(w in doc for w in ('exactly that name', 'interfaces', 'is_static', 'source order',
                                   'NOTHING', 'RE-ENTRY', 'empty ClassInfo', 'ONE ANSWER',
                                   'declared_names', 'W_CLASS')))
    doc = slrrc.__doc__
    g.check('the entry points a host needs are named for the port',
            'declare(source' in doc and 'declared_names(source)' in doc and
            'compile(source' in doc)
    # ---- re-entry: two sources that name each other
    sp = SourceProvider({
        'p.A': 'package p;\nclass A extends B {\n\tB other;\n\tint n;\n}\n',
        'p.B': 'package p;\nclass B {\n\tA back;\n\tint m;\n}\n'})
    try:
        res = slrrc.emit_trees(
            'package p;\nclass C {\n\tint f(A a) { return a.other.back.n; }\n}\n',
            'case.java', sp, table_packs(), slrrc.Options(strict=True))
        lines = dump.dump_tree(res.classes[0].trees[0]).split('\n')[:5] if res.ok else []
        g.check('a provider that keeps rule 3 declares two sources that name each other',
                res.ok and res.diagnostics == [] and sp.max_depth == 2 and sp.stubs >= 1 and
                lines == ['0 L3 path 4', '1 L3 local 1', '2 L3 field p.A.other:Lp.B;',
                          '3 L3 field p.B.back:Lp.A;', '4 L3 field p.A.n:I'],
                '%r depth %d stubs %d %r' % (at(res), sp.max_depth, sp.stubs, lines))
    except RecursionError:
        g.check('a provider that keeps rule 3 declares two sources that name each other',
                False, 'RecursionError')
    # ---- one question per name and compilation
    worst = ''
    total = 0
    for name, expect, text in t_extra4.CASES:
        cp = Counting(t_extra4.MemberProvider())
        cq = CountingPacks(table_packs())
        slrrc.compile_source(text, 'case.java', cp, cq)
        total += len(cp.calls)
        if len(cp.calls) != len(set(cp.calls)) or len(cq.calls) != len(set(cq.calls)):
            worst = name
    g.check('the provider is asked once per name (the %d synthetic sources of extra4)'
            % len(t_extra4.CASES), worst == '' and total > 200, '%s (%d questions)' % (
                worst, total))
    text = t_extra4.M('int a = cars.racers.duhen:0x1r;\nint b = cars.racers.DUHEN:0x2r;\n'
                      'int c = cars.racers.duhen:0x3r;\nint d = cars.racers.duhen:0x4r;')
    cq = CountingPacks(table_packs())
    slrrc.compile_source(text, 'case.java', t_extra4.MemberProvider(), cq)
    g.check('the pack names are asked once per written name',
            cq.calls == ['cars\\racers\\duhen.rpk', 'cars\\racers\\DUHEN.rpk'], repr(cq.calls))
    cprov = harness.provider()
    cpk = harness.packs()
    srcs = harness.all_sources()
    sample = srcs[::25] + [e for e in srcs if e.source.endswith('game/src/Garage.java')]
    calls = distinct = misses = pcalls = pdistinct = 0
    for e in sample:
        cp = Counting(cprov)
        cq = CountingPacks(cpk)
        res = slrrc.compile_source(harness.load_source(e), e.source, cp, cq)
        calls += len(cp.calls)
        distinct += len(set(cp.calls))
        misses += len(res.missed)
        pcalls += len(cq.calls)
        pdistinct += len(set(cq.calls))
        if e.source.endswith('game/src/Garage.java'):
            g.check('Garage.java: 349 questions for 349 names (600 before the fix)',
                    len(cp.calls) == 349 and len(set(cp.calls)) == 349 and
                    len(cq.calls) == 7, '%d / %d / %d' % (len(cp.calls), len(set(cp.calls)),
                                                          len(cq.calls)))
    g.check('shipped sources: one question per name and per pack (%d sources)' % len(sample),
            calls == distinct and pcalls == pdistinct and misses > 0 and calls > 1000,
            '%d calls for %d names, %d for %d packs' % (calls, distinct, pcalls, pdistinct))
    # ---- every class of the corpus keeps the contract
    for n in list(cprov.pending):
        cprov.find(n)
    wrong = [n for n, info in cprov.classes.items() if classes.check_class_info(info, n) != '']
    g.check('every class of the corpus keeps the contract',
            len(cprov.classes) >= 3500 and not wrong, '%d classes, %r' % (len(cprov.classes),
                                                                         wrong[:3]))


# ---------------------------------------------------------------------------
# parser (findings 2, 15, 16)
# ---------------------------------------------------------------------------

def raises_and_handlers(name):
    """(set of (function, exception raised), set of (function, exception
    caught)) of a module of the package."""
    tree = pyast.parse(module_text(name))
    raised = set()
    caught = set()

    def exc_name(node):
        if node is None:
            return 're-raise'
        if isinstance(node, pyast.Call):
            node = node.func
        if isinstance(node, pyast.Name):
            return node.id
        if isinstance(node, pyast.Tuple):
            return '+'.join(sorted(exc_name(x) for x in node.elts))
        return pyast.dump(node)

    def walk(node, fn):
        for child in pyast.iter_child_nodes(node):
            inner = fn
            if isinstance(child, (pyast.FunctionDef, pyast.Lambda)):
                inner = getattr(child, 'name', 'lambda')
            if isinstance(child, pyast.Raise):
                raised.add((fn, exc_name(child.exc)))
            if isinstance(child, pyast.ExceptHandler):
                caught.add((fn, 'bare' if child.type is None else exc_name(child.type)))
            walk(child, inner)

    walk(tree, '<module>')
    return raised, caught


EXCEPTIONS = {
    # module: (raised, caught); a module that is not listed has neither
    'parser.py': ({('fail', '_Stop'), ('unverified', '_Stop')},
                  {('parse', '_Stop'), ('parse', 'RecursionError')}),
    '__init__.py': ({('emit_trees', 'ValueError'), ('compile_source', 'ValueError')},
                    {('_bytes_only', 'UnicodeEncodeError'), ('_front', 'RecursionError'),
                     ('_emit', 'RecursionError'), ('compile_source', 'RecursionError'),
                     ('declare_source', 'RecursionError')}),
    # a node kind the unparser of the M2 test has no case for: a mistake in
    # the package, not an input
    'dump.py': ({('stmt', 'ValueError'), ('expr', 'ValueError')}, set()),
}

ERROR_PATHS = [
    # what, text, line of the one diagnostic
    ('no class keyword', 'package p;\npublic x C { }\n', 2),
    ('an unclosed block', 'package p;\nclass C { void f() { x = 1;\n', 3),
    ('an unclosed class body', 'package p;\nclass C { int a;\n', 3),
    ('a statement before the first case label',
     'package p;\nclass C { void f() { switch (a) {\n x = 1; } } }\n', 3),
    ('an unclosed switch body', 'package p;\nclass C { void f() { switch (a) { case 1:\n', 3),
    ('no expression', 'package p;\nclass C {\n int a = ; }\n', 3),
    ('new without "(" or "["', 'package p;\nclass C {\n int a = new T; }\n', 3),
    ('new without a type', 'package p;\nclass C {\n int a = new 3; }\n', 3),
    ('a missing ")"', 'package p;\nclass C { void f() {\n g(1; } }\n', 3),
    ('a missing identifier', 'package p;\nclass {\n}\n', 2),
    ('super without "."', 'package p;\nclass C { void f() {\n x = super; } }\n', 3),
    ('else without if', 'package p;\nclass C { void f() {\n else x = 1; } }\n', 3),
]


class Watching(ps.Parser):
    """A parser that records when every node is closed, and builds the flat
    list of items as a port does: the items of a node appended when the node
    is closed."""

    def __init__(self, source, toks, diags):
        ps.Parser.__init__(self, source, toks, diags)
        self.closed = {}            # node -> how often
        self.order = {}             # node -> position of its (last) closing
        self.count = {}             # node -> number of items when it was closed first
        self.lists = []             # the flat list
        self.slices = {}            # node -> (start, count) in lists
        self.tick = 0

    def done(self, n):
        self.closed[n.index] = self.closed.get(n.index, 0) + 1
        self.order[n.index] = self.tick
        self.tick += 1
        if n.index not in self.count:
            self.count[n.index] = len(n.items)
            self.slices[n.index] = (len(self.lists), len(n.items))
            self.lists.extend(x.index for x in n.items)
        return ps.Parser.done(self, n)


def watch(text):
    """None when the text does not parse, else what is wrong with the way its
    nodes were closed ('' when nothing is)."""
    d = Diagnostics(False)
    toks = sc.scan(text, d)
    if d.errors:
        return None
    p = Watching(text, toks, d)
    try:
        p.unit()
    except ps._Stop:
        return None
    for n in p.ast.nodes:
        if p.closed.get(n.index, 0) != 1:
            return '%s closed %d times' % (ps.KIND_NAMES[n.kind], p.closed.get(n.index, 0))
        if p.count[n.index] != len(n.items):
            return '%s got items after it was closed' % ps.KIND_NAMES[n.kind]
        start, count = p.slices[n.index]
        if p.lists[start:start + count] != [x.index for x in n.items]:
            return '%s: its slice of the flat list is not its items' % ps.KIND_NAMES[n.kind]
        for c in ps.children(n):
            if p.order[c.index] > p.order[n.index]:
                return '%s closed before its child %s' % (ps.KIND_NAMES[n.kind],
                                                          ps.KIND_NAMES[c.kind])
    return ''


# Two sources and the dump of their trees, written out by hand from the order
# in which parser.py makes its nodes ("nodes are numbered in the order they
# are made").  index kind tok0 tok1 op_tok name_tok a b c flags [items]
AST_SMALL = ('package p;\nclass C {\n\tvoid f(int a) {\n\t\ta = a + 1;\n\t}\n}\n', '''
0 Unit 0 20 - - 1 - - 0 [2]
1 QName 1 1 - 1 - - - 0 []
2 Class 3 20 - 4 - - - 0 [4]
3 Type 6 6 - 6 - - - 0 []
4 MethodDecl 6 19 - 7 3 7 - 0 [6]
5 Type 9 9 - 9 - - - 0 []
6 Param 9 10 - 10 5 - - 0 []
7 Block 12 19 - - - - - 0 [8]
8 ExprStmt 13 18 - - 10 - - 0 []
9 Name 13 13 - 13 - - - 0 []
10 Assign 13 17 14 - 9 12 - 0 []
11 Name 15 15 - 15 - - - 0 []
12 Binary 15 17 16 - 11 13 - 0 []
13 Literal 17 17 - - - - - 0 []
''')

AST_SWITCH = ('package p;\nclass C {\n\tvoid f(int a) {\n\t\tswitch (a) {\n\t\tcase 1:\n'
              '\t\tcase 2:\n\t\t\ta = 3;\n\t\t\tbreak;\n\t\tdefault:\n\t\t\ta++;\n\t\t}\n'
              '\t\tfor (int i = 0; i < a; i++) a--;\n\t}\n}\n', '''
0 Unit 0 54 - - 1 - - 0 [2]
1 QName 1 1 - 1 - - - 0 []
2 Class 3 54 - 4 - - - 0 [4]
3 Type 6 6 - 6 - - - 0 []
4 MethodDecl 6 53 - 7 3 7 - 0 [6]
5 Type 9 9 - 9 - - - 0 []
6 Param 9 10 - 10 5 - - 0 []
7 Block 12 53 - - - - - 0 [8 27]
8 Switch 13 35 - - 9 - - 0 [10 21]
9 Name 15 15 - 15 - - - 0 []
10 SwitchGroup 18 29 - - 11 - - 0 [16 20]
11 List 18 23 - - - - - 0 [12 14]
12 CaseLabel 18 20 - - 13 - - 0 []
13 Literal 19 19 - - - - - 0 []
14 CaseLabel 21 23 - - 15 - - 0 []
15 Literal 22 22 - - - - - 0 []
16 ExprStmt 24 27 - - 18 - - 0 []
17 Name 24 24 - 24 - - - 0 []
18 Assign 24 26 25 - 17 19 - 0 []
19 Literal 26 26 - - - - - 0 []
20 Break 28 29 - - - - - 0 []
21 SwitchGroup 30 34 - - 22 - - 0 [24]
22 List 30 31 - - - - - 0 [23]
23 DefaultLabel 30 31 - - - - - 0 []
24 ExprStmt 32 34 - - 26 - - 0 []
25 Name 32 32 - 32 - - - 0 []
26 Postfix 32 33 33 - 25 - - 0 []
27 For 36 52 - - 33 37 29 0 [36]
28 Type 38 38 - 38 - - - 0 []
29 LocalVar 38 41 - - 28 - - 0 [30]
30 Declarator 39 41 40 39 31 - - 0 []
31 Literal 41 41 - - - - - 0 []
32 Name 43 43 - 43 - - - 0 []
33 Binary 43 45 44 - 32 34 - 0 []
34 Name 45 45 - 45 - - - 0 []
35 Name 47 47 - 47 - - - 0 []
36 Postfix 47 48 48 - 35 - - 0 []
37 ExprStmt 50 52 - - 39 - - 0 []
38 Name 50 50 - 50 - - - 0 []
39 Postfix 50 51 51 - 38 - - 0 []
''')


def first_difference(got, want):
    got = got.strip('\n').split('\n')
    want = want.strip('\n').split('\n')
    k = 0
    while k < min(len(got), len(want)) and got[k] == want[k]:
        k += 1
    if k == len(got) == len(want):
        return ''
    return 'line %d: %r / expected %r' % (k + 1, got[k] if k < len(got) else 'end',
                                          want[k] if k < len(want) else 'end')


def g_parser(g):
    # ---- finding 2: where the exceptions are
    for name in CORE:
        raised, caught = raises_and_handlers(name)
        if name == 'classfile.py':
            ok = (raised and all(x == '_Bad' and fn != 'read' for fn, x in raised) and
                  caught == {('read', '_Bad')})
        else:
            want = EXCEPTIONS.get(name, (set(), set()))
            ok = (raised, caught) == want
        g.check('%s raises and catches only what its text says' % name, ok,
                '%r / %r' % (sorted(raised), sorted(caught)))
    doc = ps.__doc__
    g.check('the way the parser stops is written down for the port',
            'HOW THE PARSER STOPS AT AN ERROR' in doc and 'ONE exception type' in doc and
            '_Bad' in classfile.__doc__)
    prov = t_extra4.MemberProvider()
    for what, text, line in ERROR_PATHS:
        try:
            res = compile_(text, prov)
            ok = (not res.ok and len(res.diagnostics) == 1 and res.diagnostics[0].error and
                  res.diagnostics[0].code == 'E_SYNTAX' and res.diagnostics[0].line == line and
                  res.class_file == b'' and res.ast.root is None)
            detail = repr(at(res))
        except Exception as x:
            ok = False
            detail = 'exception %r' % (x,)
        g.check('one diagnostic and no tree: ' + what, ok, detail)
    # ---- finding 15: nodes are closed once, after their items
    texts = [c[2] for c in t_extra.CASES] + [c[2] for c in t_extra4.CASES]
    texts += [AST_SMALL[0], AST_SWITCH[0]]
    n = 0
    bad = ''
    for text in texts:
        why = watch(text)
        if why is None:
            continue
        n += 1
        if why:
            bad = why
    g.check('synthetic sources: every node is closed exactly once, after its children and '
            'its items', n > 150 and bad == '', '%d parsed, %s' % (n, bad))
    n = 0
    bad = ''
    for e in harness.all_sources()[::9]:
        why = watch(harness.load_source(e))
        n += 1
        if why:
            bad = '%s: %s' % (e.source, why)
    g.check('shipped sources: the same (%d sources)' % n, n > 300 and bad == '', bad)
    for name, (text, want) in (('an assignment', AST_SMALL), ('a switch and a for', AST_SWITCH)):
        d = Diagnostics(False)
        ast = ps.parse(text, sc.scan(text, d), d)
        diff = first_difference(dump.dump_ast(ast), want) if ast.root is not None else 'no tree'
        g.check('the number of every node, written out by hand: ' + name, diff == '', diff)
    doc = ps.__doc__
    g.check('the layout of the items is written down for the port',
            'NODES ARE NUMBERED IN THE ORDER THEY ARE MADE' in doc and
            'EVERY NODE IS CLOSED EXACTLY ONCE' in doc and
            'copied\ninto Ast::lists by done()' in doc)
    # ---- finding 16: where the nesting limit is reported
    def nested(depth):
        return 'package p;\nclass Deep {\n\tint a = %s1%s;\n}\n' % ('(' * depth, ')' * depth)

    res = compile_(nested(190), prov)
    g.check('190 nested parentheses compile', res.ok and res.diagnostics == [], repr(at(res)))
    for depth in (196, 197, 199):
        text = nested(depth)
        res = compile_(text, prov)
        want = ('E_LIMIT',) + position(text, '1)')
        g.check('%d nested parentheses: the limit is reported at the innermost operand' % depth,
                not res.ok and at(res) == [want], '%r, expected %r' % (at(res), want))
    text = nested(230)
    res = compile_(text, prov)
    d = res.diagnostics[0] if res.diagnostics else None
    g.check('230 nested parentheses: the limit is reported inside the nesting',
            d is not None and d.code == 'E_LIMIT' and d.line == 3 and d.column > 190,
            repr(at(res)))
    text = 'package p;\nclass C {\n\tint a =\n\t\t7 + %s;\n}\n' % ' + '.join(['1'] * 260)
    res = compile_(text, prov)
    want = ('E_LIMIT',) + position(text, '7')
    g.check('an operator chain of 260: the limit is reported at its first operand',
            at(res) == [want], '%r, expected %r' % (at(res), want))
    text = 'package p;\nclass C {\n\tvoid f() {\n%s\tx = 1;\n%s\t}\n}\n' % (
        '\t{\n' * 205, '\t}\n' * 205)
    res = compile_(text, prov)
    d = res.diagnostics[0] if res.diagnostics else None
    g.check('205 nested blocks: the limit is reported at a "{" 200 levels down',
            d is not None and d.code == 'E_LIMIT' and 195 < d.line - 3 < 205 and
            text.split('\n')[d.line - 1][d.column - 1] == '{', repr(at(res)))


# ---------------------------------------------------------------------------
# reader (finding 5)
# ---------------------------------------------------------------------------

def u32(data, pos):
    return data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24)


def with_u32(data, pos, value):
    return data[:pos] + bytes([value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF,
                               (value >> 24) & 0xFF]) + data[pos + 4:]


def wraps(pos, n, end):
    """Would `pos + n > end`, computed in 32 bits, let n through?"""
    return ((pos + n) & 0xFFFFFFFF) <= end


def tufa_rejects(data):
    try:
        tufa.parse(data)
    except tufa.TufaError:
        return True
    return False


def g_reader(g):
    tree = pyast.parse(module_text('classfile.py'))
    sums = []

    def has_sum(node):
        """A sum in an expression, the bounds of a slice of bytes apart."""
        if isinstance(node, pyast.BinOp) and isinstance(node.op, pyast.Add):
            return True
        if isinstance(node, pyast.Subscript):
            return has_sum(node.value)
        return any(has_sum(x) for x in pyast.iter_child_nodes(node))

    for node in pyast.walk(tree):
        if isinstance(node, pyast.Compare):
            for side in [node.left] + list(node.comparators):
                if has_sum(side):
                    sums.append(node.lineno)
    g.check('no comparison of the reader has a sum on either side', not sums,
            'classfile.py lines %r' % sorted(set(sums)))
    g.check('the rule is written down for the port', 'BOUNDS.' in classfile.__doc__ and
            'n > end - pos' in classfile.__doc__)
    text = ('package p;\nclass C implements I {\n\tstatic int n = 5;\n\tString s = "hi";\n'
            '\tvoid f(int a) { n = a; }\n}\n')
    res = compile_(text, t_extra4.MemberProvider())
    data = res.class_file
    ok = res.ok and classfile.read(data, True).ok and classfile.read(data).ok
    g.check('the class file the checks start from is read back', ok)
    if not ok:
        return
    size = len(data)
    cons = data.index(b'CONS')
    fild = data.index(b'FILD')
    mthd = data.index(b'MTHD')
    clss = data.index(b'CLSS')
    tr = data.index(b'TREE')
    # (what, offset of the u32, position the value is added to, end it is compared with,
    #  does the read without the nodes look at it)
    first_len = u32(data, cons + 16)
    fields = [
        ('the size of the CONS chunk', cons + 4, cons + 8, size, True),
        ('the length of the first utf8 entry', cons + 16, cons + 20,
         cons + 8 + u32(data, cons + 4), True),
        ('the number of pool entries', cons + 8, cons + 12, cons + 8 + u32(data, cons + 4), True),
        ('the number of static fields', fild + 8, fild + 12, fild + 8 + u32(data, fild + 4), True),
        ('the number of static methods', mthd + 8, mthd + 12, mthd + 8 + u32(data, mthd + 4),
         True),
        ('the number of interfaces', clss + 24, clss + 28, clss + 8 + u32(data, clss + 4), True),
        ('the size of the TREE chunk', tr + 4, tr + 8, size, True),
        ('the node count of the first tree', tr + 12, tr + 16, tr + 8 + u32(data, tr + 4), False),
    ]
    g.check('the offsets of the test are the ones of the file',
            first_len == 3 and data[cons + 20:cons + 23] == b'p.C' and u32(data, clss + 24) == 1)
    for what, off, pos, end, quick in fields:
        values = [0x7FFFFFFF, 0x80000000, 0xFFFFFFF0, 0xFFFFFFFC, 0xFFFFFFFF]
        # the values a 32-bit sum lets through: pos + n wraps to a place inside the chunk
        values += [(0x100000000 - pos + k) & 0xFFFFFFFF for k in (0, 1, 4, end - pos)]
        bad = []
        n_wrap = 0
        for v in values:
            if wraps(pos, v, end):
                n_wrap += 1
            variant = with_u32(data, off, v)
            try:
                a = classfile.read(variant, True)
                b = classfile.read(variant)
                if a.ok or (quick and b.ok) or not a.error or not tufa_rejects(variant):
                    bad.append('0x%X accepted' % v)
            except Exception as x:              # the reader must not raise
                bad.append('0x%X: %r' % (v, x))
        g.check('rejected: %s set to a length near 2**32 (%d values, %d of them wrap)' % (
            what, len(values), n_wrap), not bad and n_wrap >= 4, '; '.join(bad[:3]))
    # the reader still accepts what it accepted
    n = same = 0
    for rel in oracle.find_class_files()[::40]:
        raw = oracle.load_class_bytes(rel)
        n += 1
        same += classfile.read(raw, True).ok and classfile.read(raw).ok
    g.check('shipped class files are still accepted (%d files)' % n, n > 50 and same == n)


# ---------------------------------------------------------------------------
# numbers (finding 14)
# ---------------------------------------------------------------------------

# The places where the core compares a number with zero in a way that an
# unsigned number makes pointless, and why each may stay.
SIGNED = {
    ('scanner.py', '_find'): 'str.find answers -1; _find turns that into the length',
    ('__init__.py', 'compile_source'): 'the range check of Options.build (Python only)',
}


def is_zero(node):
    return isinstance(node, pyast.Constant) and type(node.value) is int and node.value == 0


def signed_uses(name):
    """[(function, line, what)]: negative numbers, and comparisons with zero
    that are always true or always false for an unsigned number."""
    tree = pyast.parse(module_text(name))
    out = []

    def walk(node, fn):
        for child in pyast.iter_child_nodes(node):
            inner = fn
            if isinstance(child, pyast.FunctionDef):
                inner = child.name
            if isinstance(child, pyast.UnaryOp) and isinstance(child.op, pyast.USub) and \
                    isinstance(child.operand, pyast.Constant):
                out.append((fn, child.lineno, 'a negative number'))
            if isinstance(child, pyast.Compare):
                left = child.left
                for op, right in zip(child.ops, child.comparators):
                    if (isinstance(op, (pyast.Lt, pyast.GtE)) and is_zero(right)) or \
                            (isinstance(op, (pyast.Gt, pyast.LtE)) and is_zero(left)):
                        out.append((fn, child.lineno, 'x < 0 or x >= 0'))
                    left = right
            walk(child, inner)

    walk(tree, '<module>')
    return out


def g_numbers(g):
    seen = set()
    for name in CORE:
        found = signed_uses(name)
        for fn, line, what in found:
            seen.add((name, fn))
        odd = [x for x in found if (name, x[0]) not in SIGNED]
        g.check('%s: no negative number, no counter that has to go below zero' % name,
                not odd, repr(odd[:3]))
    g.check('the two places that may compare with zero are the ones listed',
            seen == set(SIGNED), repr(sorted(seen)))
    g.check('the rule is written down for the port', 'NUMBERS.' in emit.__doc__ and
            'NUMBERS.' in classes.__doc__ and 'NUMBERS.' in sc.__doc__)
    fr = slrrc.front('package p;\nclass C extends T { }\n', 'case.java',
                     t_extra4.MemberProvider())
    scope = fr.scope
    g.check('"nothing" is NONE, NO_FIT or 0, never a negative number',
            emit.NONE == 0xFFFFFFFF and classes.NO_FIT == 0xFFFFFFFF and
            classes.type_score(scope, 'I', None) == classes.NO_FIT and
            classes.type_score(scope, 'I', STR) == classes.NO_FIT and
            classes.distance(scope, 'p.T', 'p.S') == classes.NO_FIT and
            classes.distance(scope, 'p.T', 'p.A') == 1 and
            classes.distance(scope, 'p.C', 'p.I') == 2 and
            classes.type_desc_end('(I', 2) == 0 and
            sc._string_end('"abc', 0, 4) == 0 and sc._string_end('"ab"', 0, 4) == 4 and
            sc._find('abc', 'x', 0) == 3 and sc._find('abc', 'c', 1) == 2)
    g.check('columns, float digits and resource ids without a negative subscript',
            sc.column_at('ab\ncd', 4) == 2 and sc.column_at('abc', 2) == 3 and
            sc.column_at('ab\n', 3) == 1 and
            sc.float_digits('12.50f') == ('12', '50') and sc.float_digits('10f') == ('10', '') and
            sc.float_digits('.5') == ('', '5') and sc.float_digits('5.') == ('5', '') and
            sc.float_digits('7.25F') == ('7', '25') and
            sc.rid_parts('cars.racers.Axis:0x0000FFB4r') == ('cars.racers.Axis', 0xFFB4))
    # the loops that counted down: parameters in reverse, the innermost local
    # first, releases from the highest local down, continue through a switch
    text = t_extra4.M('void g(int a, int b, int c) {\n\tint a2 = c;\n\t{ int b = a; a2 = b; }\n'
                      '\tfor (int i = 0; i < a; i++) { switch (i) { case 1: continue; } }\n}')
    res = slrrc.emit_trees(text, 'case.java', t_extra4.MemberProvider(), table_packs())
    lines = dump.dump_tree(res.classes[0].trees[0]).split('\n') if res.ok else []
    g.check('the loops that counted down still give the same nodes',
            lines[:12] == ['0 L4 declare 4', '1 L4 type "I"', '2 L4 init', '3 L4 local 1',
                           '4 L4 store', '5 L5 declare 5', '6 L5 type "I"', '7 L5 init',
                           '8 L5 local 3', '9 L5 store', '10 L5 local 5', '11 L5 local 4'] and
            '13 L5 release 5' in lines and lines[-6:-1] == [
                '%d L7 release 4' % (len(lines) - 6), '%d L7 release 3' % (len(lines) - 5),
                '%d L7 release 2' % (len(lines) - 4), '%d L7 release 1' % (len(lines) - 3),
                '%d L7 release 0' % (len(lines) - 2)], repr(lines[:14] + lines[-7:]))


# ---------------------------------------------------------------------------
# floats (finding 6)
# ---------------------------------------------------------------------------

# the settings each wrong build changes at all (tests/floatmodel.py; measured
# on 17,741 literal texts)
TOUCHES = {
    'fma': 'AB', 'x87-53': 'AB', 'x87-64': 'AB', 'x87-24': 'CD', 'signed': 'ABCD',
    'wide': 'ABCD', 'double-0.1': 'B', 'ftz': 'ABCD', 'toward-0': 'ABCD',
    'library': 'ABCD',
}


def g_floats(g):
    g.check('floatlit.self_test() finds nothing', floatlit.self_test() == [],
            repr(floatlit.self_test()[:2]))
    rows = [(r[0] + '.' + r[1], r[2:]) for r in floatlit.SELF_TEST]
    bad = []
    for text, want in rows:
        got = tuple(fm.convert(m, text) for m in fm.MODES)
        if got != tuple(want):
            bad.append(text)
    g.check('the %d rows of SELF_TEST are the bits of the exact model' % len(rows),
            len(rows) >= 12 and not bad, repr(bad))
    g.check('the four settings differ pairwise on some row',
            all(any(want[i] != want[j] for _, want in rows)
                for i in range(4) for j in range(i + 1, 4)))
    builds = fm.wrong_builds()
    g.check('the wrong builds are the ones this test knows',
            sorted(b.name for b in builds) == sorted(TOUCHES))
    for b in builds:
        missed = []
        beyond = []
        for k, m in enumerate(fm.MODES):
            hit = [text for text, want in rows if b.convert(m, text) != want[k]]
            if m in TOUCHES[b.name] and not hit:
                missed.append(m)
            if m not in TOUCHES[b.name] and hit:
                beyond.append(m)
        g.check('a build with %s fails the self test in the settings %s' % (
            b.name, ', '.join(TOUCHES[b.name])), not missed and not beyond,
            'not caught in %r, caught in %r' % (missed, beyond))
    # float_bits against the exact model: literals of the corpus and made ones
    texts = set(r[0] for r in rows)
    for e in harness.all_sources()[::20]:
        src = harness.load_source(e)
        for t in sc.scan(src, Diagnostics(False)):
            if t.kind == sc.T_FLOAT:
                texts.add(src[t.pos:t.pos + t.length])
    texts = sorted(texts)[::3]
    texts += ['2147483648.0', '4294967295.9', '16777216.5', '0.' + '0' * 44 + '1',
              '0.' + '0' * 37 + '11754944', '99999999999999999999.99999999999999999999']
    bad = []
    for text in texts:
        ip, fp = sc.float_digits(text)
        for m in fm.MODES:
            if floatlit.float_bits(m, ip, fp) != fm.convert(m, text):
                bad.append('%s %s' % (m, text))
    g.check('float_bits equals the exact model (%d literal texts, 4 settings)' % len(texts),
            len(texts) > 300 and not bad, repr(bad[:3]))
    doc = floatlit.__doc__
    g.check('the C forms name every intermediate result and the build settings',
            'float p = (float)d * s;' in doc and 'FP_CONTRACT' in doc and
            '-ffp-contract=off' in doc and 'self_test()' in doc and 'uint32_t ip' in doc)
    # a build that computes other bits compiles nothing
    keep = floatlit.SELF_TEST
    tmp = tempfile.mkdtemp(prefix='slrrc_review_')
    try:
        t6.put(os.path.join(tmp, 'm', 'src', 'A.java'), 'package p;\nclass A { }\n')
        floatlit.SELF_TEST = (('1', '9', 0x3FF33333, 0x3FF33334, 0x3FF33333, 0x3FF33333),)
        out, err = io.StringIO(), io.StringIO()
        code = cli.main(['--root', tmp, '--check'], out, err)
        g.check('the command line refuses to compile when the self test fails',
                code == 2 and 'float arithmetic' in err.getvalue() and
                'setting A, literal 1.9' in err.getvalue(), '%d %s' % (code, err.getvalue()))
    finally:
        floatlit.SELF_TEST = keep
        shutil.rmtree(tmp, ignore_errors=True)
    g.check('... and the self test is whole again', floatlit.self_test() == [])
    # ---- M9: the conversion read in the exe is the default, with its two exits
    g.check('the default setting is B, the conversion of the Build 940 exe',
            floatlit.DEFAULT_MODE == 'B' and floatlit.EXE_MODE == 'B' and
            slrrc.Options().float_mode == 'B' and
            cli.build_parser().get_default('float') == 'B' and
            cli.auto_modes() == ['B', 'A', 'C', 'D'] and
            sorted(floatlit.ORIGIN) == list(floatlit.MODES))
    edge = [(r[0] + '.' + r[1], r[2]) for r in floatlit.EDGE_TEST]
    bad = [(text, m) for text, want in edge for m in fm.MODES if fm.convert(m, text) != want]
    g.check('the rows of EDGE_TEST are the bits of the exact model under every setting',
            len(edge) >= 3 and not bad, repr(bad))
    lib = [b for b in builds if b.name == 'library'][0]
    g.check('a build that converts with a library function fails EDGE_TEST as well',
            [text for text, want in edge if lib.convert('B', text) != want] == ['.5', '.25'])
    prov = t_extra4.MemberProvider()
    bad = []
    for lit, bits, warns in (('.5', 0, True), ('.5f', 0, True), ('.999', 0, True),
                             ('0.5', 0x3F000000, False), ('5.', 0x40A00000, True),
                             ('0.0001', 0x38D1B718, False)):
        for strict in (False, True):
            res = slrrc.emit_trees(t_extra4.M('float q = %s;' % lit), 'case.java', prov,
                                   table_packs(), slrrc.Options(strict=strict))
            if strict and warns:
                if res.ok or codes(res) != ['E_UNSUPPORTED']:
                    bad.append('%s strict: %r' % (lit, codes(res)))
                continue
            line = dump.dump_tree(res.classes[0].trees[0]).split('\n')[1] if res.ok else ''
            if line != '1 L3 float 0x%08X' % bits or \
                    codes(res) != (['W_UNVERIFIED'] if warns else []):
                bad.append('%s: %s %r' % (lit, line, codes(res)))
    g.check('a literal without a digit before the point compiles to 0.0, with a warning '
            '(an error in strict mode); 0.0001 has the bits of the exe', not bad, repr(bad))
    bad = []
    long_fraction = '3' * 1100
    for m in fm.MODES:
        if not (floatlit.float_bits(m, '', '5') == 0 and
                floatlit.float_bits(m, '0', long_fraction) ==
                floatlit.float_bits(m, '0', long_fraction[:1024]) ==
                fm.convert(m, '0.' + long_fraction) == fm.convert(m, '0.' + '3' * 60)):
            bad.append(m)
    g.check('only the first 1,024 fraction digits are looked at, and no bit depends on it',
            floatlit.MAX_FRACTION_DIGITS == 1024 == fm.MAX_FRACTION_DIGITS and not bad,
            repr(bad))


# ---------------------------------------------------------------------------
# diagnostics (findings 3, 11, 13, 17)
# ---------------------------------------------------------------------------

class NoPack(packs.PackNames):
    def canonical(self, written_path):
        return None


def deep_call(n, fn):
    if n == 0:
        return fn()
    return deep_call(n - 1, fn)


def g_diagnostics(g):
    prov = t_extra4.MemberProvider()
    # ---- finding 3: a pack without a file
    text = 'package p;\nclass C {\n\tint r = some.pack:0x0000002Ar;\n\tint s = 1;\n' \
           '\tint t = some.pack:0x1r;\n}\n'
    for strict in (False, True):
        res = compile_(text, prov, strict, NoPack())
        g.check('a pack without a file: a warning at every literal, %s mode' % (
            'strict' if strict else 'default'),
            res.ok and at(res) == [('W_PACK', 3, 10), ('W_PACK', 5, 10)] and
            b'system.rpk' in res.class_file and b'some' not in res.class_file and
            'some.pack' in res.diagnostics[0].text, repr(at(res)))
    for label, pk in (('names every pack as written', packs.PackNames()),
                      ('has the table and no file list', table_packs())):
        res = compile_(text, prov, True, pk)
        g.check('no warning from a PackNames that ' + label,
                res.ok and res.diagnostics == [] and b'some\\pack.rpk' in res.class_file,
                repr(at(res)))
    doc = packs.PackNames.__doc__ + packs.PackNames.canonical.__doc__
    g.check('what None means is written at the interface',
            'INSTALL HAS NO FILE' in doc and 'not from the packs' in doc and 'W_PACK' in doc)
    # ---- M9: the id of a resource id, as the resource engine resolves it
    listing = ['system.rpk', 'cars.rpk', 'cars\\racers\\Duhen.rpk']
    tables = {'cars.rpk': ['system.rpk'],
              'cars\\racers\\Duhen.rpk': ['system.rpk', 'cars.rpk', 'missing.rpk']}

    def session():
        return packs.session_940(packs.ListedPackFiles(listing, tables), listing)

    rid_cases = [
        # what, literal, pool entry or None, code in default mode, code in strict mode
        ('an id with a high word of 0 and a low word', 'cars:0x00000005r',
         'cars.rpk:0x00000005', None, None),
        ('an id whose low 16 bits are 0', 'cars:0x00010000r',
         'system.rpk:0x00000000', 'W_UNVERIFIED', 'E_UNSUPPORTED'),
        ('a high word that names an external reference of the pack',
         'cars.racers.duhen:0x00020007r', 'cars.rpk:0x00000007', 'W_UNVERIFIED',
         'E_UNSUPPORTED'),
        ('a high word beyond the external references of the pack', 'cars:0x00050007r',
         'system.rpk:0x00000000', 'W_UNVERIFIED', 'E_UNSUPPORTED'),
        ('a high word that names a pack without a file', 'cars.racers.duhen:0x00030007r',
         None, 'E_UNSUPPORTED', 'E_UNSUPPORTED'),
    ]
    for what, lit, entry, code, code_strict in rid_cases:
        bad = []
        for strict in (False, True):
            res = slrrc.emit_trees(t_extra4.M('int q = %s;' % lit), 'case.java', prov, session(),
                                   slrrc.Options(strict=strict))
            want = code_strict if strict else code
            line = dump.dump_tree(res.classes[0].trees[0]).split('\n')[1] if res.ok else None
            want_line = None if (entry is None or want == 'E_UNSUPPORTED') else '1 L3 rid ' + entry
            if codes(res) != ([want] if want else []) or line != want_line:
                bad.append('%s: %r %r' % ('strict' if strict else 'default', codes(res), line))
        g.check('resource id: %s' % what, not bad, '; '.join(bad))
    res = slrrc.emit_trees(t_extra4.M('int q = cars:0x00010005r;'), 'case.java', prov,
                           packs.PackNames())
    g.check('resource id: a high word is an error for a PackNames that knows no external '
            'references', not res.ok and codes(res) == ['E_UNSUPPORTED'], repr(at(res)))
    e = [x for x in harness.all_sources() if x.source.endswith('/TimeAttackTest.java')]
    if e:
        e = e[0]
        res = slrrc.compile_source(harness.load_source(e), e.source, harness.provider(),
                                   harness.packs(), slrrc.Options(strict=True))
        g.check('TimeAttackTest still compiles in strict mode, byte for byte, with its two '
                'warnings', res.ok and [(d.code, d.line) for d in res.diagnostics] == [
                    ('W_PACK', 21), ('W_PACK', 22)] and
                res.class_file == oracle.load_class_bytes(e), repr(at(res)))
    else:
        g.check('TimeAttackTest is in the corpus', False)
    # ---- finding 11: three things strict mode let through
    cases = [
        ('a class that is its own superclass',
         'package p;\nclass Self extends Self {\n\tint a;\n}\n', ('E_TYPE', 2, 7)),
        ('... also when a member is looked up',
         'package p;\nclass Self extends Self {\n\tint a;\n\tvoid f() { a = 1; }\n}\n',
         ('E_TYPE', 2, 7)),
    ]
    for what, text, want in cases:
        for strict in (False, True):
            res = compile_(text, prov, strict)
            g.check('%s: an error in %s mode' % (what, 'strict' if strict else 'default'),
                    not res.ok and at(res) == [want] and res.class_file == b'', repr(at(res)))
    res = compile_('package p;\nclass X extends Y { }\nclass Y extends Z { }\n'
                   'class Z extends X { }\n', prov)
    g.check('three classes of a source in a ring: an error at each',
            not res.ok and at(res) == [('E_TYPE', 2, 7), ('E_TYPE', 3, 7), ('E_TYPE', 4, 7)],
            repr(at(res)))
    ring = Listed([mk('p.Q', 'p.X', [], [('q', 'I', False)], [])])
    res = compile_('package p;\nclass X extends Q {\n\tvoid f() { q = 1; }\n}\n', ring)
    g.check('a ring through a class of the provider: an error',
            not res.ok and at(res) == [('E_TYPE', 2, 7)], repr(at(res)))
    for strict in (False, True):
        res = compile_('package p;\nclass X extends Q {\n}\n', ring, strict)
        g.check('... also when no body looks a member up (%s mode)' % (
            'strict' if strict else 'default'),
            not res.ok and at(res) == [('E_TYPE', 2, 7)], repr(at(res)))
    res = compile_('package p;\nclass C {\n\tLoop1 l;\n\tint f() { return l.lf; }\n}\n', prov, True)
    g.check('a ring among classes of the provider alone is not an error of this source',
            res.ok and res.diagnostics == [], repr(at(res)))
    static_cases = [
        ('an instance field in a static method', 'W',
         t_extra4.M('static void g() { x = 1; }')),
        ('an inherited instance field in a static method', 'W',
         t_extra4.M('static int g() { return base; }')),
        ('an instance field as an argument in a static method', 'W',
         t_extra4.M('static void g() { nosuch(x); }')),
        ('an instance field at the head of a name in a static method', 'W',
         t_extra4.M('static int g() { return next.x; }')),
        ('an instance field in the initialiser of a static field', 'W',
         t_extra4.M('static int q = x;')),
        ('a static field in a static method', 'ok',
         t_extra4.M('static int g() { return SA + ST.x; }')),
        ('a local that hides an instance field in a static method', 'ok',
         t_extra4.M('static int g(int x) { int base = x; return base; }')),
        ('an instance field of another object in a static method', 'ok',
         t_extra4.M('static int g(T t) { return t.x + t.next.x; }')),
        ('an instance field in an instance method', 'ok', t_extra4.M('int g() { return x; }')),
        ('an instance field in the initialiser of an instance field', 'ok',
         t_extra4.M('int q = x;')),
        ('NUL in a string literal', 'W', t_extra4.S('nosuch("a\x00b");')),
        ('a control character in a string literal', 'W', t_extra4.S('nosuch("a\x01b");')),
        ('DEL in a string literal', 'W', t_extra4.S('nosuch("a\x7fb");')),
        ('a lone CR in a string literal', 'W', t_extra4.S('nosuch("a\rb");')),
        ('a TAB in a string literal', 'ok', t_extra4.S('nosuch("a\tb");')),
        ('a control character in a comment', 'ok', t_extra4.S('x = 1; // \x01 \x00')),
    ]
    for what, expect, text in static_cases:
        why = t_extra4.run_case((what, expect, text), prov)
        g.check('%s: %s' % (what, 'a warning, an error in strict mode' if expect == 'W'
                            else 'no diagnostic'), why is None, why or '')
    res = compile_(t_extra4.S('nosuch("a\x00b");'), prov)
    g.check('the NUL is copied into the utf8 entry all the same (default mode)',
            res.ok and b'\x03\x00\x00\x00a\x00b\x00' in res.class_file)
    # ---- finding 17: the order of the methods
    util = mk('p.Util', O, [], [], [('abs', '(I)I', True)])
    lp = Listed([util])
    pairs = [
        ('a class used only as the head of a name',
         '\tvoid f(int Util) { Util = 1; }\n', '\tvoid g(int a) { a = Util.abs(a); }\n', []),
        ('a class the source casts to',
         '\tvoid f(int A) { A = 1; }\n', '\tvoid g(T t) { link = (A) t; }\n', ['E_UNSUPPORTED']),
        ('a class the source creates',
         '\tvoid f(int S) { S = 1; }\n', '\tObject o = new S();\n', ['E_UNSUPPORTED']),
        ('the class of the source itself',
         '\tvoid f(int C) { C = 1; }\n', '\tint z;\n', ['E_UNSUPPORTED']),
    ]
    for what, one, two, want in pairs:
        first = compile_('package p;\nclass C extends T {\n' + one + two + '}\n', lp, True)
        second = compile_('package p;\nclass C extends T {\n' + two + one + '}\n', lp, True)
        g.check('a variable named like %s: the same diagnostics whichever method comes first'
                % what, codes(first) == codes(second) == want and
                first.ok == second.ok == (not want), '%r / %r' % (at(first), at(second)))
    # ---- finding 13: a caller deep in the stack
    texts = [
        'package p;\nclass C { int f; void m() { f = %s; } }\n' % ' + '.join(['1'] * 150),
        'package p;\nclass C { int f; void m() { f = %s; } }\n' % ' + '.join(['1'] * 185),
        'package p;\nclass C { int f = %s1%s; }\n' % ('(' * 100, ')' * 100),
        'package p;\nclass C { int f; void m() { %s f = 1; %s } }\n' % ('{' * 60, '}' * 60),
        'package p;\nclass C extends T { void m(int a) { x = t(a).f(a)[a].x; } }\n',
    ]
    limit = sys.getrecursionlimit()
    here = 0                            # how deep this frame stands
    frame = sys._getframe()
    while frame is not None:
        here += 1
        frame = frame.f_back
    escaped = []
    outcomes = set()
    try:
        sys.setrecursionlimit(1000)
        for k, text in enumerate(texts):
            for depth in range(560, 975 - here, 5):
                try:
                    res = deep_call(depth, lambda: compile_(text, prov))
                    outcomes.add('ok' if res.ok else ','.join(sorted(set(codes(res)))))
                except RecursionError:
                    escaped.append((k, depth + here))
    finally:
        sys.setrecursionlimit(limit)
    g.check('a caller up to 975 frames deep under a limit of 1000 gets a result, never '
            'RecursionError', not escaped and outcomes == {'ok', 'E_LIMIT'},
            '%r %r' % (escaped[:3], sorted(outcomes)))


# ---------------------------------------------------------------------------
# driver (findings 7, 8, 9, 12, 16)
# ---------------------------------------------------------------------------

def run_raw(args, cwd=None):
    """The command line in an interpreter of its own, stdout and stderr as
    bytes (a pipe, as a shell redirection gives)."""
    p = subprocess.run([sys.executable, '-I', PKG] + list(args), stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, cwd=cwd or harness.ROOT)
    return p.returncode, p.stdout, p.stderr


def files_of(root):
    return sorted(k.replace('\\', '/') for k in t6.snapshot(root)) if os.path.isdir(root) else []


def manifest_agrees(cache):
    """'' when every entry of the manifest describes the class file that is
    there, else what does not."""
    path = os.path.join(cache, 'manifest.json')
    if not os.path.isfile(path):
        return 'no manifest'
    doc = json.loads(t6.get(path).decode('utf-8'))
    for e in doc['entries']:
        f = os.path.join(cache, e['output'].replace('/', os.sep))
        if not os.path.isfile(f):
            return '%s is not there' % e['output']
        data = t6.get(f)
        if len(data) != e['size'] or t6.sha(data) != e['sha256']:
            return '%s is not the file of its entry' % e['output']
    return ''


SRC3 = 'package java.game.mod;\r\npublic class %s\r\n{\r\n\tint n = %d;\r\n}\r\n'

MANIFEST_SHAPES = [
    '[]', 'null', '12', '"x"', '{}', '{"format": 1}',
    '{"format": true, "entries": [], "origins": {}}',
    '{"format": 1, "entries": 3}', '{"format": 1, "entries": [], "origins": []}',
    '{"format": 1, "entries": [3], "origins": {}}',
    '{"format": 1, "entries": [{"output": 3}], "origins": {}}',
    '{"format": 1, "entries": [{"output": "old/Thing.class"}], "origins": {}}',
    '{"format": 1, "entries": [{"output": "old/Thing.class", "compiled_against": 3}], '
    '"origins": {}}',
    '{"format": 1, "entries": [{"output": "old/Thing.class", "compiled_against": {}}], '
    '"origins": {}}',
    '{"format": 1, "entries": {}, "origins": {}}',
    '{"format": 2, "entries": [], "origins": {}}',
    '{"format": "1", "entries": [], "origins": {}}',
    '[' * 100000,
    'not json',
    '\xff\xfe\x00',
]


def g_driver(g, tmp):
    put, get, run_cli = t6.put, t6.get, t6.run_cli
    tree = os.path.join(tmp, 'T')
    src = os.path.join(tree, 'm', 'src')
    for n in 'ABC':
        put(os.path.join(src, n + '.java'), SRC3 % (n, 3))
    # ---- finding 7: a run that fails part-way
    cache = os.path.join(tmp, 'C')
    code, so, se = run_cli(['--root', tree, '--out', cache])
    first = dict((k, get(os.path.join(cache, k))) for k in files_of(cache))
    g.check('a first run writes three class files and a manifest that agrees with them',
            code == 0 and sorted(first) == ['m/A.class', 'm/B.class', 'm/C.class', 'manifest.json']
            and manifest_agrees(cache) == '', '%d %s %s' % (code, se.strip(), files_of(cache)))
    for n in 'ABC':
        put(os.path.join(src, n + '.java'), SRC3 % (n, 5))
    if os.name == 'nt':
        held = open(os.path.join(cache, 'm', 'B.class'), 'rb')     # another program has it open
        try:
            code, so, se = run_cli(['--root', tree, '--out', cache])
        finally:
            held.close()
        now = dict((k, get(os.path.join(cache, k))) for k in files_of(cache))
        g.check('a class file another program has open: that source fails, the others are '
                'compiled, exit code 1',
                code == 1 and 'B.class cannot be written' in se and '1 NOT READ OR NOT WRITTEN'
                in so and 'wrote 2 class files' in so, '%d %s / %s' % (code, so.strip(),
                                                                         se.strip()[:200]))
        g.check('... the manifest describes the files that are there, and no temporary file '
                'is left',
                sorted(now) == sorted(first) and manifest_agrees(cache) == '' and
                now['m/B.class'] == first['m/B.class'] and now['m/A.class'] != first['m/A.class']
                and now['m/C.class'] != first['m/C.class'],
                '%s %s' % (files_of(cache), manifest_agrees(cache)))
    cache2 = os.path.join(tmp, 'C2')
    os.makedirs(os.path.join(cache2, 'm', 'B.class'))               # a directory in its place
    code, so, se = run_cli(['--root', tree, '--out', cache2])
    g.check('a class file that cannot be written: the same on every system',
            code == 1 and 'B.class cannot be written' in se and
            files_of(cache2) == ['m/A.class', 'm/C.class', 'manifest.json'] and
            manifest_agrees(cache2) == '' and '(2 entries)' in so,
            '%d %s %s' % (code, files_of(cache2), se.strip()[:200]))
    cache3 = os.path.join(tmp, 'C3')
    os.makedirs(os.path.join(cache3, 'manifest.json'))
    code, so, se = run_cli(['--root', tree, '--out', cache3])
    g.check('a manifest that cannot be written: refused before a class file is written',
            code == 2 and 'cannot be written' in se and 'nothing was compiled' in se and
            files_of(cache3) == [] and so == '', '%d %s %s' % (code, files_of(cache3), se.strip()))
    # the same run in this interpreter, with a source that is gone when its
    # turn comes, and then with an interrupt behind the first source
    tree_r = os.path.join(tmp, 'R')
    for n in 'ABC':
        put(os.path.join(tree_r, 'm', 'src', n + '.java'), SRC3 % (n, 3))

    def in_process(cache_dir, before_compile, compiler=None):
        args = cli.build_parser().parse_args(['--root', tree_r, '--out', cache_dir])
        cli.settle_args(args)
        inst = install.Install(tree_r)
        inst.scan()
        before_compile()
        out, err = io.StringIO(), io.StringIO()
        con = cli.Console(out, err, False, False)
        provider = cli.load_provider(inst, args, con)
        jobs = cli.select(inst, args, con)
        real = cli.compile_source
        if compiler is not None:
            cli.compile_source = compiler(real)
        try:
            code = cli.run_compile(args, None, inst, provider, jobs, con)
        except KeyboardInterrupt:
            code = 'interrupted'
        finally:
            cli.compile_source = real
        return code, out.getvalue(), err.getvalue()

    cache_r = os.path.join(tmp, 'CR')
    code, so, se = in_process(cache_r, lambda: os.remove(os.path.join(tree_r, 'm', 'src',
                                                                      'B.java')))
    g.check('a source that cannot be read fails alone: the others are compiled, exit code 1',
            code == 1 and 'error: m/src/B.java cannot be read' in se and
            files_of(cache_r) == ['m/A.class', 'm/C.class', 'manifest.json'] and
            manifest_agrees(cache_r) == '' and '1 NOT READ OR NOT WRITTEN' in so,
            '%r %s %s' % (code, files_of(cache_r), se.strip()[:200]))
    put(os.path.join(tree_r, 'm', 'src', 'B.java'), SRC3 % ('B', 3))

    def second_is_interrupted(real):
        calls = []

        def compiler(*a):
            calls.append(1)
            if len(calls) == 2:
                raise KeyboardInterrupt()
            return real(*a)
        return compiler

    cache_i = os.path.join(tmp, 'CI')
    code, so, se = in_process(cache_i, lambda: None, second_is_interrupted)
    g.check('a run that ends with an exception still writes the manifest of what it wrote',
            code == 'interrupted' and files_of(cache_i) == ['m/A.class', 'manifest.json'] and
            manifest_agrees(cache_i) == '' and
            len(json.loads(get(os.path.join(cache_i, 'manifest.json')).decode('utf-8'))['entries'])
            == 1, '%r %s' % (code, files_of(cache_i)))
    blocked = os.path.join(tmp, 'blocked')
    os.makedirs(os.path.join(blocked, 'x'))
    try:
        cli.write_atomic(os.path.join(blocked, 'x'), b'data')
        g.check('write_atomic removes its temporary file when the rename fails', False,
                'no error')
    except OSError:
        g.check('write_atomic removes its temporary file when the rename fails',
                os.listdir(blocked) == ['x'], repr(os.listdir(blocked)))
    # ---- finding 8: a manifest.json of another shape
    crashes = []
    for i, shape in enumerate(MANIFEST_SHAPES):
        c = os.path.join(tmp, 'M%d' % i)
        put(os.path.join(c, 'manifest.json'), shape)
        code, so, se = run_cli(['--root', tree, '--out', c])
        if (code != 0 or 'Traceback' in se or 'is not a manifest of this format' not in se or
                manifest_agrees(c) != '' or '(3 entries)' not in so):
            crashes.append('%s: exit %d %s' % (shape[:40], code, se.strip()[-120:]))
    g.check('%d files that are no manifest of this format are replaced with a warning'
            % len(MANIFEST_SHAPES), not crashes, '; '.join(crashes[:2]))
    kept = os.path.join(tmp, 'K')
    code, so, se = run_cli(['--root', tree, '--out', kept])
    code2, so2, se2 = run_cli(['--root', tree, '--out', kept, os.path.join(src, 'A.java')])
    g.check('a manifest of this format is taken over: the entries of other outputs stay',
            code == 0 and code2 == 0 and se2 == '' and '(3 entries)' in so2 and
            manifest_agrees(kept) == '', '%d %s %s' % (code2, so2.strip(), se2.strip()))
    # ---- finding 9: output the code page cannot encode
    hi = os.path.join(tree, 'm', 'src', 'Hi.java')
    hi_text = 'package java.game.mod;\npublic class Hi { void f() { g("\x80\xe9"); } }\n'
    put(hi, hi_text)
    d = Diagnostics(False)
    toks = sc.scan(hi_text, d)
    code, out, err = run_raw(['--root', tree, '--dump', 'tokens', hi])
    want = dump.dump_tokens(hi_text, toks).encode('latin-1')
    g.check('--dump tokens to a pipe: the bytes of the source, LF line ends',
            code == 0 and out == want and b'\x80\xe9' in out and b'\r' not in out,
            '%d %r' % (code, err[-200:]))
    inst = install.Install(tree)
    inst.scan()
    iprov = install.InstallProvider(inst)
    iprov.load()
    res = slrrc.compile_source(hi_text, 'm/src/Hi.java', iprov, install.install_packs(inst))
    wants = {'ast': dump.dump_ast(res.ast), 'trees': dump.dump_trees(res.classes),
             'class': dump.dump_classes(res.classes)}
    bad = []
    for what in ('ast', 'trees', 'class'):
        code, out, err = run_raw(['--root', tree, '--dump', what, hi])
        if code != 0 or out != wants[what].encode('latin-1') or b'\r' in out or not out:
            bad.append(what)
    g.check('--dump ast, trees and class to a pipe: the bytes of the dump functions', not bad,
            repr(bad))
    os.remove(hi)
    odd = chr(0x416)
    tree_u = os.path.join(tmp, 'U')
    try:
        for n in ('A', 'Z', odd):
            put(os.path.join(tree_u, 'm', 'src', n + '.java'),
                SRC3 % ('Zh' if n == odd else n, 3))
        made = True
    except OSError:
        made = False                    # a file system that cannot hold the name
    if made:
        cache_u = os.path.join(tmp, 'CU')
        code, out, err = run_raw(['--root', tree_u, '--out', cache_u, '-v'])
        g.check('a source whose name the code page lacks, with -v: no UnicodeEncodeError',
                code == 0 and b'compiled 3 of 3 sources' in out and b'Traceback' not in err and
                len(files_of(cache_u)) == 4 and manifest_agrees(cache_u) == '',
                '%d %r' % (code, err[-200:]))
        code, out, err = run_raw(['--root', tree_u, '--check', '-v'])
        g.check('... with --check -v', code == 0 and b'checked 3 of 3 sources' in out and
                b'Traceback' not in err, '%d %r' % (code, err[-200:]))
        code, out, err = run_raw(['--root', tree_u, '--out', os.path.join(tmp, 'DU'), '--dry-run'])
        g.check('... with --dry-run', code == 0 and b'would compile 3 of the 3 sources' in out
                and b'Traceback' not in err, '%d %r' % (code, err[-200:]))
        cache_o = os.path.join(tmp, 'C' + odd)
        code, out, err = run_raw(['--root', tree, '--out', cache_o])
        g.check('an --out directory whose name the code page lacks',
                code == 0 and b'compiled 3 of 3 sources' in out and b'Traceback' not in err and
                manifest_agrees(cache_o) == '', '%d %r' % (code, err[-200:]))
    # ---- finding 12: an unreadable class file beside a source
    tree12 = os.path.join(tmp, 'T12')
    put(os.path.join(tree12, 'm', 'src', 'Base.java'), 'package p;\nclass Base { int a = 1; }\n')
    put(os.path.join(tree12, 'm', 'src', 'Sub.java'),
        'package p;\nclass Sub extends Base { void f() { a = 2; } }\n')
    code, so, se = run_cli(['--root', tree12, '--check'])
    g.check('two sources, one the superclass of the other, compile',
            code == 0 and 'checked 2 of 2 sources' in so and se == '', so.strip() + se.strip())
    for label, data in (('an empty', ''), ('a damaged', 'TUFA\x04\x00\x00\x00\x9d\x4f\x02\x00CONS'),
                        ('a text', 'this is no class file')):
        put(os.path.join(tree12, 'm', 'Base.class'), data)
        code, so, se = run_cli(['--root', tree12, '--check'])
        g.check('%s class file beside the source of the superclass: reported, and the class '
                'is known from its source' % label,
                code == 0 and 'checked 2 of 2 sources' in so and
                'm/Base.class: not a class file the reader accepts, ignored' in se and
                'E_TYPE' not in se, '%d %s / %s' % (code, so.strip(), se.strip()))
    cache12 = os.path.join(tmp, 'C12')
    code, so, se = run_cli(['--root', tree12, '--out', cache12, '--only-missing', '--dry-run'])
    lines = sorted(x for x in so.split('\n') if x.startswith('compile '))
    g.check('--only-missing --dry-run lists the source whose class file is unreadable',
            code == 0 and lines == ['compile m/src/Base.java -> m/Base.class',
                                    'compile m/src/Sub.java -> m/Sub.class'] and
            not os.path.exists(cache12), '%d %r' % (code, lines))
    code, so, se = run_cli(['--root', tree12, '--out', cache12, '--only-missing'])
    g.check('--only-missing compiles it',
            code == 0 and files_of(cache12) == ['m/Base.class', 'm/Sub.class', 'manifest.json']
            and get(os.path.join(tree12, 'm', 'Base.class')) == b'this is no class file',
            '%d %s' % (code, files_of(cache12)))
    good = get(os.path.join(cache12, 'm', 'Base.class'))
    put(os.path.join(tree12, 'm', 'Base.class'), good.decode('latin-1'))
    code, so, se = run_cli(['--root', tree12, '--only-missing', '--dry-run'])
    g.check('a class file the reader accepts is not missing',
            code == 0 and 'compile m/src/Base.java' not in so and
            'compile m/src/Sub.java' in so and se == '', so.strip() + se.strip())
    inst = install.Install(tree12)
    inst.scan()
    os.remove(os.path.join(tree12, 'm', 'Base.class'))     # gone between the scan and the load
    iprov = install.InstallProvider(inst)
    iprov.load()
    base = iprov.find('p.Base')
    g.check('a class file that cannot be opened is reported, and the class is known from '
            'its source', len(iprov.unreadable) == 1 and iprov.unreadable[0][0] == 'm/Base.class'
            and iprov.unreadable[0][1].startswith('cannot be read') and base is not None and
            [f.name for f in base.fields] == ['a'] and
            iprov.origin.get('p.Base') == 'm/src/Base.java', repr(iprov.unreadable))
    # ---- finding 16: the command line and the nesting limit
    deep = os.path.join(tree, 'm', 'src', 'Deep.java')

    def nested(depth):
        return 'package p;\nclass Deep {\n\tint a = %s1%s;\n}\n' % ('(' * depth, ')' * depth)

    prov = t_extra4.MemberProvider()
    for depth in (150, 180, 190):
        put(deep, nested(depth))
        code, so, se = run_cli(['--root', tree, '--check', deep])
        g.check('%d nested parentheses compile through the command line' % depth,
                code == 0 and 'checked 1 of 1 sources' in so and se == '', se.strip())
    for depth in (196, 205):
        text = nested(depth)
        put(deep, text)
        code, so, se = run_cli(['--root', tree, '--check', deep])
        here = compile_(text, prov)
        want = 'm/src/Deep.java:%d:%d: error E_LIMIT:' % (here.diagnostics[0].line,
                                                           here.diagnostics[0].column)
        g.check('%d nested parentheses: the command line reports what the library reports '
                'under the limit of the tests' % depth,
                code == 1 and se.count('\n') == 1 and se.startswith(want) and
                'nested deeper than 200' in se, '%d %s / expected %s' % (code, se.strip(), want))
    os.remove(deep)
    g.check('the command line sets the limit the tests run under',
            cli.RECURSION_LIMIT == sys.getrecursionlimit() == 20000)


# ---------------------------------------------------------------------------
# gaps (finding 10)
# ---------------------------------------------------------------------------

# Line rules where both candidate tokens stand on one line in every shipped
# source: here every token has a line of its own.
#                                                   line
T_LINES = ('package p;\n'                         # 1
           'class C extends T {\n'                # 2
           '\tvoid post(int a) {\n'               # 3
           '\t\ta\n'                              # 4
           '\t\t++\n'                             # 5
           '\t\t;\n'                              # 6
           '\t}\n'                                # 7
           '\tvoid assign(int a) {\n'             # 8
           '\t\ta\n'                              # 9
           '\t\t=\n'                              # 10
           '\t\t1\n'                              # 11
           '\t\t;\n'                              # 12
           '\t}\n'                                # 13
           '\tvoid local() {\n'                   # 14
           '\t\tint\n'                            # 15
           '\t\tk\n'                              # 16
           '\t\t=\n'                              # 17
           '\t\t2\n'                              # 18
           '\t\t;\n'                              # 19
           '\t}\n'                                # 20
           '}\n')

TREE_POST = '''
0 L4 local 1
1 L5 stmt postinc
2 L7 push_void
3 L7 release 1
4 L7 release 0
5 L7 ret_value
6 L7 release 1
7 L7 release 0
'''

TREE_ASSIGN = '''
0 L11 expr literal
1 L11 int 1
2 L9 local 1
3 L12 stmt assign
4 L13 push_void
5 L13 release 1
6 L13 release 0
7 L13 ret_value
8 L13 release 1
9 L13 release 0
'''

TREE_LOCAL = '''
0 L17 declare 1
1 L17 type "I"
2 L17 init
3 L18 expr literal
4 L18 int 2
5 L17 store
6 L20 push_void
7 L20 release 1
8 L20 release 0
9 L20 ret_value
10 L20 release 1
11 L20 release 0
'''

T_BOOL = 'package p;\nclass C extends T {\n\tvoid f() {\n\t\tover(true);\n\t}\n}\n'
TREE_BOOL = '''
0 L4 expr literal
1 L4 bool 1
2 L4 push_int 1
3 L4 call
4 L4 method p.C.over:(I)I
5 L4 stmt call
6 L5 push_void
7 L5 release 0
8 L5 ret_value
9 L5 release 0
'''

# The five pool orders no shipped class decides (notes/declarations.md 11):
# the type descriptors of step 2 in the order they are interned.
POOL_ORDERS = [
    ('a field type before the types of its initialiser',
     'package p;\nclass C {\n\tA f = new T();\n}\n', ['Lp.A;', 'Lp.T;']),
    ('a local type after the types of all its initialisers',
     'package p;\nclass C {\n\tvoid m() {\n\t\tA x = new T(), y = new S();\n\t}\n}\n',
     ['V', 'Lp.T;', 'Lp.S;', 'Lp.A;']),
    ('new T[n]: the array type before the types in n',
     'package p;\nclass C {\n\tvoid m() {\n\t\tObject o = new A[sz(new T())];\n\t}\n}\n',
     ['V', '[Lp.A;', 'Lp.T;', 'Ljava.lang.Object;']),
    ('(float) x: the cast type before the types in x',
     'package p;\nclass C {\n\tvoid m() {\n\t\tfloat v = (float) sz(new T());\n\t}\n}\n',
     ['V', 'F', 'Lp.T;']),
    ('x instanceof T: the types in x before T',
     'package p;\nclass C {\n\tvoid m(int a) {\n\t\ta = sz(new T()) instanceof A;\n\t}\n}\n',
     ['I', 'V', 'Lp.T;', 'Lp.A;']),
]

# Two methods that differ by one point of score: int for a float parameter
# costs 101, int for a String parameter and float for an int parameter 100.
# The better one is declared first, so that a tie would give the other.
G_CLASS = ('p.G', O, [], [], [
    ('pick', '(I%s)I' % STR, False), ('pick', '(FI)I', False),
    ('conv', '(%s)I' % STR, False), ('conv', '(F)I', False),
    ('back', '(I)I', False), ('back', '(F)I', False),
])
SCORES = [
    ('pick', ['I', 'I'], '(I%s)I' % STR, 100),
    ('conv', ['I'], '(%s)I' % STR, 100),
    ('back', ['I'], '(I)I', 0),
    ('back', ['F'], '(F)I', 0),
]

# (name, module, text that occurs once, its replacement)
MUTANTS = [
    ('the line of x++ is the token behind it', 'emit',
     "            self.put_imm(top, X_POSTINC if self.toks[e.op_tok].kind == P_INC "
     "else X_POSTDEC,\n                         e.tok1)",
     "            self.put_imm(top, X_POSTINC if self.toks[e.op_tok].kind == P_INC "
     "else X_POSTDEC,\n                         e.tok1 + 1)"),
    ('the line of an assignment is the last token of its value', 'emit',
     "self.put_imm(top, ASSIGN_KIND[op], e.tok1 + 1)",
     "self.put_imm(top, ASSIGN_KIND[op], e.tok1)"),
    ('the nodes of a declaration carry the line of the declared name', 'emit',
     "tok = d.name_tok + 1 + 2 * d.dims()", "tok = d.name_tok"),
    ('the type of instanceof is interned before its operand', 'decl',
     "        expr_types(pool, scope, e.a)\n        pool.utf8(classes.descriptor(scope, e.b, 0))",
     "        pool.utf8(classes.descriptor(scope, e.b, 0))\n        expr_types(pool, scope, e.a)"),
    ('int for a float parameter costs 100', 'classes',
     "            return 101\n", "            return 100\n"),
    ('true and false have no type as arguments', 'emit',
     "            return 'I'                          # int, resource id, true, false",
     "            if t == KW_TRUE or t == KW_FALSE:\n                return None\n"
     "            return 'I'"),
]
SWAPPED = {
    'emit': (emit, ('emit_class',)),
    'decl': (decl, ('declare_class',)),
    'classes': (classes, ('lookup_method', 'find_method', 'find_field', 'has_method')),
}


def gap_checks():
    """[(name, None or what is wrong)]: the rules the six mutants change,
    each in a case written out by hand."""
    out = []
    prov = Listed([mk(*G_CLASS)])

    def check(what, cond, detail=''):
        out.append((what, None if cond else (str(detail) or 'failed')))

    res = slrrc.emit_trees(T_LINES, 'case.java', prov, table_packs(), slrrc.Options(strict=True))
    for k, (name, want) in enumerate((('x++', TREE_POST), ('an assignment', TREE_ASSIGN),
                                      ('a local declaration', TREE_LOCAL))):
        diff = first_difference(dump.dump_tree(res.classes[0].trees[k]), want) if res.ok else \
            repr(at(res))
        check('one token per line, written out by hand: ' + name, diff == '', diff)
    res = slrrc.emit_trees(T_BOOL, 'case.java', prov, table_packs())
    diff = first_difference(dump.dump_tree(res.classes[0].trees[0]), TREE_BOOL) if res.ok else \
        repr(at(res))
    check('true as an argument is typed: the call ends in a method ref',
          diff == '' and codes(res) == ['W_UNVERIFIED'], diff)
    for what, text, want in POOL_ORDERS:
        fr = slrrc.front(text, 'case.java', prov, slrrc.Options(strict=True))
        got = []
        if fr.ok:
            cb = fr.classes[0]
            got = [e.text for e in cb.pool.entries[cb.pool_steps[0]:cb.pool_steps[1]]]
        check('pool order: ' + what, got == want, '%r, expected %r' % (got, want))
    fr = slrrc.front('package p;\nclass C extends T { }\n', 'case.java', prov)
    for name, args, want, score in SCORES:
        hit = classes.lookup_method(fr.scope, 'p.G', name, args)
        got = None if hit.method is None else hit.method.desc
        check('lookup: %s(%s) binds to %s with score %d' % (name, ','.join(args), want, score),
              got == want and hit.score == score and not hit.tie,
              '%r score %d tie %r' % (got, hit.score, hit.tie))
    res = slrrc.emit_trees(t_extra4.S('G g = null;\nx = g.pick(a, a);\nx = g.conv(a);'),
                           'case.java', prov, table_packs(), slrrc.Options(strict=True))
    lines = dump.dump_tree(res.classes[0].trees[0]).split('\n') if res.ok else []
    check('... and the emitter writes those refs',
          'method p.G.pick:(I%s)I' % STR in ' '.join(lines) and
          'method p.G.conv:(%s)I' % STR in ' '.join(lines), repr(at(res)))
    return out


def patched(module, old, new):
    """The module with one piece of its source replaced, compiled in memory."""
    with open(module.__file__, 'r', encoding='utf-8') as f:
        src = f.read()
    if src.count(old) != 1:
        return None
    mod = types.ModuleType(module.__name__)
    mod.__package__ = 'slrrc'
    mod.__file__ = module.__file__
    exec(compile(src.replace(old, new), module.__file__, 'exec'), mod.__dict__)
    return mod


def one_token_per_line(opts):
    """(sources, sources whose rewritten text compiles to the bytes of the
    spike, failures)."""
    prov = harness.provider()
    pk = harness.packs()
    env = spike_compiler.Env()
    n = same = 0
    bad = []
    passed = []
    for e in harness.selected(harness.all_sources(), opts):
        text = harness.load_source(e)
        toks = sc.scan(text, Diagnostics(False))[:-1]
        spread = '\n'.join(text[t.pos:t.pos + t.length] for t in toks) + '\n'
        n += 1
        res = slrrc.compile_source(spread, e.source, prov, pk, slrrc.Options(float_mode='A'))
        try:
            theirs = spike_compiler.compile_source(spread, env, 'A', 'table', name=e.source)[0]
        except Exception as x:      # noqa: the spike raises for everything
            bad.append('%s: the spike does not compile it: %s' % (e.source, str(x)[:100]))
            continue
        if res.ok and res.class_file == theirs:
            same += 1
            passed.append(e.source)
        else:
            bad.append('%s: one token per line: %s' % (
                e.source, at(res)[:1] if not res.ok else 'other bytes than the spike'))
    return n, same, bad, passed


def g_gaps(g):
    for what, why in gap_checks():
        g.results.append((what, why))
    caught = 0
    for name, which, old, new in MUTANTS:
        module, attrs = SWAPPED[which]
        mod = patched(module, old, new)
        if mod is None:
            g.check('mutant: ' + name, False, 'its text does not occur exactly once in %s.py'
                    % which)
            continue
        saved = [(a, getattr(module, a)) for a in attrs]
        for a in attrs:
            setattr(module, a, getattr(mod, a))
        try:
            failing = [what for what, why in gap_checks() if why is not None]
        except Exception as x:
            failing = ['exception %r' % (x,)]
        finally:
            for a, v in saved:
                setattr(module, a, v)
        caught += bool(failing)
        g.check('mutant caught: ' + name, bool(failing), 'every check passes with it')
    after = [what for what, why in gap_checks() if why is not None]
    g.check('the package is itself again after the mutants', not after, repr(after))
    return caught


# ---------------------------------------------------------------------------
# readme (finding 20)
# ---------------------------------------------------------------------------

def g_readme(g):
    path = os.path.join(harness.ROOT, 'README.md')
    with open(path, 'r', encoding='utf-8') as f:
        text = f.read()
    g.check('the README says how to point the tests at another corpus',
            'SLRR_CORPUS' in text and 'python -I lib\\oracle.py build' in text)
    g.check('the README says which Python, and what the nesting limit asks of a caller',
            'Python 3.10 or later' in text and 'sys.setrecursionlimit' in text and
            'parser.MAX_DEPTH' in text)
    blocks = text.split('```python\n')[1:]
    example = [b.split('```')[0] for b in blocks if 'install.Install(' in b]
    ok = False
    detail = 'no example with install.Install('
    if example:
        code = example[0].replace('D:\\SLRR-test', oracle.corpus_root())
        ns = {}
        limit = sys.getrecursionlimit()
        try:
            exec(compile(code, 'README.md', 'exec'), ns)
            res = ns['result']
            shipped = oracle.load_class_bytes('sl/Scripts/game/Trigger.class')
            ok = (res.ok and res.class_file == shipped and res.class_names ==
                  ['java.game.Trigger'] and len(res.consulted) > 3)
            detail = 'ok %r, %d bytes' % (res.ok, len(res.class_file))
        except Exception as x:
            detail = 'exception %r' % (x,)
        finally:
            sys.setrecursionlimit(limit)
    g.check('the library example of the README runs and gives the shipped Trigger.class',
            ok, detail)


# ---------------------------------------------------------------------------

GROUPS = ('results', 'provider', 'parser', 'reader', 'numbers', 'floats', 'diagnostics',
          'driver', 'gaps', 'readme')


def run(opts):
    oc = harness.Outcome('review')
    groups = dict((n, Group(n)) for n in GROUPS)
    tmp = tempfile.mkdtemp(prefix='slrrc_review_')
    caught = 0
    work = [
        ('results', lambda g: g_results(g)), ('provider', lambda g: g_provider(g)),
        ('parser', lambda g: g_parser(g)), ('reader', lambda g: g_reader(g)),
        ('numbers', lambda g: g_numbers(g)),
        ('floats', lambda g: g_floats(g)), ('diagnostics', lambda g: g_diagnostics(g)),
        ('driver', lambda g: g_driver(g, tmp)), ('readme', lambda g: g_readme(g)),
    ]
    try:
        for name, fn in work:
            try:
                fn(groups[name])
            except Exception as x:              # a check that raises is a check that fails
                groups[name].check('the checks of this group run to their end', False,
                                   '%s: %s' % (type(x).__name__, x))
        try:
            caught = g_gaps(groups['gaps'])
        except Exception as x:
            groups['gaps'].check('the checks of this group run to their end', False,
                                 '%s: %s' % (type(x).__name__, x))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    n, same, bad, spread_passed = one_token_per_line(opts)
    oc.failures.extend(bad[:20])
    parts = []
    passed = []
    all_ok = True
    for name in GROUPS:
        g = groups[name]
        good = g.passed()
        passed += ['%s: %s' % (name, w) for w in good]
        for what, why in g.results:
            if why is not None:
                oc.failures.append('%s: %s: %s' % (name, what, why[:300]))
                all_ok = False
        part = '%s %d/%d' % (name, len(good), len(g.results))
        if name == 'gaps':
            part += ', %d/%d mutants caught, one token per line %d/%d sources equal to the ' \
                    'spike' % (caught, len(MUTANTS), same, n)
        parts.append(part)
        oc.notes.append('%s: %s' % (name, '; '.join(w for w, _ in g.results)))
    oc.text = '; '.join(parts)
    oc.ok = (all_ok and caught == len(MUTANTS) == 6 and same == n and
             (opts.only is not None or n == 3454) and
             all(len(groups[name].results) > 0 for name in GROUPS))
    oc.passed['review.pass'] = passed
    oc.passed['review_lines.pass'] = spread_passed
    return oc


if __name__ == '__main__':
    sys.exit(harness.main_single(run, sys.argv[1:]))
