"""cli.py - the command line of slrrc.

    python -I slrrc --root GAME --out CACHE [SOURCE ...]     compile into a cache directory
    python -I slrrc --root GAME --out CACHE --only-missing   ... only what has no class file
    python -I slrrc --root GAME --out CACHE --dry-run        list what would be done
    python -I slrrc --root GAME --check [SOURCE ...]         compile, report, write nothing
    python -I slrrc --root GAME --compare [SOURCE ...]       compile and compare with the
                                                             class files of the tree
    python -I slrrc --root GAME --dump trees SOURCE          print tokens, ast, trees or class

(run from the directory that holds the package; "python -m slrrc" works too,
but not together with -I, which keeps the current directory off the module
path.)

GAME is a tree laid out like an install of the game (install.py): the class
files, the sources in their src directories, the packs.  It is read, never
written: the compiler creates, changes and deletes nothing below --root unless
--write-install asks for class files beside their sources, and an --out
directory inside the tree is refused.

SOURCE names a source of the tree (absolute, or relative to the current
directory or to the root); without one every source of the tree is taken.
--only-missing keeps the sources that have no class file beside them, or one
the reader does not accept.

THE CACHE DIRECTORY has the layout of the tree: the class file of
X/src/Name.java is CACHE/X/Name.class, an ordinary class file with one blob per
class of the source.  Beside the class files stands CACHE/manifest.json, which
records for every output

    output, size, sha256                 the class file, relative to CACHE
    source, source_size, source_sha256   the source, relative to the root
    compiler, build, float, strict       how it was compiled
    pack_table                           the version of the pack table replay
                                         (packs.SESSION_940_VERSION)
    classes                              the classes in the file
    compiled_against                     every class the compiler looked at
                                         outside the source: its name and the
                                         sha256 of its declarations
                                         (classes.declaration_text)
    not_found                            every class name it asked for and
                                         did not find (a type name is tried in
                                         the package of the source first)
    packs                                every pack a resource id names, as
                                         written and as spelled in the pool
                                         (null: the pack has no file)
    warnings                             number of warnings

An output is a function of exactly these things, so a host can tell from the
manifest whether an entry is still valid.  (The pool spelling of a pack is
state of the pack table of the run, install.py; the entry records the
spelling the output was written with.)  The head of the manifest names the
tree, the pack table replay with its scan list and, under "origins", the file
each of those classes was read from in the run that looked at it last.  A run
into a directory that has a manifest keeps the entries of other outputs.
Files are written through a temporary file and a rename; the manifest has no
time stamp, so two runs over the same tree give the same bytes.

THE CACHE STAYS CONSISTENT WHEN A RUN FAILS.  An entry of the manifest always
describes the class file that is there:

  - the manifest is written once before anything is compiled: a directory in
    which it cannot be written is refused before a class file is replaced;
  - a class file that cannot be written (another program has it open) fails
    that one source: the old file and its entry stay, the run goes on, the
    exit code is 1; a source that cannot be read is treated the same way;
  - the manifest is written again when the run ends, also when it ends with
    an exception;
  - a temporary file is removed when the rename fails;
  - a manifest.json that is not a manifest of this format (other JSON, an
    entry without the fields this module writes) is replaced, with a warning.

OUTPUT.  Messages are text; a character the encoding of a redirected stdout
or stderr lacks is written as a backslash escape and never ends the run.  A
dump (--dump) written to a file or a pipe is bytes, one byte per character of
the source and LF line ends, whatever the locale: it is the format another
implementation is compared with.

FLOAT LITERALS.  The Build 940 exe has one conversion, setting B
(floatlit.py, notes/exe_reads.md 1), and that is the default of --float.  A,
C and D reproduce shipped class files that earlier builds of the compiler
wrote; they are for --compare.  --compare --float auto tries B, A, C, D in
this order and reports the first that reproduces each class file.

NESTING.  The limit of the dialect is parser.MAX_DEPTH; the command line
raises the recursion limit of the interpreter (RECURSION_LIMIT, the value the
tests run under) so that this limit is the one a source meets.

Exit code: 0; 1 when a source did not compile or could not be read or
written or, with --compare, a class file differs; 2 for a command line that
cannot be carried out.
"""
import argparse
import hashlib
import json
import os
import sys

from . import BUILD_940, VERSION, Options, compile_source
from . import classes
from . import dump
from . import floatlit
from . import install
from . import packs
from . import scanner

MANIFEST = 'manifest.json'
MANIFEST_FORMAT = 1
DUMPS = ('tokens', 'ast', 'trees', 'class')
RECURSION_LIMIT = 20000         # frames; parser.MAX_DEPTH needs about 4000


# ---------------------------------------------------------------------------
# small things
# ---------------------------------------------------------------------------

def sha256(data):
    return hashlib.sha256(data).hexdigest()


def declaration_hash(info):
    """The hash a manifest records for a class an output was compiled
    against: sha256 of classes.declaration_text(), one byte per character."""
    return sha256(classes.declaration_text(info).encode('latin-1'))


def pack_table_info():
    """What the head of a manifest says about the pack names of a run: the
    replay of the pack table (packs.session_940) and its scan lists."""
    return {'version': packs.SESSION_940_VERSION,
            'scans': list(packs.SCANS_940),
            'scans_of_every_directory_of': list(packs.SUB_SCANS_940)}


def auto_modes():
    """The order of --compare --float auto: the conversion of the exe, then
    the three of earlier builds."""
    out = [floatlit.EXE_MODE]
    for m in floatlit.MODES:
        if m != floatlit.EXE_MODE:
            out.append(m)
    return out


def is_inside(path, root):
    """Does path lie in the tree below root (or is it the root)?"""
    p = os.path.normcase(os.path.realpath(path))
    r = os.path.normcase(os.path.realpath(root))
    try:
        return os.path.commonpath([p, r]) == r
    except ValueError:                  # different drives
        return False


def write_atomic(path, data):
    """Write a file through a temporary one in the same directory.  When
    that fails nothing is left behind: OSError, and the file that was there
    is untouched."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp%d' % os.getpid()
    try:
        with open(tmp, 'wb') as f:
            f.write(data)
        os.replace(tmp, path)
    except OSError:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def lenient(stream):
    """Make a text stream write a character its encoding lacks as a backslash
    escape.  (A redirected stdout has the encoding of the locale with strict
    errors: the name of a source, or a byte above 0x7F of a source, would end
    the run with UnicodeEncodeError.)  A stream that cannot be reconfigured,
    such as one a caller passes in, is left as it is."""
    reconfigure = getattr(stream, 'reconfigure', None)
    if reconfigure is not None:
        try:
            reconfigure(errors='backslashreplace')
        except (ValueError, OSError):
            pass


def write_dump(out, text):
    """Write a dump.  To a file or a pipe it goes as bytes: one byte per
    character (a dump of tokens holds source text, which is bytes) and LF
    line ends, the same on every system.  To a console, and to a text stream
    a caller passed in, it goes as text."""
    raw = getattr(out, 'buffer', None)
    if raw is None or out.isatty():
        out.write(text)
        return
    out.flush()
    raw.write(text.encode('latin-1'))
    raw.flush()


class RecordingPackNames(packs.PackNames):
    """A PackNames that remembers what it was asked and what it answered."""

    def __init__(self, inner):
        self.inner = inner
        self.seen = []              # [written path, pool spelling or None], first use first
        self.index = {}

    def canonical(self, written_path):
        name = self.inner.canonical(written_path)
        if written_path not in self.index:
            self.index[written_path] = True
            self.seen.append([written_path, name])
        return name

    def external(self, written_path, k):
        return self.inner.external(written_path, k)


class Console(object):
    """Where the messages go."""

    def __init__(self, out, err, quiet, verbose):
        self.out = out
        self.err = err
        self.quiet = quiet
        self.verbose = verbose

    def say(self, text):                # the result of the run
        self.out.write(text + '\n')

    def info(self, text):               # progress, unless -q
        if not self.quiet:
            self.out.write(text + '\n')

    def detail(self, text):             # per file, with -v
        if self.verbose:
            self.out.write(text + '\n')

    def problem(self, text):
        self.err.write(text + '\n')


# ---------------------------------------------------------------------------
# the manifest
# ---------------------------------------------------------------------------

# the fields of an entry and the JSON type each has (manifest_entry writes them)
ENTRY_FIELDS = (
    ('output', str), ('size', int), ('sha256', str),
    ('source', str), ('source_size', int), ('source_sha256', str),
    ('compiler', str), ('build', str), ('float', str), ('strict', bool),
    ('pack_table', str), ('classes', list), ('compiled_against', dict),
    ('not_found', list), ('packs', list), ('warnings', int),
)


def manifest_fault(doc):
    """Is a parsed manifest.json a manifest as this module writes it?  ''
    when it is, else what is wrong.  Only such a document is taken over: its
    entries are written back, and manifest_bytes() reads their fields."""
    if type(doc) is not dict:
        return 'the document is no object'
    if type(doc.get('format')) is not int or doc['format'] != MANIFEST_FORMAT:
        return 'format %r' % (doc.get('format'),)
    if type(doc.get('entries')) is not list:
        return 'it has no list "entries"'
    if type(doc.get('origins')) is not dict:
        return 'it has no object "origins"'
    seen = {}
    for e in doc['entries']:
        if type(e) is not dict:
            return 'an entry is no object'
        for key, kind in ENTRY_FIELDS:
            if type(e.get(key)) is not kind:
                return 'an entry has no usable "%s"' % key
        if e['output'] in seen:
            return 'two entries for %s' % e['output']
        seen[e['output']] = True
    return ''


def load_manifest(path, con):
    """An existing manifest: (its entries by output path, its origins); two
    empty dicts when there is none or it cannot be used."""
    if not os.path.exists(path):
        return {}, {}
    try:
        with open(path, 'r', encoding='utf-8') as f:
            doc = json.load(f)
        why = manifest_fault(doc)
        if why != '':
            raise ValueError(why)
        out = {}
        for e in doc['entries']:
            out[e['output']] = e
        return out, dict(doc['origins'])
    except (ValueError, OSError, RecursionError) as x:
        # ValueError: no JSON, not this format; RecursionError: JSON nested
        # deeper than the interpreter reads
        con.problem('warning: %s is not a manifest of this format (%s); it is replaced'
                    % (path, str(x)[:200]))
        return {}, {}


def manifest_bytes(root, entries, origins):
    """The manifest as it is written: the entries in the order of their
    output paths, keys sorted, no time stamp.  origins keeps the classes
    some entry names."""
    used = {}
    for e in entries.values():
        for name in e['compiled_against']:
            used[name] = origins.get(name)
    doc = {
        'format': MANIFEST_FORMAT,
        'generator': 'slrrc ' + VERSION,
        'root': root,
        'pack_table': pack_table_info(),
        'origins': used,
        'entries': [entries[k] for k in sorted(entries, key=install.lower)],
    }
    return (json.dumps(doc, indent=1, sort_keys=True) + '\n').encode('utf-8')


def manifest_entry(sf, data, res, mode, opts, provider, pack_log, origins):
    """The entry of one output; the files its classes were read from go to
    origins."""
    against = {}
    for name in res.consulted:
        info = provider.find(name)
        against[name] = declaration_hash(info) if info is not None else None
        origins[name] = provider.origin.get(name)
    warnings = 0
    for d in res.diagnostics:
        if not d.error:
            warnings += 1
    return {
        'output': sf.output_rel(),
        'size': len(res.class_file),
        'sha256': sha256(res.class_file),
        'source': sf.rel,
        'source_size': len(data),
        'source_sha256': sha256(data),
        'compiler': 'slrrc ' + VERSION,
        'build': '0x%X' % opts.build,
        'float': mode,
        'strict': bool(opts.strict),
        'pack_table': packs.SESSION_940_VERSION,
        'classes': list(res.class_names),
        'compiled_against': against,
        'not_found': sorted(res.missed),
        'packs': [{'written': w, 'pool': p} for w, p in pack_log.seen],
        'warnings': warnings,
    }


# ---------------------------------------------------------------------------
# the command line
# ---------------------------------------------------------------------------

def build_parser():
    p = argparse.ArgumentParser(
        prog='python -I slrrc',
        description='Compiler for the script sources of Street Legal Racing: Redline '
                    '(Build 940).  Reads a game tree, writes class files into a cache '
                    'directory; never writes into the tree unless --write-install is given.')
    p.add_argument('sources', nargs='*', metavar='SOURCE',
                   help='sources to compile (default: every source of the tree)')
    p.add_argument('--root', required=True, metavar='DIR',
                   help='the game tree: class files, sources (X/src/Name.java), packs')
    p.add_argument('--out', metavar='DIR',
                   help='cache directory to write: DIR/X/Name.class and DIR/manifest.json')
    p.add_argument('--only-missing', action='store_true',
                   help='only the sources that have no class file beside them (or one the '
                        'reader does not accept)')
    p.add_argument('-n', '--dry-run', action='store_true',
                   help='list what would be compiled and written; do neither')
    p.add_argument('--check', action='store_true',
                   help='compile and report, write nothing')
    p.add_argument('--compare', action='store_true',
                   help='compile the sources that have a class file and compare the bytes')
    p.add_argument('--dump', choices=DUMPS, metavar='WHAT',
                   help='print the tokens, ast, trees or class (pool and rows) of one SOURCE')
    p.add_argument('--float', default=floatlit.DEFAULT_MODE, metavar='A|B|C|D',
                   help='float literal conversion (default %s, the one of the Build 940 '
                        'exe; A, C, D reproduce shipped files of earlier builds; auto with '
                        '--compare: the first of %s that reproduces the file)' % (
                            floatlit.DEFAULT_MODE, ', '.join(auto_modes())))
    p.add_argument('--strict', action='store_true',
                   help='constructs no shipped source uses are errors, not warnings')
    p.add_argument('--build', metavar='N',
                   help='build number of the file header (default 0x%X; with --compare the '
                        'number of the class file compared with)' % BUILD_940)
    p.add_argument('--write-install', action='store_true',
                   help='write X/Name.class beside X/src/Name.java IN THE GAME TREE, with the '
                        'time stamp of the source; never replaces a class file')
    p.add_argument('--overwrite', action='store_true',
                   help='with --write-install: replace class files that exist')
    p.add_argument('--exclude', action='append', default=[], metavar='PREFIX',
                   help='skip directories whose name starts with PREFIX (may be repeated)')
    p.add_argument('-v', '--verbose', action='store_true', help='one line per source')
    p.add_argument('-q', '--quiet', action='store_true', help='errors and the last line only')
    return p


class Usage(Exception):
    """The command line cannot be carried out."""


def settle_args(args):
    """Check the combination of options; returns the build number or None."""
    modes = [x for x in (args.dry_run and '--dry-run', args.check and '--check',
                         args.compare and '--compare', args.dump and '--dump') if x]
    if len(modes) > 1 and not (args.dry_run and len(modes) == 2 and not args.dump):
        raise Usage('%s exclude each other' % ' and '.join(modes))
    writes = args.out is not None or args.write_install
    if (args.check or args.compare or args.dump) and writes:
        raise Usage('%s writes nothing: leave out --out and --write-install' % modes[-1])
    if not modes and not writes:
        raise Usage('nothing to do: give --out DIR (a cache directory), or --check, '
                    '--compare, --dry-run')
    if args.overwrite and not args.write_install:
        raise Usage('--overwrite belongs to --write-install')
    if args.float.lower() == 'auto':
        args.float = 'auto'
        if not args.compare:
            raise Usage('--float auto belongs to --compare')
    else:
        args.float = args.float.upper()
        if args.float not in floatlit.MODES:
            raise Usage('--float %s: one of A, B, C, D' % args.float)
    if args.dump and len(args.sources) != 1:
        raise Usage('--dump takes exactly one SOURCE')
    if not os.path.isdir(args.root):
        raise Usage('--root %s is not a directory' % args.root)
    if args.out is not None and is_inside(args.out, args.root):
        raise Usage('--out %s lies inside the game tree %s: refused.  The compiler writes '
                    'into the tree only with --write-install (class files beside their '
                    'sources)' % (args.out, args.root))
    if args.out is not None and is_inside(args.root, args.out):
        # a cache laid out like the tree, around the tree: one of its paths
        # could be a path of the tree
        raise Usage('--out %s contains the game tree %s: refused' % (args.out, args.root))
    build = None
    if args.build is not None:
        try:
            build = int(args.build, 0)
        except ValueError:
            raise Usage('--build %s is not a number' % args.build)
        if not 0 <= build <= 0xFFFFFFFF:
            raise Usage('--build %s is not a 32-bit number' % args.build)
    return build


def select(inst, args, con):
    """The sources a run is about, in index order.  --only-missing needs the
    provider loaded first: it tells which class files the reader rejects."""
    if args.sources:
        jobs = []
        for s in args.sources:
            sf = inst.find_source(s)
            if sf is None:
                if not os.path.isfile(s):
                    raise Usage('%s is neither a source of the tree nor a file' % s)
                if is_inside(s, inst.root):
                    raise Usage('%s lies in the tree but not in a src directory: its class '
                                'file has no place' % s)
                sf = inst.external_source(s)
            jobs.append(sf)
    else:
        jobs = list(inst.sources)
    if args.only_missing:
        jobs = [sf for sf in jobs if sf.lacks_class()]
    if args.compare:
        skipped = [sf for sf in jobs if sf.class_rel is None]
        jobs = [sf for sf in jobs if sf.class_rel is not None]
        if skipped:
            con.detail('%d sources have no class file to compare with' % len(skipped))
    return jobs


def tree_line(inst):
    missing = 0
    for sf in inst.sources:
        if sf.class_rel is None:
            missing += 1
    return 'root    %s (%d class files, %d sources, %d of them without a class file, %d packs)' % (
        inst.root, len(inst.class_files), len(inst.sources), missing, len(inst.pack_files))


def load_provider(inst, args, con):
    provider = install.InstallProvider(inst, args.strict)
    provider.load()
    for rel, why in provider.unreadable:
        con.problem('warning: %s: not a class file the reader accepts, ignored: %s' % (rel, why))
    for rel, why in provider.unreadable_sources:
        con.problem('warning: %s cannot be read: %s' % (rel, why))
    con.detail('classes %d read from %d class files, %d more declared by sources only' % (
        provider.class_count, len(inst.class_files) - len(provider.unreadable),
        len(provider.pending)))
    return provider


def print_diagnostics(res, name, con, warnings):
    for d in res.diagnostics:
        if d.error or warnings:
            con.problem(d.format(name))


# ---------------------------------------------------------------------------
# the runs
# ---------------------------------------------------------------------------

def run_dry(args, inst, jobs, con):
    con.say('dry run: nothing is compiled and nothing is written')
    con.say(tree_line(inst))
    if args.out is not None:
        con.say('output  %s' % os.path.abspath(args.out))
    for sf in jobs:
        rel = sf.output_rel()
        line = 'compile %s' % sf.rel
        if args.out is not None:
            line += ' -> %s' % rel
        if args.write_install:
            if sf.external:
                line += ' (outside the tree: not written beside it)'
            elif sf.class_rel is not None and not args.overwrite:
                line += ' (tree: %s exists and is kept)' % sf.class_rel
            elif sf.class_rel is not None:
                line += ' (tree: REPLACES %s)' % sf.class_rel
            else:
                line += ' (tree: new file %s)' % rel
        elif sf.class_rel is not None and args.verbose:
            line += ' (the tree has %s)' % sf.class_rel
        con.say(line)
    what = []
    if args.out is not None:
        what.append('write %d class files and %s into %s' % (
            len(jobs), MANIFEST, os.path.abspath(args.out)))
    if args.write_install:
        what.append('write class files into the game tree')
    if not what:
        what.append('write nothing')
    con.say('would compile %d of the %d sources of the tree and %s' % (
        len(jobs), len(inst.sources), ' and '.join(what)))
    return 0


class Tally(object):
    """What a compile run counts."""

    def __init__(self):
        self.compiled = 0
        self.failed = []            # sources that did not compile
        self.unwritten = []         # sources that could not be read, or whose output
        #                             could not be written
        self.n_bytes = 0
        self.n_classes = 0
        self.n_warnings = 0
        self.cached = 0             # class files written into the cache directory
        self.in_tree = 0            # class files written into the game tree
        self.kept = 0               # class files of the tree that were left as they are


def compile_one(sf, args, opts, inst, provider, table, out_dir, entries, origins, tally, con):
    """One source of a compile run.  Whatever goes wrong is said and counted
    here; the run goes on with the next source."""
    try:
        data = install.read_source(sf)
    except OSError as x:
        con.problem('error: %s cannot be read: %s' % (sf.rel, x))
        tally.unwritten.append(sf.rel)
        return
    log = RecordingPackNames(table)
    res = compile_source(data, sf.rel, provider, log, opts)
    print_diagnostics(res, sf.rel, con, not args.quiet)
    rel = sf.output_rel()
    if not res.ok:
        tally.failed.append(sf.rel)
        if rel in entries:
            del entries[rel]
            con.problem('note: %s no longer compiles; its manifest entry is removed, '
                        'the old class file in the cache is left alone' % sf.rel)
        return
    tally.compiled += 1
    tally.n_bytes += len(res.class_file)
    tally.n_classes += len(res.class_names)
    entry = manifest_entry(sf, data, res, args.float, opts, provider, log, origins)
    tally.n_warnings += entry['warnings']
    written = True
    if out_dir is not None:
        target = os.path.join(out_dir, rel.replace('/', os.sep))
        try:
            write_atomic(target, res.class_file)
            # the entry changes with the file, never without it
            entries[rel] = entry
            tally.cached += 1
        except OSError as x:
            con.problem('error: %s cannot be written: %s; the cache keeps what it had for '
                        '%s' % (target, x, sf.rel))
            written = False
    if args.write_install:
        if sf.external:
            con.problem('note: %s is outside the tree: not written beside it' % sf.rel)
        else:
            target = inst.path(rel)
            if os.path.exists(target) and not args.overwrite:
                tally.kept += 1
                con.detail('kept    %s (exists)' % rel)
            else:
                try:
                    write_atomic(target, res.class_file)
                    st = os.stat(sf.path)
                    os.utime(target, (st.st_atime, st.st_mtime))
                    tally.in_tree += 1
                except OSError as x:
                    con.problem('error: %s cannot be written: %s' % (target, x))
                    written = False
    if not written:
        tally.unwritten.append(sf.rel)
        return
    con.detail('compiled %s -> %s (%d bytes)' % (sf.rel, rel, len(res.class_file)))


def run_compile(args, build, inst, provider, jobs, con):
    table = install.install_packs(inst)
    opts = Options(build if build is not None else BUILD_940, args.float, args.strict)
    out_dir = os.path.abspath(args.out) if args.out is not None else None
    entries = {}
    origins = {}
    manifest = None
    if out_dir is not None:
        manifest = os.path.join(out_dir, MANIFEST)
        entries, origins = load_manifest(manifest, con)
        # Before anything is compiled: the manifest as it was read.  A cache
        # directory whose manifest cannot be written is found out here, when
        # no class file in it has been replaced yet.
        try:
            write_atomic(manifest, manifest_bytes(inst.root, entries, origins))
        except OSError as x:
            raise Usage('the manifest %s cannot be written (%s): nothing was compiled'
                        % (manifest, x))
    tally = Tally()
    try:
        for sf in jobs:
            compile_one(sf, args, opts, inst, provider, table, out_dir, entries, origins,
                        tally, con)
    finally:
        # also when the loop ends with an exception: the entries describe
        # the class files that were written
        if manifest is not None:
            write_atomic(manifest, manifest_bytes(inst.root, entries, origins))
    for rel, diags in provider.failed:
        con.problem('warning: %s could not be declared for another source: %s' % (
            rel, diags[0].format(rel) if diags else 'no class'))
    line = '%s %d of %d sources (%d classes, %d bytes, float setting %s)' % (
        'checked' if args.check else 'compiled', tally.compiled, len(jobs), tally.n_classes,
        tally.n_bytes, args.float)
    if tally.n_warnings:
        line += ', %d warnings' % tally.n_warnings
    if tally.failed:
        line += ', %d FAILED' % len(tally.failed)
    if tally.unwritten:
        line += ', %d NOT READ OR NOT WRITTEN' % len(tally.unwritten)
    if out_dir is not None:
        line += '; wrote %d class files and %s (%d entries) to %s' % (
            tally.cached, MANIFEST, len(entries), out_dir)
    if args.write_install:
        line += '; wrote %d class files into the game tree, kept %d that exist' % (
            tally.in_tree, tally.kept)
    if args.check:
        line += '; nothing written'
    con.say(line)
    return 1 if (tally.failed or tally.unwritten) else 0


def first_difference(a, b):
    n = min(len(a), len(b))
    i = 0
    while i < n and a[i] == b[i]:
        i += 1
    return i


def run_compare(args, build, inst, provider, jobs, con):
    table = install.install_packs(inst)
    modes = auto_modes() if args.float == 'auto' else (args.float,)
    same = 0
    by_mode = {}
    different = []
    failed = []
    for sf in jobs:
        try:
            data = install.read_source(sf)
            theirs = inst.read(sf.class_rel)
        except OSError as x:
            con.problem('error: %s or its class file cannot be read: %s' % (sf.rel, x))
            failed.append(sf.rel)
            continue
        number = build
        if number is None:
            number = provider.builds.get(sf.class_rel, BUILD_940)
        hit = None
        first = None
        ok = True
        for mode in modes:
            res = compile_source(data, sf.rel, provider, table,
                                 Options(number, mode, args.strict))
            if not res.ok:
                print_diagnostics(res, sf.rel, con, False)
                ok = False
                break
            if res.class_file == theirs:
                hit = mode
                break
            if first is None:
                first = res.class_file
            if not any(t.kind == scanner.T_FLOAT for t in res.ast.toks):
                break                   # no float literal: the setting changes nothing
        if not ok:
            failed.append(sf.rel)
        elif hit is not None:
            same += 1
            by_mode[hit] = by_mode.get(hit, 0) + 1
            con.detail('identical %s %s' % (hit, sf.class_rel))
        else:
            at = first_difference(first, theirs)
            different.append(sf.class_rel)
            con.info('different   %s: first difference at byte 0x%X (compiled %d bytes, '
                     'the tree has %d)' % (sf.class_rel, at, len(first), len(theirs)))
    line = 'compared %d sources with their class files: %d identical' % (len(jobs), same)
    if same:
        line += ' (float setting %s)' % ', '.join(
            '%s %d' % (m, by_mode[m]) for m in floatlit.MODES if m in by_mode)
    line += ', %d different' % len(different)
    if failed:
        line += ', %d did not compile' % len(failed)
    con.say(line)
    return 1 if (different or failed) else 0


def run_dump(args, build, inst, provider, jobs, con):
    sf = jobs[0]
    data = install.read_source(sf)
    res = compile_source(data, sf.rel, provider, install.install_packs(inst),
                         Options(build if build is not None else BUILD_940, args.float,
                                 args.strict))
    print_diagnostics(res, sf.rel, con, not args.quiet)
    if res.ast is None or res.ast.root is None:
        return 1
    if args.dump == 'tokens':
        write_dump(con.out, dump.dump_tokens(res.ast.source, res.ast.toks))
        return 0
    if args.dump == 'ast':
        write_dump(con.out, dump.dump_ast(res.ast))
        return 0
    if not res.ok:
        return 1
    if args.dump == 'trees':
        write_dump(con.out, dump.dump_trees(res.classes))
    else:
        write_dump(con.out, dump.dump_classes(res.classes))
    return 0


def main(argv=None, out=None, err=None):
    """Run the command line; returns the exit code."""
    if argv is None:
        argv = sys.argv[1:]
    out = out or sys.stdout
    err = err or sys.stderr
    lenient(out)
    lenient(err)
    if sys.getrecursionlimit() < RECURSION_LIMIT:
        sys.setrecursionlimit(RECURSION_LIMIT)
    parser = build_parser()
    try:
        args = parser.parse_args(argv)
    except SystemExit as x:             # argparse has printed its message
        return x.code if isinstance(x.code, int) else 2
    con = Console(out, err, args.quiet, args.verbose)
    try:
        build = settle_args(args)
        bad = floatlit.self_test()
        if bad:
            # what a host does at start-up (floatlit.py): a build whose float
            # arithmetic differs from the reference writes no class file
            raise Usage('the float arithmetic of this interpreter does not give the bits of '
                        'the reference (setting %s, literal %s: 0x%08X, expected 0x%08X)'
                        % bad[0])
        inst = install.Install(args.root, args.exclude)
        inst.scan()
        for rel in inst.stray_sources:
            con.detail('ignored %s: not in a src directory' % rel)
        provider = None
        if not args.dry_run or args.only_missing:
            # a dry run reads no class file unless it has to know which ones
            # the reader rejects
            provider = load_provider(inst, args, con)
        jobs = select(inst, args, con)
        if args.dry_run:
            return run_dry(args, inst, jobs, con)
        if args.dump:
            return run_dump(args, build, inst, provider, jobs, con)
        if args.compare:
            return run_compare(args, build, inst, provider, jobs, con)
        return run_compile(args, build, inst, provider, jobs, con)
    except Usage as x:
        con.problem('slrrc: %s' % x)
        return 2
    except OSError as x:
        con.problem('slrrc: %s' % x)
        return 2


if __name__ == '__main__':
    sys.exit(main())
