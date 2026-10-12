#!/usr/bin/env python3
"""tree_shape.py - how every tree of the corpus starts and ends.

Over all 2,993 blobs (9,772 trees), without using any source:

  * kind of tree: field initialiser, constructor, void method, value method;
    empty or not
  * the first nodes: how a constructor opens (arguments of the super / this
    call, the count, super_init / this_init, stmt ctor)
  * the last nodes: the epilogue (implicit return, releases)
  * the u16 of the nodes: how many distinct values a tree holds (the old
    documents call it "slot" and say it looks constant inside a tree)
  * descending `local` runs at the top and descending `release` runs at the
    end (what the old dumps showed)

Writes out/tree_shape.txt and out/tree_shape.json.
"""
import json
import os
import sys
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for _p in (_LIB, _HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import disasm    # noqa: E402
import opcodes   # noqa: E402
import oracle    # noqa: E402
import tufa      # noqa: E402

LOCAL, RELEASE = 0x1001, 0x1003
PUSH_INT, SUPER_INIT, THIS_INIT, STMT = 0x1027, 0x0023, 0x0022, 0x1008
PUSH_VOID, RET, RET_VALUE, END = 0x0029, 0x002a, 0x002b, 0x0010


def mn(nd):
    return opcodes.mnemonic(nd.op, nd.imm)


def main():
    c = Counter()
    first = Counter()
    last = Counter()
    lines = Counter()
    tails = Counter()
    opens = Counter()
    bad = []
    for rel, bi, b in oracle.iter_blobs():
        own = {}
        for st, i, f in b.fields():
            if f.init != tufa.NONE:
                own[f.init] = ('field', st, f)
        for st, i, m in b.methods():
            if m.tree != tufa.NONE:
                own[m.tree] = ('method', st, m)
        for t, nodes in enumerate(b.trees):
            kind, st, row = own[t]
            code = [tufa.OP_FLAGS[x.op] for x in nodes]
            n = len(nodes)
            c['trees'] += 1
            if kind == 'field':
                what = 'field initialiser'
                nloc = 0
            else:
                name = b.utf8(row.name)
                params, ret = disasm.split_descriptor(b.utf8(row.desc))
                nloc = len(params) + (0 if st else 1)
                if row.flags & tufa.ACC_NATIVE:
                    what = 'native method'
                elif name == '<init>':
                    what = 'constructor'
                elif ret == 'V':
                    what = 'void method'
                else:
                    what = 'value method'
            c[what] += 1
            if n == 0:
                c[what + ', empty tree'] += 1
                continue
            c[what + ', with nodes'] += 1
            nl = len(set(x.slot for x in nodes))
            lines['%s: %s' % (what, '1 line' if nl == 1 else '2 lines' if nl == 2 else '3 or more lines')] += 1
            c['trees with nodes'] += 1
            if nl == 1:
                c['trees whose nodes all carry the same line'] += 1
            # are the lines non-decreasing?  (they are not: for-updates, jumps)
            first['%s: %s' % (what, mn(nodes[0]))] += 1
            last['%s: %s' % (what, mn(nodes[-1]))] += 1
            # trailing release run
            k = n
            while k > 0 and code[k - 1] == RELEASE:
                k -= 1
            ntrail = n - k
            if what == 'field initialiser':
                ok = code[-1] == END and END not in code[:-1]
                tails['field initialiser: ... end' if ok else 'field initialiser: OTHER'] += 1
                if not ok:
                    bad.append('%s blob %d tree %d' % (rel, bi, t))
                continue
            # the releases at the very end: the body locals and then the
            # parameters and this, i.e. L-1 .. 0 for some L >= nloc
            idx = [nodes[j].imm for j in range(k, n)]
            if what == 'constructor':
                # ret; R(L); R(L)
                L = ntrail // 2
                ok = ntrail % 2 == 0 and k >= 1 and code[k - 1] == RET and \
                    idx == list(range(L - 1, -1, -1)) * 2 and L >= nloc
                tails['constructor: ret; release L-1..0; release L-1..0' if ok
                      else 'constructor: OTHER'] += 1
                if ok and L > nloc:
                    c['constructor epilogue that also releases body locals'] += 1
            elif what == 'void method':
                # push_void; R(L); ret_value; R(L)
                L = ntrail
                ok = k >= 1 and code[k - 1] == RET_VALUE and L >= nloc and \
                    idx == list(range(L - 1, -1, -1)) and \
                    k - 1 - L - 1 >= 0 and code[k - 2 - L] == PUSH_VOID and \
                    [nodes[j].imm for j in range(k - 1 - L, k - 1)] == list(range(L - 1, -1, -1)) and \
                    all(code[j] == RELEASE for j in range(k - 1 - L, k - 1))
                tails['void method: push_void; release L-1..0; ret_value; release L-1..0'
                      if ok else 'void method: OTHER'] += 1
                if ok and L > nloc:
                    c['void epilogue that also releases body locals'] += 1
            else:
                # ...; ret_value; R(L)      (no implicit return)
                L = ntrail
                ok = k >= 1 and code[k - 1] == RET_VALUE and L >= nloc and \
                    idx == list(range(L - 1, -1, -1))
                tails['value method: ... ret_value; release L-1..0' if ok
                      else 'value method: OTHER'] += 1
            if not ok:
                bad.append('%s blob %d tree %d (%s)' % (rel, bi, t, what))
            if what == 'constructor':
                # the opening: arguments, push_int n, super_init|this_init, stmt ctor
                j = None
                for q in range(n):
                    if code[q] in (SUPER_INIT, THIS_INIT):
                        j = q
                        break
                if j is None or j < 1 or code[j - 1] != PUSH_INT or j + 1 >= n or \
                        code[j + 1] != STMT or nodes[j + 1].imm != 0x22:
                    opens['OTHER'] += 1
                    bad.append('%s blob %d tree %d (constructor opening)' % (rel, bi, t))
                    continue
                argc = nodes[j - 1].imm
                which = 'super_init' if code[j] == SUPER_INIT else 'this_init'
                opens['%s with %s' % (which, 'no argument' if argc == 0 else 'arguments')] += 1
                # leading descending run of local nodes
                r = 0
                while r < j - 1 and code[r] == LOCAL and (r == 0 or nodes[r].imm == nodes[r - 1].imm - 1):
                    r += 1
                if r >= 2:
                    c['constructors that open with 2 or more `local` nodes in descending order'] += 1
                if r >= 1 and r == j - 1 and r == argc:
                    c['constructors whose super/this arguments are exactly its parameters in order'] += 1
    L = []
    w = L.append
    w('tree_shape.py: first and last nodes of every tree')
    w('')
    for k in sorted(c):
        w('  %-76s %d' % (k, c[k]))
    w('')
    w('distinct line values per tree')
    for k in sorted(lines):
        w('  %-60s %d' % (k, lines[k]))
    w('')
    w('how constructors open')
    for k in sorted(opens):
        w('  %-60s %d' % (k, opens[k]))
    w('')
    w('how trees end')
    for k in sorted(tails):
        w('  %-76s %d' % (k, tails[k]))
    w('')
    w('first node')
    for k, v in sorted(first.items(), key=lambda kv: (kv[0].split(':')[0], -kv[1])):
        w('  %-60s %d' % (k, v))
    w('')
    w('last node')
    for k, v in sorted(last.items(), key=lambda kv: (kv[0].split(':')[0], -kv[1])):
        w('  %-60s %d' % (k, v))
    w('')
    w('trees outside the patterns: %d' % len(bad))
    for x in bad[:20]:
        w('  ' + x)
    text = '\n'.join(L) + '\n'
    with open(os.path.join(_HERE, 'out', 'tree_shape.txt'), 'w', newline='\n') as f:
        f.write(text)
    with open(os.path.join(_HERE, 'out', 'tree_shape.json'), 'w', newline='\n') as f:
        json.dump({'counts': dict(c), 'lines': dict(lines), 'opens': dict(opens),
                   'tails': dict(tails), 'first': dict(first), 'last': dict(last),
                   'outside': bad}, f, indent=1, sort_keys=True)
        f.write('\n')
    sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
