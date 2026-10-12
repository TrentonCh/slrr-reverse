#!/usr/bin/env python3
"""symbolic.py - class-file nodes in the symbolic form gen.py predicts.

    sym = symbolic.sym_nodes(blob, nodes)       # [(mnemonic, operand)], one per node
    keep = symbolic.filter_nodes(sym)           # indices of the nodes gen.py predicts
    symbolic.node_match(pred, act)              # one predicted node against one actual

Pool indices are replaced by what they name (text, class name, (class, member,
descriptor), (pack, id)), so two streams compare without knowing the pool order.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for p in (_LIB, _HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import opcodes as oc   # noqa: E402
import paircheck       # noqa: E402
import srclex          # noqa: E402
import tufa            # noqa: E402

# statement-level nodes: not part of any expression, not predicted by gen.py
CONTROL = frozenset(('release', 'jump_false', 'jump_true', 'case', 'pop',
                     'value', 'push_void', 'ret', 'ret_value', 'end'))


def sym_nodes(blob, nodes):
    out = []
    for nd in nodes:
        code = tufa.OP_FLAGS[nd.op]
        name = oc.OPS[nd.op].name
        if code in (0x1007, 0x1008):
            t = oc.tag_info(nd.imm)
            out.append((name, t.name if t else 'tag%02x' % nd.imm))
        elif code in (0x101a, 0x101b):
            out.append((name, blob.ref(nd.imm)))
        elif code in (0x4009, 0x4012, 0x401c, 0x4025, 0x4026, 0x1006):
            out.append((name, blob.utf8(nd.imm)))
        elif code == 0x1019:
            out.append((name, blob.class_name(nd.imm)))
        elif code == 0x100e:
            out.append((name, blob.rid(nd.imm)))
        elif code in (0x1014, 0x1015, 0x1016, 0x1017):
            imm = nd.imm - (1 << 32) if nd.imm & 0x80000000 else nd.imm
            out.append((name, imm))
        else:
            out.append((name, nd.imm))
    return out


def filter_nodes(sym):
    """Indices of the nodes that belong to expressions and declarations: all
    but the CONTROL nodes and the jumps that are not the skip of && / ||."""
    keep = []
    for i, (mn, op) in enumerate(sym):
        if mn in CONTROL:
            continue
        if mn == 'jump' and not (i and sym[i - 1][0] in ('and_test', 'or_test')):
            continue
        keep.append(i)
    return keep


def float_ok(text, bits):
    fb = paircheck.float_token_bits(text)
    return fb is not None and abs(fb - bits) <= paircheck.FLOAT_ULP_SLACK


def rid_ok(pred, act):
    pack, local = pred
    apack, alocal = act
    if (apack.lower(), alocal) == ('system.rpk', 0):
        return True                      # fallback for a pack that does not exist
    return local == alocal and srclex.rid_pack_file(pack).lower() == apack.lower()


def node_match(pred, sym, keep, ak):
    """How many actual nodes (0 = mismatch) the predicted node accounts for,
    starting at filtered position ak.  Wildcard: ('callee?', info) matches a
    method ref or a name segment, and for a call without receiver also the
    pair `path 1; name`."""
    pm, po = pred
    am, ao = sym[keep[ak]]
    if pm == 'callee?':
        if am == 'method':
            return 1 if ao[1] == po['name'] else 0
        if am == 'name':
            return 1 if ao == po['name'] else 0
        if am == 'path' and ao == 1 and po['lone'] and ak + 1 < len(keep):
            bm, bo = sym[keep[ak + 1]]
            return 2 if bm == 'name' and bo == po['name'] else 0
        return 0
    if pm != am:
        return 0
    if pm == 'float':
        return 1 if float_ok(po, ao) else 0
    if pm == 'rid':
        return 1 if rid_ok(po, ao) else 0
    return 1 if po == ao else 0


def fmt(node):
    mn, op = node
    if mn == 'callee?':
        return 'callee? %s.%s%r' % (op['cls'], op['name'], op['argt'])
    if mn in ('method', 'field'):
        return '%s %s.%s:%s' % (mn, op[0], op[1], op[2])
    if op is None:
        return mn
    if isinstance(op, tuple):
        return '%s %s' % (mn, ' '.join(str(x) for x in op))
    return '%s %r' % (mn, op) if isinstance(op, str) else '%s %s' % (mn, op)
