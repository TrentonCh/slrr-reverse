#!/usr/bin/env python3
"""disasm.py - readable, stable disassembly of SLRR .class files.

    python -I disasm.py FILE.class [options]

    --class NAME     only the blob whose class name ends with NAME
    --blob N         only blob N of the file
    --method NAME    only the trees of methods called NAME (NAME may carry the
                     descriptor: "show(I)V"; "<init>" works)
    --tree N         only tree N (may be given several times)
    --field NAME     only the initialiser tree of field NAME
    --no-pool        leave the constant pool listing out
    --no-trees       header, pool and member tables only
    --raw            add the raw node bytes to every node line
    --json           machine-readable output (one JSON document)

FILE may be a path or a path relative to the corpus (see oracle.py).  The text
form never contains addresses, times or anything else that changes between
runs; two class files with the same content disassemble to the same text.

A node line reads

      12  L36   expr literal
      13  L36   str          [56] ",0,0,0,sphere,"

node index, source line (the u16 after the op byte), mnemonic, operand.  The
operand is rendered by kind: a pool index as "[index] text", a jump as
"+8 -> 20", a local as its index plus what it holds when that is known
(this / parameter name position / declared local), a float as its value and
bit pattern.  Nodes that the VM does not dispatch on their own (carriers and
path segments) are indented by two spaces under the node that consumes them.

As a library:

    import disasm
    text = disasm.disassemble_file(path)             # str
    doc  = disasm.blob_to_json(blob)                 # dict
    for line in disasm.tree_lines(blob, tree_index): ...
"""
import json
import os
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import opcodes as oc  # noqa: E402
import tufa           # noqa: E402

NONE = tufa.NONE


# ---------------------------------------------------------------------------
# descriptors
# ---------------------------------------------------------------------------

def split_descriptor(desc):
    """'(ILjava.lang.String;[F)V' -> (['I', 'Ljava.lang.String;', '[F'], 'V').
    The return part is '' for constructors.  Raises ValueError when malformed."""
    if not desc.startswith('('):
        raise ValueError('descriptor %r does not start with "("' % desc)
    i = 1
    params = []
    n = len(desc)
    while True:
        if i >= n:
            raise ValueError('descriptor %r has no ")"' % desc)
        if desc[i] == ')':
            break
        j = i
        while j < n and desc[j] == '[':
            j += 1
        if j >= n:
            raise ValueError('descriptor %r is cut short' % desc)
        if desc[j] == 'L':
            k = desc.find(';', j)
            if k < 0:
                raise ValueError('descriptor %r: class name without ";"' % desc)
            j = k + 1
        else:
            j += 1
        params.append(desc[i:j])
        i = j
    return params, desc[i + 1:]


def local_names(blob, is_static, method):
    """index -> label for the locals a frame starts with: this, then the
    parameters in reverse order (VMThread_invokeMethod pops them top first)."""
    out = {}
    try:
        params, _ = split_descriptor(blob.utf8(method.desc))
    except (ValueError, tufa.TufaError):
        return out
    base = 0
    if not is_static:
        out[0] = 'this'
        base = 1
    n = len(params)
    for k in range(n):
        out[base + k] = 'arg%d %s' % (n - 1 - k, params[n - 1 - k])
    return out


# ---------------------------------------------------------------------------
# node rendering
# ---------------------------------------------------------------------------

def consumed_nodes(nodes):
    """Set of node indices that the VM does not dispatch on their own because
    the node before them (or the path header) consumes them: literal and type
    carriers, the 0x002C marker, the 0x1011 header after 0x0024 and the
    segments of every name path."""
    eaten = set()
    n = len(nodes)
    code = [tufa.OP_FLAGS[x.op] for x in nodes]
    i = 0
    while i < n:
        c = code[i]
        if i in eaten:
            i += 1
            continue
        if c in (0x1007, 0x1008):
            t = oc.tag_info(nodes[i].imm)
            if t is not None and t.carrier and i + 1 < n:
                eaten.add(i + 1)
        elif c == 0x1002:
            j = i + 1
            if j < n and code[j] == 0x402e:
                eaten.add(j)
                j += 1
            if j < n and code[j] == 0x1006:
                eaten.add(j)
                if j + 1 < n and code[j + 1] == 0x002c:
                    eaten.add(j + 1)
        elif c == 0x1011:
            # Thread_evalName_fieldPath: the imm nodes that follow are the
            # segments (a 0x1019 class node and its companion count as two)
            for k in range(i + 1, min(n, i + 1 + nodes[i].imm)):
                eaten.add(k)
        elif c == 0x0024:
            # Thread_evalName: a lone 0x101A, or a header whose imm is the
            # number of segment nodes that follow it
            j = i + 1
            if j < n:
                eaten.add(j)
                if code[j] != 0x101a and nodes[j].imm is not None:
                    for k in range(j + 1, min(n, j + 1 + nodes[j].imm)):
                        eaten.add(k)
        i += 1
    return eaten


def _float_text(bits):
    v = struct.unpack('<f', struct.pack('<I', bits))[0]
    return '%r (0x%08X)' % (v, bits)


def render_operand(blob, nodes, i, locals_map=None):
    """Operand text of node i and a dict with the same facts for JSON."""
    nd = nodes[i]
    o = oc.OPS[nd.op]
    info = {}
    if nd.imm is None:
        return '', info
    imm = nd.imm
    k = o.imm
    if k == oc.IMM_TAG:
        t = oc.tag_info(imm)
        info['tag'] = imm
        info['tag_name'] = t.name if t else None
        return ('' if t else '0x%02X' % imm), info
    if k in oc.POOL_KINDS:
        info['pool'] = imm
        if 0 <= imm < len(blob.cons):
            e = blob.cons[imm]
            info['pool_kind'] = e[0]
            info['pool_text'] = blob.describe(imm) if e[0] != 'utf8' else e[1]
            txt = '[%d] %s' % (imm, blob.describe(imm))
            if e[0] != k:
                txt += '   !! expected a %s entry, found %s' % (k, e[0])
                info['pool_mismatch'] = True
            return txt, info
        info['pool_mismatch'] = True
        return '[%d] !! outside the pool' % imm, info
    if k == oc.IMM_LOCAL:
        info['local'] = imm
        lab = (locals_map or {}).get(imm)
        if lab:
            info['local_label'] = lab
            return '%d  ; %s' % (imm, lab), info
        return '%d' % imm, info
    if k == oc.IMM_JUMP:
        off = imm - (1 << 32) if imm & 0x80000000 else imm
        info['offset'] = off
        info['target'] = i + off
        return '%+d -> %d' % (off, i + off), info
    if k == oc.IMM_FLOAT:
        info['float'] = struct.unpack('<f', struct.pack('<I', imm))[0]
        info['bits'] = imm
        return _float_text(imm), info
    if k == oc.IMM_INT:
        v = imm - (1 << 32) if imm & 0x80000000 else imm
        info['int'] = v
        if -4096 < v < 4096:
            return '%d' % v, info
        return '%d (0x%08X)' % (v, imm), info
    if k == oc.IMM_COUNT:
        info['count'] = imm
        return '%d' % imm, info
    return '0x%X' % imm, info


def declared_locals(nodes, locals_map):
    """Extend a copy of locals_map with 'var TYPE' for the declares of the
    tree; an index declared more than once gets 'var' with the types joined."""
    out = dict(locals_map or {})
    seen = {}
    code = [tufa.OP_FLAGS[x.op] for x in nodes]
    return out, seen, code


def tree_lines(blob, t, raw=False, locals_map=None):
    """Text lines of tree t."""
    nodes = blob.trees[t]
    eaten = consumed_nodes(nodes)
    lines = []
    lm = dict(locals_map or {})
    for i, nd in enumerate(nodes):
        o = oc.OPS[nd.op]
        if o.code == 0x1002 and i + 1 < len(nodes) and \
                tufa.OP_FLAGS[nodes[i + 1].op] == 0x1006:
            try:
                lm[nd.imm] = 'var %s' % blob.utf8(nodes[i + 1].imm)
            except tufa.TufaError:
                pass
        opnd, _ = render_operand(blob, nodes, i, lm)
        mn = oc.mnemonic(nd.op, nd.imm)
        if i in eaten:
            mn = '  ' + mn
        s = '%6d  L%-5d %-18s %s' % (i, nd.slot, mn, opnd)
        s = s.rstrip()
        if raw:
            b = struct.pack('<BH', nd.op, nd.slot)
            if nd.imm is not None:
                b += struct.pack('<I', nd.imm)
            s = '%-72s ; %s' % (s, b.hex())
        lines.append(s)
    return lines


# ---------------------------------------------------------------------------
# blob rendering
# ---------------------------------------------------------------------------

def _owner_table(blob):
    """tree index -> (kind, is_static, row index, row)"""
    own = {}
    for st, i, f in blob.fields():
        if f.init != NONE:
            own.setdefault(f.init, []).append(('field', st, i, f))
    for st, i, m in blob.methods():
        if m.tree != NONE:
            own.setdefault(m.tree, []).append(('method', st, i, m))
    return own


def _safe(blob, i):
    try:
        return blob.utf8(i)
    except tufa.TufaError:
        return '<pool %d>' % i


def owner_label(blob, own):
    kind, st, i, row = own
    if kind == 'field':
        return '%sfield %s : %s' % ('static ' if st else '', _safe(blob, row.name),
                                    _safe(blob, row.type))
    return '%smethod %s%s' % ('static ' if st else '', _safe(blob, row.name),
                              _safe(blob, row.desc))


def select_trees(blob, trees=None, methods=None, fields=None):
    """Tree indices picked by the filters (None = no filter of that kind).
    With no filter at all: every tree."""
    if not trees and not methods and not fields:
        return list(range(len(blob.trees)))
    picked = set()
    for t in trees or ():
        if not 0 <= t < len(blob.trees):
            raise SystemExit('tree %d: the class has %d trees' % (t, len(blob.trees)))
        picked.add(t)
    for st, i, m in blob.methods():
        if m.tree == NONE:
            continue
        nm = _safe(blob, m.name)
        full = nm + _safe(blob, m.desc)
        for want in methods or ():
            if want == nm or want == full:
                picked.add(m.tree)
    for st, i, f in blob.fields():
        if f.init == NONE:
            continue
        if _safe(blob, f.name) in (fields or ()):
            picked.add(f.init)
    return sorted(picked)


def blob_lines(blob, index=0, path=None, pool=True, trees=True, raw=False,
               tree_filter=None, method_filter=None, field_filter=None):
    L = []
    w = L.append
    try:
        nm = blob.name
    except tufa.TufaError:
        nm = '<bad CLSS name %d>' % blob.clss.name
    w('%s %s' % ('interface' if blob.is_interface else 'class', nm))
    if path:
        w('  file        %s  (blob %d)' % (path, index))
    w('  build       0x%X%s' % (blob.build, '' if blob.build == tufa.BUILD_940
                                else '  (not Build 940)'))
    w('  chunks      TUFA %s' % ' '.join(blob.chunks))
    try:
        w('  super       %s' % (blob.super_name or '-'))
        ifs = blob.interface_names
        w('  interfaces  %s' % (', '.join(ifs) if ifs else '-'))
    except tufa.TufaError as x:
        w('  !! %s' % x)
    c = blob.clss
    w('  clss        marker=0x%08X interface=%d name=[%d] super=%s interfaces=[%s]' % (
        c.marker, c.interface, c.name,
        '-' if c.super == NONE else '[%d]' % c.super,
        ', '.join('%d' % i for i in c.interfaces)))
    w('')
    if pool:
        w('pool (%d entries)' % len(blob.cons))
        for i, e in enumerate(blob.cons):
            if e[0] == 'utf8':
                w('  [%d] utf8   %s' % (i, tufa.repr_text(e[1])))
            elif e[0] == 'class':
                w('  [%d] class  [%d] %s' % (i, e[1], blob.describe(i)))
            elif e[0] == 'nat':
                w('  [%d] nat    [%d] [%d] %s' % (i, e[1], e[2], blob.describe(i)))
            elif e[0] == 'ref':
                w('  [%d] ref    [%d] [%d] %s' % (i, e[1], e[2], blob.describe(i)))
            elif e[0] == 'rid':
                w('  [%d] rid    [%d] 0x%08X %s' % (i, e[1], e[2], blob.describe(i)))
        w('')
    own = _owner_table(blob)
    for title, rows in (('static fields', blob.static_fields),
                        ('instance fields', blob.instance_fields)):
        w('%s (%d)' % (title, len(rows)))
        for i, f in enumerate(rows):
            w('  [%d] flags=0x%02X %-20s %s : %s   init=%s' % (
                i, f.flags, tufa.flags_text(f.flags), _safe(blob, f.name),
                _safe(blob, f.type),
                '-' if f.init == NONE else 'tree %d' % f.init))
    for title, rows in (('static methods', blob.static_methods),
                        ('instance methods', blob.instance_methods)):
        w('%s (%d)' % (title, len(rows)))
        for i, m in enumerate(rows):
            nn = ''
            if m.tree != NONE and m.tree < len(blob.trees):
                nn = ' (%d nodes)' % len(blob.trees[m.tree])
            w('  [%d] flags=0x%02X %-20s %s%s   tree=%s%s locals=%d' % (
                i, m.flags, tufa.flags_text(m.flags), _safe(blob, m.name),
                _safe(blob, m.desc),
                '-' if m.tree == NONE else '%d' % m.tree, nn, m.locals))
    w('trees (%d)' % len(blob.trees))
    if not trees:
        return L
    for t in select_trees(blob, tree_filter, method_filter, field_filter):
        w('')
        owners = own.get(t, [])
        label = '; '.join(owner_label(blob, o) for o in owners) or 'unowned'
        w('tree %d: %s   (%d nodes)' % (t, label, len(blob.trees[t])))
        lm = None
        if len(owners) == 1 and owners[0][0] == 'method':
            lm = local_names(blob, owners[0][1], owners[0][3])
        L.extend(tree_lines(blob, t, raw=raw, locals_map=lm))
    return L


def blob_to_json(blob, index=0, path=None, tree_filter=None, method_filter=None,
                 field_filter=None, pool=True):
    own = _owner_table(blob)
    doc = {
        'file': path,
        'blob': index,
        'name': blob.name,
        'super': blob.super_name,
        'interface': bool(blob.is_interface),
        'interfaces': blob.interface_names,
        'build': blob.build,
        'chunks': list(blob.chunks),
        'clss': {'marker': blob.clss.marker, 'interface': blob.clss.interface,
                 'name': blob.clss.name, 'super': blob.clss.super,
                 'interfaces': list(blob.clss.interfaces)},
    }
    if pool:
        doc['pool'] = [{'index': i, 'kind': e[0], 'args': list(e[1:]),
                        'text': e[1] if e[0] == 'utf8' else blob.describe(i)}
                       for i, e in enumerate(blob.cons)]

    def fld(st, i, f):
        return {'static': st, 'index': i, 'flags': f.flags,
                'flags_text': tufa.flags_text(f.flags), 'name': _safe(blob, f.name),
                'type': _safe(blob, f.type), 'name_index': f.name,
                'type_index': f.type, 'init': None if f.init == NONE else f.init}

    def mth(st, i, m):
        return {'static': st, 'index': i, 'flags': m.flags,
                'flags_text': tufa.flags_text(m.flags), 'name': _safe(blob, m.name),
                'descriptor': _safe(blob, m.desc), 'name_index': m.name,
                'descriptor_index': m.desc,
                'tree': None if m.tree == NONE else m.tree, 'locals': m.locals}

    doc['fields'] = [fld(*x) for x in blob.fields()]
    doc['methods'] = [mth(*x) for x in blob.methods()]
    doc['tree_count'] = len(blob.trees)
    trees = []
    for t in select_trees(blob, tree_filter, method_filter, field_filter):
        nodes = blob.trees[t]
        eaten = consumed_nodes(nodes)
        owners = own.get(t, [])
        lm = {}
        if len(owners) == 1 and owners[0][0] == 'method':
            lm = local_names(blob, owners[0][1], owners[0][3])
        out = []
        for i, nd in enumerate(nodes):
            o = oc.OPS[nd.op]
            if o.code == 0x1002 and i + 1 < len(nodes) and \
                    tufa.OP_FLAGS[nodes[i + 1].op] == 0x1006:
                try:
                    lm[nd.imm] = 'var %s' % blob.utf8(nodes[i + 1].imm)
                except tufa.TufaError:
                    pass
            _, info = render_operand(blob, nodes, i, lm)
            rec = {'i': i, 'line': nd.slot, 'op': nd.op, 'code': o.code,
                   'mnemonic': oc.mnemonic(nd.op, nd.imm), 'imm': nd.imm,
                   'imm_kind': o.imm, 'consumed': i in eaten}
            rec.update(info)
            out.append(rec)
        trees.append({'index': t,
                      'owners': [{'kind': k, 'static': st, 'index': i,
                                  'name': _safe(blob, row.name),
                                  'type': _safe(blob, row.type if k == 'field' else row.desc)}
                                 for k, st, i, row in owners],
                      'label': '; '.join(owner_label(blob, o) for o in owners) or 'unowned',
                      'nodes': out})
    doc['trees'] = trees
    return doc


def _pick_blobs(blobs, cls=None, blob_index=None):
    out = []
    for i, b in enumerate(blobs):
        if blob_index is not None and i != blob_index:
            continue
        if cls is not None:
            n = b.name
            if not (n == cls or n.endswith('.' + cls)):
                continue
        out.append((i, b))
    return out


def disassemble(blobs, path=None, cls=None, blob_index=None, **kw):
    """Text of a parsed class file (list of Blob)."""
    parts = []
    for i, b in _pick_blobs(blobs, cls, blob_index):
        parts.append('\n'.join(blob_lines(b, i, path, **kw)))
    return ('\n\n' + '=' * 78 + '\n\n').join(parts) + '\n'


def disassemble_file(path, **kw):
    return disassemble(tufa.parse_file(path), path=_display_path(path), **kw)


def _display_path(path):
    return str(path).replace('\\', '/')


def _resolve(path):
    if os.path.exists(path):
        return path
    try:
        import oracle
        p = os.path.join(oracle.corpus_root(), path.replace('/', os.sep))
        if os.path.exists(p):
            return p
    except ImportError:
        pass
    return path


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(
        description='Disassemble SLRR .class (TUFA) files.',
        epilog='See the module docstring for the output format.')
    ap.add_argument('file')
    ap.add_argument('--class', dest='cls', help='only the class with this (short) name')
    ap.add_argument('--blob', type=int, help='only this blob of the file')
    ap.add_argument('--method', action='append', help='only trees of this method')
    ap.add_argument('--tree', action='append', type=int, help='only this tree')
    ap.add_argument('--field', action='append', help='only this field initialiser')
    ap.add_argument('--no-pool', action='store_true')
    ap.add_argument('--no-trees', action='store_true')
    ap.add_argument('--raw', action='store_true', help='show node bytes')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    path = _resolve(a.file)
    blobs = tufa.parse_file(path)
    shown = _display_path(a.file)
    picked = _pick_blobs(blobs, a.cls, a.blob)
    if not picked:
        sys.stderr.write('no blob matches (the file holds: %s)\n'
                         % ', '.join(b.name for b in blobs))
        return 1
    out = sys.stdout
    try:
        out.reconfigure(encoding='utf-8', newline='\n')
    except (AttributeError, ValueError):
        pass
    if a.json:
        doc = {'file': shown, 'blobs': [
            blob_to_json(b, i, shown, a.tree, a.method, a.field,
                         pool=not a.no_pool) for i, b in picked]}
        json.dump(doc, out, indent=1)
        out.write('\n')
        return 0
    out.write(disassemble(blobs, path=shown, cls=a.cls, blob_index=a.blob,
                          pool=not a.no_pool, trees=not a.no_trees, raw=a.raw,
                          tree_filter=a.tree, method_filter=a.method,
                          field_filter=a.field))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
