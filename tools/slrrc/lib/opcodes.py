#!/usr/bin/env python3
"""opcodes.py - the opcode table of the SLRR script VM, as data.

One entry per op byte that can stand in a TREE node (0..47) and one entry per
sub-tag of the two "expression" opcodes 0x1007 / 0x1008.  disasm.py renders
with it, gen_opcodes_md.py turns it into notes/opcodes.md with corpus counts,
check_opcodes.py proves its structural claims against the corpus.

Vocabulary
    raw      the op byte on disk (0..47)
    code     tufa.OP_FLAGS[raw], the 16-bit opcode the VM switches on.  Its
             high nibble says whether the node has an immediate (1 u32,
             2 float bits, 4 utf8 index -> 7-byte node; 0 -> 3-byte node).
    line     the u16 that follows the op byte.  ENGINE.md calls it "slot";
             it is the source line the node was generated from.
    imm      the u32 immediate of a 7-byte node

Immediate kinds (IMM_*)
    none     3-byte node
    local    index into the frame's local variables
    tag      sub-tag of 0x1007 / 0x1008 (see TAGS)
    utf8     constant pool index of a utf8 entry
    rid      constant pool index of a rid entry
    class    constant pool index of a class entry
    ref      constant pool index of a member ref entry
    int      signed 32-bit integer
    float    IEEE-754 single, stored as its bits
    count    unsigned count (path segments, elements)
    jump     signed node offset, target = index of this node + imm

Sources: jvm_vmthread_part1.inc / part2.inc and vm_case_map.inc of slrr-reverse
(port of the old exe's VMThread_run) and the Build 940 decompilation of
VMThread_run 0x562D70, Thread_evalName 0x562540 and Thread_evalName_fieldPath
0x562280 (slrr-ghidra/VMThread_run_940.c, Thread_evalName_real_940.c,
Thread_callMethod_fieldPath_940.c).  Where the two differ, Build 940 wins.
"""
from collections import namedtuple

IMM_NONE, IMM_LOCAL, IMM_TAG, IMM_UTF8, IMM_RID, IMM_CLASS, IMM_REF, IMM_INT, \
    IMM_FLOAT, IMM_COUNT, IMM_JUMP = ('none', 'local', 'tag', 'utf8', 'rid',
                                      'class', 'ref', 'int', 'float', 'count',
                                      'jump')

POOL_KINDS = (IMM_UTF8, IMM_RID, IMM_CLASS, IMM_REF)

# role: how the VM reaches the node
#   exec     dispatched by the main loop and does something
#   carrier  operand of the node before it; when the main loop reaches it, it
#            is a no-op (VMThread_opDefault_advance)
#   segment  part of a name path, consumed by evalName / evalName_fieldPath
#   mixed    executed in some positions, consumed in others (see text)
#   unused   never emitted by the compiler in the corpus
Op = namedtuple('Op', 'raw code name imm role pops pushes text')

_O = []


def _op(raw, code, name, imm, role, pops, pushes, text):
    _O.append(Op(raw, code, name, imm, role, pops, pushes, ' '.join(text.split())))


_op(0, 0x0000, 'fatal0', IMM_NONE, 'unused', '-', '-',
    '''Raises "fatal script error" and leaves the PC where it is (case map
    entry 0).  Never emitted.''')
_op(1, 0x1001, 'local', IMM_LOCAL, 'mixed', '0', '1',
    '''Push the variable locals[imm] itself (a reference, so the operand stays
    assignable).  As a path segment (after 0x1011 or 0x0024) it is not
    executed: evalName takes locals[imm] as the current object.  Locals are
    numbered: 0 = this (instance methods only), then the parameters in REVERSE
    order (the last parameter has the lowest index), then declared locals in
    order of declaration.''')
_op(2, 0x1002, 'declare', IMM_LOCAL, 'exec', '0', '0 or 1',
    '''Declare a local: append a new variable to the frame's locals; imm is the
    index it gets.  The next node is always a 0x1006 type carrier whose utf8
    is the declared type; the VM reads it and steps over it.  When the node
    after the carrier is 0x002C the VM steps over that too and pushes the new
    variable as the target of the initialiser that follows (closed by 0x002D).
    (The VM would also accept a 0x402E node between 0x1002 and 0x1006; the
    compiler never emits one.)''')
_op(3, 0x1003, 'release', IMM_LOCAL, 'exec', '0', '0',
    '''Scope exit: remove the LAST variable from the frame's locals and release
    it.  imm names the local that is expected to be on top (the VM does not
    read it).  The operand stack is untouched.''')
_op(4, 0x0004, 'and_test', IMM_NONE, 'exec', '1', '1',
    '''Short-circuit test of "&&": pop, push back, test.  True: skip the next
    node (always a 0x1014 jump) so that the right operand is evaluated; false:
    fall onto that jump, which leads past the right operand and its
    "expr land".''')
_op(5, 0x0005, 'or_test', IMM_NONE, 'exec', '1', '1',
    '''Short-circuit test of "||": as 0x0004 with the polarity reversed (false
    skips the jump, true takes it).''')
_op(6, 0x1006, 'type', IMM_UTF8, 'carrier', '0', '0',
    '''Type carrier: a utf8 type descriptor ("I", "F", "Ljava.lang.String;",
    "[I", ...).  Operand of 0x1002 (declared type) and of the expr/stmt tags
    new 0x1E, newarray 0x1F, instanceof 0x20 and cast 0x21.  Reached on its
    own it does nothing.''')
_op(7, 0x1007, 'expr', IMM_TAG, 'exec', 'by tag', 'by tag',
    '''Expression node whose value is used; imm is the kind of expression
    (TAGS below).''')
_op(8, 0x1008, 'stmt', IMM_TAG, 'exec', 'by tag', '0',
    '''The same expression kinds in statement position: the value is not
    wanted, so operators only discard their operands and assignments do not
    push.''')
_op(9, 0x4009, 'str', IMM_UTF8, 'carrier', '0', '0',
    '''String literal carrier (after "expr literal"): utf8 index of the text.
    The VM pushes an interned java.lang.String.''')
_op(10, 0x200a, 'float', IMM_FLOAT, 'carrier', '0', '0',
    '''Float literal carrier: imm is the IEEE single bit pattern.''')
_op(11, 0x100b, 'int', IMM_INT, 'carrier', '0', '0',
    '''Int literal carrier: imm is the value (type "I").''')
_op(12, 0x000c, 'null', IMM_NONE, 'carrier', '0', '0',
    '''null literal carrier (an object-typed value with payload 0).''')
_op(13, 0x100d, 'bool', IMM_INT, 'carrier', '0', '0',
    '''Boolean literal carrier: the keywords true (imm 1) and false (imm 0).
    The VM boxes imm with another type descriptor than 0x100B (JVM global
    0x7A2604 instead of 0x7A2608), otherwise it behaves as an int.''')
_op(14, 0x100e, 'rid', IMM_RID, 'carrier', '0', '0',
    '''Resource id literal carrier (pack:0xHEXr): pool index of a rid entry.
    The VM resolves pack file name + local id to the global id at run time and
    pushes it as an int.''')
_op(15, 0x100f, 'op0f', IMM_INT, 'unused', '-', '-',
    '''No case in the VM (default advance).  Never emitted.''')
_op(16, 0x0010, 'end', IMM_NONE, 'exec', '0', '0',
    '''End of a field initialiser tree: marks the thread DONE and leaves the
    run loop.  The value of the initialiser is what is left on the operand
    stack.''')
_op(17, 0x1011, 'path', IMM_COUNT, 'exec', '0', '1',
    '''Name path: imm segment nodes follow.  Executed, it walks them
    (Thread_evalName_fieldPath) and pushes the variable the path ends in.
    Directly after 0x0024 it is not executed; it is the count header of the
    call's name path.  A 0x1019 class segment counts as one segment but stands
    for "ClassName." in front of the segment after it.''')
_op(18, 0x4012, 'name', IMM_UTF8, 'segment', '0', '0',
    '''Path segment by name (utf8).  In a field path: the field of that name
    on the current object, looked up at run time (never legal as the first
    segment of a field path).  In a call path: when the current object has a
    field of that name the walk goes on through it, otherwise it is the name
    of the method to call, resolved at run time from the argument types.''')
_op(19, 0x0013, 'fatal13', IMM_NONE, 'unused', '-', '-',
    '''Raises "fatal script error".  Never emitted.''')
_op(20, 0x1014, 'jump', IMM_JUMP, 'exec', '0', '0',
    '''Unconditional jump: continue at node (this index + imm); imm is signed,
    loops jump backwards.''')
_op(21, 0x1015, 'jump_false', IMM_JUMP, 'exec', '1', '0',
    '''Pop a value and convert it to boolean (int/float non-zero, object
    non-null).  False: jump to (this index + imm); true: next node.''')
_op(22, 0x1016, 'jump_true', IMM_JUMP, 'exec', '1', '0',
    '''As 0x1015 with the polarity reversed.''')
_op(23, 0x1017, 'case', IMM_JUMP, 'exec', '2', '0 or 1',
    '''switch dispatch: pop the case value, pop the subject, compare (==).
    Equal: both are released and the VM jumps to (this index + imm).
    Different: the subject is pushed back and the next node runs.''')
_op(24, 0x0018, 'pop', IMM_NONE, 'exec', '1', '0',
    '''Pop one operand and release it (the switch subject once no case
    matched).''')
_op(25, 0x1019, 'class', IMM_CLASS, 'carrier', '0', '0',
    '''Class carrier (pool class entry): the "ClassName." prefix at the start
    of a name path, or the target of a cast (expr tag 0x21).  Reached on its
    own it does nothing.  (The pool also holds class entries for array types,
    named "[I", "[F" or "[java.lang.String": bracket plus element class
    without the L...; wrapper.  They are the class of member refs such as
    array.length and never stand in a 0x1019 node.)''')
_op(26, 0x101a, 'method', IMM_REF, 'segment', '1 (path)', '0',
    '''Method reference that ends a call path (pool member ref with a
    "(...)R" descriptor).  evalName pops and releases one operand first (the
    argc box pushed by 0x1027; it is not needed because the descriptor is
    known), then resolves the method: through the cached slot when the ref's
    class is the class of the current object, otherwise by name and
    descriptor.  The call itself then pops the arguments.''')
_op(27, 0x101b, 'field', IMM_REF, 'mixed', '0', '1',
    '''Field reference (pool member ref with a type descriptor).  Executed on
    its own it pushes the field variable of this (instance field) or of the
    class (static field).  As a path segment it reads the field on the current
    object or class of the walk.''')
_op(28, 0x401c, 'getfield', IMM_UTF8, 'exec', '1', '1',
    '''Pop an object, push its field named by the utf8 (looked up at run time
    on the object's class).  Used when the receiver is an expression rather
    than a name path: this.x, a[i].x, f().x, (cast).x.''')
_op(29, 0x001d, 'superfield', IMM_NONE, 'unused', '-', '-',
    '''VM case "super.field" by utf8 name; the op has no immediate in the
    flags table, so it cannot work.  Never emitted.''')
_op(30, 0x101e, 'array_init', IMM_COUNT, 'unused', '-', '-',
    '''VM case: pop imm values and build an array from them (array
    initialiser lists).  Never emitted (the dialect census finds no
    initialiser list).''')
_op(31, 0x101f, 'push_zeros', IMM_COUNT, 'unused', '-', '-',
    '''VM case: push imm fresh int values of 0.  Never emitted.''')
_op(32, 0x0020, 'index', IMM_NONE, 'exec', '2', '1',
    '''Array element: pop the array (top), pop the index (below it), push the
    element variable (assignable).  So a[i] is compiled index first, array
    second.''')
_op(33, 0x0021, 'new_init', IMM_NONE, 'exec', 'argc+1', '0',
    '''Constructor call of "new T(args)": the object pushed by "expr new"
    lies under the arguments and the argc box; the VM calls <init> on it by
    name with the argument types found on the stack.  The call consumes the
    arguments and the argc box; the object stays as the value of the new
    expression.''')
_op(34, 0x0022, 'this_init', IMM_NONE, 'exec', 'argc+1', '0',
    '''this(args): call <init> of the frame's own class on this.  Always
    followed by "stmt ctor".''')
_op(35, 0x0023, 'super_init', IMM_NONE, 'exec', 'argc+1', '0',
    '''super(args): call <init> of the superclass on this (skipped when the
    class has no superclass).  Every constructor with code holds exactly one
    this_init or super_init: where the source has neither this(...) nor
    super(...), the compiler emits "push_int 0; super_init; stmt ctor"
    itself.''')
_op(36, 0x0024, 'call', IMM_NONE, 'exec', 'argc+1', '1',
    '''Method call through a name path.  The node after it is either a single
    0x101A method segment or a 0x1011 count header followed by that many
    segments, the last of which names the method (0x101A or 0x4012).
    Arguments were pushed left to right, then the argc box (0x1027).  The
    callee's return value is pushed by its 0x002B; a void method returns a
    dummy int 0, so every call leaves exactly one value, which the following
    "expr call" keeps or "stmt call" drops.''')
_op(37, 0x4025, 'call_on', IMM_UTF8, 'exec', 'argc+2', '1',
    '''Method call on a computed receiver: pop the receiver object, then call
    the method named by the utf8 on its class (resolved at run time from the
    argument types under the argc box).  Order on the stack: arguments, argc
    box, receiver.''')
_op(38, 0x4026, 'call_super', IMM_UTF8, 'exec', 'argc+1', '1',
    '''super.name(args): call the method named by the utf8 starting the lookup
    at the superclass, on this.''')
_op(39, 0x1027, 'push_int', IMM_INT, 'exec', '0', '1',
    '''Push a fresh int value imm.  The compiler uses it only for the argument
    count box that precedes every call form (0x0021-0x0024, 0x4025, 0x4026).''')
_op(40, 0x0028, 'value', IMM_NONE, 'exec', '1', '1',
    '''Pop the operand through VMThread_popOperand and push it again: a
    variable reference becomes a detached copy of its value.  Emitted after
    the expression of "return expr;" so that the value survives the release of
    the locals that follows.''')
_op(41, 0x0029, 'push_void', IMM_NONE, 'exec', '0', '1',
    '''Push a fresh int 0: the dummy return value of a void method ("return;"
    and the implicit return at the end of a non-constructor body).  Always
    followed by the releases of the live locals and 0x002B.''')
_op(42, 0x002a, 'ret', IMM_NONE, 'exec', '0', '0',
    '''Return without a value: destroy the frame and resume the caller after
    its call node.  Emitted only in constructors, which return nothing; every
    other method leaves through 0x002B.''')
_op(43, 0x002b, 'ret_value', IMM_NONE, 'exec', '1', '0 (1 in caller)',
    '''Return with a value: pop it, destroy the frame, push it on the caller's
    operand stack and resume the caller.''')
_op(44, 0x002c, 'init', IMM_NONE, 'carrier', '0', '0',
    '''Marker "this declaration has an initialiser".  Only ever found two
    nodes after a 0x1002 (behind its type carrier), where the VM consumes
    it.''')
_op(45, 0x002d, 'store', IMM_NONE, 'exec', '2', '0',
    '''Initialiser store: pop the value (top), pop the variable below it (it
    must be a variable), assign with conversion to the variable's type.
    Closes "declare; type; init; <expression>".''')
_op(46, 0x402e, 'op2e', IMM_UTF8, 'unused', '-', '-',
    '''Optional node between 0x1002 and its type carrier (a utf8 the VM hands
    to the variable constructor).  Never emitted.''')
_op(47, 0x0000, 'fatal47', IMM_NONE, 'unused', '-', '-',
    '''Maps to opcode 0 like op byte 0.  Never emitted.''')

OPS = tuple(_O)
assert [o.raw for o in OPS] == list(range(48))
BY_CODE = {o.code: o for o in OPS if o.raw not in (47,)}
BY_NAME = {o.name: o for o in OPS}

# ---------------------------------------------------------------------------
# sub-tags of 0x1007 (expr) and 0x1008 (stmt)
# ---------------------------------------------------------------------------
# carrier: which node(s) must follow the tagged node
#   None            nothing
#   'literal'       one of 0x4009 0x200A 0x100B 0x000C 0x100D 0x100E
#   'type'          0x1006
#   'type|class'    0x1006 or 0x1019
# pops_e / pushes_e: operand stack effect of "expr tag"
# pops_s: operand stack effect of "stmt tag" (never pushes)
Tag = namedtuple('Tag', 'tag name symbol carrier pops_e pushes_e pops_s text')

_T = []


def _tag(tag, name, symbol, carrier, pe, pu, ps, text):
    _T.append(Tag(tag, name, symbol, carrier, pe, pu, ps, ' '.join(text.split())))


_tag(0x00, 'cond', '?:', None, 3, 1, 3,
     '''Conditional: pop the else value (top), the then value, the condition;
     push the chosen one.  Both branches are evaluated before the choice.''')
_tag(0x01, 'lor', '||', None, 2, 1, 2,
     '''Logical or of two already evaluated operands.  With the 0x0005 /
     0x1014 pair in front of the right operand this node is only reached when
     the left operand was false.''')
_tag(0x02, 'land', '&&', None, 2, 1, 2, '''Logical and (see 0x0004).''')
_tag(0x03, 'bor', '|', None, 2, 1, 2, '''Bitwise or.''')
_tag(0x04, 'bxor', '^', None, 2, 1, 2, '''Bitwise exclusive or.''')
_tag(0x05, 'band', '&', None, 2, 1, 2, '''Bitwise and.''')
_tag(0x06, 'ne', '!=', None, 2, 1, 2, '''Not equal.''')
_tag(0x07, 'eq', '==', None, 2, 1, 2,
     '''Equal (strings are interned, so == on strings compares text).''')
_tag(0x08, 'ge', '>=', None, 2, 1, 2, '''Greater or equal.''')
_tag(0x09, 'le', '<=', None, 2, 1, 2, '''Less or equal.''')
_tag(0x0A, 'gt', '>', None, 2, 1, 2, '''Greater.''')
_tag(0x0B, 'lt', '<', None, 2, 1, 2, '''Less.''')
_tag(0x0C, 'tag0c', '', None, None, None, None,
     '''No body in the VM (default advance).  Never emitted.''')
_tag(0x0D, 'shr', '>>', None, 2, 1, 2, '''Shift right.''')
_tag(0x0E, 'shl', '<<', None, 2, 1, 2, '''Shift left.''')
_tag(0x0F, 'sub', '-', None, 2, 1, 2, '''Subtract.''')
_tag(0x10, 'add', '+', None, 2, 1, 2,
     '''Add; string concatenation when a side is a String.''')
_tag(0x11, 'mod', '%', None, 2, 1, 2, '''Remainder.''')
_tag(0x12, 'div', '/', None, 2, 1, 2, '''Divide.''')
_tag(0x13, 'mul', '*', None, 2, 1, 2, '''Multiply.''')
_tag(0x14, 'bnot', '~', None, 1, 1, 1, '''Bitwise complement.''')
_tag(0x15, 'lnot', '!', None, 1, 1, 1, '''Logical not.''')
_tag(0x16, 'neg', '-x', None, 1, 1, 1,
     '''Unary minus.  Negative literals are compiled as the positive literal
     followed by this node; nothing is folded.''')
_tag(0x17, 'postinc', 'x++', None, 1, 1, 1,
     '''Pop a variable; expr: push a copy of its value, then increment the
     variable; stmt: increment only.''')
_tag(0x18, 'postdec', 'x--', None, 1, 1, 1, '''As 0x17, decrement.''')
_tag(0x19, 'preinc', '++x', None, 1, 1, 1,
     '''Pop a variable, increment it; expr: push a copy of the new value.''')
_tag(0x1A, 'predec', '--x', None, 1, 1, 1, '''As 0x19, decrement.''')
_tag(0x1B, 'call', 'f()', None, 0, 0, 1,
     '''Marks the end of a method call (after the 0x101A / 0x4012 that ends a
     call path, or after 0x4025 / 0x4026).  expr: nothing to do, the return
     value is on the stack; stmt: pop and release it.''')
_tag(0x1C, 'literal', 'lit', 'literal', 0, 1, 0,
     '''Literal: the next node is the carrier (0x4009 string, 0x200A float,
     0x100B int, 0x100D true/false, 0x100E resource id, 0x000C null).  expr: push
     the value; stmt: step over the carrier.''')
_tag(0x1D, 'tag1d', '', None, None, None, None,
     '''No body in the VM (default advance).  Never emitted.''')
_tag(0x1E, 'new', 'new T', 'type', 0, 1, 0,
     '''Object creation: the next node is the 0x1006 type carrier, always a
     class descriptor "Lpackage.Name;"; the VM loads the class and pushes a
     new, uninitialised instance (both as expr and as stmt).  The arguments,
     the argc box and 0x0021 follow.''')
_tag(0x1F, 'newarray', 'new T[n]', 'type', 1, 1, 1,
     '''Array creation: the next node is the 0x1006 carrier of the ARRAY type
     ("[I", "[F", "[Ljava.lang.String;"); pop one size per "[" (the VM rejects
     more than one dimension), push the array.''')
_tag(0x20, 'instanceof', 'instanceof', 'type', 1, 1, 1,
     '''Pop an object; the next node is the 0x1006 carrier of the class
     descriptor ("Lpackage.Name;"); push int 1 when the object's class
     inherits from it, else 0 (null gives 0).''')
_tag(0x21, 'cast', '(T)', 'type|class', 1, 1, 1,
     '''Cast: pop a value, convert it to the type named by the next node and
     push it.  The carrier is a 0x1006 descriptor for a primitive type (only
     "I" and "F" occur) and a 0x1019 class entry for a class type; no cast to
     an array type occurs.  "cannot cast" is a script error.''')
_tag(0x22, 'ctor', 'this()/super()', None, 0, 0, 0,
     '''Marks the end of a this(...) / super(...) call.  No body in the VM;
     only the stmt form occurs.''')
_tag(0x23, 'assign', '=', None, 2, 1, 2,
     '''Assignment: pop the destination variable (top; "stack access: value
     on top (instead of variable)" otherwise), pop the source value, assign
     with conversion to the destination type.  expr pushes the source value.
     So the right-hand side is compiled first, the left-hand side second.''')
_tag(0x24, 'mul_assign', '*=', None, 2, 1, 2, '''Compound assignment.''')
_tag(0x25, 'div_assign', '/=', None, 2, 1, 2, '''Compound assignment.''')
_tag(0x26, 'mod_assign', '%=', None, 2, 1, 2,
     '''Compound assignment.  Never emitted in the corpus.''')
_tag(0x27, 'add_assign', '+=', None, 2, 1, 2,
     '''Compound assignment (string append when the destination is a
     String).''')
_tag(0x28, 'sub_assign', '-=', None, 2, 1, 2, '''Compound assignment.''')
_tag(0x29, 'tag29', '', None, 2, 1, 2,
     '''Assignment kind the VM reports as unsupported.  Never emitted.''')
_tag(0x2A, 'tag2a', '', None, 2, 1, 2,
     '''Assignment kind the VM reports as unsupported.  Never emitted.''')
_tag(0x2B, 'tag2b', '', None, 2, 1, 2,
     '''Assignment kind the VM reports as unsupported.  Never emitted.''')
_tag(0x2C, 'and_assign', '&=', None, 2, 1, 2, '''Compound assignment.''')
_tag(0x2D, 'xor_assign', '^=', None, 2, 1, 2, '''Compound assignment.''')
_tag(0x2E, 'or_assign', '|=', None, 2, 1, 2, '''Compound assignment.''')

TAGS = tuple(_T)
assert [t.tag for t in TAGS] == list(range(0x2F))
TAG_BY_NAME = {t.name: t for t in TAGS}

LITERAL_CARRIERS = (0x4009, 0x200a, 0x100b, 0x000c, 0x100d, 0x100e)
CALL_OPS = (0x0021, 0x0022, 0x0023, 0x0024, 0x4025, 0x4026)
JUMP_OPS = (0x1014, 0x1015, 0x1016, 0x1017)


def op_for_raw(raw):
    return OPS[raw]


def tag_info(tag):
    return TAGS[tag] if 0 <= tag < len(TAGS) else None


def mnemonic(raw, imm=None):
    """'local', 'expr add', 'stmt assign', ..."""
    o = OPS[raw]
    if o.imm == IMM_TAG and imm is not None:
        t = tag_info(imm)
        return '%s %s' % (o.name, t.name if t else 'tag%02x' % imm)
    return o.name
