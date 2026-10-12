#!/usr/bin/env python3
r"""t_extra4.py - checks of the emitter outside the corpus.

    python -I tests/t_extra4.py [-v] [--update]
    python -I tests/run_tests.py extra4

Milestone M4 compares trees of sources the game's compiler accepted.  These
checks look at what the corpus cannot show:

  dialect    small sources, one construct each, with the diagnostic codes
             expected in default and in strict mode: names that resolve
             nowhere (E_NAME), members that are not found (E_MEMBER), break
             and continue out of place (E_SYNTAX), and the constructs of
             notes/compiler_spec.md appendix B that only the emitter can see
             (W_UNVERIFIED, E_UNSUPPORTED in strict mode).
  trees      seven sources whose trees are written out by hand from the rules
             of notes/statements.md and notes/expressions.md: every statement
             form, every family of expression, the name and call forms, the
             lines of nodes that are built with lookahead, and the defaults of
             appendix B.  The expected text is the dump of slrrc/dump.py.
  lookup     classes.find_method and classes.find_field against the score
             table of notes/expressions.md 6.3, case by case.
  details    float settings and pack names through the interfaces, the list
             of consulted classes, the package in an interpreter of its own.
  mutations  every 11th source of the corpus, 20 lines moved or copied and 20
             token edits each, with a fixed seed, compiled in default mode by
             the package and by spec/spike_compiler.py (the composition of the
             analysis models).  No exception; whenever both compile the text
             they emit the same trees, node for node; and where only one of
             them compiles it the reason is a known one: a type name that
             resolves nowhere (the package compiles it to "L;" with a warning,
             the spike raises), the import of a single class (the spike has no
             rule), or a construct the package refuses on purpose.

Headline:
  extra4           dialect 75/75 cases; trees 13/13 written out by hand; lookup
                   55/55; mutations 12560 texts, 0 exceptions, 0 differences
                   from the spike
"""
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness              # noqa: E402
import symbols              # noqa: E402  lib
import spike_compiler       # noqa: E402  spec
import t4_trees             # noqa: E402

import slrrc                            # noqa: E402
from slrrc import classes               # noqa: E402
from slrrc import dump                  # noqa: E402
from slrrc import packs                 # noqa: E402
from slrrc import scanner as sc         # noqa: E402
from slrrc.diag import Diagnostics      # noqa: E402

# ---------------------------------------------------------------------------
# a handful of classes with members
# ---------------------------------------------------------------------------


def make_class(name, sup, interfaces, fields, methods, is_interface=False):
    info = classes.ClassInfo(name)
    info.super = sup
    info.interfaces = list(interfaces)
    info.is_interface = is_interface
    for n, d, st in fields:
        info.fields.append(classes.FieldInfo(n, d, st))
    for n, d, st in methods:
        info.methods.append(classes.MethodInfo(n, d, st))
    return info


class MemberProvider(classes.ClassProvider):
    """
    java.lang.Object, java.lang.String

    interface p.I

    class p.A                       class p.T extends A implements I
        int base;                       int x;
        static int SA;                  T next;
        A link;                         int[] arr;
        void m();                       static T ST;
        int over(int);                  String name;
        int over(float);                T[] many;
        void obj(Object);               T get();
        static int sm(int);             void set(int, T);
        void two(A, float);             int f(int);
        void str(String);
        void ifc(I);

    class p.S and class p.S2 extends S: overloads for the lookup checks."""

    def __init__(self):
        self.known = {}
        O = 'Ljava.lang.Object;'
        STR = 'Ljava.lang.String;'
        for c in (
            make_class('java.lang.Object', None, [], [], []),
            make_class('java.lang.String', 'java.lang.Object', [], [], []),
            make_class('p.I', None, [], [('IC', 'I', True)], [], True),
            make_class('p.A', 'java.lang.Object', [],
                       [('base', 'I', False), ('SA', 'I', True), ('link', 'Lp.A;', False)],
                       [('m', '()V', False), ('over', '(I)I', False), ('over', '(F)I', False),
                        ('obj', '(%s)V' % O, False), ('sm', '(I)I', True),
                        ('two', '(Lp.A;F)V', False), ('str', '(%s)V' % STR, False),
                        ('ifc', '(Lp.I;)V', False)]),
            make_class('p.T', 'p.A', ['p.I'],
                       [('ST', 'Lp.T;', True), ('x', 'I', False), ('next', 'Lp.T;', False),
                        ('arr', '[I', False), ('name', STR, False), ('many', '[Lp.T;', False)],
                       [('get', '()Lp.T;', False), ('set', '(ILp.T;)V', False),
                        ('f', '(I)I', False)]),
            make_class('p.S', 'java.lang.Object', [], [],
                       [('num', '(I)I', False), ('num', '(F)I', False),
                        ('conv', '(F)I', False), ('onlyint', '(I)I', False),
                        ('st', '(%s)I' % STR, False), ('ob', '(%s)I' % O, False),
                        ('ar', '([I)I', False),
                        ('pick', '(Lp.A;)I', False), ('pick', '(Lp.T;)I', False),
                        ('pick', '(%s)I' % O, False),
                        ('ifc', '(Lp.I;)I', False), ('far', '(Lp.A;)I', False),
                        ('tie', '(Lp.A;)I', False), ('tie', '(Lp.T;)I', False),
                        ('mixed', '(Lp.A;)I', True), ('mixed', '(Lp.T;)I', False),
                        ('two', '(II)I', False), ('unk', '(L;)I', False)]),
            make_class('p.S2', 'p.S', [], [],
                       [('num', '(F)I', False), ('far', '(%s)I' % O, False)]),
            make_class('p.Loop1', 'p.Loop2', [], [], []),
            make_class('p.Loop2', 'p.Loop1', [], [('lf', 'I', False)], []),
        ):
            self.known[c.name] = c

    def find(self, fqn):
        return self.known.get(fqn)


def M(members):
    """Members of class C."""
    return 'package p;\nclass C extends T {\n' + members + '\n}\n'


def S(stmts):
    """Statements of a method of class C."""
    return M('void f(int a, T t) {\n' + stmts + '\n}')


def both(errors, warnings):
    return ((errors, warnings), (errors, warnings))


# expectation -> ((errors, warnings) in default mode, (errors, warnings) in strict mode)
EXPECT = {
    'ok': both((), ()),
    'W': (((), ('W_UNVERIFIED',)), (('E_UNSUPPORTED',), ())),
    'U': both(('E_UNSUPPORTED',), ()),
    'N': both(('E_NAME',), ()),
    'MB': both(('E_MEMBER',), ()),
    'X': both(('E_SYNTAX',), ()),
    'NM': both(('E_MEMBER', 'E_NAME'), ()),
}

CASES = [
    # ---- the proven subset: no diagnostic ---------------------------------
    ('ok names', 'ok', S('x = base;\nx = SA;\nx = t.x;\nx = t.next.base;\nx = T.ST.x;\n'
                         'x = arr.length;\nx = many.length;\nx = this.x;\nx = many[0].x;\n'
                         'x = get().x;\nx = ((T)t).x;\nt.x = a;\nthis.x = a;\narr[a] = a;')),
    ('ok calls', 'ok', S('m();\nt.m();\nt.next.m();\nA.sm(1);\nT.sm(a);\nt.get().m();\n'
                         'super.m();\nthis.m();\nnosuch(a + 1);\nx = over(a);\nx = f(1);\n'
                         'new T().m();\nmany[0].set(1, null);\nobj(this);\nifc(t);')),
    ('ok continue in a for with update', 'ok',
     S('for (int i = 0; i < 3; i++) { if (i) continue; x++; }')),
    ('ok return forms', 'ok', M('int g() { return 1; }\nvoid h() { if (x) return; x = 1; }\n'
                                'C() { if (x) return; x = 1; }')),
    ('ok value method with an empty body', 'ok', M('int g() { }\nT h();')),
    ('ok true as an argument of a call nothing resolves', 'ok', S('nosuch(true);')),
    ('ok odd arguments of a call nothing resolves', 'ok', S('nosuch((a), !a, ~a);')),
    ('ok static method', 'ok', M('static int g(int a) { return a + SA; }')),
    ('ok local that hides a field', 'ok', S('int x = 1;\nx = x + a;\nint base;\nbase = 2;')),
    ('ok field initialisers', 'ok', M('int q = 1;\nstatic T u = new T();\nint w = SA + base;\n'
                                      'float fl = 1.5;\nString s = "a";\nint r = sm(2);')),
    ('ok switch and loops', 'ok',
     S('switch (a) { case 1: int k = 2; x = k; break; default: x = 0; }\n'
       'while (a) { if (x) break; a--; }\ndo a--; while (a);\nfor (;;) break;')),
    ('ok unreachable statements', 'ok', M('int g() { return 1; x = 2; return 3; }')),
    ('ok best of several overloads', 'ok',
     S('S s = null;\nx = s.pick(t);\nx = s.pick(this);\nx = s.num(1);\nx = s.conv(1);')),
    # ---- appendix B, seen by the emitter: a warning, an error in strict mode ----
    ('W continue in a while', 'W', S('while (a) { a--; continue; }')),
    ('W continue in a do', 'W', S('do { continue; } while (a);')),
    ('W continue in a for without update', 'W', S('for (;;) { continue; }')),
    ('W continue inside a switch in a for', 'W',
     S('for (int i = 0; i < 3; i++) { switch (i) { case 1: continue; } }')),
    ('W compound assignment as a value', 'W', S('x = a += 1;')),
    ('W compound assignment as an argument', 'W', S('nosuch(a *= 2);')),
    ('W call named like a local', 'W', S('int m = 1;\nm();')),
    ('W call named like a parameter', 'W', S('a();')),
    ('W call named like a field', 'W', S('x();')),
    ('W call named like an inherited field', 'W', S('base(1);')),
    ('W value method that does not end in return', 'W', M('int g() { x = 1; }')),
    ('W value method that ends in if else', 'W',
     M('int g() { if (x) return 1; else return 2; }')),
    ('W void method that ends in return', 'W', M('void g() { x = 1; return; }')),
    ('W constructor that ends in return', 'W', M('C() { return; }')),
    ('W return with a value in a void method', 'W', M('void g() { if (x) return 1; x = 2; }')),
    ('W return without a value in a value method', 'W',
     M('int g() { if (x) return; return 1; }')),
    ('W this in a static method', 'W', M('static void g() { Object q = this; }')),
    ('W this in a field initialiser', 'W', M('Object q = this;')),
    ('W this in a static field initialiser', 'W', M('static Object q = this;')),
    ('W parenthesised name as the receiver of a member', 'W', S('x = (t).x;')),
    ('W parenthesised name as the receiver of a call', 'W', S('(t).m();')),
    ('W parenthesised dotted name as a receiver', 'W', S('x = (t.next).x;')),
    ('W true as an argument of a resolvable call', 'W', S('x = over(true);')),
    ('W parenthesised name as an argument of a resolvable call', 'W', S('x = over((a));')),
    ('W parenthesised literal as an argument of a resolvable call', 'W', S('x = over((1));')),
    ('W not as an argument of a resolvable call', 'W', S('x = over(!a);')),
    ('W complement as an argument of a resolvable call', 'W', S('x = over(~a);')),
    ('W ?: as an argument of a resolvable call', 'W', S('x = over(a ? 1 : 2);')),
    ('W instanceof as an argument of a resolvable call', 'W', S('x = over(t instanceof T);')),
    ('W new array as an argument of a resolvable call', 'W', S('obj(new int[2]);')),
    ('W odd argument in a call through a name', 'W', S('t.set(true, t);')),
    ('W method body that emits no node', 'W', M('void g() { { } }')),
    ('W variable named like a class of the source', 'W', S('int T = 1;\nx = T;')),
    ('W local used in its own initialiser', 'W', S('int k = k + 1;')),
    ('W local passed to a call in its own initialiser', 'W', S('int k = over(k);')),
    ('W length of a computed value', 'W', S('x = many[0].arr.length;')),
    ('W length of the value of a call', 'W', S('x = get().length;')),
    ('W two overloads fit equally well', 'W', S('S s = null;\nx = s.tie(null);')),
    # ---- no rule and no default ---------------------------------------------
    ('U return with a value in a constructor', 'U', M('C() { if (x) return 1; x = 2; }')),
    # ---- names ----------------------------------------------------------------
    ('N unknown name', 'N', S('x = nowhere;')),
    ('N unknown head of a dotted name', 'N', S('x = nowhere.x;')),
    ('N unknown name as an assignment target', 'N', S('nowhere = 1;')),
    ('N unknown receiver of a call', 'N', S('nowhere.m();')),
    ('N class name as a value', 'N', S('t = T;')),
    ('N class written with its package', 'N', S('x = java.lang.String.x;')),
    ('N unknown name in a field initialiser', 'N', M('int q = nowhere;')),
    ('N local used behind its scope', 'N', S('{ int k = 1; }\nx = k;')),
    ('N local of a for used behind it', 'N', S('for (int i = 0; i < 3; i++) x++;\nx = i;')),
    ('N local of a case group used in the next group', 'N',
     S('switch (a) { case 1: int k = 1; break; case 2: x = k; break; }')),
    # ---- members ----------------------------------------------------------------
    ('MB unknown member', 'MB', S('x = t.nowhere;')),
    ('MB member of an int', 'MB', S('x = a.x;')),
    ('MB member of an array other than length', 'MB', S('x = arr.size;')),
    ('MB member behind length', 'MB', S('x = arr.length.x;')),
    ('MB unknown member of a class', 'MB', S('x = T.nowhere;')),
    ('MB unknown member in the receiver of a call', 'MB', S('t.nowhere.m();')),
    ('MB method name used as a field', 'MB', S('x = t.get.x;')),
    ('MB field of an interface is not inherited', 'MB', S('x = t.IC;')),
    ('NM several errors are all reported', 'NM', S('x = nowhere;\nx = t.nowhere;\nnowhere.m(1);')),
    # ---- break and continue out of place ---------------------------------------
    ('X break outside a loop', 'X', S('x = 1;\nbreak;')),
    ('X continue outside a loop', 'X', S('x = 1;\ncontinue;')),
    ('X continue in a switch without a loop', 'X', S('switch (a) { case 1: continue; }')),
    ('X break in an if', 'X', S('if (a) break;')),
]


def codes(res):
    err = sorted(set(d.code for d in res.diagnostics if d.error))
    warn = sorted(set(d.code for d in res.diagnostics if not d.error))
    return tuple(err), tuple(warn)


def compile_case(text, prov, strict=False, mode='A', pk=None):
    if pk is None:
        pk = packs.TablePackNames(packs.TABLE_940, None)
    return slrrc.emit_trees(text, 'case.java', prov, pk,
                            slrrc.Options(strict=strict, float_mode=mode))


def run_case(case, prov):
    """None, or why the case fails."""
    name, expect, text = case
    want = EXPECT[expect]
    for strict in (False, True):
        try:
            res = compile_case(text, prov, strict)
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
        for d in res.diagnostics:
            if d.line < 1 or d.column < 1:
                return '%s mode: diagnostic at line %d column %d' % (mode, d.line, d.column)
        if res.ok:
            for cb in res.classes:
                if len(cb.trees) != len(cb.rows.owners):
                    return '%s mode: %d trees for %d owners' % (
                        mode, len(cb.trees), len(cb.rows.owners))
    return None


# ---------------------------------------------------------------------------
# trees written out by hand
# ---------------------------------------------------------------------------
# (name, source, [(tree number, locals, expected dump)])
# The comment behind a jump names the node it lands on.

T_STATEMENTS = (r'''package p;
class C extends T {
    int f(int a, float b) {
        int i = a;
        while (i < 10)
            i++;
        if (i == 10) return 1;
        else { int k; k = i; }
        return i + a;
    }
}
''', [(0, 5, r'''
0 L4 declare 3
1 L4 type "I"
2 L4 init
3 L4 local 2
4 L4 store
5 L5 local 3
6 L5 expr literal
7 L5 int 10
8 L5 expr lt
9 L6 jump_false +4
10 L6 local 3
11 L6 stmt postinc
12 L6 jump -7
13 L7 local 3
14 L7 expr literal
15 L7 int 10
16 L7 expr eq
17 L8 jump_false +10
18 L7 expr literal
19 L7 int 1
20 L7 value
21 L7 release 3
22 L7 release 2
23 L7 release 1
24 L7 release 0
25 L7 ret_value
26 L8 jump +7
27 L8 declare 4
28 L8 type "I"
29 L8 local 3
30 L8 local 4
31 L8 stmt assign
32 L8 release 4
33 L9 local 3
34 L9 local 2
35 L9 expr add
36 L9 value
37 L9 release 3
38 L9 release 2
39 L9 release 1
40 L9 release 0
41 L9 ret_value
42 L10 release 3
43 L10 release 2
44 L10 release 1
45 L10 release 0
''')])

T_LOOPS = (r'''package p;
class C extends T {
    C(int n) {
        x = n;
    }
    C() {
        this(1);
    }
    void g(int n) {
        for (int i = 0; i < n; i++) {
            if (i == 2) continue;
            if (i == 5) break;
            x += i;
        }
        do {
            x--;
        } while (x > 0);
    }
}
''', [(0, 2, r'''
0 L4 push_int 0
1 L4 super_init
2 L4 stmt ctor
3 L4 local 1
4 L4 field p.C.x:I
5 L4 stmt assign
6 L5 ret
7 L5 release 1
8 L5 release 0
9 L5 release 1
10 L5 release 0
'''), (1, 1, r'''
0 L7 expr literal
1 L7 int 1
2 L7 push_int 1
3 L7 this_init
4 L7 stmt ctor
5 L8 ret
6 L8 release 0
7 L8 release 0
'''), (2, 3, r'''
0 L10 declare 2
1 L10 type "I"
2 L10 init
3 L10 expr literal
4 L10 int 0
5 L10 store
6 L10 local 2
7 L10 local 1
8 L10 expr lt
9 L14 jump_false +19
10 L11 local 2
11 L11 expr literal
12 L11 int 2
13 L11 expr eq
14 L12 jump_false +2
15 L11 jump +10
16 L12 local 2
17 L12 expr literal
18 L12 int 5
19 L12 expr eq
20 L13 jump_false +2
21 L12 jump +7
22 L13 local 2
23 L13 field p.C.x:I
24 L13 stmt add_assign
25 L10 local 2
26 L10 stmt postinc
27 L14 jump -21
28 L14 release 2
29 L16 field p.C.x:I
30 L16 stmt postdec
31 L17 field p.C.x:I
32 L17 expr literal
33 L17 int 0
34 L17 expr gt
35 L17 jump_true -6
36 L18 push_void
37 L18 release 1
38 L18 release 0
39 L18 ret_value
40 L18 release 1
41 L18 release 0
''')])

T_SWITCH = (r'''package p;
class C extends T {
    void s(int v) {
        switch (v) {
            case 1:
                int k = v;
                x = k;
                break;
            case 2: x = 2; break;
            default: x = 0;
        }
    }
}
''', [(0, 3, r'''
0 L4 local 1
1 L5 expr literal
2 L5 int 1
3 L11 case +6
4 L9 expr literal
5 L9 int 2
6 L11 case +14
7 L11 pop
8 L11 jump +17
9 L6 declare 2
10 L6 type "I"
11 L6 init
12 L6 local 1
13 L6 store
14 L7 local 2
15 L7 field p.C.x:I
16 L7 stmt assign
17 L8 release 2
18 L8 jump +11
19 L9 release 2
20 L9 expr literal
21 L9 int 2
22 L9 field p.C.x:I
23 L9 stmt assign
24 L9 jump +5
25 L10 expr literal
26 L10 int 0
27 L10 field p.C.x:I
28 L10 stmt assign
29 L12 push_void
30 L12 release 1
31 L12 release 0
32 L12 ret_value
33 L12 release 1
34 L12 release 0
''')])

T_NAMES = (r'''package p;
class C extends T {
    int own;
    static int SO;
    void n(T t, int[] a) {
        own = base;
        SO = SA;
        x = t.x;
        x = t.next.x;
        x = next.base;
        x = T.ST.x;
        x = a.length;
        x = many.length;
        x = this.x;
        x = a[1];
        x = many[x].next.x;
        x = get().x;
        t.x = 1;
        this.x = 2;
        a[0] = 3;
    }
}
''', [(0, 3, r'''
0 L6 field p.C.base:I
1 L6 field p.C.own:I
2 L6 stmt assign
3 L7 field p.A.SA:I
4 L7 field p.C.SO:I
5 L7 stmt assign
6 L8 path 2
7 L8 local 2
8 L8 field p.T.x:I
9 L8 field p.C.x:I
10 L8 stmt assign
11 L9 path 3
12 L9 local 2
13 L9 field p.T.next:Lp.T;
14 L9 field p.T.x:I
15 L9 field p.C.x:I
16 L9 stmt assign
17 L10 path 2
18 L10 field p.C.next:Lp.T;
19 L10 field p.T.base:I
20 L10 field p.C.x:I
21 L10 stmt assign
22 L11 path 3
23 L11 class p.T
24 L11 field p.T.ST:Lp.T;
25 L11 field p.T.x:I
26 L11 field p.C.x:I
27 L11 stmt assign
28 L12 path 2
29 L12 local 1
30 L12 field [I.length:I
31 L12 field p.C.x:I
32 L12 stmt assign
33 L13 path 2
34 L13 field p.C.many:[Lp.T;
35 L13 field [p.T.length:I
36 L13 field p.C.x:I
37 L13 stmt assign
38 L14 local 0
39 L14 getfield "x"
40 L14 field p.C.x:I
41 L14 stmt assign
42 L15 expr literal
43 L15 int 1
44 L15 local 1
45 L15 index
46 L15 field p.C.x:I
47 L15 stmt assign
48 L16 field p.C.x:I
49 L16 field p.C.many:[Lp.T;
50 L16 index
51 L16 getfield "next"
52 L16 getfield "x"
53 L16 field p.C.x:I
54 L16 stmt assign
55 L17 push_int 0
56 L17 call
57 L17 method p.C.get:()Lp.T;
58 L17 expr call
59 L17 getfield "x"
60 L17 field p.C.x:I
61 L17 stmt assign
62 L18 expr literal
63 L18 int 1
64 L18 path 2
65 L18 local 2
66 L18 field p.T.x:I
67 L18 stmt assign
68 L19 expr literal
69 L19 int 2
70 L19 local 0
71 L19 getfield "x"
72 L19 stmt assign
73 L20 expr literal
74 L20 int 3
75 L20 expr literal
76 L20 int 0
77 L20 local 1
78 L20 index
79 L20 stmt assign
80 L21 push_void
81 L21 release 2
82 L21 release 1
83 L21 release 0
84 L21 ret_value
85 L21 release 2
86 L21 release 1
87 L21 release 0
''')])

T_CALLS = (r'''package p;
class C extends T {
    void k(T t, A a, float q) {
        m();
        x = over(1);
        x = over(q);
        x = over(-1);
        two(t, 2);
        obj(null);
        str("s");
        nosuch(1);
        t.set(3, t);
        t.set(x + 1, t);
        t.next.m();
        A.sm(4);
        t.get().m();
        this.m();
        super.m();
        many[0].set(5, null);
        obj(a);
        obj(this);
        new T().m();
        ifc(t);
    }
}
''', [(0, 4, r'''
0 L4 push_int 0
1 L4 call
2 L4 method p.C.m:()V
3 L4 stmt call
4 L5 expr literal
5 L5 int 1
6 L5 push_int 1
7 L5 call
8 L5 method p.C.over:(I)I
9 L5 expr call
10 L5 field p.C.x:I
11 L5 stmt assign
12 L6 local 1
13 L6 push_int 1
14 L6 call
15 L6 method p.C.over:(F)I
16 L6 expr call
17 L6 field p.C.x:I
18 L6 stmt assign
19 L7 expr literal
20 L7 int 1
21 L7 expr neg
22 L7 push_int 1
23 L7 call
24 L7 path 1
25 L7 name "over"
26 L7 expr call
27 L7 field p.C.x:I
28 L7 stmt assign
29 L8 local 3
30 L8 expr literal
31 L8 int 2
32 L8 push_int 2
33 L8 call
34 L8 method p.C.two:(Lp.A;F)V
35 L8 stmt call
36 L9 expr literal
37 L9 null
38 L9 push_int 1
39 L9 call
40 L9 method p.C.obj:(Ljava.lang.Object;)V
41 L9 stmt call
42 L10 expr literal
43 L10 str "s"
44 L10 push_int 1
45 L10 call
46 L10 method p.C.str:(Ljava.lang.String;)V
47 L10 stmt call
48 L11 expr literal
49 L11 int 1
50 L11 push_int 1
51 L11 call
52 L11 path 1
53 L11 name "nosuch"
54 L11 stmt call
55 L12 expr literal
56 L12 int 3
57 L12 local 3
58 L12 push_int 2
59 L12 call
60 L12 path 2
61 L12 local 3
62 L12 method p.T.set:(ILp.T;)V
63 L12 stmt call
64 L13 field p.C.x:I
65 L13 expr literal
66 L13 int 1
67 L13 expr add
68 L13 local 3
69 L13 push_int 2
70 L13 call
71 L13 path 2
72 L13 local 3
73 L13 name "set"
74 L13 stmt call
75 L14 push_int 0
76 L14 call
77 L14 path 3
78 L14 local 3
79 L14 field p.T.next:Lp.T;
80 L14 method p.T.m:()V
81 L14 stmt call
82 L15 expr literal
83 L15 int 4
84 L15 push_int 1
85 L15 call
86 L15 path 2
87 L15 class p.A
88 L15 method p.A.sm:(I)I
89 L15 stmt call
90 L16 push_int 0
91 L16 push_int 0
92 L16 call
93 L16 path 2
94 L16 local 3
95 L16 method p.T.get:()Lp.T;
96 L16 expr call
97 L16 call_on "m"
98 L16 stmt call
99 L17 push_int 0
100 L17 local 0
101 L17 call_on "m"
102 L17 stmt call
103 L18 push_int 0
104 L18 call_super "m"
105 L18 stmt call
106 L19 expr literal
107 L19 int 5
108 L19 expr literal
109 L19 null
110 L19 push_int 2
111 L19 expr literal
112 L19 int 0
113 L19 field p.C.many:[Lp.T;
114 L19 index
115 L19 call_on "set"
116 L19 stmt call
117 L20 local 2
118 L20 push_int 1
119 L20 call
120 L20 method p.C.obj:(Ljava.lang.Object;)V
121 L20 stmt call
122 L21 local 0
123 L21 push_int 1
124 L21 call
125 L21 method p.C.obj:(Ljava.lang.Object;)V
126 L21 stmt call
127 L22 push_int 0
128 L22 expr new
129 L22 type "Lp.T;"
130 L22 push_int 0
131 L22 new_init
132 L22 call_on "m"
133 L22 stmt call
134 L23 local 3
135 L23 push_int 1
136 L23 call
137 L23 method p.C.ifc:(Lp.I;)V
138 L23 stmt call
139 L24 push_void
140 L24 release 3
141 L24 release 2
142 L24 release 1
143 L24 release 0
144 L24 ret_value
145 L24 release 3
146 L24 release 2
147 L24 release 1
148 L24 release 0
''')])

T_OPERATORS = (r'''package p;
class C extends T {
    static float F = 1.5;
    int r = frontend:0x005Cr;
    String s = "a\tb\"c";
    int e = 1 +
        2 * 3;
    void o(int a, int b, T t, Object q) {
        x = a && b
            || !a;
        x = a < b ? -a : ~b;
        t = (T)q;
        x = (int)1.5 + (a);
        if (q instanceof T) x++;
        arr = new int[a + 1];
        t = new T();
        x = a = b;
        a *= 2; a /= 2; a -= 1; a &= 1; a |= 1; a ^= 1;
        x = +a - --b + b++ ;
        x = true;  t = null;
        x = a << 1 >> 2 & 3 | 4 ^ 5;
        x = a % 2 / 3 != 4 == 5 >= 6 <= 7 > 8;
    }
}
''', [(0, 0, r'''
0 L3 expr literal
1 L3 float 0x3FC00000
2 L3 end
'''), (1, 0, r'''
0 L4 expr literal
1 L4 rid frontend.rpk:0x0000005C
2 L4 end
'''), (2, 0, r'''
0 L5 expr literal
1 L5 str "a\tb\"c"
2 L5 end
'''), (3, 0, r'''
0 L6 expr literal
1 L6 int 1
2 L7 expr literal
3 L7 int 2
4 L7 expr literal
5 L7 int 3
6 L7 expr mul
7 L7 expr add
8 L7 end
'''), (4, 5, r'''
0 L9 local 4
1 L10 and_test
2 L10 jump +3
3 L9 local 3
4 L10 expr land
5 L10 or_test
6 L10 jump +4
7 L10 local 4
8 L10 expr lnot
9 L10 expr lor
10 L9 field p.C.x:I
11 L10 stmt assign
12 L11 local 4
13 L11 local 3
14 L11 expr lt
15 L11 local 4
16 L11 expr neg
17 L11 local 3
18 L11 expr bnot
19 L11 expr cond
20 L11 field p.C.x:I
21 L11 stmt assign
22 L12 local 1
23 L12 expr cast
24 L12 class p.T
25 L12 local 2
26 L12 stmt assign
27 L13 expr literal
28 L13 float 0x3FC00000
29 L13 expr cast
30 L13 type "I"
31 L13 local 4
32 L13 expr add
33 L13 field p.C.x:I
34 L13 stmt assign
35 L14 local 1
36 L14 expr instanceof
37 L14 type "Lp.T;"
38 L15 jump_false +3
39 L14 field p.C.x:I
40 L14 stmt postinc
41 L15 local 4
42 L15 expr literal
43 L15 int 1
44 L15 expr add
45 L15 expr newarray
46 L15 type "[I"
47 L15 field p.C.arr:[I
48 L15 stmt assign
49 L16 expr new
50 L16 type "Lp.T;"
51 L16 push_int 0
52 L16 new_init
53 L16 local 2
54 L16 stmt assign
55 L17 local 3
56 L17 local 4
57 L17 expr assign
58 L17 field p.C.x:I
59 L17 stmt assign
60 L18 expr literal
61 L18 int 2
62 L18 local 4
63 L18 stmt mul_assign
64 L18 expr literal
65 L18 int 2
66 L18 local 4
67 L18 stmt div_assign
68 L18 expr literal
69 L18 int 1
70 L18 local 4
71 L18 stmt sub_assign
72 L18 expr literal
73 L18 int 1
74 L18 local 4
75 L18 stmt and_assign
76 L18 expr literal
77 L18 int 1
78 L18 local 4
79 L18 stmt or_assign
80 L18 expr literal
81 L18 int 1
82 L18 local 4
83 L18 stmt xor_assign
84 L19 local 4
85 L19 local 3
86 L19 expr predec
87 L19 expr sub
88 L19 local 3
89 L19 expr postinc
90 L19 expr add
91 L19 field p.C.x:I
92 L19 stmt assign
93 L20 expr literal
94 L20 bool 1
95 L20 field p.C.x:I
96 L20 stmt assign
97 L20 expr literal
98 L20 null
99 L20 local 2
100 L20 stmt assign
101 L21 local 4
102 L21 expr literal
103 L21 int 1
104 L21 expr shl
105 L21 expr literal
106 L21 int 2
107 L21 expr shr
108 L21 expr literal
109 L21 int 3
110 L21 expr band
111 L21 expr literal
112 L21 int 4
113 L21 expr literal
114 L21 int 5
115 L21 expr bxor
116 L21 expr bor
117 L21 field p.C.x:I
118 L21 stmt assign
119 L22 local 4
120 L22 expr literal
121 L22 int 2
122 L22 expr mod
123 L22 expr literal
124 L22 int 3
125 L22 expr div
126 L22 expr literal
127 L22 int 4
128 L22 expr ne
129 L22 expr literal
130 L22 int 5
131 L22 expr literal
132 L22 int 6
133 L22 expr ge
134 L22 expr literal
135 L22 int 7
136 L22 expr le
137 L22 expr literal
138 L22 int 8
139 L22 expr gt
140 L22 expr eq
141 L22 field p.C.x:I
142 L22 stmt assign
143 L23 push_void
144 L23 release 4
145 L23 release 3
146 L23 release 2
147 L23 release 1
148 L23 release 0
149 L23 ret_value
150 L23 release 4
151 L23 release 3
152 L23 release 2
153 L23 release 1
154 L23 release 0
''')])

# The defaults of appendix B: no shipped source has these constructs, the
# trees are the plain extension of the rules (warnings in default mode).
T_UNVERIFIED = (r'''package p;
class C extends T {
    void u(int a) {
        while (a) { if (x) continue; a--; }
        do { if (x) continue; a--; } while (a);
        for (;;) { if (x) continue; break; }
        switch (a) {
            default: x = 0;
            case 1: case 2: x = 1;
            case 3:
        }
        x = (a += 1);
        new T(a);
        a %= 2;
        ;
    }
}
''', [(0, 2, r'''
0 L4 local 1
1 L4 jump_false +7
2 L4 field p.C.x:I
3 L4 jump_false +2
4 L4 jump -4
5 L4 local 1
6 L4 stmt postdec
7 L4 jump -7
8 L5 field p.C.x:I
9 L5 jump_false +2
10 L5 jump +3
11 L5 local 1
12 L5 stmt postdec
13 L5 local 1
14 L5 jump_true -6
15 L6 field p.C.x:I
16 L6 jump_false +2
17 L6 jump +2
18 L6 jump +2
19 L6 jump -4
20 L7 local 1
21 L9 expr literal
22 L9 int 1
23 L11 case +13
24 L9 expr literal
25 L9 int 2
26 L11 case +10
27 L10 expr literal
28 L10 int 3
29 L11 case +11
30 L11 pop
31 L11 jump +1
32 L8 expr literal
33 L8 int 0
34 L8 field p.C.x:I
35 L8 stmt assign
36 L9 expr literal
37 L9 int 1
38 L9 field p.C.x:I
39 L9 stmt assign
40 L12 expr literal
41 L12 int 1
42 L12 local 1
43 L12 expr add_assign
44 L12 field p.C.x:I
45 L12 stmt assign
46 L13 stmt new
47 L13 type "Lp.T;"
48 L13 local 1
49 L13 push_int 1
50 L13 new_init
51 L14 expr literal
52 L14 int 2
53 L14 local 1
54 L14 stmt mod_assign
55 L16 push_void
56 L16 release 1
57 L16 release 0
58 L16 ret_value
59 L16 release 1
60 L16 release 0
''')])

TREES = [
    ('statements', T_STATEMENTS),
    ('loops and constructors', T_LOOPS),
    ('switch', T_SWITCH),
    ('names', T_NAMES),
    ('calls', T_CALLS),
    ('operators and literals', T_OPERATORS),
    ('defaults of appendix B', T_UNVERIFIED),
]


def run_trees(prov):
    """(passed, total, failures) over the trees written out by hand."""
    passed = []
    total = 0
    bad = []
    for name, (text, expected) in TREES:
        try:
            res = compile_case(text, prov)
        except Exception as x:
            total += len(expected)
            bad.append('%s: exception %r' % (name, x))
            continue
        for t, nlocals, want in expected:
            total += 1
            item = '%s#%d' % (name, t)
            if not res.ok:
                d = [x for x in res.diagnostics if x.error][0]
                bad.append('%s: line %d: %s %s' % (item, d.line, d.code, d.text))
                continue
            tree = res.classes[0].trees[t]
            got = dump.dump_tree(tree).split('\n')
            exp = want.strip('\n').split('\n')
            if got and got[-1] == '':
                got.pop()
            if got != exp:
                k = 0
                while k < min(len(got), len(exp)) and got[k] == exp[k]:
                    k += 1
                bad.append('%s: node %d: %s / expected %s' % (
                    item, k, got[k] if k < len(got) else 'end of tree',
                    exp[k] if k < len(exp) else 'end of tree'))
            elif tree.max_locals != nlocals:
                bad.append('%s: locals %d / expected %d' % (item, tree.max_locals, nlocals))
            else:
                passed.append(item)
    return passed, total, bad


# ---------------------------------------------------------------------------
# the lookups, case by case
# ---------------------------------------------------------------------------

O = 'Ljava.lang.Object;'
STR = 'Ljava.lang.String;'
NULL = classes.NULL_TYPE

# (class, method name, argument types, descriptor of the method found or None)
METHOD_CASES = [
    # identical types: score 0
    ('p.S', 'num', ['I'], '(I)I'),
    ('p.S', 'num', ['F'], '(F)I'),
    ('p.S', 'two', ['I', 'I'], '(II)I'),
    # int for a float parameter (101), float for an int parameter (100)
    ('p.S', 'conv', ['I'], '(F)I'),
    ('p.S', 'onlyint', ['F'], '(I)I'),
    # int or float for a String parameter (100), not for any other class
    ('p.S', 'st', ['I'], '(%s)I' % STR),
    ('p.S', 'st', ['F'], '(%s)I' % STR),
    ('p.S', 'ob', ['I'], None),
    ('p.S', 'ar', ['I'], None),
    ('p.S', 'conv', [STR], None),
    # null for an object or an array parameter (100), not for int
    ('p.S', 'ob', [NULL], '(%s)I' % O),
    ('p.S', 'ar', [NULL], '([I)I'),
    ('p.S', 'onlyint', [NULL], None),
    # a subclass for a superclass parameter: the number of steps; the lowest wins
    ('p.S', 'pick', ['Lp.T;'], '(Lp.T;)I'),
    ('p.S', 'pick', ['Lp.A;'], '(Lp.A;)I'),
    ('p.S', 'pick', ['Lp.C;'], '(Lp.T;)I'),
    ('p.S', 'pick', [STR], '(%s)I' % O),
    ('p.S', 'far', ['Lp.T;'], '(Lp.A;)I'),
    ('p.S', 'far', [O], None),
    ('p.S', 'far', [STR], None),
    # a class for a parameter of an interface it (or a superclass) implements
    ('p.S', 'ifc', ['Lp.T;'], '(Lp.I;)I'),
    ('p.S', 'ifc', ['Lp.C;'], '(Lp.I;)I'),
    ('p.S', 'ifc', ['Lp.A;'], None),
    # an array for an Object parameter (1), for its own type (0), for nothing else
    ('p.S', 'ob', ['[I'], '(%s)I' % O),
    ('p.S', 'ob', ['[Lp.T;'], '(%s)I' % O),
    ('p.S', 'ar', ['[I'], '([I)I'),
    ('p.S', 'ar', ['[F'], None),
    ('p.S', 'st', ['[I'], None),
    # the superclasses: score 0 further up beats a conversion nearer by, and
    # among conversions the lowest score of the whole chain wins
    ('p.S2', 'num', ['I'], '(I)I'),
    ('p.S2', 'num', ['F'], '(F)I'),
    ('p.S2', 'conv', ['I'], '(F)I'),
    ('p.S2', 'far', ['Lp.T;'], '(Lp.A;)I'),
    ('p.S2', 'far', [STR], '(%s)I' % O),
    # equal scores: the method met first stays; the scan goes through the
    # instance methods from the last to the first, then the static ones
    # (from the decompilation; no shipped call site has a tie)
    ('p.S', 'tie', [NULL], '(Lp.T;)I'),
    ('p.S', 'mixed', [NULL], '(Lp.T;)I'),
    # a value whose type name resolved nowhere is passed like null; a
    # parameter of that type takes it at score 0
    ('p.S', 'ob', ['L;'], '(%s)I' % O),
    ('p.S', 'unk', ['L;'], '(L;)I'),
    # nothing to find
    ('p.S', 'num', [], None),
    ('p.S', 'num', ['I', 'I'], None),
    ('p.S', 'nosuch', ['I'], None),
    ('p.Nowhere', 'num', ['I'], None),
    ('p.S', 'num', [None], None),
    # an inheritance loop ends the search
    ('p.Loop1', 'num', ['I'], None),
]

# (class, field name, (class that declares it, descriptor, static) or None)
FIELD_CASES = [
    ('p.T', 'x', ('p.T', 'I', False)),
    ('p.T', 'ST', ('p.T', 'Lp.T;', True)),
    ('p.T', 'base', ('p.A', 'I', False)),
    ('p.T', 'SA', ('p.A', 'I', True)),
    ('p.C', 'x', ('p.T', 'I', False)),
    ('p.C', 'link', ('p.A', 'Lp.A;', False)),
    ('p.T', 'IC', None),            # interfaces are not searched
    ('p.I', 'IC', ('p.I', 'I', True)),
    ('p.T', 'nowhere', None),
    ('p.Nowhere', 'x', None),
    ('p.Loop1', 'lf', ('p.Loop2', 'I', False)),
    ('p.Loop1', 'nowhere', None),
]


def run_lookup(prov):
    """(passed, total, failures)."""
    res = slrrc.front('package p;\nclass C extends T { }\n', 'case.java', prov)
    scope = res.scope
    bad = []
    passed = []
    for cls, name, args, want in METHOD_CASES:
        m = classes.find_method(scope, cls, name, args)
        got = None if m is None else m.desc
        item = '%s.%s(%s)' % (cls, name, ','.join('?' if a is None else a for a in args))
        if got == want:
            passed.append(item)
        else:
            bad.append('lookup %s: %r, expected %r' % (item, got, want))
    for cls, name, want in FIELD_CASES:
        hit = classes.find_field(scope, cls, name)
        got = None if hit is None else (hit.owner, hit.field.desc, hit.field.is_static)
        item = '%s.%s' % (cls, name)
        if got == want:
            passed.append(item)
        else:
            bad.append('lookup %s: %r, expected %r' % (item, got, want))
    return passed, len(METHOD_CASES) + len(FIELD_CASES), bad


# ---------------------------------------------------------------------------
# details
# ---------------------------------------------------------------------------

def first_tree(res):
    return dump.dump_tree(res.classes[0].trees[0])


def details(prov):
    """A few results looked at one by one.  Returns the list of failures."""
    bad = []
    # the four float conversions are selectable; 0.00065 is the literal of the
    # corpus on which they differ most (spec/out/float_files.txt)
    want = {'A': 0x3A2A64C1, 'B': 0x3A2A64C4, 'C': 0x3A2A64C1, 'D': 0x3A2A64C3}
    text = M('float q = 0.00065;')
    for mode in 'ABCD':
        res = compile_case(text, prov, mode=mode)
        got = first_tree(res).split('\n')[1]
        if got != '1 L3 float 0x%08X' % want[mode]:
            bad.append('float setting %s: %s' % (mode, got))
    res = slrrc.emit_trees(text, 'case.java', prov, packs.PackNames())
    if first_tree(res).split('\n')[1] != '1 L3 float 0x3A2A64C1':
        bad.append('the default float setting is not A')
    try:
        compile_case(text, prov, mode='E')
        bad.append('float setting E accepted')
    except ValueError:
        pass
    # floats: the integer part wraps at 32 bits, suffix and missing digits
    for lit, bits in (('1.0', 0x3F800000), ('0.5f', 0x3F000000), ('10f', 0x41200000),
                      ('4294967297.0', 0x3F800000), ('0.1', 0x3DCCCCCD)):
        res = compile_case(M('float q = %s;' % lit), prov)
        got = first_tree(res).split('\n')[1]
        if got != '1 L3 float 0x%08X' % bits:
            bad.append('float %s: %s' % (lit, got))
    # pack names: through the interface
    text = M('int q = cars.racers.duhen:0x0000002Ar;\nint r = no.such.pack:0x12r;\n'
             'int s = MAPS.roc_TRACK.smallmap:0x1r;')

    def rids(pk):
        res = compile_case(text, prov, pk=pk)
        return [dump.dump_tree(t).split('\n')[1] for t in res.classes[0].trees]

    got = rids(packs.PackNames())                   # as written
    if got != ['1 L3 rid cars\\racers\\duhen.rpk:0x0000002A',
               '1 L4 rid no\\such\\pack.rpk:0x00000012',
               '1 L5 rid MAPS\\roc_TRACK\\smallmap.rpk:0x00000001']:
        bad.append('pack names as written: %r' % got)
    got = rids(packs.TablePackNames(packs.TABLE_940, None))     # the table alone
    if got != ['1 L3 rid cars\\racers\\Duhen.rpk:0x0000002A',
               '1 L4 rid no\\such\\pack.rpk:0x00000012',
               '1 L5 rid maps\\ROC_Track\\smallmap.rpk:0x00000001']:
        bad.append('pack names from the table: %r' % got)
    files = ['cars\\racers\\Duhen.rpk', 'maps\\ROC_track\\smallmap.rpk', 'Other\\Pack.rpk']
    got = rids(packs.TablePackNames(packs.TABLE_940, files))    # the table over the files
    if got != ['1 L3 rid cars\\racers\\Duhen.rpk:0x0000002A',
               '1 L4 rid system.rpk:0x00000000',
               '1 L5 rid maps\\ROC_Track\\smallmap.rpk:0x00000001']:
        bad.append('pack names from the table and the files: %r' % got)
    got = rids(packs.TablePackNames((), ['CARS\\Racers\\DUHEN.rpk']))     # the files alone
    if got[0] != '1 L3 rid CARS\\Racers\\DUHEN.rpk:0x0000002A' or \
            got[1] != '1 L4 rid system.rpk:0x00000000':
        bad.append('pack names from the files: %r' % got)
    if len(packs.TABLE_940) != 126 or len(set(packs.lower(p) for p in packs.TABLE_940)) != 126:
        bad.append('the pack table does not hold 126 packs')
    # the classes the emitter looked at are consulted classes
    res = compile_case(S('t.m();'), prov)
    for name in ('p.T', 'p.A', 'java.lang.Object'):
        if name not in res.consulted:
            bad.append('%s is not in the consulted classes %r' % (name, res.consulted))
    # a tree of a source with an error is not handed out as a result
    res = compile_case(S('x = nowhere;'), prov)
    if res.ok:
        bad.append('ok with an unknown name')
    # the own class wins over the provider, and a later class of the source
    # is visible in an earlier one
    res = compile_case('package p;\nclass C { int g(D d) { return d.q + A.q; } }\n'
                       'class D { int q; }\nclass A { static int q; }\n', prov)
    if not res.ok or dump.dump_tree(res.classes[0].trees[0]).split('\n')[:5] != [
            '0 L2 path 2', '1 L2 local 1', '2 L2 field p.D.q:I', '3 L2 path 2', '4 L2 class p.A']:
        bad.append('classes of the source itself: %r' % [d.text for d in res.diagnostics])
    # in an interpreter of its own, with nothing but the package on the path
    code = '\n'.join([
        'import sys',
        'sys.path.insert(0, %r)' % harness.ROOT,
        'import slrrc',
        'from slrrc import classes, dump, packs',
        'class P(classes.ClassProvider):',
        '    def find(self, fqn):',
        '        return classes.ClassInfo(fqn) if fqn == "java.lang.Object" else None',
        'r = slrrc.emit_trees("package p; class C { int a; void f() { a = 1; } }", "C.java",',
        '                     P(), packs.PackNames())',
        'assert r.ok, [d.text for d in r.diagnostics]',
        'assert dump.dump_tree(r.classes[0].trees[0]).split("\\n")[2] == "2 L1 field p.C.a:I"',
        'bad = [m for m in ("oracle", "tufa", "lexer", "parser", "symbols", "poolmodel",',
        '       "stmtgen", "gen", "classdb", "spike_compiler") if m in sys.modules]',
        'print("loaded:", bad)'])
    out = subprocess.run([sys.executable, '-I', '-c', code], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT).stdout.decode('utf-8', 'replace').strip()
    if out != 'loaded: []':
        bad.append('package alone: %s' % out)
    return bad


# ---------------------------------------------------------------------------
# mutations against the composition spike
# ---------------------------------------------------------------------------

# Refusals of the package that the spike does not share, by the first words
# of the diagnostic (notes/impl_m1-m3.md section 4, notes/impl_m4.md).
ON_PURPOSE = (
    'not a statement:',
    'this(..) or super(..) elsewhere',
    'declaration as the body of',
    'assignment to something that is not',
    '++ or -- applied to something that is not',
    'return with a value in a constructor',
    'expected ";" in an expression statement',      # this(..) / super(..) with a suffix
    'cast to an array type',
    'instanceof with',
    'array creation',
    'member access directly on new',
    'array type with more than one dimension',
    'expected "[" after the primitive type of new',
    'modifier other than final on',
)


class Tally(object):
    def __init__(self, prov, pk, env):
        self.prov = prov
        self.pk = pk
        self.env = env
        self.universe = env.sym_universe()      # as the corpus has it
        self.texts = 0
        self.same = 0
        self.unknown_type = 0
        self.only_package = 0
        self.only_spike = 0
        self.exceptions = []
        self.differences = []

    def spike(self, text):
        """The trees of the spike for a text.  The spike adds the classes a
        source declares to the symbol universe of its Env and leaves them
        there; for the sources of the corpus that changes nothing, a mutated
        text with a new class name would be seen by every text behind it.
        So each text gets a universe of its own."""
        u = symbols.Universe()
        u.classes = dict(self.universe.classes)
        u.packages = dict((k, set(v)) for k, v in self.universe.packages.items())
        self.env._sym_uni = u
        return t4_trees.spike_trees(self.env, text, 'x')

    def one(self, text, where):
        self.texts += 1
        try:
            res = slrrc.emit_trees(text, 'x.java', self.prov, self.pk, slrrc.Options())
        except Exception as x:
            self.exceptions.append('%s: %r' % (where, x))
            return
        if res.ok:
            # the high-water mark of the emitter is the locals word of the row
            for x in t4_trees.locals_differ(res):
                self.differences.append('%s: %s' % (where, x))
        blobs, why = self.spike(text)
        # The spike has no rule for a type name that resolves nowhere: it
        # raises, or (the type of a field or of a parameter) writes the name
        # into the descriptor, where the package writes "L;".
        unknown_type = any(d.code == 'W_TYPE' for d in res.diagnostics)
        if res.ok and blobs is not None:
            diffs = t4_trees.compare(res, blobs)
            if len(blobs) != len(res.classes):
                diffs.append((0, 0, '%d classes / spike %d' % (len(res.classes), len(blobs))))
            bad = [d for d in diffs if d[2] is not None]
            if not bad:
                self.same += 1
            elif unknown_type:
                self.unknown_type += 1
            else:
                self.differences.append('%s: blob %d tree %d: %s' % ((where,) + bad[0]))
        elif res.ok:
            # ... and none for the import of a single class
            self.only_package += 1
            single_import = any(d.text.startswith('import of a single class')
                                for d in res.diagnostics)
            if not (unknown_type or single_import):
                self.differences.append('%s: compiled, the spike says %s' % (where, why))
        elif blobs is not None:
            self.only_spike += 1
            d = [x for x in res.diagnostics if x.error][0]
            if not d.text.startswith(ON_PURPOSE):
                self.differences.append('%s: line %d %s %s, the spike compiles it' % (
                    where, d.line, d.code, d.text))


def mutations(tally):
    rng = random.Random(941)
    srcs = harness.all_sources()
    for si in range(0, len(srcs), 11):
        e = srcs[si]
        text = harness.load_source(e)
        lines = text.split('\n')
        for m in range(20):
            # move or copy a line: declarations, statements and names change place
            x = rng.randrange(len(lines))
            y = rng.randrange(len(lines))
            l2 = list(lines)
            if rng.randrange(2):
                l2.insert(y, l2.pop(x))
            else:
                l2.insert(y, l2[x])
            tally.one('\n'.join(l2), '%s line edit %d' % (e.source, m))
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
            tally.one(t2, '%s token edit %d' % (e.source, m))


def run(opts):
    oc = harness.Outcome('extra4')
    prov = MemberProvider()
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
    t_passed, t_total, t_bad = run_trees(prov)
    for x in t_bad:
        oc.failures.append('tree: %s' % x)
    l_passed, l_total, l_bad = run_lookup(prov)
    oc.failures.extend(l_bad)
    bad = details(prov)
    for x in bad:
        oc.failures.append('detail: %s' % x)
    tally = Tally(harness.provider(), harness.packs(), spike_compiler.Env())
    mutations(tally)
    oc.failures.extend(tally.exceptions)
    oc.failures.extend(tally.differences)
    oc.text = ('dialect %d/%d cases; trees %d/%d written out by hand; lookup %d/%d; '
               'mutations %d texts, %d exceptions, %d differences from the spike' % (
                   len(passed), len(CASES), len(t_passed), t_total, len(l_passed), l_total,
                   tally.texts, len(tally.exceptions), len(tally.differences)))
    oc.ok = (len(passed) == len(CASES) and len(t_passed) == t_total and
             len(l_passed) == l_total and not bad and tally.texts == 12560 and
             not tally.exceptions and not tally.differences and tally.same > 1000)
    oc.passed['extra4.pass'] = (passed + ['tree ' + x for x in t_passed] +
                                ['lookup ' + x for x in l_passed])
    oc.notes.append('mutated texts both compile, with the same trees: %d; both compile, '
                    'another descriptor for a type name that resolves nowhere: %d; only the '
                    'package compiles (unknown type, single import): %d; only the spike '
                    'compiles (refused on purpose): %d' % (
                        tally.same, tally.unknown_type, tally.only_package, tally.only_spike))
    return oc


if __name__ == '__main__':
    sys.exit(harness.main_single(run, sys.argv[1:]))
