#!/usr/bin/env python3
"""t_extra.py - checks of the front half outside the corpus.

    python -I tests/t_extra.py [-v] [--update]
    python -I tests/run_tests.py extra

The milestone tests only see sources the game's compiler accepted.  These
checks look at everything else:

  dialect    small sources, one per construct: the proven subset gives no
             diagnostic; a construct no shipped source uses gives W_UNVERIFIED
             (E_UNSUPPORTED in strict mode) when notes/compiler_spec.md
             appendix B states a default, E_UNSUPPORTED in both modes when it
             does not; anything outside the grammar E_SYNTAX / E_LEX; limits
             E_LIMIT; unknown types W_TYPE / E_TYPE.  A case passes when both
             modes give exactly the expected codes.
  deletions  the strictness sample of notes/grammar.md 10.1 (N1): every 7th
             source with every 11th of its  ; { } ( ) [ ]  tokens blanked
             out.  The compiler must end in a diagnostic or in a tree, never
             in an exception, and must accept exactly what lib/parser.py
             accepts (one deletion of 3,068 leaves a valid program).
  mutations  every 23rd source, with a fixed seed: 20 lines moved or copied,
             20 random byte edits (scanner only) and 20 random token edits.
             No exception; whenever both the compiler and lib/lexer.py /
             lib/parser.py accept the text they give the same tokens / the
             same tree; the compiler rejects only what lib rejects, except
             for the constructs it refuses on purpose (listed in
             notes/impl_m1-m3.md); and for an accepted text the pool prefix
             and the rows equal the ones of analysis/decl/poolmodel.py.
  details    a few results checked value for value: descriptors, the order of
             type name resolution, a pool prefix and rows written out by
             hand, the package in an interpreter of its own.

Headline:
  extra            dialect 181/181 cases; deletions 3067/3068 rejected, lib 3067;
                   mutations 9060 texts, 0 exceptions, 0 differences from lib
"""
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness              # noqa: E402
import lexer                # noqa: E402  lib
import parser as libparser  # noqa: E402  lib
import universe             # noqa: E402  analysis/decl
import t2_parse             # noqa: E402
import t3_decl              # noqa: E402

import slrrc                            # noqa: E402
from slrrc import classes               # noqa: E402
from slrrc import parser as ps          # noqa: E402
from slrrc import scanner as sc         # noqa: E402
from slrrc.diag import Diagnostics      # noqa: E402

# ---------------------------------------------------------------------------
# dialect cases
# ---------------------------------------------------------------------------


class MapProvider(classes.ClassProvider):
    """A handful of classes for the synthetic sources."""

    def __init__(self):
        self.known = {}
        for name in ('java.lang.Object', 'java.lang.String', 'java.util.Vector',
                     'p.A', 'p.B', 'p.T', 'p.I', 'p.J', 'a.b.K'):
            self.known[name] = classes.ClassInfo(name)

    def find(self, fqn):
        return self.known.get(fqn)


def U(text):
    """A whole source."""
    return text


def M(members):
    """Members of a class."""
    return 'package p;\nclass C extends A {\n' + members + '\n}\n'


def S(stmts):
    """Statements of a method body."""
    return M('int x, y, a, b, c, d, e, n;\nint[] arr;\nT t;\nvoid f() {\n' + stmts + '\n}')


def E(expr):
    """An expression."""
    return S('x = ' + expr + ';')


def both(errors, warnings):
    return ((errors, warnings), (errors, warnings))


# expectation -> ((errors, warnings) in default mode, (errors, warnings) in strict mode)
EXPECT = {
    'ok': both((), ()),
    'W': (((), ('W_UNVERIFIED',)), (('E_UNSUPPORTED',), ())),
    'U': both(('E_UNSUPPORTED',), ()),
    'X': both(('E_SYNTAX',), ()),
    'L': both(('E_LEX',), ()),
    'LIM': both(('E_LIMIT',), ()),
    'B': both((), ('W_BYTE',)),
    'T': (((), ('W_TYPE',)), (('E_TYPE',), ())),
    'TT': both(('E_TYPE',), ()),
    # no package, and then something that is an error anyway
    'WX': ((('E_SYNTAX',), ('W_UNVERIFIED',)), (('E_UNSUPPORTED',), ())),
    # written with its package, and unknown
    'WT': (((), ('W_TYPE', 'W_UNVERIFIED')), (('E_UNSUPPORTED',), ())),
}

CASES = [
    # ---- the proven subset: no diagnostic ---------------------------------
    ('ok minimal class', 'ok', U('package p;\nclass C { }\n')),
    ('ok public class with extends and implements', 'ok',
     U('package p;\nimport java.util.*;\npublic class C extends A implements I, J { }\n')),
    ('ok interface', 'ok', U('package p;\npublic interface C { }\n')),
    ('ok several classes', 'ok', U('package p;\nclass C { D d; }\nclass D extends C { C c; }\n')),
    ('ok import with a colon', 'ok', U('package p;\nimport parts:part.root.BodyPart.*;\nclass C { }\n')),
    ('ok import of a package without classes', 'ok', U('package p;\nimport no.such.pack.*;\nclass C { }\n')),
    ('ok class in java.lang without import', 'ok', M('String s; Object o;')),
    ('ok class through an import', 'ok', U('package p;\nimport java.util.*;\nclass C { Vector v; }\n')),
    ('ok abstract class', 'ok', U('package p;\nabstract public class C { }\n')),
    ('ok fields', 'ok', M('int a, b = 1, c;\nfinal static float F = 1.5;\nT[] ts;\nprivate int p;')),
    ('ok native and abstract field', 'ok', M('static native int a;\nabstract T t;')),
    ('ok field with side effect', 'ok', M('static int n;\nfinal static int A = ++n;')),
    ('ok methods', 'ok', M('public void f() { }\nint g(int a, float b, T c, int[] d) { return a; }\n'
                           'T[] h() { return null; }\npublic static void s() { }')),
    ('ok bodiless method and constructor', 'ok', M('public void f();\npublic C();\npublic native int g(T t);')),
    ('ok method modifiers', 'ok', M('void f() { }\nprivate void g() { }\nprivate static void h() { }\n'
                                    'native static int i();\npublic static native int j();\nC() { }')),
    ('ok constructors', 'ok', M('public C() { }\npublic C(int a) { this(); }\npublic C(T t) { super(t); x = 1; }\nint x;')),
    ('ok int literals', 'ok', E('1 + 0 + 2147483647 + 0xFFFFFFFF + 0X10 + 0x7f')),
    ('ok float literals', 'ok', E('1.5 + 12.5f + 10f + 08.500 + 1000000000000000.0')),
    ('ok string literals', 'ok', S('t = "a\\n\\t\\"\\\\b";\nt = "";\nt = "//not /* a comment";')),
    ('ok string with a raw TAB', 'ok', S('t = "a\tb";')),
    ('ok resource id', 'ok', E('cars.racers.Axis:0x0000FFB4r + frontend:0x007Dr + parts:0x00053ffr')),
    ('ok null true false this', 'ok', S('t = null;\nx = true;\nx = false;\nt = this;')),
    ('ok unary operators', 'ok', S('x = -a;\nx = +a;\nx = !a;\nx = ~a;\nx = !--a;\nx = a + + b;\n++x;\n--x;\nx++;\nx--;')),
    ('ok binary operators', 'ok', E('a || b && c | d ^ e & a == b != c < d > e <= a >= b << c >> d + e - a * b / c % d')),
    ('ok assignment operators', 'ok', S('a = b = c;\na += 1;\na -= 1;\na *= 2;\na /= 2;\na &= 1;\na |= 1;\na ^= 1;\narr[0] = arr[1] = 2;\nt.x = 1;')),
    ('ok casts', 'ok', S('x = (int)1.5;\ny = (float)x / (float)y;\nt = (T)t;\nt = (T)null;\nx = ((T)t).x;\nx = (a) + b;\nx = (a) - b;')),
    ('ok instanceof', 'ok', S('if (t instanceof T) x = 1;\nx = t instanceof T && a < b;')),
    ('ok new and arrays', 'ok', S('t = new T();\nt = new T(1, a, "s");\narr = new int[3];\narr = new int[n + 1];\n'
                                 'T[] ts = new T[2];\nfloat[] fs = new float[2];\nnew T(1).f();\nx = arr[0] + arr[a + 1];')),
    ('ok conditional', 'ok', S('x = a < b ? 0 : 1;\nt = (a == 0) ? null : new T(a);')),
    ('ok calls', 'ok', S('f();\nt.f(1);\nt.t.f(a, b);\nsuper.f();\nthis.f();\nf(x = g());\nx = f(1)[0];\nt.f().g();\narr[0].f();')),
    ('ok names', 'ok', S('x = a;\nx = t.x;\nx = t.t.t.x;\nx = this.x;\nx = arr.length;\nx = arr[1].x;')),
    ('ok if', 'ok', S('if (a) x = 1;\nif (a) { x = 1; } else x = 2;\nif (a) x = 1; else if (b) x = 2; else { }\n'
                      'if (a) if (b) x = 1; else x = 2;\nif (x = f()) { }')),
    ('ok loops', 'ok', S('while (1) { break; }\nwhile (a--) x++;\ndo { x++; } while (x < 3);\n'
                         'for (int i = 0; i < 3; i++) x += i;\nfor (;;) { break; }\nfor (x = 0; x < 3; ) { continue; }\n'
                         'for (int i = 0; ; i--) { }')),
    ('ok switch', 'ok', S('switch (x) { case 1: y = 1; break; case(A): { y = 2; } break; case (a + 1): int k = 1; break; default: y = 3; }')),
    ('ok switch groups ending in return', 'ok',
     M('int f(int x) { switch (x) { case 1: return 1; case 2: return 2; default: return 0; } }')),
    ('ok locals', 'ok', S('int i;\nint j = 1, k, l = j;\nfloat g = 1.0;\nT u = new T();\nT[] us;\nint[] is = new int[2];\n{ int m; }')),
    ('ok return', 'ok', M('int f() { return 1; }\nvoid g() { return; }\nC() { return; }\nT h() { return (a && b); }\nint a, b;')),
    ('ok statements after return', 'ok', M('int x;\nint f() { return 1; x = 2; }')),
    ('ok comments', 'ok', U('package p; // c\n/* a\n b */ class /* c */ C /*implements I*/ { // x /* y\n}\n/* // */\n')),
    ('ok empty class body', 'ok', U('package p;\nclass C extends A{}')),
    ('ok no line end at the end', 'ok', U('package p;\nclass C { }')),
    # ---- appendix B and its like: a warning, an error in strict mode ---------
    ('W import of a single class', 'W', U('package p;\nimport a.b.K;\nclass C { K k; }\n')),
    ('W import after a type', 'W', U('package p;\nclass C { }\nimport a.b.*;\nclass D { }\n')),
    ('W semicolon at top level', 'W', U('package p;;\nclass C { }\n')),
    ('W semicolon between members', 'W', M('int a;;')),
    ('W no package', 'W', U('class C { }\n')),
    ('W interface with members', 'W', U('package p;\ninterface C { void f(); }\n')),
    ('W constructor named otherwise', 'W', M('D() { }')),
    ('W final class', 'W', U('package p;\nfinal class C { }\n')),
    ('W private constructor', 'W', M('private C() { }')),
    ('W final method', 'W', M('final void f() { }')),
    ('W modifier twice', 'W', M('public public int a;')),
    ('W protected', 'W', M('protected int a;')),
    ('W synchronized', 'W', M('synchronized void f() { }')),
    ('W abstract method', 'W', M('abstract void f();')),
    ('W brackets after a field name', 'W', M('int a[];')),
    ('W brackets after a parameter name', 'W', M('void f(int a[]) { }')),
    ('W brackets after a local name', 'W', S('int q[];')),
    ('W qualified type name', 'W', M('java.util.Vector v;')),
    ('W qualified name behind new', 'W', S('t = new java.util.Vector();')),
    ('W qualified name behind extends', 'W', U('package q;\nclass C extends p.A { }\n')),
    ('W final local', 'W', S('final int q = 1;')),
    ('W final parameter', 'W', M('void f(final int a) { }')),
    ('W empty statement', 'W', S(';')),
    ('W several init expressions in for', 'W', S('for (a = 0, b = 1; a < 3; a++) { }')),
    ('W several variables declared in for', 'W', S('for (int i = 0, j = 1; i < 3; i++) { }')),
    ('W several update expressions in for', 'W', S('for (a = 0; a < 3; a++, b++) { }')),
    ('W several labels on a group', 'W', S('switch (x) { case 1: case 2: y = 1; break; }')),
    ('W group that falls through', 'W', S('switch (x) { case 1: y = 1; case 2: y = 2; break; }')),
    ('W default not last', 'W', S('switch (x) { default: y = 1; break; case 2: y = 2; break; }')),
    ('W empty group', 'W', S('switch (x) { case 1: y = 1; break; case 2: }')),
    ('W switch without groups', 'W', S('switch (x) { }')),
    ('W operator %=', 'W', S('x %= 2;')),
    ('W ?: inside ?:', 'W', E('a ? b : c ? d : e')),
    ('W ?: inside the condition of ?:', 'W', E('(a ? b : c) ? d : e')),
    ('W ?: inside an argument of ?:', 'W', E('a ? f(b ? c : d) : e')),
    ('W new as a statement', 'W', S('new T(1);')),
    ('W float without digits before the point', 'W', E('.5')),
    ('W float without digits after the point', 'W', E('5.')),
    ('W float with suffix F', 'W', E('1.5F')),
    ('W decimal int with a leading zero', 'W', E('08')),
    ('W decimal int above 2147483647', 'W', E('3000000000')),
    ('W hex int above 32 bits', 'W', E('0x123456789')),
    ('W resource id above 32 bits', 'W', E('a:0x123456789r')),
    ('W escape other than the four', 'W', S('t = "a\\rb";')),
    # ---- no rule and no default: E_UNSUPPORTED in both modes ---------------
    ('U operator >>>', 'U', E('a >>> 2')),
    ('U operator <<=', 'U', S('x <<= 1;')),
    ('U operator >>=', 'U', S('x >>= 1;')),
    ('U operator >>>=', 'U', S('x >>>= 1;')),
    ('U super.field', 'U', E('super.x')),
    ('U array initialiser of a field', 'U', M('int[] a = { 1, 2 };')),
    ('U array initialiser of a local', 'U', S('int[] q = { 1, 2 };')),
    ('U new with an initialiser list', 'U', S('arr = new int[] { 1 };')),
    ('U two dimensions in new', 'U', S('arr = new int[3][4];')),
    ('U open dimension in new', 'U', S('arr = new int[3][];')),
    ('U two dimensions in a type', 'U', M('int[][] a;')),
    ('U two dimensions type and name', 'U', M('int[] a[];')),
    ('U member of new T[n]', 'U', E('new int[3].length')),
    ('U labelled statement', 'U', S('L: x = 1;')),
    ('U break with a label', 'U', S('while (1) { break L; }')),
    ('U continue with a label', 'U', S('while (1) { continue L; }')),
    ('U character literal', 'U', E("'a'")),
    ('U float with an exponent', 'U', E('1e5')),
    ('U float with an exponent and a point', 'U', E('1.5e-3')),
    ('U float with suffix d', 'U', E('1.5d')),
    ('U initialiser block', 'U', M('{ x = 1; }')),
    ('U static initialiser block', 'U', M('static { x = 1; }')),
    ('U interface with extends', 'U', U('package p;\ninterface C extends I { }\n')),
    ('U interface with implements', 'U', U('package p;\ninterface C implements I { }\n')),
    ('U brackets after a parameter list', 'U', M('int f()[] { return null; }')),
    ('U native method with a body', 'U', M('native void f() { }')),
    ('U modifier on a local', 'U', S('static int q;')),
    ('U modifier on a parameter', 'U', M('void f(static int a) { }')),
    ('U declaration as the body of if', 'U', S('if (a) int q = 1;')),
    ('U declaration as the body of else', 'U', S('if (a) x = 1; else T q;')),
    ('U declaration as the body of while', 'U', S('while (a) int q;')),
    ('U field of type void', 'U', M('void a;')),
    ('U parameter of type void', 'U', M('void f(void a) { }')),
    ('U array of void', 'U', M('void[] f() { }')),
    ('U this(..) in a method', 'U', S('this(1);')),
    ('U super(..) not first', 'U', M('C() { x = 1; super(); }\nint x;')),
    ('U this(..) as a value', 'U', M('C() { x = this(1); }\nint x;')),
    ('U assignment to a call', 'U', S('f() = 1;')),
    ('U assignment to a parenthesised name', 'U', S('(a) = 1;')),
    ('U ++ on a call', 'U', S('++f();')),
    ('U -- behind a literal', 'U', S('1--;')),
    ('U cast to an array type', 'U', S('arr = (int[])t;')),
    ('U cast to a class array type', 'U', S('t = (T[])t;')),
    ('U instanceof with a primitive type', 'U', E('t instanceof int')),
    ('U instanceof with an array type', 'U', E('t instanceof T[]')),
    ('U second default label', 'U', S('switch (x) { default: break; default: break; }')),
    ('U type declared twice', 'U', U('package p;\nclass C { }\nclass C { }\n')),
    # ---- outside the grammar ---------------------------------------------------
    ('X missing semicolon', 'X', S('x = 1')),
    ('X extends list', 'X', U('package p;\nclass C extends A, B { }\n')),
    ('X name as a statement', 'X', S('x;')),
    ('X sum as a statement', 'X', S('a + b;')),
    ('X parenthesised assignment as a statement', 'X', S('(a = b);')),
    ('X array creation as a statement', 'X', S('new int[3];')),
    ('X literal as a for update', 'X', S('for (;; 1) { }')),
    ('X new of a primitive type with arguments', 'X', E('new int(3)')),
    ('X second package declaration', 'X', U('package p;\npackage q;\nclass C { }\n')),
    ('X import before the package', 'WX', U('import a.b.*;\npackage p;\nclass C { }\n')),
    ('X else without if', 'X', S('else x = 1;')),
    ('X case outside a switch', 'X', S('case 1: x = 1;')),
    ('X statement before the first label', 'X', S('switch (x) { y = 1; case 1: break; }')),
    ('X super alone', 'X', E('super')),
    ('X class body not closed', 'X', U('package p;\nclass C {\n')),
    ('X block not closed', 'X', U('package p;\nclass C { void f() { x = 1;\n')),
    ('X no class', 'X', U('package p;\nimport a.b.*;\n')),
    ('X empty source', 'WX', U('')),
    ('X missing class name', 'X', U('package p;\nclass { }\n')),
    ('X expression expected', 'X', E('')),
    ('X type expected in a parameter', 'X', M('void f(, int a) { }')),
    ('X unbalanced parenthesis', 'X', E('(a + b')),
    ('X import without a name', 'X', U('package p;\nimport ;\nclass C { }\n')),
    # ---- characters no rule accepts ---------------------------------------------
    ('L stray character', 'L', E('1 @ 2')),
    ('L string not closed', 'L', S('t = "abc;')),
    ('L string ends in a backslash', 'L', S('t = "abc\\')),
    ('L block comment not closed', 'L', S('x = 1; /* abc')),
    ('L number with letters', 'L', E('12abc')),
    ('L hex without digits', 'L', E('0x')),
    ('L byte above 0x7F outside a string', 'L', E('1 \xe9 2')),
    ('L backslash', 'L', E('a \\ b')),
    # ---- limits -------------------------------------------------------------------
    ('LIM more than 65535 lines', 'LIM', U('package p;\nclass C {\n' + '\n' * 65535 + '}\n')),
    ('LIM 65535 lines are fine', 'ok', U('package p;\nclass C {' + '\n' * 65533 + '}\n')),
    ('LIM parentheses 300 deep', 'LIM', E('(' * 300 + '1' + ')' * 300)),
    ('LIM blocks 300 deep', 'LIM', S('{' * 300 + '}' * 300)),
    ('LIM sum of 300 terms', 'LIM', E('1' + ' + 1' * 300)),
    ('LIM name chain of 300', 'LIM', E('a' + '.b' * 300)),
    ('LIM nesting of 150 is fine', 'ok', E('(' * 150 + '1' + ')' * 150)),
    ('LIM sum of 150 terms is fine', 'ok', E('1' + ' + 1' * 150)),
    # ---- bytes ----------------------------------------------------------------------
    ('B byte above 0x7F in a string', 'B', S('t = "caf\xe9";')),
    ('B byte above 0x7F in a line comment', 'B', S('x = 1; // caf\xe9')),
    ('B byte above 0x7F in a block comment', 'B', S('x = 1; /* caf\xe9 */')),
    ('B byte order mark', 'B', U('\xef\xbb\xbfpackage p;\nclass C { }\n')),
    # ---- types ----------------------------------------------------------------------
    ('T unknown field type', 'T', M('Nowhere a;')),
    ('T unknown parameter type', 'T', M('void f(Nowhere a) { }')),
    ('T unknown type behind new', 'T', S('t = new Nowhere();')),
    ('T unknown qualified type', 'WT', M('no.such.Type a;')),
    ('TT unknown superclass', 'TT', U('package p;\nclass C extends Nowhere { }\n')),
    ('TT unknown interface', 'TT', U('package p;\nclass C implements Nowhere { }\n')),
]


def codes(res):
    err = sorted(set(d.code for d in res.diagnostics if d.error))
    warn = sorted(set(d.code for d in res.diagnostics if not d.error))
    return tuple(err), tuple(warn)


def run_case(case, prov):
    """None, or why the case fails."""
    name, expect, text = case
    want = EXPECT[expect]
    for strict in (False, True):
        try:
            res = slrrc.front(text, 'case.java', prov, slrrc.Options(strict=strict))
        except Exception as x:                  # the compiler must not raise
            return 'exception %r' % (x,)
        got = codes(res)
        exp = (tuple(sorted(want[strict][0])), tuple(sorted(want[strict][1])))
        mode = 'strict' if strict else 'default'
        if got != exp:
            first = res.diagnostics[0].text if res.diagnostics else ''
            return '%s mode: %r, expected %r (%s)' % (mode, got, exp, first)
        if res.ok != (not got[0]):
            return '%s mode: ok is %r with errors %r' % (mode, res.ok, got[0])
        if res.ok and not res.classes:
            return '%s mode: ok without a class' % mode
        for d in res.diagnostics:
            if d.line < 1 or d.column < 1:
                return '%s mode: diagnostic at line %d column %d' % (mode, d.line, d.column)
    return None


def details(prov):
    """A few results looked at in detail.  Returns the list of failures."""
    bad = []

    def front(text, strict=False):
        return slrrc.front(text, 'case.java', prov, slrrc.Options(strict=strict))

    # an unknown type is "L;" and the pool holds it
    res = front(M('Nowhere a;\nvoid f(Nowhere[] b) { }'))
    info = res.classes[0].info
    if [f.desc for f in info.fields] != ['L;'] or info.methods[0].desc != '([L;)V':
        bad.append('unknown type: %r %r' % ([f.desc for f in info.fields], info.methods[0].desc))
    if len([d for d in res.diagnostics if d.code == 'W_TYPE']) != 1:
        bad.append('unknown type reported %d times' % len(res.diagnostics))
    # resolution: own file, own package, imports in source order, java.lang
    res = front('package p;\nimport java.util.*;\nimport a.b.*;\nclass C extends A {\n'
                ' D d; T t; Vector v; K k; String s; int[] i; float f; }\nclass D { }\n')
    got = [f.desc for f in res.classes[0].info.fields]
    want = ['Lp.D;', 'Lp.T;', 'Ljava.util.Vector;', 'La.b.K;', 'Ljava.lang.String;', '[I', 'F']
    if got != want:
        bad.append('resolution: %r' % got)
    # the classes the declarations name, in the order they are met; then the
    # superclasses above them: the chain of every class of the source is
    # walked once, to find a class that is its own superclass (A extends
    # java.lang.Object)
    if res.consulted != ['p.A', 'p.T', 'java.util.Vector', 'a.b.K', 'java.lang.String',
                         'java.lang.Object']:
        bad.append('consulted: %r' % res.consulted)
    # the pool prefix of a small class, written out by hand from
    # notes/declarations.md 12
    res = front('package p;\nimport java.util.*;\npublic class C extends A implements I {\n'
                ' static int n = 1;\n T t = new T(new Vector());\n'
                ' public C(int a, float b) { T u = (T)t; int[] v = new int[3]; }\n'
                ' public float f(T x) { if (x instanceof A) return 1.0; return (float)n; }\n'
                ' native void g();\n}\nclass D { C c; }\n')
    c0 = [e.text if e.kind == 0 else (e.kind, e.a, e.b) for e in res.classes[0].pool.entries]
    want = ['p.C', (4, 0, 0), 'p.A', (4, 2, 0), 'p.I', (4, 4, 0),
            'I', 'Lp.T;', 'Ljava.util.Vector;', 'F', '[I', 'Lp.A;', 'V', 'Lp.I;',
            'n', 't', '(IF)', '<init>', '(Lp.T;)F', 'f', '()V', 'g']
    if c0 != want:
        bad.append('pool prefix of C: %r' % c0)
    c1 = [e.text if e.kind == 0 else (e.kind, e.a, e.b) for e in res.classes[1].pool.entries]
    want = ['p.D', (4, 0, 0), 'java.lang.Object', (4, 2, 0), 'Lp.C;', 'Lp.D;', 'c']
    if c1 != want:
        bad.append('pool prefix of D: %r' % c1)
    rows = res.classes[0].rows
    got = ([(r.flags, r.name, r.type, r.tree) for r in rows.static_fields],
           [(r.flags, r.name, r.type, r.tree) for r in rows.instance_fields],
           [(r.flags, r.name, r.desc, r.tree, r.locals) for r in rows.static_methods],
           [(r.flags, r.name, r.desc, r.tree, r.locals) for r in rows.instance_methods],
           (rows.interface, rows.name, rows.super, rows.interfaces), len(rows.owners))
    want = ([(0x8, 14, 6, 0)], [(0, 15, 7, 1)], [],
            [(0x1, 17, 16, 2, 5), (0x1, 19, 18, 3, 2), (0x40, 21, 20, 4, 0)],
            (0, 1, 3, [5]), 5)
    if got != want:
        bad.append('rows of C: %r' % (got,))
    # bytes as well as str
    res = slrrc.front(b'package p;\r\nclass C { }\r\n', 'case.java', prov)
    if not res.ok or res.classes[0].info.name != 'p.C':
        bad.append('bytes input')
    # in an interpreter of its own, with nothing but the package on the path:
    # the front half works and no module of lib/, analysis/ or spec/ is loaded
    code = '\n'.join([
        'import sys',
        'sys.path.insert(0, %r)' % harness.ROOT,
        'import slrrc',
        'from slrrc import classes',
        'class P(classes.ClassProvider):',
        '    def find(self, fqn):',
        '        return classes.ClassInfo(fqn) if fqn == "java.lang.String" else None',
        'r = slrrc.front("package p; class C { String s; }", "C.java", P())',
        'assert r.ok and r.classes[0].info.fields[0].desc == "Ljava.lang.String;"',
        'bad = [m for m in ("oracle", "tufa", "lexer", "parser", "symbols", "poolmodel",',
        '       "stmtgen", "gen", "classdb", "spike_compiler") if m in sys.modules]',
        'print("loaded:", bad)'])
    out = subprocess.run([sys.executable, '-I', '-c', code], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT).stdout.decode('utf-8', 'replace').strip()
    if out != 'loaded: []':
        bad.append('package alone: %s' % out)
    # the exe's wording of a syntax error
    res = front('package p;\nclass C {\n int a\n}\n')
    d = res.diagnostics[0]
    if (d.code, d.line, d.exe_wording('C.java')) != (
            'E_SYNTAX', 4, 'JVM::compileSource: Syntax error in "C.java" at line 4'):
        bad.append('syntax error: %r %r' % (d.code, d.exe_wording('C.java')))
    return bad


# ---------------------------------------------------------------------------
# deletions and mutations against lib
# ---------------------------------------------------------------------------

STRICT_TOKENS = (sc.P_SEMI, sc.P_LBRACE, sc.P_RBRACE, sc.P_LPAREN, sc.P_RPAREN,
                 sc.P_LBRACKET, sc.P_RBRACKET)
# Constructs of appendix B that the compiler gives their obvious meaning and
# poolmodel.py has no rule for: it skips an import without ".*" and does not
# count brackets behind the name of a local.
MODEL_HAS_NO_RULE = ('import of a single class', 'brackets after a declared name')
ALPHABET = ' \t\n"\'/*.0123456789abcdefxXrRfFdDeE+-_:;,(){}[]<>=!&|^~%?\\@#$`\xe9'


def lib_features(unit):
    """Constructs lib/parser.py accepts and the compiler refuses on purpose."""
    out = set()
    for code, line, text in unit.notes:
        if code in ('expr-statement', 'extends-list'):
            out.add(code)
    for n in unit.walk():
        if n.kind == 'New' and n.type.primitive:
            out.add('new-primitive')
    # this(..) / super(..) anywhere but as the first statement of a constructor
    first = set()
    for c in unit.types:
        for m in c.members:
            if m.kind == 'MethodDecl' and m.constructor and m.body is not None and m.body.stmts:
                s = m.body.stmts[0]
                if s.kind == 'ExprStmt' and s.expr.kind == 'CtorCall':
                    first.add(id(s.expr))
    for n in unit.walk():
        if n.kind == 'CtorCall' and id(n) not in first:
            out.add('ctor-call')
    return out


class Tally(object):
    def __init__(self):
        self.texts = 0
        self.exceptions = []
        self.differences = []
        self.accepted = 0
        self.declared = 0
        self.lib_accepted = False

    def decl(self, text, where, prov, uni):
        """Declarations of a text both parsers accept, against poolmodel."""
        try:
            res = slrrc.front(text, 'x.java', prov)
        except Exception as x:
            self.exceptions.append('%s: front: %r' % (where, x))
            return
        if not res.ok:
            return                  # an unknown superclass, a class declared twice
        for d in res.diagnostics:
            if d.code == 'W_TYPE':
                return              # the model has another placeholder for an unknown type
            if d.text.startswith(MODEL_HAS_NO_RULE):
                return
        try:
            out = t3_decl.against_model(res, text, 'x', uni)
        except ValueError:
            return                  # a construct the model's adapter has no node for
        self.declared += 1
        for cname, why in out:
            if why is not None:
                self.differences.append('%s: declarations of %s: %s' % (where, cname, why))

    def scan(self, text, where):
        """Scanner against lib/lexer.py."""
        self.texts += 1
        d = Diagnostics(False)
        try:
            toks = sc.scan(text, d)
        except Exception as x:
            self.exceptions.append('%s: scan: %r' % (where, x))
            return
        try:
            ref = lexer.tokenize(text)
        except lexer.LexError:
            ref = None
        if d.errors:
            e = [x for x in d.items if x.error][0]
            if e.code != 'E_LEX' or ref is None:
                return
            if e.text == 'block comment is not closed':
                return          # lib reads "/" "*" there
            self.differences.append('%s: scan: line %d %s, lib/lexer.py accepts' % (
                where, e.line, e.text))
            return
        if ref is None:
            self.differences.append('%s: scan: accepted, lib/lexer.py rejects' % where)
            return
        a = [(sc.KIND_NAMES[t.kind], t.pos, t.length, t.line) for t in toks]
        b = [(t.kind, t.pos, len(t.text), t.line) for t in ref]
        if a != b:
            self.differences.append('%s: scan: tokens differ from lib/lexer.py' % where)

    def parse(self, text, where):
        """Parser against lib/parser.py.  Returns True when accepted;
        self.lib_accepted says what lib/parser.py did with the same text."""
        self.texts += 1
        d = Diagnostics(False)
        try:
            toks = sc.scan(text, d)
            ast = ps.parse(text, toks, d) if not d.errors else None
        except Exception as x:
            self.exceptions.append('%s: parse: %r' % (where, x))
            return False
        try:
            ref = libparser.parse(text, 'x')
        except (libparser.ParseError, lexer.LexError):
            ref = None
        self.lib_accepted = ref is not None
        if d.errors:
            e = [x for x in d.items if x.error][0]
            if e.code != 'E_SYNTAX' or ref is None:
                return False
            if not lib_features(ref):
                self.differences.append('%s: parse: line %d %s, lib/parser.py accepts' % (
                    where, e.line, e.text))
            return False
        if ast is None or ast.root is None:
            self.exceptions.append('%s: parse: no tree and no error' % where)
            return False
        if ref is None:
            self.differences.append('%s: parse: accepted, lib/parser.py rejects' % where)
            return True
        if t2_parse.mine(ast) != t2_parse.theirs(ref):
            self.differences.append('%s: parse: tree differs from lib/parser.py' % where)
        return True


def deletions(tally):
    """(tried, rejected, rejected by lib)."""
    tried = rejected = lib_rejected = 0
    paths = sorted((e for e in harness.all_sources()), key=lambda e: e.source.lower())
    for si, e in enumerate(paths):
        if si % 7:
            continue
        text = harness.load_source(e)
        toks = sc.scan(text, Diagnostics(False))
        k = 0
        for t in toks:
            if t.kind not in STRICT_TOKENS:
                continue
            k += 1
            if k % 11:
                continue
            tried += 1
            damaged = text[:t.pos] + ' ' + text[t.pos + 1:]
            if not tally.parse(damaged, '%s:%d' % (e.source, t.line)):
                rejected += 1
            if not tally.lib_accepted:
                lib_rejected += 1
    return tried, rejected, lib_rejected


def mutations(tally, prov, uni):
    rng = random.Random(940)
    srcs = harness.all_sources()
    for si in range(0, len(srcs), 23):
        e = srcs[si]
        text = harness.load_source(e)
        lines = text.split('\n')
        for m in range(20):
            # move or copy a line: declarations and statements change place
            x = rng.randrange(len(lines))
            y = rng.randrange(len(lines))
            l2 = list(lines)
            if rng.randrange(2):
                l2.insert(y, l2.pop(x))
            else:
                l2.insert(y, l2[x])
            t2 = '\n'.join(l2)
            where = '%s line edit %d' % (e.source, m)
            if tally.parse(t2, where):
                tally.accepted += 1
                if tally.lib_accepted:
                    tally.decl(t2, where, prov, uni)
        for m in range(20):
            p = rng.randrange(len(text))
            kind = rng.randrange(4)
            if kind == 0:
                t2 = text[:p] + rng.choice(ALPHABET) + text[p + 1:]
            elif kind == 1:
                t2 = text[:p] + text[p + 1:]
            elif kind == 2:
                t2 = text[:p] + rng.choice(ALPHABET) + text[p:]
            else:
                q = rng.randrange(len(text))
                t2 = text[:p] + text[q:q + rng.randrange(1, 6)] + text[p:]
            tally.scan(t2, '%s byte edit %d' % (e.source, m))
        toks = sc.scan(text, Diagnostics(False))[:-1]
        spans = [(t.pos, t.pos + t.length) for t in toks]
        for m in range(20):
            k = rng.randrange(len(toks))
            a, b = spans[k]
            j = rng.randrange(len(toks))
            other = text[spans[j][0]:spans[j][1]]
            kind = rng.randrange(5)
            if kind == 0:
                t2 = text[:a] + ' ' + text[b:]
            elif kind == 1:
                t2 = text[:b] + ' ' + text[a:b] + ' ' + text[b:]
            elif kind == 2:
                t2 = text[:a] + ' ' + other + ' ' + text[b:]
            elif kind == 3:
                t2 = text[:a] + ' ' + other + ' ' + text[a:]
            else:
                k2 = min(k + 1, len(toks) - 1)
                c, z = spans[k2]
                t2 = text if k2 == k else text[:a] + text[c:z] + text[b:c] + text[a:b] + text[z:]
            where = '%s token edit %d' % (e.source, m)
            if tally.parse(t2, where):
                tally.accepted += 1
                if tally.lib_accepted:
                    tally.decl(t2, where, prov, uni)


def run(opts):
    oc = harness.Outcome('extra')
    prov = MapProvider()
    passed = []
    names = set()
    for case in CASES:
        if case[0] in names:
            oc.failures.append('dialect: case name used twice: %s' % case[0])
        names.add(case[0])
        why = run_case(case, prov)
        if why is None:
            passed.append(case[0])
        else:
            oc.failures.append('dialect: %s: %s' % (case[0], why))
    bad = details(prov)
    for x in bad:
        oc.failures.append('detail: %s' % x)
    tally = Tally()
    tried, rejected, lib_rejected = deletions(tally)
    n_del = tally.texts
    mutations(tally, harness.provider(), universe.load())
    for x in tally.exceptions:
        oc.failures.append(x)
    for x in tally.differences:
        oc.failures.append(x)
    oc.text = ('dialect %d/%d cases; deletions %d/%d rejected, lib %d; '
               'mutations %d texts, %d exceptions, %d differences from lib' % (
                   len(passed), len(CASES), rejected, tried, lib_rejected,
                   tally.texts - n_del, len(tally.exceptions), len(tally.differences)))
    oc.ok = (len(passed) == len(CASES) and not bad and rejected == lib_rejected and
             tried == 3068 and rejected == 3067 and
             not tally.exceptions and not tally.differences)
    oc.passed['extra.pass'] = passed
    oc.notes.append('edited texts the parser accepted: %d; of these with declarations '
                    'equal to poolmodel: %d' % (tally.accepted, tally.declared))
    return oc


if __name__ == '__main__':
    sys.exit(harness.main_single(run, sys.argv[1:]))
