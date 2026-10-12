#!/usr/bin/env python3
"""expected_totals.py - what the tree and whole-file milestones can reach, given
what the four analyses proved and what is still open.

    python -I expected_totals.py

Every oracle pair is byte-identical when (1) its declarations and pool follow
the model (proven for all), (2) every tree is reproduced node for node
(proven for all but 2 trees, in 2 pairs compiled against another version of a
class they call), (3) every float node has the bits the compiler's conversion
gives and (4) every pack name in a rid entry is spelled as the compiler spells
it.  (3) and (4) are the open points; this script counts the trees and pairs
each choice loses.

Float settings: A div pc24, B mul pc24, C div pc53, D diva pc53 (see
float_models.py).  Pack name rules: 'written' (as in the source), 'disk' (as
the file is spelled in the install), 'table' (the one spelling each pack has
in all shipped pools; a pack without a file gives system.rpk id 0).
"""
import os
import pickle
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', 'lib'))
if _LIB not in sys.path:
    sys.path.insert(0, _LIB)

import lexer    # noqa: E402
import oracle   # noqa: E402
import tufa     # noqa: E402

DEP_TREES = {('parts/scripts/enginepart/slidingenginepart/reciprocatingenginepart/'
              'differential/Center_Differential.class', 0, 2),
             ('sl/Scripts/game/Garage.class', 0, 99)}
SETS = (('A', 'div pc24 v24 s24 c24'), ('B', 'mul pc24 v24 s24 c24'),
        ('C', 'div pc53 v24 s24 c24'), ('D', 'diva pc53 v53 s53 c24'))


def disk_packs(root):
    out = {}
    for dp, dn, fn in os.walk(root):
        dn[:] = [d for d in dn if not d.lower().startswith('tree_rpk_scan_boot')]
        for f in fn:
            if f.lower().endswith('.rpk'):
                rel = os.path.relpath(os.path.join(dp, f), root).replace('/', '\\')
                out[rel.lower()] = rel
    return out


def main(argv):
    with open(os.path.join(_HERE, 'out', 'float_res_oracle.pickle'), 'rb') as f:
        names, res = pickle.load(f)
    idx = {n: k for k, n in enumerate(names)}
    root = oracle.corpus_root()
    disk = disk_packs(root)
    # the table: one spelling per pack over all shipped pools
    table = {}
    for e in oracle.iter_entries():
        if not e.cls:
            continue
        for b in oracle.load_class(e):
            for ent in b.cons:
                if ent[0] == 'rid':
                    p = b.utf8(ent[1])
                    table.setdefault(p.lower(), p)
    trees = 0
    pairs = 0
    tree_bad = defaultdict(set)       # reason -> set of (cls, blob, tree)
    pair_bad = defaultdict(set)
    float_trees = rid_trees = 0
    for e in oracle.iter_oracle_pairs():
        pairs += 1
        text = oracle.load_source(e)
        fl = defaultdict(list)
        rd = defaultdict(list)
        for t in lexer.tokenize(text):
            if t.kind == 'float':
                fl[t.line].append(t.text)
            elif t.kind == 'rid':
                rd[t.line].append(lexer.rid_parts(t.text))
        for bi, b in enumerate(oracle.load_class(e)):
            for ti, tree in enumerate(b.trees):
                trees += 1
                key = (e.cls, bi, ti)
                if key in DEP_TREES:
                    tree_bad['dep'].add(key)
                    pair_bad['dep'].add(e.cls)
                used = defaultdict(list)
                has_f = has_r = False
                for nd in tree:
                    code = tufa.OP_FLAGS[nd.op]
                    if code == 0x200a:
                        has_f = True
                        free = [t for t in fl.get(nd.slot, ())]
                        for u in used[nd.slot]:
                            if u in free:
                                free.remove(u)
                        # the token whose results under some setting equal the node
                        best = None
                        for tk in free:
                            if tk in res and any(res[tk][idx[nm]] == nd.imm for _, nm in SETS):
                                best = tk
                                break
                        if best is None:
                            for ch, _ in SETS:
                                tree_bad['float ' + ch].add(key)
                                pair_bad['float ' + ch].add(e.cls)
                            continue
                        used[nd.slot].append(best)
                        for ch, nm in SETS:
                            if res[best][idx[nm]] != nd.imm:
                                tree_bad['float ' + ch].add(key)
                                pair_bad['float ' + ch].add(e.cls)
                    elif code == 0x100e:
                        has_r = True
                        ptxt, lid = b.rid(nd.imm)
                        cand = [(p, i) for p, i in rd.get(nd.slot, ())]
                        ok = {'written': False, 'disk': False, 'table': False}
                        for p, i in cand:
                            w = p.replace('.', '\\') + '.rpk'
                            d = disk.get(w.lower())
                            if i == lid and ptxt == w:
                                ok['written'] = True
                            if i == lid and ptxt == (d or w):
                                ok['disk'] = True
                            if d is None and w.lower() not in table:
                                tb, tid = 'system.rpk', 0
                            elif d is None:
                                tb, tid = ('system.rpk', 0) if e.category == 'pair_940' else (table[w.lower()], i)
                            else:
                                tb, tid = table.get(w.lower(), d), i
                            if ptxt == tb and lid == tid:
                                ok['table'] = True
                        for k, v in ok.items():
                            if not v:
                                tree_bad['rid ' + k].add(key)
                                pair_bad['rid ' + k].add(e.cls)
                float_trees += has_f
                rid_trees += has_r
    print('oracle pairs %d, trees %d (with a float node %d, with a rid node %d)' % (
        pairs, trees, float_trees, rid_trees))
    print()
    print('%-44s %8s %8s' % ('lost to', 'trees', 'pairs'))
    for k in ('dep', 'float A', 'float B', 'float C', 'float D', 'rid written', 'rid disk', 'rid table'):
        print('%-44s %8d %8d' % (k, len(tree_bad[k]), len(pair_bad[k])))
    print()

    def reach(fkey, rkey):
        tb = tree_bad['dep'] | tree_bad[rkey] | (tree_bad[fkey] if fkey else set())
        pb = pair_bad['dep'] | pair_bad[rkey] | (pair_bad[fkey] if fkey else set())
        return trees - len(tb), pairs - len(pb)
    print('%-60s %14s %14s' % ('configuration', 'trees exact', 'pairs identical'))
    for label, fkey, rkey in (
            ('float A, pack names as written', 'float A', 'rid written'),
            ('float A, pack names as on disk', 'float A', 'rid disk'),
            ('float A, pack name table', 'float A', 'rid table'),
            ('float B, pack name table', 'float B', 'rid table'),
            ('float C, pack name table', 'float C', 'rid table')):
        t, p = reach(fkey, rkey)
        print('%-60s %6d of %d %6d of %d' % (label, t, trees, p, pairs))
    # best setting per FILE
    best_pairs = pairs - len(pair_bad['dep'] | pair_bad['rid table'])
    per = set()
    allbad = None
    for ch, _ in SETS:
        s = pair_bad['float ' + ch]
        allbad = s if allbad is None else (allbad & s)
    print('%-60s %6s    %6d of %d' % ('float setting chosen per file (A, B, C or D), table',
                                      '', pairs - len(pair_bad['dep'] | pair_bad['rid table'] | allbad), pairs))
    tb = None
    # per file choice, trees: a tree is lost when its file's best setting loses it
    lost_trees = set(tree_bad['dep'] | tree_bad['rid table'])
    by_file = defaultdict(lambda: defaultdict(set))
    for ch, _ in SETS:
        for key in tree_bad['float ' + ch]:
            by_file[key[0]][ch].add(key)
    for f, d in by_file.items():
        choice = min((len(d.get(ch, ())), ch) for ch, _ in SETS)
        lost_trees |= d.get(choice[1], set())
    print('%-60s %6d of %d' % ('  trees under that choice', trees - len(lost_trees), trees))
    print('pairs no float setting reproduces: %d' % len(allbad))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
