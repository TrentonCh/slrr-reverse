#!/usr/bin/env python3
"""check_layout.py - proof 2: the remaining details of the class layout, each
checked against every blob of the corpus.

    python -I check_layout.py [--corpus DIR] [--json OUT] [--max-list N]

Every rule prints "holds for A of B" and lists the exceptions (up to N per
rule, default 12).  Exit status 0 when every rule marked MUST holds without
exception.  Rules marked INFO are measurements.

Rules (ids are quoted by notes/foundation.md):

  M1  method rows of the first MTHD vector carry ACC_STATIC, rows of the second
      do not
  M2  the descriptor is "(params)ret"; ret is empty exactly for <init>
  M3  tree == 0xFFFFFFFF exactly on the rows with ACC_ABSTRACT
  M4  ACC_NATIVE rows own an empty tree (0 nodes)
  M5  fifth word ("locals"): 0 when the tree is empty or absent, else
      1 + the highest local index used by a 0x1001/0x1002/0x1003 node
      = max(arguments incl. this, 1 + highest index declared by 0x1002)
  M6  no flag bit outside 0xFF (0x80, synchronized, occurs only in classes
      without a source)
  M7  the flags of every field and method row equal the modifier set of a
      declaration of that name in the source (pairs; public 0x1, private 0x4,
      static 0x8, abstract 0x10, final 0x20, native 0x40)
  F1  field rows of the first FILD vector carry ACC_STATIC, the second do not
  F2  the type is a field descriptor (does not start with "(")
  F3  an initialiser tree ends with op 0x10 (end) and op 0x10 occurs nowhere
      else
  T1  every tree is owned by exactly one row (field initialiser or method)
  T2  inside each of the four row vectors the tree indices ascend
  T3  rows that own a tree, ordered by tree index, list fields and methods in
      the order static/instance vectors interleave (INFO: how often the order
      is "static field inits, instance field inits, static methods, instance
      methods")
  T4  trees are numbered in source order: the highest line of a tree is not
      above the lowest line of the next non-empty tree
  T5  the blobs of a file are in source order (same test across blobs)
  C1  CLSS marker is 0xFFFFFFFF, the interface flag is 0 or 1
  C2  pool[0] is the utf8 of the class name, pool[1] its class entry and CLSS
      name == 1
  C3  when CLSS super != 0xFFFFFFFF: pool[2] is the utf8 of the superclass
      name, pool[3] its class entry and CLSS super == 3
  C4  CLSS super == 0xFFFFFFFF only for java.lang.Object and for interfaces
  P1  no pool holds two equal entries (same kind, same payload)
  P2  INFO pool entries that nothing references (a row, CLSS, a node
      immediate of the matching kind, or another entry): the pool keeps
      entries the compiler interned on the way
  P3  an entry refers only to entries with a lower index
  P4  utf8 payloads are 7-bit, without NUL
  P5  rid entries: the pack name ends in ".rpk"
  P6  INFO the pack file of a rid entry exists in the corpus (path relative to
      the install, backslash separated)
  N1  no op byte outside the set that the opcode table marks as emitted
  N2  node immediates that are pool indices name an entry of the kind the
      interpreter expects (0x1006 0x4009 0x4012 0x401C 0x4025 0x4026 utf8,
      0x100E rid, 0x1019 class, 0x101A method ref, 0x101B field ref)
  N3  the u16 after the op byte is a source line: for every pair it lies in
      1..number of lines of the source
      (what stands on that line is checked by check_opcodes.py, "line
      check")
"""
import json
import os
import re
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import disasm   # noqa: E402
import opcodes as oc  # noqa: E402
import oracle   # noqa: E402
import tufa     # noqa: E402

NONE = tufa.NONE
EMITTED = set(o.raw for o in oc.OPS if o.role != 'unused')
EXPECT = {0x1006: 'utf8', 0x4009: 'utf8', 0x4012: 'utf8', 0x401c: 'utf8',
          0x4025: 'utf8', 0x4026: 'utf8', 0x100e: 'rid', 0x1019: 'class',
          0x101a: 'ref', 0x101b: 'ref'}


class Rule(object):
    def __init__(self, rid, text, must=True):
        self.rid, self.text, self.must = rid, text, must
        self.ok = 0
        self.total = 0
        self.bad = []

    def check(self, cond, where=None):
        self.total += 1
        if cond:
            self.ok += 1
        elif where is not None and len(self.bad) < 2000:
            self.bad.append(where() if callable(where) else where)
        return cond


def escape_text(s):
    """Source spelling candidates of a pool string inside double quotes."""
    out = []
    for ch in s:
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\t':
            out.append('\\t')
        elif ch == '\r':
            out.append('\\r')
        else:
            out.append(ch)
    return '"' + ''.join(out) + '"'


MODS = {'public': 0x1, 'protected': 0x2, 'private': 0x4, 'static': 0x8,
        'abstract': 0x10, 'final': 0x20, 'native': 0x40, 'synchronized': 0x80}
_MODRE = r'((?:\b(?:public|protected|private|static|abstract|final|native|synchronized)\s+)*)'
_TYPERE = r'[A-Za-z_][\w.]*(?:\s*\[\s*\])*'
_NOT_TYPES = frozenset(('return', 'new', 'else', 'case', 'instanceof', 'import',
                        'package', 'extends', 'implements', 'class', 'interface'))


def _flagset(mods):
    fl = 0
    for w in mods.split():
        fl |= MODS[w]
    return fl


def field_decl_flags(clean, name):
    """Modifier sets of the declarations of a variable called name."""
    out = set()
    pat = _MODRE + '(' + _TYPERE + r')\s+(?:[A-Za-z_]\w*\s*(?:\[\s*\])*\s*' \
        r'(?:=[^;,]*)?,\s*)*' + re.escape(name) + r'\s*(?:\[\s*\])*\s*(?==|;|,)'
    for m in re.finditer(pat, clean):
        if m.group(2) in _NOT_TYPES:
            continue
        out.add(_flagset(m.group(1)))
    return out


def method_decl_flags(clean, name, class_names):
    """Modifier sets of the declarations of a method called name."""
    out = set()
    if name == '<init>':
        for cn in class_names:
            for m in re.finditer(_MODRE + r'(?<![\w.])' + re.escape(cn) +
                                 r'\s*\([^()]*\)\s*[{;]', clean):
                out.add(_flagset(m.group(1)))
        return out
    pat = _MODRE + '(' + _TYPERE + r')\s+' + re.escape(name) + r'\s*\([^()]*\)\s*[{;]'
    for m in re.finditer(pat, clean):
        if m.group(2) in _NOT_TYPES:
            continue
        out.add(_flagset(m.group(1)))
    return out


def main(argv):
    out_json = None
    max_list = 12
    if '--corpus' in argv:
        os.environ['SLRR_CORPUS'] = argv[argv.index('--corpus') + 1]
    if '--json' in argv:
        out_json = argv[argv.index('--json') + 1]
    if '--max-list' in argv:
        max_list = int(argv[argv.index('--max-list') + 1])

    R = {}

    def rule(rid, text, must=True):
        R[rid] = Rule(rid, text, must)
        return R[rid]

    M1 = rule('M1', 'method ACC_STATIC bit matches the vector')
    M2 = rule('M2', 'descriptor "(params)ret", ret empty exactly for <init>')
    M3 = rule('M3', 'tree absent exactly on ACC_ABSTRACT rows')
    M4 = rule('M4', 'ACC_NATIVE rows own an empty tree')
    M5a = rule('M5a', 'locals word is 0 when the tree is empty or absent')
    M5b = rule('M5b', 'locals word = 1 + highest local index used in the tree')
    M5c = rule('M5c', 'locals word = max(args incl. this, 1 + highest declared index)')
    M6 = rule('M6', 'no method/field flag bit outside 0xFF')
    M7f = rule('M7f', 'field flags equal the modifiers of a source declaration (pairs)')
    M7m = rule('M7m', 'method flags equal the modifiers of a source declaration (pairs)')
    F1 = rule('F1', 'field ACC_STATIC bit matches the vector')
    F2 = rule('F2', 'field type is a field descriptor')
    F3a = rule('F3a', 'initialiser trees end with op 0x10 (end)')
    F3b = rule('F3b', 'op 0x10 occurs only as the last node of an initialiser tree')
    T1 = rule('T1', 'every tree is owned by exactly one row')
    T2 = rule('T2', 'tree indices ascend inside each row vector')
    T3 = rule('T3', 'tree order is static inits, instance inits, static methods, '
              'instance methods', must=False)
    T4 = rule('T4', 'trees are numbered in source order (line spans do not overlap '
              'and ascend)')
    T5 = rule('T5', 'blobs of a file are in source order (line spans ascend)')
    C1 = rule('C1', 'CLSS marker 0xFFFFFFFF and interface flag 0/1')
    C2 = rule('C2', 'pool[0] = class name utf8, pool[1] = its class entry = CLSS name')
    C3 = rule('C3', 'pool[2] = super name utf8, pool[3] = its class entry = CLSS super')
    C4 = rule('C4', 'no superclass only for java.lang.Object and interfaces')
    P1 = rule('P1', 'no duplicate pool entries')
    P2 = rule('P2', 'every pool entry is referenced', must=False)
    P3 = rule('P3', 'pool entries refer only to lower indices')
    P4 = rule('P4', 'utf8 payloads are 7-bit without NUL')
    P5 = rule('P5', 'rid pack names end in .rpk')
    P6 = rule('P6', 'the pack file of a rid entry exists in the corpus', must=False)
    N1 = rule('N1', 'op bytes are in the emitted set of the opcode table')
    N2 = rule('N2', 'pool-index immediates name an entry of the expected kind')
    N3 = rule('N3', 'node line is within 1..source lines (pairs)')
    N3b = rule('N3b', 'node line is >= 1 (all classes)')

    hist = defaultdict(Counter)
    pack_exists = {}
    idx = {e.cls: e for e in oracle.load_index() if e.cls}
    nblobs = 0
    for rel, blobs in oracle.iter_class_files():
        e = idx.get(rel)
        src_lines = None
        clean = None
        class_names = ()
        if e is not None and e.source:
            text = oracle.load_source(e)
            src_lines = text.split('\r\n')
            if src_lines and src_lines[-1] == '':
                src_lines.pop()
            clean = oracle.strip_comments_and_strings(text)
            class_names = [d['name'] for d in e.declared]
        prev_blob_hi = None
        for bi, b in enumerate(blobs):
            nblobs += 1
            where = '%s#%d' % (rel, bi)
            spans = [(ti, min(n.slot for n in t), max(n.slot for n in t))
                     for ti, t in enumerate(b.trees) if t]
            for q in range(len(spans) - 1):
                T4.check(spans[q][2] <= spans[q + 1][1],
                         lambda: '%s tree %d lines %d-%d, tree %d lines %d-%d' % (
                             (where,) + spans[q] + spans[q + 1]))
            if spans:
                if prev_blob_hi is not None:
                    T5.check(prev_blob_hi <= spans[0][1], where)
                prev_blob_hi = spans[-1][2]
            hist['build'][b.build] += 1
            cons = b.cons
            kinds = [x[0] for x in cons]
            nt = len(b.trees)
            codes = [[tufa.OP_FLAGS[n.op] for n in t] for t in b.trees]
            # ---- methods
            owners = defaultdict(list)
            for st, i, m in b.methods():
                name = b.utf8(m.name)
                desc = b.utf8(m.desc)
                w = lambda: '%s %s%s flags=0x%X tree=%s locals=%d' % (
                    where, name, desc, m.flags,
                    '-' if m.tree == NONE else m.tree, m.locals)
                hist['method_flags'][m.flags] += 1
                M1.check(bool(m.flags & 8) == st, w)
                M6.check(not (m.flags & ~0xFF), w)
                if m.flags & 0x80:
                    hist['flag_0x80_category'][e.category if e else '?'] += 1
                if clean is not None:
                    M7m.check(m.flags in method_decl_flags(clean, name, class_names), w)
                try:
                    params, ret = disasm.split_descriptor(desc)
                    good = True
                except ValueError:
                    params, ret, good = [], '', False
                M2.check(good and ((ret == '') == (name == '<init>')), w)
                hist['return_kind'][ret[:1] or 'ctor'] += 1
                M3.check((m.tree == NONE) == bool(m.flags & 0x10), w)
                if m.tree == NONE:
                    hist['method_body']['absent'] += 1
                    M5a.check(m.locals == 0, w)
                    continue
                owners[m.tree].append(('M', st, i))
                if m.tree >= nt:
                    continue
                t = b.trees[m.tree]
                if m.flags & 0x40:
                    M4.check(len(t) == 0, w)
                base = len(params) + (0 if st else 1)
                if not t:
                    hist['method_body']['empty native' if m.flags & 0x40
                                        else 'empty'] += 1
                    M5a.check(m.locals == 0, w)
                    continue
                hist['method_body']['code'] += 1
                used = [n.imm for n, c in zip(t, codes[m.tree])
                        if c in (0x1001, 0x1002, 0x1003)]
                decl = [n.imm for n, c in zip(t, codes[m.tree]) if c == 0x1002]
                M5b.check(m.locals == (max(used) + 1 if used else 0), w)
                M5c.check(m.locals == max([base] + [d + 1 for d in decl]), w)
                hist['locals_minus_args'][m.locals - base] += 1
            # ---- fields
            for st, i, f in b.fields():
                name = b.utf8(f.name)
                typ = b.utf8(f.type)
                w = lambda: '%s field %s:%s flags=0x%X' % (where, name, typ, f.flags)
                hist['field_flags_%s' % ('static' if st else 'instance')][f.flags] += 1
                F1.check(bool(f.flags & 8) == st, w)
                M6.check(not (f.flags & ~0xFF), w)
                if f.flags & 0x02:
                    hist['flag_0x02_category'][e.category if e else '?'] += 1
                if clean is not None:
                    M7f.check(f.flags in field_decl_flags(clean, name), w)
                F2.check(typ != '' and not typ.startswith('('), w)
                if f.init != NONE:
                    owners[f.init].append(('F', st, i))
                    if f.init < nt:
                        t = codes[f.init]
                        F3a.check(bool(t) and t[-1] == 0x10, w)
            # ---- trees
            for ti in range(nt):
                T1.check(len(owners.get(ti, ())) == 1,
                         lambda: '%s tree %d owners %r' % (where, ti, owners.get(ti)))
                c = codes[ti]
                is_init = bool(owners.get(ti)) and owners[ti][0][0] == 'F'
                for k, x in enumerate(c):
                    if x == 0x10:
                        F3b.check(is_init and k == len(c) - 1,
                                  lambda: '%s tree %d node %d' % (where, ti, k))
            for label, rows, attr in (('static fields', b.static_fields, 'init'),
                                      ('instance fields', b.instance_fields, 'init'),
                                      ('static methods', b.static_methods, 'tree'),
                                      ('instance methods', b.instance_methods, 'tree')):
                seq = [getattr(r, attr) for r in rows if getattr(r, attr) != NONE]
                T2.check(seq == sorted(seq) and len(set(seq)) == len(seq),
                         lambda: '%s %s %r' % (where, label, seq))
            seq = [owners[t][0] for t in range(nt) if owners.get(t)]
            want = [('F', True, i) for i, f in enumerate(b.static_fields) if f.init != NONE] + \
                   [('F', False, i) for i, f in enumerate(b.instance_fields) if f.init != NONE] + \
                   [('M', True, i) for i, m in enumerate(b.static_methods) if m.tree != NONE] + \
                   [('M', False, i) for i, m in enumerate(b.instance_methods) if m.tree != NONE]
            T3.check(seq == want, where)
            # ---- CLSS
            c = b.clss
            C1.check(c.marker == NONE and c.interface in (0, 1), where)
            hist['interface_count'][len(c.interfaces)] += 1
            hist['is_interface'][c.interface] += 1
            C2.check(len(cons) >= 2 and cons[0][0] == 'utf8' and cons[1] == ('class', 0)
                     and c.name == 1, where)
            if c.super != NONE:
                C3.check(len(cons) >= 4 and cons[2][0] == 'utf8' and
                         cons[3] == ('class', 2) and c.super == 3, where)
            else:
                C4.check(b.name == 'java.lang.Object' or c.interface == 1,
                         lambda: '%s %s' % (where, b.name))
            # ---- pool
            seen = {}
            refd = [False] * len(cons)

            def mark(i):
                if 0 <= i < len(refd):
                    refd[i] = True

            for i, x in enumerate(cons):
                hist['cons_kind'][x[0]] += 1
                P1.check(x not in seen, lambda: '%s pool[%d] == pool[%d] %r' % (
                    where, i, seen.get(x), x))
                seen.setdefault(x, i)
                if x[0] == 'utf8':
                    P4.check(all(0 < ord(ch) < 128 for ch in x[1]),
                             lambda: '%s pool[%d] %s' % (where, i, tufa.repr_text(x[1])))
                    if any(ord(ch) < 32 for ch in x[1]):
                        hist['utf8_control_chars'][','.join(sorted(set(
                            '0x%02X' % ord(ch) for ch in x[1] if ord(ch) < 32)))] += 1
                    if x[1] == '':
                        hist['utf8_empty']['empty'] += 1
                elif x[0] == 'class':
                    mark(x[1])
                    P3.check(x[1] < i, lambda: '%s pool[%d] %r' % (where, i, x))
                else:
                    mark(x[1])
                    if x[0] != 'rid':
                        mark(x[2])
                        P3.check(x[1] < i and x[2] < i,
                                 lambda: '%s pool[%d] %r' % (where, i, x))
                    else:
                        P3.check(x[1] < i, lambda: '%s pool[%d] %r' % (where, i, x))
                        if kinds[x[1]] == 'utf8':
                            P5.check(cons[x[1]][1].lower().endswith('.rpk'),
                                     lambda: '%s pool[%d] %s' % (where, i, b.describe(i)))
                            pk = cons[x[1]][1]
                            if pk not in pack_exists:
                                pack_exists[pk] = os.path.isfile(os.path.join(
                                    oracle.corpus_root(), pk.replace('\\', os.sep)))
                            P6.check(pack_exists[pk],
                                     lambda: '%s pool[%d] %s' % (where, i, b.describe(i)))
                            hist['rid_pack_depth'][pk.count('\\') + 1] += 1
                        hist['rid_local_bits'][x[2].bit_length()] += 1
            for st, i, f in b.fields():
                mark(f.name)
                mark(f.type)
            for st, i, m in b.methods():
                mark(m.name)
                mark(m.desc)
            mark(c.name)
            if c.super != NONE:
                mark(c.super)
            for j in c.interfaces:
                mark(j)
            # ---- nodes
            nlines = len(src_lines) if src_lines is not None else None
            for ti, t in enumerate(b.trees):
                cs = codes[ti]
                for k, n in enumerate(t):
                    code = cs[k]
                    hist['op'][code] += 1
                    N1.check(n.op in EMITTED,
                             lambda: '%s tree %d node %d op %d' % (where, ti, k, n.op))
                    N3b.check(n.slot >= 1,
                              lambda: '%s tree %d node %d line %d' % (where, ti, k, n.slot))
                    if nlines is not None:
                        N3.check(1 <= n.slot <= nlines,
                                 lambda: '%s tree %d node %d line %d of %d' % (
                                     where, ti, k, n.slot, nlines))
                    want_kind = EXPECT.get(code)
                    if want_kind is not None:
                        good = 0 <= n.imm < len(cons) and kinds[n.imm] == want_kind
                        if good and code in (0x101a, 0x101b):
                            d = b.ref(n.imm)[2]
                            good = d.startswith('(') == (code == 0x101a)
                        N2.check(good, lambda: '%s tree %d node %d op 0x%04X imm %d' % (
                            where, ti, k, code, n.imm))
                        mark(n.imm)
            for i, r in enumerate(refd):
                P2.check(r, lambda: '%s pool[%d] %s %s' % (where, i, kinds[i],
                                                           b.describe(i)))
                if not r:
                    hist['unreferenced_pool_entries'][kinds[i]] += 1
                    if kinds[i] == 'utf8':
                        t = cons[i][1]
                        hist['unreferenced_utf8_shape'][
                            'descriptor (...)' if t.startswith('(') else
                            'type L...;' if t.startswith('L') and t.endswith(';') else
                            'array type' if t.startswith('[') else
                            'primitive type' if t in ('I', 'F', 'V', 'Z') else
                            'other'] += 1
            hist['blobs_with_unreferenced_entries'][
                'some' if not all(refd) else 'none'] += 1

    # ---- report
    print('corpus  %s' % oracle.corpus_root())
    print('blobs   %d' % nblobs)
    print('')
    failed = []
    for rid in R:
        r = R[rid]
        status = 'holds' if r.ok == r.total else ('FAILS' if r.must else 'info')
        print('%-4s %-5s %d of %d  %s' % (rid, status, r.ok, r.total, r.text))
        for w in r.bad[:max_list]:
            print('       - %s' % (w,))
        if len(r.bad) > max_list:
            print('       ... %d more' % (len(r.bad) - max_list))
        if r.must and r.ok != r.total:
            failed.append(rid)
    print('')
    for name in sorted(hist):
        h = hist[name]
        print('%s:' % name)
        for k, v in sorted(h.items(), key=lambda kv: (str(type(kv[0])), kv[0])):
            ks = ('0x%X' % k) if isinstance(k, int) and name in (
                'op', 'method_flags', 'build', 'field_flags_static',
                'field_flags_instance') else str(k)
            extra = ''
            if name in ('method_flags', 'field_flags_static', 'field_flags_instance'):
                extra = '  ' + tufa.flags_text(k)
            print('  %-22s %d%s' % (ks, v, extra))
    if out_json:
        doc = {'blobs': nblobs,
               'rules': {rid: {'text': r.text, 'must': r.must, 'ok': r.ok,
                               'total': r.total, 'exceptions': r.bad[:200]}
                         for rid, r in R.items()},
               'hist': {name: {str(k): v for k, v in h.items()}
                        for name, h in hist.items()}}
        with open(out_json, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(doc, f, indent=1)
    print('')
    print('FAILED: %s' % ', '.join(failed) if failed else 'all MUST rules hold')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
