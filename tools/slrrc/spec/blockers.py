#!/usr/bin/env python3
"""blockers.py - how many sources each open question of the compiler work touches.

    python -I blockers.py [--json OUT]

For every source of the corpus the script counts the constructs that an open
question is about, and reports per question the number of sources in each
group that hold at least one such construct:

    oracle        Build 940 pairs fit for byte comparison      (2,651)
    all pairs     every source that has a class file           (2,819)
    source only   the sources the host cannot load today       (  635)

"Touches" is an upper bound for "blocks": a pair that holds a float literal is
only blocked from byte identity when its literals need the conversion the
compiler does not use.  Where the corpus can say how many are really blocked
(floats, pack names) the script says so; the float numbers come from
out/float_models_*.pickle (run float_models.py first).
"""
import json
import os
import pickle
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', 'lib'))
if _LIB not in sys.path:
    sys.path.insert(0, _LIB)

import lexer              # noqa: E402
import oracle             # noqa: E402
import parser             # noqa: E402
import symbols            # noqa: E402
import study_class_dirs   # noqa: E402

DEP_STALE = ('parts/scripts/enginepart/slidingenginepart/reciprocatingenginepart/'
             'differential/Center_Differential', 'sl/Scripts/game/Garage')
DIALOG_TWINS_DIFFER = ('java.render.osd.dialog.StringRequesterDialog',
                       'java.render.osd.dialog.FileRequesterDialog',
                       'java.render.osd.dialog.TextDialog')


def group_of(e):
    if e.category == 'source_only':
        return 'source_only'
    if e.category == 'pair_940':
        return 'oracle' if e.consistent else 'stale_940'
    if e.category == 'pair_other':
        return 'other' if e.consistent else 'stale_other'
    return None


def disk_packs(root):
    out = {}
    for dp, dn, fn in os.walk(root):
        dn[:] = [d for d in dn if not d.lower().startswith('tree_rpk_scan_boot')]
        for f in fn:
            if f.lower().endswith('.rpk'):
                rel = os.path.relpath(os.path.join(dp, f), root).replace('/', '\\')
                out[rel.lower()] = rel
    return out


def main(argv):
    out_json = argv[argv.index('--json') + 1] if '--json' in argv else None
    root = oracle.corpus_root()
    disk = disk_packs(root)
    entries = [e for e in oracle.iter_entries() if e.source]
    uni = symbols.Universe.from_index()
    # where classes live, and which have a class file
    dirs = defaultdict(set)
    has_class = set()
    src_only_class = {}
    several = defaultdict(set)
    for e in oracle.iter_entries():
        if e.blobs:
            for b in e.blobs:
                dirs[b['name']].add(e.dir.lower())
                has_class.add(b['name'])
                several[b['name']].add(e.cls)
        else:
            for d in (e.declared or []):
                full = e.package + '.' + d['name']
                dirs[full].add(e.dir.lower())
                src_only_class.setdefault(full, []).append(e.source)
                several[full].add(e.source)
    multi_def = set(k for k, v in several.items() if len(v) > 1)
    only_src = set(k for k in src_only_class if k not in has_class)

    groups = Counter()
    q = defaultdict(Counter)          # question -> group -> sources
    qn = defaultdict(Counter)         # question -> group -> constructs
    ex = defaultdict(list)
    dep_graph = {}                    # source-only source -> source-only classes it names
    for e in entries:
        g = group_of(e)
        groups[g] += 1
        text = oracle.load_source(e)
        toks = lexer.tokenize(text)
        unit = parser.parse(text, e.source)
        res = symbols.Resolver(unit, uni)

        def hit(question, n=1):
            if n:
                q[question][g] += 1
                qn[question][g] += n
                if len(ex[question]) < 4:
                    ex[question].append(e.source)

        nfloat = sum(1 for t in toks if t.kind == 'float')
        hit('float literal (bit pattern of the 0x200A node)', nfloat)
        rid_case = rid_nofile = nrid = 0
        for t in toks:
            if t.kind == 'rid':
                nrid += 1
                pack, lid = lexer.rid_parts(t.text)
                w = pack.replace('.', '\\') + '.rpk'
                d = disk.get(w.lower())
                if d is None:
                    rid_nofile += 1
                elif d != w:
                    rid_case += 1
        hit('resource id literal (spelling of the pack name in the pool)', nrid)
        hit('  of these: pack written in another letter case than the file on disk', rid_case)
        hit('  of these: pack without a file', rid_nofile)
        # class names
        names = set()
        for cls in unit.types:
            if cls.extends:
                names.add(cls.extends)
            names.update(cls.implements)
        for n in unit.walk():
            if n.kind == 'Type' and not n.primitive:
                names.add(n.name)
            elif n.kind == 'Name':
                names.add(n.name)
        own = set(t.name for t in unit.types)
        here = e.dir.lower()
        off = srcdir = deps = twins = multi = later = 0
        depset = set()
        order = [t.name for t in unit.types]
        for nm in names:
            full = res.resolve(nm)
            if full is None:
                continue
            if nm in own:
                continue
            ds = dirs[full]
            cp = study_class_dirs.classpath_dir(full.rpartition('.')[0])
            if cp in ds:
                pass
            elif here in ds:
                srcdir += 1
            else:
                off += 1
            if full in only_src:
                deps += 1
                depset.add(full)
            if full in DIALOG_TWINS_DIFFER:
                twins += 1
            if full in multi_def:
                multi += 1
        hit('names a class that lives outside the classpath and outside its own directory', off)
        hit('names a class of its own directory that is not on the classpath', srcdir)
        hit('names a class that has no class file (must be compiled first)', deps)
        hit('names one of the three dialog classes that ship in two different versions', twins)
        hit('names a class whose full name is defined by several files', multi)
        if g == 'source_only':
            dep_graph[e.source] = sorted(depset)
        if len(unit.types) > 1:
            hit('source with several classes (visibility of later classes, own LC; entry)', len(unit.types))
        if any(i.pack for i in unit.imports):
            hit('import with a colon (import parts:part.root.BodyPart.*;)')
        key = (e.dir + '/' + e.name)
        if key in DEP_STALE:
            hit('compiled against another version of a class it calls (cannot be reproduced)')
        if e.category == 'pair_other':
            hit('class file written by an older build (0x24F5C, 0x11AE1)')
        if e.consistent is False:
            hit('source edited after the class was compiled (stale pair)')
        # where the source sits
        cp = study_class_dirs.classpath_dir(unit.package or '')
        if cp != here:
            hit('source in a directory that the classpath does not reach from its package')
        ncall = sum(1 for n in unit.walk() if n.kind == 'Call')
        hit('call through a name (method ref or bare name: needs the members of other classes)', ncall)

    cols = ('oracle', 'stale_940', 'other', 'stale_other', 'source_only')
    print('sources by group: %s' % ', '.join('%s %d' % (c, groups[c]) for c in cols))
    print()
    print('%-88s %7s %9s %11s   constructs (oracle / all pairs / source only)' % (
        'question: sources that hold at least one such construct', 'oracle', 'all pairs', 'source only'))
    rows = []
    for question in q:
        o = q[question]['oracle']
        allp = sum(q[question][c] for c in cols[:4])
        so = q[question]['source_only']
        no = qn[question]['oracle']
        nall = sum(qn[question][c] for c in cols[:4])
        nso = qn[question]['source_only']
        rows.append((question, o, allp, so, no, nall, nso))
    for r in rows:
        print('%-88s %7d %9d %11d   %d / %d / %d' % r)
    print()
    # floats: really blocked
    doc = {'groups': dict(groups), 'rows': rows, 'examples': dict(ex)}
    for which in ('oracle', 'other'):
        sp = os.path.join(_HERE, 'out', 'float_samples_%s.pickle' % which)
        rp = os.path.join(_HERE, 'out', 'float_res_%s.pickle' % which)
        if not (os.path.exists(sp) and os.path.exists(rp)):
            print('float_models.py --set %s has not been run: no float detail' % which)
            continue
        with open(sp, 'rb') as f:
            per_file = pickle.load(f)
        with open(rp, 'rb') as f:
            names, res = pickle.load(f)
        idx = {n: k for k, n in enumerate(names)}
        sets = [('A single precision, scale divided by 10 (div pc24)', 'div pc24 v24 s24 c24'),
                ('B single precision, scale times 0.1f (mul pc24)', 'mul pc24 v24 s24 c24'),
                ('C sum and product in double, float variables (div pc53)', 'div pc53 v24 s24 c24'),
                ('D double variables, scale 0.1f divided by 10 (diva pc53)', 'diva pc53 v53 s53 c24')]
        full = {}
        for label, nm in sets:
            k = idx[nm]
            full[label] = set(f for f, v in per_file.items()
                              if all(res[tk][k] == imm for tk, imm, _ in v))
        allf = set(per_file)
        nodes = sum(len(v) for v in per_file.values())
        print('float literals, %s pairs: %d class files hold a float node (%d nodes)' % (
            which, len(allf), nodes))
        for label, nm in sets:
            k = idx[nm]
            nh = sum(1 for v in per_file.values() for tk, imm, _ in v if res[tk][k] == imm)
            print('  every node reproduced by %-58s %4d files (blocked otherwise: %3d); nodes %d' % (
                label, len(full[label]), len(allf) - len(full[label]), nh))
        got = set()
        for n, (label, _) in enumerate(sets):
            got |= full[label]
            print('  by one of the first %d, chosen per file: %4d  (left: %d)' % (
                n + 1, len(got), len(allf - got)))
        need = Counter()
        for f in allf:
            key = ''.join(ch for ch, s in zip('ABCD', (full[x[0]] for x in sets)) if f in s) or 'none'
            need[key] += 1
        print('  files by the settings that reproduce them: %s' % dict(sorted(need.items())))
        left = sorted(allf - got)
        for f in left:
            print('    not reproduced by any: %s (%d float nodes)' % (f, len(per_file[f])))
        doc['float_' + which] = {'files': len(allf), 'need': dict(need), 'left': left,
                                 'full': {k: len(v) for k, v in full.items()}}
    print()
    # dependency depth among the sources without a class
    name_to_src = {}
    for full, srcs in src_only_class.items():
        if full in only_src:
            name_to_src[full] = srcs
    depth = {}

    def d_of(src, seen=()):
        if src in depth:
            return depth[src]
        if src in seen:
            return 0
        best = 0
        for full in dep_graph.get(src, ()):
            for s2 in name_to_src.get(full, ()):
                if s2 != src:
                    best = max(best, 1 + d_of(s2, seen + (src,)))
        depth[src] = best
        return best
    for s in dep_graph:
        d_of(s)
    dc = Counter(depth.values())
    print('sources without a class by the depth of their dependence on other such sources: %s'
          % dict(sorted(dc.items())))
    targets = Counter()
    for s, ds in dep_graph.items():
        for full in ds:
            targets[full] += 1
    print('classes without a class file that other class-less sources name (%d):' % len(targets))
    for full, n in targets.most_common(20):
        print('  %4d  %-70s %s' % (n, full, src_only_class[full][0]))
    doc['dep_depth'] = dict(dc)
    doc['dep_targets'] = dict(targets)
    if out_json:
        with open(out_json, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(doc, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
