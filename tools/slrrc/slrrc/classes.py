"""classes.py - what the compiler knows about classes.

    ClassInfo       what a class declares: name, superclass, interfaces,
                    fields and methods with their descriptors
    ClassProvider   the interface through which the compiler sees the classes
                    of its host (class files, loaded classes, other sources),
                    and the contract a host has to keep
    UnitScope       the classes one compilation sees: the classes of the
                    source itself over the provider, and the type names of
                    the source resolved to full class names
    declare_unit    the classes of a source, from its declarations alone
    find_field      a field by name in a class and its superclasses
    find_method     the method a call binds to at compile time: the scored
                    lookup of the game's VM (the second half of this file)

A class file holds full names (Ljava.util.Vector;); the source writes simple
ones.  resolve_type looks a name up in this order (notes/grammar.md 7.2,
notes/declarations.md 5):

    1  a class declared in the same source
    2  the package of the source
    3  the imports, in source order
    4  java.lang

All 16,345 type names of the shipped sources resolve, none in more than one
place, so the order of the steps is not observable.  A name that resolves
nowhere becomes the descriptor "L;" (observed once in a shipped class file)
with a warning, or an error in strict mode.

Nothing here reads a file: every other class comes from the provider.

NUMBERS.  Indices and counters are unsigned (C++: uint32_t / size_t): no loop
here counts below zero and no index is compared with zero.  "Nothing" is 0
where 0 is no possible value (type_desc_end) and NO_FIT for a score.

C++ (sc_classes.cpp): the same structs; ClassProvider is the abstract class
of script_compiler.hpp.
"""
from . import parser as ps
from . import scanner as sc
from .diag import E_TYPE, E_UNSUPPORTED, W_CLASS

OBJECT = 'java.lang.Object'
INIT = '<init>'                 # the name of every constructor row


class FieldInfo(object):
    __slots__ = ('name', 'desc', 'is_static')

    def __init__(self, name, desc, is_static):
        self.name = name
        self.desc = desc            # I, F, Lpkg.Name;, [I, [F, [Lpkg.Name;
        self.is_static = is_static


class MethodInfo(object):
    __slots__ = ('name', 'desc', 'is_static')

    def __init__(self, name, desc, is_static):
        self.name = name            # "<init>" for a constructor
        self.desc = desc            # (params)ret; a constructor has no return part
        self.is_static = is_static


class ClassInfo(object):
    __slots__ = ('name', 'super', 'interfaces', 'is_interface', 'fields', 'methods')

    def __init__(self, name):
        self.name = name            # full name with dots
        self.super = None           # full name, None for java.lang.Object and interfaces
        self.interfaces = []        # full names, in implements order
        self.is_interface = False
        self.fields = []            # FieldInfo, source order
        self.methods = []           # MethodInfo, source order


def declaration_text(info):
    """Everything the compiler can read from a ClassInfo, as one text: the
    canonical form a cache hashes to tell whether a class another class was
    compiled against still declares the same (notes/compiler_spec.md 2.10).
    One line per item, each closed by LF:

        class <name>
        super <name>            ("super -" without a superclass)
        interface <0 | 1>
        implements <name>       per interface, in order
        field s <name> <descriptor>             per static field, in order
        field i <name> <descriptor>             per instance field, in order
        method <s | i> <name> <descriptor>      per method, in order

    s marks a static member.  The fields stand as a class file holds them,
    the static ones first: a field is looked up by its name, so the order
    between the two kinds means nothing, and the text of a class comes out
    the same whether it was declared from its source or read from the class
    file compiled from it (2,858 of the 2,858 classes of the consistent
    pairs).  The order of the methods is part of the text: the method lookup
    scans in an order derived from it.

    A constructor is written without a return part, "<init> (I)", as the
    pool of a class file holds it, also when the ClassInfo says "(I)V" (see
    ClassProvider): the compiler never reads the return part of a
    constructor, so the text must not depend on it."""
    out = ['class ' + info.name,
           'super ' + (info.super if info.super is not None else '-'),
           'interface ' + ('1' if info.is_interface else '0')]
    for i in info.interfaces:
        out.append('implements ' + i)
    for f in info.fields:
        if f.is_static:
            out.append('field s ' + f.name + ' ' + f.desc)
    for f in info.fields:
        if not f.is_static:
            out.append('field i ' + f.name + ' ' + f.desc)
    for m in info.methods:
        desc = m.desc
        if m.name == INIT:
            desc = params_part(desc)
        out.append('method ' + ('s' if m.is_static else 'i') + ' ' + m.name + ' ' + desc)
    return '\n'.join(out) + '\n'


def params_part(desc):
    """A method descriptor without its return part: '(ILp.T;)V' -> '(ILp.T;)'."""
    n = len(desc)
    i = 0
    while i < n and desc[i] != ')':
        i += 1
    return desc[:i + 1]


class ClassProvider(object):
    """The classes the compiler can see besides the ones of its source.

    THE CONTRACT.  A compiled class embeds what other classes declared at
    compile time, so everything below reaches the bytes or the cache; the
    numbers say how many of the 3,454 shipped sources compile to other bytes
    when a provider leaves the item out.

    1  THE NAME.  find(fqn) is asked with a full name with dots and answers
       the class of exactly that name, upper and lower case included, or
       None.  The ClassInfo carries that name.  (A provider that looks a
       class up by file name on a file system without case would answer
       "p.base" with the class p.Base, and "p.base" would be written into the
       class file.)  Checked by the compiler: W_CLASS, treated as not found.

    2  WHAT A ClassInfo HOLDS.
         super          the full name of the superclass; None for
                        java.lang.Object and for an interface
         interfaces     the full names behind implements, in order, and
         is_interface   the flag.  The method lookup counts an interface that
                        the class of an argument implements; without the list
                        a call that passes an object for an interface
                        parameter loses its method ref (3 sources)
         fields         every field: name, descriptor, is_static.  A field
                        ref names the declaring class for a static field and
                        the type of the receiver for an instance field
                        (2,574 sources without the flag).  The order between
                        static and instance fields is free
         methods        every method row: name ("<init>" for a constructor),
                        descriptor, is_static; in source order, which is the
                        order of their tree numbers in a class file, the rows
                        without a tree behind the others.  The lookup scans
                        in an order derived from it, and it is part of
                        declaration_text() and so of every cache entry
       Descriptors are the texts the pool of a class file holds:
         a type         I, F or Lpkg.Name; with one "[" in front per
                        dimension; "L;" is the type that resolved nowhere
         a method       "(" parameter types ")" and V or a type
         a constructor  "(" parameter types ")" and NOTHING: "(I)", "()"
       The one deviation that is taken: a constructor handed over as "(I)V"
       (the engine's loader rewrites it so for its own use).  The compiler
       never reads the return part of a constructor, and declaration_text()
       leaves it out, so neither the bytes nor a cache entry depend on it.
       Anything else has to be the text of the pool: a loader that decodes a
       pool heuristically must not be the source of a ClassInfo.  Build it
       with the strict reader (classfile.class_info), or from the source
       (declare_source).  Checked by the compiler: the grammar of every
       descriptor and non-empty names (check_class_info): W_CLASS, treated
       as not found.

    3  RE-ENTRY.  find() may be entered again before it has returned.  To
       answer for a class that exists as source only, a provider calls
       declare_source() on that source, and declare_source() asks find() for
       the superclass and the types of the members, possibly for a class
       whose source names the first one in turn.  The rule that ends this:
       while a source is being declared, the provider answers every name the
       source declares (declared_names()) with an empty ClassInfo of that
       name, a stub without superclass and members.  install.InstallProvider
       is an implementation.

    4  ONE ANSWER.  The answer for a name does not change while a
       compilation runs: the compiler asks once per name and keeps the
       answer (UnitScope.find).  A negative answer must be cheap: a type name
       is tried in the package of the source, in every import and in
       java.lang, so more than a third of the questions are for classes that
       do not exist (7.2 per shipped source).

    C++ (script_compiler.hpp):
        struct ClassProvider { virtual const ClassInfo* find(std::string_view fqn) = 0; };
    The pointer stays valid until compile() returns.  The header also
    declares declare() and declared_names() (__init__.py: declare_source,
    declared_names): a host provider needs both for rule 3."""

    def find(self, fqn):
        """ClassInfo of the class with exactly this full name, or None.  The
        provider may load or declare the class to answer, and may be
        re-entered while it does (rule 3 of the contract)."""
        return None


# ---------------------------------------------------------------------------
# descriptors
# ---------------------------------------------------------------------------

def type_desc_end(desc, i):
    """A type descriptor stands at offset i of desc:  { "[" } ( "I" | "F" |
    "L" name ";" ),  the name being any characters but ";" (none at all in
    "L;").  Returns the offset behind it, or 0 when none stands there (0 is
    no offset a descriptor can end at)."""
    n = len(desc)
    while i < n and desc[i] == '[':
        i += 1
    if i >= n:
        return 0
    ch = desc[i]
    if ch == 'I' or ch == 'F':
        return i + 1
    if ch == 'L':
        j = i + 1
        while j < n and desc[j] != ';':
            j += 1
        if j >= n:
            return 0
        return j + 1
    return 0


DESC_BAD = 0            # no descriptor
DESC_TYPE = 1           # a type: I, F, Lp.T;, [I, ...
DESC_METHOD = 2         # (params) and V or a type
DESC_CTOR = 3           # (params) and nothing


def desc_kind(desc):
    """What kind of descriptor a text is (DESC_*)."""
    n = len(desc)
    if n == 0:
        return DESC_BAD
    if desc[0] != '(':
        if type_desc_end(desc, 0) == n:
            return DESC_TYPE
        return DESC_BAD
    i = 1
    while i < n and desc[i] != ')':
        i = type_desc_end(desc, i)
        if i == 0:
            return DESC_BAD
    if i >= n:
        return DESC_BAD                 # no ")"
    i += 1
    if i == n:
        return DESC_CTOR
    if i + 1 == n and desc[i] == 'V':
        return DESC_METHOD
    if type_desc_end(desc, i) == n:
        return DESC_METHOD
    return DESC_BAD


def check_class_info(info, fqn):
    """Does a ClassInfo the provider handed over for the name fqn keep the
    contract of ClassProvider, as far as that can be seen?  Returns '' when
    it does, else one line that says what is wrong."""
    if info.name != fqn:
        return 'it carries the name "%s"' % info.name
    if info.super is not None and info.super == '':
        return 'its superclass has an empty name'
    for i in info.interfaces:
        if i == '':
            return 'one of its interfaces has an empty name'
    for f in info.fields:
        if f.name == '':
            return 'a field has an empty name'
        if desc_kind(f.desc) != DESC_TYPE:
            return 'field %s has the descriptor "%s"' % (f.name, f.desc)
    for m in info.methods:
        if m.name == '':
            return 'a method has an empty name'
        k = desc_kind(m.desc)
        if m.name == INIT:
            # with or without a return part: the compiler reads neither
            if k != DESC_CTOR and k != DESC_METHOD:
                return 'a constructor has the descriptor "%s"' % m.desc
        elif k != DESC_METHOD:
            return 'method %s has the descriptor "%s"' % (m.name, m.desc)
    return ''


class UnitScope(ClassProvider):
    """The classes one compilation sees and the type names of its source."""

    def __init__(self, ast, provider, diags):
        self.ast = ast
        self.provider = provider
        self.diags = diags
        self.package = ''           # '' when the source has no package
        self.imports = []           # (name, is_star) of the usable imports, source order
        self.own = []               # ClassInfo of the classes of the source, source order
        self.own_by_simple = {}
        self.own_by_full = {}
        self.own_name_toks = {}     # full name -> the token of the class name
        self.type_names = {}        # every class name the source writes as a type
        self.resolved = {}          # type name as written -> full name or None
        self.reported = {}          # type names already reported as unknown
        self.answers = {}           # full name -> what the provider answered (ClassInfo or
        #                             None), asked once per compilation
        self.consulted = []         # full names found through the provider, first use first
        self.missed = []            # full names asked of the provider and not found
        self.cycles = {}            # own classes already reported as their own superclass
        # caches of the member lookups, per class name
        self.chains = {}            # -> [ClassInfo]: the class and its superclasses
        self.field_maps = {}        # -> {field name: FieldInfo}
        self.method_maps = {}       # -> {method name: [MethodInfo] in scan order}

    def find(self, fqn):
        """The class of the source itself, else what the provider answers.

        The provider is asked ONCE per name and compilation; its answer,
        found or not, is kept (answers).  Both answers are also recorded for
        the caller: a compilation depends on the classes it found (consulted)
        and on the absence of the ones it did not (missed: a type name is
        tried in the own package first).  A class that breaks the contract
        of ClassProvider is reported (W_CLASS) and counts as not found."""
        info = self.own_by_full.get(fqn)
        if info is not None:
            return info
        if fqn in self.answers:
            return self.answers[fqn]
        info = self.provider.find(fqn)
        if info is not None:
            why = check_class_info(info, fqn)
            if why != '':
                self.diags.warning(W_CLASS, 1, 1,
                                   'class %s as the provider declares it cannot be used: %s; '
                                   'it is treated as not found' % (fqn, why))
                info = None
        self.answers[fqn] = info
        if info is not None:
            self.consulted.append(fqn)
        else:
            self.missed.append(fqn)
        return info


def resolve_type(scope, name):
    """Full name of the class a source names, or None when it is unknown."""
    if name in scope.resolved:
        return scope.resolved[name]
    full = _lookup(scope, name)
    scope.resolved[name] = full
    return full


def _lookup(scope, name):
    if '.' in name:
        # written with its package (no shipped source does): taken as it is
        if scope.find(name) is not None:
            return name
        return None
    own = scope.own_by_simple.get(name)
    if own is not None:
        return own.name
    full = scope.package + '.' + name if scope.package else name
    if scope.find(full) is not None:
        return full
    for imp, is_star in scope.imports:
        if is_star:
            full = imp + '.' + name
        elif imp == name or imp.endswith('.' + name):
            full = imp
        else:
            continue
        if scope.find(full) is not None:
            return full
    full = 'java.lang.' + name
    if scope.find(full) is not None:
        return full
    return None


def descriptor(scope, type_node, extra_dims):
    """Descriptor of a TYPE node: I, F, V or Lpkg.Name; with one "[" per
    dimension in front.  extra_dims counts brackets written behind the
    declared name.  An unknown class gives "L;"."""
    ast = scope.ast
    tok = type_node.tok0
    k = ast.toks[tok].kind
    if k == sc.KW_INT:
        base = 'I'
    elif k == sc.KW_FLOAT:
        base = 'F'
    elif k == sc.KW_VOID:
        base = 'V'
    else:
        name = ast.type_name(type_node)
        full = resolve_type(scope, name)
        if full is None:
            if name not in scope.reported:
                scope.reported[name] = True
                t = ast.toks[tok]
                scope.diags.unresolved_type(t.line, sc.column_at(ast.source, t.pos), name)
            base = 'L;'
        else:
            base = 'L' + full + ';'
    return '[' * (type_node.dims() + extra_dims) + base


def method_descriptor(scope, method_node):
    """"(" parameter descriptors ")" return descriptor.  A constructor has
    no return part at all: "(I)", "()"."""
    d = '('
    for p in method_node.items:
        d += descriptor(scope, p.a, p.dims())
    d += ')'
    if method_node.a is not None:
        d += descriptor(scope, method_node.a, 0)
    return d


def _class_name(scope, type_node, what):
    """Full name of a superclass or an interface; an unknown one is an error
    in either mode."""
    ast = scope.ast
    name = ast.type_name(type_node)
    full = resolve_type(scope, name)
    if full is None:
        t = ast.toks[type_node.tok0]
        scope.diags.error(E_TYPE, t.line, sc.column_at(ast.source, t.pos),
                          'unknown %s %s' % (what, name))
        return ''
    return full


def report_cycle(scope, name):
    """The own class `name` is its own superclass, directly or through other
    classes: an error in both modes, reported once, at the name in its
    declaration.  (No shipped class is; the game's loader would not end.)"""
    if name in scope.cycles:
        return
    scope.cycles[name] = True
    ast = scope.ast
    t = ast.toks[scope.own_name_toks[name]]
    scope.diags.error(E_TYPE, t.line, sc.column_at(ast.source, t.pos),
                      'class %s is its own superclass' % name)


def declare_unit(ast, provider, diags):
    """The UnitScope of a parsed source.  scope.own holds one ClassInfo per
    type declaration, in source order, built from the declarations alone (no
    body is looked at).  All classes of the source are known by name before
    the first descriptor is made: a class may name one declared further down
    (147 type names of the shipped sources do)."""
    scope = UnitScope(ast, provider, diags)
    unit = ast.root
    if unit.a is not None:
        scope.package = ast.dotted(unit.a.tok0, unit.a.tok1)
    nodes = []
    for n in unit.items:
        if n.kind == ps.IMPORT:
            # "import parts:part.root.BodyPart.*;" (2 sources) names nothing
            if not (n.flags & ps.F_COLON):
                scope.imports.append((ast.dotted(n.a.tok0, n.a.tok1),
                                      (n.flags & ps.F_STAR) != 0))
            continue
        simple = ast.text(n.name_tok)
        if simple in scope.own_by_simple:
            t = ast.toks[n.name_tok]
            diags.error(E_UNSUPPORTED, t.line, sc.column_at(ast.source, t.pos),
                        'type %s declared twice in one source' % simple)
            continue
        info = ClassInfo(scope.package + '.' + simple if scope.package else simple)
        scope.own.append(info)
        scope.own_by_simple[simple] = info
        scope.own_by_full[info.name] = info
        scope.own_name_toks[info.name] = n.name_tok
        nodes.append(n)
    # every class name the source writes as a type, wherever it stands (the
    # emitter tells a variable of such a name from the class)
    for n in ast.nodes:
        if n.kind == ps.TYPE and not ps.is_primitive(ast, n):
            scope.type_names[ast.type_name(n)] = True
    for n, info in zip(nodes, scope.own):
        info.is_interface = (n.flags & ps.F_INTERFACE) != 0
        if n.a is not None:
            info.super = _class_name(scope, n.a, 'superclass')
        elif not info.is_interface and info.name != OBJECT:
            info.super = OBJECT
        if n.b is not None:
            for t in n.b.items:
                info.interfaces.append(_class_name(scope, t, 'interface'))
        for m in n.items:
            is_static = (m.flags & ps.ACC_STATIC) != 0
            if m.kind == ps.FIELD_DECL:
                for d in m.items:
                    info.fields.append(FieldInfo(ast.text(d.name_tok),
                                                 descriptor(scope, m.a, d.dims()),
                                                 is_static))
            else:
                name = INIT if m.flags & ps.F_CTOR else ast.text(m.name_tok)
                info.methods.append(MethodInfo(name, method_descriptor(scope, m), is_static))
    # The superclasses of every class of the source are walked once, here:
    # a class that is its own superclass, directly or through classes of the
    # source or of the provider, is an error whatever its bodies look up
    # (class_chain reports it).  Not when a superclass is unknown: that is an
    # error already, and its name is empty.
    if not diags.errors:
        for info in scope.own:
            class_chain(scope, info.name)
    return scope


def class_nodes(ast):
    """The CLASS nodes of a parsed source, in source order."""
    return [n for n in ast.root.items if n.kind == ps.CLASS]


# ---------------------------------------------------------------------------
# member lookup (notes/expressions.md 5.3 and 6.3)
# ---------------------------------------------------------------------------
#
# Fields are found by name in the class and then in its superclasses, nearest
# first.  Interfaces are not searched.
#
# Methods are found by the scored lookup of the game's VM (Class::getMethod,
# Build 940 VA 0x54A150, scores at 0x55BD80).  That the compiler calls this
# very function is inferred from the outcome: 16,850 call sites of the oracle
# pairs resolve as it says and 77 fail to resolve as it says.
#
#   - start at the receiver class and go up the superclasses;
#   - in a class, look at every method of that name with as many parameters
#     as the call has arguments; its score is the sum of the type scores of
#     the arguments, and a method with an impossible argument is out;
#   - a method with score 0 is taken at once; otherwise the lowest score seen
#     on the whole way up wins, and of equal scores the one met first stays.
#
# The scan order inside a class is the one of the decompilation: the instance
# methods from the last to the first, then the static methods from the last
# to the first.  No call site of the corpus has two candidates with the same
# best score, so the order is not constrained by any shipped class; the
# lookup says when it met such a tie (MethodHit.tie).

STRING_DESC = 'Ljava.lang.String;'
OBJECT_DESC = 'Ljava.lang.Object;'
NULL_TYPE = 'null'              # the type of the literal null, as an argument
UNKNOWN_DESC = 'L;'             # the descriptor of a type name that resolved nowhere
NO_METHOD = 1000000             # above every score a method can reach
NO_FIT = 0xFFFFFFFF             # type_score, distance: the value cannot be passed


class FieldHit(object):
    """A field found by find_field: the FieldInfo and the full name of the
    class that declares it."""
    __slots__ = ('field', 'owner')

    def __init__(self, field, owner):
        self.field = field
        self.owner = owner


class MethodHit(object):
    """The outcome of lookup_method.  method: the MethodInfo found, or None.
    score: its score.  tie: another method with another descriptor reached
    the same score, so that the scan order decided."""
    __slots__ = ('method', 'score', 'tie')

    def __init__(self):
        self.method = None
        self.score = NO_METHOD
        self.tie = False


def class_chain(scope, name):
    """The class and its superclasses, nearest first, as far as they are
    known ([ClassInfo]).  A class that is its own ancestor ends the chain;
    when a class of the source being compiled is part of such a ring, that
    is an error (report_cycle)."""
    chain = scope.chains.get(name)
    if chain is None:
        chain = []
        seen = {}
        n = name
        while n is not None and n not in seen:
            info = scope.find(n)
            if info is None:
                break
            seen[n] = True
            chain.append(info)
            n = info.super
        if n is not None and n in seen:
            # the ring is the part of the chain from the class named n on
            in_ring = False
            for info in chain:
                if info.name == n:
                    in_ring = True
                if in_ring and info.name in scope.own_by_full:
                    report_cycle(scope, info.name)
        scope.chains[name] = chain
    return chain


def _field_map(scope, info):
    m = scope.field_maps.get(info.name)
    if m is None:
        m = {}
        for f in info.fields:
            if f.name not in m:
                m[f.name] = f
        scope.field_maps[info.name] = m
    return m


def _method_map(scope, info):
    """Method name -> the methods of that name in the order the lookup scans
    them: the instance methods from the last to the first, then the static
    methods from the last to the first."""
    m = scope.method_maps.get(info.name)
    if m is None:
        m = {}
        k = len(info.methods)
        while k > 0:
            k -= 1
            x = info.methods[k]
            if not x.is_static:
                m.setdefault(x.name, []).append(x)
        k = len(info.methods)
        while k > 0:
            k -= 1
            x = info.methods[k]
            if x.is_static:
                m.setdefault(x.name, []).append(x)
        scope.method_maps[info.name] = m
    return m


def find_field(scope, cls, name):
    """The field `name` of class cls (full name) or of a superclass: a
    FieldHit, or None."""
    for info in class_chain(scope, cls):
        f = _field_map(scope, info).get(name)
        if f is not None:
            return FieldHit(f, info.name)
    return None


def split_params(desc):
    """The parameter descriptors of a method descriptor:
    '(ILp.T;[F)V' -> ['I', 'Lp.T;', '[F'].  desc is a descriptor of kind
    DESC_METHOD or DESC_CTOR (what a ClassInfo holds; UnitScope.find checks
    the ones of the provider)."""
    out = []
    n = len(desc)
    i = 1
    while i < n and desc[i] != ')':
        j = type_desc_end(desc, i)
        if j == 0:
            break                       # no descriptor: not reached, see above
        out.append(desc[i:j])
        i = j
    return out


def class_of_desc(desc):
    """The class a value of this type is looked up in: 'p.T' for 'Lp.T;';
    for an array its element type with a '[' in front ('[p.T', '[I'), which
    has the one member length; None for int, float and void."""
    if desc is None or desc == '':
        return None
    n = len(desc)
    if desc[0] == 'L' and desc[n - 1] == ';':
        return desc[1:n - 1]
    if desc[0] == '[':
        if n >= 3 and desc[1] == 'L' and desc[n - 1] == ';':
            return '[' + desc[2:n - 1]
        return '[' + desc[1:]
    return None


def distance(scope, arg_cls, par_cls):
    """Superclass steps from class arg_cls up to par_cls, one more for an
    interface a class on the way implements; NO_FIT when par_cls is not
    reached.  (The distance function of the exe, 0x54ABF0, is not
    decompiled; this reading reproduces every call site of the corpus.)"""
    d = 0
    for info in class_chain(scope, arg_cls):
        if info.name == par_cls:
            return d
        for i in info.interfaces:
            if i == par_cls:
                return d + 1
        d += 1
    return NO_FIT


def type_score(scope, par, arg):
    """Score of passing a value of type arg for a parameter of type par: 0
    for the same type, NO_FIT when it cannot be passed, else a penalty.  arg
    is a descriptor, NULL_TYPE, or None for an argument whose type is not
    known; par is a type descriptor (never empty)."""
    if par == arg:
        return 0
    if arg is None:
        return NO_FIT
    p0 = par[0]
    # null, and a value whose type name resolved nowhere ("L;"), fit every
    # object or array parameter
    if arg == NULL_TYPE or arg == UNKNOWN_DESC:
        a0 = 'N'
    else:
        a0 = arg[0]
    if p0 == 'F':
        if a0 == 'I':
            return 101
        return NO_FIT
    if p0 == 'I':
        if a0 == 'F':
            return 100
        return NO_FIT
    if p0 == 'L' or p0 == '[':
        if a0 == 'I' or a0 == 'F':
            if par == STRING_DESC:
                return 100
            return NO_FIT
        if a0 == 'N':
            return 100
        if a0 == 'L' or a0 == '[':
            if p0 == '[' or a0 == '[':
                # an array is passed for its own type (score 0 above) or
                # for java.lang.Object
                if par == OBJECT_DESC:
                    return 1
                return NO_FIT
            return distance(scope, arg[1:len(arg) - 1], par[1:len(par) - 1])
    return NO_FIT


def lookup_method(scope, cls, name, arg_types):
    """The method a call  name(arguments)  on class cls (full name) binds to
    at compile time, as a MethodHit (method None when there is none).
    arg_types: one entry per argument, see type_score."""
    hit = MethodHit()
    argc = len(arg_types)
    for info in class_chain(scope, cls):
        cands = _method_map(scope, info).get(name)
        if cands is None:
            continue
        for m in cands:
            params = split_params(m.desc)
            if len(params) != argc:
                continue
            score = 0
            fits = True
            k = 0
            while k < argc:
                s = type_score(scope, params[k], arg_types[k])
                if s == NO_FIT:
                    fits = False
                    break
                score += s
                k += 1
            if not fits:
                continue
            if score == 0:
                hit.method = m
                hit.score = 0
                hit.tie = False
                return hit
            if score < hit.score:
                hit.method = m
                hit.score = score
                hit.tie = False
            elif score == hit.score and m.desc != hit.method.desc:
                hit.tie = True
    return hit


def find_method(scope, cls, name, arg_types):
    """The MethodInfo lookup_method finds, or None."""
    return lookup_method(scope, cls, name, arg_types).method


def has_method(scope, cls, name, argc):
    """Does class cls or a superclass declare a method of that name with
    argc parameters?"""
    for info in class_chain(scope, cls):
        cands = _method_map(scope, info).get(name)
        if cands is None:
            continue
        for m in cands:
            if len(split_params(m.desc)) == argc:
                return True
    return False
