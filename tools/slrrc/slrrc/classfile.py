"""classfile.py - a strict reader of class files.

    res = read(data)                    # ReadResult: ok, error, blobs
    info = class_info(res.blobs[0])     # ClassInfo, or None

The compiler writes class files (write.py) and never reads one.  Its host
does: the declarations of a class that exists as a class file reach the
compiler through a ClassProvider, and the provider of the command line
(install.py) gets them from here.  The layout is the one write.py writes
(notes/foundation.md 3): a flat stream of chunks {tag, size, payload}; per
class TUFA, CONS, [FILD], [MTHD], CLSS, TREE; little endian, no padding.

The reader is strict: every byte has to fit the layout, every chunk has to
be read to its last byte, and a tag, pool kind or op byte outside the known
set is an error.  A file that does not fit gives ok False and one line that
says where; nothing is guessed.  (The engine's own loader decodes the pool
heuristically; a compiler must not build on that.)

Bad input never raises: the reader unwinds with a private exception (_Bad)
that is raised where the layout does not fit and caught in read(), and
nowhere else.  The code behind a check relies on not being reached, as in
the parser (parser.py, "how the parser stops at an error"), and the port
keeps it: one exception type of the reader, thrown at the 23 places and
caught in read(), which returns ok false and the line.  Nothing here opens
a file.

BOUNDS.  Every length, count and chunk size is a u32 taken from the file.
No check adds such a number to a position: with 32-bit integers (the engine
is built for 32 bits) `pos + n > end` wraps for an n near 2**32 and passes.
A check compares the number with the bytes that are left, `n > end - pos`;
pos never exceeds end, so that difference cannot wrap.  A position is moved
on only behind such a check.

By default the nodes of the TREE chunk are skipped, since a provider needs
declarations only; the chunk is still checked for its node count and size
when with_trees is True, and its bytes are always inside the chunk bounds.

C++ (sc_classfile.cpp, part of the test tool and of the host's provider):
the same functions on a std::string_view.
"""
from . import classes
from . import pool as pl
from .decl import FieldRow, MethodRow

NONE = 0xFFFFFFFF

# Which op bytes are followed by a u32 (notes/opcodes.md; the 48-entry table
# of the VM, engine/include/tree_op_flags.inc: the high nibble of the 16-bit
# opcode is 1, 2 or 4).  The index is the op byte.
OP_HAS_OPERAND = (
    False, True, True, True, False, False, True, True,          # 0x00
    True, True, True, True, False, True, True, True,            # 0x08
    False, True, True, False, True, True, True, True,           # 0x10
    False, True, True, True, True, False, True, True,           # 0x18
    False, False, False, False, False, True, True, True,        # 0x20
    False, False, False, False, False, False, True, False,      # 0x28
)
N_OPS = 48


class BlobData(object):
    """One class of a class file, as its chunks hold it."""
    __slots__ = ('build', 'offset', 'size', 'pool', 'has_fild', 'has_mthd',
                 'static_fields', 'instance_fields', 'static_methods', 'instance_methods',
                 'interface', 'name', 'super', 'interfaces', 'tree_count', 'trees')

    def __init__(self):
        self.build = 0
        self.offset = 0             # where the blob starts in the file
        self.size = 0               # bytes it occupies
        self.pool = []              # pool.PoolEntry
        self.has_fild = False
        self.has_mthd = False
        self.static_fields = []     # decl.FieldRow
        self.instance_fields = []
        self.static_methods = []    # decl.MethodRow
        self.instance_methods = []
        self.interface = 0          # CLSS: 1 for an interface
        self.name = NONE            # CLSS: class entry
        self.super = NONE           # CLSS: class entry, or NONE
        self.interfaces = []        # CLSS: class entries
        self.tree_count = 0
        self.trees = None           # with_trees: [[(op, line, operand or NONE)]]


class ReadResult(object):
    __slots__ = ('ok', 'error', 'blobs')

    def __init__(self):
        self.ok = False
        self.error = ''             # one line: what does not fit, and where
        self.blobs = []


class _Bad(Exception):
    """Private: unwinds the reader to read()."""


class _Cursor(object):
    """A bounded little-endian reader over one chunk payload: the bytes
    data[pos:end], with pos <= end at all times."""
    __slots__ = ('data', 'pos', 'end', 'what')

    def __init__(self, data, pos, end, what):
        self.data = data
        self.pos = pos
        self.end = end
        self.what = what

    def left(self):
        """The number of bytes not yet read (never negative)."""
        return self.end - self.pos

    def u32(self):
        p = self.pos
        if self.end - p < 4:
            raise _Bad('%s: a u32 at 0x%X runs past the end of the chunk' % (self.what, p))
        d = self.data
        self.pos = p + 4
        return d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | (d[p + 3] << 24)

    def take(self, n):
        """The next n bytes; n comes from the file and may be any u32."""
        p = self.pos
        if n > self.end - p:
            raise _Bad('%s: %d bytes at 0x%X run past the end of the chunk' % (self.what, n, p))
        self.pos = p + n
        return self.data[p:p + n]

    def done(self):
        if self.pos != self.end:
            raise _Bad('%s: %d unread bytes at 0x%X' % (self.what, self.end - self.pos,
                                                        self.pos))


def _read_cons(c):
    n = c.u32()
    out = []
    i = 0
    while i < n:
        kind = c.u32()
        if kind == pl.UTF8:
            length = c.u32()
            raw = c.take(length)
            if c.take(1) != b'\x00':
                raise _Bad('%s: utf8 entry %d is not closed by a NUL' % (c.what, i))
            out.append(pl.PoolEntry(pl.UTF8, 0, 0, raw.decode('latin-1')))
        elif kind == pl.CLASS:
            out.append(pl.PoolEntry(pl.CLASS, c.u32(), 0, ''))
        elif kind == pl.RID or kind == pl.REF or kind == pl.NAT:
            a = c.u32()
            b = c.u32()
            out.append(pl.PoolEntry(kind, a, b, ''))
        else:
            raise _Bad('%s: entry %d has the unknown kind %d' % (c.what, i, kind))
        i += 1
    c.done()
    return out


def _read_field_rows(c):
    n = c.u32()
    if n > c.left() // 16:
        raise _Bad('%s: %d rows of 16 bytes run past the end of the chunk' % (c.what, n))
    out = []
    i = 0
    while i < n:
        flags = c.u32()
        name = c.u32()
        type_ = c.u32()
        tree = c.u32()
        out.append(FieldRow(flags, name, type_, tree))
        i += 1
    return out


def _read_method_rows(c):
    n = c.u32()
    if n > c.left() // 20:
        raise _Bad('%s: %d rows of 20 bytes run past the end of the chunk' % (c.what, n))
    out = []
    i = 0
    while i < n:
        flags = c.u32()
        name = c.u32()
        desc = c.u32()
        tree = c.u32()
        local_slots = c.u32()
        out.append(MethodRow(flags, name, desc, tree, local_slots))
        i += 1
    return out


def _read_clss(c, blob):
    marker = c.u32()
    if marker != NONE:
        raise _Bad('%s: the first word is 0x%X, not 0xFFFFFFFF' % (c.what, marker))
    blob.interface = c.u32()
    if blob.interface > 1:
        raise _Bad('%s: interface flag %d' % (c.what, blob.interface))
    blob.name = c.u32()
    blob.super = c.u32()
    n = c.u32()
    if n > c.left() // 4:
        raise _Bad('%s: %d interfaces run past the end of the chunk' % (c.what, n))
    i = 0
    while i < n:
        blob.interfaces.append(c.u32())
        i += 1
    c.done()


def _read_tree(c, blob, with_trees):
    blob.tree_count = c.u32()
    if not with_trees:
        return
    data = c.data
    trees = []
    t = 0
    while t < blob.tree_count:
        count = c.u32()
        nodes = []
        p = c.pos
        end = c.end
        k = 0
        while k < count:
            if end - p < 3:
                raise _Bad('%s: tree %d node %d runs past the end of the chunk'
                           % (c.what, t, k))
            op = data[p]
            if op >= N_OPS:
                raise _Bad('%s: tree %d node %d has the op byte %d' % (c.what, t, k, op))
            line = data[p + 1] | (data[p + 2] << 8)
            if OP_HAS_OPERAND[op]:
                if end - p < 7:
                    raise _Bad('%s: tree %d node %d runs past the end of the chunk'
                               % (c.what, t, k))
                nodes.append((op, line, data[p + 3] | (data[p + 4] << 8) |
                              (data[p + 5] << 16) | (data[p + 6] << 24)))
                p += 7
            else:
                nodes.append((op, line, NONE))
                p += 3
            k += 1
        c.pos = p
        trees.append(nodes)
        t += 1
    c.done()
    blob.trees = trees


def _read(data, with_trees, res):
    size = len(data)
    if size == 0:
        raise _Bad('empty file')
    pos = 0
    while pos < size:
        start = pos
        if size - pos < 12:
            raise _Bad('%d trailing bytes at 0x%X, too short for a TUFA chunk'
                       % (size - pos, pos))
        if data[pos:pos + 4] != b'TUFA':
            raise _Bad('no TUFA tag at 0x%X' % pos)
        head = _Cursor(data, pos + 4, pos + 12, 'TUFA at 0x%X' % pos)
        if head.u32() != 4:
            raise _Bad('the TUFA chunk at 0x%X does not have size 4' % pos)
        blob = BlobData()
        blob.offset = start
        blob.build = head.u32()
        pos += 12
        seen = []
        while True:
            if size - pos < 8:
                raise _Bad('the class at 0x%X ends at 0x%X without a TREE chunk'
                           % (start, pos))
            tag = data[pos:pos + 4]
            c = _Cursor(data, pos + 4, pos + 8, 'chunk at 0x%X' % pos)
            csize = c.u32()
            begin = pos + 8
            if csize > size - begin:
                raise _Bad('the chunk at 0x%X (size %d) runs past the end of the file'
                           % (pos, csize))
            end = begin + csize
            if tag in seen:
                raise _Bad('second %s chunk at 0x%X' % (tag.decode('latin-1'), pos))
            if not seen and tag != b'CONS':
                raise _Bad('the class at 0x%X does not start with CONS' % start)
            c = _Cursor(data, begin, end, '%s at 0x%X' % (tag.decode('latin-1'), pos))
            if tag == b'CONS':
                blob.pool = _read_cons(c)
            elif tag == b'FILD':
                blob.has_fild = True
                blob.static_fields = _read_field_rows(c)
                blob.instance_fields = _read_field_rows(c)
                c.done()
            elif tag == b'MTHD':
                blob.has_mthd = True
                blob.static_methods = _read_method_rows(c)
                blob.instance_methods = _read_method_rows(c)
                c.done()
            elif tag == b'CLSS':
                _read_clss(c, blob)
            elif tag == b'TREE':
                _read_tree(c, blob, with_trees)
            else:
                raise _Bad('unknown chunk tag %r at 0x%X' % (tag, pos))
            seen.append(tag)
            pos = end
            if tag == b'TREE':
                break
        if b'CLSS' not in seen:
            raise _Bad('the class at 0x%X has no CLSS chunk' % start)
        blob.size = pos - start
        res.blobs.append(blob)


def read(data, with_trees=False):
    """Read the bytes of a class file: a ReadResult with one BlobData per
    class.  with_trees: also decode the nodes (blob.trees)."""
    res = ReadResult()
    try:
        _read(bytes(data), with_trees, res)
        res.ok = True
    except _Bad as x:
        res.error = str(x)
        res.blobs = []
    return res


# ---------------------------------------------------------------------------
# what a class file declares
# ---------------------------------------------------------------------------

def _utf8(blob, i):
    """The text of utf8 entry i, None when i is no utf8 entry."""
    if i < len(blob.pool) and blob.pool[i].kind == pl.UTF8:
        return blob.pool[i].text
    return None


def _class_name(blob, i):
    """The name behind class entry i, None when i is no class entry."""
    if i < len(blob.pool) and blob.pool[i].kind == pl.CLASS:
        return _utf8(blob, blob.pool[i].a)
    return None


def class_info(blob):
    """The ClassInfo of one class of a class file, or None when a row or the
    CLSS record points at an entry of the wrong kind.

    Fields: the static vector, then the instance vector (each in source
    order).  Methods: in source order, which is the order of their tree
    numbers; a row without a tree (abstract) stands behind the others in row
    order.  The lookup of classes.py scans methods in an order derived from
    this one."""
    name = _class_name(blob, blob.name)
    if name is None:
        return None
    info = classes.ClassInfo(name)
    info.is_interface = blob.interface != 0
    if blob.super != NONE:
        info.super = _class_name(blob, blob.super)
        if info.super is None:
            return None
    for i in blob.interfaces:
        n = _class_name(blob, i)
        if n is None:
            return None
        info.interfaces.append(n)
    for rows, is_static in ((blob.static_fields, True), (blob.instance_fields, False)):
        for r in rows:
            n = _utf8(blob, r.name)
            d = _utf8(blob, r.type)
            if n is None or d is None:
                return None
            info.fields.append(classes.FieldInfo(n, d, is_static))
    with_tree = []                      # (tree number, MethodInfo)
    without = []
    for rows, is_static in ((blob.static_methods, True), (blob.instance_methods, False)):
        for r in rows:
            n = _utf8(blob, r.name)
            d = _utf8(blob, r.desc)
            if n is None or d is None:
                return None
            m = classes.MethodInfo(n, d, is_static)
            if r.tree == NONE:
                without.append(m)
            else:
                with_tree.append((r.tree, len(with_tree), m))
    with_tree.sort(key=_tree_key)
    for x in with_tree:
        info.methods.append(x[2])
    info.methods.extend(without)
    return info


def _tree_key(x):
    return (x[0], x[1])
