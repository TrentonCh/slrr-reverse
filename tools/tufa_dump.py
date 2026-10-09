#!/usr/bin/env python3
"""Dump SLRR TUFA .class files: header, constant pool, methods, fields, trees.

Mirrors engine/Core/Jvm/tufa.cpp (old-exe layout, TUFA build 0x11AE1) so the
same heuristics can be checked against Build 940 output (0x24F9D).

Usage:
  tufa_dump.py FILE.class [--method NAME] [--no-cons] [--raw]
"""
import argparse
import struct
import sys

# engine/include/tree_op_flags.inc (old exe VA 0x5F0914).
TREE_OP_FLAGS = [
    0x0000, 0x1001, 0x1002, 0x1003, 0x0004, 0x0005, 0x1006, 0x1007, 0x1008,
    0x4009, 0x200a, 0x100b, 0x000c, 0x100d, 0x100e, 0x100f, 0x0010, 0x1011,
    0x4012, 0x0013, 0x1014, 0x1015, 0x1016, 0x1017, 0x0018, 0x1019, 0x101a,
    0x101b, 0x401c, 0x001d, 0x101e, 0x101f, 0x0020, 0x0021, 0x0022, 0x0023,
    0x0024, 0x4025, 0x4026, 0x1027, 0x0028, 0x0029, 0x002a, 0x002b, 0x002c,
    0x002d, 0x402e, 0x0000,
]


def node_bytes(op):
    if op >= 48:
        return 3
    nib = (TREE_OP_FLAGS[op] >> 12) & 0xF
    return 7 if nib in (1, 2, 4) else 3


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def looks_ascii(bs):
    return all(32 <= c < 127 for c in bs)


def is_string_at(p, pos, ln):
    if ln > 0x10000 or pos + ln > len(p):
        return False
    if not looks_ascii(p[pos:pos + ln]):
        return False
    if pos + ln < len(p):
        return p[pos + ln] == 0
    return True


def read_string(p, pos, ln):
    s = p[pos:pos + ln].decode('latin-1')
    pos += ln
    if pos < len(p) and p[pos] == 0:
        pos += 1
    return s, pos


def parse_cons(p):
    """Port of parse_cons_slrr. Returns list of ('str', s) / ('int', n) / ('ref', (kind, a, b))."""
    out = []
    if len(p) < 4:
        return out
    count = u32(p, 0)
    pos = 4
    for _ in range(count):
        if pos + 4 > len(p):
            break
        peek = u32(p, pos)
        if peek == 0:
            pos += 4
            if pos + 4 > len(p):
                break
            ln = u32(p, pos)
            pos += 4
            if ln > 0x100000 or pos + ln > len(p):
                break
            s, pos = read_string(p, pos, ln)
            out.append(('str', s))
            continue
        if peek == 1:
            if pos + 6 <= len(p) and p[pos + 5] == 0 and looks_ascii(p[pos + 4:pos + 5]):
                if pos + 10 <= len(p):
                    val32 = u32(p, pos + 4)
                    pad16 = struct.unpack_from('<H', p, pos + 8)[0]
                    if pad16 == 0 and (val32 >> 8) == 0:
                        pos += 10
                        ch = val32 & 0xFF
                        out.append(('str', chr(ch)) if 32 <= ch < 127 else ('int', val32))
                        continue
                pos += 4
                s, pos = read_string(p, pos, 1)
                out.append(('str', s))
                continue
            pos += 4
            val = u32(p, pos)
            pos += 4
            if pos + 2 <= len(p) and struct.unpack_from('<H', p, pos)[0] == 0:
                pos += 2
            ch = val & 0xFF
            out.append(('str', chr(ch)) if (val >> 8) == 0 and 32 <= ch < 127 else ('int', val))
            continue
        if peek == 3:
            if is_string_at(p, pos + 4, 3):
                pos += 4
                s, pos = read_string(p, pos, 3)
                out.append(('str', s))
                continue
            pos += 4
            if pos + 8 > len(p):
                break
            out.append(('ref', (3, u32(p, pos), u32(p, pos + 4))))
            pos += 8  # RID is 12 bytes total (kind, pack, local)
            continue
        if peek == 4:
            pos += 4
            # is_class_ref
            idx = u32(p, pos) if pos + 4 <= len(p) else 0xFFFFFFFF
            cref = pos + 8 <= len(p) and idx <= 0x100000
            if cref and is_string_at(p, pos, 4):
                pad = u32(p, pos + 4)
                cref = (pad == 0 and idx < 512)
            if cref:
                pos += 4
                if pos + 4 <= len(p) and u32(p, pos) == 0:
                    pos += 4
                out.append(('int', idx))
            else:
                s, pos = read_string(p, pos, 4)
                out.append(('str', s))
            continue
        if peek in (5, 6, 7):
            q = pos + 4
            ref = True
            if is_string_at(p, q, peek):
                ref = False
                if q + 8 <= len(p):
                    a, b = u32(p, q), u32(p, q + 4)
                    if a < 0x10000 and b < 0x10000 and not looks_ascii(p[q:q + peek]):
                        ref = True
            else:
                ref = q + 8 <= len(p)
            if ref:
                pos += 4
                out.append(('ref', (peek, u32(p, pos), u32(p, pos + 4))))
                pos += 8
            else:
                pos += 4
                s, pos = read_string(p, pos, peek)
                out.append(('str', s))
            continue
        ln = peek
        if ln > 7 and not is_string_at(p, pos + 4, ln):
            pos += 4
            out.append(('int', ln))
            if pos + 4 <= len(p) and u32(p, pos) == 0:
                pos += 4
            continue
        pos += 4
        if ln > 0x100000 or pos + ln > len(p):
            break
        s, pos = read_string(p, pos, ln)
        out.append(('str', s))
    return out


def cons_str(pool, i):
    if i == 0xFFFFFFFF or i >= len(pool):
        return '?'
    k, v = pool[i]
    if k == 'str':
        return v
    if k == 'int' and 0 <= v < len(pool) and pool[v][0] == 'str':
        return pool[v][1]
    if k == 'ref':
        kind, a, b = v
        if kind == 7:
            return '%s:%s' % (cons_str(pool, a), cons_str(pool, b))
        if kind in (5, 6):
            return '%s.%s' % (cons_str(pool, a), cons_str(pool, b))
        return 'rid(%s,%d)' % (cons_str(pool, a), b)
    return repr(v)


def split_blobs(b):
    off = 0
    while off + 12 <= len(b) and b[off:off + 4] == b'TUFA':
        pos = off + 12
        while pos + 8 <= len(b):
            if b[pos:pos + 4] == b'TUFA':
                break
            pos += 8 + u32(b, pos + 4)
        yield b[off:pos]
        off = pos


def sections(blob):
    pos = 12
    out = []
    while pos + 8 <= len(blob):
        tag = blob[pos:pos + 4].decode('ascii', 'replace')
        sz = u32(blob, pos + 4)
        out.append((tag, blob[pos + 8:pos + 8 + sz]))
        pos += 8 + sz
    return out


def parse_trees(p):
    trees = []
    if len(p) < 4:
        return trees, 0
    n = u32(p, 0)
    off = 4
    for _ in range(n):
        nn = u32(p, off)
        off += 4
        nodes = []
        for _ in range(nn):
            op = p[off]
            nb = node_bytes(op)
            slot = struct.unpack_from('<H', p, off + 1)[0]
            imm = u32(p, off + 3) if nb == 7 else None
            nodes.append((op, slot, imm))
            off += nb
        trees.append(nodes)
    return trees, off


def parse_mthd(p, pool):
    out = []
    if len(p) < 8:
        return out
    n_name_first = u32(p, 0)
    n_total = (len(p) - 8) // 20
    if n_name_first > n_total:
        n_name_first = 0
    off = 8
    for mi in range(n_total):
        w = struct.unpack_from('<5I', p, off)
        off += 20
        if mi < n_name_first:  # first container = static methods
            name, sig, tree_i, flags = cons_str(pool, w[0]), cons_str(pool, w[1]), w[2], w[3] | 0x8
        else:
            flags, name, sig, tree_i = w[0], cons_str(pool, w[1]), cons_str(pool, w[2]), w[3]
        out.append((name, sig, tree_i, flags, w))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--method', help='only dump trees for this method name')
    ap.add_argument('--no-cons', action='store_true')
    ap.add_argument('--raw', action='store_true', help='print raw node bytes too')
    a = ap.parse_args()
    data = open(a.file, 'rb').read()
    for bi, blob in enumerate(split_blobs(data)):
        print('== TUFA blob %d  size=%d  version=0x%X' % (bi, len(blob), u32(blob, 8)))
        secs = dict(sections(blob))
        for tag, p in sections(blob):
            print('   section %s %d bytes' % (tag, len(p)))
        pool = parse_cons(secs.get('CONS', b''))
        if not a.no_cons:
            print('-- CONS (%d entries, declared %d)' % (len(pool), u32(secs['CONS'], 0) if 'CONS' in secs else -1))
            for i, (k, v) in enumerate(pool):
                print('   [%3d] %-4s %r' % (i, k, v if k != 'ref' else v), '' if k != 'ref' else '-> ' + cons_str(pool, i))
        if 'CLSS' in secs:
            w = struct.unpack_from('<5I', secs['CLSS'], 0)
            print('-- CLSS', [hex(x) for x in w], 'name=%s super=%s' % (cons_str(pool, w[2]), cons_str(pool, w[3])))
        trees, used = parse_trees(secs.get('TREE', b''))
        print('-- TREE %d trees, parsed %d of %d bytes' % (len(trees), used, len(secs.get('TREE', b''))))
        methods = parse_mthd(secs.get('MTHD', b''), pool)
        print('-- MTHD %d methods (n_name_first=%d)' % (len(methods), u32(secs['MTHD'], 0) if 'MTHD' in secs else -1))
        for name, sig, ti, flags, w in methods:
            nn = len(trees[ti]) if ti < len(trees) else -1
            print('   %-28s %-40s tree=%-3s nodes=%-4d flags=0x%X raw=%s' % (name, sig, ti if ti != 0xFFFFFFFF else '-', nn, flags, [hex(x) for x in w]))
        if 'FILD' in secs:
            p = secs['FILD']
            print('-- FILD %d bytes: ' % len(p), ' '.join('%08x' % x for x in struct.unpack_from('<%dI' % (len(p) // 4), p, 0)))
        for name, sig, ti, flags, w in methods:
            if a.method and name != a.method:
                continue
            if ti >= len(trees):
                continue
            if not a.method and len(trees[ti]) == 0:
                continue
            print('-- tree %d  %s %s  (%d nodes)' % (ti, name, sig, len(trees[ti])))
            for ip, (op, slot, imm) in enumerate(trees[ti]):
                fl = TREE_OP_FLAGS[op] if op < 48 else None
                immtxt = ''
                if imm is not None:
                    immtxt = 'imm=%-6d' % imm
                    if 0 <= imm < len(pool):
                        immtxt += ' (%s)' % cons_str(pool, imm)
                print('   %3d: op=0x%02x slot=%-4d %s' % (ip, op, slot, immtxt))


if __name__ == '__main__':
    main()
