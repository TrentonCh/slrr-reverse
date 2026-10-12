#!/usr/bin/env python3
r"""t_extra5.py - checks of the writer and of the class file reader outside the
corpus.

    python -I tests/t_extra5.py [-v] [--update]
    python -I tests/run_tests.py extra5

Milestone M5 compares bytes of sources the game's compiler accepted.  These
checks look at what the corpus cannot show:

  hand       one source with two classes whose pool is written out by hand,
             entry by entry, from the rules of notes/declarations.md 12 (the
             five steps), with its rows; and the bytes of its second class
             written out by hand from notes/foundation.md 3, as hexadecimal.
             Neither comes from running anything.
  outputs    every synthetic source of t_extra.py and t_extra4.py (256 cases,
             among them every construct of appendix B that compiles with a
             warning), compiled to bytes in default and in strict mode: when
             the result is ok the strict reader of the foundation reads the
             file back, validate() finds nothing, it serialises to the same
             bytes, and the pool is complete after its five steps; when it is
             not ok there are no bytes.
  reader     slrrc/classfile.py against lib/tufa.py: every class file of the
             corpus read by both gives the same pool, rows, CLSS record and
             nodes; damaged variants of six class files (every truncation,
             single bytes changed, bytes inserted, a tail appended) are
             accepted by both or rejected by both, and never raise.
  details    the build number of the header, bytes and text as input, the
             result of a source with an error, characters that are no bytes,
             option values that do not exist, the canonical declaration text.
  mutations  every 11th source of the corpus, 20 lines moved or copied and 20
             token edits each (the texts of t_extra4.py), compiled in default
             mode by the package and by spec/spike_compiler.py.  No exception;
             every output of the package passes the checks of `outputs`;
             whenever both compile the text the BYTES are equal; and where
             only one of them compiles it the reason is a known one.

Headline:
  extra5           hand 3/3; outputs 512/512 runs well-formed; reader 2921/2921
                   class files equal to lib/tufa, N damaged variants, 0
                   disagreements; details 0 failures; mutations 12560 texts, 0
                   exceptions, 0 differences from the spike
"""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness              # noqa: E402
import oracle               # noqa: E402  lib
import symbols              # noqa: E402  lib
import tufa                 # noqa: E402  lib
import spike_compiler       # noqa: E402  spec
import t5_files             # noqa: E402
import t_extra              # noqa: E402
import t_extra4             # noqa: E402

import slrrc                            # noqa: E402
from slrrc import classes               # noqa: E402
from slrrc import classfile             # noqa: E402
from slrrc import dump                  # noqa: E402
from slrrc import packs                 # noqa: E402
from slrrc import pool as pl            # noqa: E402

# ---------------------------------------------------------------------------
# a class file written out by hand
# ---------------------------------------------------------------------------
# The classes p.T, p.A and p.I are those of t_extra4.MemberProvider.  The pack
# names come from a table with the one entry packs\Things.rpk.

HAND_SOURCE = (
    'package p;\n'                                  # 1
    'class C extends T implements I {\n'            # 2
    '\tstatic int N = 5;\n'                         # 3
    '\tString s = "hi";\n'                          # 4
    '\tC(int a, T t) {\n'                           # 5
    '\t\tx = a;\n'                                  # 6
    '\t\tt.next.x = N;\n'                           # 7
    '\t\tthis.m();\n'                               # 8
    '\t\tt.undefined(new A());\n'                   # 9
    '\t}\n'                                         # 10
    '\tstatic int g(float f) {\n'                   # 11
    '\t\treturn ST.x;\n'                            # 12
    '\t}\n'                                         # 13
    '\tvoid h() {\n'                                # 14
    '\t\ts = "there";\n'                            # 15
    '\t\tobj(this);\n'                              # 16
    '\t\tint k = packs.things:0x00000007r;\n'       # 17
    '\t}\n'                                         # 18
    '}\n'                                           # 19
    'class D {\n'                                   # 20
    '\tD() { }\n'                                   # 21
    '}\n')

# Class C.  Trees in tree order: 0 the initialiser of N, 1 the initialiser of
# s, 2 the constructor, 3 g, 4 h.
#   step 1  the class, the superclass, the interface
#   step 2  field types (I, String), parameter types (I is there, T), new A,
#           parameter F of g (its return type I is there), return type V of h,
#           (the local int k: there); at the end of the body LI; for the
#           interface, and no own type: C is the first class of the source
#   step 3  N, s; descriptor and name of the constructor, of g, of h
#   step 4  static methods first: g (ST, then x through a receiver of type T),
#           then the instance methods: the constructor (x and N of the own
#           class; t.next.x: next, x is there; this as a value: LC;) and h (s,
#           obj), then the field initialisers: nothing
#   step 5  tree order: "hi" (tree 1), m and undefined (tree 2), "there" and
#           the pack with its resource (tree 4)
HAND_C = r'''class p.C entries 46 steps 6 13 21 40 46
0 utf8 "p.C"
1 class 0
2 utf8 "p.T"
3 class 2
4 utf8 "p.I"
5 class 4
6 utf8 "I"
7 utf8 "Ljava.lang.String;"
8 utf8 "Lp.T;"
9 utf8 "Lp.A;"
10 utf8 "F"
11 utf8 "V"
12 utf8 "Lp.I;"
13 utf8 "N"
14 utf8 "s"
15 utf8 "(ILp.T;)"
16 utf8 "<init>"
17 utf8 "(F)I"
18 utf8 "g"
19 utf8 "()V"
20 utf8 "h"
21 utf8 "ST"
22 nat 21 8
23 ref 3 22
24 utf8 "x"
25 nat 24 6
26 ref 3 25
27 ref 1 25
28 nat 13 6
29 ref 1 28
30 utf8 "next"
31 nat 30 8
32 ref 3 31
33 utf8 "Lp.C;"
34 nat 14 7
35 ref 1 34
36 utf8 "obj"
37 utf8 "(Ljava.lang.Object;)V"
38 nat 36 37
39 ref 1 38
40 utf8 "hi"
41 utf8 "m"
42 utf8 "undefined"
43 utf8 "there"
44 utf8 "packs\\Things.rpk"
45 rid 44 0x00000007
clss interface 0 name 1 super 3 interfaces [5]
field s flags 8 name 13 type 6 tree 0
field i flags 0 name 14 type 7 tree 1
method s flags 8 name 18 desc 17 tree 3 locals 1
method i flags 0 name 16 desc 15 tree 2 locals 3
method i flags 0 name 20 desc 19 tree 4 locals 2
'''

# Class D, the second class of the source: its own type LD; closes step 2.
HAND_D = r'''class p.D entries 7 steps 4 5 7 7 7
0 utf8 "p.D"
1 class 0
2 utf8 "java.lang.Object"
3 class 2
4 utf8 "Lp.D;"
5 utf8 "()"
6 utf8 "<init>"
clss interface 0 name 1 super 3 interfaces []
method i flags 0 name 6 desc 5 tree 0 locals 1
'''

# The bytes of class D: chunk tag, size, payload; all integers little endian.
HAND_D_HEX = '''
54554641 04000000 9D4F0200
434F4E53 61000000
    07000000
    00000000 03000000 702E44 00
    04000000 00000000
    00000000 10000000 6A6176612E6C616E672E4F626A656374 00
    04000000 02000000
    00000000 05000000 4C702E443B 00
    00000000 02000000 2829 00
    00000000 06000000 3C696E69743E 00
4D544844 1C000000
    00000000
    01000000  00000000 06000000 05000000 00000000 01000000
434C5353 14000000
    FFFFFFFF 00000000 01000000 03000000 00000000
54524545 2A000000
    01000000
    06000000
    27 1500 00000000
    23 1500
    08 1500 22000000
    2A 1500
    03 1500 00000000
    03 1500 00000000
'''


def first_difference(got, exp):
    got = got.split('\n')
    exp = exp.split('\n')
    k = 0
    while k < min(len(got), len(exp)) and got[k] == exp[k]:
        k += 1
    return 'line %d: %r / expected %r' % (
        k + 1, got[k] if k < len(got) else 'end', exp[k] if k < len(exp) else 'end')


def run_hand(prov):
    """(passed, total, failures) for the class file written out by hand."""
    bad = []
    passed = []
    pk = packs.TablePackNames(('packs\\Things.rpk',), None)
    res = slrrc.compile_source(HAND_SOURCE, 'C.java', prov, pk, slrrc.Options(strict=True))
    if not res.ok or res.diagnostics:
        d = res.diagnostics[0]
        return passed, 3, ['hand: line %d: %s %s' % (d.line, d.code, d.text)]
    for name, cb, want in (('pool and rows of the first class', res.classes[0], HAND_C),
                           ('pool and rows of a later class', res.classes[1], HAND_D)):
        got = dump.dump_class(cb)
        if got == want:
            passed.append(name)
        else:
            bad.append('hand: %s: %s' % (name, first_difference(got, want)))
    want = bytes.fromhex(''.join(HAND_D_HEX.split()))
    got = res.class_file[-len(want):]
    if got == want and len(res.class_file) > len(want):
        passed.append('bytes of a class')
    else:
        k = 0
        while k < min(len(got), len(want)) and got[k] == want[k]:
            k += 1
        bad.append('hand: bytes of class D differ at byte 0x%X of %d' % (k, len(want)))
    return passed, 3, bad


# ---------------------------------------------------------------------------
# every synthetic source, to bytes
# ---------------------------------------------------------------------------

def well_formed(res, text, prov, strict):
    """None, or what is wrong with the result of a compilation."""
    if not res.ok:
        if res.class_file != b'' or res.classes or res.class_names:
            return 'not ok, but bytes, classes or class names'
        if not any(d.error for d in res.diagnostics):
            return 'not ok without an error'
        return None
    if any(d.error for d in res.diagnostics):
        return 'ok with an error'
    front = slrrc.front(text, 'x.java', prov, slrrc.Options(strict=strict))
    return t5_files.check_output(res, text, [len(cb.pool.entries) for cb in front.classes])


def run_outputs():
    """(runs, well-formed runs, compiled runs, failures) over the synthetic
    sources of the two other extra tests."""
    bad = []
    runs = good = compiled = 0
    table = packs.TablePackNames(packs.TABLE_940, None)
    for label, cases, prov in (('extra', t_extra.CASES, t_extra.MapProvider()),
                               ('extra4', t_extra4.CASES, t_extra4.MemberProvider())):
        for name, expect, text in cases:
            for strict in (False, True):
                runs += 1
                try:
                    res = slrrc.compile_source(text, 'x.java', prov, table,
                                               slrrc.Options(strict=strict))
                    why = well_formed(res, text, prov, strict)
                except Exception as x:          # the compiler must not raise
                    bad.append('outputs: %s: %s: exception %r' % (label, name, x))
                    continue
                if why is None:
                    good += 1
                    compiled += res.ok
                else:
                    bad.append('outputs: %s: %s (%s): %s' % (
                        label, name, 'strict' if strict else 'default', why))
    return runs, good, compiled, bad


# ---------------------------------------------------------------------------
# the reader of the package against the reader of the foundation
# ---------------------------------------------------------------------------

def same_content(mine, theirs):
    """None when a classfile.BlobData and a tufa.Blob hold the same, else what
    differs."""
    if mine.build != theirs.build:
        return 'build'
    if mine.has_fild != ('FILD' in theirs.chunks) or mine.has_mthd != ('MTHD' in theirs.chunks):
        return 'chunk list'
    if len(mine.pool) != len(theirs.cons):
        return 'pool length'
    for e, t in zip(mine.pool, theirs.cons):
        if e.kind == pl.UTF8:
            ok = t == ('utf8', e.text)
        elif e.kind == pl.CLASS:
            ok = t == ('class', e.a)
        else:
            ok = t == ({pl.NAT: 'nat', pl.REF: 'ref', pl.RID: 'rid'}[e.kind], e.a, e.b)
        if not ok:
            return 'pool entry %r' % (t,)
    for a, b in ((mine.static_fields, theirs.static_fields),
                 (mine.instance_fields, theirs.instance_fields)):
        if [(r.flags, r.name, r.type, r.tree) for r in a] != [tuple(r) for r in b]:
            return 'field rows'
    for a, b in ((mine.static_methods, theirs.static_methods),
                 (mine.instance_methods, theirs.instance_methods)):
        if [(r.flags, r.name, r.desc, r.tree, r.locals) for r in a] != [tuple(r) for r in b]:
            return 'method rows'
    c = theirs.clss
    if (mine.interface, mine.name, mine.super, mine.interfaces) != (
            c.interface, c.name, c.super, list(c.interfaces)):
        return 'CLSS'
    if mine.tree_count != len(theirs.trees) or len(mine.trees) != len(theirs.trees):
        return 'tree count'
    for a, b in zip(mine.trees, theirs.trees):
        if a != [(n.op, n.slot, classfile.NONE if n.imm is None else n.imm) for n in b]:
            return 'nodes'
    return None


def tufa_accepts(data):
    """Does the reader of the foundation take the bytes for a class file?
    (parse() leaves the first two words of CLSS to validate().)"""
    try:
        blobs = tufa.parse(data)
    except tufa.TufaError:
        return False
    for b in blobs:
        if b.clss.marker != tufa.NONE or b.clss.interface not in (0, 1):
            return False
    return True


def run_reader():
    """(class files, equal ones, damaged variants, failures)."""
    bad = []
    files = equal = 0
    sizes = []
    for rel in oracle.find_class_files():
        data = oracle.load_class_bytes(rel)
        files += 1
        theirs = tufa.parse(data)
        mine = classfile.read(data, True)
        quick = classfile.read(data)
        why = None
        if not mine.ok or not quick.ok:
            why = 'rejected: %s' % (mine.error or quick.error)
        elif len(mine.blobs) != len(theirs):
            why = '%d blobs / %d' % (len(mine.blobs), len(theirs))
        else:
            for k, (a, b) in enumerate(zip(mine.blobs, theirs)):
                why = same_content(a, b)
                if why is None:
                    info = classfile.class_info(quick.blobs[k])
                    ref = harness.info_from_blob(b)
                    if info is None or classes.declaration_text(info) != \
                            classes.declaration_text(ref):
                        why = 'declarations'
                if why is not None:
                    why = 'blob %d: %s' % (k, why)
                    break
        if why is None:
            equal += 1
        elif len(bad) < 20:
            bad.append('reader: %s: %s' % (rel, why))
        sizes.append((len(data), len(theirs), any('FILD' in b.chunks for b in theirs), rel))
    # ---- damaged variants: both readers accept, or both reject
    sizes.sort()
    picks = [x[3] for x in sizes[:3]]
    picks += [x[3] for x in sizes if x[2]][:1]
    picks += [x[3] for x in sizes if x[1] > 1][:1]
    picks.append('sl/Scripts/game/Trigger.class')
    rng = random.Random(940)
    variants = 0
    disagree = 0
    accepted = 0
    for rel in picks:
        data = oracle.load_class_bytes(rel)
        n = len(data)
        cands = [data[:k] for k in range(0, n, 1 if n <= 4000 else 7)]
        for _ in range(400):
            k = rng.randrange(n)
            cands.append(data[:k] + bytes([rng.randrange(256)]) + data[k + 1:])
        for _ in range(100):
            k = rng.randrange(n)
            cands.append(data[:k] + bytes([data[k] ^ (1 << rng.randrange(8))]) + data[k + 1:])
        for _ in range(40):
            k = rng.randrange(n + 1)
            cands.append(data[:k] + bytes(rng.randrange(256) for _ in range(rng.randrange(1, 9)))
                         + data[k:])
        cands.append(data + b'\x00')
        cands.append(data + b'TUFA')
        cands.append(data + data)
        for v in cands:
            variants += 1
            try:
                mine = classfile.read(v, True)
                for b in mine.blobs:
                    classfile.class_info(b)
                classfile.read(v)
            except Exception as x:              # the reader must not raise
                bad.append('reader: %s, a damaged variant of %d bytes: exception %r' % (
                    rel, len(v), x))
                continue
            if mine.ok != tufa_accepts(v):
                disagree += 1
                if disagree <= 5:
                    bad.append('reader: %s, a damaged variant of %d bytes: the package %s, '
                               'lib/tufa.py %s (%s)' % (
                                   rel, len(v), 'accepts' if mine.ok else 'rejects',
                                   'rejects' if mine.ok else 'accepts', mine.error))
            accepted += mine.ok
    if tuple(classfile.OP_HAS_OPERAND) != tuple(tufa.HAS_IMM):
        bad.append('reader: the opcode table differs from lib/tufa.py')
    return files, equal, variants, accepted, disagree, bad


# ---------------------------------------------------------------------------
# details
# ---------------------------------------------------------------------------

T_DECLARATIONS = ('class p.T\n'
                  'super p.A\n'
                  'interface 0\n'
                  'implements p.I\n'
                  'field s ST Lp.T;\n'
                  'field i x I\n'
                  'field i next Lp.T;\n'
                  'field i arr [I\n'
                  'field i name Ljava.lang.String;\n'
                  'field i many [Lp.T;\n'
                  'method i get ()Lp.T;\n'
                  'method i set (ILp.T;)V\n'
                  'method i f (I)I\n')


def details(prov):
    bad = []
    table = packs.TablePackNames(packs.TABLE_940, None)
    text = t_extra4.M('int q = 1;\nvoid g() { q = 2; }')

    def comp(source, **kw):
        return slrrc.compile_source(source, 'case.java', prov, table, slrrc.Options(**kw))

    # the build number is the payload of the first chunk, and nothing else changes
    a = comp(text).class_file
    b = comp(text, build=0x24F5C).class_file
    if a[:12] != b'TUFA\x04\x00\x00\x00\x9d\x4f\x02\x00' or \
            b[:12] != b'TUFA\x04\x00\x00\x00\x5c\x4f\x02\x00' or a[12:] != b[12:]:
        bad.append('the build number of the header')
    # bytes and text are the same input
    if comp(text.encode('latin-1')).class_file != a or comp(bytearray(text, 'latin-1')).class_file != a:
        bad.append('bytes as input give other bytes than text')
    # the float setting reaches the float nodes and nothing else
    f = t_extra4.M('float q = 0.00065;')
    outs = [comp(f, float_mode=m).class_file for m in 'ABCD']
    if len(set(outs)) != 3 or outs[0] != outs[2] or len(set(len(x) for x in outs)) != 1:
        bad.append('the four float settings: %d different files' % len(set(outs)))
    if comp(f).class_file != outs[0]:
        bad.append('the default float setting is not A')
    # a source with an error: no bytes, the diagnostics, the consulted classes
    res = comp(t_extra4.S('x = nowhere;'))
    if res.ok or res.class_file != b'' or res.classes or res.class_names or \
            [d.code for d in res.diagnostics if d.error] != ['E_NAME'] or \
            'p.T' not in res.consulted:
        bad.append('the result of a source with an unknown name')
    res = comp('package p;\nclass C {\n\tint a = ;\n}\n')
    if res.ok or res.class_file != b'' or res.diagnostics[0].code != 'E_SYNTAX' or \
            res.diagnostics[0].line != 3:
        bad.append('the result of a source with a syntax error')
    # a text with a character that is no byte
    try:
        res = comp('package p;\nclass C {\n\tString s = "x' + chr(0x20AC) + 'y";\n}\n')
        if res.ok or [(d.code, d.line) for d in res.diagnostics] != [('E_LEX', 3)]:
            bad.append('a character above U+00FF: %r' % [(d.code, d.line)
                                                         for d in res.diagnostics])
        d = slrrc.declare_source('package p;\nclass C { }\n// ' + chr(0x416) + '\n', 'x', prov)
        if d.ok or slrrc.declared_names('package p;\nclass ' + chr(0x416) + ' { }\n') != []:
            bad.append('a character above U+00FF in declare_source or declared_names')
    except Exception as x:
        bad.append('a character above U+00FF raises %r' % (x,))
    # a byte above 0x7F in a string is copied (warning W_BYTE)
    res = comp(b'package p;\nclass C {\n\tString s = "caf\xe9";\n}\n')
    if not res.ok or [d.code for d in res.diagnostics] != ['W_BYTE'] or \
            b'\x04\x00\x00\x00caf\xe9\x00' not in res.class_file:
        bad.append('a byte above 0x7F in a string literal')
    # option values that do not exist are mistakes of the caller
    for kw in ({'float_mode': 'E'}, {'build': -1}, {'build': 1 << 32}):
        try:
            comp(text, **kw)
            bad.append('accepted: %r' % (kw,))
        except ValueError:
            pass
    # the names of the classes, one per blob
    res = comp('package p;\nclass C { }\ninterface J { }\nclass D extends C { }\n')
    if res.class_names != ['p.C', 'p.J', 'p.D'] or \
            [b.name for b in tufa.parse(res.class_file)] != res.class_names:
        bad.append('class names %r' % (res.class_names,))
    if slrrc.declared_names('package q.r;\nclass C { }\ninterface J { }\n') != ['q.r.C', 'q.r.J'] \
            or slrrc.declared_names('class C { }') != ['C'] or \
            slrrc.declared_names('class C {') != []:
        bad.append('declared_names')
    # what a cache hashes for a class
    got = classes.declaration_text(prov.find('p.T'))
    if got != T_DECLARATIONS:
        bad.append('declaration_text: %s' % first_difference(got, T_DECLARATIONS))
    obj = classes.declaration_text(prov.find('java.lang.Object'))
    if obj != 'class java.lang.Object\nsuper -\ninterface 0\n':
        bad.append('declaration_text of java.lang.Object: %r' % obj)
    # static fields first, as a class file holds them; the methods as they stand
    mixed = t_extra4.make_class('p.X', 'p.A', ['p.I', 'p.J'],
                                [('a', 'I', False), ('S', 'I', True), ('b', 'F', False)],
                                [('n', '()V', False), ('m', '(I)I', True)], True)
    got = classes.declaration_text(mixed)
    want = ('class p.X\nsuper p.A\ninterface 1\nimplements p.I\nimplements p.J\n'
            'field s S I\nfield i a I\nfield i b F\nmethod i n ()V\nmethod s m (I)I\n')
    if got != want:
        bad.append('declaration_text with mixed fields: %s' % first_difference(got, want))
    return bad


# ---------------------------------------------------------------------------
# mutations against the composition spike, byte for byte
# ---------------------------------------------------------------------------

class Tally(t_extra4.Tally):
    def __init__(self, prov, pk, env):
        t_extra4.Tally.__init__(self, prov, pk, env)
        self.malformed = 0

    def spike_bytes(self, text):
        """The bytes of the spike for a text, in a symbol universe of its own
        (t_extra4.Tally.spike says why)."""
        u = symbols.Universe()
        u.classes = dict(self.universe.classes)
        u.packages = dict((k, set(v)) for k, v in self.universe.packages.items())
        self.env._sym_uni = u
        try:
            return spike_compiler.compile_source(text, self.env, 'A', 'table', name='x')[0], None
        except Exception as x:      # noqa: the spike raises for everything
            return None, '%s: %s' % (type(x).__name__, str(x)[:120])

    def one(self, text, where):
        self.texts += 1
        try:
            res = slrrc.compile_source(text, 'x.java', self.prov, self.pk, slrrc.Options())
            why = well_formed(res, text, self.prov, False)
        except Exception as x:
            self.exceptions.append('%s: %r' % (where, x))
            return
        if why is not None:
            self.malformed += 1
            self.differences.append('%s: %s' % (where, why))
        data, why = self.spike_bytes(text)
        unknown_type = any(d.code == 'W_TYPE' for d in res.diagnostics)
        if res.ok and data is not None:
            if res.class_file == data:
                self.same += 1
            elif unknown_type:
                self.unknown_type += 1
            else:
                self.differences.append('%s: %s' % (
                    where, t5_files.why_different(res.class_file, data)))
        elif res.ok:
            self.only_package += 1
            single_import = any(d.text.startswith('import of a single class')
                                for d in res.diagnostics)
            if not (unknown_type or single_import):
                self.differences.append('%s: compiled, the spike says %s' % (where, why))
        elif data is not None:
            self.only_spike += 1
            d = [x for x in res.diagnostics if x.error][0]
            if not d.text.startswith(t_extra4.ON_PURPOSE):
                self.differences.append('%s: line %d %s %s, the spike compiles it' % (
                    where, d.line, d.code, d.text))


def run(opts):
    oc = harness.Outcome('extra5')
    prov = t_extra4.MemberProvider()
    h_passed, h_total, h_bad = run_hand(prov)
    oc.failures.extend(h_bad)
    runs, good, compiled, o_bad = run_outputs()
    oc.failures.extend(o_bad)
    files, equal, variants, accepted, disagree, r_bad = run_reader()
    oc.failures.extend(r_bad)
    d_bad = details(prov)
    for x in d_bad:
        oc.failures.append('detail: %s' % x)
    tally = Tally(harness.provider(), harness.packs(), spike_compiler.Env())
    t_extra4.mutations(tally)
    oc.failures.extend(tally.exceptions)
    oc.failures.extend(tally.differences)
    oc.text = ('hand %d/%d; outputs %d/%d runs well-formed; reader %d/%d class files equal to '
               'lib/tufa, %d damaged variants, %d disagreements; details %d failures; '
               'mutations %d texts, %d exceptions, %d differences from the spike' % (
                   len(h_passed), h_total, good, runs, equal, files, variants, disagree,
                   len(d_bad), tally.texts, len(tally.exceptions), len(tally.differences)))
    oc.ok = (len(h_passed) == h_total and good == runs == 512 and
             equal == files == 2921 and disagree == 0 and not r_bad and not d_bad and
             tally.texts == 12560 and not tally.exceptions and not tally.differences and
             tally.same > 1000)
    oc.passed['extra5.pass'] = ['hand ' + x for x in h_passed]
    oc.notes.append('synthetic sources: %d of %d runs end in a class file' % (compiled, runs))
    oc.notes.append('damaged variants both readers accept: %d of %d' % (accepted, variants))
    oc.notes.append('mutated texts both compile, with the same bytes: %d; both compile, '
                    'another descriptor for a type name that resolves nowhere: %d; only the '
                    'package compiles (unknown type, single import): %d; only the spike '
                    'compiles (refused on purpose): %d' % (
                        tally.same, tally.unknown_type, tally.only_package, tally.only_spike))
    return oc


if __name__ == '__main__':
    sys.exit(harness.main_single(run, sys.argv[1:]))
