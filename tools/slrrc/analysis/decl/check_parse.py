#!/usr/bin/env python3
"""check_parse.py - does srcparse.py accept every source of the corpus, and do
its declarations agree with the class files?

Checks, over all sources:
  P1  every source parses
  P2  for every pair: the classes declared equal the blobs (count and order)
  P3  for every pair: number of declared fields == FILD rows, declared
      methods + constructors == MTHD rows (per class)
  P4  census of syntax forms the model has to know about
"""
import os
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'lib')))
sys.path.insert(0, HERE)

import oracle     # noqa: E402
import srcparse   # noqa: E402


def main():
    fails = []
    forms = Counter()
    nsrc = 0
    p2 = [0, 0]
    p3f = [0, 0]
    p3m = [0, 0]
    bad3 = []
    for e in oracle.iter_entries():
        if e.source is None:
            continue
        nsrc += 1
        text = oracle.load_source(e)
        try:
            u = srcparse.parse(text)
        except (srcparse.ParseError, srcparse.srclex.LexError) as x:
            fails.append((e.source, str(x)))
            continue
        forms['stray ; at top level'] += u.stray
        forms['imports'] += len(u.imports)
        forms['imports .*'] += sum(1 for i in u.imports if i.star)
        forms['imports single'] += sum(1 for i in u.imports if not i.star)
        if not u.imports:
            forms['sources without import'] += 1
        for c in u.classes:
            forms['classes'] += 1
            forms['class kind ' + c.kind] += 1
            forms['class mods ' + ' '.join(c.mods)] += 1
            forms['stray ; in class body'] += c.stray
            if c.extends is None:
                forms['class without extends'] += 1
            elif '.' in c.extends:
                forms['extends qualified name'] += 1
            for m in c.members:
                forms['member ' + m.k] += 1
                if m.k == 'field':
                    if m.dims:
                        forms['field with [] after the name'] += 1
                    if '.' in m.type.base:
                        forms['field type qualified'] += 1
                    if m.type.dims > 1:
                        forms['field type with 2+ dims'] += 1
                else:
                    forms['%s body %s' % (m.k, 'block' if m.body else ';')] += 1
                    for p in m.params:
                        if p.dims:
                            forms['param with [] after the name'] += 1
                        if p.final:
                            forms['final param'] += 1
                    if m.ret is not None and m.ret.dims:
                        forms['array return type'] += 1
                for x in srcparse.walk(m):
                    if x.k == 'local':
                        forms['local declarations'] += 1
                        if x.final:
                            forms['final local'] += 1
                        if len(x.decls) > 1:
                            forms['local with several declarators'] += 1
                        for d in x.decls:
                            if d.dims:
                                forms['local with [] after the name'] += 1
                    elif x.k == 'cast':
                        forms['cast'] += 1
                        forms['cast to ' + ('primitive' if x.type.base in ('int', 'float') else 'class') + ('[]' if x.type.dims else '')] += 1
                    elif x.k == 'newarray':
                        forms['new array'] += 1
                        if x.type.dims > 1:
                            forms['new array 2+ dims'] += 1
                    elif x.k == 'type' and '.' in x.base:
                        forms['qualified type name'] += 1
        if e.is_pair:
            pre = (u.package + '.') if u.package else ''
            names = [pre + c.name for c in u.classes]
            p2[1] += 1
            if names == e.blob_names:
                p2[0] += 1
                blobs = oracle.load_class(e)
                for c, b in zip(u.classes, blobs):
                    nf = sum(1 for m in c.members if m.k == 'field')
                    nm = sum(1 for m in c.members if m.k != 'field')
                    p3f[1] += 1
                    p3m[1] += 1
                    okf = nf == len(b.static_fields) + len(b.instance_fields)
                    okm = nm == len(b.static_methods) + len(b.instance_methods)
                    p3f[0] += okf
                    p3m[0] += okm
                    if not (okf and okm):
                        bad3.append((e.key, c.name, nf, len(b.static_fields) + len(b.instance_fields), nm, len(b.static_methods) + len(b.instance_methods), e.consistent, e.builds))
    print('P1 sources that parse: %d of %d' % (nsrc - len(fails), nsrc))
    for f in fails[:40]:
        print('   FAIL', f)
    print('P2 pairs whose declared classes equal the blobs: %d of %d' % tuple(p2))
    print('P3 classes whose field count equals the FILD rows: %d of %d' % tuple(p3f))
    print('P3 classes whose method count equals the MTHD rows: %d of %d' % tuple(p3m))
    for b in bad3[:40]:
        print('   DIFF', b)
    print('P4 forms:')
    for k in sorted(forms):
        print('   %-40s %d' % (k, forms[k]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
