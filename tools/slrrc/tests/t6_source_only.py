#!/usr/bin/env python3
"""t6_source_only.py - milestone M6, the offline half: the sources that have no
class file, compiled through the command line into a cache directory.

    python -I tests/t6_source_only.py [--only TEXT] [-v] [--update]

The test runs the driver as a host would,

    python -I slrrc --root <corpus> --out <temporary directory> --only-missing

and then looks at what it wrote.  There is no shipped class to compare the 635
outputs with, so they are checked in every other way there is:

  baseline   the SHA-256 of every output equals tests/baseline/m6.sha256 (the
             bytes the composition spike produced; a wanted change is
             re-baselined on purpose)
  reader     lib/tufa.py (the strict reader of the foundation) reads each file
             back, validate() finds nothing, it serialises to the same bytes
  layout     the 30 layout rules of the foundation (lib/check_layout.py, run
             on the cache directory) hold without exception
  stack      the stack simulation of lib/check_opcodes.py (rules X1 to X6): no
             underflow, no unbalanced join, an empty stack after every
             statement
  structure  every tree with nodes splits into the statement forms of
             notes/statements.md (analysis/stmt/destruct.py) and fits the
             expression shapes of notes/expressions.md (analysis/expr/decomp.py)
  siblings   where a sibling in the same directory has the same tokens on the
             same lines and ships a class: opcode, line, local index, count,
             jump offset and kind of expression of every node equal the
             sibling's shipped class
  manifest   one entry per output with the source path, its size and hash,
             the float setting, the pack table version and the classes it was
             compiled against; every hash in it is recomputed here, the
             declarations from the CorpusProvider of the tests (which reads
             the class files with lib/tufa.py, not with the package's reader)
  tree       the game tree is the same before and after: no file of it was
             created, changed or removed
  provider   the provider of the driver (slrrc/install.py) declares every
             class of the corpus as the CorpusProvider does
  driver     the command line on a small tree made for the test: an output
             directory inside the tree is refused, a dry run writes nothing,
             --write-install writes beside the source with its time stamp and
             never replaces a class file unless --overwrite is given, a source
             that does not compile leaves no output and exit code 1

What this test cannot say: whether the host loads the files (the second half
of M6; that needs the host to run).

Headline:
  M6 source-only   635/635 compiled through the cache (635 classes, 771 trees,
  32644 nodes, 693847 bytes); hashes 635/635 equal to the baseline; strict
  reader 635/635; layout rules hold; stack simulation holds (21523 nodes);
  structure 767/767 trees; siblings 522/522; manifest 635/635 entries; driver
  checks N/N
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness              # noqa: E402
import oracle               # noqa: E402  lib
import tufa                 # noqa: E402  lib
import check_opcodes        # noqa: E402  lib
import spike_source_only as sso     # noqa: E402  spec (tree_info, shapes, simulate_tree)
import destruct             # noqa: E402  analysis/stmt
import decomp               # noqa: E402  analysis/expr

import slrrc                        # noqa: E402
from slrrc import classes           # noqa: E402
from slrrc import classfile         # noqa: E402
from slrrc import cli               # noqa: E402
from slrrc import install           # noqa: E402
from slrrc import packs             # noqa: E402

PACKAGE_DIR = os.path.join(harness.ROOT, 'slrrc')


def run_cli(args, cwd=None, isolated=True, module=False):
    """Run the command line in an interpreter of its own: (exit code, stdout,
    stderr)."""
    cmd = [sys.executable]
    if isolated:
        cmd.append('-I')
    if module:
        cmd += ['-m', 'slrrc']
    else:
        cmd.append(PACKAGE_DIR)
    p = subprocess.run(cmd + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       cwd=cwd or harness.ROOT)
    return (p.returncode, p.stdout.decode('utf-8', 'replace').replace('\r\n', '\n'),
            p.stderr.decode('utf-8', 'replace').replace('\r\n', '\n'))


def snapshot(root):
    """Every file below root with its size and time stamp."""
    out = {}
    for dp, dn, fn in os.walk(root):
        for f in fn:
            p = os.path.join(dp, f)
            st = os.stat(p)
            out[os.path.relpath(p, root)] = (st.st_size, st.st_mtime_ns)
    return out


def script_snapshot(root):
    """The class files, sources and packs of the corpus with size and time
    stamp, and anything named like a file the driver writes (a class file,
    its temporary file, a manifest).  Other files are not looked at: the
    corpus is a working install, and other programs write their own files
    there (save games) while a test runs."""
    out = {}
    for dp, dn, fn in os.walk(root):
        for f in fn:
            low = f.lower()
            if low.endswith(('.class', '.java', '.rpk')) or '.class.tmp' in low or \
                    low.startswith('manifest.json'):
                p = os.path.join(dp, f)
                st = os.stat(p)
                out[os.path.relpath(p, root)] = (st.st_size, st.st_mtime_ns)
    return out


def sha(data):
    return hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------------------
# the command line on a small tree
# ---------------------------------------------------------------------------

ALPHA = ('package java.game.mod;\r\n\r\npublic class Alpha\r\n{\r\n\tint n = 3;\r\n\r\n'
         '\tpublic Alpha()\r\n\t{\r\n\t\tn = n + 1;\r\n\t}\r\n}\r\n')
BETA = ('package java.game.mod;\r\n\r\npublic class Beta extends Alpha\r\n{\r\n'
        '\tint pic = packs.things:0x00000012r;\r\n\tint gone = packs.nothing:0x00000034r;\r\n\r\n'
        '\tpublic Beta()\r\n\t{\r\n\t\tsuper();\r\n\t\tn = 7;\r\n\t}\r\n}\r\n')
BROKEN = 'package java.game.mod;\r\n\r\npublic class Broken\r\n{\r\n\tint n = ;\r\n}\r\n'
GAMMA = 'package java.game.mod;\r\n\r\npublic class Gamma\r\n{\r\n\tint a[];\r\n}\r\n'
STAMP = 1000000000                  # 2001-09-09, the time stamp given to the sources


def put(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(text.encode('latin-1'))
    os.utime(path, (STAMP, STAMP))


def get(path):
    with open(path, 'rb') as f:
        return f.read()


def driver_checks(tmp):
    """[(what, None or what went wrong)] for the command line on a small
    tree."""
    out = []

    def check(what, cond, detail=''):
        out.append((what, None if cond else (detail or 'failed')))

    tree = os.path.join(tmp, 'tree')
    src = os.path.join(tree, 'mod', 'scripts', 'src')
    put(os.path.join(src, 'Alpha.java'), ALPHA)
    put(os.path.join(src, 'Beta.java'), BETA)
    put(os.path.join(tree, 'loose', 'Stray.java'), ALPHA)
    put(os.path.join(tree, 'packs', 'Things.rpk'), '')
    before = snapshot(tree)
    alpha_class = os.path.join(tree, 'mod', 'scripts', 'Alpha.class')
    beta_class = os.path.join(tree, 'mod', 'scripts', 'Beta.class')

    # an output directory inside the tree
    code, so, se = run_cli(['--root', tree, '--out', os.path.join(tree, 'cache')])
    check('an --out inside the tree is refused', code == 2 and 'refused' in se and
          snapshot(tree) == before, 'exit %d: %s' % (code, se.strip()))
    code, so, se = run_cli(['--root', tree, '--out', tmp])
    check('an --out that contains the tree is refused', code == 2 and 'refused' in se and
          snapshot(tree) == before and not os.path.exists(os.path.join(tmp, 'manifest.json')),
          'exit %d: %s' % (code, se.strip()))
    # nothing to do
    code, so, se = run_cli(['--root', tree])
    check('a run without a destination is refused', code == 2 and snapshot(tree) == before,
          'exit %d' % code)
    # dry run
    dry = os.path.join(tmp, 'dry')
    code, so, se = run_cli(['--root', tree, '--out', dry, '--dry-run'])
    lines = [x for x in so.split('\n') if x.startswith('compile ')]
    check('a dry run lists the sources and writes nothing',
          code == 0 and len(lines) == 2 and not os.path.exists(dry) and
          snapshot(tree) == before, 'exit %d, %d lines' % (code, len(lines)))
    # --check
    code, so, se = run_cli(['--root', tree, '--check'])
    check('--check compiles and writes nothing',
          code == 0 and 'checked 2 of 2 sources' in so and snapshot(tree) == before,
          'exit %d: %s' % (code, so.strip()))
    # a construct no shipped source uses: a warning, an error with --strict
    gamma = os.path.join(src, 'Gamma.java')
    put(gamma, GAMMA)
    code, so, se = run_cli(['--root', tree, '--check', gamma])
    check('a construct no shipped source uses compiles with a warning',
          code == 0 and 'Gamma.java:5:' in se and 'warning W_UNVERIFIED' in se and
          '1 warnings' in so, 'exit %d: %s / %s' % (code, so.strip(), se.strip()))
    code, so, se = run_cli(['--root', tree, '--check', '--strict', gamma])
    check('... and is an error with --strict',
          code == 1 and 'error E_UNSUPPORTED' in se and '1 FAILED' in so,
          'exit %d: %s / %s' % (code, so.strip(), se.strip()))
    os.remove(gamma)
    # into a cache directory
    cache = os.path.join(tmp, 'small')
    code, so, se = run_cli(['--root', tree, '--out', cache])
    have = sorted(snapshot(cache))
    want = sorted([os.path.join('mod', 'scripts', 'Alpha.class'),
                   os.path.join('mod', 'scripts', 'Beta.class'), 'manifest.json'])
    check('a run writes the cache layout and the manifest, and leaves the tree alone',
          code == 0 and have == want and snapshot(tree) == before,
          'exit %d, files %r, %s' % (code, have, se.strip()))
    man = {}
    origins = {}
    if os.path.exists(os.path.join(cache, 'manifest.json')):
        doc = json.loads(get(os.path.join(cache, 'manifest.json')).decode('utf-8'))
        man = dict((e['output'], e) for e in doc['entries'])
        origins = doc['origins']
    beta = man.get('mod/scripts/Beta.class', {})
    against = sorted(beta.get('compiled_against', {}))
    check('the manifest names the class a source was compiled against, where it was '
          'read, and the class it looked for in vain',
          against == ['java.game.mod.Alpha'] and
          origins == {'java.game.mod.Alpha': 'mod/scripts/src/Alpha.java'} and
          beta.get('not_found') == ['java.lang.Object'],
          '%r %r %r' % (against, origins, beta.get('not_found')))
    check('the manifest records the packs: the file spelling for a pack that has a file, '
          'none for one that has not',
          beta.get('packs') == [{'written': 'packs\\things.rpk', 'pool': 'packs\\Things.rpk'},
                                {'written': 'packs\\nothing.rpk', 'pool': None}],
          repr(beta.get('packs')))
    blobs = tufa.parse(get(os.path.join(cache, 'mod', 'scripts', 'Beta.class'))) \
        if have == want else []
    rids = [b.rid(i) for b in blobs for i, e in enumerate(b.cons) if e[0] == 'rid']
    check('a pack without a file compiles to system.rpk id 0',
          rids == [('packs\\Things.rpk', 0x12), ('system.rpk', 0)], repr(rids))
    # the same run again: the same bytes
    first = dict((k, get(os.path.join(cache, k))) for k in have)
    code, so, se = run_cli(['--root', tree, '--out', cache])
    again = dict((k, get(os.path.join(cache, k))) for k in sorted(snapshot(cache)))
    check('a second run gives the same bytes, manifest included', code == 0 and again == first)
    # one source into a cache that has entries: the others stay
    code, so, se = run_cli(['--root', tree, '--out', cache, os.path.join(src, 'Alpha.java')])
    check('a run for one source keeps the manifest entries of the others',
          code == 0 and 'compiled 1 of 1 sources' in so and '(2 entries)' in so and
          get(os.path.join(cache, 'manifest.json')) == first.get('manifest.json'),
          'exit %d: %s' % (code, so.strip()))
    # a manifest that is none
    junk = os.path.join(tmp, 'junk')
    put(os.path.join(junk, 'manifest.json'), 'not a manifest')
    code, so, se = run_cli(['--root', tree, '--out', junk])
    check('a manifest of another format is replaced, with a warning',
          code == 0 and 'is not a manifest' in se and
          get(os.path.join(junk, 'manifest.json')) == first.get('manifest.json'),
          'exit %d: %s' % (code, se.strip()))
    # one source, the module form of the command line
    one = os.path.join(tmp, 'one')
    code, so, se = run_cli(['--root', tree, '--out', one,
                            os.path.join(src, 'Beta.java')], isolated=False, module=True)
    check('one source, started as "python -m slrrc"',
          code == 0 and sorted(snapshot(one)) == sorted(
              [os.path.join('mod', 'scripts', 'Beta.class'), 'manifest.json']) and
          get(os.path.join(one, 'mod', 'scripts', 'Beta.class')) == first.get(
              os.path.join('mod', 'scripts', 'Beta.class')),
          'exit %d: %s %s' % (code, so.strip(), se.strip()))
    # --write-install
    code, so, se = run_cli(['--root', tree, '--write-install'])
    ok = code == 0 and os.path.exists(alpha_class) and os.path.exists(beta_class)
    check('--write-install writes the class beside the source, with its time stamp',
          ok and int(os.stat(alpha_class).st_mtime) == STAMP and
          get(alpha_class) == first.get(os.path.join('mod', 'scripts', 'Alpha.class')) and
          get(beta_class) == first.get(os.path.join('mod', 'scripts', 'Beta.class')),
          'exit %d: %s' % (code, se.strip()))
    code, so, se = run_cli(['--root', tree, '--only-missing', '--dry-run'])
    check('--only-missing skips a source once its class file exists',
          code == 0 and 'would compile 0 of the 2 sources' in so, so.strip())
    put(os.path.join(src, 'Alpha.java'), ALPHA.replace('n = 3', 'n = 5'))
    kept = get(alpha_class) if ok else b''
    code, so, se = run_cli(['--root', tree, '--write-install'])
    check('--write-install never replaces a class file',
          code == 0 and ok and get(alpha_class) == kept and 'kept 2' in so, so.strip())
    code, so, se = run_cli(['--root', tree, '--write-install', '--overwrite'])
    check('... unless --overwrite is given',
          code == 0 and ok and get(alpha_class) != kept and
          int(os.stat(alpha_class).st_mtime) == STAMP, so.strip())
    # --compare on the tree it has just written
    code, so, se = run_cli(['--root', tree, '--compare'])
    check('--compare finds the class files it wrote identical',
          code == 0 and 'compared 2 sources with their class files: 2 identical' in so,
          so.strip())
    # a source that does not compile
    put(os.path.join(src, 'Broken.java'), BROKEN)
    bad = os.path.join(tmp, 'bad')
    code, so, se = run_cli(['--root', tree, '--out', bad])
    names = sorted(snapshot(bad))
    check('a source that does not compile: exit code 1, a diagnostic with line and code, '
          'no output for it, the others compiled',
          code == 1 and 'Broken.java:5:' in se and 'E_SYNTAX' in se and
          not any('Broken' in n for n in names) and len(names) == 3 and '1 FAILED' in so,
          'exit %d, %r, %s' % (code, names, se.strip()))
    stray = [k for k in snapshot(tree) if 'Stray' in k]
    check('a source outside a src directory is left alone',
          stray == [os.path.join('loose', 'Stray.java')], repr(stray))
    return out


# ---------------------------------------------------------------------------
# the checks without an oracle
# ---------------------------------------------------------------------------

def layout_rules(cache, tmp):
    """Run lib/check_layout.py on the cache directory: (rules that fail,
    number of MUST rules, blobs it saw)."""
    js = os.path.join(tmp, 'layout.json')
    p = subprocess.run([sys.executable, '-I', os.path.join(harness.ROOT, 'lib', 'check_layout.py'),
                        '--corpus', cache, '--json', js],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=harness.ROOT)
    if not os.path.exists(js):
        return ['check_layout.py did not run: %s' % p.stdout.decode('utf-8', 'replace')[-300:]], 0, 0
    with open(js, 'r', encoding='utf-8') as f:
        doc = json.load(f)
    bad = []
    must = 0
    for rid, r in sorted(doc['rules'].items()):
        if not r['must']:
            continue
        must += 1
        if r['ok'] != r['total']:
            bad.append('%s (%s): %d of %d; %s' % (rid, r['text'], r['ok'], r['total'],
                                                  '; '.join(r['exceptions'][:2])))
    if p.returncode != 0 and not bad:
        bad.append('check_layout.py exit code %d' % p.returncode)
    return bad, must, doc['blobs']


def provider_differences(corpus_root):
    """Class names the provider of the driver and the CorpusProvider of the
    tests declare differently: (names compared, differences)."""
    inst = install.Install(corpus_root)
    inst.scan()
    mine = install.InstallProvider(inst, True)
    mine.load()
    ref = harness.provider()
    names = sorted(set(ref.classes) | set(ref.pending) | set(mine.classes) | set(mine.pending))
    bad = []
    for n in names:
        a = mine.find(n)
        b = ref.find(n)
        ta = classes.declaration_text(a) if a is not None else None
        tb = classes.declaration_text(b) if b is not None else None
        if ta != tb:
            bad.append(n)
    if mine.unreadable:
        bad.append('unreadable: %s' % mine.unreadable[0][0])
    return len(names), bad, inst


def run(opts):
    oc = harness.Outcome('M6 source-only')
    corpus = oracle.corpus_root()
    entries = harness.selected(harness.source_only(), opts)
    c = Counter()
    fails = defaultdict(list)
    tmp = tempfile.mkdtemp(prefix='slrrc_m6_')
    cache = os.path.join(tmp, 'cache')
    passed = []
    hashes = []
    notes = oc.notes
    try:
        before = script_snapshot(corpus)
        t0 = time.time()
        args = ['--root', corpus, '--out', cache, '--only-missing']
        if opts.only is not None:
            args += [e.source for e in entries]
        code, so, se = run_cli(args)
        took = time.time() - t0
        last = [x for x in so.split('\n') if x][-1:] or ['']
        notes.append('command line: exit %d in %.0f s: %s' % (code, took, last[0]))
        if code != 0:
            oc.failures.append('the command line ended with exit code %d: %s' % (
                code, (se.strip() or so.strip())[-400:]))
        if se.strip():
            oc.failures.append('the command line wrote to stderr: %s' % se.strip()[:400])
        after = script_snapshot(corpus)
        tree_same = before == after
        if not tree_same:
            changed = sorted(set(before.items()) ^ set(after.items()))
            oc.failures.append('THE GAME TREE CHANGED: %s' % (changed[:3],))
        # ---- the manifest
        man = {}
        mpath = os.path.join(cache, cli.MANIFEST)
        doc = {}
        if os.path.exists(mpath):
            with open(mpath, 'r', encoding='utf-8') as f:
                doc = json.load(f)
            man = dict((e['output'], e) for e in doc['entries'])
        prov = harness.provider()
        written = snapshot(cache) if os.path.isdir(cache) else {}
        expected_files = set([cli.MANIFEST])
        # ---- shipped siblings by directory and shape
        sib = defaultdict(dict)
        sib_strict = defaultdict(dict)
        for e in oracle.iter_oracle_pairs():
            txt = oracle.load_source(e)
            k = sso.shape_key(txt)
            sib[e.dir].setdefault(k, e)
            sib_strict[e.dir].setdefault((k, sso.line_key(txt)), e)
        xrules = tuple(check_opcodes.Rule(r, r) for r in ('X1', 'X2', 'X3', 'X4', 'X5', 'X6'))
        for e in entries:
            c['sources'] += 1
            rel = '%s/%s.class' % (e.dir, e.name)
            expected_files.add(rel.replace('/', os.sep))
            path = os.path.join(cache, rel.replace('/', os.sep))
            if not os.path.exists(path):
                fails['compile'].append('%s: no output' % e.source)
                continue
            with open(path, 'rb') as f:
                data = f.read()
            c['compiled'] += 1
            c['bytes'] += len(data)
            hashes.append('%s  %s' % (sha(data), rel))
            passed.append(e.source)
            # ---- the manifest entry
            m = man.get(rel)
            src_bytes = oracle.load_source_bytes(e)
            why = None
            if m is None:
                why = 'no entry'
            elif m['source'] != e.source:
                why = 'source %r' % m['source']
            elif m['source_size'] != len(src_bytes) or m['source_sha256'] != sha(src_bytes):
                why = 'size or hash of the source'
            elif m['size'] != len(data) or m['sha256'] != sha(data):
                why = 'size or hash of the output'
            elif m['float'] != 'A' or m['build'] != '0x24F9D' or m['strict'] is not False:
                why = 'float %r build %r strict %r' % (m['float'], m['build'], m['strict'])
            elif m['compiler'] != 'slrrc ' + slrrc.VERSION:
                why = 'compiler %r' % m['compiler']
            elif m['pack_table'] != packs.TABLE_940_VERSION:
                why = 'pack table %r' % m['pack_table']
            else:
                for dep, digest in sorted(m['compiled_against'].items()):
                    info = prov.find(dep)
                    if info is None or digest != sha(
                            classes.declaration_text(info).encode('latin-1')):
                        why = 'declarations of %s' % dep
                        break
                    where = doc.get('origins', {}).get(dep)
                    if where is None or not os.path.isfile(os.path.join(
                            corpus, where.replace('/', os.sep))):
                        why = 'origin of %s: %r' % (dep, where)
                        break
                    c['dependencies'] += 1
                for name in m['not_found']:
                    if prov.find(name) is not None or name in m['compiled_against']:
                        why = 'not_found names %s, which exists' % name
                    c['absences'] += 1
            # ---- the strict reader
            try:
                blobs = tufa.parse(data)
                for b in blobs:
                    bad = tufa.validate(b)
                    if bad:
                        raise tufa.TufaError(bad[0])
                if tufa.serialise(blobs) != data:
                    raise tufa.TufaError('does not serialise to the same bytes')
                c['strict'] += 1
            except tufa.TufaError as x:
                fails['strict'].append('%s: %s' % (e.source, x))
                continue
            if why is None and m['classes'] != [b.name for b in blobs]:
                why = 'classes %r' % (m['classes'],)
            if why is None:
                c['manifest'] += 1
            else:
                fails['manifest'].append('%s: %s' % (rel, why))
            mine = classfile.read(data, True)
            if not mine.ok or [classfile.class_info(b).name for b in mine.blobs] != \
                    [b.name for b in blobs]:
                fails['own reader'].append('%s: %s' % (e.source, mine.error))
            # ---- structure and stack
            for b in blobs:
                c['classes'] += 1
                ti = sso.tree_info(b)
                for t, nodes in enumerate(b.trees):
                    c['trees'] += 1
                    c['nodes'] += len(nodes)
                    if not nodes:
                        c['empty trees'] += 1
                        continue
                    kind, is_ctor, st, nargs, ret_void = ti[t]
                    ok = True
                    try:
                        destruct.Destruct(b, nodes, kind, is_ctor, st, nargs, ret_void).run()
                    except Exception as x:      # noqa: the analysis raises for a misfit
                        ok = False
                        fails['statement forms'].append('%s tree %d: %s' % (e.source, t, x))
                    try:
                        decomp.decompile(b, nodes)
                    except Exception as x:      # noqa
                        ok = False
                        fails['expression shapes'].append('%s tree %d: %s' % (e.source, t, x))
                    if ok:
                        c['structure'] += 1
                    sso.simulate_tree(b, nodes, kind, st, nargs, xrules,
                                      '%s tree %d' % (e.source, t))
            # ---- siblings
            text = oracle.load_source(e)
            tw = sib.get(e.dir, {}).get(sso.shape_key(text))
            if tw is not None:
                c['with twin'] += 1
                if sso.node_shape(blobs) == sso.node_shape(oracle.load_class(tw)):
                    c['twin same opcodes'] += 1
                else:
                    fails['sibling opcodes'].append('%s against %s' % (e.source, tw.cls))
            tw = sib_strict.get(e.dir, {}).get((sso.shape_key(text), sso.line_key(text)))
            if tw is not None:
                c['with strict twin'] += 1
                if sso.node_shape_strict(blobs) == sso.node_shape_strict(oracle.load_class(tw)):
                    c['strict twin equal'] += 1
                else:
                    fails['siblings'].append('%s against %s' % (e.source, tw.cls))
        for r in xrules:
            if r.ok != r.total:
                fails['stack simulation ' + r.rid] = list(r.bad)
        extra = sorted(set(written) - expected_files)
        if extra:
            fails['files'].append('files nobody asked for: %s' % extra[:5])
        if opts.only is None and len(man) != len(entries):
            fails['manifest'].append('%d entries for %d sources' % (len(man), len(entries)))
        if doc and (doc.get('pack_table') != cli.pack_table_info() or
                    doc.get('generator') != 'slrrc ' + slrrc.VERSION or
                    doc.get('format') != cli.MANIFEST_FORMAT):
            fails['manifest'].append('header %r' % {k: doc.get(k) for k in (
                'format', 'generator', 'pack_table')})
        # ---- the layout rules of the foundation, on the directory
        bad_rules, n_must, n_blobs = layout_rules(cache, tmp) if c['compiled'] else ([], 0, 0)
        for x in bad_rules:
            fails['layout'].append(x)
        if n_blobs != c['classes']:
            fails['layout'].append('check_layout.py saw %d blobs, the files hold %d' % (
                n_blobs, c['classes']))
        # ---- the provider of the driver against the provider of the tests
        n_names, bad_names, inst = provider_differences(corpus)
        for n in bad_names[:10]:
            fails['provider'].append('declared differently: %s' % n)
        missing = sum(1 for sf in inst.sources if sf.class_rel is None)
        if (len(inst.class_files), len(inst.sources), missing) != (2921, 3454, 635):
            fails['provider'].append('the tree has %d class files, %d sources, %d without a '
                                     'class file' % (len(inst.class_files), len(inst.sources),
                                                     missing))
        if tuple(classfile.OP_HAS_OPERAND) != tuple(tufa.HAS_IMM):
            fails['own reader'].append('the opcode table differs from lib/tufa.py')
        # ---- the command line on a small tree
        drv = driver_checks(tmp)
        for what, why in drv:
            if why is not None:
                fails['driver'].append('%s: %s' % (what, why))
        drv_ok = sum(1 for _, why in drv if why is None)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for k in sorted(fails):
        for x in fails[k]:
            oc.failures.append('%s: %s' % (k, str(x)[:300]))
    with_nodes = c['trees'] - c['empty trees']
    x1 = xrules[0]
    sim_ok = all(r.ok == r.total for r in xrules)
    oc.text = ('%d/%d compiled through the cache (%d classes, %d trees, %d nodes, %d bytes); '
               'hashes %s equal to the baseline; strict reader %d/%d; layout rules %s; '
               'stack simulation %s (%d nodes); structure %d/%d trees; siblings %d/%d; '
               'manifest %d/%d entries; driver checks %d/%d' % (
                   c['compiled'], c['sources'], c['classes'], c['trees'], c['nodes'],
                   c['bytes'], '%d/%d' % (len(hashes) - len(harness.Baseline(
                       'm6.sha256').compare(hashes)[1]), c['sources']),
                   c['strict'], c['compiled'],
                   'hold' if not bad_rules and n_must else 'FAIL',
                   'holds' if sim_ok else 'FAILS', x1.total,
                   c['structure'], with_nodes, c['strict twin equal'], c['with strict twin'],
                   c['manifest'], c['sources'], drv_ok, len(drv)))
    full = (c['sources'] == c['compiled'] == c['strict'] == c['manifest'] == 635 and
            c['classes'] == 635 and c['trees'] == 771 and c['nodes'] == 32644 and
            c['bytes'] == 693847 and x1.total == 21523 and
            c['structure'] == with_nodes == 767 and
            c['strict twin equal'] == c['with strict twin'] == 522 and
            c['twin same opcodes'] == c['with twin'] == 528 and
            not harness.Baseline('m6.sha256').compare(hashes)[1])
    oc.ok = (full and not oc.failures and tree_same and sim_ok and not bad_rules and
             n_must > 0 and drv_ok == len(drv) and code == 0)
    oc.passed['m6.pass'] = passed
    oc.passed['m6.sha256'] = hashes
    notes.append('layout: %d MUST rules of lib/check_layout.py on %d blobs; stack simulation: %s'
                 % (n_must, n_blobs, ', '.join('%s %d of %d' % (r.rid, r.ok, r.total)
                                               for r in xrules)))
    notes.append('siblings with the same token shape: %d, same opcode sequence %d; with every '
                 'token on the same line: %d, equal %d' % (
                     c['with twin'], c['twin same opcodes'], c['with strict twin'],
                     c['strict twin equal']))
    notes.append('manifest: %d entries, %d dependencies with their declaration hash recomputed '
                 'from the provider of the tests, %d names recorded as not found' % (
                     len(man), c['dependencies'], c['absences']))
    notes.append('provider of the driver: %d class names declared as the provider of the '
                 'tests declares them, %d differently' % (n_names - len(bad_names),
                                                          len(bad_names)))
    notes.append('game tree: %d class files, sources and packs with the same size and time '
                 'stamp before and after: %s' % (len(before), 'yes' if tree_same else 'NO'))
    for what, why in drv:
        notes.append('driver: %s: %s' % (what, 'ok' if why is None else 'FAILED'))
    return oc


if __name__ == '__main__':
    sys.exit(harness.main_single(run, sys.argv[1:]))
