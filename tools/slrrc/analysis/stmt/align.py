#!/usr/bin/env python3
"""align.py - which tree of a class blob belongs to which member of the source.

Trees are numbered in source order, field initialisers and method bodies
interleaved (foundation.md 3.5).  align() walks the members of a parsed class
(sparse.py) and the rows of the blob together and yields

    (tree index, 'field', decl N, Field row, is_static)
    (tree index, 'method', method N, Method row, is_static)

It raises AlignError when names or counts disagree, so a pair that passes has
every tree attributed to exactly one declaration.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for _p in (_LIB, _HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import tufa  # noqa: E402

NONE = tufa.NONE


class AlignError(Exception):
    pass


def owners(blob):
    own = {}
    for st, i, f in blob.fields():
        if f.init != NONE:
            if f.init in own:
                raise AlignError('tree %d has two owners' % f.init)
            own[f.init] = ('field', st, f)
    for st, i, m in blob.methods():
        if m.tree != NONE:
            if m.tree in own:
                raise AlignError('tree %d has two owners' % m.tree)
            own[m.tree] = ('method', st, m)
    return own


def align(cls, blob):
    """cls: sparse N('class'), blob: tufa.Blob.  Returns the list described
    in the module docstring plus the members that own no tree:
    (None, 'method', N, row or None, is_static)."""
    own = owners(blob)
    out = []
    t = 0
    # rows without a tree (abstract), by name, for the methods that have none
    notree = {}
    for st, i, m in blob.methods():
        if m.tree == NONE:
            notree.setdefault(blob.utf8(m.name), []).append((st, m))
    for mem in cls.members:
        if mem.k == 'field':
            for d in mem.decls:
                if d.init is None:
                    continue
                o = own.get(t)
                if o is None or o[0] != 'field' or blob.utf8(o[2].name) != d.name:
                    raise AlignError('tree %d: source says initialiser of field %s, '
                                     'class says %s' % (t, d.name, _lab(blob, o)))
                out.append((t, 'field', d, o[2], o[1]))
                t += 1
        elif mem.k == 'method':
            name = '<init>' if mem.ctor else mem.name
            if 'abstract' in mem.mods:
                rows = notree.get(name)
                if not rows:
                    raise AlignError('abstract method %s has no tree-less row' % name)
                st, row = rows.pop(0)
                out.append((None, 'method', mem, row, st))
                continue
            o = own.get(t)
            if o is None or o[0] != 'method' or blob.utf8(o[2].name) != name:
                raise AlignError('tree %d: source says method %s, class says %s'
                                 % (t, name, _lab(blob, o)))
            out.append((t, 'method', mem, o[2], o[1]))
            t += 1
        else:
            raise AlignError('member kind %s' % mem.k)
    if t != len(blob.trees):
        raise AlignError('source accounts for %d trees, class has %d'
                         % (t, len(blob.trees)))
    return out


def _lab(blob, o):
    if o is None:
        return 'nothing'
    return '%s %s' % (o[0], blob.utf8(o[2].name))
