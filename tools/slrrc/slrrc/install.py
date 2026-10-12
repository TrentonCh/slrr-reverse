"""install.py - a game tree on disk, as the compiler's host.

The compiler core reads no file.  What it needs from its surroundings comes
through two interfaces, and this module implements both for a directory tree
laid out like an install of the game:

    Install             the tree: its class files, its sources, its packs
    InstallProvider     classes.ClassProvider: what the classes of the tree
                        declare
    install_packs()     packs.PackNames: the table of the shipped pools over
                        the pack files of the tree

LAYOUT OF A TREE.  A class file is X/Name.class; its source, when there is
one, is X/src/Name.java (the directory is spelled src, Src or SRC).  Packs are
*.rpk files anywhere.  A .java file that does not sit in such a directory has
no place for its class file and is ignored (Install.stray_sources).

WHICH CLASS ANSWERS FOR A NAME.  The compiler asks for a class by its full
name (java.game.parts.Part); where the game's own compiler finds such a class
is not known for the classes outside its class path (notes/compiler_spec.md
1.4 item 4).  The provider therefore indexes every class file of the tree
by the name the class carries inside the file:

  - a class file wins over a source: the classes of a source are known from
    the source only when no class file defines them (so a compilation sees
    its neighbours as they were compiled, not as their sources read today);
  - a name that several class files define keeps the first file in index
    order (directory, then file name, without regard to case), except that a
    file named like the class wins over a class that sits inside a file of
    another name;
  - a class that only a source declares is declared from that source on first
    use, without compiling a body; of several sources the first in index
    order.  While that goes on the name answers with an empty class, so that
    two sources that name each other cannot recurse (rule 3 of the contract
    of classes.ClassProvider);
  - a class file the reader does not accept (damaged, empty, not a class
    file) defines nothing: the source beside it is treated as a source
    without a class file, so its classes are still known.  The file is
    reported (InstallProvider.unreadable) and left alone.

This is the rule the oracle tests use (tests/harness.py: CorpusProvider);
with it 2,649 of the 2,651 comparable shipped classes are reproduced byte for
byte.  Class names are not unique in an install (21 maps declare
java.game.track_data), so a result can depend on this rule; no shipped
source does.

Reading is all this module does to a tree: it never writes, creates,
renames or deletes anything in it.
"""
import os

from . import classes
from . import classfile
from . import packs
from . import Options, declare_source, declared_names

SOURCE_DIR = 'src'              # compared without regard to case
SOURCE_EXT = '.java'
CLASS_EXT = '.class'
PACK_EXT = '.rpk'


def lower(text):
    """ASCII lower case: paths are compared without regard to case, and the
    result must not depend on a locale."""
    return packs.lower(text)


def index_key(class_dir, name):
    """The order of the index: directory, then name, both lower case."""
    return (lower(class_dir), lower(name))


class SourceFile(object):
    """One source of a tree."""
    __slots__ = ('path', 'rel', 'class_dir', 'name', 'class_rel', 'class_unreadable',
                 'external', 'declared')

    def __init__(self, path, rel, class_dir, name):
        self.path = path            # absolute path of the file
        self.rel = rel              # relative to the root, with "/"
        self.class_dir = class_dir  # where its class file belongs: relative, "/"; '' = the root
        self.name = name            # file name without ".java"
        self.class_rel = None       # the class file beside it, relative; None when there is none
        self.class_unreadable = False   # ... and the reader does not accept that file
        #                             (known once InstallProvider.load() has run)
        self.external = False       # a file outside the tree, named on the command line
        self.declared = None        # full names of its classes, once they were asked for

    def lacks_class(self):
        """Has this source no usable class file beside it: none at all, or
        one the reader rejects?  (What --only-missing selects.)"""
        return self.class_rel is None or self.class_unreadable

    def output_rel(self):
        """Where the class file of this source belongs, relative to the root
        of the tree or of a cache directory."""
        if self.class_dir:
            return self.class_dir + '/' + self.name + CLASS_EXT
        return self.name + CLASS_EXT


class Install(object):
    """The files of a game tree that matter to the compiler."""

    def __init__(self, root, exclude=None):
        self.root = os.path.abspath(root)
        self.exclude = [lower(x) for x in (exclude or [])]
        self.class_files = []       # relative paths with "/", in index order
        self.sources = []           # SourceFile, in index order
        self.pack_files = []        # relative paths with "\", as the directory lists them
        self.stray_sources = []     # .java files outside a src directory (ignored)

    def skipped(self, dir_name):
        low = lower(dir_name)
        for x in self.exclude:
            if low.startswith(x):
                return True
        return False

    def scan(self):
        """Walk the tree once and fill the lists."""
        found_classes = []          # (key, rel)
        found_sources = []          # (key, SourceFile)
        by_key = {}                 # index key -> class file
        for dirpath, dirnames, filenames in os.walk(self.root):
            dirnames[:] = sorted(d for d in dirnames if not self.skipped(d))
            rel_dir = os.path.relpath(dirpath, self.root).replace('\\', '/')
            if rel_dir == '.':
                rel_dir = ''
            for f in sorted(filenames):
                low = lower(f)
                rel = rel_dir + '/' + f if rel_dir else f
                if low.endswith(CLASS_EXT):
                    key = index_key(rel_dir, f[:-len(CLASS_EXT)])
                    found_classes.append((key, rel))
                    if key not in by_key:
                        by_key[key] = rel
                elif low.endswith(PACK_EXT):
                    self.pack_files.append(rel.replace('/', '\\'))
                elif low.endswith(SOURCE_EXT):
                    head, _, last = rel_dir.rpartition('/')
                    if lower(last) != SOURCE_DIR:
                        self.stray_sources.append(rel)
                        continue
                    name = f[:-len(SOURCE_EXT)]
                    sf = SourceFile(os.path.join(dirpath, f), rel, head, name)
                    found_sources.append((index_key(head, name), sf))
        found_classes.sort(key=_first)
        found_sources.sort(key=_first)
        self.class_files = [x[1] for x in found_classes]
        for key, sf in found_sources:
            sf.class_rel = by_key.get(key)
            self.sources.append(sf)

    def path(self, rel):
        return os.path.join(self.root, rel.replace('/', os.sep))

    def read(self, rel):
        with open(self.path(rel), 'rb') as f:
            return f.read()

    def external_source(self, path):
        """A SourceFile for a file that is not a source of the tree: its
        class file has no directory of its own."""
        path = os.path.abspath(path)
        name = os.path.basename(path)
        if lower(name).endswith(SOURCE_EXT):
            name = name[:-len(SOURCE_EXT)]
        sf = SourceFile(path, path.replace('\\', '/'), '', name)
        sf.external = True
        return sf

    def find_source(self, path):
        """The SourceFile of the tree for a path (absolute, or relative to
        the current directory or to the root), None when it is not one."""
        cands = [os.path.abspath(path), os.path.abspath(os.path.join(self.root, path))]
        want = [os.path.normcase(os.path.normpath(c)) for c in cands]
        for sf in self.sources:
            if os.path.normcase(os.path.normpath(sf.path)) in want:
                return sf
        return None


def _first(x):
    return x[0]


def read_source(sf):
    with open(sf.path, 'rb') as f:
        return f.read()


class InstallProvider(classes.ClassProvider):
    """What the classes of a tree declare (the rule is in the module text).

    load() reads every class file once (about 3 s for the 2,921 files of an
    install) and the type declarations of the sources without a class file
    the reader accepts."""

    def __init__(self, install, strict=False):
        self.install = install
        self.strict = strict
        self.classes = {}           # full name -> ClassInfo
        self.origin = {}            # full name -> the file it was taken from (relative)
        self.builds = {}            # class file -> the build number of its header
        self.unreadable = []        # (class file, what does not fit)
        self.unreadable_sources = []    # (source, why it could not be read)
        self.pending = {}           # full name -> SourceFile that declares it
        self.in_progress = {}       # full name -> stub, while its source is declared
        self.failed = []            # (source, [Diagnostic]) of sources that did not declare
        self.class_count = 0        # classes read from class files, duplicates included

    def load(self):
        inst = self.install
        rejected = {}               # class files that define less than they should
        for rel in inst.class_files:
            try:
                res = classfile.read(inst.read(rel))
            except OSError as x:
                self.unreadable.append((rel, 'cannot be read: %s' % x))
                rejected[rel] = True
                continue
            if not res.ok:
                self.unreadable.append((rel, res.error))
                rejected[rel] = True
                continue
            self.builds[rel] = res.blobs[0].build
            file_name = rel.rpartition('/')[2][:-len(CLASS_EXT)]
            for blob in res.blobs:
                info = classfile.class_info(blob)
                if info is None:
                    self.unreadable.append((rel, 'a row or the CLSS record names a pool '
                                                 'entry of the wrong kind'))
                    rejected[rel] = True
                    continue
                self.class_count += 1
                old = self.origin.get(info.name)
                if old is not None:
                    simple = info.name.rpartition('.')[2]
                    old_name = old.rpartition('/')[2][:-len(CLASS_EXT)]
                    if not (file_name == simple and old != rel and old_name != simple):
                        continue
                self.classes[info.name] = info
                self.origin[info.name] = rel
        for sf in inst.sources:
            if sf.class_rel is not None:
                if sf.class_rel not in rejected:
                    continue
                # the class file beside the source defines nothing (or not
                # everything): the classes it lacks are known from the source
                sf.class_unreadable = True
            try:
                sf.declared = declared_names(read_source(sf))
            except OSError as x:
                sf.declared = []
                self.unreadable_sources.append((sf.rel, str(x)))
            for full in sf.declared:
                if full not in self.classes and full not in self.pending:
                    self.pending[full] = sf

    def find(self, fqn):
        """The contract of classes.ClassProvider: the class of exactly this
        name; while a source is being declared, a stub for every name it
        declares (find() is re-entered through declare_source())."""
        info = self.classes.get(fqn)
        if info is not None:
            return info
        info = self.in_progress.get(fqn)
        if info is not None:
            return info
        sf = self.pending.get(fqn)
        if sf is None:
            return None
        mine = [n for n in sf.declared if self.pending.get(n) is sf]
        for n in mine:
            del self.pending[n]
        try:
            data = read_source(sf)
        except OSError as x:
            self.unreadable_sources.append((sf.rel, str(x)))
            return None
        for n in mine:
            self.in_progress[n] = classes.ClassInfo(n)
        res = declare_source(data, sf.rel, self, Options(strict=self.strict))
        for n in mine:
            del self.in_progress[n]
        if not res.ok:
            self.failed.append((sf.rel, res.diagnostics))
            return None
        for ci in res.infos:
            if ci.name in mine:
                self.classes[ci.name] = ci
                self.origin[ci.name] = sf.rel
        return self.classes.get(fqn)


def install_packs(install):
    """The PackNames of a tree: the one spelling each pack has in the shipped
    pools (packs.TABLE_940), over the pack files the tree has.  A pack
    without a file has no name; a pack with a file that the table does not
    know is spelled as the directory lists it.  Where the game itself takes
    the spelling from is an open question (notes/compiler_spec.md 1.4)."""
    return packs.TablePackNames(packs.TABLE_940, install.pack_files)
