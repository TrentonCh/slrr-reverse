"""slrrc - reference compiler for the script dialect of Street Legal Racing:
Redline (Build 940).  Its output is meant to be byte-identical to the class
files the game's own compiler wrote; notes/compiler_spec.md is the plan.

Modules (the C++ port has the same ones with the same function names):

    scanner     source text -> tokens; literal values
    parser      tokens -> AST (one Node type, token indices kept)
    classes     ClassInfo, ClassProvider, type name resolution, descriptors,
                field lookup, the scored method lookup
    pool        the interning constant pool
    decl        pool steps 1 to 3 (seed, types, names), FILD / MTHD / CLSS rows
    emit        the emitter: AST of a method or field initialiser -> node list
                with symbolic operands
    floatlit    the float literal conversions A to D
    packs       PackNames (the pool spelling of a pack) and the table of the
                shipped pools
    write       the writer: pool steps 4 and 5 over the finished trees
                (resolve, emit), node encoding, chunks, the file
    dump        text forms of tokens, AST, trees and classes (tests,
                comparing implementations)
    diag        diagnostics

    classfile   strict reader of the declarations of a class file
    install     a game tree on disk: its class files, sources and packs
                behind ClassProvider and PackNames
    cli         the command line (python -I slrrc ...): one source or a
                whole tree into a cache directory with a manifest

The compiler is compile_source(): source text in, class file bytes out.
front() and emit_trees() are its stages and are public for the tests: front()
takes a source to the point where every class has its declarations, its rows
and the first three steps of its pool; emit_trees() adds the trees, node for
node, with operands that name pool content; compile_source() adds the last
two steps of the pool and the bytes.

Two things are state of the game's exe that no source holds, and both stay
open until the exe has been read (notes/compiler_spec.md 1.4): the float
literal conversion (Options.float_mode: four conversions occur, A is the
default) and the spelling of pack names (the PackNames a host passes in).

The core (everything above the line in the list) does no file access and
keeps no global state: the source comes in as text, other classes through a
ClassProvider, pack names through a PackNames.  Bad input never raises: the
result carries diagnostics with line, column and code, and ok is False.  The
I/O is in install.py and cli.py, which this module does not import.

WHAT A RESULT HOLDS IS COPIED INTO IT.  While a compilation runs, one
Diagnostics object and one UnitScope collect the diagnostics, the classes
found through the provider (consulted) and the names asked for in vain
(missed); every stage appends to them, the emitter most of all (for 3,449 of
the 3,454 shipped sources it consults classes the declarations did not).  A
result gets its three lists from _finish(), which copies them once, at the end
of the stage that hands the result out.  No result shares a list with the
scope: a port may copy or move the vectors at that one place and nowhere
else.

C++ (script_compiler.hpp, sc_compile.cpp): the three entry points a host
needs are

    Result compile(source, file_name, ClassProvider&, PackNames&, const Options&);
    DeclareResult declare(source, file_name, ClassProvider&, const Options&);
    std::vector<std::string> declared_names(source);

compile() is compile_source().  declare() is declare_source() and
declared_names() is the function of that name: a provider that has to answer
for a class that exists as source only needs both (classes.ClassProvider,
rule 3).

The package imports nothing outside itself and the standard library.
"""
from . import classes
from . import decl
from . import emit
from . import floatlit
from . import packs as pk
from . import parser
from . import scanner
from . import write
from .diag import Diagnostics, E_LEX, E_LIMIT, E_SYNTAX

VERSION = '0.4'                 # part of every cache entry: a change of the
#                                 output of any source needs a new one

BUILD_940 = 0x24F9D


class Options(object):
    """build: the build number written into the file header.
    float_mode: the float literal conversion, 'A' (default), 'B', 'C' or 'D'
    (notes/compiler_spec.md 2.8; which one the game uses is an open question).
    strict: constructs no shipped source uses are errors instead of warnings."""
    __slots__ = ('build', 'float_mode', 'strict')

    def __init__(self, build=BUILD_940, float_mode=floatlit.DEFAULT_MODE, strict=False):
        self.build = build
        self.float_mode = float_mode
        self.strict = strict


class FrontResult(object):
    """name: the file name given.  ok: no error.  diagnostics: [Diagnostic].
    ast: the Ast (its root is None when the source did not parse).  scope:
    the UnitScope (None before the declarations).  classes: one ClassBuild
    per class of the source, in source order.  consulted: full names of the
    classes found through the provider.  missed: full names asked of the
    provider and not found.

    diagnostics, consulted and missed are lists of this result alone, filled
    when the stage that returns it has ended (_finish)."""
    __slots__ = ('name', 'ok', 'diagnostics', 'ast', 'scope', 'classes', 'consulted',
                 'missed')

    def __init__(self, name):
        self.name = name
        self.ok = False
        self.diagnostics = []
        self.ast = None
        self.scope = None
        self.classes = []
        self.consulted = []
        self.missed = []


def _text(source):
    """The source as a str with one character per byte."""
    if isinstance(source, (bytes, bytearray)):
        return bytes(source).decode('latin-1')
    return source


def _bytes_only(source, diags):
    """A source handed over as str stands for bytes: a character above
    U+00FF is none.  (The Python reference only: the C++ compiler is given
    bytes.)  True when the text is usable."""
    if source.isascii():
        return True
    try:
        source.encode('latin-1')
    except UnicodeEncodeError as x:
        at = x.start
        diags.error(E_LEX, source.count('\n', 0, at) + 1, scanner.column_at(source, at),
                    'character U+%04X: a source is a sequence of bytes (pass bytes, or text '
                    'with one character per byte)' % ord(source[at]))
        return False
    return True


def _parse(source, diags, res):
    """Scan and parse; True when the source has a tree with a class in it."""
    if not _bytes_only(source, diags):
        return False
    toks = scanner.scan(source, diags)
    if diags.errors:
        return False
    res.ast = parser.parse(source, toks, diags)
    if diags.errors:
        return False
    if not classes.class_nodes(res.ast):
        diags.error(E_SYNTAX, toks[len(toks) - 1].line, 1, 'the source declares no class')
        return False
    return True


def _too_deep(diags):
    """The interpreter ran out of stack in a pass behind the parser.  A
    safety net of the Python reference only: the height of the tree is
    bounded by parser.MAX_DEPTH, which costs a pass at most two interpreter
    frames a level, so this is reached only when the caller is deep in the
    stack itself or the recursion limit is low (parser.MAX_DEPTH says what a
    caller has to set).  The place is not known here: line 1, column 1."""
    diags.error(E_LIMIT, 1, 1, 'statements or expressions nested too deep for the stack the '
                               'interpreter has left')


def _front(source, provider, diags, res):
    """The work of front(): fills res.ast, res.scope and res.classes; True
    when there was no error.  Appends to diags; _finish() copies."""
    builds = []
    try:
        source = _text(source)
        if not _parse(source, diags, res):
            return False
        scope = classes.declare_unit(res.ast, provider, diags)
        res.scope = scope
        if diags.errors:
            return False
        first = True
        for node, info in zip(classes.class_nodes(res.ast), scope.own):
            builds.append(decl.declare_class(scope, node, info, first))
            first = False
    except RecursionError:
        _too_deep(diags)
    if diags.errors:                    # also: a type inside a body, in strict mode
        return False
    res.classes = builds
    return True


def _emit(res, diags, packs, options):
    """The work of emit_trees() behind the front half: every ClassBuild of
    res gets its trees; True when there was no error."""
    names = pk.CachedPackNames(packs)   # one question per pack and compilation
    try:
        for build in res.classes:
            build.trees = emit.emit_class(res.scope, build, names, options.float_mode)
    except RecursionError:
        _too_deep(diags)
    if diags.errors:
        # no output on error: the trees of a source with an unknown name
        # are not handed out
        res.classes = []
        return False
    return True


def _finish(res, scope, diags):
    """The one place where a result gets its lists: copies of what the
    compilation collected, made when the stage that returns the result has
    ended.  res is a FrontResult, a Result or a DeclareResult; scope is None
    when the source did not get as far as its declarations."""
    res.diagnostics = list(diags.items)
    if scope is not None:
        res.consulted = list(scope.consulted)
        res.missed = list(scope.missed)


def front(source, name, provider, options=None):
    """The front half of a compilation: scan, parse, declare the classes of
    the source, and for each class the pool steps 1 to 3 and its rows.

    source: the text of the file (str with one character per byte, or bytes).
    name: the file name, for messages.  provider: a ClassProvider."""
    if options is None:
        options = Options()
    res = FrontResult(name)
    diags = Diagnostics(options.strict)
    res.ok = _front(source, provider, diags, res)
    _finish(res, res.scope, diags)
    return res


def emit_trees(source, name, provider, packs, options=None):
    """front(), then the emitter: every ClassBuild of the result carries its
    trees (ClassBuild.trees, one emit.Tree per tree owner of its rows, in
    tree order).  The operands of the nodes are symbolic: they name pool
    content, not pool indices.

    packs: a packs.PackNames.  options.float_mode selects the float literal
    conversion; a mode that does not exist is a mistake of the caller and
    raises ValueError."""
    if options is None:
        options = Options()
    if options.float_mode not in floatlit.MODES:
        raise ValueError('float_mode %r: one of A, B, C, D' % (options.float_mode,))
    res = FrontResult(name)
    diags = Diagnostics(options.strict)
    ok = _front(source, provider, diags, res)
    if ok:
        ok = _emit(res, diags, packs, options)
    res.ok = ok
    _finish(res, res.scope, diags)
    return res


class Result(object):
    """What compile_source() returns.

    ok            no error; class_file holds the output
    class_file    the bytes of the class file: the blobs of all classes of
                  the source, in source order (b'' when not ok)
    class_names   the full name of each class, one per blob
    consulted     full names of the classes found through the provider, in
                  the order of their first use: what the output depends on
                  besides the source (a cache records their declarations)
    missed        full names asked of the provider and not found: the output
                  also depends on their absence (a type name is looked for in
                  the package of the source before the imports)
    diagnostics   [Diagnostic]: errors and warnings, in the order found
    ast, classes  the Ast and one ClassBuild per class (pool, rows, trees),
                  for dumps and tests; classes is empty when not ok

    consulted, missed and diagnostics are complete: they hold what every
    stage of the compilation collected, the emitter included (_finish)."""
    __slots__ = ('name', 'ok', 'class_file', 'class_names', 'consulted', 'missed',
                 'diagnostics', 'ast', 'classes')

    def __init__(self, name):
        self.name = name
        self.ok = False
        self.class_file = b''
        self.class_names = []
        self.consulted = []
        self.missed = []
        self.diagnostics = []
        self.ast = None
        self.classes = []


def compile_source(source, name, provider, packs, options=None):
    """Compile one source file to the bytes of its class file.

    source: the text of the file (str with one character per byte, or bytes).
    name: the file name, for messages.  provider: a classes.ClassProvider, the
    classes the compiler can see besides the ones of the source.  packs: a
    packs.PackNames.  options: Options (build number of the header, float
    literal conversion, strict mode).

    Never raises for bad input and writes nothing anywhere: an error leaves
    ok False, no bytes, and the diagnostics.  The one exception is an Options
    value that does not exist (float_mode, build), a mistake of the caller:
    ValueError."""
    if options is None:
        options = Options()
    if not 0 <= options.build <= 0xFFFFFFFF:
        raise ValueError('build %r: a 32-bit number' % (options.build,))
    if options.float_mode not in floatlit.MODES:
        raise ValueError('float_mode %r: one of A, B, C, D' % (options.float_mode,))
    stage = FrontResult(name)
    diags = Diagnostics(options.strict)
    ok = _front(source, provider, diags, stage)
    if ok:
        ok = _emit(stage, diags, packs, options)
    res = Result(name)
    res.ast = stage.ast
    if ok:
        try:
            names = []
            for build in stage.classes:
                write.finish_pool(build)
                names.append(build.info.name)
            data = write.class_file(stage.classes, options.build)
        except RecursionError:          # the writer does not recurse; see _too_deep
            _too_deep(diags)
            ok = False
    if ok:
        res.class_names = names
        res.class_file = data
        res.classes = stage.classes
        res.ok = True
    _finish(res, stage.scope, diags)
    return res


class DeclareResult(object):
    """ok, diagnostics, and infos: one ClassInfo per class of the source;
    consulted and missed as in a Result."""
    __slots__ = ('ok', 'diagnostics', 'infos', 'consulted', 'missed')

    def __init__(self):
        self.ok = False
        self.diagnostics = []
        self.infos = []
        self.consulted = []
        self.missed = []


def declared_names(source):
    """The full names of the classes a source declares, in source order,
    from its package line and its type declarations alone (no class is
    looked up); [] for a source that does not parse.  For a host that has
    to know which source to ask for a class."""
    diags = Diagnostics(False)
    tmp = FrontResult('')
    if not _parse(_text(source), diags, tmp):
        return []
    ast = tmp.ast
    package = ''
    if ast.root.a is not None:
        package = ast.dotted(ast.root.a.tok0, ast.root.a.tok1)
    out = []
    for n in classes.class_nodes(ast):
        simple = ast.text(n.name_tok)
        out.append(package + '.' + simple if package else simple)
    return out


def declare_source(source, name, provider, options=None):
    """What the classes of a source declare, without compiling a body: for a
    provider that has to answer for a class that exists as source only.

    The provider passed in is asked for the superclass and for the types of
    the members, so a provider that calls this from its own find() is
    re-entered; classes.ClassProvider (rule 3) says what it has to answer
    meanwhile."""
    if options is None:
        options = Options()
    res = DeclareResult()
    diags = Diagnostics(options.strict)
    tmp = FrontResult(name)
    scope = None
    try:
        if _parse(_text(source), diags, tmp):
            scope = classes.declare_unit(tmp.ast, provider, diags)
    except RecursionError:
        _too_deep(diags)
    if scope is not None and not diags.errors:
        res.infos = scope.own
        res.ok = True
    _finish(res, scope, diags)
    return res
