#!/usr/bin/env python3
"""spike_check.py - run the composition spike over the corpus.

    python -I spike_check.py [--only KEY] [--show N] [--json OUT]

For every pair the source is compiled by spike_compiler.py (nothing is read
from the class file of that source) and the bytes are compared with the class
file on disk.  The float conversion is the one open point with a choice, so
each pair is tried under setting A first and, when that fails, under B, C, D.

Groups:
    oracle        Build 940, consistent with the source          (the proof)
    older         builds 0x24F5C / 0x11AE1, consistent           (second-rate oracle;
                  compiled with the build number of the shipped class)
    stale         source edited after the compile                (control: must fail)
    source only   no class file: compiled, re-read with the strict reader

Writes out/spike_check.txt, out/spike_pass_<group>.txt (the pass lists a
regression test would store) and, with --json, the numbers.
"""
import json
import os
import sys
import time
import traceback
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import spike_compiler as sc   # noqa: E402  (sets up sys.path)
import oracle                 # noqa: E402
import tufa                   # noqa: E402
import stmtgen                # noqa: E402

DEP_STALE = ('parts/scripts/enginepart/slidingenginepart/reciprocatingenginepart/'
             'differential/Center_Differential', 'sl/Scripts/game/Garage')


def group_of(e):
    if e.category == 'source_only':
        return 'source only'
    if e.category == 'pair_940':
        return 'oracle' if e.consistent else 'stale'
    if e.category == 'pair_other':
        return 'older' if e.consistent else 'stale'
    return None


def classify(mine, theirs):
    """Why two class files differ: (short reason, detail)."""
    try:
        a = tufa.parse(mine)
        b = tufa.parse(theirs)
    except tufa.TufaError as x:
        return 'unreadable', str(x)
    if len(a) != len(b):
        return 'blob count', '%d / %d' % (len(a), len(b))
    for bi, (x, y) in enumerate(zip(a, b)):
        if len(x.trees) != len(y.trees):
            return 'tree count', 'blob %d: %d / %d' % (bi, len(x.trees), len(y.trees))
        float_only = True
        first = None
        for ti, (tx, ty) in enumerate(zip(x.trees, y.trees)):
            if len(tx) != len(ty):
                return 'tree length', 'blob %d tree %d: %d / %d nodes' % (bi, ti, len(tx), len(ty))
            for ni, (nx, ny) in enumerate(zip(tx, ty)):
                if nx.op != ny.op:
                    return 'opcode', 'blob %d tree %d node %d: op %d / %d' % (bi, ti, ni, nx.op, ny.op)
                if nx.slot != ny.slot:
                    return 'line', 'blob %d tree %d node %d: line %d / %d' % (
                        bi, ti, ni, nx.slot, ny.slot)
                code = tufa.OP_FLAGS[nx.op]
                if code == 0x200a:
                    if nx.imm != ny.imm:
                        # the settings differ by up to 3 units in the last place;
                        # anything further apart is another literal
                        if abs(nx.imm - ny.imm) > 4:
                            return 'float value', 'blob %d tree %d node %d: %08X / %08X' % (
                                bi, ti, ni, nx.imm, ny.imm)
                        first = first or 'blob %d tree %d node %d: %08X / %08X' % (
                            bi, ti, ni, nx.imm, ny.imm)
                elif code in (0x1006, 0x4009, 0x100e, 0x4012, 0x1019, 0x101a, 0x101b,
                              0x401c, 0x4025, 0x4026):
                    dx, dy = x.describe(nx.imm), y.describe(ny.imm)
                    if dx != dy:
                        return 'operand', 'blob %d tree %d node %d line %d: %s / %s' % (
                            bi, ti, ni, nx.slot, dx, dy)
                elif nx.imm != ny.imm:
                    return 'immediate', 'blob %d tree %d node %d: %r / %r' % (
                        bi, ti, ni, nx.imm, ny.imm)
        if first:
            return 'float bits only', first
        d = oracle.compare_blobs(x, y)
        if not d.equal:
            return 'other chunk', 'blob %d: %s' % (bi, d)
    return 'unknown', ''


POOL_CODES = (0x1006, 0x4009, 0x100e, 0x4012, 0x1019, 0x101a, 0x101b, 0x401c, 0x4025, 0x4026)


def trees_equal(mine, theirs):
    """(trees that are the same node for node, trees) of two class files; a
    pool operand is compared through what it names."""
    try:
        a = tufa.parse(mine)
    except tufa.TufaError:
        return 0, 0
    b = tufa.parse(theirs)
    same = total = 0
    for x, y in zip(a, b):
        for tx, ty in zip(x.trees, y.trees):
            total += 1
            if len(tx) != len(ty):
                continue
            ok = True
            for nx, ny in zip(tx, ty):
                if nx.op != ny.op or nx.slot != ny.slot:
                    ok = False
                    break
                if tufa.OP_FLAGS[nx.op] in POOL_CODES:
                    if x.describe(nx.imm) != y.describe(ny.imm):
                        ok = False
                        break
                elif nx.imm != ny.imm:
                    ok = False
                    break
            same += ok
        total += abs(len(x.trees) - len(y.trees))
    return same, total


def main(argv):
    only = argv[argv.index('--only') + 1] if '--only' in argv else None
    pack_rule = argv[argv.index('--packs') + 1] if '--packs' in argv else 'table'
    tag = '' if pack_rule == 'table' else '_' + pack_rule
    show = int(argv[argv.index('--show') + 1]) if '--show' in argv else 6
    out_json = argv[argv.index('--json') + 1] if '--json' in argv else None
    t0 = time.time()
    env = sc.Env()
    c = Counter()
    why = defaultdict(Counter)
    ex = defaultdict(list)
    passed = defaultdict(list)
    need = Counter()
    so_nodes = so_trees = so_bytes = 0
    modes = []
    for e in oracle.iter_entries():
        g = group_of(e)
        if g is None:
            continue
        key = e.dir + '/' + e.name
        if only and not key.lower().endswith(only.lower()):
            continue
        c[g, 'sources'] += 1
        text = oracle.load_source(e)
        if g == 'source only':
            try:
                data, info = sc.compile_source(text, env, 'A', pack_rule, name=e.source)
                blobs = tufa.parse(data)
                for b in blobs:
                    tufa.validate(b)
                if tufa.serialise(blobs) != data:
                    raise sc.CompileError('does not round-trip')
                c[g, 'compiled'] += 1
                c[g, 'classes'] += len(blobs)
                so_trees += info['trees']
                so_nodes += info['nodes']
                so_bytes += len(data)
                passed[g].append(e.source)
            except Exception as x:   # noqa: every failure is a finding here
                r = '%s: %s' % (type(x).__name__, str(x)[:100])
                why[g][type(x).__name__ + ': ' + str(x).split(' ')[0]] += 1
                if len(ex[g]) < show:
                    ex[g].append('%s: %s' % (e.source, r))
            continue
        theirs = oracle.load_class_bytes(e)
        build = e.builds[0] if e.builds else tufa.BUILD_940
        got = None
        first_reason = None
        best_trees = None
        for mode in 'ABCD':
            try:
                data, info = sc.compile_source(text, env, mode, pack_rule, build=build, name=e.source)
            except Exception as x:   # noqa
                first_reason = ('compile error', '%s: %s' % (type(x).__name__, str(x)[:160]))
                if not isinstance(x, sc.CompileError):
                    first_reason = ('compile error',
                                    first_reason[1] + ' @ ' + traceback.format_exc().splitlines()[-3].strip())
                break
            st, tt = trees_equal(data, theirs)
            if mode == 'A':
                c[g, 'trees'] += tt
                c[g, 'trees under A'] += st
            best_trees = st if best_trees is None else max(best_trees, st)
            if data == theirs:
                got = mode
                break
            if first_reason is None:
                first_reason = classify(data, theirs)
            if first_reason[0] != 'float bits only':
                break
            if sc.float_token_count(text) == 0:
                break
        c[g, 'trees best'] += best_trees or 0
        if got == 'A':
            c[g, 'identical under A'] += 1
        if got:
            c[g, 'identical'] += 1
            need[g, got] += 1
            passed[g].append(e.cls)
            modes.append((e.cls, got if sc.float_token_count(text) else '-'))
        else:
            r, det = first_reason
            if key in DEP_STALE:
                r = 'callee changed after the compile (known): ' + r
            why[g][r] += 1
            if len(ex[g, r]) < show:
                ex[g, r].append('%s: %s' % (e.cls, det))
    lines = []
    out = lines.append
    out('composition spike: stmtgen (free mode) + name and call resolution + poolmodel + tufa')
    out('pack names: %s' % pack_rule)
    out('')
    for g in ('oracle', 'older', 'stale'):
        n = c[g, 'sources']
        if not n:
            continue
        out('%-12s pairs %4d   byte-identical %4d (float setting A alone: %4d)   by setting: %s'
            % (g, n, c[g, 'identical'], c[g, 'identical under A'],
               ', '.join('%s %d' % (m, need[g, m]) for m in 'ABCD' if need[g, m])))
        out('%-12s trees %4d   node for node  %4d (float setting A alone: %4d)'
            % ('', c[g, 'trees'], c[g, 'trees best'], c[g, 'trees under A']))
        for r, k in why[g].most_common():
            out('    %4d  not identical: %s' % (k, r))
            for x in ex[g, r]:
                out('            %s' % x)
    g = 'source only'
    if c[g, 'sources']:
        out('%-12s sources %d   compiled and re-read by the strict reader %d (%d classes, %d trees, '
            '%d nodes, %d bytes)' % (g, c[g, 'sources'], c[g, 'compiled'], c[g, 'classes'],
                                     so_trees, so_nodes, so_bytes))
        for r, k in why[g].most_common():
            out('    %4d  failed: %s' % (k, r))
        for x in ex[g]:
            out('            %s' % x)
    out('')
    out('%.0f s' % (time.time() - t0))
    text = '\n'.join(lines) + '\n'
    sys.stdout.write(text)
    if not only:
        os.makedirs(os.path.join(_HERE, 'out'), exist_ok=True)
        with open(os.path.join(_HERE, 'out', 'spike_check%s.txt' % tag), 'w', encoding='utf-8',
                  newline='\n') as f:
            f.write(text)
        with open(os.path.join(_HERE, 'out', 'spike_float_modes%s.tsv' % tag), 'w',
                  encoding='utf-8', newline='\n') as f:
            f.write('# class file\tfirst float setting (A, B, C, D) that makes it '
                    'byte-identical; - = no float literal\n')
            for cls, mode in sorted(modes):
                f.write('%s\t%s\n' % (cls, mode))
        for g, lst in passed.items():
            with open(os.path.join(_HERE, 'out', 'spike_pass%s_%s.txt' % (tag, g.replace(' ', '_'))),
                      'w', encoding='utf-8', newline='\n') as f:
                f.write('\n'.join(sorted(lst)) + '\n')
    if out_json:
        doc = {'counts': {'%s|%s' % k: v for k, v in c.items()},
               'need': {'%s|%s' % k: v for k, v in need.items()},
               'why': {g: dict(v) for g, v in why.items()}}
        with open(out_json, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(doc, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
