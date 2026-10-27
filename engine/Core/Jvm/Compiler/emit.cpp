// emit.cpp - the emitter: the AST of a method or of a field initialiser -> the
// node list of its tree.  (Reference: slrrc/emit.py; the functions of Emitter
// stand in the order of that file and carry its names.)
//
// WHERE THE C++ DIFFERS IN FORM FROM THE REFERENCE
//
//   - A node of the source tree is its index in ast.nodes.  The tree is not
//     changed while it is emitted, so a `const Node&` into it stays good; the
//     tree that is being built (tree.nodes, tree.operands) grows, and no
//     reference into it is held.
//   - "A text or None": Walk::has_type / has_cls beside type / cls; the
//     receiver class of callee() is (has_cls, cls); an argument type is a
//     classes::ArgType; a field that was not found is a FieldHit without a
//     field.  The empty text is a value of its own: the class of the
//     descriptor "L;" is "", and it is looked up like any other name.
//   - The names of the live locals are views of the source text (the text of
//     the declaring token); `this` is a view of a constant.
//   - A message with a "%s" in it is put together in the function that
//     reports, error() or unverified() with three parts, or in the function
//     that found the fault when that function does not recurse (walk, name,
//     rid_note).
//   - BINARY_KIND, ASSIGN_KIND, OP_NAMES and X_NAMES are switch functions.  A
//     token that is not in BINARY_KIND or ASSIGN_KIND cannot arrive (the
//     parser refuses >>>, <<=, >>= and >>>=); where the reference would end
//     with KeyError, the expression is reported as one the emitter has no
//     rule for.
//   - finish() hands the tree out by moving it; emit_class() fills the vector
//     of trees that the driver moves into the ClassBuild.
//
// STACK.  expr() and stmt() recurse over the tree, which the parser keeps at
// most parser::MAX_DEPTH high: one frame of expr() per level of an expression,
// two (expr and call) through a call, one of stmt() per level of a statement
// (two through for_stmt and switch_stmt).  What would make those frames big is
// kept out of line (SLRRC_NOINLINE): the functions that report, the functions
// that make an operand (three texts), type_carrier() and cast_carrier() (the
// text of a descriptor).  notes/port_3-emitter.md has the measurement.
#include "emit.hpp"

#include <utility>

#include "decl.hpp"
#include "diag.hpp"
#include "parser.hpp"
#include "scanner.hpp"

#if defined(_MSC_VER)
#define SLRRC_NOINLINE __declspec(noinline)
#else
#define SLRRC_NOINLINE __attribute__((noinline))
#endif

namespace slrrc {
namespace emit {

namespace sc = scanner;
namespace ps = parser;
namespace pk = packs;

// ---------------------------------------------------------------------------
// tables
// ---------------------------------------------------------------------------

const char* op_name(uint8_t op) {
    switch (op) {
    case OP_LOCAL: return "local";
    case OP_DECLARE: return "declare";
    case OP_RELEASE: return "release";
    case OP_AND_TEST: return "and_test";
    case OP_OR_TEST: return "or_test";
    case OP_TYPE: return "type";
    case OP_EXPR: return "expr";
    case OP_STMT: return "stmt";
    case OP_STR: return "str";
    case OP_FLOAT: return "float";
    case OP_INT: return "int";
    case OP_NULL: return "null";
    case OP_BOOL: return "bool";
    case OP_RID: return "rid";
    case OP_END: return "end";
    case OP_PATH: return "path";
    case OP_NAME: return "name";
    case OP_JUMP: return "jump";
    case OP_JUMP_FALSE: return "jump_false";
    case OP_JUMP_TRUE: return "jump_true";
    case OP_CASE: return "case";
    case OP_POP: return "pop";
    case OP_CLASS: return "class";
    case OP_METHOD: return "method";
    case OP_FIELD: return "field";
    case OP_GETFIELD: return "getfield";
    case OP_INDEX: return "index";
    case OP_NEW_INIT: return "new_init";
    case OP_THIS_INIT: return "this_init";
    case OP_SUPER_INIT: return "super_init";
    case OP_CALL: return "call";
    case OP_CALL_ON: return "call_on";
    case OP_CALL_SUPER: return "call_super";
    case OP_PUSH_INT: return "push_int";
    case OP_VALUE: return "value";
    case OP_PUSH_VOID: return "push_void";
    case OP_RET: return "ret";
    case OP_RET_VALUE: return "ret_value";
    case OP_INIT: return "init";
    case OP_STORE: return "store";
    default: return nullptr;
    }
}

uint32_t binary_kind(uint8_t token_kind) {
    switch (token_kind) {
    case sc::P_OR: return 0x03;
    case sc::P_XOR: return 0x04;
    case sc::P_AND: return 0x05;
    case sc::P_NE: return 0x06;
    case sc::P_EQ: return 0x07;
    case sc::P_GE: return 0x08;
    case sc::P_LE: return 0x09;
    case sc::P_GT: return 0x0A;
    case sc::P_LT: return 0x0B;
    case sc::P_SHR: return 0x0D;
    case sc::P_SHL: return 0x0E;
    case sc::P_SUB: return 0x0F;
    case sc::P_ADD: return 0x10;
    case sc::P_MOD: return 0x11;
    case sc::P_DIV: return 0x12;
    case sc::P_MUL: return 0x13;
    default: return NONE;
    }
}

uint32_t assign_kind(uint8_t token_kind) {
    switch (token_kind) {
    case sc::P_ASSIGN: return 0x23;
    case sc::P_MUL_ASSIGN: return 0x24;
    case sc::P_DIV_ASSIGN: return 0x25;
    case sc::P_MOD_ASSIGN: return 0x26;
    case sc::P_ADD_ASSIGN: return 0x27;
    case sc::P_SUB_ASSIGN: return 0x28;
    case sc::P_AND_ASSIGN: return 0x2C;
    case sc::P_XOR_ASSIGN: return 0x2D;
    case sc::P_OR_ASSIGN: return 0x2E;
    default: return NONE;
    }
}

const char* x_name(uint32_t kind) {
    switch (kind) {
    case 0x00: return "cond";
    case 0x01: return "lor";
    case 0x02: return "land";
    case 0x03: return "bor";
    case 0x04: return "bxor";
    case 0x05: return "band";
    case 0x06: return "ne";
    case 0x07: return "eq";
    case 0x08: return "ge";
    case 0x09: return "le";
    case 0x0A: return "gt";
    case 0x0B: return "lt";
    case 0x0D: return "shr";
    case 0x0E: return "shl";
    case 0x0F: return "sub";
    case 0x10: return "add";
    case 0x11: return "mod";
    case 0x12: return "div";
    case 0x13: return "mul";
    case 0x14: return "bnot";
    case 0x15: return "lnot";
    case 0x16: return "neg";
    case 0x17: return "postinc";
    case 0x18: return "postdec";
    case 0x19: return "preinc";
    case 0x1A: return "predec";
    case 0x1B: return "call";
    case 0x1C: return "literal";
    case 0x1E: return "new";
    case 0x1F: return "newarray";
    case 0x20: return "instanceof";
    case 0x21: return "cast";
    case 0x22: return "ctor";
    case 0x23: return "assign";
    case 0x24: return "mul_assign";
    case 0x25: return "div_assign";
    case 0x26: return "mod_assign";
    case 0x27: return "add_assign";
    case 0x28: return "sub_assign";
    case 0x2C: return "and_assign";
    case 0x2D: return "xor_assign";
    case 0x2E: return "or_assign";
    default: return nullptr;
    }
}

// ---------------------------------------------------------------------------
// the tree of the source, as the emitter reads it
// ---------------------------------------------------------------------------

bool is_name(const ps::Ast& ast, uint32_t e) {
    while (ast.nodes[e].kind == ps::FIELD) {
        e = ast.nodes[e].a;
    }
    return ast.nodes[e].kind == ps::NAME;
}

std::vector<uint32_t> name_toks(const ps::Ast& ast, uint32_t e) {
    std::vector<uint32_t> out;
    while (ast.nodes[e].kind == ps::FIELD) {
        out.push_back(ast.nodes[e].name_tok);
        e = ast.nodes[e].a;
    }
    out.push_back(ast.nodes[e].tok0);
    // out.reverse()
    size_t i = 0;
    size_t j = out.size();
    while (j > i + 1) {
        j -= 1;
        const uint32_t t = out[i];
        out[i] = out[j];
        out[j] = t;
        i += 1;
    }
    return out;
}

uint32_t strip_parens(const ps::Ast& ast, uint32_t e) {
    while (ast.nodes[e].kind == ps::PAREN) {
        e = ast.nodes[e].a;
    }
    return e;
}

bool is_ctor_call(const ps::Ast& ast, uint32_t s) {
    const ps::Node& n = ast.nodes[s];
    return n.kind == ps::EXPR_STMT && ast.nodes[n.a].kind == ps::CTOR_CALL;
}

namespace {

// ---------------------------------------------------------------------------
// what the emitter keeps
// ---------------------------------------------------------------------------

// An enclosing loop or switch.
struct Flow {
    bool is_loop;
    uint32_t brk;           // label behind the statement
    uint32_t cont;          // label continue jumps to (loops)
    uint32_t mark;          // number of live locals at the entry
    bool cont_proven;       // a for with an update: the one place a shipped source has
    //                         a continue in
};

const uint32_t SEG_LOCAL = 0;
const uint32_t SEG_FIELD = 1;
const uint32_t SEG_CLASS = 2;

// One identifier of a dotted name, resolved.
struct Segment {
    uint32_t kind;
    uint32_t index;         // SEG_LOCAL: the local
    std::string cls;        // SEG_FIELD: the class of the ref; SEG_CLASS: the class
    std::string name;       // SEG_FIELD
    std::string desc;       // SEG_FIELD
};

// A dotted name, resolved identifier by identifier.
struct Walk {
    bool ok = true;
    std::vector<Segment> segs;
    bool has_type = false;
    std::string type;       // descriptor of the value the name stands for
    bool has_cls = false;
    std::string cls;        // the class, when the name is a class name
};

// Emits the trees of one class.
class Emitter {
public:
    Emitter(classes::UnitScope& unit_scope, const classes::ClassInfo& info, pk::PackNames& pack_names,
            floatlit::Mode mode)
        : scope(unit_scope),
          ast(unit_scope.ast),
          toks(unit_scope.ast.toks),
          diags(unit_scope.diags),
          cls(info.name),
          this_desc("L" + info.name + ";"),
          packs(pack_names),
          float_mode(mode),
          declaring(NONE),
          is_ctor(false),
          is_static(false),
          returns_value(false),
          in_field(false) {}

    Emitter(const Emitter&) = delete;
    Emitter& operator=(const Emitter&) = delete;

private:
    classes::UnitScope& scope;
    const ps::Ast& ast;
    const std::vector<sc::Token>& toks;
    Diagnostics& diags;
    const std::string cls;                      // the class being compiled
    const std::string this_desc;
    pk::PackNames& packs;
    const floatlit::Mode float_mode;
    // the tree being emitted
    Tree tree;
    std::vector<uint32_t> labels;               // label -> node position, NONE until placed
    std::vector<std::string_view> local_names;  // the live locals; the index is the local
    std::vector<std::string> local_types;       // their descriptors
    uint32_t declaring;                         // the local whose initialiser is being compiled
    std::vector<Flow> flow;
    bool is_ctor;
    bool is_static;
    bool returns_value;                         // a method with a return type other than void
    bool in_field;

    const ps::Node& N(uint32_t n) const { return ast.nodes[n]; }

    // len(self.local_names)
    uint32_t live() const { return static_cast<uint32_t>(local_names.size()); }

    // -- trees ---------------------------------------------------------------

    void begin() {
        tree = Tree();
        labels.clear();
        local_names.clear();
        local_types.clear();
        declaring = NONE;
        flow.clear();
        is_ctor = false;
        is_static = false;
        returns_value = false;
        in_field = false;
    }

    // Turn the labels into offsets: target position - position of the jump,
    // in nodes, as an unsigned 32-bit number.
    Tree finish() {
        Tree out = std::move(tree);
        tree = Tree();
        const uint32_t count = static_cast<uint32_t>(out.nodes.size());
        uint32_t i = 0;
        for (SymNode& n : out.nodes) {
            if (n.kind == K_LABEL) {
                uint32_t target = labels[n.value];
                if (target == NONE) {           // only after an error
                    target = count;
                }
                n.value = target - i;           // modulo 2**32
                n.kind = K_IMM;
            }
            i += 1;
        }
        return out;
    }

public:
    // The tree of a field initialiser (DECLARATOR d with an initialiser):
    // E(init), end.
    Tree field_tree(uint32_t d, bool is_static_field) {
        begin();
        is_static = is_static_field;
        in_field = true;
        expr(N(d).a, OP_EXPR);
        put(OP_END, N(d).tok1 + 1);
        return finish();
    }

    // The tree of a method or constructor (METHOD_DECL m).
    //
    //     constructor     the this(..) / super(..) call; when none is
    //                     written: push_int 0, super_init, stmt ctor
    //     all             the statements of the body
    //     void method     push_void, releases, ret_value    (implicit return)
    //     constructor     ret, releases                     (implicit return)
    //     all             releases of the locals of the body, then of the
    //                     parameters and this
    //
    // A body written ";" gives an empty tree, and so does "{}" on a method; a
    // constructor written "{}" still holds the super call and the return.
    Tree method_tree(uint32_t method) {
        begin();
        const ps::Node& m = N(method);
        is_ctor = (m.flags & ps::F_CTOR) != 0;
        is_static = (m.flags & ps::ACC_STATIC) != 0;
        if (m.b == ps::NONE) {
            return finish();
        }
        const ps::Node& body = N(m.b);
        const ps::Items stmts = ast.items(body);
        if (!is_ctor && stmts.empty()) {
            return finish();
        }
        returns_value = !is_ctor && toks[N(m.a).tok0].kind != sc::KW_VOID;
        if (!is_static) {
            declare("this", this_desc);
        }
        const ps::Items params = ast.items(m);
        uint32_t k = params.size();
        while (k > 0) {                         // the last parameter first
            k -= 1;
            const ps::Node& p = N(params[k]);
            declare(ast.text(p.name_tok), classes::descriptor(scope, N(p.a), p.dims()));
        }
        const uint32_t frame = live();          // this + parameters
        const uint32_t close = body.tok1;       // the "}" of the body
        if (is_ctor && !(!stmts.empty() && is_ctor_call(ast, stmts[0]))) {
            // built when the parser has read the first token of the body and
            // sees that it is not this( / super(
            const uint32_t first = body.tok0 + 1;
            put_imm(OP_PUSH_INT, 0, first);
            put(OP_SUPER_INIT, first);
            put_imm(OP_STMT, X_CTOR, first);
        }
        const size_t before = tree.nodes.size();
        const uint32_t errors = diags.errors;
        for (uint32_t s : stmts) {
            stmt(s, false);
        }
        if (!is_ctor && tree.nodes.size() == before && diags.errors == errors) {
            // "{ ; }" or "{ { } }": whether the game's compiler takes such a
            // body for an empty one is not known; it is not taken for one here
            unverified(body.tok0, "method body whose statements emit no node");
        }
        // The implicit return: constructors and void methods have it, value
        // methods do not.  No shipped source tells "by the kind of method"
        // from "unless the last statement is a return"; the kind decides here.
        const uint32_t last = stmts.empty() ? ps::NONE : stmts[stmts.size() - 1];
        if (returns_value) {
            // (a method with a return type and no statement gave an empty
            // tree above: there is a last statement)
            if (N(last).kind != ps::RETURN) {
                unverified(close, "method with a return type that does not end in a return statement");
            }
        } else {
            if (last != ps::NONE && N(last).kind == ps::RETURN) {
                unverified(N(last).tok0,
                           "return as the last statement of a void method or a constructor");
            }
            ret(ps::NONE, close);
        }
        releases(frame, close, true);           // end of the body block
        releases(0, close, true);               // end of the method
        return finish();
    }

private:
    // -- nodes ---------------------------------------------------------------

    uint16_t line(uint32_t tok) const {
        if (tok >= toks.size()) {
            tok = static_cast<uint32_t>(toks.size()) - 1;
        }
        // (a line is at most scanner::MAX_LINE: the scanner refuses the rest)
        return static_cast<uint16_t>(toks[tok].line);
    }

    void put(uint8_t op, uint32_t tok) {
        tree.nodes.push_back(SymNode{op, line(tok), K_NONE, 0});
    }

    void put_imm(uint8_t op, uint32_t value, uint32_t tok) {
        tree.nodes.push_back(SymNode{op, line(tok), K_IMM, value});
    }

    void put_jump(uint8_t op, uint32_t label, uint32_t tok) {
        tree.nodes.push_back(SymNode{op, line(tok), K_LABEL, label});
    }

    SLRRC_NOINLINE void put_operand(uint8_t op, uint8_t kind, Operand operand, uint32_t tok) {
        tree.nodes.push_back(
            SymNode{op, line(tok), kind, static_cast<uint32_t>(tree.operands.size())});
        tree.operands.push_back(std::move(operand));
    }

    SLRRC_NOINLINE void put_utf8(uint8_t op, std::string_view text, uint32_t tok) {
        put_operand(op, K_UTF8, Operand{std::string(text), std::string(), std::string(), 0}, tok);
    }

    SLRRC_NOINLINE void put_class(uint8_t op, std::string_view name, uint32_t tok) {
        put_operand(op, K_CLASS, Operand{std::string(name), std::string(), std::string(), 0}, tok);
    }

    SLRRC_NOINLINE void put_ref(uint8_t op, std::string_view cls_name, std::string_view name,
                                std::string_view desc, uint32_t tok) {
        put_operand(op, K_REF, Operand{std::string(cls_name), std::string(name), std::string(desc), 0},
                    tok);
    }

    // put_utf8(OP_TYPE, classes.descriptor(scope, type, extra_dims), tok): the
    // carrier of the type of a new, a new T[n] or an instanceof.
    SLRRC_NOINLINE void type_carrier(uint32_t type_node, uint32_t extra_dims, uint32_t tok) {
        put_utf8(OP_TYPE, classes::descriptor(scope, N(type_node), extra_dims), tok);
    }

    // The carrier of a cast: a cast to a class carries a class entry, a cast
    // to int or float a type descriptor.
    SLRRC_NOINLINE void cast_carrier(uint32_t type_node, uint32_t tok) {
        const std::string desc = classes::descriptor(scope, N(type_node), 0);
        if (ps::is_primitive(ast, N(type_node))) {
            put_utf8(OP_TYPE, desc, tok);
        } else {
            // desc[1:len(desc) - 1]
            const std::string_view inner =
                desc.size() >= 2 ? std::string_view(desc).substr(1, desc.size() - 2) : std::string_view();
            put_class(OP_CLASS, inner, tok);
        }
    }

    uint32_t new_label() {
        labels.push_back(NONE);
        return static_cast<uint32_t>(labels.size()) - 1;
    }

    void place(uint32_t label) {
        labels[label] = static_cast<uint32_t>(tree.nodes.size());
    }

    // -- diagnostics ---------------------------------------------------------

    SLRRC_NOINLINE void error(const char* code, uint32_t tok, std::string_view text) {
        const sc::Token& t = toks[tok];
        diags.error(code, t.line, sc::column_at(ast.source, t.pos), std::string(text));
    }

    SLRRC_NOINLINE void unverified(uint32_t tok, std::string_view text) {
        const sc::Token& t = toks[tok];
        diags.unverified(t.line, sc::column_at(ast.source, t.pos), text);
    }

    // unverified(tok, head + name + tail): the "%s" of the reference
    SLRRC_NOINLINE void unverified(uint32_t tok, std::string_view head, std::string_view name,
                                   std::string_view tail) {
        std::string text(head);
        text += name;
        text += tail;
        unverified(tok, text);
    }

    // -- locals --------------------------------------------------------------

    uint32_t declare(std::string_view name, std::string desc) {
        local_names.push_back(name);
        local_types.push_back(std::move(desc));
        if (local_names.size() > tree.max_locals) {
            tree.max_locals = live();
        }
        return live() - 1;
    }

    // Index of the live local of that name, the innermost first; NONE when
    // there is none.
    uint32_t lookup_local(std::string_view name) const {
        uint32_t k = live();
        while (k > 0) {
            k -= 1;
            if (local_names[k] == name) {
                return k;
            }
        }
        return NONE;
    }

    // release nodes for the locals from the highest down to `down_to`.
    // close: the scope ends, the locals are gone; otherwise (break, continue,
    // return) the code behind is compiled as if nothing had happened.
    void releases(uint32_t down_to, uint32_t tok, bool close) {
        uint32_t k = live();
        while (k > down_to) {
            k -= 1;
            put_imm(OP_RELEASE, k, tok);
        }
        if (close && local_names.size() > down_to) {
            local_names.resize(down_to);
            local_types.resize(down_to);
        }
    }

    // -- statements ----------------------------------------------------------

    // The token the game's parser had read when it reduced statement s
    // (statements.md 8.2).  dual: s stands where the grammar allows both
    // Statement and StatementNoShortIf, that is in the then-branch of an if,
    // and in the else-branch or loop body of a statement that itself stands in
    // such a place.  There the parser reads one token past a finished
    // statement before it reduces it; an if without else always costs that
    // token.
    uint32_t end_tok(uint32_t s, bool dual) const {
        for (;;) {
            const ps::Node& n = N(s);
            const uint16_t k = n.kind;
            if (k == ps::IF) {
                if (n.c == ps::NONE) {
                    return n.tok1 + 1;
                }
                s = n.c;
            } else if (k == ps::WHILE || k == ps::FOR) {
                s = n.b;
            } else {
                return dual ? n.tok1 + 1 : n.tok1;
            }
        }
    }

    void stmt(uint32_t statement, bool dual) {
        const ps::Node& s = N(statement);
        const uint16_t k = s.kind;
        if (k == ps::EXPR_STMT) {
            expr(s.a, OP_STMT);
        } else if (k == ps::IF) {
            // E(c), jump_false X, [then], X:
            // E(c), jump_false X, [then], jump Y, X: [else], Y:
            const uint32_t end = end_tok(statement, dual);
            expr(s.a, OP_EXPR);
            const uint32_t l_else = new_label();
            put_jump(OP_JUMP_FALSE, l_else, end);
            stmt(s.b, true);
            if (s.c == ps::NONE) {
                place(l_else);
            } else {
                const uint32_t l_end = new_label();
                put_jump(OP_JUMP, l_end, end);
                place(l_else);
                stmt(s.c, dual);
                place(l_end);
            }
        } else if (k == ps::BLOCK) {
            const uint32_t mark = live();
            for (uint32_t x : ast.items(s)) {
                stmt(x, false);
            }
            releases(mark, s.tok1, true);
        } else if (k == ps::LOCAL_VAR) {
            local_decl(statement);
        } else if (k == ps::RETURN) {
            return_stmt(statement);
        } else if (k == ps::FOR) {
            for_stmt(statement, dual);
        } else if (k == ps::WHILE) {
            // T: E(c), jump_false X, [body], jump T, X:
            const uint32_t end = end_tok(statement, dual);
            const uint32_t l_top = new_label();
            const uint32_t l_end = new_label();
            place(l_top);
            expr(s.a, OP_EXPR);
            put_jump(OP_JUMP_FALSE, l_end, end);
            flow.push_back(Flow{true, l_end, l_top, live(), false});
            stmt(s.b, dual);
            flow.pop_back();
            put_jump(OP_JUMP, l_top, end);
            place(l_end);
        } else if (k == ps::DO) {
            // T: [body], C: E(c), jump_true T, X:
            const uint32_t l_top = new_label();
            const uint32_t l_cont = new_label();
            const uint32_t l_end = new_label();
            place(l_top);
            flow.push_back(Flow{true, l_end, l_cont, live(), false});
            stmt(s.a, false);
            flow.pop_back();
            place(l_cont);
            expr(s.b, OP_EXPR);
            put_jump(OP_JUMP_TRUE, l_top, s.tok1);
            place(l_end);
        } else if (k == ps::SWITCH) {
            switch_stmt(statement);
        } else if (k == ps::BREAK) {
            // releases of the locals declared since the loop or switch was
            // entered, jump behind it
            if (flow.empty()) {
                error(E_SYNTAX, s.tok0, "break outside a loop or a switch");
                return;
            }
            const Flow f = flow[flow.size() - 1];
            releases(f.mark, s.tok1, false);
            put_jump(OP_JUMP, f.brk, s.tok1);
        } else if (k == ps::CONTINUE) {
            continue_stmt(statement);
        } else if (k == ps::EMPTY) {
            // nothing
        } else {
            error(E_UNSUPPORTED, s.tok0, "statement the emitter has no rule for");
        }
    }

    // T v;      declare i, type T
    // T v = e;  declare i, type T, init, E(e), store
    // One sequence per declarator.  All its nodes carry the line of the token
    // behind the declared name.  The local exists before its initialiser is
    // compiled; no shipped initialiser names its own variable, so that order
    // is not confirmed (declaring).
    void local_decl(uint32_t statement) {
        const ps::Node& s = N(statement);
        for (uint32_t item : ast.items(s)) {
            const ps::Node& d = N(item);
            const uint32_t tok = d.name_tok + 1 + 2 * d.dims();
            std::string desc = classes::descriptor(scope, N(s.a), d.dims());
            put_imm(OP_DECLARE, live(), tok);
            put_utf8(OP_TYPE, desc, tok);
            const uint32_t local = declare(ast.text(d.name_tok), std::move(desc));
            if (d.a != ps::NONE) {
                put(OP_INIT, tok);
                declaring = local;
                expr(d.a, OP_EXPR);
                declaring = NONE;
                put(OP_STORE, tok);
            }
        }
    }

    // [init], T: E(c), jump_false X, [body], C: S(update), jump T,
    // X: releases of the locals of the init.
    // Without a condition there is no test; without an update C is the
    // closing jump.
    void for_stmt(uint32_t statement, bool dual) {
        const ps::Node& s = N(statement);
        const uint32_t end = end_tok(statement, dual);
        const uint32_t mark = live();
        const uint32_t init = s.c;
        if (init != ps::NONE) {
            if (N(init).kind == ps::LOCAL_VAR) {
                local_decl(init);
            } else {
                for (uint32_t x : ast.items(init)) {
                    expr(x, OP_STMT);
                }
            }
        }
        const uint32_t l_top = new_label();
        const uint32_t l_cont = new_label();
        const uint32_t l_end = new_label();
        place(l_top);
        if (s.a != ps::NONE) {
            expr(s.a, OP_EXPR);
            put_jump(OP_JUMP_FALSE, l_end, end);
        }
        flow.push_back(Flow{true, l_end, l_cont, live(), s.count > 0});
        stmt(s.b, dual);
        flow.pop_back();
        place(l_cont);
        for (uint32_t x : ast.items(s)) {
            expr(x, OP_STMT);
        }
        put_jump(OP_JUMP, l_top, end);
        place(l_end);
        releases(mark, end, true);
    }

    // E(subject); per case label: E(value), case G; pop; jump to the default
    // group or behind the switch; then the groups in source order, each
    // closed by the releases of its own locals (a group is a scope).  Nothing
    // separates the groups: one that does not end in break runs into the
    // next.
    void switch_stmt(uint32_t statement) {
        const ps::Node& s = N(statement);
        const uint32_t close = s.tok1;          // the "}" of the switch
        expr(s.a, OP_EXPR);
        const uint32_t l_end = new_label();
        uint32_t l_default = NONE;
        std::vector<uint32_t> group_labels;
        for (uint32_t group : ast.items(s)) {
            const uint32_t lab = new_label();
            group_labels.push_back(lab);
            for (uint32_t label : ast.items(N(group).a)) {
                const ps::Node& c = N(label);
                if (c.kind == ps::CASE_LABEL) {
                    expr(c.a, OP_EXPR);
                    put_jump(OP_CASE, lab, close);
                } else {
                    l_default = lab;
                }
            }
        }
        put(OP_POP, close);
        put_jump(OP_JUMP, l_default == NONE ? l_end : l_default, close);
        const uint32_t mark = live();
        flow.push_back(Flow{false, l_end, NONE, mark, false});
        size_t k = 0;
        for (uint32_t group : ast.items(s)) {
            const ps::Node& g = N(group);
            place(group_labels[k]);
            k += 1;
            for (uint32_t x : ast.items(g)) {
                stmt(x, false);
            }
            // the releases carry the line of the token behind the group: the
            // next label, or the "}"
            releases(mark, g.tok1 + 1, true);
        }
        flow.pop_back();
        place(l_end);
    }

    // releases, then a jump to the update of the for (to the condition of a
    // while or do).
    void continue_stmt(uint32_t statement) {
        const ps::Node& s = N(statement);
        bool found = false;             // the innermost enclosing loop
        Flow f = Flow{false, NONE, NONE, 0, false};
        bool through_switch = false;
        size_t k = flow.size();
        while (k > 0) {
            k -= 1;
            if (flow[k].is_loop) {
                f = flow[k];
                found = true;
                break;
            }
            through_switch = true;
        }
        if (!found) {
            error(E_SYNTAX, s.tok0, "continue outside a loop");
            return;
        }
        if (through_switch || !f.cont_proven) {
            unverified(s.tok0, "continue in a while or do loop, in a for without update, or inside a switch");
        }
        releases(f.mark, s.tok1, false);
        put_jump(OP_JUMP, f.cont, s.tok1);
    }

    // Every return of the shipped sources fits its method: a value in a
    // method with a return type, none in a void method or a constructor.
    void return_stmt(uint32_t statement) {
        const ps::Node& s = N(statement);
        if (s.a != ps::NONE) {
            if (is_ctor) {
                error(E_UNSUPPORTED, s.tok0,
                      "return with a value in a constructor (no shipped source uses it)");
                return;
            }
            if (!returns_value) {
                unverified(s.tok0, "return with a value in a void method");
            }
        } else if (returns_value) {
            unverified(s.tok0, "return without a value in a method with a return type");
        }
        ret(s.a, s.tok1);
    }

    // return e;                E(e), value, releases, ret_value
    // return;                  push_void, releases, ret_value
    // return; in a constructor ret, releases
    // Every live local is released, down to this; the scope stays open.
    // value: the expression, or NONE.
    void ret(uint32_t value, uint32_t tok) {
        if (is_ctor) {
            put(OP_RET, tok);
            releases(0, tok, false);
            return;
        }
        if (value != ps::NONE) {
            expr(value, OP_EXPR);
            put(OP_VALUE, tok);
        } else {
            put(OP_PUSH_VOID, tok);
        }
        releases(0, tok, false);
        put(OP_RET_VALUE, tok);
    }

    // -- expressions ---------------------------------------------------------

    // Emit expression e.  top is OP_EXPR when its value is used and OP_STMT
    // when e is an expression statement (or an element of a for header): the
    // node of the outermost construct is written with it, nothing else
    // differs.
    void expr(uint32_t expression, uint8_t top) {
        const ps::Node& e = N(expression);
        const uint16_t k = e.kind;
        if (k == ps::LITERAL) {
            literal(expression, top);
        } else if (k == ps::NAME) {
            name(expression);
        } else if (k == ps::FIELD) {
            if (is_name(ast, expression)) {
                name(expression);
            } else {
                // a member of a computed value: by name
                const std::string_view member = ast.text(e.name_tok);
                receiver_check(e.a);
                if (member == "length") {
                    // inside a name the length of an array is a field ref
                    // of the array type; here the type is not known
                    unverified(e.name_tok, "member length of a computed value");
                }
                expr(e.a, OP_EXPR);
                put_utf8(OP_GETFIELD, member, e.tok1);
            }
        } else if (k == ps::CALL) {
            call(expression, top);
        } else if (k == ps::ASSIGN) {
            // the value first, then the target
            const uint8_t op = toks[e.op_tok].kind;
            if (op != sc::P_ASSIGN && top == OP_EXPR) {
                unverified(e.op_tok, "compound assignment used as a value");
            }
            expr(e.b, OP_EXPR);
            expr(e.a, OP_EXPR);
            const uint32_t kind = assign_kind(op);
            if (kind == NONE) {
                no_rule(e.tok0);                // (not reached: see the head of the file)
                return;
            }
            put_imm(top, kind, e.tok1 + 1);
        } else if (k == ps::BINARY) {
            const uint8_t op = toks[e.op_tok].kind;
            const uint32_t behind = e.tok1 + 1;
            if (op == sc::P_LAND || op == sc::P_LOR) {
                // E(x), test, jump Z, E(y), expr land / lor, Z:
                expr(e.a, OP_EXPR);
                put(op == sc::P_LAND ? OP_AND_TEST : OP_OR_TEST, behind);
                const uint32_t skip = new_label();
                put_jump(OP_JUMP, skip, behind);
                expr(e.b, OP_EXPR);
                put_imm(top, op == sc::P_LAND ? X_LAND : X_LOR, behind);
                place(skip);
            } else {
                expr(e.a, OP_EXPR);
                expr(e.b, OP_EXPR);
                const uint32_t kind = binary_kind(op);
                if (kind == NONE) {
                    no_rule(e.tok0);            // (not reached: see the head of the file)
                    return;
                }
                put_imm(top, kind, behind);
            }
        } else if (k == ps::PAREN) {
            expr(e.a, top);
        } else if (k == ps::INDEX) {
            // the index first, then the array
            expr(e.b, OP_EXPR);
            expr(e.a, OP_EXPR);
            put(OP_INDEX, e.tok1);
        } else if (k == ps::UNARY) {
            const uint8_t op = toks[e.op_tok].kind;
            expr(e.a, OP_EXPR);
            if (op == sc::P_SUB) {
                put_imm(top, X_NEG, e.tok1 + 1);
            } else if (op == sc::P_NOT) {
                put_imm(top, X_LNOT, e.tok1 + 1);
            } else if (op == sc::P_BITNOT) {
                put_imm(top, X_BNOT, e.tok1 + 1);
            } else if (op == sc::P_INC) {
                put_imm(top, X_PREINC, e.tok1 + 1);
            } else if (op == sc::P_DEC) {
                put_imm(top, X_PREDEC, e.tok1 + 1);
            }
            // a unary plus has no node
        } else if (k == ps::POSTFIX) {
            expr(e.a, OP_EXPR);
            put_imm(top, toks[e.op_tok].kind == sc::P_INC ? X_POSTINC : X_POSTDEC, e.tok1);
        } else if (k == ps::NEW) {
            // expr new, type, arguments, count, new_init; the type carrier is
            // built when the class name is reduced: line of the "("
            put_imm(top, X_NEW, e.tok1);
            type_carrier(e.a, 0, N(e.a).tok1 + 1);
            for (uint32_t a : ast.items(e)) {
                expr(a, OP_EXPR);
            }
            put_imm(OP_PUSH_INT, e.count, e.tok1);
            put(OP_NEW_INIT, e.tok1);
        } else if (k == ps::NEW_ARRAY) {
            expr(e.b, OP_EXPR);
            put_imm(top, X_NEWARRAY, e.tok1 + 1);
            type_carrier(e.a, 1, e.tok1 + 1);
        } else if (k == ps::THIS) {
            if (is_static) {
                unverified(e.tok0, "this in a static method or in the initialiser of a static field");
            } else if (in_field) {
                unverified(e.tok0, "this in a field initialiser");
            }
            put_imm(OP_LOCAL, 0, e.tok0);
        } else if (k == ps::CAST) {
            // a cast to a class carries a class entry, a cast to int or
            // float a type descriptor
            expr(e.b, OP_EXPR);
            put_imm(top, X_CAST, e.tok1 + 1);
            cast_carrier(e.a, e.tok1 + 1);
        } else if (k == ps::INSTANCEOF) {
            expr(e.a, OP_EXPR);
            put_imm(top, X_INSTANCEOF, e.tok1 + 1);
            type_carrier(e.b, 0, e.tok1 + 1);
        } else if (k == ps::COND) {
            // no jump: both branches are evaluated
            expr(e.a, OP_EXPR);
            expr(e.b, OP_EXPR);
            expr(e.c, OP_EXPR);
            put_imm(top, X_COND, e.tok1 + 1);
        } else if (k == ps::CTOR_CALL) {
            // this(..) / super(..): arguments, count, this_init / super_init,
            // stmt ctor; the three nodes carry the line of the ";"
            for (uint32_t a : ast.items(e)) {
                expr(a, OP_EXPR);
            }
            const uint32_t semi = e.tok1 + 1;
            put_imm(OP_PUSH_INT, e.count, semi);
            put(toks[e.tok0].kind == sc::KW_THIS ? OP_THIS_INIT : OP_SUPER_INIT, semi);
            put_imm(OP_STMT, X_CTOR, semi);
        } else {
            no_rule(e.tok0);
        }
    }

    SLRRC_NOINLINE void no_rule(uint32_t tok) {
        error(E_UNSUPPORTED, tok, "expression the emitter has no rule for");
    }

    // expr literal and its carrier, on the line of the literal.
    void literal(uint32_t expression, uint8_t top) {
        const uint32_t tok = N(expression).tok0;
        const uint8_t kind = toks[tok].kind;
        const std::string_view text = ast.text(tok);
        put_imm(top, X_LITERAL, tok);
        if (kind == sc::T_INT) {
            put_imm(OP_INT, sc::int_value(text), tok);
        } else if (kind == sc::T_FLOAT) {
            const sc::FloatDigits digits = sc::float_digits(text);
            put_imm(OP_FLOAT, floatlit::float_bits(float_mode, digits.int_digits, digits.frac_digits),
                    tok);
        } else if (kind == sc::T_STR) {
            put_utf8(OP_STR, sc::string_value(text), tok);
        } else if (kind == sc::T_RID) {
            const sc::RidParts parts = sc::rid_parts(text);
            pk::Answer answer = pk::resolve(packs, pk::pack_path(parts.pack), parts.id);
            if (answer.why != pk::PackWhy::Ok) {
                rid_note(tok, parts.pack, parts.id, answer);
            }
            put_operand(OP_RID, K_RID,
                        Operand{std::move(answer.name), std::string(), std::string(), answer.id}, tok);
        } else if (kind == sc::KW_NULL) {
            put(OP_NULL, tok);
        } else {
            put_imm(OP_BOOL, kind == sc::KW_TRUE ? 1 : 0, tok);
        }
    }

    // The diagnostic for a resource id that is not compiled to its own pack
    // and id (answer: what packs::resolve() gave).
    void rid_note(uint32_t tok, std::string_view pack, uint32_t number, const pk::Answer& answer) {
        const pk::PackWhy why = answer.why;
        const uint32_t k = (number >> 16) & 0xFFFF;
        if (why == pk::PackWhy::NoFile) {
            // A pack without a file (observed once, Build 940: the two
            // literals of TimeAttackTest).  The resource the class will name
            // is not the one the source names, so this is said at every such
            // literal, and in strict mode too: a warning, because the shipped
            // source has to compile.
            const sc::Token& t = toks[tok];
            std::string text("pack ");
            text += pack;
            text += " has no file: the resource id is compiled to ";
            text += pk::NO_FILE_PACK;
            text += " id ";
            text += dec(pk::NO_FILE_ID);
            diags.warning(W_PACK, t.line, sc::column_at(ast.source, t.pos), std::move(text));
        } else if (why == pk::PackWhy::Zero) {
            std::string text("resource id whose low 16 bits are 0: compiled to ");
            text += pk::NO_FILE_PACK;
            text += " id ";
            text += dec(pk::NO_FILE_ID);
            text += ", as the resource engine of the game resolves it";
            unverified(tok, text);
        } else if (why == pk::PackWhy::External) {
            std::string text("resource id with a high word: external reference ");
            text += dec(k);
            text += " of pack ";
            text += pack;
            text += ", compiled to ";
            text += answer.name;
            text += " id 0x";
            text += hex(answer.id, 1, true);
            unverified(tok, text);
        } else if (why == pk::PackWhy::NoExternal) {
            std::string text("resource id with a high word: pack ");
            text += pack;
            text += " has no external reference ";
            text += dec(k);
            text += ", compiled to ";
            text += pk::NO_FILE_PACK;
            text += " id ";
            text += dec(pk::NO_FILE_ID);
            unverified(tok, text);
        } else if (why == pk::PackWhy::Fault) {
            std::string text("resource id with a high word: external reference ");
            text += dec(k);
            text += " of pack ";
            text += pack;
            text += " is a pack without a file; the compiler of the game fails on this";
            error(E_UNSUPPORTED, tok, text);
        } else {
            std::string text("resource id with a high word (external reference ");
            text += dec(k);
            text += " of pack ";
            text += pack;
            text += "): the pack names of this host do not know the external references of a pack";
            error(E_UNSUPPORTED, tok, text);
        }
    }

    // -- names ---------------------------------------------------------------

    // Resolve the first `count` identifiers of a dotted name.  The first one
    // is a local, else a field of the class or a superclass, else a class;
    // every further one is a field of the type reached so far (length for an
    // array).
    Walk walk(const std::vector<uint32_t>& name_tokens, uint32_t count) {
        Walk w;
        std::string_view id = ast.text(name_tokens[0]);
        const uint32_t local = lookup_local(id);
        classes::FieldHit hit;
        if (local == NONE) {
            hit = classes::find_field(scope, cls, id);
        }
        if (local != NONE || hit.field != nullptr) {
            // No shipped source has a name that is a variable and a class,
            // so the place of class names in this order is not known.  The
            // question is decided from the source alone: a class the source
            // declares, or a name it writes as a type anywhere.  (The
            // provider is not asked about every variable, and what an
            // earlier method happened to resolve plays no part.  A class the
            // source uses only as the head of a name, Util.abs(a), is not
            // seen here.)
            if (scope.own_by_simple.find(id) != scope.own_by_simple.end() ||
                scope.type_names.find(id) != scope.type_names.end()) {
                unverified(name_tokens[0], "", id,
                           " is a variable and the name of a class; the variable is taken");
            }
        }
        if (local != NONE) {
            if (local == declaring) {
                unverified(name_tokens[0], "local variable ", id, " used in its own initialiser");
            }
            w.segs.push_back(Segment{SEG_LOCAL, local, std::string(), std::string(), std::string()});
            w.has_type = true;
            w.type = local_types[local];
        } else if (hit.field != nullptr) {
            const classes::FieldInfo& f = *hit.field;
            if (is_static && !f.is_static) {
                // no static method and no initialiser of a static field of
                // the shipped sources names an instance field
                unverified(name_tokens[0], "instance field ", id,
                           " used in a static method or in the initialiser of a static field");
            }
            w.segs.push_back(Segment{SEG_FIELD, 0, f.is_static ? *hit.owner : cls, f.name, f.desc});
            w.has_type = true;
            w.type = f.desc;
        } else {
            const std::string* full = classes::resolve_type(scope, id);
            if (full == nullptr) {
                std::string text(id);
                text += " is no local variable, field or class";
                error(E_NAME, name_tokens[0], text);
                w.ok = false;
                return w;
            }
            w.segs.push_back(Segment{SEG_CLASS, 0, *full, std::string(), std::string()});
            w.has_cls = true;
            w.cls = *full;
        }
        uint32_t k = 1;
        while (k < count) {
            id = ast.text(name_tokens[k]);
            // cls = w.cls; if cls is None: cls = classes.class_of_desc(w.type)
            bool has_c = w.has_cls;
            std::string c;
            if (has_c) {
                c = w.cls;
            } else {
                has_c = classes::class_of_desc(w.type, c);
            }
            if (has_c && !c.empty() && c[0] == '[') {
                // the one member of an array; the ref names the array type
                if (id != "length") {
                    std::string text("an array has no member ");
                    text += id;
                    error(E_MEMBER, name_tokens[k], text);
                    w.ok = false;
                    return w;
                }
                w.segs.push_back(Segment{SEG_FIELD, 0, c, "length", "I"});
                w.has_type = true;
                w.type = "I";
            } else {
                classes::FieldHit member;
                if (has_c) {
                    member = classes::find_field(scope, c, id);
                }
                if (member.field == nullptr) {
                    // 'no field %s in %s' % (name, cls if cls else 'a value of type %s' % w.type)
                    std::string text("no field ");
                    text += id;
                    text += " in ";
                    if (has_c && !c.empty()) {
                        text += c;
                    } else {
                        text += "a value of type ";
                        text += w.type;
                    }
                    error(E_MEMBER, name_tokens[k], text);
                    w.ok = false;
                    return w;
                }
                const classes::FieldInfo& f = *member.field;
                w.segs.push_back(Segment{SEG_FIELD, 0, f.is_static ? *member.owner : c, f.name, f.desc});
                w.has_type = true;
                w.type = f.desc;
            }
            w.has_cls = false;
            w.cls.clear();
            k += 1;
        }
        return w;
    }

    void put_segment(const Segment& seg, uint32_t tok) {
        if (seg.kind == SEG_LOCAL) {
            put_imm(OP_LOCAL, seg.index, tok);
        } else if (seg.kind == SEG_FIELD) {
            put_ref(OP_FIELD, seg.cls, seg.name, seg.desc, tok);
        } else {
            put_class(OP_CLASS, seg.cls, tok);
        }
    }

    // A name in value position (read or written alike):
    // x        local i  /  field [C.x:T]
    // a.b.c    path 3, head, field [..], field [..]
    void name(uint32_t expression) {
        const std::vector<uint32_t> name_tokens = name_toks(ast, expression);
        const Walk w = walk(name_tokens, static_cast<uint32_t>(name_tokens.size()));
        if (!w.ok) {
            return;
        }
        if (w.has_cls) {
            std::string text("the class name ");
            text += ast.text(name_tokens[0]);
            text += " is used as a value";
            error(E_NAME, name_tokens[0], text);
            return;
        }
        const uint32_t n = static_cast<uint32_t>(w.segs.size());
        if (n == 1) {
            put_segment(w.segs[0], name_tokens[0]);
            return;
        }
        put_imm(OP_PATH, n, name_tokens[n - 1]);
        uint32_t k = 0;
        while (k < n) {
            put_segment(w.segs[k], name_tokens[k]);
            k += 1;
        }
    }

    // A parenthesised name as the receiver of a member or a call: no shipped
    // source has one.  It is compiled as any computed receiver.
    void receiver_check(uint32_t x) {
        if (N(x).kind == ps::PAREN && is_name(ast, strip_parens(ast, x))) {
            unverified(N(x).tok0, "parenthesised name as a receiver");
        }
    }

    // -- calls ---------------------------------------------------------------

    // f(..)          args, count, call, method [ref]  |  path 1, name "f"
    // a.b.f(..)      args, count, call, path n, segments, method [ref] | name "f"
    // X.f(..)        args, count, E(X), call_on "f"       (X computed)
    // super.f(..)    args, count, call_super "f"
    // closed by expr call / stmt call.  The count and the closing nodes carry
    // the line of the ")", the path the line of the callee name.
    void call(uint32_t expression, uint8_t top) {
        const ps::Node& e = N(expression);
        for (uint32_t a : ast.items(e)) {
            expr(a, OP_EXPR);
        }
        const uint32_t close = e.tok1;
        const uint32_t argc = e.count;
        const uint32_t target = e.a;
        const std::string_view callee_name = ast.text(e.name_tok);
        put_imm(OP_PUSH_INT, argc, close);
        if (target == ps::NONE) {
            put(OP_CALL, close);
            // One class file without source shows a local in the place of
            // the method (Texture.<init>(I)): the game's compiler seems to
            // look the callee up as a variable first.  No source has such a
            // clash; the call is compiled as a call.
            if (lookup_local(callee_name) != NONE) {
                unverified(e.name_tok, "call of ", callee_name,
                           " while a local variable of that name is in scope");
            } else if (classes::find_field(scope, cls, callee_name).field != nullptr) {
                unverified(e.name_tok, "call of ", callee_name,
                           " while the class has a field of that name");
            }
            callee(true, cls, expression, true);
        } else if (N(target).kind == ps::SUPER) {
            put_utf8(OP_CALL_SUPER, callee_name, close);
        } else if (is_name(ast, target)) {
            put(OP_CALL, close);
            call_path(expression);
        } else {
            // the receiver is computed behind the arguments and the count
            receiver_check(target);
            expr(target, OP_EXPR);
            put_utf8(OP_CALL_ON, callee_name, close);
        }
        put_imm(top, X_CALL, close);
    }

    // The path of a call through a name, a.b.f(..): path n, the segments of
    // the receiver, the callee.  (In the reference this stands in call(); it
    // is a function of its own here so that the resolved name is no part of
    // the frame of call(), which recurses.)
    SLRRC_NOINLINE void call_path(uint32_t expression) {
        const ps::Node& e = N(expression);
        const std::vector<uint32_t> name_tokens = name_toks(ast, e.a);
        const Walk w = walk(name_tokens, static_cast<uint32_t>(name_tokens.size()));
        if (w.ok) {
            const uint32_t n = static_cast<uint32_t>(w.segs.size());
            put_imm(OP_PATH, n + 1, e.name_tok);
            uint32_t k = 0;
            while (k < n) {
                put_segment(w.segs[k], name_tokens[k]);
                k += 1;
            }
            // cls = w.cls; if cls is None: cls = classes.class_of_desc(w.type)
            bool has_c = w.has_cls;
            std::string c;
            if (has_c) {
                c = w.cls;
            } else {
                has_c = classes::class_of_desc(w.type, c);
            }
            callee(has_c, c, expression, false);
        }
    }

    // The last segment of a call path: a method ref when every argument has
    // a typed form and the lookup on the receiver class finds a method, else
    // the name (behind a `path 1` header for a call without receiver).
    // has_cls false: the receiver has no class (the reference's None).
    SLRRC_NOINLINE void callee(bool has_cls, const std::string& receiver, uint32_t expression,
                               bool lone) {
        const ps::Node& e = N(expression);
        const std::string_view callee_name = ast.text(e.name_tok);
        const uint32_t argc = e.count;
        const bool known = has_cls && !(!receiver.empty() && receiver[0] == '[');
        bool typed = true;
        bool unknown_form = false;
        std::vector<classes::ArgType> types;
        for (uint32_t a : ast.items(e)) {
            if (is_typed(a)) {
                types.push_back(arg_type(a));
            } else {
                typed = false;
            }
            if (form_unknown(a)) {
                unknown_form = true;
            }
        }
        if (unknown_form && known && classes::has_method(scope, receiver, callee_name, argc)) {
            unverified(e.name_tok,
                       "argument true, false, (x), !x, ~x, ?:, instanceof or new T[n] in a call the "
                       "receiver class has a method for");
        }
        if (typed && known) {
            const classes::MethodHit hit = classes::lookup_method(scope, receiver, callee_name, types);
            if (hit.method != nullptr) {
                if (hit.tie) {
                    // which of them the game's lookup meets first is not known
                    unverified(e.name_tok, "two methods ", callee_name,
                               " fit the arguments equally well");
                }
                put_ref(OP_METHOD, receiver, callee_name, hit.method->desc, e.name_tok);
                return;
            }
        }
        if (lone) {
            put_imm(OP_PATH, 1, e.name_tok);
        }
        put_utf8(OP_NAME, callee_name, e.name_tok);
    }

    // Does the compiler know the type of an argument of this form?  Only for
    // a literal, a local, a field written as a simple name and this; a
    // parenthesised one does not count.
    bool is_typed(uint32_t a) const {
        const uint16_t k = N(a).kind;
        return k == ps::LITERAL || k == ps::NAME || k == ps::THIS;
    }

    // Type of an argument of a typed form: a descriptor, NULL_TYPE, or
    // nothing when the name is no variable.  (The view is of a constant, of
    // this_desc, of the type of a live local or of a field of a class: each
    // stays as it is until the lookup has been made.)
    classes::ArgType arg_type(uint32_t a) {
        const ps::Node& n = N(a);
        const uint16_t k = n.kind;
        if (k == ps::LITERAL) {
            const uint8_t t = toks[n.tok0].kind;
            if (t == sc::T_FLOAT) {
                return classes::ArgType{true, "F"};
            }
            if (t == sc::T_STR) {
                return classes::ArgType{true, classes::STRING_DESC};
            }
            if (t == sc::KW_NULL) {
                return classes::ArgType{true, classes::NULL_TYPE};
            }
            return classes::ArgType{true, "I"};         // int, resource id, true, false
        }
        if (k == ps::THIS) {
            return classes::ArgType{true, this_desc};
        }
        const std::string_view id = ast.text(n.tok0);
        const uint32_t local = lookup_local(id);
        if (local != NONE) {
            return classes::ArgType{true, local_types[local]};
        }
        const classes::FieldHit hit = classes::find_field(scope, cls, id);
        if (hit.field != nullptr) {
            return classes::ArgType{true, hit.field->desc};
        }
        return classes::ArgType{false, std::string_view()};
    }

    // Argument forms no shipped source has in a call that could be resolved
    // (expressions.md 6.2): whether the game's compiler knows their type is
    // not known.  true and false are taken as typed (int), the others as
    // untyped.
    bool form_unknown(uint32_t a) const {
        const ps::Node& n = N(a);
        const uint16_t k = n.kind;
        if (k == ps::LITERAL) {
            const uint8_t t = toks[n.tok0].kind;
            return t == sc::KW_TRUE || t == sc::KW_FALSE;
        }
        if (k == ps::PAREN) {
            const uint16_t x = N(strip_parens(ast, a)).kind;
            return x == ps::LITERAL || x == ps::NAME || x == ps::THIS;
        }
        if (k == ps::UNARY) {
            const uint8_t t = toks[n.op_tok].kind;
            return t == sc::P_NOT || t == sc::P_BITNOT;
        }
        return k == ps::COND || k == ps::INSTANCEOF || k == ps::NEW_ARRAY;
    }
};

}  // namespace

std::vector<Tree> emit_class(classes::UnitScope& scope, const decl::ClassBuild& build,
                             packs::PackNames& packs, floatlit::Mode float_mode) {
    Emitter em(scope, *build.info, packs, float_mode);
    std::vector<Tree> trees;
    trees.reserve(build.rows.owners.size());
    for (const decl::TreeOwner& owner : build.rows.owners) {
        if (owner.is_field) {
            trees.push_back(em.field_tree(owner.node, owner.is_static));
        } else {
            trees.push_back(em.method_tree(owner.node));
        }
    }
    return trees;
}

}  // namespace emit
}  // namespace slrrc
