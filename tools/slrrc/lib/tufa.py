#!/usr/bin/env python3
"""tufa.py - strict reader and writer for the SLRR script class format ("TUFA").

A .class file is a flat stream of chunks {tag: 4 bytes, size: u32 LE, payload}.
The exe's loader (JVM_addClass_fromChunks, Build 940 VA 0x554EE0) walks that
stream to the end of the file: a CONS chunk opens a class, a TREE chunk closes
it, and the 12-byte "header" of a class is itself a chunk: tag "TUFA", size 4,
payload = the build number of the compiler that wrote it.  One file therefore
holds one "blob" per top-level class of the source, back to back, no padding.

    blob  := TUFA CONS [FILD] [MTHD] CLSS TREE          (always this order;
             FILD only when the class has a field, MTHD only when it has a
             method: canonical_chunks())
    TUFA  := u32 build
    CONS  := u32 count, count * entry
             entry := u32 kind, payload
               kind 0  utf8   u32 len, len bytes, one NUL  (len excludes NUL)
               kind 3  rid    u32 utf8 index of the pack file name, u32 local id
               kind 4  class  u32 utf8 index (dotted class name)
               kind 5  ref    u32 class entry, u32 nat entry  (field or method)
               kind 7  nat    u32 utf8 name, u32 utf8 descriptor/type
    FILD  := u32 n, n * field ; u32 m, m * field     (statics, then instance)
             field := u32 flags, u32 name utf8, u32 type utf8, u32 init tree
                      (0xFFFFFFFF = no initialiser)                 16 bytes
    MTHD  := u32 n, n * method ; u32 m, m * method   (statics, then instance)
             method := u32 flags, u32 name utf8, u32 descriptor utf8,
                       u32 tree (0xFFFFFFFF = abstract), u32 locals 20 bytes
                       locals = number of local variable slots the body
                       uses (this + parameters + declared locals), 0 for an
                       empty or absent body
             flags (FILD and MTHD): 0x1 public, 0x2 protected, 0x4 private,
                       0x8 static, 0x10 abstract, 0x20 final, 0x40 native,
                       0x80 synchronized
    CLSS  := u32 0xFFFFFFFF, u32 is_interface, u32 name (class entry),
             u32 super (class entry or 0xFFFFFFFF), u32 n, n * u32 (class entry)
    TREE  := u32 tree count, per tree: u32 node count, nodes
             node := u8 op, u16 line [, u32 imm]
             the node is 7 bytes when OP_FLAGS[op] >> 12 is 1, 2 or 4, else 3;
             the u16 ("slot" in ENGINE.md) is the source line number.

All integers are little endian.  Strings are kept as Python str decoded with
latin-1, which is a byte-for-byte bijection: s.encode('latin-1') gives back the
file bytes.  (Every pool string of the corpus is 7-bit and every source is
ASCII, so this is plain text in practice; see notes/foundation.md.)

Everything unexpected raises TufaError; nothing is skipped or guessed.

Structures returned by parse_file():

    [Blob, ...]
    Blob.build              int   (0x24F9D for Build 940)
    Blob.chunks             list of chunk tags in file order (without TUFA)
    Blob.cons               list of entries, each a tuple:
                              ('utf8', str) ('rid', utf8_index, local_id)
                              ('class', utf8_index) ('ref', class_index, nat_index)
                              ('nat', name_index, desc_index)
    Blob.static_fields      list of Field(flags, name, type, init)
    Blob.instance_fields    list of Field
    Blob.static_methods     list of Method(flags, name, desc, tree, locals)
    Blob.instance_methods   list of Method
    Blob.clss               Clss(marker, interface, name, super, interfaces)
    Blob.trees              list of trees; a tree is a list of
                              Node(op, slot, imm)   imm is None on 3-byte nodes;
                              .line is an alias of .slot, .code = OP_FLAGS[op]

Command line:  python -I tufa.py FILE.class ...      (round-trip check)
"""
import struct
import sys
from collections import namedtuple

MAGIC = b'TUFA'
BUILD_940 = 0x24F9D
NONE = 0xFFFFFFFF

# engine/include/tree_op_flags.inc (old exe VA 0x5F0914; Build 940 VA 0x5B5260,
# byte-identical).  Index = the op byte on disk, value = the 16-bit opcode the
# VM dispatches on (TREE_readNode stores OP_FLAGS[op] into the 8-byte runtime
# record).  The high nibble is the immediate class: 0 none, 1 u32, 2 float,
# 4 utf8 index.  Entry 47 is 0 and no op byte >= 47 occurs in the corpus.
OP_FLAGS = (
    0x0000, 0x1001, 0x1002, 0x1003, 0x0004, 0x0005, 0x1006, 0x1007,
    0x1008, 0x4009, 0x200a, 0x100b, 0x000c, 0x100d, 0x100e, 0x100f,
    0x0010, 0x1011, 0x4012, 0x0013, 0x1014, 0x1015, 0x1016, 0x1017,
    0x0018, 0x1019, 0x101a, 0x101b, 0x401c, 0x001d, 0x101e, 0x101f,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x4025, 0x4026, 0x1027,
    0x0028, 0x0029, 0x002a, 0x002b, 0x002c, 0x002d, 0x402e, 0x0000,
)
N_OPS = len(OP_FLAGS)                       # 48
HAS_IMM = tuple(((f >> 12) & 0xF) in (1, 2, 4) for f in OP_FLAGS)
NODE_SIZE = tuple(7 if h else 3 for h in HAS_IMM)

# access mask shared by FILD and MTHD rows
ACC_PUBLIC, ACC_PROTECTED, ACC_PRIVATE = 0x1, 0x2, 0x4
ACC_STATIC, ACC_ABSTRACT, ACC_FINAL, ACC_NATIVE = 0x8, 0x10, 0x20, 0x40
ACC_SYNCHRONIZED = 0x80
ACC_NAMES = ((0x1, 'public'), (0x2, 'protected'), (0x4, 'private'),
             (0x8, 'static'), (0x10, 'abstract'), (0x20, 'final'),
             (0x40, 'native'), (0x80, 'synchronized'))

CONS_KIND = {0: 'utf8', 3: 'rid', 4: 'class', 5: 'ref', 7: 'nat'}
CONS_CODE = {v: k for k, v in CONS_KIND.items()}

CHUNK_TAGS = ('CONS', 'FILD', 'MTHD', 'CLSS', 'TREE')

Field = namedtuple('Field', 'flags name type init')
Method = namedtuple('Method', 'flags name desc tree locals')
Clss = namedtuple('Clss', 'marker interface name super interfaces')


class Node(namedtuple('Node', 'op slot imm')):
    """One TREE node.  op is the byte on disk (index into OP_FLAGS), slot the
    u16 that follows it and imm the u32 immediate (None on 3-byte nodes).
    ENGINE.md calls the u16 "slot"; it is the source line the node was
    generated from, hence the alias .line; .code is the 16-bit opcode."""
    __slots__ = ()

    @property
    def line(self):
        return self.slot

    @property
    def code(self):
        return OP_FLAGS[self.op]


_U32 = struct.Struct('<I')
_NODE7 = struct.Struct('<BHI')
_NODE3 = struct.Struct('<BH')
_F16 = struct.Struct('<4I')
_M20 = struct.Struct('<5I')


class TufaError(ValueError):
    """Raised for every byte that does not fit the documented layout."""


class Blob(object):
    __slots__ = ('build', 'chunks', 'cons', 'static_fields', 'instance_fields',
                 'static_methods', 'instance_methods', 'clss', 'trees',
                 'offset', 'size', 'chunk_spans')

    def __init__(self):
        self.build = BUILD_940
        self.chunks = ['CONS', 'FILD', 'MTHD', 'CLSS', 'TREE']
        self.cons = []
        self.static_fields = []
        self.instance_fields = []
        self.static_methods = []
        self.instance_methods = []
        self.clss = Clss(NONE, 0, NONE, NONE, [])
        self.trees = []
        self.offset = 0          # where the blob started in its file (reader)
        self.size = 0            # bytes it occupied (reader)
        self.chunk_spans = {}    # tag -> (payload offset in file, payload size)

    # ---- pool helpers ---------------------------------------------------
    def utf8(self, i):
        """Text of utf8 entry i (raises unless it is one)."""
        e = self._entry(i)
        if e[0] != 'utf8':
            raise TufaError('pool entry %d is %s, not utf8' % (i, e[0]))
        return e[1]

    def _entry(self, i):
        if not 0 <= i < len(self.cons):
            raise TufaError('pool index %d out of range (%d entries)'
                            % (i, len(self.cons)))
        return self.cons[i]

    def class_name(self, i):
        """Dotted name behind class entry i."""
        e = self._entry(i)
        if e[0] != 'class':
            raise TufaError('pool entry %d is %s, not class' % (i, e[0]))
        return self.utf8(e[1])

    def nat(self, i):
        """(name, descriptor) behind nat entry i."""
        e = self._entry(i)
        if e[0] != 'nat':
            raise TufaError('pool entry %d is %s, not nat' % (i, e[0]))
        return self.utf8(e[1]), self.utf8(e[2])

    def ref(self, i):
        """(class name, member name, descriptor) behind ref entry i."""
        e = self._entry(i)
        if e[0] != 'ref':
            raise TufaError('pool entry %d is %s, not ref' % (i, e[0]))
        n, d = self.nat(e[2])
        return self.class_name(e[1]), n, d

    def rid(self, i):
        """(pack file name, local id) behind rid entry i."""
        e = self._entry(i)
        if e[0] != 'rid':
            raise TufaError('pool entry %d is %s, not rid' % (i, e[0]))
        return self.utf8(e[1]), e[2]

    def describe(self, i):
        """Readable rendering of any pool entry (never raises on bad links)."""
        if i == NONE:
            return '-'
        if not 0 <= i < len(self.cons):
            return '<bad index %d>' % i
        e = self.cons[i]
        try:
            if e[0] == 'utf8':
                return repr_text(e[1])
            if e[0] == 'class':
                return self.utf8(e[1])
            if e[0] == 'nat':
                return '%s:%s' % self.nat(i)
            if e[0] == 'ref':
                c, n, d = self.ref(i)
                return '%s.%s:%s' % (c, n, d)
            if e[0] == 'rid':
                p, l = self.rid(i)
                return '%s:0x%08X' % (p, l)
        except TufaError as x:
            return '<%s>' % x
        return repr(e)

    # ---- class helpers --------------------------------------------------
    @property
    def name(self):
        return self.class_name(self.clss.name)

    @property
    def super_name(self):
        return None if self.clss.super == NONE else self.class_name(self.clss.super)

    @property
    def interface_names(self):
        return [self.class_name(i) for i in self.clss.interfaces]

    @property
    def is_interface(self):
        return self.clss.interface != 0

    def methods(self):
        """All method rows, statics first: yields (is_static, index, Method)."""
        for i, m in enumerate(self.static_methods):
            yield True, i, m
        for i, m in enumerate(self.instance_methods):
            yield False, i, m

    def fields(self):
        """All field rows, statics first: yields (is_static, index, Field)."""
        for i, f in enumerate(self.static_fields):
            yield True, i, f
        for i, f in enumerate(self.instance_fields):
            yield False, i, f

    def tree_owners(self):
        """tree index -> list of labels of the rows that point at it."""
        own = {}
        for st, i, f in self.fields():
            if f.init != NONE:
                own.setdefault(f.init, []).append(
                    '%sfield %s' % ('static ' if st else '', self.describe(f.name)))
        for st, i, m in self.methods():
            if m.tree != NONE:
                own.setdefault(m.tree, []).append(
                    '%smethod %s%s' % ('static ' if st else '',
                                       _plain(self, m.name), _plain(self, m.desc)))
        return own

    def to_dict(self):
        """JSON-ready plain dict/list rendering (lossless)."""
        return {
            'build': self.build,
            'chunks': list(self.chunks),
            'cons': [list(e) for e in self.cons],
            'static_fields': [list(f) for f in self.static_fields],
            'instance_fields': [list(f) for f in self.instance_fields],
            'static_methods': [list(m) for m in self.static_methods],
            'instance_methods': [list(m) for m in self.instance_methods],
            'clss': {'marker': self.clss.marker, 'interface': self.clss.interface,
                     'name': self.clss.name, 'super': self.clss.super,
                     'interfaces': list(self.clss.interfaces)},
            'trees': [[list(n) for n in t] for t in self.trees],
        }

    @classmethod
    def from_dict(cls, d):
        b = cls()
        b.build = d['build']
        b.chunks = list(d['chunks'])
        b.cons = [tuple(e) for e in d['cons']]
        b.static_fields = [Field(*f) for f in d['static_fields']]
        b.instance_fields = [Field(*f) for f in d['instance_fields']]
        b.static_methods = [Method(*m) for m in d['static_methods']]
        b.instance_methods = [Method(*m) for m in d['instance_methods']]
        c = d['clss']
        b.clss = Clss(c['marker'], c['interface'], c['name'], c['super'],
                      list(c['interfaces']))
        b.trees = [[Node(*n) for n in t] for t in d['trees']]
        return b


def _plain(blob, i):
    try:
        return blob.utf8(i)
    except TufaError:
        return '<%d>' % i


def repr_text(s):
    """Stable, ASCII-only quoted rendering of a pool string."""
    out = ['"']
    for ch in s:
        o = ord(ch)
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\r':
            out.append('\\r')
        elif ch == '\t':
            out.append('\\t')
        elif 32 <= o < 127:
            out.append(ch)
        else:
            out.append('\\x%02x' % o)
    out.append('"')
    return ''.join(out)


def flags_text(flags):
    names = [n for bit, n in ACC_NAMES if flags & bit]
    rest = flags & ~0xFF
    if rest:
        names.append('0x%X' % rest)
    return ' '.join(names) if names else '-'


def node_size(op):
    if not 0 <= op < N_OPS:
        raise TufaError('op byte %d outside the 48-entry flags table' % op)
    return NODE_SIZE[op]


def opcode(op):
    """The 16-bit opcode the VM dispatches on for a disk op byte."""
    if not 0 <= op < N_OPS:
        raise TufaError('op byte %d outside the 48-entry flags table' % op)
    return OP_FLAGS[op]


# ---------------------------------------------------------------------------
# reader
# ---------------------------------------------------------------------------

class _Cur(object):
    """Bounded little-endian cursor over one chunk payload."""
    __slots__ = ('b', 'p', 'end', 'what')

    def __init__(self, b, start, end, what):
        self.b, self.p, self.end, self.what = b, start, end, what

    def u32(self):
        if self.p + 4 > self.end:
            raise TufaError('%s: u32 at 0x%X runs past the chunk end 0x%X'
                            % (self.what, self.p, self.end))
        v = _U32.unpack_from(self.b, self.p)[0]
        self.p += 4
        return v

    def take(self, n):
        if n < 0 or self.p + n > self.end:
            raise TufaError('%s: %d bytes at 0x%X run past the chunk end 0x%X'
                            % (self.what, n, self.p, self.end))
        v = self.b[self.p:self.p + n]
        self.p += n
        return v

    def done(self):
        if self.p != self.end:
            raise TufaError('%s: %d unread bytes at 0x%X (chunk ends at 0x%X)'
                            % (self.what, self.end - self.p, self.p, self.end))


def _read_cons(c):
    n = c.u32()
    out = []
    for i in range(n):
        kind = c.u32()
        if kind == 0:
            ln = c.u32()
            raw = c.take(ln)
            nul = c.take(1)
            if nul != b'\x00':
                raise TufaError('%s: utf8 entry %d is not NUL terminated '
                                '(0x%02X at 0x%X)' % (c.what, i, nul[0], c.p - 1))
            out.append(('utf8', raw.decode('latin-1')))
        elif kind == 3:
            a = c.u32()
            out.append(('rid', a, c.u32()))
        elif kind == 4:
            out.append(('class', c.u32()))
        elif kind == 5:
            a = c.u32()
            out.append(('ref', a, c.u32()))
        elif kind == 7:
            a = c.u32()
            out.append(('nat', a, c.u32()))
        else:
            raise TufaError('%s: entry %d has unknown kind %d at 0x%X'
                            % (c.what, i, kind, c.p - 4))
    c.done()
    return out


def _read_rows(c, st, make):
    n = c.u32()
    raw = c.take(n * st.size)
    return [make(*st.unpack_from(raw, k * st.size)) for k in range(n)]


def _read_fild(c):
    s = _read_rows(c, _F16, Field)
    i = _read_rows(c, _F16, Field)
    c.done()
    return s, i


def _read_mthd(c):
    s = _read_rows(c, _M20, Method)
    i = _read_rows(c, _M20, Method)
    c.done()
    return s, i


def _read_clss(c):
    marker = c.u32()
    iface = c.u32()
    name = c.u32()
    sup = c.u32()
    n = c.u32()
    ifs = [c.u32() for _ in range(n)]
    c.done()
    return Clss(marker, iface, name, sup, ifs)


def _read_tree(c):
    b = c.b
    ntrees = c.u32()
    trees = []
    for t in range(ntrees):
        nn = c.u32()
        nodes = []
        p, end = c.p, c.end
        for k in range(nn):
            if p >= end:
                raise TufaError('%s: tree %d node %d starts at the chunk end'
                                % (c.what, t, k))
            op = b[p]
            if op >= N_OPS:
                raise TufaError('%s: tree %d node %d has op byte %d at 0x%X, '
                                'outside the 48-entry flags table'
                                % (c.what, t, k, op, p))
            if HAS_IMM[op]:
                if p + 7 > end:
                    raise TufaError('%s: tree %d node %d runs past the chunk end'
                                    % (c.what, t, k))
                nodes.append(Node(*_NODE7.unpack_from(b, p)))
                p += 7
            else:
                if p + 3 > end:
                    raise TufaError('%s: tree %d node %d runs past the chunk end'
                                    % (c.what, t, k))
                o, s = _NODE3.unpack_from(b, p)
                nodes.append(Node(o, s, None))
                p += 3
        c.p = p
        trees.append(nodes)
    c.done()
    return trees


def parse(data, name='<bytes>'):
    """Parse the bytes of one .class file into a list of Blob."""
    data = bytes(data)
    blobs = []
    pos = 0
    size = len(data)
    if size == 0:
        raise TufaError('%s: empty file' % name)
    while pos < size:
        start = pos
        if pos + 12 > size:
            raise TufaError('%s: %d trailing bytes at 0x%X, too short for a '
                            'TUFA chunk' % (name, size - pos, pos))
        if data[pos:pos + 4] != MAGIC:
            raise TufaError('%s: expected TUFA at 0x%X, found %r'
                            % (name, pos, data[pos:pos + 4]))
        hsize = _U32.unpack_from(data, pos + 4)[0]
        if hsize != 4:
            raise TufaError('%s: TUFA chunk size %d at 0x%X (expected 4)'
                            % (name, hsize, pos + 4))
        blob = Blob()
        blob.offset = start
        blob.build = _U32.unpack_from(data, pos + 8)[0]
        blob.chunks = []
        pos += 12
        seen = {}
        while True:
            if pos + 8 > size:
                raise TufaError('%s: blob at 0x%X ends at 0x%X without a TREE '
                                'chunk' % (name, start, pos))
            tag_b = data[pos:pos + 4]
            try:
                tag = tag_b.decode('ascii')
            except UnicodeDecodeError:
                tag = None
            if tag not in CHUNK_TAGS:
                raise TufaError('%s: unknown chunk tag %r at 0x%X'
                                % (name, tag_b, pos))
            if tag in seen:
                raise TufaError('%s: second %s chunk at 0x%X in the blob at 0x%X'
                                % (name, tag, pos, start))
            csize = _U32.unpack_from(data, pos + 4)[0]
            p0 = pos + 8
            p1 = p0 + csize
            if p1 > size:
                raise TufaError('%s: %s chunk at 0x%X (size %d) runs past the '
                                'end of the file' % (name, tag, pos, csize))
            if not blob.chunks and tag != 'CONS':
                raise TufaError('%s: blob at 0x%X starts with %s, not CONS'
                                % (name, start, tag))
            cur = _Cur(data, p0, p1, '%s: %s@0x%X' % (name, tag, pos))
            if tag == 'CONS':
                blob.cons = _read_cons(cur)
            elif tag == 'FILD':
                blob.static_fields, blob.instance_fields = _read_fild(cur)
            elif tag == 'MTHD':
                blob.static_methods, blob.instance_methods = _read_mthd(cur)
            elif tag == 'CLSS':
                blob.clss = _read_clss(cur)
            else:
                blob.trees = _read_tree(cur)
            seen[tag] = True
            blob.chunks.append(tag)
            blob.chunk_spans[tag] = (p0, csize)
            pos = p1
            if tag == 'TREE':
                break
        if 'CLSS' not in seen:
            raise TufaError('%s: blob at 0x%X has no CLSS chunk' % (name, start))
        blob.size = pos - start
        blobs.append(blob)
    return blobs


def parse_file(path):
    with open(path, 'rb') as f:
        return parse(f.read(), str(path))


# ---------------------------------------------------------------------------
# writer
# ---------------------------------------------------------------------------

def _w_cons(blob):
    out = [_U32.pack(len(blob.cons))]
    for i, e in enumerate(blob.cons):
        k = e[0]
        if k == 'utf8':
            raw = e[1].encode('latin-1')
            out.append(struct.pack('<II', 0, len(raw)))
            out.append(raw)
            out.append(b'\x00')
        elif k in ('rid', 'ref', 'nat'):
            out.append(struct.pack('<III', CONS_CODE[k], e[1], e[2]))
        elif k == 'class':
            out.append(struct.pack('<II', 4, e[1]))
        else:
            raise TufaError('pool entry %d has unknown kind %r' % (i, k))
    return b''.join(out)


def _w_rows(rows, st):
    return _U32.pack(len(rows)) + b''.join(st.pack(*r) for r in rows)


def _w_clss(c):
    return struct.pack('<5I', c.marker, c.interface, c.name, c.super,
                       len(c.interfaces)) + b''.join(_U32.pack(i) for i in c.interfaces)


def _w_tree(trees):
    out = [_U32.pack(len(trees))]
    for t, nodes in enumerate(trees):
        out.append(_U32.pack(len(nodes)))
        for k, n in enumerate(nodes):
            op, slot, imm = n
            if not 0 <= op < N_OPS:
                raise TufaError('tree %d node %d: op byte %d outside the flags '
                                'table' % (t, k, op))
            if HAS_IMM[op]:
                if imm is None:
                    raise TufaError('tree %d node %d: op 0x%02X needs an '
                                    'immediate' % (t, k, op))
                out.append(_NODE7.pack(op, slot, imm & 0xFFFFFFFF))
            else:
                if imm is not None:
                    raise TufaError('tree %d node %d: op 0x%02X takes no '
                                    'immediate' % (t, k, op))
                out.append(_NODE3.pack(op, slot))
    return b''.join(out)


def chunk_payload(blob, tag):
    """Serialised payload of one chunk of a blob."""
    if tag == 'CONS':
        return _w_cons(blob)
    if tag == 'FILD':
        return _w_rows(blob.static_fields, _F16) + _w_rows(blob.instance_fields, _F16)
    if tag == 'MTHD':
        return _w_rows(blob.static_methods, _M20) + _w_rows(blob.instance_methods, _M20)
    if tag == 'CLSS':
        return _w_clss(blob.clss)
    if tag == 'TREE':
        return _w_tree(blob.trees)
    raise TufaError('unknown chunk tag %r' % (tag,))


def canonical_chunks(blob):
    """The chunk list the compiler writes for this content: FILD only when the
    class has a field, MTHD only when it has a method (see notes)."""
    out = ['CONS']
    if blob.static_fields or blob.instance_fields:
        out.append('FILD')
    if blob.static_methods or blob.instance_methods:
        out.append('MTHD')
    out += ['CLSS', 'TREE']
    return out


def serialise_blob(blob, chunks=None):
    """Bytes of one blob.  chunks defaults to blob.chunks (what the reader saw);
    pass canonical_chunks(blob) for a freshly built class."""
    tags = list(blob.chunks if chunks is None else chunks)
    if not tags or tags[0] != 'CONS' or tags[-1] != 'TREE' or 'CLSS' not in tags:
        raise TufaError('chunk list %r must start with CONS, hold CLSS and end '
                        'with TREE' % (tags,))
    if len(set(tags)) != len(tags):
        raise TufaError('chunk list %r repeats a tag' % (tags,))
    if 'FILD' not in tags and (blob.static_fields or blob.instance_fields):
        raise TufaError('fields present but no FILD chunk in %r' % (tags,))
    if 'MTHD' not in tags and (blob.static_methods or blob.instance_methods):
        raise TufaError('methods present but no MTHD chunk in %r' % (tags,))
    out = [MAGIC, struct.pack('<II', 4, blob.build)]
    for tag in tags:
        p = chunk_payload(blob, tag)
        out.append(tag.encode('ascii'))
        out.append(_U32.pack(len(p)))
        out.append(p)
    return b''.join(out)


def serialise(blobs):
    """Bytes of a .class file holding the given blobs."""
    return b''.join(serialise_blob(b) for b in blobs)


def write_file(path, blobs):
    with open(path, 'wb') as f:
        f.write(serialise(blobs))


# ---------------------------------------------------------------------------
# validation (cross references; separate from the layout so that the reader
# stays a pure layout check)
# ---------------------------------------------------------------------------

def validate(blob):
    """Return a list of problem strings (empty when every cross reference of
    the blob resolves to an entry of the expected kind)."""
    bad = []
    cons = blob.cons
    n = len(cons)

    def kind(i):
        return cons[i][0] if 0 <= i < n else None

    for i, e in enumerate(cons):
        k = e[0]
        if k == 'rid' and kind(e[1]) != 'utf8':
            bad.append('cons[%d] rid: pack %d is not utf8' % (i, e[1]))
        elif k == 'class' and kind(e[1]) != 'utf8':
            bad.append('cons[%d] class: %d is not utf8' % (i, e[1]))
        elif k == 'ref':
            if kind(e[1]) != 'class':
                bad.append('cons[%d] ref: %d is not class' % (i, e[1]))
            if kind(e[2]) != 'nat':
                bad.append('cons[%d] ref: %d is not nat' % (i, e[2]))
        elif k == 'nat':
            if kind(e[1]) != 'utf8':
                bad.append('cons[%d] nat: name %d is not utf8' % (i, e[1]))
            if kind(e[2]) != 'utf8':
                bad.append('cons[%d] nat: descriptor %d is not utf8' % (i, e[2]))
    nt = len(blob.trees)
    for st, i, f in blob.fields():
        w = 'static' if st else 'instance'
        if kind(f.name) != 'utf8':
            bad.append('%s field %d: name %d is not utf8' % (w, i, f.name))
        if kind(f.type) != 'utf8':
            bad.append('%s field %d: type %d is not utf8' % (w, i, f.type))
        if f.init != NONE and not f.init < nt:
            bad.append('%s field %d: init tree %d out of range' % (w, i, f.init))
        if bool(f.flags & ACC_STATIC) != st:
            bad.append('%s field %d: flags 0x%X disagree with the vector'
                       % (w, i, f.flags))
    for st, i, m in blob.methods():
        w = 'static' if st else 'instance'
        if kind(m.name) != 'utf8':
            bad.append('%s method %d: name %d is not utf8' % (w, i, m.name))
        if kind(m.desc) != 'utf8':
            bad.append('%s method %d: descriptor %d is not utf8' % (w, i, m.desc))
        if m.tree != NONE and not m.tree < nt:
            bad.append('%s method %d: tree %d out of range' % (w, i, m.tree))
        if bool(m.flags & ACC_STATIC) != st:
            bad.append('%s method %d: flags 0x%X disagree with the vector'
                       % (w, i, m.flags))
    c = blob.clss
    if c.marker != NONE:
        bad.append('CLSS marker is 0x%X' % c.marker)
    if c.interface not in (0, 1):
        bad.append('CLSS interface flag is %d' % c.interface)
    if kind(c.name) != 'class':
        bad.append('CLSS name %d is not class' % c.name)
    if c.super != NONE and kind(c.super) != 'class':
        bad.append('CLSS super %d is not class' % c.super)
    for j in c.interfaces:
        if kind(j) != 'class':
            bad.append('CLSS interface %d is not class' % j)
    return bad


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    rc = 0
    for path in argv:
        try:
            with open(path, 'rb') as f:
                data = f.read()
            blobs = parse(data, path)
            ok = serialise(blobs) == data
            probs = sum(len(validate(b)) for b in blobs)
            print('%s  %s  blobs=%d  %s  validate=%d problems' % (
                'OK ' if ok else 'DIFF', path, len(blobs),
                ','.join('0x%X' % b.build for b in blobs), probs))
            if not ok:
                rc = 1
        except TufaError as x:
            print('FAIL %s  %s' % (path, x))
            rc = 1
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
