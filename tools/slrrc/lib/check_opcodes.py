#!/usr/bin/env python3
"""check_opcodes.py - proof 3: the opcode table (opcodes.py) against the corpus.

    python -I check_opcodes.py [--corpus DIR] [--json OUT] [--max-list N]

Three parts.

1. Structure.  Where each opcode may stand and what must follow it:
   S1   an expr/stmt tag that takes a carrier is followed by that carrier
   S2   literal carriers stand only behind expr/stmt literal
   S3   0x1006 type stands only behind declare, new, newarray, instanceof, cast
   S4   0x1019 class stands only as the first segment of a path of >= 2
        segments or as the carrier of a cast
   S5   declare is followed by type; 0x002C init stands only two nodes after a
        declare
   S6   0x0024 call is preceded by push_int and followed by a lone method
        segment or by a path header; the last segment is method (0x101A) or
        name (0x4012); the node after it is expr/stmt call
   S7   inside a call path: first segment local/field/class/name, middle
        segments field/name, 0x101A only last
   S8   inside a field path (executed 0x1011): first segment local/field/class,
        the others field/name, never 0x101A
   S9   0x101A and 0x4012 stand only inside paths
   S10  0x4025 / 0x4026 are followed by expr/stmt call; expr/stmt call stands
        only behind the three call forms
   S11  0x0021 0x0022 0x0023 0x4026 are preceded by push_int; 0x0022/0x0023
        are followed by stmt ctor and stmt ctor stands only there
   S12  jump targets lie inside the tree (0..node count) and never on a node
        that another node consumes
   S13  and_test / or_test are followed by a jump whose target is the node
        after the matching expr land / lor
   S14  0x0018 pop is preceded by case and followed by jump
   S15  bool carriers hold 0 or 1
   S16  the argument count in push_int equals the parameter count of the
        descriptor when the call ends in a 0x101A method ref
   S17  no expr/stmt tag outside the set the table marks as emitted
   S18  ret (0x002A) stands only in constructors; ret_value, push_void and
        value never do
   S19  every constructor with code holds exactly one this_init/super_init
        (INFO: how many start with "push_int 0; super_init; stmt ctor")
   S20  value and push_void are followed by releases and then ret_value
   S21  parameter numbering: where a parameter is the receiver of a member
        ref in a path, its descriptor type under "last parameter = lowest
        index" names the class of the ref (S21f: the same test with the
        opposite numbering, as a control)

2. Simulation.  Every tree is walked along all its control-flow paths with an
   abstract operand stack and local count, using the pops/pushes of the table:
   X1   no node pops more than the stack holds
   X2   the stack depth and the local count agree wherever paths join
   X3   end is reached with exactly 1 value, ret with 0, ret_value with 1
   X4   the stack is empty after every stmt node and every store
   X5   declare's index is the number of live locals, release's index is the
        top local, local's index is below the live count (also in paths)
   X6   at ret_value no local is live; no path runs off the end of a tree
   plus measurements: unreachable nodes by opcode.

3. Token census and line check (paircheck.py).  For every source/class pair
   of Build 940 the tokens of the source (srclex.py) are counted per token
   class and compared with the node counts of the class; a class "agrees" for
   a file when both counts are equal.  This ties the expr/stmt tags to the
   operators of the language.  The line check takes every node that carries a
   literal or a member name and looks for that token on the source line the
   node records, over all pairs.
"""
import json
import os
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import disasm   # noqa: E402
import opcodes as oc  # noqa: E402
import oracle   # noqa: E402
import paircheck  # noqa: E402
import tufa     # noqa: E402

NONE = tufa.NONE
LIT = set(oc.LITERAL_CARRIERS)
TAGGED = (0x1007, 0x1008)
EMITTED_TAGS = set(t.tag for t in oc.TAGS if 'Never emitted' not in t.text)


class Rule(object):
    def __init__(self, rid, text, must=True):
        self.rid, self.text, self.must = rid, text, must
        self.ok = 0
        self.total = 0
        self.bad = []

    def check(self, cond, where=None):
        self.total += 1
        if cond:
            self.ok += 1
        elif where is not None and len(self.bad) < 500:
            self.bad.append(where() if callable(where) else where)
        return cond


# The two places of the corpus where the compiler itself emitted something
# irregular.  Both sit in class files without a source (the runtime library).
KNOWN = {
    'S6b': {'system/scripts/util/resource/Texture.class#0 tree 1 node 5':
            'call followed by a bare 0x1001: the callee name resolved to a '
            'local variable (Texture.<init>(I), line 21)'},
    'S10b': {'system/scripts/util/resource/Texture.class#0 tree 1 node 7':
             'the expr call that follows the same malformed call'},
    'X3': {'system/scripts/render/Frontend.class#0 tree 16 node 28':
           'the only "new T(...);" used as a statement: stmt new leaves the '
           'object on the stack (Frontend, line 100)'},
}


def tag_of(nodes, codes, i):
    return nodes[i].imm if 0 <= i < len(nodes) and codes[i] in TAGGED else None


def simulate(nodes, codes, eaten, base_locals, rules, where, stats):
    """Abstract interpretation over the control-flow graph of one tree."""
    X1, X2, X3, X4, X5, X6 = rules
    n = len(nodes)
    if n == 0:
        return set()
    state = {}                      # node index -> (stack tuple, live)
    work = [(0, (), base_locals)]
    reached = set()

    def nxt(i):
        j = i + 1
        while j < n and j in eaten:
            j += 1
        return j

    def flow(j, stack, live):
        if j > n:
            return
        if j == n:
            X6.check(False, lambda: '%s runs off the end (depth %d, locals %d)'
                     % (where, len(stack), live))
            return
        old = state.get(j)
        if old is None:
            state[j] = (stack, live)
            work.append((j, stack, live))
        else:
            X2.check(len(old[0]) == len(stack) and old[1] == live,
                     lambda: '%s node %d: depth %d/%d locals %d/%d' % (
                         where, j, len(old[0]), len(stack), old[1], live))

    state[0] = ((), base_locals)
    while work:
        i, stack, live = work.pop()
        if i in reached:
            continue
        reached.add(i)
        nd = nodes[i]
        c = codes[i]
        st = list(stack)
        ok = [True]

        def pop(k, i=i, st=st, ok=ok):
            if len(st) < k:
                ok[0] = False
                X1.check(False, lambda: '%s node %d (0x%04X): needs %d, has %d'
                         % (where, i, codes[i], k, len(st)))
                del st[:]
                return
            if k:
                del st[-k:]

        def pop_args(i=i, st=st, ok=ok):
            if not st or not isinstance(st[-1], tuple):
                ok[0] = False
                X1.check(False, lambda: '%s node %d (0x%04X): no argc box on top'
                         % (where, i, codes[i]))
                del st[:]
                return None
            k = st.pop()[1]
            pop(k)
            return k

        j = nxt(i)
        succ = None
        stats['simulated']['nodes'] += 1
        if c == 0x1001:
            X5.check(nd.imm < live, lambda: '%s node %d local %d of %d' % (
                where, i, nd.imm, live))
            st.append('v')
        elif c == 0x1002:
            X5.check(nd.imm == live, lambda: '%s node %d declare %d with %d live'
                     % (where, i, nd.imm, live))
            live += 1
            if i + 2 < n and codes[i + 2] == 0x002c and codes[i + 1] == 0x1006:
                st.append('v')
        elif c == 0x1003:
            X5.check(live > 0 and nd.imm == live - 1,
                     lambda: '%s node %d release %d with %d live' % (
                         where, i, nd.imm, live))
            live -= 1
        elif c in (0x0004, 0x0005):
            pop(1)
            st.append('v')
            succ = [i + 1, i + 2]
        elif c == 0x1007:
            t = oc.tag_info(nd.imm)
            if t is None or t.pops_e is None:
                X1.check(False, lambda: '%s node %d: expr tag 0x%X has no '
                         'stack effect' % (where, i, nd.imm))
            else:
                pop(t.pops_e)
                st.extend(['v'] * t.pushes_e)
        elif c == 0x1008:
            t = oc.tag_info(nd.imm)
            if t is None or t.pops_s is None:
                X1.check(False, lambda: '%s node %d: stmt tag 0x%X has no '
                         'stack effect' % (where, i, nd.imm))
            else:
                pop(t.pops_s)
                if t.name == 'new':
                    st.append('v')
                else:
                    X4.check(len(st) == 0, lambda: '%s node %d stmt %s leaves '
                             'depth %d' % (where, i, t.name, len(st)))
                    stats['stmt_tag_depth_after'][(t.name, len(st))] += 1
        elif c == 0x0010:
            X3.check(len(st) == 1, lambda: '%s node %d end with depth %d' % (
                where, i, len(st)))
            succ = []
        elif c == 0x1011:
            for k in range(i + 1, min(n, i + 1 + nd.imm)):
                if codes[k] == 0x1001:
                    X5.check(nodes[k].imm < live, lambda: '%s node %d path local '
                             '%d of %d' % (where, k, nodes[k].imm, live))
            st.append('v')
        elif c == 0x1014:
            succ = [i + _signed(nd.imm)]
        elif c in (0x1015, 0x1016):
            pop(1)
            succ = [j, i + _signed(nd.imm)]
        elif c == 0x1017:
            pop(2)
            if ok[0]:
                flow(i + _signed(nd.imm), tuple(st), live)
            st.append('v')
        elif c == 0x0018:
            pop(1)
        elif c == 0x101b:
            st.append('v')
        elif c == 0x401c:
            pop(1)
            st.append('v')
        elif c == 0x0020:
            pop(2)
            st.append('v')
        elif c == 0x0021:
            pop_args()
            if ok[0] and len(st) < 1:
                ok[0] = False
                X1.check(False, lambda: '%s node %d new_init without an '
                         'object below the arguments' % (where, i))
        elif c in (0x0022, 0x0023):
            pop_args()
        elif c == 0x0024:
            # locals named inside the call path
            hdr = i + 1
            if hdr < n and codes[hdr] != 0x101a and nodes[hdr].imm is not None:
                for k in range(hdr + 1, min(n, hdr + 1 + nodes[hdr].imm)):
                    if codes[k] == 0x1001:
                        X5.check(nodes[k].imm < live, lambda: '%s node %d call '
                                 'path local %d of %d' % (where, k, nodes[k].imm, live))
            pop_args()
            st.append('v')
        elif c == 0x4025:
            pop(1)
            pop_args()
            st.append('v')
        elif c == 0x4026:
            pop_args()
            st.append('v')
        elif c == 0x1027:
            st.append(('n', nd.imm))
        elif c == 0x0028:
            pop(1)
            st.append('v')
        elif c == 0x0029:
            st.append('v')
        elif c == 0x002a:
            X3.check(len(st) == 0, lambda: '%s node %d ret with depth %d' % (
                where, i, len(st)))
            stats['locals_live_at_ret'][live - base_locals] += 1
            succ = []
        elif c == 0x002b:
            X3.check(len(st) == 1, lambda: '%s node %d ret_value with depth %d'
                     % (where, i, len(st)))
            X6.check(live == 0, lambda: '%s node %d ret_value with %d locals '
                     'live' % (where, i, live))
            succ = []
        elif c == 0x002d:
            pop(2)
            X4.check(len(st) == 0, lambda: '%s node %d store leaves depth %d' % (
                where, i, len(st)))
        else:
            X1.check(False, lambda: '%s node %d: opcode 0x%04X reached by the '
                     'main loop' % (where, i, c))
        if not ok[0]:
            continue
        X1.check(True)
        if succ is None:
            succ = [j]
        for s in succ:
            flow(s, tuple(st), live)
    return reached


def _signed(v):
    return v - (1 << 32) if v & 0x80000000 else v


def _class_of(desc):
    """Name the pool gives the class of a value of this descriptor:
    'Ljava.lang.String;' -> 'java.lang.String'; arrays keep their brackets
    and drop the L...; wrapper: '[Ljava.lang.String;' -> '[java.lang.String',
    '[I' -> '[I'."""
    inner = desc.lstrip('[')
    dims = len(desc) - len(inner)
    if inner.startswith('L') and inner.endswith(';'):
        inner = inner[1:-1]
    return '[' * dims + inner


def main(argv):
    out_json = None
    max_list = 10
    if '--corpus' in argv:
        os.environ['SLRR_CORPUS'] = argv[argv.index('--corpus') + 1]
    if '--json' in argv:
        out_json = argv[argv.index('--json') + 1]
    if '--max-list' in argv:
        max_list = int(argv[argv.index('--max-list') + 1])

    R = {}

    def rule(rid, text, must=True):
        R[rid] = Rule(rid, text, must)
        return R[rid]

    S1 = rule('S1', 'tags with a carrier are followed by it')
    S2 = rule('S2', 'literal carriers only behind expr/stmt literal')
    S3 = rule('S3', '0x1006 only behind declare, new, newarray, instanceof, cast')
    S4 = rule('S4', '0x1019 only first in a path of >= 2 segments or as cast carrier')
    S5a = rule('S5a', 'declare is followed by type')
    S5b = rule('S5b', '0x002C only two nodes after a declare')
    S6a = rule('S6a', 'call is preceded by push_int')
    S6b = rule('S6b', 'call is followed by a lone method ref or a path header '
               'whose last segment is method/name')
    S6c = rule('S6c', 'the node after a call path is expr/stmt call')
    S7 = rule('S7', 'call path segments: first local/field/class/name, middle '
              'field/name, method only last')
    S8 = rule('S8', 'field path segments: first local/field/class, rest field/name')
    S9 = rule('S9', '0x101A and 0x4012 only inside paths')
    S10a = rule('S10a', '0x4025/0x4026 are followed by expr/stmt call')
    S10b = rule('S10b', 'expr/stmt call only behind a call form')
    S11a = rule('S11a', '0x0021 0x0022 0x0023 0x4026 are preceded by push_int')
    S11b = rule('S11b', '0x0022/0x0023 are followed by stmt ctor')
    S11c = rule('S11c', 'stmt ctor only behind 0x0022/0x0023; expr ctor never')
    S12 = rule('S12', 'jump targets inside the tree and not on a consumed node')
    S13 = rule('S13', 'and_test/or_test + jump land behind the matching expr land/lor')
    S14 = rule('S14', 'pop is preceded by case and followed by jump')
    S15 = rule('S15', 'bool carriers hold 0 or 1')
    S16 = rule('S16', 'push_int before a 0x101A call equals the descriptor\'s '
               'parameter count')
    S17 = rule('S17', 'expr/stmt tags are in the emitted set of the table')
    S18 = rule('S18', 'ret only in constructors; ret_value/push_void/value never there')
    S19 = rule('S19', 'a constructor with code holds exactly one this_init/super_init')
    S20 = rule('S20', 'value and push_void are followed by releases, then ret_value')
    S21 = rule('S21', 'parameter receivers: type under reversed numbering equals '
               'the class of the member ref')
    S21f = rule('S21f', '... the same test with forward numbering (control)',
                must=False)
    X1 = rule('X1', 'no node pops more than the stack holds')
    X2 = rule('X2', 'depth and local count agree at joins')
    X3 = rule('X3', 'end with 1 value, ret with 0, ret_value with 1')
    X4 = rule('X4', 'stack empty after every stmt and store')
    X5 = rule('X5', 'declare/release/local indices follow the live local count')
    X6 = rule('X6', 'no local live at ret_value; no path runs off the end')

    stats = defaultdict(Counter)
    idx = {e.cls: e for e in oracle.load_index() if e.cls}
    ntrees = 0
    nnodes = 0
    for rel, blobs in oracle.iter_class_files():
        for bi, b in enumerate(blobs):
            own = {}
            for st, i, m in b.methods():
                if m.tree != NONE:
                    own[m.tree] = ('M', st, m)
            for st, i, f in b.fields():
                if f.init != NONE:
                    own[f.init] = ('F', st, f)
            for ti, nodes in enumerate(b.trees):
                ntrees += 1
                n = len(nodes)
                nnodes += n
                codes = [tufa.OP_FLAGS[x.op] for x in nodes]
                eaten = disasm.consumed_nodes(nodes)
                where = '%s#%d tree %d' % (rel, bi, ti)
                W = lambda i: '%s node %d' % (where, i)
                in_call_path = set()
                in_field_path = set()
                call_after = set()      # nodes right after a call form
                o = own.get(ti)
                is_ctor = False
                own_params = []
                is_static = False
                if o is not None and o[0] == 'M':
                    is_ctor = b.utf8(o[2].name) == '<init>'
                    is_static = o[1]
                    try:
                        own_params, _ = disasm.split_descriptor(b.utf8(o[2].desc))
                    except ValueError:
                        own_params = []
                if is_ctor and n:
                    k = sum(1 for c in codes if c in (0x0022, 0x0023))
                    S19.check(k == 1, lambda: '%s has %d' % (where, k))
                    stats['ctor_start'][
                        'push_int 0; super_init; stmt ctor' if n >= 3 and
                        codes[0] == 0x1027 and nodes[0].imm == 0 and
                        codes[1] == 0x0023 else
                        'this_init' if 0x0022 in codes else
                        'super_init with arguments'] += 1
                for i, c in enumerate(codes):
                    nd = nodes[i]
                    nx = codes[i + 1] if i + 1 < n else None
                    pv = codes[i - 1] if i > 0 else None
                    if c in TAGGED:
                        stats['tag_expr' if c == 0x1007 else 'tag_stmt'][nd.imm] += 1
                        S17.check(nd.imm in EMITTED_TAGS, lambda: W(i))
                        t = oc.tag_info(nd.imm)
                        if t is not None and t.carrier:
                            if t.carrier == 'literal':
                                good = nx in LIT
                            elif t.carrier == 'type':
                                good = nx == 0x1006
                            else:
                                good = nx in (0x1006, 0x1019)
                            S1.check(good, lambda: W(i))
                            if good:
                                stats['carrier_of_%s' % t.name][nx] += 1
                    if c in LIT:
                        S2.check(tag_of(nodes, codes, i - 1) == 0x1C, lambda: W(i))
                        if c == 0x100d:
                            S15.check(nd.imm in (0, 1), lambda: W(i))
                    if c == 0x1006:
                        S3.check(pv == 0x1002 or tag_of(nodes, codes, i - 1) in (
                            0x1E, 0x1F, 0x20, 0x21), lambda: W(i))
                    if c == 0x1002:
                        S5a.check(nx == 0x1006, lambda: W(i))
                    if c == 0x002c:
                        S5b.check(i >= 2 and codes[i - 2] == 0x1002 and
                                  codes[i - 1] == 0x1006, lambda: W(i))
                    if c == 0x0024:
                        S6a.check(pv == 0x1027, lambda: W(i))
                        last = None
                        if nx == 0x101a:
                            last = i + 1
                            in_call_path.add(i + 1)
                            stats['call_form']['lone method ref'] += 1
                        elif nx == 0x1011:
                            cnt = nodes[i + 1].imm
                            last = i + 1 + cnt
                            segs = list(range(i + 2, i + 2 + cnt))
                            good = cnt >= 1 and last < n
                            if good:
                                for q, s in enumerate(segs):
                                    in_call_path.add(s)
                                    sc = codes[s]
                                    if q == len(segs) - 1:
                                        okk = sc in (0x101a, 0x4012)
                                    elif q == 0:
                                        okk = sc in (0x1001, 0x101b, 0x1019, 0x4012)
                                    else:
                                        okk = sc in (0x101b, 0x4012)
                                    if q == 1 and codes[segs[0]] == 0x1019:
                                        okk = okk or sc in (0x1001,)
                                    S7.check(okk, lambda: '%s seg %d op 0x%04X' % (W(i), q, sc))
                                stats['call_path_len'][cnt] += 1
                                stats['call_last'][codes[last]] += 1
                                stats['call_first'][codes[segs[0]]] += 1
                            else:
                                last = None
                        S6b.check(last is not None and codes[last] in (0x101a, 0x4012),
                                  lambda: W(i))
                        if last is not None and last + 1 <= n:
                            S6c.check(tag_of(nodes, codes, last + 1) == 0x1B,
                                      lambda: W(i))
                            call_after.add(last + 1)
                            if codes[last] == 0x101a:
                                try:
                                    params, _ = disasm.split_descriptor(
                                        b.ref(nodes[last].imm)[2])
                                    S16.check(nodes[i - 1].imm == len(params),
                                              lambda: W(i))
                                except (ValueError, tufa.TufaError):
                                    S16.check(False, lambda: W(i))
                    if c == 0x1011 and i not in eaten:
                        cnt = nd.imm
                        segs = list(range(i + 1, i + 1 + cnt))
                        good = cnt >= 1 and i + cnt < n
                        if good:
                            for q, s in enumerate(segs):
                                in_field_path.add(s)
                                sc = codes[s]
                                if q == 0:
                                    okk = sc in (0x1001, 0x101b, 0x1019)
                                else:
                                    okk = sc in (0x101b, 0x4012)
                                S8.check(okk, lambda: '%s seg %d op 0x%04X' % (W(i), q, sc))
                            stats['field_path_len'][cnt] += 1
                            stats['field_path_first'][codes[segs[0]]] += 1
                        else:
                            S8.check(False, lambda: W(i))
                    if c in (0x4025, 0x4026):
                        S10a.check(tag_of(nodes, codes, i + 1) == 0x1B, lambda: W(i))
                        call_after.add(i + 1)
                    if c in (0x0021, 0x0022, 0x0023, 0x4026):
                        S11a.check(pv == 0x1027, lambda: W(i))
                    if c in (0x0022, 0x0023):
                        S11b.check(nx == 0x1008 and nodes[i + 1].imm == 0x22,
                                   lambda: W(i))
                    if c in TAGGED and nd.imm == 0x22:
                        S11c.check(c == 0x1008 and pv in (0x0022, 0x0023),
                                   lambda: W(i))
                    if c in oc.JUMP_OPS:
                        tgt = i + _signed(nd.imm)
                        S12.check(0 <= tgt <= n and tgt not in eaten, lambda: W(i))
                        stats['jump_direction_0x%04X' % c][
                            'back' if tgt <= i else 'forward'] += 1
                    if c in (0x0004, 0x0005):
                        want = 0x02 if c == 0x0004 else 0x01
                        good = nx == 0x1014
                        if good:
                            tgt = i + 1 + _signed(nodes[i + 1].imm)
                            good = 1 <= tgt <= n and codes[tgt - 1] in TAGGED and \
                                nodes[tgt - 1].imm == want
                        S13.check(good, lambda: W(i))
                    if c == 0x0018:
                        S14.check(pv == 0x1017 and nx == 0x1014, lambda: W(i))
                    if c == 0x002a:
                        S18.check(is_ctor, lambda: W(i))
                    if c in (0x002b, 0x0029, 0x0028):
                        S18.check(not is_ctor, lambda: W(i))
                    if c in (0x0028, 0x0029):
                        q = i + 1
                        while q < n and codes[q] == 0x1003:
                            q += 1
                        S20.check(q < n and codes[q] == 0x002b, lambda: W(i))
                    if c == 0x1001 and own_params and i + 1 < n and \
                            codes[i + 1] in (0x101a, 0x101b) and \
                            (i in in_call_path or i in in_field_path or
                             pv in (0x1011,)):
                        base = 0 if is_static else 1
                        k = nd.imm - base
                        if 0 <= k < len(own_params):
                            want = b.ref(nodes[i + 1].imm)[0]
                            S21.check(_class_of(own_params[len(own_params) - 1 - k]) == want,
                                      lambda: '%s local %d %s vs %s' % (
                                          W(i), nd.imm,
                                          own_params[len(own_params) - 1 - k], want))
                            S21f.check(_class_of(own_params[k]) == want)
                for i, c in enumerate(codes):
                    if c == 0x1019:
                        castc = tag_of(nodes, codes, i - 1) == 0x21
                        first = (i in in_call_path and codes[i - 1] == 0x1011 and
                                 nodes[i - 1].imm >= 2) or \
                                (i in in_field_path and codes[i - 1] == 0x1011 and
                                 nodes[i - 1].imm >= 2)
                        S4.check(castc or first, lambda: W(i))
                    if c in (0x101a, 0x4012):
                        S9.check(i in in_call_path or i in in_field_path, lambda: W(i))
                    if c in TAGGED and nodes[i].imm == 0x1B:
                        S10b.check(i in call_after, lambda: W(i))
                # ---- simulation
                o = own.get(ti)
                base = 0
                if o is not None and o[0] == 'M':
                    try:
                        params, _ = disasm.split_descriptor(b.utf8(o[2].desc))
                        base = len(params) + (0 if o[1] else 1)
                    except ValueError:
                        base = 0
                reached = simulate(nodes, codes, eaten, base,
                                   (X1, X2, X3, X4, X5, X6), where, stats)
                for i in range(n):
                    if i not in reached and i not in eaten:
                        stats['unreachable'][codes[i]] += 1
                stats['nodes']['reached'] += len(reached)
                stats['nodes']['consumed'] += len(eaten)
                stats['nodes']['unreachable'] += n - len(reached) - len(
                    [x for x in eaten if x < n])

    # ---- token census over the Build 940 pairs, line check over all pairs
    CENSUS = paircheck.CENSUS
    census = {lab: {'src': 0, 'cls': 0, 'files': 0, 'agree': 0, 'bad': []}
              for lab, _, _ in CENSUS}
    nfiles = 0
    file_bad = Counter()
    super_tokens = 0
    super_nodes = 0
    super_bad = []
    line_tot = Counter()
    line_bad = Counter()
    line_files = defaultdict(set)
    npairs = 0
    for e in oracle.iter_pairs(build=None):
        npairs += 1
        text = oracle.load_source(e)
        blobs = oracle.load_class(e)
        bad, tot, _ = paircheck.line_check(text, blobs)
        for k, v in tot.items():
            line_tot[k] += v
        for k, v in bad.items():
            line_bad[k] += v
            line_files[k].add(e.cls)
        if e.category != 'pair_940':
            continue
        nfiles += 1
        sc = paircheck.source_counts(text)
        cc = paircheck.class_counts(blobs)
        n23 = sum(1 for b in blobs for t in b.trees for nd in t
                  if tufa.OP_FLAGS[nd.op] == 0x0023)
        super_tokens += sc.get('super(', 0)
        super_nodes += n23
        if n23 < sc.get('super(', 0):
            super_bad.append(e.cls)
        for lab, key, _ in CENSUS:
            a = sc.get(key, 0)
            bq = cc.get(key)
            d = census[lab]
            d['src'] += a
            d['files'] += 1
            if bq is None:              # token class without a node
                d['agree'] += 1
                continue
            d['cls'] += bq
            if a == bq:
                d['agree'] += 1
            else:
                d['bad'].append((e.cls, a, bq))
                file_bad[e.cls] += 1

    # ---- report
    print('corpus  %s' % oracle.corpus_root())
    print('trees   %d   nodes %d' % (ntrees, nnodes))
    print('')
    failed = []
    for rid, r in R.items():
        known = [w for w in r.bad if any(str(w).startswith(k) for k in KNOWN.get(rid, ()))]
        unknown = [w for w in r.bad if w not in known]
        if r.ok == r.total:
            status = 'holds'
        elif not unknown and len(known) == r.total - r.ok:
            status = 'holds*'
        else:
            status = 'FAILS' if r.must else 'info'
        print('%-5s %-6s %d of %d  %s' % (rid, status, r.ok, r.total, r.text))
        for w in known:
            reason = [v for k, v in KNOWN[rid].items() if str(w).startswith(k)][0]
            print('        * known exception: %s (%s)' % (w, reason))
        for w in unknown[:max_list]:
            print('        - %s' % (w,))
        if len(unknown) > max_list:
            print('        ... %d more' % (len(unknown) - max_list))
        if r.must and unknown:
            failed.append(rid)
    print('')
    for name in sorted(stats):
        print('%s:' % name)
        for k, v in sorted(stats[name].items(), key=lambda kv: str(kv[0])):
            ks = '0x%04X' % k if isinstance(k, int) and (
                name.startswith(('carrier', 'call_last', 'call_first',
                                 'field_path_first', 'unreachable'))) else (
                '0x%02X' % k if isinstance(k, int) and name.startswith('tag_') else str(k))
            print('  %-28s %d' % (ks, v))
    print('')
    print('token census over %d Build 940 pairs' % nfiles)
    print('  %-22s %9s %9s  %-16s %s' % ('token', 'source', 'class',
                                         'files agreeing', 'class side'))
    for lab, key, ckey in CENSUS:
        d = census[lab]
        print('  %-22s %9d %9s  %5d of %-7d %s' % (
            lab, d['src'], '-' if ckey == 'no node' else d['cls'],
            d['agree'], d['files'], ckey))
    print('  super( tokens %d, super_init nodes %d: %d super_init nodes have no '
          'token (implicit super()); files with fewer nodes than tokens: %d'
          % (super_tokens, super_nodes, super_nodes - super_tokens, len(super_bad)))
    print('  files with at least one disagreement: %d' % len(file_bad))
    for f in sorted(file_bad):
        which = [(lab, a, bq) for lab, _, _ in CENSUS
                 for (ff, a, bq) in census[lab]['bad'] if ff == f]
        print('    %s: %s' % (f, '; '.join('%s source %d class %d' % w for w in which)))
    print('')
    print('line check over %d pairs (all builds): node token found on the '
          'recorded source line' % npairs)
    for k in ('str', 'int', 'float', 'rid', 'name', 'line'):
        if line_tot.get(k) or line_bad.get(k):
            print('  %-6s %7d of %7d   exceptions in %d files' % (
                k, line_tot[k] - line_bad[k], line_tot[k], len(line_files[k])))
    allbad = set()
    for v in line_files.values():
        allbad |= v
    print('  files with an exception: %d' % len(allbad))
    for f in sorted(allbad):
        print('    %s: %s' % (f, ', '.join('%s' % k for k in sorted(line_files)
                                           if f in line_files[k])))
    if out_json:
        doc = {'trees': ntrees, 'nodes': nnodes,
               'rules': {rid: {'text': r.text, 'must': r.must, 'ok': r.ok,
                               'total': r.total, 'exceptions': r.bad[:100]}
                         for rid, r in R.items()},
               'known_exceptions': KNOWN,
               'stats': {name: {str(k): v for k, v in h.items()}
                         for name, h in stats.items()},
               'census': {lab: {'source': census[lab]['src'], 'class': census[lab]['cls'],
                                'files': census[lab]['files'],
                                'agree': census[lab]['agree'],
                                'class_side': ckey,
                                'disagree': census[lab]['bad'][:50]}
                          for lab, _, ckey in CENSUS},
               'census_files': nfiles,
               'super_tokens': super_tokens, 'super_nodes': super_nodes,
               'census_files_disagreeing': sorted(file_bad),
               'line_check': {'pairs': npairs, 'total': dict(line_tot),
                              'bad': dict(line_bad),
                              'files': {k: sorted(v) for k, v in line_files.items()}}}
        with open(out_json, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(doc, f, indent=1)
    print('')
    print('FAILED: %s' % ', '.join(failed) if failed else
          'all MUST rules hold (rules marked holds* have only the known exceptions)')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
