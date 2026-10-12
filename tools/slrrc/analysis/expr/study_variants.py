#!/usr/bin/env python3
"""study_variants.py - control experiments: change one rule of the model and
count how many trees it then fails to reproduce.

A rule that the corpus really constrains loses trees when it is replaced by
its alternative; a detail that the corpus does not constrain loses none, and
is then marked as taken from the decompilation of the exe (or as unknown).

    python -I study_variants.py [--json FILE]

Runs over the 2,804 consistent pairs (all builds).  Baseline: 7,722 of 7,724
trees (the two dependency-stale trees of notes/expressions.md).
"""
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for p in (_LIB, _HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import check_expr   # noqa: E402

VARIANTS = [
    ('baseline', {}, 'the model as documented'),
    # which class a field ref names
    ('ref_class=owner', {'ref_class': 'owner'},
     'every field ref names the declaring class'),
    ('ref_class=static', {'ref_class': 'static'},
     'every field ref names the static type of the receiver'),
    # name lookup order
    ('head_order=field,local,class', {'head_order': 'field,local,class'},
     'a field hides a local of the same name'),
    ('head_order=class,local,field', {'head_order': 'class,local,field'},
     'a class name is tried before locals and fields'),
    ('head_order=local,class,field', {'head_order': 'local,class,field'},
     'a class name is tried before fields'),
    # argument forms whose type the compiler would know
    ('typed_extra=path', {'typed_extra': 'path'}, 'a dotted name a.b as argument is typed'),
    ('typed_extra=new', {'typed_extra': 'new'}, 'new T(...) as argument is typed'),
    ('typed_extra=call', {'typed_extra': 'call'}, 'a call as argument is typed'),
    ('typed_extra=unary', {'typed_extra': 'unary'}, 'a unary expression (-1) as argument is typed'),
    ('typed_extra=binary', {'typed_extra': 'binary'}, 'a binary expression as argument is typed'),
    ('typed_extra=index', {'typed_extra': 'index'}, 'a[i] as argument is typed'),
    ('typed_extra=getfield', {'typed_extra': 'getfield'}, 'X.f (computed X) as argument is typed'),
    ('typed_extra=cast', {'typed_extra': 'cast'}, 'a cast as argument is typed'),
    ('typed_extra=assign', {'typed_extra': 'assign'}, 'an assignment as argument is typed'),
    ('typed_extra=postfix', {'typed_extra': 'postfix'}, 'x++ as argument is typed'),
    # details of the method lookup
    ('ifaces=no', {'ifaces': 'no'}, 'an argument does not match a parameter of an interface type it implements'),
    ('prim_to_obj=none', {'prim_to_obj': 'none'}, 'an int or float argument does not match a String parameter'),
    ('prim_to_obj=string_or_super', {'prim_to_obj': 'string_or_super'},
     'an int or float argument also matches an Object parameter'),
    ('array_to_object=no', {'array_to_object': 'no'}, 'an array argument does not match an Object parameter'),
    ('vec_order=static_first', {'vec_order': 'static_first'},
     'the lookup scans the static methods of a class before the instance methods'),
    ('row_order=forward', {'row_order': 'forward'},
     'the lookup scans the methods of a class first to last'),
    ('score_mode=first', {'score_mode': 'first'},
     'the lookup takes the first acceptable method it meets instead of the best score'),
    ('score_mode=first,row_order=forward', {'score_mode': 'first', 'row_order': 'forward'},
     'the same, scanning first to last'),
    ('score_mode=exact_else_first', {'score_mode': 'exact_else_first'},
     'an exact match wins, otherwise the first acceptable method met (no best score)'),
    ('score_mode=exact_else_last', {'score_mode': 'exact_else_last'},
     'an exact match wins, otherwise the last acceptable method met'),
]


def main(argv):
    entries = check_expr.entries_of('940') + check_expr.entries_of('other')
    out = []
    base = None
    for name, opt, what in VARIANTS:
        r = check_expr.run(entries, dict(opt), quiet=True)
        ok = r['counts'].get('trees identical', 0)
        tot = r['counts'].get('trees', 0)
        if base is None:
            base = ok
        out.append({'variant': name, 'what': what, 'trees': tot, 'identical': ok,
                    'lost': base - ok})
        print('%-34s %5d of %5d  lost %5d   %s' % (name, ok, tot, base - ok, what))
        sys.stdout.flush()
    if '--json' in argv:
        with open(argv[argv.index('--json') + 1], 'w', encoding='utf-8') as f:
            json.dump(out, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
