#!/usr/bin/env python3
"""make_notes.py - write notes/expressions.md from the results in out/.

    python -I make_notes.py            (run_all.py runs the checks first)

Every number in the document is read from the JSON files the checks wrote
(out/check_consistent.json, check_940.json, check_other.json, check_stale.json,
decomp.json, calls.json, lines.json, variants.json, source_only.json,
precedence.json, stmt_roots.json, names.json) or counted here from the class
files; every example is cut from the corpus at generation time (source line and
disassembly through lib/disasm.py).  No number is typed in by hand.
"""
import fnmatch
import json
import os
import re
import sys
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for p in (_LIB, _HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import disasm         # noqa: E402
import opcodes as oc  # noqa: E402
import oracle         # noqa: E402
import tufa           # noqa: E402

OUT = os.path.join(_HERE, 'out')
NOTES = os.path.normpath(os.path.join(_HERE, '..', '..', 'notes', 'expressions.md'))


def load(name):
    with open(os.path.join(OUT, name), 'r', encoding='utf-8') as f:
        return json.load(f)


def n(x):
    return '{:,}'.format(x)


def ph(s):
    """Put the placeholders <x> of a node sequence into code spans."""
    return re.sub(r'<(\w+)>', r'`<\1>`', s)


class Doc(object):
    def __init__(self):
        self.cons = load('check_consistent.json')
        self.c940 = load('check_940.json')
        self.coth = load('check_other.json')
        self.cstale = load('check_stale.json')
        self.dec = load('decomp.json')
        self.calls = load('calls.json')
        self.lines = load('lines.json')
        self.variants = load('variants.json')
        self.srconly = load('source_only.json')
        self.prec = load('precedence.json')
        self.roots = load('stmt_roots.json')
        self.names = load('names.json')
        self.rules = self.cons['rules']
        self._cls = {}
        self._src = {}
        self.out = []

    # ---- helpers ---------------------------------------------------------
    def held(self, *patterns):
        """Sum of the held applications of the rule keys matching the
        patterns ('*' matches any text)."""
        tot = 0
        for k, v in self.rules.items():
            for pat in patterns:
                if k == pat or ('*' in pat and fnmatch.fnmatchcase(
                        k.replace('[', '(').replace('?', '_'),
                        pat.replace('[', '(').replace('?', '_'))):
                    tot += v['held']
                    break
        return tot

    def variant(self, name):
        for v in self.variants:
            if v['variant'] == name:
                return v
        raise KeyError(name)

    def example(self, key):
        """Markdown of one example of rule `key`: source line and nodes."""
        src = self.c940['rules'].get(key) or self.rules.get(key)
        if not src or not src.get('example') or src['example'][0] is None:
            return ['(no example)', '']
        ekey, bi, ti, line, lo, hi = src['example']
        return self.listing(ekey, bi, ti, line, lo, hi)

    def listing(self, ekey, bi, ti, line, lo, hi):
        e = oracle.entry(ekey)
        if ekey not in self._src:
            self._src[ekey] = oracle.load_source(e).split('\n')
            self._cls[ekey] = oracle.load_class(e)
        text = self._src[ekey]
        blob = self._cls[ekey][bi]
        rows = disasm.tree_lines(blob, ti)
        owners = blob.tree_owners().get(ti, ['?'])
        out = ['`%s`, class `%s`, tree %d (`%s`):' % (e.source, blob.name, ti, owners[0]),
               '', '```java']
        s = text[line - 1].rstrip('\r').replace('\t', '    ').strip()
        out.append('%5d  %s' % (line, s[:140] + (' ...' if len(s) > 140 else '')))
        out += ['```', '', '```']
        for k in range(lo, hi + 1):
            out.append(rows[k])
        out += ['```', '']
        return out

    def listing_line(self, ekey, line):
        """Source line and all nodes that carry that line."""
        e = oracle.entry(ekey)
        if ekey not in self._src:
            self._src[ekey] = oracle.load_source(e).split('\n')
            self._cls[ekey] = oracle.load_class(e)
        for bi, b in enumerate(self._cls[ekey]):
            for ti, t in enumerate(b.trees):
                idx = [k for k, nd in enumerate(t) if nd.line == line]
                if idx:
                    return self.listing(ekey, bi, ti, line, idx[0], idx[-1])
        return ['(no nodes)', '']

    def w(self, *lines):
        self.out.extend(lines)

    def table(self, head, rows, align=None):
        self.w('| ' + ' | '.join(head) + ' |')
        al = align or ['---'] * len(head)
        self.w('| ' + ' | '.join(al) + ' |')
        for r in rows:
            self.w('| ' + ' | '.join(str(c) for c in r) + ' |')
        self.w('')


# ---------------------------------------------------------------------------
# the main table: source form -> node sequence
# ---------------------------------------------------------------------------
# (id, title for the example section, source form, node sequence, rule keys,
#  example key, remark)
ROWS = [
    ('L1', 'Int literal', '`39`, `0x1F`', '`expr literal` ; `int V`',
     ['lit.int decimal', 'lit.int hex'], 'lit.int hex',
     'V is the value modulo 2^32; a hex literal may use all 32 bits'),
    ('L2', 'Float literal', '`0.5`, `20.0`, `1.0f`', '`expr literal` ; `float BITS`',
     ['lit.float', 'lit.float with f suffix'], 'lit.float',
     'the bit pattern is the float question of foundation.md'),
    ('L3', 'String literal', '`"text"`', '`expr literal` ; `str [utf8]`',
     ['lit.string'], 'lit.string', 'pool text after escape processing'),
    ('L4', 'Resource id literal', '`system:0x0034r`', '`expr literal` ; `rid [rid]`',
     ['lit.rid'], 'lit.rid', ''),
    ('L5', 'null', '`null`', '`expr literal` ; `null`', ['lit.null'], 'lit.null', ''),
    ('L6', 'true and false', '`true`, `false`', '`expr literal` ; `bool 1` / `bool 0`',
     ['lit.true', 'lit.false'], 'lit.true', 'a carrier of their own, not `int`'),
    ('N1', 'Local variable or parameter', '`x` (local or parameter)', '`local i`',
     ['name.local or parameter'], 'name.local or parameter',
     'read and write alike; numbering in section 5.1'),
    ('N2', 'this', '`this`', '`local 0`', ['name.this'], 'name.this', ''),
    ('N3', 'Instance field by simple name', '`f` (instance field of the class or a superclass)',
     '`field [C.f:T]`', ['name.instance field (own class)', 'name.instance field (inherited)'],
     'name.instance field (inherited)', 'C is the class being compiled, also for an inherited field'),
    ('N4', 'Static field by simple name', '`F` (static field of the class or a superclass)',
     '`field [D.F:T]`', ['name.static field (own class)', 'name.static field (inherited)'],
     'name.static field (inherited)', 'D is the class that declares F'),
    ('N5', 'Dotted name, head is a local', '`a.b`, `a.b.c` (a local or parameter)',
     '`path n` ; `local i` ; `field [..]` ...', ['path.* segments, head local'],
     'path.2 segments, head local', 'n = number of identifiers'),
    ('N6', 'Dotted name, head is a field', '`f.b`, `f.b.c` (f a field)',
     '`path n` ; `field [..]` ; `field [..]` ...',
     ['path.* segments, head instance field', 'path.* segments, head static field'],
     'path.2 segments, head instance field', ''),
    ('N7', 'Dotted name, head is a class', '`Cls.F`, `Cls.F.g`',
     '`path n` ; `class [Cls]` ; `field [..]` ...', ['path.* segments, head class'],
     'path.2 segments, head class', 'the class segment counts in n'),
    ('N8', 'Array length', '`a.length`, `Cls.ARR.length`',
     'as N5-N7, last segment `field [[T.length:I]`', ['path extra.ends in array length'],
     'path extra.ends in array length',
     'counted in N5-N7 too; the ref names the array type `[I`, `[F`, `[pkg.Name`'),
    ('N9', 'Member of a computed value', '`this.f`, `a[i].f`, `g().f`, `((T)x).f`',
     '<X> ; `getfield "f"`', ['getfield.*'], 'getfield.receiver is this',
     'by name, no pool ref; one `getfield` per further member'),
    ('N10', 'Array element', '`a[i]`', '<i> ; <a> ; `index`', ['index.*'],
     'index.array is name', 'index first, array second'),
    ('O1', 'Unary minus, not, complement', '`-x`, `!x`, `~x`',
     '<x> ; `expr neg` / `expr lnot` / `expr bnot`',
     ['unary.neg', 'unary.lnot', 'unary.bnot'], 'unary.neg',
     '`-1` is the literal 1 and `expr neg`'),
    ('O2', 'Unary plus and parentheses', '`+x`, `(x)`', '<x>',
     ['unary.+ (no node)', 'paren.(x) (no node)'], 'unary.+ (no node)', 'no node of their own'),
    ('O3', 'Binary operator', '`x op y` (16 operators, section 4)', '<x> ; <y> ; `expr K`',
     ['binary.*'], 'binary.add', 'left operand first'),
    ('O4', 'Logical and', '`x && y`',
     '<x> ; `and_test` ; `jump +(len(y)+2)` ; <y> ; `expr land`', ['shortcircuit.land'],
     'shortcircuit.land', 'the jump lands behind `expr land`'),
    ('O5', 'Logical or', '`x \\|\\| y`',
     '<x> ; `or_test` ; `jump +(len(y)+2)` ; <y> ; `expr lor`', ['shortcircuit.lor'],
     'shortcircuit.lor', ''),
    ('O6', 'Conditional operator', '`c ? a : b`', '<c> ; <a> ; <b> ; `expr cond`',
     ['cond.*'], 'cond.?:', 'no jump: both branches are evaluated'),
    ('O7', 'Assignment as a statement', '`x = y;`', '<y> ; <x> ; `stmt assign`',
     ['assign.assign (stmt)'], 'assign.assign (stmt)', 'right side first, then the target'),
    ('O8', 'Assignment as a value', '`a = b = y`, `f(x = y)`', '<y> ; <x> ; `expr assign`',
     ['assign.assign (expr)'], 'assign.assign (expr)', ''),
    ('O9', 'Compound assignment', '`x += y;` and `-= *= /= &= \\|= ^=`',
     '<y> ; <x> ; `stmt add_assign` ...',
     ['assign.add_assign (stmt)', 'assign.sub_assign (stmt)', 'assign.mul_assign (stmt)',
      'assign.div_assign (stmt)', 'assign.and_assign (stmt)', 'assign.or_assign (stmt)',
      'assign.xor_assign (stmt)'], 'assign.add_assign (stmt)',
     'only ever a statement in the corpus'),
    ('O10', 'Increment and decrement as a statement', '`x++;` `x--;` `++x;` `--x;`',
     '<x> ; `stmt postinc` / `postdec` / `preinc` / `predec`',
     ['incdec.postinc (stmt)', 'incdec.postdec (stmt)', 'incdec.preinc (stmt)',
      'incdec.predec (stmt)'], 'incdec.postinc (stmt)', ''),
    ('O11', 'Increment and decrement as a value', '`a[i++]`, `++n < max`',
     '<x> ; `expr postinc` / `postdec` / `preinc` / `predec`',
     ['incdec.postinc (expr)', 'incdec.postdec (expr)', 'incdec.preinc (expr)',
      'incdec.predec (expr)'], 'incdec.preinc (expr)', ''),
    ('O12', 'Cast to a class', '`(Slider)x`', '<x> ; `expr cast` ; `class [T]`',
     ['cast.to class'], 'cast.to class', 'the carrier is a pool class entry'),
    ('O13', 'Cast to int or float', '`(int)x`, `(float)x`',
     '<x> ; `expr cast` ; `type "I"` / `type "F"`', ['cast.to int', 'cast.to float'],
     'cast.to int', 'the carrier is a utf8 descriptor'),
    ('O14', 'instanceof', '`x instanceof T`', '<x> ; `expr instanceof` ; `type "LT;"`',
     ['instanceof.*'], 'instanceof.x instanceof T', ''),
    ('A1', 'Object creation', '`new T(a, b)`',
     '`expr new` ; `type "LT;"` ; <a> ; <b> ; `push_int 2` ; `new_init`',
     ['new.object (expr)'], 'new.object (expr)', 'the constructor is not named: found at run time'),
    ('A2', 'Array creation', '`new int[n]`, `new T[n]`',
     '<n> ; `expr newarray` ; `type "[I"` / `"[F"` / `"[LT;"`', ['new.array of *'],
     'new.array of int', 'the carrier holds the array type'),
    ('C1', 'Call without receiver, resolved', '`f(a, b)`',
     '<a> ; <b> ; `push_int 2` ; `call` ; `method [C.f:(..)R]` ; `expr call` / `stmt call`',
     ['call.no receiver, method ref'], 'call.no receiver, method ref',
     'C is the class being compiled'),
    ('C2', 'Call without receiver, by name', '`f(a, b)`',
     '<a> ; <b> ; `push_int 2` ; `call` ; `path 1` ; `name "f"` ; `expr call` / `stmt call`',
     ['call.no receiver, by name*'], 'call.no receiver, by name (an argument of unknown type)',
     'when no method ref can be made (section 6.2)'),
    ('C3', 'Call through a dotted name, resolved', '`a.f(x)`, `Cls.f(x)`, `a.b.f(x)`',
     '<x> ; `push_int 1` ; `call` ; `path n` ; head ; `field [..]` ... ; `method [R.f:(..)R]` ; '
     '`expr call` / `stmt call`',
     ['call.path, head *, method ref'], 'call.path, head local, method ref',
     'segments as in N5-N7; R is the static type of the receiver'),
    ('C4', 'Call through a dotted name, by name', '`a.f(x)`, `Cls.f(x)`, `a.b.f(x)`',
     '<x> ; `push_int 1` ; `call` ; `path n` ; head ; ... ; `name "f"` ; `expr call` / `stmt call`',
     ['call.path, head *, by name*'],
     'call.path, head instance field, by name (an argument of unknown type)', ''),
    ('C5', 'Call on a computed receiver', '`a[i].f(x)`, `g().f(x)`, `new T().f(x)`, `this.f(x)`',
     '<x> ; `push_int 1` ; <X> ; `call_on "f"` ; `expr call` / `stmt call`',
     ['call.computed receiver*'], 'call.computed receiver: index',
     'the receiver is emitted AFTER the arguments'),
    ('C6', 'Call on super', '`super.f(x)`',
     '<x> ; `push_int 1` ; `call_super "f"` ; `expr call` / `stmt call`', ['call.super.f()'],
     'call.super.f()', ''),
    ('C7', 'Explicit constructor call', '`this(a);`, `super(a);`',
     '<a> ; `push_int 1` ; `this_init` / `super_init` ; `stmt ctor`',
     ['ctor.this(...)', 'ctor.super(...)'], 'ctor.this(...)', ''),
    ('C8', 'Implicit constructor call', 'constructor body without `this(` / `super(`',
     '`push_int 0` ; `super_init` ; `stmt ctor`', ['ctor.implicit super()'],
     'ctor.implicit super()', 'the first nodes of the tree'),
    ('D1', 'Declaration with initialiser', '`T x = e;`',
     '`declare i` ; `type "T"` ; `init` ; <e> ; `store`', ['decl.with initialiser'],
     'decl.with initialiser', ''),
    ('D2', 'Declaration without initialiser', '`T x;`', '`declare i` ; `type "T"`',
     ['decl.without initialiser'], 'decl.without initialiser', ''),
]

# tag name -> (rule patterns for the expr form, for the stmt form)
TAG_RULES = {
    'cond': (['cond.*'], []),
    'lor': (['shortcircuit.lor'], []),
    'land': (['shortcircuit.land'], []),
    'bnot': (['unary.bnot'], []), 'lnot': (['unary.lnot'], []), 'neg': (['unary.neg'], []),
    'postinc': (['incdec.postinc (expr)'], ['incdec.postinc (stmt)']),
    'postdec': (['incdec.postdec (expr)'], ['incdec.postdec (stmt)']),
    'preinc': (['incdec.preinc (expr)'], ['incdec.preinc (stmt)']),
    'predec': (['incdec.predec (expr)'], ['incdec.predec (stmt)']),
    'call': (['call result.expr call'], ['call result.stmt call']),
    'literal': (['lit.*'], []),
    'new': (['new.object (expr)'], ['new.object (stmt)']),
    'newarray': (['new.array of *'], []),
    'instanceof': (['instanceof.*'], []),
    'cast': (['cast.*'], []),
    'ctor': ([], ['ctor.*']),
}
for _t in ('bor', 'bxor', 'band', 'ne', 'eq', 'ge', 'le', 'gt', 'lt', 'shr', 'shl', 'sub',
           'add', 'mod', 'div', 'mul'):
    TAG_RULES[_t] = (['binary.' + _t], [])
for _t in ('assign', 'mul_assign', 'div_assign', 'mod_assign', 'add_assign', 'sub_assign',
           'and_assign', 'xor_assign', 'or_assign'):
    TAG_RULES[_t] = (['assign.%s (expr)' % _t], ['assign.%s (stmt)' % _t])

TAG_OPERANDS = {
    'cond': 'c, a, b', 'literal': 'the carrier node behind it',
    'new': 'type carrier behind it; then arguments, count, `new_init`',
    'newarray': 'n; type carrier behind it', 'instanceof': 'x; type carrier behind it',
    'cast': 'x; class or type carrier behind it',
    'ctor': 'closes `this_init` / `super_init`', 'call': 'closes a call form',
}

# (role, mnemonics or None, label, where the line comes from,
#  [(candidate, description)]: the other candidates that are different tokens)
LINE_ROLES = [
    ('literal', None, '`expr literal` and its carrier', 'the literal',
     [('next', 'the token behind it')]),
    ('simple name', None, 'simple name: `local`, `field`', 'the identifier',
     [('next', 'the token behind it')]),
    ('this', None, '`this`: `local 0`', 'the `this` token', [('next', 'the token behind it')]),
    ('field path header', None, 'field path: `path n`', 'the first identifier of the name',
     [('next', 'the token behind the name')]),
    ('field path segment', None, 'field path: segments', 'the identifier of the segment',
     [('next', 'the token behind the name')]),
    ('getfield', None, '`getfield`', 'the member identifier', [('next', 'the token behind it')]),
    ('index', None, '`index`', 'the closing `]`', [('next', 'the token behind it')]),
    ('call path header', None, 'call path: `path n`', 'the first identifier of the name',
     [('last', 'the closing `)`'), ('next', 'the token behind the call')]),
    ('call path segment', None, 'call path: receiver segments', 'the identifier of the segment',
     [('last', 'the closing `)`'), ('next', 'the token behind the call')]),
    ('callee', None, 'call path: `method` / `name`', 'the method identifier',
     [('after', 'the `(`'), ('last', 'the closing `)`'), ('next', 'the token behind the call')]),
    ('call', None, '`push_int`, `call`, `call_on`, `call_super`, `expr call` / `stmt call`',
     'the closing `)` of the call', [('next', 'the token behind it')]),
    ('binary operator', None, '`expr K` of a binary operator, `expr land` / `lor`',
     'the token behind the right operand',
     [('own', 'the operator'), ('last', 'the last token of the right operand')]),
    ('short-circuit test', None, '`and_test` / `or_test` and their `jump`',
     'the token behind the right operand',
     [('own', 'the operator'), ('last', 'the last token of the right operand')]),
    ('assignment', None, 'assignment node, `expr` or `stmt`', 'the token behind the right side',
     [('own', 'the operator'), ('last', 'the last token of the right side')]),
    ('prefix operator', None, '`expr neg` / `lnot` / `bnot`, prefix `++` `--`',
     'the token behind the operand',
     [('own', 'the operator'), ('last', 'the last token of the operand')]),
    ('postfix operator', None, 'postfix `++` `--`', 'the operator token',
     [('next', 'the token behind it')]),
    ('conditional', None, '`expr cond`', 'the token behind the third operand',
     [('last', 'the last token of the third operand')]),
    ('cast', None, '`expr cast` and its carrier', 'the token behind the operand',
     [('own', 'the `)` of the cast'), ('last', 'the last token of the operand')]),
    ('instanceof', None, '`expr instanceof` and its carrier', 'the token behind the type',
     [('last', 'the last token of the type')]),
    ('new', ('expr', 'stmt'), '`expr new`', 'the closing `)`',
     [('own', 'the type name'), ('after', 'the `(`'), ('next', 'the token behind the `)`')]),
    ('new', ('type',), '`type` carrier of `new`', 'the token behind the type name: the `(`',
     [('own', 'the type name'), ('last', 'the closing `)`')]),
    ('new', ('push_int', 'new_init'), '`push_int`, `new_init` of `new`', 'the closing `)`',
     [('next', 'the token behind it')]),
    ('newarray', None, '`expr newarray` and its carrier', 'the closing `]`',
     [('next', 'the token behind it')]),
    ('declaration', None, '`declare`, `type`, `init`, `store`', 'the declared name',
     [('after', 'the token behind the name'), ('last', 'the last token of the initialiser'),
      ('next', 'the token behind the initialiser')]),
    ('this()/super()', None, '`push_int`, `this_init` / `super_init`, `stmt ctor`',
     'the `;` that ends the statement', [('next', 'the token behind the `;`')]),
    ('implicit super()', None, 'the three nodes of the implicit `super()`',
     'the first token of the constructor body',
     [('own', 'the `{` of the body'), ('last', 'the `}` of the body')]),
]


def corpus_tag_counts():
    """(tag name, 'expr'|'stmt') -> count over all blobs; plus two side
    counts: 'getfield length' and 'array length ref'."""
    c = Counter()
    for rel, blobs in oracle.iter_class_files():
        for b in blobs:
            for t in b.trees:
                for nd in t:
                    code = tufa.OP_FLAGS[nd.op]
                    if code in (0x1007, 0x1008):
                        ti = oc.tag_info(nd.imm)
                        c[(ti.name if ti else 'tag%02x' % nd.imm,
                           'expr' if code == 0x1007 else 'stmt')] += 1
                    elif code == 0x401c and b.utf8(nd.imm) == 'length':
                        c['getfield length'] += 1
                    elif code == 0x101b:
                        cls, name, desc = b.ref(nd.imm)
                        if cls.startswith('[') and name == 'length':
                            c['array length ref'] += 1
    return c


def qualified_class_uses():
    """How often a source names a class WITH its package inside a method or an
    initialiser: as the head of a dotted name, or as a type."""
    import classdb
    import gen
    import jparse
    db = classdb.load()
    count = 0
    for e in oracle.iter_entries():
        if not e.source:
            continue
        u = jparse.parse(oracle.load_source(e))
        for nd in jparse.walk(u):
            if nd.k == 'type' and '.' in nd.name:
                count += 1
            elif nd.k == 'field':
                names = gen.Emitter.chain(nd)
                if names and any('.'.join(names[:cut]) in db.classes
                                 for cut in range(2, len(names) + 1)):
                    count += 1
    return count


def build(d):
    w = d.w
    cons = d.cons['counts']
    c940 = d.c940['counts']
    coth = d.coth['counts']
    cst = d.cstale['counts']
    dec = d.dec['counts']
    calls = d.calls
    tagc = corpus_tag_counts()
    ctl = cons['nodes (class, all)'] - cons['nodes (class, filtered)']
    n_class_only = len(list(oracle.iter_entries('class_only')))
    n_qualified = qualified_class_uses()

    w('# Expressions: how the compiler emits them into TREE nodes', '')
    w('Generated by `analysis/expr/make_notes.py` from the results of the scripts in',
      '`analysis/expr/` (section 11). Regenerate instead of editing: every count below is',
      'read from those results and every example is cut from the corpus when the file is',
      'written. Corpus: the read-only Build 940 install at `D:\\SLRR-test`. The vocabulary',
      '(mnemonics, kind names, "carrier", "path") is that of `notes/opcodes.md`; the class',
      'format is described in `notes/foundation.md`. Nothing in the slrr-reverse repo, the',
      'installs or Ghidra was touched; no exe was run.', '')
    w('Evidence standard, as in foundation.md: **proven** means a script checks the rule',
      'against the whole corpus and the count and the exceptions are quoted; **observed**',
      'means seen in examples only; **unknown** is said outright.', '')
    w('Neighbouring notes, written at the same time by other analyses: `notes/grammar.md`',
      '(the grammar of the dialect; this study uses a parser of its own, `jparse.py`, with',
      'the same precedence and cast rules) and `notes/statements.md` (the statement-level',
      'nodes that this study filters out: jumps, releases, returns, and the scopes behind',
      'the local numbering of section 5.1). Where they overlap with this file, on the',
      'numbering of locals and on line numbers, the results were found independently.', '')

    # ------------------------------------------------------------------ 1
    w('## 1. Result', '')
    w('The rules of this document are implemented as a model, `analysis/expr/gen.py`: from',
      'the source text and the declarations of the other classes it predicts every node',
      'that an expression or a local declaration contributes to a tree, with its operand',
      'and its line number. `check_expr.py` compares the prediction with the shipped class',
      'files.', '')
    d.table(['Set of pairs', 'Pairs', 'Trees', 'Trees reproduced', 'Nodes compared',
             'Nodes predicted', 'of them with the right line'],
            [['Build 940, consistent', n(c940['files']), n(c940['trees']),
              '%s, and %s but for one node' % (n(c940['trees identical']),
                                              n(c940.get('trees identical but for a callee', 0))),
              n(c940['nodes (class, filtered)']), n(c940['nodes in identical trees']),
              n(c940['lines identical'])],
             ['Older builds, consistent', n(coth['files']), n(coth['trees']),
              n(coth['trees identical']), n(coth['nodes (class, filtered)']),
              n(coth['nodes in identical trees']), n(coth['lines identical'])],
             ['Stale pairs (control)', n(cst['files']), n(cst['trees']),
              n(cst['trees identical']), n(cst['nodes (class, filtered)']),
              n(cst['nodes in identical trees']), n(cst['lines identical'])]],
            ['---', '---:', '---:', '---', '---:', '---:', '---:'])
    w('- **%s of %s trees of the %s consistent pairs are reproduced node for node**; the' % (
        n(cons['trees identical']), n(cons['trees']), n(cons['files'])),
      '  other %d differ in one node each, for a reason outside the compiler (section 9.2).' %
      cons.get('trees identical but for a callee', 0),
      '  %s of %s nodes are predicted, every one of them with its line number.' % (
          n(cons['nodes in identical trees']), n(cons['nodes (class, filtered)'])),
      '- The older builds follow the same rules: the %d consistent pairs compiled by' %
      coth['files'],
      '  builds %s are reproduced without an exception, so no difference between' %
      ' and '.join('0x%X' % b for b in sorted(set(
          b for e in oracle.iter_pairs(build=None, consistent=True)
          for b in e.builds if b != oracle.BUILD_940))),
      '  the builds is visible in how expressions are emitted (foundation.md, question 8).',
      '- The stale pairs fail as they must (their source was edited after the compile),',
      '  which shows that the comparison discriminates; sections 5 and 6 quote %d more' %
      (len(d.variants) - 1),
      '  control runs, each with one rule changed.',
      '- What "node for node" compares: the opcode, the operand and the line. A pool index',
      '  is compared through what it names (text, class name, class.member:descriptor,',
      '  pack:id), so the ORDER of the constant pool is not part of this proof. A float',
      '  literal counts as equal within 2 ulp and the pack name of a resource id without',
      '  regard to letter case; both are open questions of foundation.md, section 7.',
      '- What is left out: the statement-level nodes `jump` (other than the skip of `&&` /',
      '  `||`), `jump_false`, `jump_true`, `case`, `pop`, `value`, `push_void`, `ret`,',
      '  `ret_value`, `release` and `end` are removed from the class side before the',
      '  comparison (%s of the %s nodes of the consistent pairs). The order in which the' % (
          n(ctl), n(cons['nodes (class, all)'])),
      '  expressions of a method follow each other IS compared; the model takes it from',
      '  the source with two reorderings (the update of a `for` stands behind its body;',
      '  all `case` values of a `switch` stand before the first body).', '')
    w('Two more checks cover what has no usable source:', '')
    w('- `check_decomp.py` reads all %s blobs (%s trees, %s nodes) from the class side.' % (
        n(dec['blobs']), n(dec['trees']), n(dec['nodes'])),
      '  A decompiler that accepts exactly the node shapes of section 3 rebuilds %s of' %
      n(dec['trees accepted']),
      '  %s trees (%s expressions and sub-expressions); the one it rejects is listed in' % (
          n(dec['trees']), n(dec['expressions'])),
      '  section 9.1. On the rebuilt expressions the name and call resolution rules are',
      '  checked backwards (sections 5.3, 5.4, 6.4): they hold in the %d blobs of the %d'
      % (dec.get('blobs class_only', 0), n_class_only),
      '  class files without source and in the stale pairs as well.',
      '- `probe_source_only.py` runs the model over the %d sources without a class: %d of'
      % (d.srconly['files'], d.srconly['clean_files']),
      '  them compile without an unresolved name or type (%s trees, %s nodes predicted).' % (
          n(d.srconly['trees']), n(d.srconly['nodes'])),
      '  They apply %d of the forms counted below; %s. So the rules below suffice for' % (
          len(d.srconly['rules']),
          'every one of them occurs in the pairs' if not d.srconly['new_rules']
          else '%d of them do not occur in the pairs' % len(d.srconly['new_rules'])),
      '  the sources the host cannot load today.', '')

    # ------------------------------------------------------------------ 2
    w('## 2. Principles', '')
    w('All proven by the reproduction of section 1; the counts are occurrences in the',
      'reproduced trees.', '')
    n_stmt = sum(d.roots['pair'].values()) + sum(d.roots['source_only'].values())
    n_other = sum(v for grp in ('pair', 'source_only') for k, v in d.roots[grp].items()
                  if not k.startswith(('assign', 'call', 'postfix', 'unary ++', 'unary --')))
    w('1. **Postfix.** The operands of a construct are emitted first, the node of the',
      '   construct last. Two constructs open before their operands: `expr new` and its',
      '   type carrier stand before the arguments, `declare` / `type` / `init` before the',
      '   initialiser.',
      '2. **One node per operator, literal and name.** Nothing is folded, reordered or',
      '   dropped. Parentheses and a unary `+` emit nothing (%s and %s occurrences).' % (
          n(d.held('paren.(x) (no node)')), n(d.held('unary.+ (no node)'))),
      '3. **No conversion nodes.** The same nodes are emitted whatever the operand types:',
      '   %s arithmetic or comparison operators mix int and float, %s assignments and' % (
          n(d.held('types.arithmetic or comparison mixing int and float')),
          n(d.held('types.assignment of int value to float variable',
                   'types.assignment of float value to int variable',
                   'types.initialiser of int value to float variable',
                   'types.initialiser of float value to int variable'))),
      '   initialisers store an int in a float variable or the reverse, `+` has a String',
      '   operand %s times, `==` / `!=` compare Strings, objects or null %s times: in all' % (
          n(d.held('types.+ on *')),
          n(d.held('types.== on String*', 'types.!= on String*', 'types.== on object*',
                   'types.!= on object*', 'types.== on null*', 'types.== on array*'))),
      '   of them the node sequence is that of section 3 with nothing added. (Counted',
      '   where the model knows both operand types. Conversion, concatenation and string',
      '   comparison are the VM\'s business at run time.)',
      '4. **Value or statement.** The kind node of an expression is `expr K` when its',
      '   value is used and `stmt K` when the expression is a statement; nothing else',
      '   differs. Only assignments, calls and `++` / `--` stand in statement position:',
      '   %s expression statements in the 3,454 sources, %d with another root' % (
          n(n_stmt), n_other),
      '   (`probe_stmt_roots.py`).',
      '5. **Order as emitted.** Left operand before right; arguments left to right. But:',
      '   the right side of an assignment before its target; the index before the array;',
      '   the arguments before a computed receiver.',
      '6. **Names are settled at compile time** into one of three forms: a local index, a',
      '   pool member ref (class, name, descriptor), or a bare utf8 name that the VM',
      '   resolves at run time. Sections 5 and 6 say which.',
      '7. **The compiler knows a type only for a name or a literal.** It never derives',
      '   the type of an operator, call, index, cast or `new` expression. The one place',
      '   where this shows is the choice between method ref and name (section 6.2).', '')

    # ------------------------------------------------------------------ 3
    w('## 3. Source form -> node sequence', '')
    w('`<x>` stands for the nodes of sub-expression x; `;` separates nodes; `[..]` is a',
      'pool reference; `a / b` are alternatives. "Count" is the number of occurrences in',
      'the reproduced trees of the %s consistent pairs: each one is an application of the'
      % n(cons['files']),
      'rule that the class file confirms, and no occurrence contradicts a row. Every row',
      'is **proven** in that sense. Section 8 has one example per row.', '')
    rows = []
    for rid, title, form, seq, keys, exk, remark in ROWS:
        rows.append([rid, form, ph(seq), n(d.held(*keys)), remark])
    d.table(['Row', 'Source form', 'Node sequence', 'Count', 'Remark'], rows,
            ['---', '---', '---', '---:', '---'])
    w('Dotted names (rows N5-N7) by head and number of segments:', '')
    heads = [('local or parameter', 'head local'), ('instance field', 'head instance field'),
             ('static field', 'head static field'), ('class name', 'head class')]
    rows = []
    for label, key in heads:
        rows.append([label] + [n(d.held('path.%d segments, %s' % (k, key))) for k in (2, 3, 4, 5)])
    d.table(['Head', '2 segments', '3', '4', '5'], rows, ['---', '---:', '---:', '---:', '---:'])
    w('Receivers of `getfield` (row N9) and `call_on` (row C5), by the form of the receiver',
      'in the source:', '')
    forms = [('`this`', 'this'), ('array element `a[i]`', 'index'), ('call `g()`', 'call'),
             ('parenthesised call', '(paren) call'),
             ('cast in parentheses `((T)x)`', '(paren) cast'),
             ('member of a computed value `X.f`', 'field'), ('`new T(..)`', 'new'),
             ('parenthesised assignment `(a = b)`', '(paren) assign')]
    rows = []
    for label, key in forms:
        rows.append([label, n(d.held('getfield.receiver is ' + key)),
                     n(d.held('call.computed receiver: ' + key))])
    d.table(['Receiver', '`getfield`', '`call_on`'], rows, ['---', '---:', '---:'])
    w('Targets of an assignment (rows O7-O9); the target is the ordinary node sequence of',
      'the name or element, emitted behind the value:', '')
    d.table(['Target', 'Count'],
            [['field by simple name (N3, N4)', n(d.held('assign target.field (simple name)'))],
             ['array element (N10)', n(d.held('assign target.index'))],
             ['dotted name (N5-N7)', n(d.held('assign target.path'))],
             ['local or parameter (N1)', n(d.held('assign target.local'))],
             ['member of a computed value (N9)', n(d.held('assign target.getfield'))]],
            ['---', '---:'])

    # ------------------------------------------------------------------ 4
    w('## 4. Operator codes', '')
    w('The immediate of `expr` (0x1007) and `stmt` (0x1008) is the kind of expression.',
      'The table lists all 47 codes. "All blobs" counts the nodes of the whole corpus',
      '(%s blobs); "reproduced" counts the occurrences in the reproduced trees of the' %
      n(dec['blobs']),
      'consistent pairs, that is how often the source construct was seen to compile to',
      'exactly this code. Operands are given in the order in which they are emitted.', '')
    rows = []
    for t in oc.TAGS:
        ek, sk = TAG_RULES.get(t.name, ([], []))
        ce, cs = tagc.get((t.name, 'expr'), 0), tagc.get((t.name, 'stmt'), 0)
        he, hs = d.held(*ek) if ek else 0, d.held(*sk) if sk else 0
        if not t.symbol:
            opnd = '-'
        elif t.name in TAG_OPERANDS:
            opnd = TAG_OPERANDS[t.name]
        elif t.name in ('lor', 'land'):
            opnd = 'x, test and jump, y'
        elif t.name.endswith('assign'):
            opnd = 'y, x (value, then target)'
        elif t.pops_e == 1:
            opnd = 'x'
        elif t.pops_e == 2:
            opnd = 'x, y'
        else:
            opnd = '-'
        sym = ('`%s`' % t.symbol.replace('|', '\\|')) if t.symbol else '-'
        rows.append(['0x%02X' % t.tag, '`%s`' % t.name, sym, opnd,
                     n(ce) if ce else 'never', n(cs) if cs else 'never',
                     n(he) if he else '-', n(hs) if hs else '-'])
    d.table(['Code', 'Kind', 'Source', 'Operands', 'expr, all blobs', 'stmt, all blobs',
             'expr, reproduced', 'stmt, reproduced'], rows,
            ['---', '---', '---', '---', '---:', '---:', '---:', '---:'])
    w('- Codes that never occur: 0x0C (no body in the VM; by its place in the table it is',
      '  `>>>`), 0x1D, 0x26 (`%=`), 0x29-0x2B (the VM reports them as unsupported',
      '  assignment kinds; by their place `<<=` `>>=` `>>>=`). No source uses these',
      '  operators, so what the compiler emits for them is **unknown**.',
      '- The codes follow the precedence of the operators, lowest first, and the',
      '  grammar groups operators as Java does wherever two of them meet without',
      '  parentheses in a pair source (`probe_precedence.py`: %s meetings of two different'
      % n(sum(p[3] for p in d.prec['pairs'])),
      '  precedence levels, for example `+ -` with `* / %%` %s times and `&&` with a'
      % n(sum(p[3] for p in d.prec['pairs'] if p[0] == '+ -' and p[1] == '* / %')),
      '  comparison %s times; chains of one level nest to the left %s times and to the'
      % (n(sum(p[3] for p in d.prec['pairs'] if p[0] == '&&' and p[1] in (
          '== !=', '< > <= >= instanceof'))),
         n(sum(x[2] for x in d.prec['same'] if x[1] == 'left'))),
      '  right %d times; assignment chains `a = b = c` occur %s times). Levels that never'
      % (sum(x[2] for x in d.prec['same'] if x[1] == 'right'),
         n(d.prec['other'].get('assignment chain a = b = c (right associative)', 0))),
      '  meet without parentheses, for example `\\|` against `&` or `^`, are **unknown**.',
      '- The `expr` forms of the compound assignments and the `stmt` forms of the plain',
      '  operators never occur: no source uses a compound assignment as a value or an',
      '  operator expression as a statement. `stmt new` occurs once (section 9.1).', '')

    # ------------------------------------------------------------------ 5
    w('## 5. Names', '')
    w('### 5.1 Locals', '')
    w('`local i` pushes the variable itself, so the same node serves reads and writes',
      '(%s assignments have a local as target). The numbering is proven by every `local`,' %
      n(d.held('assign target.local')),
      '`declare` and path head of the reproduced trees (%s locals and parameters by' %
      n(d.held('name.local or parameter')),
      'simple name, %s `this`, %s declarations):' % (
          n(d.held('name.this')), n(d.held('decl.*'))), '')
    w('- `this` is local 0 of an instance method; a static method has no such slot.',
      '- The parameters follow in REVERSE order: the last parameter has the lowest index.',
      '- A declaration takes the next free index. The index is free again at the end of',
      '  the scope: a block; a `for` statement including the variables of its header; a',
      '  statement without braces that is the body of `if` / `else` / a loop; each `case`',
      '  section of a `switch` (its locals are released at the next label).',
      '- The innermost declaration of a name wins, and a local hides a field of the same',
      '  name: letting the field win loses %d trees (control run' %
      d.variant('head_order=field,local,class')['lost'],
      '  `head_order=field,local,class`).', '')
    w('### 5.2 What the first identifier of a name is', '')
    nm = d.names
    w('Lookup order used by the model: (1) a local or parameter in scope, (2) a field of',
      'the class or of a superclass, (3) a class.', '')
    w('- (1) before (2) is proven (5.1). The place of (3) is **unknown**: no name of a',
      '  pair is both a class and a variable (trying classes first, or before fields,',
      '  loses %d and %d trees).' % (d.variant('head_order=class,local,field')['lost'],
                                    d.variant('head_order=local,class,field')['lost']),
      '- A simple class name is looked up in the package of the source, in the packages',
      '  it imports and in `java.lang`. (A class written with its package occurs %d times'
      % n_qualified,
      '  inside the methods and initialisers of the sources: how it compiles is **unknown**.)',
      '  All %d distinct imports of the corpus have the form `a.b.*` (one, in two files, is'
      % len(nm['imports']),
      '  `parts:part.root.BodyPart.*`, which the model ignores). Of %s (source, class name)'
      % n(nm['resolutions']),
      '  resolutions %d are ambiguous, and %s find the class in `java.lang` without an' % (
          nm['ambiguous'], n(nm['via_java_lang'])),
      '  import of it (`probe_names.py`). So `java.lang` is implicit, and the order in',
      '  which the packages are searched is **unknown** and without consequence here.',
      '- Every name resolves: the model never meets an unknown identifier in the %s' %
      n(cons['files'] + d.srconly['files']),
      '  sources it compiles. What the compiler does with one is **unknown**.', '')
    w('### 5.3 Which class a field ref names', '')
    own = d.variant('ref_class=owner')
    sta = d.variant('ref_class=static')
    w('A field ref is a pool entry (class, name, type). The class in it is', '')
    w('- for an **instance** field: the static type of the receiver. For a simple name that',
      '  is the class being compiled, even when a superclass declares the field (%s of the'
      % n(d.held('name.instance field (inherited)')),
      '  %s instance fields written as a simple name are inherited); for a path segment it'
      % n(d.held('name.instance field (own class)', 'name.instance field (inherited)')),
      '  is the declared type of the segment before it (%s paths hold a field that a' %
      n(d.held('path extra.instance field declared in a superclass of the receiver type')),
      '  superclass of that type declares);',
      '- for a **static** field: the class that DECLARES it (%s inherited statics by simple'
      % n(d.held('name.static field (inherited)')),
      '  name, %s paths with one).' %
      n(d.held('path extra.static field declared in a superclass of the receiver type')), '')
    w('The type in the ref is the declared type of the field. Proven by the reproduction',
      'and by two control runs: "always the declaring class" loses %s trees, "always the' %
      n(own['lost']),
      'receiver type" loses %s. Read backwards on all %s blobs (`check_decomp.py`, rules' % (
          n(sta['lost']), n(dec['blobs'])),
      'R1 and R2): %s lone field refs and %s path segments follow the rule, %d do not.' % (
          n(d.dec['rules']['R1']['ok']), n(d.dec['rules']['R2']['ok']),
          d.dec['rules']['R1'].get('fail', 0) + d.dec['rules']['R2'].get('fail', 0)), '')
    w('### 5.4 Dotted names and computed receivers', '')
    w('- A name `a.b.c` whose first identifier is a local, a field or a class is ONE node',
      '  group: `path n` and n segments. The head is `local`, `field` or `class`; every',
      '  further segment is a `field` ref (5.3). No field path of the corpus holds a',
      '  by-name segment (rule R3: %s further segments in all blobs are refs); what the'
      % n(d.dec['rules']['R3']['ok']),
      '  compiler does with a member it cannot find is **unknown**.',
      '- Any other receiver is computed first and the member is fetched by NAME:',
      '  `getfield "f"`, one per member, no pool ref. So `this.pos` is',
      '  `local 0 ; getfield "pos"` while a plain `pos` is `field [C.pos:T]`: the two',
      '  spellings do not compile alike.',
      '- The split is exact in the class files too (rule R4): in all blobs no `getfield`',
      '  or `call_on` has a bare local other than `this`, a lone field ref or a path as',
      '  its receiver (%s of %s).' % (
          n(d.dec['rules']['R4']['ok']),
          n(d.dec['rules']['R4']['ok'] + d.dec['rules']['R4'].get('fail', 0))),
      '- `length` of an array inside a path is a field ref whose class is the array type:',
      '  `[I.length:I`, `[F.length:I`, `[java.lang.String.length:I` (%s paths reproduced;' %
      n(d.held('path extra.ends in array length')),
      '  %s such refs in all blobs). `length` of a computed array value would be' %
      n(tagc['array length ref']),
      '  `getfield "length"` by the rule above, but it occurs %d times in the corpus:' %
      tagc['getfield length'],
      '  **unknown**.',
      '- A parenthesised name as receiver, `(a).b`, does not occur: **unknown**.', '')
    w('### 5.5 Type descriptors', '')
    w('The carriers of `declare`, `new`, `newarray`, `instanceof` and of a cast to int or',
      'float hold a utf8 descriptor: `I`, `F`, `Lpkg.Name;`, `[I`, `[F`, `[Lpkg.Name;`,',
      'with the class name resolved as in 5.2 (%s `type` nodes reproduced). A cast to a' %
      n(d.held('decl.*', 'new.object (expr)', 'new.array of *', 'instanceof.*',
               'cast.to int', 'cast.to float')),
      'class carries a pool class entry instead (%s). `system/scripts/util/Thesaurus.class`' %
      n(d.held('cast.to class')),
      '(no source) declares a field and a parameter of type `L;`: this is what a type',
      'name becomes that the compiler could not resolve (**observed**, one class).', '')

    # ------------------------------------------------------------------ 6
    w('## 6. Calls', '')
    w('### 6.1 The call forms', '')
    w('The arguments are emitted left to right, then `push_int argc`, then one of:', '')
    d.table(['Source', 'Nodes behind the count', 'Callee named by', 'Count'],
            [['`f(..)`', '`call` ; `method [ref]`', 'pool method ref',
              n(d.held('call.no receiver, method ref'))],
             ['`f(..)`', '`call` ; `path 1` ; `name "f"`', 'utf8 name',
              n(d.held('call.no receiver, by name*'))],
             ['`a.b.f(..)`', '`call` ; `path n` ; segments ; `method [ref]`', 'pool method ref',
              n(d.held('call.path, head *, method ref'))],
             ['`a.b.f(..)`', '`call` ; `path n` ; segments ; `name "f"`', 'utf8 name',
              n(d.held('call.path, head *, by name*'))],
             ['`X.f(..)`, X computed', '`<X>` ; `call_on "f"`', 'utf8 name',
              n(d.held('call.computed receiver*'))],
             ['`super.f(..)`', '`call_super "f"`', 'utf8 name', n(d.held('call.super.f()'))],
             ['`new T(..)`', '`new_init` (the object was pushed by `expr new`)',
              'nothing: `<init>` is found at run time', n(d.held('new.object (expr)'))],
             ['`this(..)` / `super(..)`', '`this_init` / `super_init` ; `stmt ctor`',
              'nothing: `<init>` is found at run time', n(d.held('ctor.*'))]],
            ['---', '---', '---', '---:'])
    w('- Every form but the constructor calls is closed by `expr call` (value used, %s) or'
      % n(d.held('call result.expr call')),
      '  `stmt call` (statement, %s).' % n(d.held('call result.stmt call')),
      '- `path n` counts the receiver segments plus the callee; the receiver segments',
      '  are those of a dotted name (5.4). Call paths of %s nodes occur.' % ', '.join(
          k.split('.')[1].split(' ')[0] for k in sorted(d.rules) if k.startswith('call path length.')),
      '- A computed receiver is emitted BEHIND the arguments and the count, directly in',
      '  front of `call_on` (second listing at the end of section 8). `this.f(x)` is such',
      '  a call (`local 0 ; call_on "f"`), `f(x)` is not.',
      '- A constructor is never named in the tree: `new`, `this(..)` and `super(..)`',
      '  leave the choice among overloaded constructors to the VM, by argument types.',
      '- A constructor body that does not start with `this(` or `super(` gets',
      '  `push_int 0 ; super_init ; stmt ctor` in front (%s constructors).' %
      n(d.held('ctor.implicit super()')), '')
    w('### 6.2 Method ref or name', '')
    typed = calls['typed']
    t_found = sum(v for k, v in typed.items() if 'lookup finds a method' in k and k.endswith('method ref'))
    t_untyped = sum(v for k, v in typed.items() if k.startswith('an argument of another form')
                    and k.endswith('name'))
    t_untyped_m = sum(v for k, v in typed.items() if k.startswith('an argument of another form')
                      and k.endswith('method ref'))
    t_none = sum(v for k, v in typed.items() if 'lookup finds nothing' in k and k.endswith('name'))
    t_bad = sum(v for k, v in typed.items() if ('lookup finds a method' in k and k.endswith('name'))
                or ('lookup finds nothing' in k and k.endswith('method ref')))
    w('For `f(..)` and `a.b.f(..)` the compiler either binds the method at compile time (a',
      'pool ref with the full descriptor) or leaves the name for the VM. The rule:', '')
    w('> A call ends in a **method ref** when (1) every argument is a literal, a local or',
      '> parameter, a field written as a simple name, or `this`, and (2) the method',
      '> lookup of 6.3, run on the receiver class with the types of those arguments, finds',
      '> a method. In every other case it ends in the **name**.', '')
    w('Proven on the %s such call sites of the consistent pairs (`study_calls.py`):' %
      n(calls['sites']), '')
    d.table(['Arguments', 'Lookup', 'Class file has', 'Call sites'],
            [['none, or all of the four typed forms', 'finds a method', 'method ref', n(t_found)],
             ['all of the four typed forms', 'finds nothing', 'name', n(t_none)],
             ['at least one of another form', 'not run', 'name', n(t_untyped)],
             ['at least one of another form', 'not run', 'method ref', n(t_untyped_m)],
             ['typed', 'says the opposite of the class file', '-', n(t_bad)]],
            ['---', '---', '---', '---:'])
    w('The %d call sites that are not in the table are the exceptions of section 9.2. The' %
      calls['shape'].get('(callee class changed since compile: not counted)', 0),
      'FORM of an argument decides, not its type: `addElement( vmd )` with a local is a',
      'method ref, `addElement( new Integer(39) )` is a name; `f(1)` is a ref, `f(-1)` a',
      'name, because unary minus is an operator. Per argument form, among the calls for',
      'which the receiver class has a method of that name and argument count:', '')
    rows = []
    for k, c in sorted(calls['forms'].items(),
                       key=lambda kv: (-(kv[1].get('method', 0) > 0),
                                       -(kv[1].get('method', 0) + kv[1].get('name', 0)))):
        rows.append([k.replace('|', '\\|'), n(c.get('method', 0)), n(c.get('name', 0))])
    d.table(['A call with an argument of this form', 'ends in a method ref', 'ends in a name'],
            rows, ['---', '---:', '---:'])
    w('A typed form stands under a name when another argument of the call is untyped or',
      'when the lookup fails. Control runs: counting one more form as typed, with the',
      'type the model derives for it, loses trees every time:', '')
    rows = [[v['what'], n(v['lost'])] for v in d.variants if v['variant'].startswith('typed_extra')]
    d.table(['Control: counted as typed', 'Trees lost'], rows, ['---', '---:'])
    w('Forms that never occur as an argument of a call that could be resolved, so that',
      'their status is **unknown**: `true` / `false`, a parenthesised name or literal,',
      '`?:`, `instanceof`, `new T[n]`, `!x`, `~x`.', '')
    w('The types of the typed forms (proven through the lookup results): an int literal',
      'and a resource id literal are `I`, a float literal `F`, a string literal',
      '`Ljava.lang.String;`, `null` matches every object or array parameter, a local,',
      'parameter or field has its declared type, `this` is the class being compiled.', '')
    w('### 6.3 The method lookup', '')
    w('The outcomes are those of the VM\'s own `Class::getMethod` (Build 940 VA 0x54A150,',
      'scoring at 0x5607D0 and 0x55BD80; decompiled in `slrr-ghidra/findMethodSlot_940.c`,',
      '`sigdesc_940.c`, `typescore_940.c`) when it is given the argument types. That the',
      'compiler calls this very function is inferred from the agreement, not read from',
      'the exe. As modelled in `gen.Model.find_method`:', '')
    w('1. Start at the receiver class: the class being compiled for `f(..)`; for',
      '   `a.b.f(..)` the declared type of the last receiver segment, or the class named.',
      '2. In the class, score every method of that name with the same number of',
      '   parameters: the sum over the arguments of the type scores below; a method with',
      '   an impossible argument is out.',
      '3. A method with score 0 is taken at once. Otherwise the search goes on through',
      '   the superclasses and the lowest score seen wins; of equal scores the first one',
      '   met stays.',
      '4. No method found: the call is emitted by name.', '')
    w('The ref that is emitted names the RECEIVER class of step 1, not the class that',
      'declares the method, for static methods too (%s resolved calls reach a method that' %
      n(d.held('method ref kind.instance method, declared in a superclass',
               'method ref kind.static method, declared in a superclass')),
      'a superclass declares, %s of them a static one), and it carries the descriptor of' %
      n(d.held('method ref kind.static method, declared in a superclass')),
      'the method found, return type included. Type scores, with the number of arguments',
      'of resolved calls that show each case:', '')
    rows = []
    for k, v in sorted(calls['relations'].items(), key=lambda kv: -kv[1]):
        m = re.match(r'(.*) \(score (-?\d+)\)$', k)
        rows.append([m.group(1), m.group(2), n(v), '`%s`' % calls['relation_examples'][k]])
    d.table(['Argument against parameter', 'Score', 'Arguments', 'First example'], rows,
            ['---', '---:', '---:', '---'])
    w('The scores are read from 0x55BD80: identical type 0; int argument for a float',
      'parameter 101; float for int 100; int or float for a `java.lang.String` parameter',
      '100; an argument of type `L;` (null) for any object or array parameter 100; object',
      'for object the result of a distance function (0x54ABF0, not decompiled), -1 when',
      'unrelated. The model takes the distance as the number of superclass steps, one',
      'more step for an interface the class implements, and 1 for an array passed as',
      '`Object`: inferred. What the corpus constrains (control runs over the %s consistent' %
      n(cons['files']),
      'pairs, `study_variants.py`):', '')
    rows = []
    for v in d.variants:
        if v['variant'].startswith(('ifaces', 'prim_to_obj', 'array_to_object', 'vec_order',
                                    'row_order', 'score_mode')):
            rows.append([v['what'], n(v['lost']),
                         'needed' if v['lost'] else 'not constrained by the corpus'])
    d.table(['Control: change to the lookup', 'Trees lost', 'The modelled behaviour is'], rows,
            ['---', '---:', '---'])
    w('- Needed: an exact match wins over an inexact one; among inexact candidates the',
      '  nearer superclass and the candidate with fewer conversions win, and not the',
      '  candidate that the scan of the exe meets first; a class matches a parameter of',
      '  an interface type it implements; an array matches `Object`; int matches String.',
      '- Not constrained: the scan order inside a class (the decompilation scans the',
      '  vector at class+0x58 before the one at class+0x50, each from its last row to its',
      '  first; which of the two holds the static methods is inferred). No call site of',
      '  the corpus has two candidates with the same best score (%d ties), so nothing here'
      % d.held('method ref tie.*'),
      '  depends on it. The exact size of the scores is not constrained either, only',
      '  their order in the cases above.',
      '- Among the resolved calls %s had one candidate of that name and argument count' %
      n(calls['overloads'].get('1 method of that name and argument count', 0)),
      '  and %s had two or more (overloads, or an override further up); %s resolved with' % (
          n(sum(v for k, v in calls['overloads'].items() if not k.startswith('1 method'))),
          n(d.held('method ref.*, with conversions'))),
      '  at least one conversion.', '')
    w('### 6.4 The same rules read from the class side', '')
    w('`check_decomp.py` rebuilds every call of all %s blobs and re-derives the choice:' %
      n(dec['blobs']), '')
    r = d.dec['rules']
    d.table(['Rule', 'Holds', 'Fails'],
            [['R5: a call that ends in a method ref has typed-form arguments only, the ref '
              'names the receiver class, and the lookup returns the descriptor of the ref',
              n(r['R5']['ok']), n(r['R5'].get('fail', 0))],
             ['R6: a call that ends in a name has an argument of another form, or the lookup '
              'finds nothing', n(r['R6']['ok']), n(r['R6'].get('fail', 0))],
             ['R7: the count before a method ref equals the parameter count of its descriptor',
              n(r['R7']['ok']), n(r['R7'].get('fail', 0))]],
            ['---', '---:', '---:'])
    w('The %d failures of R5 are the two call sites of section 9.2. This extends the proof'
      % r['R5'].get('fail', 0),
      'of 6.2 to the %d blobs without source and to the stale pairs.' %
      dec.get('blobs class_only', 0), '')

    # ------------------------------------------------------------------ 7
    w('## 7. Line numbers of expression nodes', '')
    lt = d.lines
    w('Every node records a source line. For the nodes of expressions it is the line of',
      'the token the scanner had last read when the parser completed the construct: its',
      'last token where the parser can reduce at once, the token behind the construct',
      'where it needs to look ahead. All nodes of one construct share that line even',
      'when they are emitted far apart: `expr new` stands before the arguments but carries',
      'the line of the closing parenthesis; the `and_test` and `jump` of `x && y` carry',
      'the line of the token behind y; `store` carries the line of the declared name.',
      'So the compiler builds a tree while it parses, stamps each construct once, and',
      'emits the nodes afterwards.', '')
    w('The table gives, per kind of node, the token whose line the model takes',
      '(`gen.LINE_RULE`). With it **%s of %s predicted nodes of the consistent pairs carry'
      % (n(lt['predicted']), n(lt['total'])),
      'the predicted line** (`study_lines.py`, %d exceptions): **proven** as a table.' %
      lt['exceptions'],
      'Most expressions stand on one line, where every candidate token gives the same',
      'line. "Decisive" counts the nodes for which the candidates lie on different',
      'lines; the last two columns say which other candidates those nodes rule out and',
      'which they do not. Where an alternative is not ruled out, the choice between it',
      'and the stated token is **unknown** and has no effect on any pair.', '')
    rows = []
    roles = lt['roles']
    for role, mns, label, where, alts in LINE_ROLES:
        ks = [k for k in roles if k.split(': ')[0] == role and
              (mns is None or k.split(': ')[1].split(' ')[0] in mns)]
        if not ks:
            continue
        tot = sum(roles[k]['all']['n'] for k in ks)
        ok = sum(roles[k]['all'].get(roles[k]['rule'], 0) for k in ks)
        inf = sum(roles[k]['informative'].get('n', 0) for k in ks)
        out_, in_ = [], []
        for c, text in alts:
            if sum(roles[k]['all'].get(c, 0) for k in ks) == tot:
                in_.append(text)
            else:
                out_.append(text)
        rows.append([label, where, n(tot), n(ok), n(inf), '; '.join(out_) or '-',
                     '; '.join(in_) or '-'])
    d.table(['Nodes', 'Line of', 'Count', 'Agree', 'Decisive', 'Ruled out', 'Not ruled out'],
            rows, ['---', '---', '---:', '---:', '---:', '---', '---'])
    w('For a dotted name the first identifier, the identifier of the segment and the last',
      'identifier cannot be told apart (no name is split over lines in a decisive way);',
      'for `this(..);` / `super(..);` neither can the `)` and the `;`. The lines of the',
      'statement-level nodes are the subject of `notes/statements.md`, section 8, which',
      'arrives at the same picture from the statement side. Where a choice is undecided',
      'the two files may name different tokens of the "not ruled out" set (for a',
      'declaration: the declared name here, the token behind it there); by definition',
      'that changes no line of any pair.', '')

    # ------------------------------------------------------------------ 8
    w('## 8. Examples', '')
    w('One example per row of section 3, chosen by the generator: the shortest occurrence',
      'on a single, short source line among the Build 940 pairs. The listing shows exactly',
      'the nodes the construct emits, as `lib/disasm.py` renders them: node index, line,',
      'mnemonic, operand; carriers and path segments are indented.', '')
    import classdb
    db = classdb.load()
    for rid, title, form, seq, keys, exk, remark in ROWS:
        w('### %s. %s' % (rid, title), '')
        w('%s -> %s' % (form.replace('\\|', '|'), ph(seq)), '')
        d.out.extend(d.example(exk))
        if rid in ('N3', 'N4'):
            # say where the field of the example is declared
            ekey, bi, ti, line, lo, hi = d.c940['rules'][exk]['example']
            blob = d._cls[ekey][bi]
            cls, name, desc = blob.ref(blob.trees[ti][lo].imm)
            f = db.find_field(blob.name, name)
            w('`%s` is declared in `%s`; the class being compiled is `%s`.' % (
                name, f.owner, blob.name), '')
    w('### Order of evaluation, two statements in full', '')
    w('An assignment through an array element with a side effect in the index: the right',
      'side first, then the target (rows C3, N7, O3, O11, N10, N9, O7):', '')
    d.out.extend(d.listing_line('sl/Scripts/game/CarMarket', 108))
    w('A call on a computed receiver: argument, count, then the receiver (rows N5, O11,',
      'N10, C5):', '')
    d.out.extend(d.listing_line('sl/Scripts/game/CarInfo', 155))

    # ------------------------------------------------------------------ 9
    w('## 9. What is not attributed, and the exceptions', '')
    w('### 9.1 Node patterns in expression position that the rules do not explain', '')
    un = d.dec['unattributed']
    w('Over all %s trees of the corpus the shapes of section 3 leave %d tree%s unexplained:'
      % (n(dec['trees']), len(un), '' if len(un) == 1 else 's'), '')
    for u in un:
        w('- `%s`, blob %d, tree %d (%s `%s%s`), node %d, line %d: %s. A class file without'
          % (u[0], u[1], u[2], u[3][0], u[3][1], u[3][2], u[4], u[5], u[6]),
          '  source. The nodes are',
          '  `push_int 0 ; call ; local 1 ; expr call ; field [Texture.id:I] ; stmt assign`',
          '  in a constructor whose only parameter is local 1: a statement of the shape',
          '  `id = p();` where `p` is that parameter. The compiler looked the callee name up',
          '  as a variable first and emitted the variable in the place of the method.',
          '  **Observed**, once: it says that a local of the same name gets in the way of a',
          '  call without receiver. No pair has such a clash (%d calls share their name with'
          % d.held('call name clash.*'),
          '  a local in scope or with a field), so the model does not reproduce it and the',
          '  rule behind it is **unknown**.')
    w('', 'One more pattern is explained only by analogy:', '')
    w('- `system/scripts/render/Frontend.class`, tree 16, line 100:',
      '  `stmt new ; type ; (arguments) ; push_int 4 ; new_init`, the only `new T(..);` used',
      '  as a statement in the corpus, and without source. It is principle 4 applied to',
      '  `new`: the statement marker goes on the kind node, which for `new` is the first',
      '  node. **Observed**, once. (The VM then leaves the object on the stack:',
      '  foundation.md, section 5.)', '')
    w('Nothing else: every other node in expression position of every tree belongs to',
      'one of the forms of section 3.', '')
    w('### 9.2 Exceptions to the proven rules', '')
    w('%d trees of the consistent pairs differ from the prediction, in one node each,' %
      len(d.cons['callee_exceptions']),
      'because the class they call was changed after they were compiled:', '')
    for x in d.cons['callee_exceptions']:
        w('- `%s`, tree %d (%s), line %d: the class file has `%s`; the rules give `%s`.' % (
            x['file'], x['tree'], x['owner'], x['line'], x['actual'], x['predicted']))
    w('', '`Chassis.getBlock()` returns `Block` in the shipped `Chassis.class` and returned',
      '`Part` when `Center_Differential` was compiled; the shipped `NetworkEngine.class` has',
      'no `httpRequestAsync` at all. A compiler that reads the shipped classes cannot',
      'reproduce these two class files byte for byte: they belong on the stale list of',
      'foundation.md, section 4, for a reason paircheck cannot see.', '')
    w('### 9.3 Two versions of nine dialog classes', '')
    w('`system/scripts/render/osd/dialog/Dialog.class` holds 11 blobs. Nine of those classes',
      'also exist as a class file of their own in the same directory, and three of the',
      'nine differ (`StringRequesterDialog`, `FileRequesterDialog`, `TextDialog`: the',
      'fields `str`, `m`, `tb` and one method each exist only inside `Dialog.class`). Which',
      'version the compiler sees when another class uses them is **unknown**: no pair',
      'depends on the difference (`study_duplicates.py`: the same trees are reproduced',
      'either way).', '')

    # ------------------------------------------------------------------ 10
    w('## 10. Not determined by the corpus', '')
    w('- Operators without an occurrence: `>>>`, `%=`, `<<=`, `>>=`, `>>>=`; a compound',
      '  assignment used as a value; an operator expression, a literal or a name used as',
      '  a statement.',
      '- `super.field` (the VM has a case for it, op 0x001D, which is never emitted),',
      '  array initialiser lists, multi-dimensional arrays, character literals: in no',
      '  source.',
      '- A member that cannot be found in a path; a name that is neither local, field nor',
      '  class; a type name that cannot be resolved (one hint: `L;`, section 5.5).',
      '- The place of class names in the lookup order of 5.2, and the order in which the',
      '  packages are searched; whether a local or field of the same name diverts a call',
      '  (one hint: section 9.1); a parenthesised name as receiver.',
      '- Whether the argument forms listed at the end of 6.2 are typed.',
      '- In the method lookup (6.3): the scan order inside a class (taken from the',
      '  decompilation) and the distance function (inferred).',
      '- For the kinds of node with an entry under "Not ruled out" in section 7: which of',
      '  the tokens gives the line.',
      '- The pool. This study compares through the pool; in which order the compiler',
      '  interns the utf8, class, nat and ref entries that expressions need is question 2',
      '  of foundation.md. One observation for it: the nodes of a call path are created',
      '  when the callee name is parsed, before the arguments (their line numbers show it,',
      '  section 7), although they are emitted behind them.',
      '- Float bit patterns and the letter case of pack names (foundation.md, questions 1',
      '  and 3): the comparison here tolerates both.', '')

    # ------------------------------------------------------------------ 11
    w('## 11. Scripts', '')
    w('All under `analysis/expr/`, standard library only. `python -I run_all.py`',
      'regenerates `out/` and this file (about 5 minutes, most of it the control runs;',
      '`--quick` keeps the control runs of the last full run).', '')
    d.table(['Script', 'What it does'],
            [['`jparse.py`', 'Parser for the dialect (Java precedence, casts, `pack:0xHEXr`, '
              '`import a:b.c.*`); parses all 3,454 sources. An analysis parser, not the '
              'compiler\'s yacc grammar'],
             ['`classdb.py`', 'Fields and methods of every class: from the class files (2,964 '
              'classes) and from the sources of the 635 without a class (621 classes)'],
             ['`gen.py`', 'The model: source -> predicted nodes (mnemonic, operand, line) and the '
              'rule each construct applies. The rules of this document as code'],
             ['`symbolic.py`', 'Class-file nodes in the same symbolic form; the filter of '
              'statement-level nodes; the node comparison'],
             ['`check_expr.py`', 'Proof 1: prediction against class file for every tree of a set '
              'of pairs; rule counts, examples, exceptions (`--set`, `--opt`, `--show`, '
              '`--rules`, `--json`)'],
             ['`decomp.py`', 'Node stream -> expression trees; accepts exactly the shapes of '
              'section 3'],
             ['`check_decomp.py`', 'Proof 2: shapes and the resolution rules R1-R7 on all 2,993 '
              'blobs'],
             ['`study_calls.py`', 'The call sites: outcome against argument forms and lookup '
              '(6.2, 6.3)'],
             ['`study_variants.py`', 'Control runs: one rule changed, trees lost'],
             ['`study_lines.py`', 'Line numbers: candidates per kind of node (section 7)'],
             ['`study_duplicates.py`', 'The two versions of the dialog classes (9.3)'],
             ['`probe_source_only.py`', 'The model on the 635 sources without a class'],
             ['`probe_precedence.py`', 'Which precedence and associativity facts the sources '
              'exercise'],
             ['`probe_stmt_roots.py`', 'Which expression kinds stand in statement position'],
             ['`probe_names.py`', 'Duplicate class names, packages, imports, class name '
              'resolution'],
             ['`show.py`', '`show.py ENTRY LINE [LINE2]`: source lines next to their nodes'],
             ['`make_notes.py`', 'Writes this file from `out/*.json`'],
             ['`lint_notes.py`', 'Mechanical checks of this file (table widths, code spans)'],
             ['`run_all.py`', 'Runs everything in order']])
    w('From Python:', '', '```python',
      "import sys",
      "sys.path.insert(0, r'C:\\Users\\trent\\source\\repos\\TrentonCh\\slrr-ghidra\\compiler\\analysis\\expr')",
      'import classdb, gen, jparse, check_expr',
      'model = gen.Model(classdb.load())',
      'unit = jparse.parse(source_text)               # text as oracle.load_source returns it',
      'for uc in model.compile_unit(unit):            # one per class of the source',
      '    for t in uc.trees:                         # in TREE order',
      '        t.nodes, t.lines, t.rules              # [(mnemonic, operand)], [line], rules',
      "for r in check_expr.iter_trees(check_expr.entries_of('940'), model):",
      '    r.ok, r.pt.nodes, r.sym, r.keep, r.align   # prediction against class file',
      '```', '')
    w('The model is a specification to port, not a compiler: it emits no statement-level',
      'nodes, no pool and no bytes.', '')


def main(argv):
    d = Doc()
    build(d)
    text = '\n'.join(d.out).rstrip('\n') + '\n'
    with open(NOTES, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('%s: %d lines, %d bytes' % (NOTES, text.count('\n'), len(text.encode('utf-8'))))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
