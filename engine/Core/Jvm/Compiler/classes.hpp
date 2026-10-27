// classes.hpp - what the compiler knows about classes.  (Reference:
// slrrc/classes.py.)
//
//     ClassInfo       what a class declares: name, superclass, interfaces,
//                     fields and methods with their descriptors
//     ClassProvider   the interface through which the compiler sees the classes
//                     of its host (class files, loaded classes, other sources),
//                     and the contract a host has to keep
//     UnitScope       the classes one compilation sees: the classes of the
//                     source itself over the provider, and the type names of
//                     the source resolved to full class names
//     declare_unit    the classes of a source, from its declarations alone
//     find_field      a field by name in a class and its superclasses
//     find_method     the method a call binds to at compile time: the scored
//                     lookup of the game's VM (the second half of this file)
//
// A class file holds full names (Ljava.util.Vector;); the source writes simple
// ones.  resolve_type looks a name up in this order (notes/grammar.md 7.2,
// notes/declarations.md 5):
//
//     1  a class declared in the same source
//     2  the package of the source
//     3  the imports, in source order
//     4  java.lang
//
// All 16,345 type names of the shipped sources resolve, none in more than one
// place, so the order of the steps is not observable.  A name that resolves
// nowhere becomes the descriptor "L;" (observed once in a shipped class file)
// with a warning, or an error in strict mode.
//
// Nothing here reads a file: every other class comes from the provider.
//
// NUMBERS.  Indices and counters are unsigned: no loop here counts below zero
// and no index is compared with zero.  "Nothing" is 0 where 0 is no possible
// value (type_desc_end) and NO_FIT for a score.
//
// THIS HEADER DOES NOT INCLUDE parser.hpp: ClassInfo and ClassProvider are
// part of the public interface of the compiler (slrrc.hpp), and parser.hpp
// cannot stand behind <windows.h> (its node kind THIS is a macro there).  The
// tree is named here by declaration only; classes.cpp includes the parser.
#ifndef SLRRC_CLASSES_HPP
#define SLRRC_CLASSES_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "diag.hpp"

namespace slrrc {

namespace parser {
struct Ast;
struct Node;
}  // namespace parser

namespace classes {

inline constexpr char OBJECT[] = "java.lang.Object";
inline constexpr char INIT[] = "<init>";            // the name of every constructor row

struct FieldInfo {
    std::string name;
    std::string desc;           // I, F, Lpkg.Name;, [I, [F, [Lpkg.Name;
    bool is_static = false;
};

struct MethodInfo {
    std::string name;           // "<init>" for a constructor
    std::string desc;           // (params)ret; a constructor has no return part
    bool is_static = false;
};

struct ClassInfo {
    ClassInfo() = default;
    explicit ClassInfo(std::string_view full_name) : name(full_name) {}

    std::string name;                       // full name with dots
    // The superclass.  has_super false is the reference's `super is None`:
    // java.lang.Object and interfaces.  (`super` is then not looked at.  An
    // empty name with has_super true is a name, and a broken one: the
    // superclass a source names and nobody knows.)
    bool has_super = false;
    std::string super;                      // full name
    std::vector<std::string> interfaces;    // full names, in implements order
    bool is_interface = false;
    std::vector<FieldInfo> fields;          // source order
    std::vector<MethodInfo> methods;        // source order
};

// Everything the compiler can read from a ClassInfo, as one text: the
// canonical form a cache hashes to tell whether a class another class was
// compiled against still declares the same (notes/compiler_spec.md 2.10).
// One line per item, each closed by LF:
//
//     class <name>
//     super <name>            ("super -" without a superclass)
//     interface <0 | 1>
//     implements <name>       per interface, in order
//     field s <name> <descriptor>             per static field, in order
//     field i <name> <descriptor>             per instance field, in order
//     method <s | i> <name> <descriptor>      per method, in order
//
// s marks a static member.  The fields stand as a class file holds them, the
// static ones first: a field is looked up by its name, so the order between
// the two kinds means nothing, and the text of a class comes out the same
// whether it was declared from its source or read from the class file
// compiled from it (2,858 of the 2,858 classes of the consistent pairs).  The
// order of the methods is part of the text: the method lookup scans in an
// order derived from it.
//
// A constructor is written without a return part, "<init> (I)", as the pool
// of a class file holds it, also when the ClassInfo says "(I)V" (see
// ClassProvider): the compiler never reads the return part of a constructor,
// so the text must not depend on it.
std::string declaration_text(const ClassInfo& info);

// A method descriptor without its return part: "(ILp.T;)V" -> "(ILp.T;)".
std::string_view params_part(std::string_view desc);

// The classes the compiler can see besides the ones of its source.
//
// THE CONTRACT.  A compiled class embeds what other classes declared at
// compile time, so everything below reaches the bytes or the cache; the
// numbers say how many of the 3,454 shipped sources compile to other bytes
// when a provider leaves the item out.
//
// 1  THE NAME.  find(fqn) is asked with a full name with dots and answers the
//    class of exactly that name, upper and lower case included, or nullptr.
//    The ClassInfo carries that name.  (A provider that looks a class up by
//    file name on a file system without case would answer "p.base" with the
//    class p.Base, and "p.base" would be written into the class file.)
//    Checked by the compiler: W_CLASS, treated as not found.
//
// 2  WHAT A ClassInfo HOLDS.
//      super          the full name of the superclass; none (has_super false)
//                     for java.lang.Object and for an interface
//      interfaces     the full names behind implements, in order, and
//      is_interface   the flag.  The method lookup counts an interface that
//                     the class of an argument implements; without the list a
//                     call that passes an object for an interface parameter
//                     loses its method ref (3 sources)
//      fields         every field: name, descriptor, is_static.  A field ref
//                     names the declaring class for a static field and the
//                     type of the receiver for an instance field (2,574
//                     sources without the flag).  The order between static
//                     and instance fields is free
//      methods        every method row: name ("<init>" for a constructor),
//                     descriptor, is_static; in source order, which is the
//                     order of their tree numbers in a class file, the rows
//                     without a tree behind the others.  The lookup scans in
//                     an order derived from it, and it is part of
//                     declaration_text() and so of every cache entry
//    Descriptors are the texts the pool of a class file holds:
//      a type         I, F or Lpkg.Name; with one "[" in front per dimension;
//                     "L;" is the type that resolved nowhere
//      a method       "(" parameter types ")" and V or a type
//      a constructor  "(" parameter types ")" and NOTHING: "(I)", "()"
//    The one deviation that is taken: a constructor handed over as "(I)V"
//    (the engine's loader rewrites it so for its own use).  The compiler never
//    reads the return part of a constructor, and declaration_text() leaves it
//    out, so neither the bytes nor a cache entry depend on it.  Anything else
//    has to be the text of the pool: a loader that decodes a pool
//    heuristically must not be the source of a ClassInfo.  Build it with the
//    strict reader (classfile::class_info), or from the source (declare()).
//    Checked by the compiler: the grammar of every descriptor and non-empty
//    names (check_class_info): W_CLASS, treated as not found.
//
// 3  RE-ENTRY.  find() may be entered again before it has returned.  To
//    answer for a class that exists as source only, a provider calls declare()
//    on that source, and declare() asks find() for the superclass and the
//    types of the members, possibly for a class whose source names the first
//    one in turn.  The rule that ends this: while a source is being declared,
//    the provider answers every name the source declares (declared_names())
//    with an empty ClassInfo of that name, a stub without superclass and
//    members.  The provider of the command line tool (tool/install.cpp:
//    InstallProvider) is an implementation.
//
// 4  ONE ANSWER.  The answer for a name does not change while a compilation
//    runs: the compiler asks once per name and keeps the answer
//    (UnitScope::find).  A negative answer must be cheap: a type name is tried
//    in the package of the source, in every import and in java.lang, so more
//    than a third of the questions are for classes that do not exist (7.2 per
//    shipped source).
//
// THE POINTER find() returns stays valid, and the ClassInfo behind it
// unchanged, until the call of the compiler that asked (compile(), declare())
// has returned.
struct ClassProvider {
    // ClassInfo of the class with exactly this full name, or nullptr.  The
    // provider may load or declare the class to answer, and may be re-entered
    // while it does (rule 3 of the contract).
    virtual const ClassInfo* find(std::string_view fqn) = 0;
    virtual ~ClassProvider() = default;
};

// ---------------------------------------------------------------------------
// descriptors
// ---------------------------------------------------------------------------

// A type descriptor stands at offset i of desc:  { "[" } ( "I" | "F" | "L"
// name ";" ),  the name being any characters but ";" (none at all in "L;").
// Returns the offset behind it, or 0 when none stands there (0 is no offset a
// descriptor can end at).
size_t type_desc_end(std::string_view desc, size_t i);

inline constexpr uint32_t DESC_BAD = 0;             // no descriptor
inline constexpr uint32_t DESC_TYPE = 1;            // a type: I, F, Lp.T;, [I, ...
inline constexpr uint32_t DESC_METHOD = 2;          // (params) and V or a type
inline constexpr uint32_t DESC_CTOR = 3;            // (params) and nothing

// What kind of descriptor a text is (DESC_*).
uint32_t desc_kind(std::string_view desc);

// Does a ClassInfo the provider handed over for the name fqn keep the
// contract of ClassProvider, as far as that can be seen?  Returns "" when it
// does, else one line that says what is wrong.
std::string check_class_info(const ClassInfo& info, std::string_view fqn);

// ---------------------------------------------------------------------------
// the scope of one compilation
// ---------------------------------------------------------------------------

// A text that is the key of a map and is looked up by a view of it.
template <class T>
using NameMap = std::map<std::string, T, std::less<>>;
typedef std::set<std::string, std::less<>> NameSet;

struct Import {
    std::string name;
    bool is_star;
};

// What resolve_type found for a type name: found false is the reference's
// None.
struct Resolved {
    bool found;
    std::string full;
};

// The classes one compilation sees and the type names of its source.
//
// It refers to the tree, the provider and the diagnostics of the compilation
// and has to die before them; it is neither copied nor moved (the caches hold
// pointers into `own`).
class UnitScope : public ClassProvider {
public:
    UnitScope(const parser::Ast& unit_ast, ClassProvider& class_provider, Diagnostics& diagnostics);
    UnitScope(const UnitScope&) = delete;
    UnitScope& operator=(const UnitScope&) = delete;

    const parser::Ast& ast;
    ClassProvider& provider;
    Diagnostics& diags;
    std::string package;                    // "" when the source has no package
    std::vector<Import> imports;            // the usable imports, source order
    std::vector<ClassInfo> own;             // the classes of the source, source order
    NameMap<uint32_t> own_by_simple;        // simple name -> index in own
    NameMap<uint32_t> own_by_full;          // full name -> index in own
    NameMap<uint32_t> own_name_toks;        // full name -> the token of the class name
    NameSet type_names;                     // every class name the source writes as a type
    NameMap<Resolved> resolved;             // type name as written -> full name or nothing
    NameSet reported;                       // type names already reported as unknown
    NameMap<const ClassInfo*> answers;      // full name -> what the provider answered (nullptr:
    //                                         not found), asked once per compilation
    std::vector<std::string> consulted;     // full names found through the provider, first use first
    std::vector<std::string> missed;        // full names asked of the provider and not found
    NameSet cycles;                         // own classes already reported as their own superclass
    // caches of the member lookups, per class name
    NameMap<std::vector<const ClassInfo*>> chains;                  // the class and its superclasses
    NameMap<NameMap<const FieldInfo*>> field_maps;                  // field name -> the field
    NameMap<NameMap<std::vector<const MethodInfo*>>> method_maps;   // method name -> the methods of
    //                                                                 that name, in scan order

    // The class of the source itself, else what the provider answers.
    //
    // The provider is asked ONCE per name and compilation; its answer, found
    // or not, is kept (answers).  Both answers are also recorded for the
    // caller: a compilation depends on the classes it found (consulted) and
    // on the absence of the ones it did not (missed: a type name is tried in
    // the own package first).  A class that breaks the contract of
    // ClassProvider is reported (W_CLASS) and counts as not found.
    const ClassInfo* find(std::string_view fqn) override;

    // own_by_full.get(fqn): the class of the source with this full name, or
    // nullptr.
    const ClassInfo* own_class(std::string_view fqn) const;
};

// Full name of the class a source names, or nullptr when it is unknown.  (The
// text lives in scope.resolved.)
const std::string* resolve_type(UnitScope& scope, std::string_view name);

// Descriptor of a TYPE node: I, F, V or Lpkg.Name; with one "[" per dimension
// in front.  extra_dims counts brackets written behind the declared name.  An
// unknown class gives "L;".
std::string descriptor(UnitScope& scope, const parser::Node& type_node, uint32_t extra_dims);

// "(" parameter descriptors ")" return descriptor.  A constructor has no
// return part at all: "(I)", "()".
std::string method_descriptor(UnitScope& scope, const parser::Node& method_node);

// The own class `name` is its own superclass, directly or through other
// classes: an error in both modes, reported once, at the name in its
// declaration.  (No shipped class is; the game's loader would not end.)
void report_cycle(UnitScope& scope, std::string_view name);

// The UnitScope of a parsed source.  scope.own holds one ClassInfo per type
// declaration, in source order, built from the declarations alone (no body is
// looked at).  All classes of the source are known by name before the first
// descriptor is made: a class may name one declared further down (147 type
// names of the shipped sources do).
std::unique_ptr<UnitScope> declare_unit(const parser::Ast& ast, ClassProvider& provider,
                                        Diagnostics& diags);

// The CLASS nodes of a parsed source, in source order.
std::vector<uint32_t> class_nodes(const parser::Ast& ast);

// ---------------------------------------------------------------------------
// member lookup (notes/expressions.md 5.3 and 6.3)
// ---------------------------------------------------------------------------
//
// Fields are found by name in the class and then in its superclasses, nearest
// first.  Interfaces are not searched.
//
// Methods are found by the scored lookup of the game's VM (Class::getMethod,
// Build 940 VA 0x54A150, scores at 0x55BD80).  That the compiler calls this
// very function is inferred from the outcome: 16,850 call sites of the oracle
// pairs resolve as it says and 77 fail to resolve as it says.
//
//   - start at the receiver class and go up the superclasses;
//   - in a class, look at every method of that name with as many parameters
//     as the call has arguments; its score is the sum of the type scores of
//     the arguments, and a method with an impossible argument is out;
//   - a method with score 0 is taken at once; otherwise the lowest score seen
//     on the whole way up wins, and of equal scores the one met first stays.
//
// The scan order inside a class is the one of the decompilation: the instance
// methods from the last to the first, then the static methods from the last
// to the first.  No call site of the corpus has two candidates with the same
// best score, so the order is not constrained by any shipped class; the
// lookup says when it met such a tie (MethodHit::tie).

inline constexpr char STRING_DESC[] = "Ljava.lang.String;";
inline constexpr char OBJECT_DESC[] = "Ljava.lang.Object;";
inline constexpr char NULL_TYPE[] = "null";         // the type of the literal null, as an argument
inline constexpr char UNKNOWN_DESC[] = "L;";        // the descriptor of a type name that resolved nowhere
inline constexpr uint32_t NO_METHOD = 1000000;      // above every score a method can reach
inline constexpr uint32_t NO_FIT = 0xFFFFFFFF;      // type_score, distance: the value cannot be passed

// A field found by find_field: the FieldInfo and the full name of the class
// that declares it.  field nullptr: there is none (the reference's None).
struct FieldHit {
    const FieldInfo* field = nullptr;
    const std::string* owner = nullptr;
};

// The outcome of lookup_method.  method: the MethodInfo found, or nullptr.
// score: its score.  tie: another method with another descriptor reached the
// same score, so that the scan order decided.
struct MethodHit {
    const MethodInfo* method = nullptr;
    uint32_t score = NO_METHOD;
    bool tie = false;
};

// The type of an argument of a call, as the lookup takes it: a descriptor,
// NULL_TYPE, or nothing (known false; the reference's None) for an argument
// whose type is not known.  desc is a view: the text has to outlive the call.
struct ArgType {
    bool known;
    std::string_view desc;
};

// The class and its superclasses, nearest first, as far as they are known.
// A class that is its own ancestor ends the chain; when a class of the source
// being compiled is part of such a ring, that is an error (report_cycle).
const std::vector<const ClassInfo*>& class_chain(UnitScope& scope, std::string_view name);

// The field `name` of class cls (full name) or of a superclass.
FieldHit find_field(UnitScope& scope, std::string_view cls, std::string_view name);

// The parameter descriptors of a method descriptor: "(ILp.T;[F)V" -> "I",
// "Lp.T;", "[F" (views into desc).  desc is a descriptor of kind DESC_METHOD
// or DESC_CTOR (what a ClassInfo holds; UnitScope::find checks the ones of
// the provider).
std::vector<std::string_view> split_params(std::string_view desc);

// The class a value of this type is looked up in: "p.T" for "Lp.T;"; for an
// array its element type with a "[" in front ("[p.T", "[I"), which has the
// one member length.  Returns false (the reference's None) for int, float,
// void and the empty text; cls is then left as it is.  ("L;" answers true and
// the empty name.)
bool class_of_desc(std::string_view desc, std::string& cls);

// Superclass steps from class arg_cls up to par_cls, one more for an
// interface a class on the way implements; NO_FIT when par_cls is not
// reached.  (The distance function of the exe, 0x54ABF0, is not decompiled;
// this reading reproduces every call site of the corpus.)
uint32_t distance(UnitScope& scope, std::string_view arg_cls, std::string_view par_cls);

// Score of passing a value of type arg for a parameter of type par: 0 for the
// same type, NO_FIT when it cannot be passed, else a penalty.  par is a type
// descriptor (never empty).
uint32_t type_score(UnitScope& scope, std::string_view par, const ArgType& arg);

// The method a call  name(arguments)  on class cls (full name) binds to at
// compile time (method nullptr when there is none).  arg_types: one entry per
// argument, see type_score.
MethodHit lookup_method(UnitScope& scope, std::string_view cls, std::string_view name,
                        const std::vector<ArgType>& arg_types);

// The MethodInfo lookup_method finds, or nullptr.
const MethodInfo* find_method(UnitScope& scope, std::string_view cls, std::string_view name,
                              const std::vector<ArgType>& arg_types);

// Does class cls or a superclass declare a method of that name with argc
// parameters?
bool has_method(UnitScope& scope, std::string_view cls, std::string_view name, size_t argc);

}  // namespace classes
}  // namespace slrrc

#endif
