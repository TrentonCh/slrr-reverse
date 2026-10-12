"""write.py - the writer: the last two steps of the pool and the bytes of a
class file.

    finish_pool(build)              steps 4 and 5 of the pool, over the trees
    class_blob(build, number)       the bytes of one class
    class_file(builds, number)      the blobs of a source, one behind the other

THE LAST TWO STEPS OF THE POOL (notes/declarations.md 6.5 and 6.6).  The
trees of a class exist in full, with operands that name pool content, before
a single ref or string has an index.  Two passes over them append to the pool
that decl.py left behind (seed, types, names):

  4 resolve   the trees BY ROW VECTOR: static methods, instance methods,
              static field initialisers, instance field initialisers, each
              vector in row order, every tree node by node:
                class node        utf8 of the name, class entry
                field, method     the class of the ref (utf8, class entry),
                                  the nat (utf8 name, utf8 descriptor, nat),
                                  the ref
                a segment of a path behind the first
                                  first the class entry of the static type of
                                  the segment in front of it (the declared
                                  type of a local or parameter, the type of a
                                  field), whether the segment then is a ref
                                  or stays a bare name
                this as a value   utf8 "LC;" of the class being compiled
  5 emit      ALL trees IN TREE ORDER (source order, fields and methods
              mixed), node by node:
                str               utf8 of the text
                name, getfield, call_on, call_super
                                  utf8 of the member name
                rid               utf8 of the pack file name, rid entry

Both orders are proven by the shipped pools: step 4 in tree order fails 50 of
2,873 classes, step 5 in the order of step 4 fails 47, one merged pass 2,806.
A type carrier (declare, new, new T[n], instanceof, cast to int or float)
interns nothing here: its descriptor is in the pool since step 2.

The static type of a local is read from the tree itself: `declare i` is
followed by the carrier of its type.  A frame starts with `this` (not in
static code) and the parameters in reverse order.

THE BYTES (notes/foundation.md 3).  A file is a flat stream of chunks
{tag: 4 bytes, size: u32, payload}; all integers are little endian, nothing
is padded.  A class is the chunks

    TUFA    the build number (u32)
    CONS    u32 count, then per entry u32 kind + payload; a utf8 entry is its
            length, the bytes and one NUL (the length excludes the NUL)
    FILD    statics, then instance fields: u32 n and n rows of 16 bytes;
            written exactly when the class has a field
    MTHD    statics, then instance methods: u32 n and n rows of 20 bytes;
            written exactly when the class has a method
    CLSS    0xFFFFFFFF, interface flag, name, superclass, n, n interfaces
    TREE    u32 tree count, then per tree u32 node count and the nodes

A node is op: u8, line: u16 and, when the node has an operand, a u32: the
number itself, or the index of the pool entry the operand names.  A source
with several classes gives one such sequence per class, in source order,
with nothing in between.

Texts are str with one character per byte.

C++ (sc_write.cpp): the same functions appending to a std::vector<uint8_t>.
"""
from . import classes
from . import pool as pl
from .emit import (
    K_NONE, K_IMM, K_UTF8, K_CLASS, K_REF,
    OP_LOCAL, OP_DECLARE, OP_TYPE, OP_STR, OP_RID, OP_PATH, OP_NAME, OP_CLASS,
    OP_METHOD, OP_FIELD, OP_GETFIELD, OP_CALL, OP_CALL_ON, OP_CALL_SUPER)

NONE = 0xFFFFFFFF


# ---------------------------------------------------------------------------
# step 4: resolve
# ---------------------------------------------------------------------------

def frame_types(is_static, desc):
    """The static types of the locals a tree starts with: `this` (not in
    static code; '' stands for it here), then the parameters in reverse
    order.  desc: the method descriptor, None for a field initialiser."""
    types = []
    if not is_static:
        types.append('')
    if desc is not None:
        params = classes.split_params(desc)
        k = len(params)
        while k > 0:                    # the last parameter first
            k -= 1
            types.append(params[k])
    return types


def touch_receiver(pool, type_desc):
    """A member is about to be looked up on a value of this static type: the
    class entry of the type comes first.  Only a class type has one here; an
    array receiver (its one member is length) interns nothing in advance."""
    if type_desc is None:
        return
    n = len(type_desc)
    if n >= 2 and type_desc[0] == 'L' and type_desc[n - 1] == ';':
        pool.cls(type_desc[1:n - 1])


def resolve_tree(pool, tree, is_static, desc, own_type):
    """Step 4 for one tree.  own_type: "LC;" of the class being compiled."""
    nodes = tree.nodes
    ops = tree.operands
    n = len(nodes)
    types = frame_types(is_static, desc)        # local index -> descriptor
    has_this = not is_static
    i = 0
    while i < n:
        nd = nodes[i]
        op = nd.op
        if op == OP_DECLARE:
            # declare k, type T: local k has the static type T from here on
            if i + 1 < n and nodes[i + 1].op == OP_TYPE:
                k = nd.value
                while len(types) <= k:
                    types.append(None)
                types[k] = ops[nodes[i + 1].value].text
            i += 1
            continue
        if op == OP_PATH or op == OP_CALL:
            # a name path, or a call followed by its callee
            j = i + 1
            if op == OP_CALL:
                if j < n and nodes[j].op == OP_METHOD:
                    o = ops[nodes[j].value]
                    pool.ref(o.text, o.name, o.desc)
                    i = j + 1
                    continue
                if j < n and nodes[j].op == OP_PATH:
                    count = nodes[j].value
                    j += 1
                else:
                    i += 1
                    continue
            else:
                count = nd.value
            end = j + count
            if end > n:
                end = n
            reached = None                      # static type of the path so far
            k = j
            while k < end:
                seg = nodes[k]
                sop = seg.op
                first = k == j
                if sop == OP_LOCAL:
                    if has_this and seg.value == 0:
                        reached = own_type
                        pool.utf8(own_type)
                    elif seg.value < len(types):
                        reached = types[seg.value]
                    else:
                        reached = None
                elif sop == OP_CLASS:
                    pool.cls(ops[seg.value].text)
                    reached = None
                elif sop == OP_FIELD:
                    if not first:
                        touch_receiver(pool, reached)
                    o = ops[seg.value]
                    pool.ref(o.text, o.name, o.desc)
                    reached = o.desc
                elif sop == OP_METHOD:
                    if not first:
                        touch_receiver(pool, reached)
                    o = ops[seg.value]
                    pool.ref(o.text, o.name, o.desc)
                    reached = None
                elif sop == OP_NAME:
                    if not first:
                        touch_receiver(pool, reached)
                    reached = None
                k += 1
            i = j + count
            continue
        if op == OP_CLASS:
            pool.cls(ops[nd.value].text)
        elif op == OP_METHOD or op == OP_FIELD:
            o = ops[nd.value]
            pool.ref(o.text, o.name, o.desc)
        elif op == OP_LOCAL and has_this and nd.value == 0:
            pool.utf8(own_type)
        i += 1


def resolve_pass(build):
    """Step 4 for a class: the trees by row vector."""
    pool = build.pool
    rows = build.rows
    trees = build.trees
    own_type = 'L' + build.info.name + ';'
    for r in rows.static_methods:
        if r.tree != NONE:
            resolve_tree(pool, trees[r.tree], True, pool.entries[r.desc].text, own_type)
    for r in rows.instance_methods:
        if r.tree != NONE:
            resolve_tree(pool, trees[r.tree], False, pool.entries[r.desc].text, own_type)
    for r in rows.static_fields:
        if r.tree != NONE:
            resolve_tree(pool, trees[r.tree], True, None, own_type)
    for r in rows.instance_fields:
        if r.tree != NONE:
            resolve_tree(pool, trees[r.tree], False, None, own_type)


# ---------------------------------------------------------------------------
# step 5: emit
# ---------------------------------------------------------------------------

def emit_pass(build):
    """Step 5 for a class: the trees in tree order."""
    pool = build.pool
    for tree in build.trees:
        ops = tree.operands
        for nd in tree.nodes:
            op = nd.op
            if op == OP_STR:
                pool.utf8(ops[nd.value].text)
            elif (op == OP_NAME or op == OP_GETFIELD or op == OP_CALL_ON or
                  op == OP_CALL_SUPER):
                pool.utf8(ops[nd.value].text)
            elif op == OP_RID:
                o = ops[nd.value]
                pool.rid(o.text, o.number)


def finish_pool(build):
    """Steps 4 and 5 for a class whose trees are emitted.  The length of the
    pool after each of the five steps is in build.pool_steps."""
    resolve_pass(build)
    build.pool_steps.append(len(build.pool.entries))
    emit_pass(build)
    build.pool_steps.append(len(build.pool.entries))


# ---------------------------------------------------------------------------
# bytes
# ---------------------------------------------------------------------------

def put_u16(out, v):
    out.append(v & 0xFF)
    out.append((v >> 8) & 0xFF)


def put_u32(out, v):
    out.append(v & 0xFF)
    out.append((v >> 8) & 0xFF)
    out.append((v >> 16) & 0xFF)
    out.append((v >> 24) & 0xFF)


def put_chunk(out, tag, payload):
    out.extend(tag.encode('latin-1'))
    put_u32(out, len(payload))
    out.extend(payload)


def cons_payload(pool):
    out = bytearray()
    put_u32(out, len(pool.entries))
    for e in pool.entries:
        put_u32(out, e.kind)
        if e.kind == pl.UTF8:
            put_u32(out, len(e.text))
            out.extend(e.text.encode('latin-1'))
            out.append(0)
        elif e.kind == pl.CLASS:
            put_u32(out, e.a)
        else:                           # NAT, REF, RID: two words
            put_u32(out, e.a)
            put_u32(out, e.b)
    return bytes(out)


def has_fild(rows):
    """FILD is written exactly when the class has a field."""
    return len(rows.static_fields) + len(rows.instance_fields) > 0


def has_mthd(rows):
    """MTHD is written exactly when the class has a method."""
    return len(rows.static_methods) + len(rows.instance_methods) > 0


def _field_rows(out, rows):
    put_u32(out, len(rows))
    for r in rows:
        put_u32(out, r.flags)
        put_u32(out, r.name)
        put_u32(out, r.type)
        put_u32(out, r.tree)


def _method_rows(out, rows):
    put_u32(out, len(rows))
    for r in rows:
        put_u32(out, r.flags)
        put_u32(out, r.name)
        put_u32(out, r.desc)
        put_u32(out, r.tree)
        put_u32(out, r.locals)


def fild_payload(rows):
    out = bytearray()
    _field_rows(out, rows.static_fields)
    _field_rows(out, rows.instance_fields)
    return bytes(out)


def mthd_payload(rows):
    out = bytearray()
    _method_rows(out, rows.static_methods)
    _method_rows(out, rows.instance_methods)
    return bytes(out)


def clss_payload(rows):
    out = bytearray()
    put_u32(out, 0xFFFFFFFF)
    put_u32(out, rows.interface)
    put_u32(out, rows.name)
    put_u32(out, rows.super)
    put_u32(out, len(rows.interfaces))
    for i in rows.interfaces:
        put_u32(out, i)
    return bytes(out)


def operand_index(pool, tree, nd):
    """The u32 behind a node: the number, or the index of the pool entry its
    operand names.  After finish_pool() every such entry exists and interning
    only finds it."""
    k = nd.kind
    if k == K_IMM:
        return nd.value
    o = tree.operands[nd.value]
    if k == K_UTF8:
        return pool.utf8(o.text)
    if k == K_CLASS:
        return pool.cls(o.text)
    if k == K_REF:
        return pool.ref(o.text, o.name, o.desc)
    return pool.rid(o.text, o.number)       # K_RID


def tree_payload(pool, trees):
    out = bytearray()
    put_u32(out, len(trees))
    for tree in trees:
        put_u32(out, len(tree.nodes))
        for nd in tree.nodes:
            out.append(nd.op)
            put_u16(out, nd.line)
            if nd.kind != K_NONE:
                put_u32(out, operand_index(pool, tree, nd))
    return bytes(out)


def class_blob(build, build_number):
    """The bytes of one class: TUFA, CONS, [FILD], [MTHD], CLSS, TREE.
    finish_pool(build) must have run."""
    rows = build.rows
    # the trees first: CONS stands in front of TREE in the file, but only the
    # encoded trees prove that the pool is complete
    tree = tree_payload(build.pool, build.trees)
    out = bytearray()
    number = bytearray()
    put_u32(number, build_number)
    put_chunk(out, 'TUFA', number)
    put_chunk(out, 'CONS', cons_payload(build.pool))
    if has_fild(rows):
        put_chunk(out, 'FILD', fild_payload(rows))
    if has_mthd(rows):
        put_chunk(out, 'MTHD', mthd_payload(rows))
    put_chunk(out, 'CLSS', clss_payload(rows))
    put_chunk(out, 'TREE', tree)
    return bytes(out)


def class_file(builds, build_number):
    """The bytes of a class file: the classes of a source in source order."""
    out = bytearray()
    for b in builds:
        out.extend(class_blob(b, build_number))
    return bytes(out)
