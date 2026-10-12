#!/usr/bin/env python3
"""destruct.py - read the statement structure back out of a node sequence.

The inverse of stmtgen.py, and independent of it: no source is used.  A tree
is accepted only when the whole node sequence can be split into the statement
forms of notes/statements.md, with every jump target, every release index and
every epilogue as those forms prescribe.  This is how the 2,030 trees of the
class files that ship WITHOUT a source (the runtime library under
system/scripts) are checked, and it gives every jump of the corpus a role.

    d = Destruct(blob, nodes, kind, is_ctor, is_static, nargs, ret_void)
    d.run()           raises Fail(reason, node index) when no form fits
    d.forms           Counter of statement forms
    d.roles           {node index of a jump: role}

Where two readings produce the same nodes the choice is fixed and documented
(the two programs are equivalent, so nothing depends on it):

    A1  `if (c) { ...; break; }` against `if (c) ... else ...` whose end is
        the break target: read as break.
    A2  the else-jump of an if-else that closes a loop body against
        `continue`: read as if-else.
    A3  `super();` written out against the implicit super call: one form.
    A4  while against for: both are "loop with condition"; a for without
        condition is "loop without condition".
"""
import os
import sys
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for _p in (_LIB, _HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import tufa  # noqa: E402

LOCAL, DECLARE, RELEASE = 0x1001, 0x1002, 0x1003
AND_TEST, OR_TEST = 0x0004, 0x0005
TYPE, EXPR, STMT = 0x1006, 0x1007, 0x1008
END, PATH, NAME = 0x0010, 0x1011, 0x4012
JUMP, JUMP_FALSE, JUMP_TRUE, CASE, POP = 0x1014, 0x1015, 0x1016, 0x1017, 0x0018
CLASS, METHOD, FIELD, GETFIELD = 0x1019, 0x101a, 0x101b, 0x401c
INDEX, NEW_INIT, THIS_INIT, SUPER_INIT, CALL = 0x0020, 0x0021, 0x0022, 0x0023, 0x0024
CALL_ON, CALL_SUPER, PUSH_INT = 0x4025, 0x4026, 0x1027
VALUE, PUSH_VOID, RET, RET_VALUE, INIT, STORE = 0x0028, 0x0029, 0x002a, 0x002b, 0x002c, 0x002d
LITERALS = (0x4009, 0x200a, 0x100b, 0x000c, 0x100d, 0x100e)

T_COND, T_LOR, T_LAND = 0x00, 0x01, 0x02
T_CALL, T_LITERAL, T_NEW, T_NEWARRAY, T_INSTANCEOF, T_CAST, T_CTOR = \
    0x1B, 0x1C, 0x1E, 0x1F, 0x20, 0x21, 0x22
BINARY = frozenset(range(0x03, 0x14)) - {0x0C}
UNARY = frozenset((0x14, 0x15, 0x16))
INCDEC = frozenset((0x17, 0x18, 0x19, 0x1A))
ASSIGN = frozenset(range(0x23, 0x2F))
STMT_NAMES = {0x17: 'x++', 0x18: 'x--', 0x19: '++x', 0x1A: '--x', 0x1B: 'call',
              0x1E: 'new', 0x22: 'this()/super()'}


def signed(v):
    return v - (1 << 32) if v & 0x80000000 else v


class Fail(Exception):
    def __init__(self, why, pos):
        Exception.__init__(self, '%s (node %s)' % (why, pos))
        self.why = why
        self.pos = pos


class Flow(object):
    __slots__ = ('kind', 'brk', 'top', 'back', 'mark', 'conts')

    def __init__(self, kind, brk, top, back, mark):
        self.kind = kind      # 'loop' or 'switch'
        self.brk = brk        # node index a break jumps to
        self.top = top        # loop: index a while-style continue jumps to
        self.back = back      # loop: index of the closing jump node
        self.mark = mark      # live locals when the construct was entered
        self.conts = []       # forward continue targets seen (to verify)


class Destruct(object):
    def __init__(self, blob, nodes, kind, is_ctor=False, is_static=False,
                 nargs=0, ret_void=True):
        self.blob = blob
        self.nd = nodes
        self.N = len(nodes)
        self.code = [tufa.OP_FLAGS[x.op] for x in nodes]
        self.kind = kind                      # 'field' or 'method'
        self.is_ctor = is_ctor
        self.ret_void = ret_void
        live = nargs + (0 if is_static else 1)
        self.lo = self.hi = live              # possible number of live locals
        self.nparams = live
        self.max_live = live
        self.forms = Counter()
        self.roles = {}
        self.flow = []
        self.starts = set()
        self.notes = []
        code = self.code
        # jumps that belong to && / ||
        self.sc = set(i for i in range(1, self.N)
                      if code[i] == JUMP and code[i - 1] in (AND_TEST, OR_TEST))
        # statement-level backward jumps, by target
        self.back = {}
        self.do_at = {}
        for i in range(self.N):
            if code[i] == JUMP and i not in self.sc:
                off = signed(nodes[i].imm)
                if off < 0:
                    self.back.setdefault(i + off, []).append(i)
            elif code[i] == JUMP_TRUE:
                off = signed(nodes[i].imm)
                if off >= 0:
                    self.notes.append('jump_true forward at %d' % i)
                self.do_at.setdefault(i + off, []).append(i)

    # ---- helpers --------------------------------------------------------
    def target(self, i):
        return i + signed(self.nd[i].imm)

    def set_live(self, v):
        self.lo = self.hi = v
        if v > self.max_live:
            self.max_live = v

    def clamp(self, v, pos, what):
        """At the end of a branch, loop body or case group the live locals
        are those of its start (v: a count or a (lo, hi) range)."""
        lo, hi = v if isinstance(v, tuple) else (v, v)
        a, b = max(lo, self.lo), min(hi, self.hi)
        if a > b:
            raise Fail('%s: %d..%d locals expected live, model has %d..%d'
                       % (what, lo, hi, self.lo, self.hi), pos)
        self.lo, self.hi = a, b

    def snapshot(self):
        return (self.lo, self.hi, self.max_live, Counter(self.forms),
                dict(self.roles), list(self.flow), set(self.starts),
                list(self.notes), [list(f.conts) for f in self.flow])

    def restore(self, snap):
        (self.lo, self.hi, self.max_live, self.forms, self.roles, self.flow,
         self.starts, self.notes, conts) = snap
        self.forms = Counter(self.forms)
        self.roles = dict(self.roles)
        self.flow = list(self.flow)
        self.starts = set(self.starts)
        self.notes = list(self.notes)
        for f, c in zip(self.flow, conts):
            f.conts = list(c)

    def release_run(self, p, end):
        """Indices released by the consecutive release nodes at p."""
        out = []
        while p < end and self.code[p] == RELEASE:
            k = self.nd[p].imm
            if out and k != out[-1] - 1:
                break
            out.append(k)
            p += 1
        return out

    # ---- the expression machine ----------------------------------------
    def machine(self, p, end, stack=None, dry=False):
        """Run the nodes from p as postfix code until a node that ends an
        expression.  Returns (terminator, index, stack depth).  Terminators:
        'stmt' 'jf' 'jt' 'value' 'case' 'store' 'end' 'pop'."""
        code, nd = self.code, self.nd
        st = list(stack or [])
        pend = []            # targets of the && / || jumps that are open
        roles = {}
        stmt_new = False
        i = p

        def pop(n, what):
            if len(st) < n:
                raise Fail('%s pops %d, stack holds %d' % (what, n, len(st)), i)
            for _ in range(n):
                if st.pop() != 'v':
                    raise Fail('%s pops an argument count' % what, i)

        def pop_args(what):
            if not st or st[-1] == 'v':
                raise Fail('%s without an argument count on top' % what, i)
            k = st.pop()[1]
            pop(k, what)
            return k

        def path(j, call):
            """j is the index of a path header; returns the index behind the
            segments."""
            n = nd[j].imm
            if n < 1 or j + n >= self.N:
                raise Fail('path runs off the tree', j)
            for s in range(n):
                c = code[j + 1 + s]
                first, last = s == 0, s == n - 1
                ok = (c == LOCAL and first) or c == FIELD or \
                     (c == CLASS and first and n >= 2) or \
                     (c == NAME and (call or not first)) or \
                     (c == METHOD and call and last)
                if not ok:
                    raise Fail('path segment %d of %d is 0x%04X' % (s, n, c), j + 1 + s)
            if call and code[j + n] not in (METHOD, NAME):
                raise Fail('call path ends in 0x%04X' % code[j + n], j + n)
            return j + 1 + n

        while True:
            if i >= end:
                raise Fail('expression runs to the end of its region', i)
            c = code[i]
            if c == LOCAL or c == FIELD:
                st.append('v')
                i += 1
            elif c == PATH:
                i = path(i, False)
                st.append('v')
            elif c == EXPR or c == STMT:
                tag = nd[i].imm
                is_stmt = c == STMT
                if tag == T_LITERAL:
                    if i + 1 >= self.N or code[i + 1] not in LITERALS:
                        raise Fail('literal without a carrier', i)
                    if is_stmt:
                        raise Fail('stmt literal', i)
                    st.append('v')
                    i += 2
                    continue
                if tag == T_NEW:
                    if code[i + 1] != TYPE:
                        raise Fail('new without a type carrier', i)
                    st.append('v')
                    if is_stmt:
                        stmt_new = True
                    i += 2
                    continue
                if tag in (T_NEWARRAY, T_INSTANCEOF, T_CAST):
                    want = (TYPE, CLASS) if tag == T_CAST else (TYPE,)
                    if code[i + 1] not in want:
                        raise Fail('tag 0x%02X without its carrier' % tag, i)
                    pop(1, 'tag 0x%02X' % tag)
                    if not is_stmt:
                        st.append('v')
                    i += 2
                elif tag == T_CALL:
                    # the marker stands directly behind a call form
                    if i == 0 or code[i - 1] not in (METHOD, NAME, CALL_ON, CALL_SUPER):
                        raise Fail('expr/stmt call not behind a call', i)
                    if is_stmt:
                        pop(1, 'stmt call')
                    i += 1
                elif tag == T_CTOR:
                    if not is_stmt:
                        raise Fail('expr ctor', i)
                    i += 1
                elif tag in BINARY:
                    pop(2, 'binary')
                    if not is_stmt:
                        st.append('v')
                    i += 1
                elif tag in (T_LAND, T_LOR):
                    if not pend or pend[-1][0] != i + 1:
                        raise Fail('land/lor without the matching test jump', i)
                    t, kind = pend.pop()
                    if kind != tag:
                        raise Fail('&& test closed by lor or the reverse', i)
                    pop(2, 'land/lor')
                    if not is_stmt:
                        st.append('v')
                    i += 1
                elif tag in UNARY:
                    pop(1, 'unary')
                    if not is_stmt:
                        st.append('v')
                    i += 1
                elif tag in INCDEC:
                    pop(1, 'inc/dec')
                    if not is_stmt:
                        st.append('v')
                    i += 1
                elif tag == T_COND:
                    pop(3, '?:')
                    if not is_stmt:
                        st.append('v')
                    i += 1
                elif tag in ASSIGN:
                    pop(2, 'assignment')
                    if not is_stmt:
                        st.append('v')
                    i += 1
                else:
                    raise Fail('expression kind 0x%02X' % tag, i)
                if is_stmt:
                    if st or pend:
                        raise Fail('statement node leaves %d operand(s)' % len(st), i - 1)
                    if not dry:
                        self.roles.update(roles)
                    return 'stmt', i - 1, tag
            elif c == PUSH_INT:
                st.append(('argc', nd[i].imm))
                i += 1
            elif c == NEW_INIT:
                pop_args('new_init')
                if not st or st[-1] != 'v':
                    raise Fail('new_init without the object under the arguments', i)
                i += 1
                if stmt_new and len(st) == 1:
                    # `new T(..);` as a statement: the object stays on the
                    # operand stack (one occurrence, Frontend)
                    if not dry:
                        self.roles.update(roles)
                        self.notes.append('statement "new T(..);" leaves the object on the stack (node %d)' % (i - 1))
                    return 'stmt', i - 1, T_NEW
            elif c == CALL:
                j = i + 1
                if j < self.N and code[j] == METHOD:
                    pop_args('call')
                    i = j + 1
                elif j < self.N and code[j] == PATH:
                    pop_args('call')
                    i = path(j, True)
                else:
                    raise Fail('call followed by 0x%04X instead of a method or a path'
                               % (code[j] if j < self.N else 0), i)
                st.append('v')
                if i >= self.N or code[i] not in (EXPR, STMT) or nd[i].imm != T_CALL:
                    raise Fail('call path not followed by expr/stmt call', i)
            elif c == CALL_ON:
                pop(1, 'call_on receiver')
                pop_args('call_on')
                st.append('v')
                i += 1
                if i >= self.N or code[i] not in (EXPR, STMT) or nd[i].imm != T_CALL:
                    raise Fail('call_on not followed by expr/stmt call', i)
            elif c == CALL_SUPER:
                pop_args('call_super')
                st.append('v')
                i += 1
                if i >= self.N or code[i] not in (EXPR, STMT) or nd[i].imm != T_CALL:
                    raise Fail('call_super not followed by expr/stmt call', i)
            elif c in (THIS_INIT, SUPER_INIT):
                pop_args('this_init/super_init')
                i += 1
                if i >= self.N or code[i] != STMT or nd[i].imm != T_CTOR:
                    raise Fail('this_init/super_init not followed by stmt ctor', i)
            elif c == INDEX:
                pop(2, 'index')
                st.append('v')
                i += 1
            elif c == GETFIELD:
                pop(1, 'getfield')
                st.append('v')
                i += 1
            elif c in (AND_TEST, OR_TEST):
                if not st or st[-1] != 'v':
                    raise Fail('and_test/or_test on an empty stack', i)
                if i + 1 >= self.N or code[i + 1] != JUMP:
                    raise Fail('and_test/or_test without its jump', i)
                t = self.target(i + 1)
                if t <= i + 1:
                    raise Fail('&&/|| jump goes backwards', i + 1)
                pend.append((t, T_LAND if c == AND_TEST else T_LOR))
                roles[i + 1] = 'sc.jump'
                i += 2
            elif c in (JUMP_FALSE, JUMP_TRUE, VALUE, CASE, STORE, END, POP):
                if pend:
                    raise Fail('expression ends inside && / ||', i)
                if not dry:
                    self.roles.update(roles)
                name = {JUMP_FALSE: 'jf', JUMP_TRUE: 'jt', VALUE: 'value',
                        CASE: 'case', STORE: 'store', END: 'end', POP: 'pop'}[c]
                return name, i, len(st)
            else:
                raise Fail('node 0x%04X inside an expression' % c, i)

    # ---- statements -----------------------------------------------------
    def exit_flow(self, q):
        """Is the statement-level jump at q a break or a continue?  Returns
        (kind, Flow) or None."""
        t = self.target(q)
        if self.flow and t == self.flow[-1].brk:
            return 'break', self.flow[-1]
        for f in reversed(self.flow):
            if f.kind == 'loop':
                if f.top is not None and t == f.top:
                    return 'continue', f
                if q < t <= f.back:
                    return 'continue', f
                break
        return None

    def do_exit(self, p, q, run, kind, f):
        """break / continue at q, preceded by the release run that starts at p."""
        if run:
            if not self.lo <= run[0] + 1 <= self.hi:
                raise Fail('%s releases local %d, %d..%d are live'
                           % (kind, run[0], self.lo, self.hi), p)
            if run[-1] != f.mark:
                raise Fail('%s releases down to %d, the %s was entered with %d'
                           % (kind, run[-1], f.kind, f.mark), p)
            self.lo, self.hi = f.mark, run[0] + 1
            self.forms['%s that releases locals' % kind] += 1
        else:
            if self.lo > f.mark:
                raise Fail('%s without releases although %d locals were declared '
                           'inside the %s' % (kind, self.lo - f.mark, f.kind), q)
            self.lo = f.mark
        self.roles[q] = kind + '.jump'
        self.forms[kind] += 1
        self.forms['%s in a %s' % (kind, f.kind)] += 1
        if kind == 'continue':
            if self.target(q) != f.top:
                f.conts.append(self.target(q))
                self.forms['continue: jump forward (to the update / the closing jump)'] += 1
            else:
                self.forms['continue: jump back to the condition'] += 1
            self.notes.append('continue at node %d (%+d)' % (q, signed(self.nd[q].imm)))

    def seq(self, p, end, do_j=None):
        """Statements from p up to exactly end.  do_j: the sequence is the
        body of a do-while and ends with a condition closed by the jump_true
        at index do_j."""
        code = self.code
        while p < end:
            self.starts.add(p)
            p = self.stmt(p, end, do_j)
            if do_j is not None and p == do_j + 1:
                return p
        if p != end:
            raise Fail('statement runs past the end of its region %d' % end, p)
        if do_j is not None:
            raise Fail('do-while body without its condition', p)
        return p

    def pick_back(self, p, end):
        """The closing jump of a loop that starts at p and lies inside the
        region: the last backward jump to p before `end`."""
        best = None
        for i in self.back.get(p, ()):
            if i < end and i not in self.roles:
                if best is None or i > best:
                    best = i
        return best

    def stmt(self, p, end, do_j=None):
        code, nd = self.code, self.nd
        c = code[p]
        # ---- do-while: a jump_true further down comes back to p
        for j in sorted(self.do_at.get(p, ()), reverse=True):
            if j < end and j not in self.roles:
                self.roles[j] = 'do.jump_true'
                f = Flow('loop', j + 1, None, j, self.hi)
                self.flow.append(f)
                entry = (self.lo, self.hi)
                q = self.seq(p, j + 1, do_j=j)
                self.flow.pop()
                self.check_conts(f)
                self.clamp(entry, j, 'end of a do-while')
                self.forms['do-while'] += 1
                return q
        # ---- loops: a backward jump further down comes back to p
        i = self.pick_back(p, end)
        if i is not None:
            entry = (self.lo, self.hi)
            kind, f_idx, depth = None, None, None
            try:
                kind, f_idx, depth = self.machine(p, i, dry=True)
            except Fail:
                kind = None
            if kind == 'jf' and self.target(f_idx) == i + 1 and depth == 1:
                self.machine(p, i)
                self.roles[f_idx] = 'loop.jump_false'
                self.roles[i] = 'loop.jump_back'
                f = Flow('loop', i + 1, p, i, self.hi)
                self.flow.append(f)
                self.seq(f_idx + 1, i)
                self.flow.pop()
                self.check_conts(f)
                self.clamp(entry, i, 'end of a loop body')
                self.forms['loop with condition (while / for)'] += 1
                return i + 1
            self.roles[i] = 'loop.jump_back'
            f = Flow('loop', i + 1, None, i, self.hi)
            self.flow.append(f)
            self.seq(p, i)
            self.flow.pop()
            self.check_conts(f)
            self.clamp(entry, i, 'end of a loop body')
            self.forms['loop without condition (for(;;))'] += 1
            return i + 1
        if c == DECLARE:
            k = nd[p].imm
            if not self.lo <= k <= self.hi:
                raise Fail('declare %d while %d..%d locals are live' % (k, self.lo, self.hi), p)
            if p + 1 >= end or code[p + 1] != TYPE:
                raise Fail('declare without a type carrier', p)
            self.set_live(k + 1)
            if p + 2 < end and code[p + 2] == INIT:
                kind, q, depth = self.machine(p + 3, end, stack=['v'])
                if kind != 'store' or depth != 2:
                    raise Fail('initialiser not closed by store (%s, depth %s)' % (kind, depth), q)
                self.forms['local with initialiser'] += 1
                return q + 1
            self.forms['local without initialiser'] += 1
            return p + 2
        if c == RELEASE or (c == JUMP and p not in self.sc):
            run = self.release_run(p, end) if c == RELEASE else []
            q = p + len(run)
            if q < end and code[q] == JUMP and q not in self.sc:
                ex = self.exit_flow(q)
                if ex is not None:
                    self.do_exit(p, q, run, ex[0], ex[1])
                    return q + 1
                if not run:
                    raise Fail('jump %+d fits no statement form' % signed(nd[q].imm), q)
            # scope end: one release per local of the scope that closes
            k = run[0]
            if not self.lo <= k + 1 <= self.hi:
                raise Fail('release %d while %d..%d locals are live' % (k, self.lo, self.hi), p)
            self.lo = self.hi = k
            self.forms['scope-end release'] += 1
            return p + 1
        if c == PUSH_VOID:
            run = self.release_run(p + 1, end)
            q = p + 1 + len(run)
            self.check_all(run, p, 'return')
            if q >= end or code[q] != RET_VALUE:
                raise Fail('push_void and releases not closed by ret_value', q)
            self.forms['return;'] += 1
            return q + 1
        if c == RET:
            run = self.release_run(p + 1, end)
            self.check_all(run, p, 'return in a constructor')
            self.forms['return; in a constructor'] += 1
            return p + 1 + len(run)
        # ---- everything else starts with an expression
        kind, q, info = self.machine(p, end)
        if kind == 'stmt':
            self.forms['expression statement'] += 1
            self.forms['expression statement: %s' % STMT_NAMES.get(info, 'assignment')] += 1
            return q + 1
        if kind == 'value':
            if info != 1:
                raise Fail('return value with %d operands' % info, q)
            run = self.release_run(q + 1, end)
            r = q + 1 + len(run)
            self.check_all(run, q, 'return expr')
            if r >= end or code[r] != RET_VALUE:
                raise Fail('value and releases not closed by ret_value', r)
            self.forms['return expr;'] += 1
            return r + 1
        if kind == 'jt':
            if q != do_j or info != 1:
                raise Fail('jump_true that closes no do-while', q)
            return q + 1
        if kind == 'jf':
            if info != 1:
                raise Fail('condition with %d operands' % info, q)
            return self.if_stmt(p, q, end)
        if kind == 'case':
            if info != 2:
                raise Fail('case with %d operands' % info, q)
            return self.switch(p, q, end)
        raise Fail('expression closed by %s at statement level' % kind, q)

    def check_all(self, run, pos, what):
        """A return releases every live local, highest first, down to 0."""
        if run:
            if run[-1] != 0 or not self.lo <= run[0] + 1 <= self.hi:
                raise Fail('%s releases %d..%d, %d..%d locals are live'
                           % (what, run[0], run[-1], self.lo, self.hi), pos)
            self.lo, self.hi = 0, run[0] + 1
        else:
            if self.lo > 0:
                raise Fail('%s without releases, %d locals are live' % (what, self.lo), pos)
            self.lo = 0

    def check_conts(self, f):
        for t in f.conts:
            if t != f.back and t not in self.starts:
                raise Fail('continue target %d is not a statement boundary' % t, t)

    def if_stmt(self, p, f, end):
        code = self.code
        X = self.target(f)
        if X <= f or X > end:
            raise Fail('jump_false leaves its region (%d, region ends at %d)' % (X, end), f)
        entry = (self.lo, self.hi)
        x1 = X - 1
        readings = ['if']
        if x1 > f and code[x1] == JUMP and x1 not in self.sc and self.target(x1) >= X:
            Y = self.target(x1)
            # the releases in front of the jump: do they reach below the
            # locals of the then-branch itself?
            r0 = x1
            while r0 - 1 > f and code[r0 - 1] == RELEASE and (
                    r0 == x1 or self.nd[r0 - 1].imm == self.nd[r0].imm + 1):
                r0 -= 1
            run = [self.nd[k].imm for k in range(r0, x1)]
            if run and code[r0 - 1] == RET:
                run = []                             # releases of `return;` in a constructor
            reaches_out = bool(run) and min(run) < entry[1]
            is_break = bool(self.flow) and Y == self.flow[-1].brk
            if reaches_out:
                readings = ['if', 'if-else']         # break / continue with releases
            elif is_break and not run and entry[0] <= self.flow[-1].mark:
                readings = ['if', 'if-else']         # A1: break first
            elif Y <= end:
                readings = ['if-else', 'if']
        first_fail = None
        for n, how in enumerate(readings):
            snap = self.snapshot() if n + 1 < len(readings) else None
            try:
                self.roles[f] = 'if.jump_false'
                if how == 'if-else':
                    self.roles[x1] = 'if.jump_else'
                    self.seq(f + 1, x1)
                    self.clamp(entry, x1, 'end of a then-branch')
                    self.seq(X, Y)
                    self.clamp(entry, Y, 'end of an else-branch')
                    self.forms['if-else'] += 1
                    return Y
                self.seq(f + 1, X)
                self.clamp(entry, X, 'end of a then-branch')
                self.forms['if'] += 1
                return X
            except Fail as x:
                if first_fail is None:
                    first_fail = x
                if snap is None:
                    raise first_fail
                self.restore(snap)
        raise first_fail

    def switch(self, p, q, end):
        code = self.code
        cases = [(q, self.target(q))]
        i = q + 1
        while True:
            if i >= end:
                raise Fail('switch dispatch runs off its region', i)
            if code[i] == POP:
                break
            kind, j, depth = self.machine(i, end, stack=['v'])
            if kind != 'case' or depth != 2:
                raise Fail('switch dispatch: %s with %s operands' % (kind, depth), j)
            cases.append((j, self.target(j)))
            i = j + 1
        if i + 1 >= end or code[i + 1] != JUMP:
            raise Fail('pop of a switch not followed by a jump', i)
        dj = i + 1
        D = self.target(dj)
        body = dj + 1
        targets = [t for _, t in cases]
        if targets != sorted(targets):
            raise Fail('case targets do not ascend', q)
        if targets[0] != body and D != body:
            raise Fail('first case group does not start behind the dispatch', q)
        last_label = max(targets)
        stmt_jumps = lambda a, b: [k for k in range(a, b)
                                   if code[k] == JUMP and k not in self.sc]
        if D <= last_label:
            # the default group stands among the case groups
            has_default = True
            labels = sorted(set(targets + [D]))
            # the end is where the breaks of the groups before the last one go
            cand = [self.target(k) for k in stmt_jumps(body, last_label)
                    if self.target(k) > last_label]
            if not cand:
                cand = [self.target(k) for k in stmt_jumps(last_label, end)
                        if k < self.target(k) <= end]
                if not cand:
                    raise Fail('switch with default: no break shows where it ends', dj)
                E = max(cand)
            else:
                E = min(cand)
        else:
            # D lies behind every case group: it is the end of the switch, or
            # the start of a default group that comes last.  A break in a case
            # group that jumps further than D shows the second.
            cand = [self.target(k) for k in stmt_jumps(body, D) if self.target(k) > D]
            if cand:
                has_default = True
                labels = sorted(set(targets)) + [D]
                last_label = D
                E = min(cand)
            else:
                has_default = False
                labels = sorted(set(targets))
                E = D
        if E > end or E < last_label:
            raise Fail('switch ends at %d, outside its region' % E, dj)
        for j, t in cases:
            self.roles[j] = 'switch.case'
        self.roles[dj] = 'switch.jump_default'
        f = Flow('switch', E, None, None, self.hi)
        entry = (self.lo, self.hi)
        self.flow.append(f)
        seen = Counter(targets)
        for n, L in enumerate(labels):
            stop = labels[n + 1] if n + 1 < len(labels) else E
            self.clamp(entry, L, 'start of a case group')
            self.seq(L, stop)
            self.forms['case group'] += 1
            if seen[L] > 1 or (L == D and has_default and seen[L] >= 1):
                self.forms['case group with several labels'] += 1
            if L == stop:
                self.forms['case group without statements'] += 1
            elif stop != E:
                k = stop - 1
                while k > L and code[k] == RELEASE:
                    k -= 1
                if not (code[k] in (JUMP, RET_VALUE, RET) and k not in self.sc):
                    self.forms['case group that falls through into the next'] += 1
        self.flow.pop()
        self.clamp(entry, E, 'end of a switch')
        self.forms['switch'] += 1
        self.forms['case label'] += len(cases)
        if has_default:
            self.forms['default label'] += 1
            if D != labels[-1]:
                self.forms['default label not last'] += 1
        else:
            self.forms['switch without default'] += 1
        return E

    # ---- trees ----------------------------------------------------------
    def epilogue(self, p):
        """Is nodes[p:] the end of the method for some number L of live
        locals?  Returns L or None."""
        code, nd = self.code, self.nd
        rest = self.N - p
        if self.is_ctor:
            if rest < 1 or code[p] != RET or (rest - 1) % 2:
                return None
            L = (rest - 1) // 2
            segs = [(p + 1, L), (p + 1 + L, L)]
        elif self.ret_void:
            if rest < 2 or code[p] != PUSH_VOID or (rest - 2) % 2:
                return None
            L = (rest - 2) // 2
            if code[p + 1 + L] != RET_VALUE:
                return None
            segs = [(p + 1, L), (p + 2 + L, L)]
        else:
            L = rest
            segs = [(p, L)]
        if not self.lo <= L <= self.hi:
            return None
        for a, n in segs:
            for k in range(n):
                if code[a + k] != RELEASE or nd[a + k].imm != L - 1 - k:
                    return None
        return L

    def run(self):
        if self.kind == 'field':
            kind, q, depth = self.machine(0, self.N)
            if kind != 'end' or depth != 1 or q != self.N - 1:
                raise Fail('field initialiser is not one expression and end', q)
            self.forms['field initialiser'] += 1
            return
        if self.N == 0:
            self.forms['empty tree'] += 1
            return
        p = 0
        last = None
        while True:
            L = self.epilogue(p)
            if L is not None:
                break
            if p >= self.N:
                raise Fail('tree ends without the method epilogue', p)
            self.starts.add(p)
            last = p
            p = self.stmt(p, self.N)
        self.body_end = p
        if last is not None and (self.is_ctor or self.ret_void) and \
                self.code[last] in (PUSH_VOID, RET):
            # an explicit `return;` as the last statement, and the implicit
            # return still follows it
            self.forms['explicit return; directly in front of the implicit return'] += 1
        if self.is_ctor:
            self.forms['constructor: ret + releases'] += 1
        elif self.ret_void:
            self.forms['void method: implicit return'] += 1
        else:
            self.forms['value method: releases only'] += 1
        if self.is_ctor:
            c = self.code
            n = [i for i in range(self.N) if c[i] in (THIS_INIT, SUPER_INIT)]
            if len(n) != 1:
                raise Fail('constructor with %d this_init/super_init' % len(n), 0)
        for i in range(self.N):
            if self.code[i] in (JUMP, JUMP_FALSE, JUMP_TRUE, CASE) and i not in self.roles:
                raise Fail('jump without a role', i)
