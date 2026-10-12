#!/usr/bin/env python3
"""gen_opcodes_md.py - write notes/opcodes.md: the opcode reference for a
compiler writer, from the table in opcodes.py, the corpus counts and the
results of check_opcodes.py (notes/opcode_checks.json, run that first).

    python -I check_opcodes.py --json ../notes/opcode_checks.json
    python -I gen_opcodes_md.py
"""
import json
import os
import sys
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import disasm   # noqa: E402
import opcodes as oc  # noqa: E402
import oracle   # noqa: E402
import tufa     # noqa: E402

OUT = os.path.join(oracle.NOTES_DIR, 'opcodes.md')
CHECKS = os.path.join(oracle.NOTES_DIR, 'opcode_checks.json')

IMM_TEXT = {
    'none': '-',
    'local': 'local index',
    'tag': 'expression kind (table 2)',
    'utf8': 'pool: utf8',
    'rid': 'pool: rid',
    'class': 'pool: class',
    'ref': 'pool: member ref',
    'int': 'int value',
    'float': 'float bits',
    'count': 'count',
    'jump': 'signed node offset',
}

# (title, class file, blob, tree, first node, last node, first line, last line, note)
EXAMPLES = [
    ('Field initialiser with `new` and a resource id',
     'sl/Scripts/game/Marker.class', 0, 0, 0, 6, 12, 12,
     'One tree per initialised field, closed by `end`. `expr new` pushes the '
     'raw object, the arguments and the count follow, `new_init` runs the '
     'constructor and leaves the object as the value.'),
    ('Constructor: implicit `super()`, field store, return',
     'sl/Scripts/game/Marker.class', 0, 19, 0, 10, 38, 41,
     'Assignment is compiled right-hand side first. Locals: 0 = this, 1 = s. '
     'After `ret` the releases of all live locals follow twice; they are never '
     'executed.'),
    ('`this(...)` and `this.field = value`',
     'sl/Scripts/game/Trigger.class', 0, 1, 0, 17, 24, 29,
     'Arguments are pushed left to right; the parameters live in the locals in '
     'reverse order (parent is local 4, alias local 1). `this.pos` uses '
     '`local 0` + `getfield`, a plain `pos` would be a `field` node.'),
    ('`if`, a call through a name path, a void method',
     'sl/Scripts/game/Trigger.class', 0, 4, 0, 24, 51, 61,
     'An object is a legal condition. A call is: arguments, `push_int` count, '
     '`call`, the path, then `expr call` or `stmt call`. The implicit return '
     'of a void method is `push_void`, the releases, `ret_value`.'),
    ('`return expr;`',
     'parts/scripts/Part.class', 0, 113, 0, 4, 730, 730,
     '`value` detaches the result from the variable before the locals are '
     'released.'),
    ('`for` loop, array element, call on a computed receiver',
     'multibot/maps/DragStrip/data/track_data.class', 0, 5, 238, 263, 114, 118,
     'Initialiser, condition, `jump_false` to the exit, body, update, jump '
     'back, release of the loop variable. `a[i]` is index first, array second. '
     '`light[i].finalize()` has an expression as receiver, so it is compiled as '
     'count, receiver, `call_on`. The last `jump +1` is the `break` of the '
     'enclosing `switch`.'),
    ('`switch`',
     'parts/scripts/enginepart/ExhaustTip.class', 0, 14, 16, 40, 47, 57,
     'The subject, then per `case` its value and a `case` node; `pop` + `jump` '
     'for "no case matched"; then the bodies, each `break` a `jump` to the end.'),
    ('Short-circuit `&&`',
     'cars/racers/Einvagen_data/scripts/Einvagen_110_GTK.class', 0, 1, 208, 223, 83, 83,
     '`and_test` keeps the left value; when it is false the `jump` skips the '
     'right operand and the `expr land`.'),
    ('Conditional expression in a declaration',
     'sl/Scripts/game/City.class', 0, 81, 204, 215, 1648, 1648,
     '`declare` + `type` + `init` pushes the new variable, `store` assigns. '
     'Both branches of `?:` are evaluated.'),
    ('Cast and field of the cast value',
     'parts/running_gear/scripts/shocks/shock_absorber_Baiern_GT_front.class',
     0, 2, 47, 64, 56, 57,
     'A cast to a class carries a `class` node, a cast to a primitive or array '
     'type a `type` node.'),
]


def md(x):
    """Escape a table cell."""
    return str(x).replace('|', '\\|')


def corpus_counts():
    ops = Counter()
    ops940 = Counter()
    te = Counter()
    ts = Counter()
    te940 = Counter()
    ts940 = Counter()
    blobs = 0
    blobs940 = 0
    trees = 0
    for rel, bi, b in oracle.iter_blobs():
        blobs += 1
        is940 = b.build == tufa.BUILD_940
        blobs940 += is940
        trees += len(b.trees)
        for t in b.trees:
            for nd in t:
                ops[nd.op] += 1
                if is940:
                    ops940[nd.op] += 1
                code = tufa.OP_FLAGS[nd.op]
                if code == 0x1007:
                    te[nd.imm] += 1
                    te940[nd.imm] += is940
                elif code == 0x1008:
                    ts[nd.imm] += 1
                    ts940[nd.imm] += is940
    return dict(ops=ops, ops940=ops940, te=te, ts=ts, te940=te940, ts940=ts940,
                blobs=blobs, blobs940=blobs940, trees=trees)


def example_block(title, rel, bi, ti, n0, n1, l0, l1, note):
    L = []
    e = next((x for x in oracle.load_index() if x.cls == rel), None)
    blobs = oracle.load_class(rel)
    b = blobs[bi]
    own = disasm._owner_table(b).get(ti, [])
    lm = None
    if len(own) == 1 and own[0][0] == 'method':
        lm = disasm.local_names(b, own[0][1], own[0][3])
    lines = disasm.tree_lines(b, ti, locals_map=lm)
    L.append('### %s' % title)
    L.append('')
    label = '; '.join(disasm.owner_label(b, o) for o in own)
    L.append('`%s`, class `%s`, tree %d (%s), nodes %d-%d.' % (
        rel, b.name, ti, label, n0, n1))
    L.append('')
    if e is not None and e.source:
        src = oracle.load_source(e).split('\r\n')
        L.append('```java')
        for ln in range(l0, l1 + 1):
            L.append('%5d  %s' % (ln, src[ln - 1].replace('\t', '    ').rstrip()))
        L.append('```')
        L.append('')
    L.append('```')
    L.extend(lines[n0:n1 + 1])
    L.append('```')
    L.append('')
    L.append(note)
    L.append('')
    return L


def main(argv):
    if not os.path.exists(CHECKS):
        sys.stderr.write('run check_opcodes.py --json %s first\n' % CHECKS)
        return 1
    with open(CHECKS, 'r', encoding='utf-8') as f:
        chk = json.load(f)
    C = corpus_counts()
    R = chk['rules']
    S = chk['stats']
    total_nodes = sum(C['ops'].values())

    def rr(rid):
        r = R[rid]
        return '%s of %s' % (format(r['ok'], ','), format(r['total'], ','))

    L = []
    w = L.append
    w('# TREE opcodes: reference for a compiler writer')
    w('')
    w('Generated by `lib/gen_opcodes_md.py` from the table in `lib/opcodes.py` '
      '(semantics), the corpus `%s` (counts) and `notes/opcode_checks.json` '
      '(proofs, written by `lib/check_opcodes.py`). Regenerate instead of '
      'editing.' % oracle.corpus_root())
    w('')
    w('Scope: all %s class blobs of the install (%s trees, %s nodes), of which '
      '%s blobs carry build 0x24F9D. Counts are given as "all / build 0x24F9D".'
      % (format(C['blobs'], ','), format(C['trees'], ','), format(total_nodes, ','),
         format(C['blobs940'], ',')))
    w('')
    w('## 1. Reading a node')
    w('')
    w('A node on disk is `op:u8, line:u16[, imm:u32]`.')
    w('')
    w('- `op` indexes the 48-entry table `kTreeOpFlags` '
      '(`engine/include/tree_op_flags.inc`). The table value is the 16-bit '
      'opcode the VM dispatches on; its low byte repeats the index and its high '
      'nibble is the immediate class: 0 none, 1 u32, 2 float bits, 4 utf8 '
      'index. The node is 7 bytes when the nibble is 1, 2 or 4 and 3 bytes '
      'otherwise. At load the VM expands every node to an 8-byte record '
      '`{opcode:u16, line:u16, imm:u32}`; the PC strides 8, so every jump '
      'offset counts nodes, not bytes.')
    w('- `line` (called `slot` in docs/ENGINE.md) is the source line the '
      'compiler had reached when it built the node. The VM copies it to '
      '`jvm+0x70` before every instruction, for error messages. Proof: over '
      'the %s pairs every node line lies inside the source file, and the '
      'literal or member name a node carries stands on that line for %s of '
      '%s such nodes; the rest sit in 15 stale pairs (`notes/corpus.md`).' % (
          format(chk['line_check']['pairs'], ','),
          format(sum(chk['line_check']['total'].values()) -
                 sum(chk['line_check']['bad'].values()), ','),
          format(sum(chk['line_check']['total'].values()), ',')))
    w('- `imm` is, depending on the opcode, a constant pool index, a local '
      'index, a count, a signed jump offset, a literal value or the '
      'expression kind of `expr` / `stmt`.')
    w('')
    w('The code is postfix: operands first, operator after. There are two '
      'stores per frame. The **operand stack** holds values and variables (a '
      'variable is a reference to a local, a field or an array element and '
      'stays assignable). The **locals** are a list: `this` at index 0 in '
      'instance methods, then the parameters in reverse order (the last '
      'parameter has the lowest index), then one entry per `declare`, removed '
      'again by `release` at the end of its scope. The fifth word of a method '
      'row is the high-water mark of that list.')
    w('')
    w('Three kinds of node are not executed on their own: **carriers** '
      '(operand of the node before them: literal, type, class), the **init** '
      'marker behind a declaration, and **path segments** (consumed by the '
      'name walk of `path` and `call`). The disassembler indents them.')
    w('')
    w('## 2. Table 1: opcodes')
    w('')
    w('Columns: op byte, 16-bit opcode, mnemonic used by `lib/disasm.py`, node '
      'size in bytes, meaning of the immediate, operands popped and pushed, '
      'occurrences (all / build 0x24F9D).')
    w('')
    w('| Op | Opcode | Mnemonic | Size | Immediate | Pops | Pushes | Count |')
    w('| ---: | --- | --- | ---: | --- | --- | --- | ---: |')
    for o in oc.OPS:
        n = C['ops'].get(o.raw, 0)
        n9 = C['ops940'].get(o.raw, 0)
        w('| %d | 0x%04X | `%s` | %d | %s | %s | %s | %s |' % (
            o.raw, o.code, o.name, tufa.NODE_SIZE[o.raw], IMM_TEXT[o.imm],
            o.pops, o.pushes,
            ('%s / %s' % (format(n, ','), format(n9, ','))) if n else 'never'))
    w('')
    w('### What each opcode does')
    w('')
    for o in oc.OPS:
        n = C['ops'].get(o.raw, 0)
        w('**0x%04X `%s`** (op %d, %s). %s' % (
            o.code, o.name, o.raw,
            {'exec': 'executed', 'carrier': 'carrier', 'segment': 'path segment',
             'mixed': 'executed or path segment', 'unused': 'not emitted'}[o.role],
            o.text))
        w('')
    w('## 3. Table 2: expression kinds (immediate of 0x1007 `expr` and 0x1008 `stmt`)')
    w('')
    w('`expr K` is emitted where the value of the expression is used, `stmt K` '
      'where the expression is a statement. The stmt form pops what the expr '
      'form pops and pushes nothing (exceptions: `stmt new` pushes the object, '
      '`stmt call` pops the return value that `expr call` leaves). Counts are '
      '"all / build 0x24F9D".')
    w('')
    w('| Tag | Name | Source | Follower | expr pops | expr pushes | stmt pops | expr count | stmt count |')
    w('| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: |')
    for t in oc.TAGS:
        ne, ns = C['te'].get(t.tag, 0), C['ts'].get(t.tag, 0)
        ne9, ns9 = C['te940'].get(t.tag, 0), C['ts940'].get(t.tag, 0)
        car = {None: '-', 'literal': 'literal carrier', 'type': '0x1006 type',
               'type|class': '0x1006 type or 0x1019 class'}[t.carrier]
        f = lambda v: '-' if v is None else str(v)
        cnt = lambda a, b: ('%s / %s' % (format(a, ','), format(b, ','))) if a else 'never'
        w('| 0x%02X | `%s` | %s | %s | %s | %s | %s | %s | %s |' % (
            t.tag, t.name, ('`%s`' % md(t.symbol)) if t.symbol else '-', car,
            f(t.pops_e), f(t.pushes_e), f(t.pops_s), cnt(ne, ne9), cnt(ns, ns9)))
    w('')
    for t in oc.TAGS:
        w('**0x%02X `%s`**. %s' % (t.tag, t.name, t.text))
        w('')
    w('### Literal carriers behind `expr literal`')
    w('')
    w('| Carrier | Count |')
    w('| --- | ---: |')
    car = S.get('carrier_of_literal', {})
    for code in oc.LITERAL_CARRIERS:
        w('| 0x%04X `%s` | %s |' % (code, oc.BY_CODE[code].name,
                                    format(car.get(str(code), 0), ',')))
    w('')
    cc = S.get('carrier_of_cast', {})
    w('Cast carriers: 0x1019 `class` %s, 0x1006 `type` %s. `new`, `newarray` '
      'and `instanceof` always carry 0x1006 (%s, %s, %s).' % (
          format(cc.get(str(0x1019), 0), ','), format(cc.get(str(0x1006), 0), ','),
          format(S.get('carrier_of_new', {}).get(str(0x1006), 0), ','),
          format(S.get('carrier_of_newarray', {}).get(str(0x1006), 0), ','),
          format(S.get('carrier_of_instanceof', {}).get(str(0x1006), 0), ',')))
    w('')
    w('## 4. Name paths')
    w('')
    w('A **field path** is an executed `path n` followed by n segment nodes; '
      'it pushes the variable the path ends in. A **call path** follows a '
      '`call` node: either one `method` node, or a `path n` header (not '
      'executed) and n segments of which the last names the method. In both, '
      'a leading `class` node means "ClassName." and counts as a segment.')
    w('')
    w('| Shape | Count |')
    w('| --- | ---: |')
    fl = S.get('field_path_len', {})
    for k in sorted(fl, key=int):
        w('| field path of %s segments | %s |' % (k, format(fl[k], ',')))
    w('| field paths in all | %s |' % format(sum(fl.values()), ','))
    ff = S.get('field_path_first', {})
    for code in (0x1001, 0x101b, 0x1019):
        w('| field path starting with 0x%04X `%s` | %s |' % (
            code, oc.BY_CODE[code].name, format(ff.get(str(code), 0), ',')))
    w('| call with a lone `method` node (implicit this or static, same class chain) | %s |'
      % format(S.get('call_form', {}).get('lone method ref', 0), ','))
    cl = S.get('call_path_len', {})
    for k in sorted(cl, key=int):
        w('| call path of %s segment%s behind a `path` header | %s |' % (
            k, '' if k == '1' else 's', format(cl[k], ',')))
    cf = S.get('call_first', {})
    for code in (0x1001, 0x101b, 0x1019, 0x4012):
        w('| call path starting with 0x%04X `%s` | %s |' % (
            code, oc.BY_CODE[code].name, format(cf.get(str(code), 0), ',')))
    ce = S.get('call_last', {})
    for code in (0x101a, 0x4012):
        w('| call path ending in 0x%04X `%s` | %s |' % (
            code, oc.BY_CODE[code].name, format(ce.get(str(code), 0), ',')))
    w('')
    w('A call path ends in `method` (pool member ref, descriptor known at '
      'compile time) or in `name` (utf8 only, resolved at run time from the '
      'argument types). Which of the two the compiler picks, and when it '
      'emits `field` + `getfield`/`call_on` instead of a path, is a question '
      'of its symbol resolution and is not settled here. `local` and `field` '
      'nodes are counted in table 1 whether they are executed or segments.')
    w('')
    w('## 5. Proofs')
    w('')
    w('`lib/check_opcodes.py` checks the tables above against every tree of '
      'the corpus. Results of the last run (`notes/opcode_checks.txt`):')
    w('')
    w('| Rule | Holds for | Statement |')
    w('| --- | ---: | --- |')
    for rid, r in R.items():
        if r['ok'] == r['total']:
            star = ''
        elif rid in chk.get('known_exceptions', {}):
            star = ' (known exception, section 6)'
        else:
            star = ' (measurement, not a rule)'
        w('| %s | %s of %s%s | %s |' % (rid, format(r['ok'], ','),
                                        format(r['total'], ','), star, r['text']))
    w('')
    w('The S rules are positional. The X rules come from a simulation: every '
      'tree is walked along all control-flow paths with an abstract operand '
      'stack and local count, applying exactly the pops and pushes of tables 1 '
      'and 2 (%s node executions). The stack never underflows, paths that '
      'join agree in depth and local count, every statement leaves the stack '
      'empty, `end` is reached with one value, `ret` with none, `ret_value` '
      'with one and no live local.' % format(S.get('simulated', {}).get('nodes', 0), ','))
    w('')
    w('The mapping of expression kinds to operators is proven by a token '
      'census (`lib/paircheck.py`): for each of the %s Build 940 pairs the '
      'tokens of the source are counted per class and compared with the node '
      'counts of the class file.' % format(chk['census_files'], ','))
    w('')
    w('| Token | In sources | In classes | Files that agree | Class side |')
    w('| --- | ---: | ---: | ---: | --- |')
    for lab, d in chk['census'].items():
        w('| `%s` | %s | %s | %s of %s | %s |' % (
            md(lab), format(d['source'], ','),
            '-' if d['class_side'] == 'no node' else format(d['class'], ','),
            format(d['agree'], ','), format(d['files'], ','), d['class_side']))
    w('')
    bad = chk['census_files_disagreeing']
    w('Files with a disagreement: %d, all on the int literal count and all '
      'stale pairs (the class holds `type = 1;` where the shipped source says '
      '`type = TT_AUTO;`): %s.' % (len(bad), ', '.join(
          '`%s`' % os.path.basename(x) for x in bad)))
    w('')
    w('Consequences a compiler writer can rely on: one node per operator, '
      'literal and keyword occurrence; no constant folding (`-1` is `int 1` + '
      '`expr neg`); no dead code removal (%s nodes of the corpus are '
      'unreachable, %s of them `release` nodes behind a return); a unary `+` '
      'emits nothing.' % (
          format(S.get('nodes', {}).get('unreachable', 0), ','),
          format(S.get('unreachable', {}).get(str(0x1003), 0), ',')))
    w('')
    w('## 6. Opcodes that occur and could not be explained')
    w('')
    w('None. Every op byte and every expression kind that occurs in the corpus '
      'has a body in the interpreter and a role in the tables above. Two '
      'occurrences are irregular; both are in class files without a source:')
    w('')
    for rid, d in chk.get('known_exceptions', {}).items():
        for where, why in d.items():
            w('- %s: `%s`: %s.' % (rid, where, why))
    w('')
    never = [o for o in oc.OPS if not C['ops'].get(o.raw)]
    w('Op bytes that never occur: %s. Expression kinds that never occur: %s.' % (
        ', '.join('%d (0x%04X `%s`)' % (o.raw, o.code, o.name) for o in never),
        ', '.join('0x%02X' % t.tag for t in oc.TAGS
                  if not C['te'].get(t.tag) and not C['ts'].get(t.tag))))
    w('')
    w('Kinds that occur in one form only: expr only: %s; stmt only: %s.' % (
        ', '.join('0x%02X `%s`' % (t.tag, t.name) for t in oc.TAGS
                  if C['te'].get(t.tag) and not C['ts'].get(t.tag)),
        ', '.join('0x%02X `%s`' % (t.tag, t.name) for t in oc.TAGS
                  if C['ts'].get(t.tag) and not C['te'].get(t.tag))))
    w('')
    w('## 7. Shapes seen in the corpus (examples, status: observed)')
    w('')
    w('These are single examples taken from the corpus with the source beside '
      'them. They show what the proven rules look like in practice; the exact '
      'emission rule of each statement kind is the next analysts\' job.')
    w('')
    for ex in EXAMPLES:
        L.extend(example_block(*ex))
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(L))
        f.write('\n')
    print('wrote %s (%d lines)' % (OUT, len(L)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
