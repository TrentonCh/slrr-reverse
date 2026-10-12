#!/usr/bin/env python3
"""harness.py - what the milestone tests share.

    corpus      the four groups of notes/compiler_spec.md: oracle pairs (Build
                940, consistent), older-build pairs, stale pairs, sources
                without a class; taken from the index of lib/oracle.py
    provider    CorpusProvider: the ClassProvider of the tests.  It answers
                from the shipped class files (read with lib/tufa.py) and, for
                the classes that have no class file, declares them from their
                source with the compiler under test
    packs       the PackNames of the tests: the pack table of a session of
                the game, replayed over the pack files of the corpus
                (slrrc/packs.py: session_940)
    baseline    tests/baseline/<name>: the sorted list of passing items of a
                milestone; a run that loses one is a regression
    headline    the one line every test prints

The package under test (slrrc) imports nothing from lib/, analysis/ or spec/;
the tests do, as oracles.  Standard library only: python -I tests/run_tests.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
BASELINE_DIR = os.path.join(HERE, 'baseline')
for _p in (os.path.join(ROOT, 'spec'), os.path.join(ROOT, 'analysis', 'decl'),
           os.path.join(ROOT, 'lib'), ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import oracle   # noqa: E402  (lib)
import tufa     # noqa: E402  (lib)

import slrrc                    # noqa: E402
from slrrc import classes       # noqa: E402

IMPLS = ('py', 'cpp')

# slrrc.parser.MAX_DEPTH is the limit under test, not the interpreter's
sys.setrecursionlimit(20000)


class NotBuilt(Exception):
    """The implementation asked for with --impl does not exist yet."""


def require_impl(impl):
    if impl not in IMPLS:
        raise NotBuilt('unknown implementation %r (py or cpp)' % impl)
    if impl == 'cpp':
        raise NotBuilt('--impl cpp: the C++ compiler is not built (milestone M7)')


# The modules of the package that may touch the world: the driver and the
# host side of the two provider interfaces.  Every other module is the
# compiler core.
DRIVER_MODULES = ('__main__.py', 'cli.py', 'install.py')
# what the core may import from the standard library (floatlit: the bits of a float)
CORE_MAY_IMPORT = ('struct',)
# functions the core does not call: file access, output, reflection
CORE_MAY_NOT_NAME = ('open', 'print', 'input', 'exec', 'eval', 'compile', '__import__',
                     'getattr', 'setattr', 'delattr', 'hasattr', 'globals', 'locals', 'vars')


def package_is_self_contained():
    """The package under test may import its own modules and the standard
    library, nothing else: in particular nothing from lib/, analysis/ or
    spec/.  Its core (every module but the driver's) imports nothing but
    `struct`, opens no file, prints nothing, uses no reflection and holds no
    `global` statement.  Returns None, or what was found."""
    import ast as pyast
    foreign = {}
    for d in ('lib', 'spec', os.path.join('analysis', 'decl'),
              os.path.join('analysis', 'expr'), os.path.join('analysis', 'stmt')):
        for f in os.listdir(os.path.join(ROOT, d)):
            if f.endswith('.py'):
                foreign[f[:-3]] = d
    pkg = os.path.join(ROOT, 'slrrc')
    for f in sorted(os.listdir(pkg)):
        if not f.endswith('.py'):
            continue
        core = f not in DRIVER_MODULES
        with open(os.path.join(pkg, f), 'r', encoding='utf-8') as fh:
            tree = pyast.parse(fh.read(), f)
        for node in pyast.walk(tree):
            if (core and isinstance(node, pyast.Call) and isinstance(node.func, pyast.Name)
                    and node.func.id in CORE_MAY_NOT_NAME):
                return 'slrrc/%s line %d calls %s (the compiler core does no I/O and no ' \
                       'reflection)' % (f, node.lineno, node.func.id)
            if core and isinstance(node, (pyast.Global, pyast.Nonlocal)):
                return 'slrrc/%s line %d: a global statement in the compiler core' % (
                    f, node.lineno)
            if isinstance(node, pyast.Import):
                names = [a.name.split('.')[0] for a in node.names]
            elif isinstance(node, pyast.ImportFrom):
                if node.level > 0:
                    if core and node.module in ('cli', 'install'):
                        return 'slrrc/%s imports the driver module %s' % (f, node.module)
                    if core and node.module is None:
                        for a in node.names:
                            if a.name in ('cli', 'install'):
                                return 'slrrc/%s imports the driver module %s' % (f, a.name)
                    continue                    # relative: the package itself
                names = [node.module.split('.')[0]]
            else:
                continue
            for n in names:
                if n == 'slrrc' and not core:
                    continue                    # the entry point names its own package
                if n in foreign:
                    return 'slrrc/%s imports %s (%s)' % (f, n, foreign[n])
                if n not in sys.stdlib_module_names:
                    return 'slrrc/%s imports %s, not a standard module' % (f, n)
                if core and n not in CORE_MAY_IMPORT:
                    return 'slrrc/%s imports %s (the compiler core imports only %s)' % (
                        f, n, ', '.join(CORE_MAY_IMPORT))
    return None


# ---------------------------------------------------------------------------
# corpus
# ---------------------------------------------------------------------------

def all_sources():
    """Every source of the corpus (3,454 entries of the index)."""
    return [e for e in oracle.iter_entries() if e.source is not None]


def oracle_pairs():
    """The 2,651 Build 940 pairs fit for byte comparison."""
    return list(oracle.iter_oracle_pairs())


def other_pairs():
    """The pairs outside the oracle set: stale ones and older builds (168)."""
    keep = set(e.key for e in oracle.iter_oracle_pairs())
    return [e for e in oracle.iter_entries(('pair_940', 'pair_other'))
            if e.key not in keep]


def older_pairs():
    """Pairs written by older builds that are consistent (153)."""
    return [e for e in oracle.iter_entries('pair_other') if e.consistent]


def stale_pairs():
    """Pairs whose source was edited after the class was compiled (15)."""
    return [e for e in oracle.iter_entries(('pair_940', 'pair_other'))
            if not e.consistent]


def source_only():
    """Sources without a class file (635)."""
    return list(oracle.iter_entries('source_only'))


def load_source(e):
    """Text of a source, one character per byte."""
    return oracle.load_source(e)


# ---------------------------------------------------------------------------
# the class provider of the tests
# ---------------------------------------------------------------------------

def info_from_blob(blob):
    """ClassInfo of a shipped class.  Fields: the static vector, then the
    instance vector.  Methods: in source order, which is the order of their
    tree numbers (rows without a tree keep their row position behind)."""
    info = classes.ClassInfo(blob.name)
    info.super = blob.super_name
    info.interfaces = list(blob.interface_names)
    info.is_interface = blob.is_interface
    for static, _, f in blob.fields():
        info.fields.append(classes.FieldInfo(blob.utf8(f.name), blob.utf8(f.type), static))
    rows = []
    for static, _, m in blob.methods():
        key = m.tree if m.tree != tufa.NONE else 1 << 30
        rows.append((key, len(rows), classes.MethodInfo(blob.utf8(m.name),
                                                        blob.utf8(m.desc), static)))
    rows.sort(key=lambda r: (r[0], r[1]))
    info.methods = [r[2] for r in rows]
    return info


class CorpusProvider(classes.ClassProvider):
    """Every class of the corpus by its full name.

    A class file wins over a source.  A name that several class files define
    (12 names) keeps the first file in index order, except that a file named
    like the class wins over a blob inside another file (the rule of
    analysis/expr/classdb.py, which the composition spike used).  A class
    that only a source declares is declared from that source on first use;
    while that is going on its name answers with an empty ClassInfo, so that
    two sources that name each other cannot recurse."""

    def __init__(self):
        self.classes = {}       # full name -> ClassInfo
        self.files = {}         # full name -> class file it came from
        self.pending = {}       # full name -> index entry of the declaring source
        self.in_progress = {}   # full name -> stub, while its source is declared
        self.failed = []        # (source, diagnostics) of sources that did not declare
        self.pack_spellings = {}    # lower-case pack file name -> its spelling in the pools
        for e in oracle.iter_entries():
            if e.cls is None:
                continue
            for blob in oracle.load_class(e):
                for ent in blob.cons:
                    if ent[0] == 'rid':
                        p = blob.utf8(ent[1])
                        self.pack_spellings.setdefault(p.lower(), p)
                info = info_from_blob(blob)
                simple = info.name.rpartition('.')[2]
                old = self.files.get(info.name)
                if old is not None:
                    old_simple = os.path.basename(old)[:-len('.class')]
                    if not (e.name == simple and old != e.cls and old_simple != simple):
                        continue
                self.classes[info.name] = info
                self.files[info.name] = e.cls
        for e in oracle.iter_entries('source_only'):
            for full in e.declared_names or []:
                if full not in self.classes and full not in self.pending:
                    self.pending[full] = e

    def find(self, fqn):
        info = self.classes.get(fqn)
        if info is not None:
            return info
        info = self.in_progress.get(fqn)
        if info is not None:
            return info
        e = self.pending.get(fqn)
        if e is None:
            return None
        mine = [n for n in e.declared_names if self.pending.get(n) is e]
        for n in mine:
            del self.pending[n]
            self.in_progress[n] = classes.ClassInfo(n)
        res = slrrc.declare_source(load_source(e), e.source, self,
                                   slrrc.Options(strict=True))
        for n in mine:
            del self.in_progress[n]
        if not res.ok:
            self.failed.append((e.source, res.diagnostics))
            return None
        for ci in res.infos:
            if ci.name in mine:
                self.classes[ci.name] = ci
        return self.classes.get(fqn)


_PROVIDER = None


def provider():
    """The one CorpusProvider of a test run (about 3 s to build)."""
    global _PROVIDER
    if _PROVIDER is None:
        _PROVIDER = CorpusProvider()
    return _PROVIDER


# ---------------------------------------------------------------------------
# the pack names of the tests
# ---------------------------------------------------------------------------

_PACK_FILES = None
_PACK_DISK = None


def pack_files():
    """The .rpk files of the corpus as the directory listing spells them,
    relative to the corpus root, with backslashes.  (Walked once; the tests
    do not change the corpus.)"""
    global _PACK_FILES
    if _PACK_FILES is None:
        out = []
        root = oracle.corpus_root()
        for dp, dn, fn in os.walk(root):
            dn[:] = sorted(d for d in dn if not d.lower().startswith(oracle.SKIP_DIR_PREFIX))
            for f in sorted(fn):
                if f.lower().endswith('.rpk'):
                    out.append(os.path.relpath(os.path.join(dp, f), root).replace('/', '\\'))
        _PACK_FILES = out
    return list(_PACK_FILES)


def pack_disk():
    """The PackFiles of the corpus: which pack files exist, and the external
    reference table at the head of each (read once per file)."""
    global _PACK_DISK
    if _PACK_DISK is None:
        from slrrc import install
        _PACK_DISK = install.DiskPackFiles(oracle.corpus_root(), pack_files())
    return _PACK_DISK


def packs(scans=True):
    """The PackNames of the tests: the pack table of a session of the game
    as it stands when the scripts are compiled (slrrc.packs.session_940:
    system.rpk, then the pack scans of the constructor of GameLogic), over
    the pack files of the corpus.  A pack in a scanned directory is spelled
    as the directory listing spells it, any other pack as it is first asked
    for, a pack without a file has no name (the compiler then writes
    system.rpk, id 0).

    Every call gives a NEW table: the table has state (the first spelling
    asked for stays), and one test must not see what another one asked.  The
    answers for the sources of the corpus do not depend on the order in
    which they are compiled (tests/t9_exe.py checks that); they are the ones
    of the 126-entry table of tests/pack_oracle.py, which is the rule of
    spec/spike_compiler.py."""
    from slrrc import packs as pk
    return pk.session_940(pack_disk(), pack_files(), scans)


def table_packs():
    """The stateless PackNames of the old rule, for synthetic sources: the
    table of tests/pack_oracle.py and no file list."""
    import pack_oracle
    return pack_oracle.TablePackNames(pack_oracle.TABLE_940, None)


# ---------------------------------------------------------------------------
# the diagnostics a shipped source is expected to give
# ---------------------------------------------------------------------------
# The milestone tests compile in strict mode and take a diagnostic of any kind
# for a failure.  One shipped source has to give two: TimeAttackTest names a
# pack that has no file, twice, and the compiler says so wherever it writes
# system.rpk id 0 for a resource id (W_PACK, a warning in both modes).  The
# list is exact: a diagnostic that is not on it fails the source, and so does
# a listed one that does not come.

EXPECTED_DIAGNOSTICS = {
    'sl/Scripts/game/CareerEvents/src/TimeAttackTest.java': (('W_PACK', 21), ('W_PACK', 22)),
}


def surprises(source, diagnostics):
    """What the diagnostics of a full compilation of the corpus source
    `source` (index path) hold beyond what is expected of it, and what they
    lack: a list of texts, empty when all is as expected."""
    want = list(EXPECTED_DIAGNOSTICS.get(source, ()))
    out = []
    for d in diagnostics:
        key = (d.code, d.line)
        if not d.error and key in want:
            want.remove(key)
        else:
            out.append('line %d column %d: %s %s' % (d.line, d.column, d.code, d.text))
    for code, line in want:
        out.append('line %d: the expected %s did not come' % (line, code))
    return out


# ---------------------------------------------------------------------------
# baseline
# ---------------------------------------------------------------------------

class Baseline(object):
    """tests/baseline/<name>: one passing item per line, sorted."""

    def __init__(self, name):
        self.name = name
        self.path = os.path.join(BASELINE_DIR, name)

    def load(self):
        """The stored items, or None when the file does not exist."""
        if not os.path.exists(self.path):
            return None
        with open(self.path, 'r', encoding='utf-8') as f:
            return [x for x in f.read().split('\n') if x and not x.startswith('#')]

    def compare(self, passed):
        """(lost, new): items of the baseline that no longer pass, and items
        that pass and are not in the baseline.  Without a stored baseline
        nothing is lost and everything is new."""
        old = self.load()
        have = set(passed)
        if old is None:
            return [], sorted(have)
        was = set(old)
        return sorted(was - have), sorted(have - was)

    def update(self, passed):
        os.makedirs(BASELINE_DIR, exist_ok=True)
        with open(self.path, 'w', encoding='utf-8', newline='\n') as f:
            for x in sorted(set(passed)):
                f.write(x + '\n')


# ---------------------------------------------------------------------------
# outcome of a test, headline, command line
# ---------------------------------------------------------------------------

class Outcome(object):
    """What a milestone test returns: its headline, whether the headline
    shows what the milestone asks for, the pass lists per baseline file, and
    the failures in detail."""

    def __init__(self, label):
        self.label = label          # 'M1 tokenise'
        self.text = ''              # the rest of the headline
        self.ok = False
        self.passed = {}            # baseline file name -> list of items
        self.failures = []          # readable lines, one per failing item
        self.notes = []             # further lines printed with --verbose


def headline(label, text):
    return '%-17s%s' % (label, text)


class Options(object):
    def __init__(self):
        self.impl = 'py'
        self.update = False
        self.verbose = False
        self.only = None            # substring a source path must contain
        self.names = []             # positional arguments


def parse_args(argv):
    o = Options()
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--impl':
            o.impl = argv[i + 1]
            i += 1
        elif a.startswith('--impl='):
            o.impl = a[len('--impl='):]
        elif a == '--update':
            o.update = True
        elif a in ('-v', '--verbose'):
            o.verbose = True
        elif a == '--only':
            o.only = argv[i + 1]
            i += 1
        elif a.startswith('-'):
            raise SystemExit('unknown option %s' % a)
        else:
            o.names.append(a)
        i += 1
    return o


def selected(entries, opts):
    """The entries a run looks at (--only narrows it for debugging)."""
    if opts.only is None:
        return entries
    q = opts.only.replace('\\', '/').lower()
    return [e for e in entries if q in (e.source or e.cls).lower()]


def settle(outcomes, opts, out=None):
    """Print the headlines, compare every pass list with its baseline and
    print the baseline line.  Returns the exit code: 1 when a headline is
    not what its milestone asks for or when a baseline item was lost."""
    w = (out or sys.stdout).write
    lost_all = []
    new_all = []
    for oc in outcomes:
        w(headline(oc.label, oc.text) + '\n')
        for name, items in sorted(oc.passed.items()):
            lost, new = Baseline(name).compare(items)
            lost_all += ['%s: %s' % (name, x) for x in lost]
            new_all += [(name, x) for x in new]
    partial = opts.only is not None
    if partial:
        w(headline('baseline', 'not compared (--only)') + '\n')
    else:
        w(headline('baseline', '%d lost' % len(lost_all)) + '\n')
    for oc in outcomes:
        shown = oc.failures if opts.verbose else oc.failures[:20]
        for x in shown:
            w('  FAIL %s: %s\n' % (oc.label.split()[0], x))
        if len(oc.failures) > len(shown):
            w('  ... %d more failures (-v shows all)\n' % (len(oc.failures) - len(shown)))
        if opts.verbose:
            for x in oc.notes:
                w('  %s\n' % x)
    if not partial:
        for x in lost_all[:50]:
            w('  LOST %s\n' % x)
        by = {}
        for name, x in new_all:
            by[name] = by.get(name, 0) + 1
        text = ', '.join('%s %d' % (k, v) for k, v in sorted(by.items()))
        if opts.update and lost_all:
            w('  baseline NOT updated: items were lost\n')
        elif opts.update and new_all:
            for oc in outcomes:
                for name, items in oc.passed.items():
                    Baseline(name).update(items)
            w('  baseline updated: new passes %s\n' % text)
        elif new_all:
            w('  new passes not in the baseline (%s); --update stores them\n' % text)
    bad = [oc for oc in outcomes if not oc.ok]
    if partial:
        return 1 if any(oc.failures for oc in outcomes) else 0
    return 1 if (bad or lost_all) else 0


def main_single(run, argv):
    """Entry point of one test script."""
    opts = parse_args(argv)
    try:
        require_impl(opts.impl)
    except NotBuilt as x:
        print(x)
        return 2
    return settle([run(opts)], opts)
