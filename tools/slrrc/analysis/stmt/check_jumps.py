#!/usr/bin/env python3
"""check_jumps.py - the jump encoding, checked on every jump of the corpus.

Four opcodes carry a jump: 0x1014 jump, 0x1015 jump_false, 0x1016 jump_true,
0x1017 case.  The rule under test:

    the immediate is a signed 32-bit count of NODES relative to the jump node
    itself: target index = index of the jump + imm

Checks, over all 2,993 class blobs (9,772 trees):

  J1  the target lies inside the tree: 0 <= target <= node count
  J2  the target is not a node that another node consumes (a literal / type /
      class carrier, the `init` marker, a path header behind `call`, a path
      segment): it is a node the interpreter dispatches
  J3  no jump has offset 0 (a jump to itself) and none targets itself + 1 by
      accident of a rival reading (see the rivals below)
  J4  in every tree that has a source, the model compiler (stmtgen.py)
      predicts the exact offset of every jump from the statement it belongs
      to; the role table lists them by statement form

Rival readings of the immediate, to show that the rule is not one of several
that fit:

  R1  relative to the NEXT node (target = index + 1 + imm)
  R2  an absolute node index (target = imm)
  R3  a byte offset inside the tree as stored on disk (3- and 7-byte nodes),
      relative to the first byte of the jump node
  R4  a byte offset relative to the byte after the jump node

For each rival the script counts the jumps whose target would be a
dispatchable node boundary inside the tree.

Writes out/check_jumps.txt and out/check_jumps.json.
"""
import json
import os
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for _p in (_LIB, _HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import align       # noqa: E402
import check_stmt  # noqa: E402
import disasm      # noqa: E402
import oracle      # noqa: E402
import sparse      # noqa: E402
import tufa        # noqa: E402

JUMPS = {0x1014: 'jump', 0x1015: 'jump_false', 0x1016: 'jump_true', 0x1017: 'case'}


def signed(v):
    return v - (1 << 32) if v & 0x80000000 else v


def main():
    c = Counter()
    by_op = defaultdict(Counter)
    bad = []
    role = defaultdict(Counter)       # (role) -> Counter(fwd/back/count/min/max)
    role_minmax = {}
    src_roles = Counter()
    # which class files have a source-backed, reproduced tree set
    entries = {e.cls: e for e in oracle.iter_entries(('pair_940', 'pair_other'))}
    for rel, blobs in oracle.iter_class_files():
        e = entries.get(rel)
        unit = None
        if e is not None:
            unit = sparse.parse(oracle.load_source(e))
        for bi, b in enumerate(blobs):
            preds = {}
            if unit is not None and len(unit.classes) == len(blobs):
                try:
                    al = align.align(unit.classes[bi], b)
                except align.AlignError:
                    al = []
                for t, kind, node, row, st in al:
                    if t is None:
                        continue
                    r = check_stmt.run_tree(unit, b, t, kind, node, row, st)
                    if r.ok_struct:
                        preds[t] = r.gen.out
            for t, nodes in enumerate(b.trees):
                n = len(nodes)
                c['trees'] += 1
                eaten = disasm.consumed_nodes(nodes)
                # byte offset of every node on disk, inside the tree
                boff = [0]
                for nd in nodes:
                    boff.append(boff[-1] + tufa.NODE_SIZE[nd.op])
                bset = {o: i for i, o in enumerate(boff)}
                pred = preds.get(t)
                if pred is not None:
                    c['trees the model reproduces'] += 1
                for i, nd in enumerate(nodes):
                    code = tufa.OP_FLAGS[nd.op]
                    if code not in JUMPS:
                        continue
                    if i in eaten:
                        c['jump opcodes in a consumed position'] += 1
                        continue
                    off = signed(nd.imm)
                    tgt = i + off
                    nm = JUMPS[code]
                    c['jumps'] += 1
                    by_op[nm]['count'] += 1
                    by_op[nm]['forward' if off > 0 else 'backward' if off < 0 else 'zero'] += 1
                    ok1 = 0 <= tgt <= n
                    c['J1 holds'] += ok1
                    if tgt == n:
                        c['target is the end of the tree'] += 1
                    ok2 = ok1 and tgt not in eaten
                    c['J2 holds'] += ok2
                    c['J3 offset is not 0'] += (off != 0)
                    if off == 1:
                        c['offset +1 (a jump to the next node)'] += 1
                        by_op[nm]['offset +1'] += 1
                    if not (ok1 and ok2):
                        bad.append('%s blob %d tree %d node %d: %s %+d -> %d of %d'
                                   % (rel, bi, t, i, nm, off, tgt, n))
                    # rivals
                    t1 = i + 1 + off
                    c['R1 fits'] += (0 <= t1 <= n and t1 not in eaten)
                    t2 = nd.imm
                    c['R2 fits'] += (0 <= t2 <= n and t2 not in eaten and off >= 0)
                    b3 = boff[i] + off
                    j3 = bset.get(b3)
                    c['R3 fits'] += (j3 is not None and j3 not in eaten)
                    b4 = boff[i + 1] + off
                    j4 = bset.get(b4)
                    c['R4 fits'] += (j4 is not None and j4 not in eaten)
                    if pred is not None:
                        g = pred[i]
                        c['jumps in trees the model reproduces'] += 1
                        if g.code == code and g.imm == nd.imm and g.src == 'rule':
                            c['J4 offset predicted by the model'] += 1
                        r = g.cat
                        role[r]['count'] += 1
                        role[r]['forward' if off > 0 else 'backward'] += 1
                        role[r][nm] += 1
                        lo, hi = role_minmax.get(r, (off, off))
                        role_minmax[r] = (min(lo, off), max(hi, off))
                    else:
                        c['jumps in trees without a reproduced source'] += 1
    L = []
    w = L.append
    w('check_jumps.py: the jump encoding on every jump of the corpus')
    w('')
    w('trees %d, jump nodes %d' % (c['trees'], c['jumps']))
    for nm in ('jump', 'jump_false', 'jump_true', 'case'):
        o = by_op[nm]
        w('  %-11s %6d   forward %6d   backward %5d   zero %d   offset +1: %d'
          % (nm, o['count'], o['forward'], o['backward'], o['zero'], o['offset +1']))
    w('')
    w('rule: target = index of the jump node + signed immediate, in nodes')
    w('  J1 target inside the tree (0..node count)       %d of %d' % (c['J1 holds'], c['jumps']))
    w('     of these, target = node count (tree end)      %d' % c['target is the end of the tree'])
    w('  J2 target is a node the interpreter dispatches  %d of %d' % (c['J2 holds'], c['jumps']))
    w('  J3 offset is not 0                              %d of %d' % (c['J3 offset is not 0'], c['jumps']))
    w('  J4 offset predicted from the statement          %d of %d jumps in the %d trees the model reproduces'
      % (c['J4 offset predicted by the model'], c['jumps in trees the model reproduces'],
         c['trees the model reproduces']))
    w('     jumps in trees without a (reproduced) source %d' % c['jumps in trees without a reproduced source'])
    w('')
    w('rival readings (jumps whose target would be a dispatchable node inside the tree)')
    w('  R1 relative to the next node                    %d of %d' % (c['R1 fits'], c['jumps']))
    w('  R2 absolute node index                          %d of %d' % (c['R2 fits'], c['jumps']))
    w('  R3 byte offset from the start of the jump node  %d of %d' % (c['R3 fits'], c['jumps']))
    w('  R4 byte offset from the end of the jump node    %d of %d' % (c['R4 fits'], c['jumps']))
    w('')
    w('roles of the jumps in source-backed trees (category of the model node)')
    w('  %-24s %7s %8s %8s  %s' % ('role', 'count', 'forward', 'backward', 'offset range'))
    for r in sorted(role):
        o = role[r]
        lo, hi = role_minmax[r]
        w('  %-24s %7d %8d %8d  %+d .. %+d' % (r, o['count'], o['forward'], o['backward'], lo, hi))
    w('')
    w('violations: %d' % len(bad))
    for x in bad[:40]:
        w('  ' + x)
    text = '\n'.join(L) + '\n'
    with open(os.path.join(_HERE, 'out', 'check_jumps.txt'), 'w', newline='\n') as f:
        f.write(text)
    with open(os.path.join(_HERE, 'out', 'check_jumps.json'), 'w', newline='\n') as f:
        json.dump({'counts': dict(c), 'by_op': {k: dict(v) for k, v in by_op.items()},
                   'roles': {k: dict(v, min=role_minmax[k][0], max=role_minmax[k][1])
                             for k, v in role.items()},
                   'violations': bad}, f, indent=1, sort_keys=True)
        f.write('\n')
    sys.stdout.write(text)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
