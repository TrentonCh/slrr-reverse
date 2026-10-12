"""diag.py - diagnostics of the compiler.

The compiler never raises for bad input.  Every module reports into one
Diagnostics list; an entry carries a line, a column and a code.  A source
that produced an error gives no output (compiler_spec.md 2.9).

Codes
    E_LEX          character no rule accepts, unterminated string or comment
    E_SYNTAX       token sequence outside the grammar
    E_UNSUPPORTED  construct no shipped source uses and that has no stated
                   default (or any such construct in strict mode)
    E_TYPE         type name that resolves nowhere (strict mode; always for a
                   superclass or an interface); a class that is its own
                   superclass
    E_NAME         identifier that is no local, field or class
    E_MEMBER       member not found in a dotted name
    E_LIMIT        a limit of the class format or of the compiler
    W_UNVERIFIED   construct no shipped source uses, compiled by the plain
                   extension of the proven rules (appendix B)
    W_TYPE         type name that resolves nowhere, compiled to "L;"
    W_BYTE         byte above 0x7F in a string or comment, byte order mark
    W_PACK         resource id whose pack has no file, compiled to
                   "system.rpk" id 0 (a warning in both modes: one shipped
                   class shows this form, so strict mode must compile it)
    W_CLASS        a class the provider handed over that breaks the contract
                   of classes.ClassProvider (another name than the one asked
                   for, a descriptor that is none); the class is treated as
                   not found.  It has no place in the source: line 1, column 1

C++: struct Diagnostic { bool error; uint32_t line, column; const char* code;
std::string text; } and a std::vector of them with the two counters.
"""

E_LEX = 'E_LEX'
E_SYNTAX = 'E_SYNTAX'
E_UNSUPPORTED = 'E_UNSUPPORTED'
E_TYPE = 'E_TYPE'
E_NAME = 'E_NAME'
E_MEMBER = 'E_MEMBER'
E_LIMIT = 'E_LIMIT'
W_UNVERIFIED = 'W_UNVERIFIED'
W_TYPE = 'W_TYPE'
W_BYTE = 'W_BYTE'
W_PACK = 'W_PACK'
W_CLASS = 'W_CLASS'


class Diagnostic(object):
    __slots__ = ('error', 'line', 'column', 'code', 'text')

    def __init__(self, error, line, column, code, text):
        self.error = error          # True: no output is produced
        self.line = line            # 1-based, lines are counted by LF
        self.column = column        # 1-based, a TAB counts as one
        self.code = code
        self.text = text

    def format(self, file_name):
        return '%s:%d:%d: %s %s: %s' % (file_name, self.line, self.column,
                                        'error' if self.error else 'warning',
                                        self.code, self.text)

    def exe_wording(self, file_name):
        """The line the game's own compiler logs for a source that does not
        parse (docs/ENGINE.md 4.2); the host prints it for E_SYNTAX."""
        return 'JVM::compileSource: Syntax error in "%s" at line %d' % (file_name, self.line)


class Diagnostics(object):
    """The diagnostics of one compilation, in the order they were found.

    strict selects what a construct of appendix B does: an error
    (E_UNSUPPORTED) in strict mode, a warning (W_UNVERIFIED) otherwise.

    One Diagnostics object lives as long as one compilation and every stage
    appends to it.  A result hands out a COPY of items, made once at the end
    of the stage that returns it (__init__._finish); nothing outside keeps a
    reference to the list that grows."""
    __slots__ = ('items', 'errors', 'strict')

    def __init__(self, strict=False):
        self.items = []
        self.errors = 0
        self.strict = strict

    def error(self, code, line, column, text):
        self.items.append(Diagnostic(True, line, column, code, text))
        self.errors += 1

    def warning(self, code, line, column, text):
        self.items.append(Diagnostic(False, line, column, code, text))

    def unverified(self, line, column, text):
        """A construct no shipped source uses but whose compilation is the
        plain extension of the proven rules.  Returns True when it was
        reported as an error (strict mode)."""
        if self.strict:
            self.error(E_UNSUPPORTED, line, column, text + ' (no shipped source uses it)')
            return True
        self.warning(W_UNVERIFIED, line, column, text + ' (no shipped source uses it)')
        return False

    def unresolved_type(self, line, column, name):
        """A type name that resolves nowhere.  Returns True when it was
        reported as an error (strict mode)."""
        if self.strict:
            self.error(E_TYPE, line, column, 'unknown type %s' % name)
            return True
        self.warning(W_TYPE, line, column, 'unknown type %s, compiled to "L;"' % name)
        return False
