#!/usr/bin/env python3
"""rid_packs.py - how the compiler spells the pack file name of a resource id.

    python -I rid_packs.py [--json OUT]

A literal  cars.racers.baiern:0x0000002Ar  becomes a pool rid entry
{utf8 pack file name, local id}.  foundation.md question 3: the utf8 is not
always the source spelling.  This script collects, for every rid node of every
consistent pair, the spelling in the source, the spelling in the pool and the
spelling of the .rpk file on disk, and tests candidate rules.

Rules tested (pool text = ...):
    written    the source name, dots to backslashes, + ".rpk"
    disk       the path of the .rpk file as the directory listing spells it
               (found case-insensitively); when there is no such file: written
    disk/sys   as disk, but a pack without a file becomes "system.rpk" id 0
"""
import json
import os
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', 'lib'))
if _LIB not in sys.path:
    sys.path.insert(0, _LIB)

import lexer    # noqa: E402
import oracle   # noqa: E402
import tufa     # noqa: E402


def disk_packs(root):
    """lower-case relative path with backslashes -> spelling on disk."""
    out = {}
    for dp, dn, fn in os.walk(root):
        dn[:] = [d for d in dn if not d.lower().startswith('tree_rpk_scan_boot')]
        for f in fn:
            if f.lower().endswith('.rpk'):
                rel = os.path.relpath(os.path.join(dp, f), root).replace('/', '\\')
                out[rel.lower()] = rel
    return out


def written_name(pack):
    return pack.replace('.', '\\') + '.rpk'


def main(argv):
    out_json = argv[argv.index('--json') + 1] if '--json' in argv else None
    root = oracle.corpus_root()
    disk = disk_packs(root)
    print('.rpk files on disk: %d' % len(disk))
    rules = Counter()
    nodes = 0
    unmatched = 0
    pair_fail = defaultdict(set)          # rule -> pairs with a failing node
    pairs_with_rid = set()
    spell = Counter()                     # (written, pool, disk) -> nodes
    ext_case = Counter()
    for e in oracle.iter_entries():
        if e.category not in ('pair_940', 'pair_other') or not e.consistent:
            continue
        text = oracle.load_source(e)
        by = defaultdict(list)
        for t in lexer.tokenize(text):
            if t.kind == 'rid':
                pack, lid = lexer.rid_parts(t.text)
                by[t.line].append((pack, lid))
        for b in oracle.load_class(e):
            for tree in b.trees:
                for nd in tree:
                    if tufa.OP_FLAGS[nd.op] != 0x100e:
                        continue
                    nodes += 1
                    pairs_with_rid.add(e.cls)
                    ptxt, lid = b.rid(nd.imm)
                    cand = [(p, i) for p, i in by.get(nd.slot, ()) if i == lid]
                    if not cand:
                        cand = by.get(nd.slot, ())
                    if not cand:
                        unmatched += 1
                        continue
                    # prefer the token whose written name equals the pool text without case
                    best = None
                    for p, i in cand:
                        if written_name(p).lower() == ptxt.lower() and i == lid:
                            best = (p, i)
                            break
                    if best is None:
                        best = cand[0]
                    p, i = best
                    w = written_name(p)
                    d = disk.get(w.lower())
                    spell[(w, ptxt, d or '-')] += 1
                    ok_w = (ptxt == w and i == lid)
                    ok_d = (ptxt == (d or w) and i == lid)
                    ok_s = ((ptxt == d and i == lid) if d else (ptxt == 'system.rpk' and lid == 0))
                    for name, ok in (('written', ok_w), ('disk', ok_d), ('disk/sys', ok_s)):
                        rules[name] += ok
                        if not ok:
                            pair_fail[name].add(e.cls)
                    ext_case[ptxt[-4:]] += 1
    print('rid nodes in consistent pairs: %d (not matched to a literal on their line: %d)'
          % (nodes, unmatched))
    print('pairs with a rid node: %d' % len(pairs_with_rid))
    for name in ('written', 'disk', 'disk/sys'):
        print('  rule %-9s holds for %d of %d nodes; pairs with a failing node: %d'
              % (name, rules[name], nodes - unmatched, len(pair_fail[name])))
    print('extension as stored: %s' % dict(ext_case))
    print()
    print('spellings where the pool differs from the written name or from the disk name:')
    odd = [(k, v) for k, v in spell.items() if not (k[0] == k[1] == k[2])]
    for (w, ptxt, d), v in sorted(odd, key=lambda kv: -kv[1]):
        tag = []
        if ptxt == d:
            tag.append('pool=disk')
        if ptxt == w:
            tag.append('pool=written')
        if not tag:
            tag.append('pool differs from both')
        print('  %5d  written %-52s pool %-52s disk %-52s %s' % (v, w, ptxt, d, ', '.join(tag)))
    print()
    print('pairs failing rule disk: %s' % sorted(pair_fail['disk'])[:20])
    print('pairs failing rule disk/sys: %s' % sorted(pair_fail['disk/sys'])[:20])
    # the sources without a class
    so = Counter()
    so_files = defaultdict(set)
    for e in oracle.iter_entries('source_only'):
        text = oracle.load_source(e)
        for t in lexer.tokenize(text):
            if t.kind != 'rid':
                continue
            pack, lid = lexer.rid_parts(t.text)
            w = written_name(pack)
            d = disk.get(w.lower())
            so['literals'] += 1
            so_files['any'].add(e.source)
            if d is None:
                so['pack without a file'] += 1
                so_files['nofile'].add(e.source)
            elif d != w:
                so['written in another letter case than the file'] += 1
                so_files['case'].add(e.source)
            else:
                so['written exactly like the file'] += 1
    print()
    print('sources without a class: rid literals %d in %d sources' % (so['literals'], len(so_files['any'])))
    for k in ('written exactly like the file', 'written in another letter case than the file',
              'pack without a file'):
        print('  %-48s %d' % (k, so[k]))
    print('  sources with a literal in another letter case: %d; with a pack without a file: %d'
          % (len(so_files['case']), len(so_files['nofile'])))
    if out_json:
        doc = {'nodes': nodes, 'unmatched': unmatched, 'pairs_with_rid': len(pairs_with_rid),
               'rules': dict(rules),
               'pairs_failing': {k: sorted(v) for k, v in pair_fail.items()},
               'spellings': [[w, p, d, v] for (w, p, d), v in sorted(spell.items())],
               'source_only': dict(so),
               'source_only_files': {k: len(v) for k, v in so_files.items()}}
        with open(out_json, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(doc, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
