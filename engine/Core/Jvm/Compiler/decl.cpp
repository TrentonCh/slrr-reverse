// decl.cpp - the declarations of a class: the first three steps of its pool
// and the rows of FILD, MTHD and CLSS.  (Reference: slrrc/decl.py; the
// functions stand in the order of that file and carry its names.)
//
// The walks recurse over the tree, which the parser keeps at most
// parser::MAX_DEPTH high; a level costs one small frame.
#include "decl.hpp"

#include <string>
#include <utility>

#include "parser.hpp"

namespace slrrc {
namespace decl {

namespace ps = parser;
using classes::ClassInfo;
using classes::UnitScope;
using pool::Pool;

// ---------------------------------------------------------------------------
// step 1: seed
// ---------------------------------------------------------------------------

void seed(Pool& pool, const ClassInfo& info) {
    pool.cls(info.name);
    if (info.has_super) {
        pool.cls(info.super);
    }
    for (const std::string& i : info.interfaces) {
        pool.cls(i);
    }
}

// ---------------------------------------------------------------------------
// step 2: types
// ---------------------------------------------------------------------------

void type_walk(Pool& pool, UnitScope& scope, uint32_t class_node, const ClassInfo& info,
               bool first_in_file) {
    const ps::Ast& ast = scope.ast;
    for (uint32_t member : ast.items(class_node)) {
        const ps::Node& m = ast.nodes[member];
        if (m.kind == ps::FIELD_DECL) {
            for (uint32_t item : ast.items(m)) {
                const ps::Node& d = ast.nodes[item];
                pool.utf8(classes::descriptor(scope, ast.nodes[m.a], d.dims()));
                if (d.a != ps::NONE) {
                    expr_types(pool, scope, d.a);
                }
            }
        } else {
            for (uint32_t item : ast.items(m)) {
                const ps::Node& p = ast.nodes[item];
                pool.utf8(classes::descriptor(scope, ast.nodes[p.a], p.dims()));
            }
            if (m.a != ps::NONE) {
                pool.utf8(classes::descriptor(scope, ast.nodes[m.a], 0));
            }
            if (m.b != ps::NONE) {
                stmt_types(pool, scope, m.b);
            }
        }
    }
    for (const std::string& i : info.interfaces) {
        std::string desc("L");
        desc += i;
        desc += ';';
        pool.utf8(desc);
    }
    if (!first_in_file) {
        std::string desc("L");
        desc += info.name;
        desc += ';';
        pool.utf8(desc);
    }
}

void local_types(Pool& pool, UnitScope& scope, uint32_t s) {
    const ps::Ast& ast = scope.ast;
    const ps::Node& n = ast.nodes[s];
    for (uint32_t item : ast.items(n)) {
        const ps::Node& d = ast.nodes[item];
        if (d.a != ps::NONE) {
            expr_types(pool, scope, d.a);
        }
    }
    for (uint32_t item : ast.items(n)) {
        const ps::Node& d = ast.nodes[item];
        pool.utf8(classes::descriptor(scope, ast.nodes[n.a], d.dims()));
    }
}

void stmt_types(Pool& pool, UnitScope& scope, uint32_t s) {
    const ps::Ast& ast = scope.ast;
    const ps::Node& n = ast.nodes[s];
    const uint16_t k = n.kind;
    if (k == ps::BLOCK) {
        for (uint32_t x : ast.items(n)) {
            stmt_types(pool, scope, x);
        }
    } else if (k == ps::LOCAL_VAR) {
        local_types(pool, scope, s);
    } else if (k == ps::EXPR_STMT) {
        expr_types(pool, scope, n.a);
    } else if (k == ps::IF) {
        expr_types(pool, scope, n.a);
        stmt_types(pool, scope, n.b);
        if (n.c != ps::NONE) {
            stmt_types(pool, scope, n.c);
        }
    } else if (k == ps::WHILE) {
        expr_types(pool, scope, n.a);
        stmt_types(pool, scope, n.b);
    } else if (k == ps::DO) {
        stmt_types(pool, scope, n.a);
        expr_types(pool, scope, n.b);
    } else if (k == ps::FOR) {
        if (n.c != ps::NONE) {
            if (ast.nodes[n.c].kind == ps::LOCAL_VAR) {
                local_types(pool, scope, n.c);
            } else {
                for (uint32_t e : ast.items(n.c)) {
                    expr_types(pool, scope, e);
                }
            }
        }
        if (n.a != ps::NONE) {
            expr_types(pool, scope, n.a);
        }
        for (uint32_t e : ast.items(n)) {
            expr_types(pool, scope, e);
        }
        stmt_types(pool, scope, n.b);
    } else if (k == ps::SWITCH) {
        expr_types(pool, scope, n.a);
        for (uint32_t group : ast.items(n)) {
            const ps::Node& g = ast.nodes[group];
            for (uint32_t label : ast.items(g.a)) {
                const ps::Node& lab = ast.nodes[label];
                if (lab.kind == ps::CASE_LABEL) {
                    expr_types(pool, scope, lab.a);
                }
            }
            for (uint32_t x : ast.items(g)) {
                stmt_types(pool, scope, x);
            }
        }
    } else if (k == ps::RETURN) {
        if (n.a != ps::NONE) {
            expr_types(pool, scope, n.a);
        }
    }
    // BREAK, CONTINUE, EMPTY carry no type
}

void expr_types(Pool& pool, UnitScope& scope, uint32_t e) {
    const ps::Ast& ast = scope.ast;
    const ps::Node& n = ast.nodes[e];
    const uint16_t k = n.kind;
    if (k == ps::NEW) {
        pool.utf8(classes::descriptor(scope, ast.nodes[n.a], 0));
        for (uint32_t x : ast.items(n)) {
            expr_types(pool, scope, x);
        }
    } else if (k == ps::NEW_ARRAY) {
        pool.utf8(classes::descriptor(scope, ast.nodes[n.a], 1));
        expr_types(pool, scope, n.b);
    } else if (k == ps::CAST) {
        if (ps::is_primitive(ast, ast.nodes[n.a])) {
            pool.utf8(classes::descriptor(scope, ast.nodes[n.a], 0));
        }
        expr_types(pool, scope, n.b);
    } else if (k == ps::INSTANCEOF) {
        expr_types(pool, scope, n.a);
        pool.utf8(classes::descriptor(scope, ast.nodes[n.b], 0));
    } else if (k == ps::FIELD || k == ps::UNARY || k == ps::POSTFIX || k == ps::PAREN) {
        expr_types(pool, scope, n.a);
    } else if (k == ps::INDEX || k == ps::BINARY || k == ps::ASSIGN) {
        expr_types(pool, scope, n.a);
        expr_types(pool, scope, n.b);
    } else if (k == ps::COND) {
        expr_types(pool, scope, n.a);
        expr_types(pool, scope, n.b);
        expr_types(pool, scope, n.c);
    } else if (k == ps::CALL) {
        if (n.a != ps::NONE) {
            expr_types(pool, scope, n.a);
        }
        for (uint32_t x : ast.items(n)) {
            expr_types(pool, scope, x);
        }
    } else if (k == ps::CTOR_CALL) {
        for (uint32_t x : ast.items(n)) {
            expr_types(pool, scope, x);
        }
    }
    // LITERAL, NAME, THIS, SUPER carry no type
}

// ---------------------------------------------------------------------------
// step 3: names, and the rows
// ---------------------------------------------------------------------------

bool has_code(const ps::Ast& ast, uint32_t m) {
    const ps::Node& n = ast.nodes[m];
    if (n.b == ps::NONE) {
        return false;
    }
    if (n.flags & ps::F_CTOR) {
        return true;
    }
    return ast.nodes[n.b].count > 0;
}

uint32_t max_live(const ps::Ast& ast, uint32_t s) {
    const ps::Node& n = ast.nodes[s];
    const uint16_t k = n.kind;
    if (k == ps::BLOCK) {
        return max_live_list(ast, ast.items(n));
    }
    if (k == ps::SWITCH) {
        uint32_t best = 0;
        for (uint32_t g : ast.items(n)) {
            const uint32_t live = max_live_list(ast, ast.items(g));
            if (live > best) {
                best = live;
            }
        }
        return best;
    }
    if (k == ps::LOCAL_VAR) {
        return n.count;
    }
    if (k == ps::IF) {
        uint32_t live = max_live(ast, n.b);
        if (n.c != ps::NONE) {
            const uint32_t m = max_live(ast, n.c);
            if (m > live) {
                live = m;
            }
        }
        return live;
    }
    if (k == ps::WHILE) {
        return max_live(ast, n.b);
    }
    if (k == ps::DO) {
        return max_live(ast, n.a);
    }
    if (k == ps::FOR) {
        uint32_t live = 0;
        if (n.c != ps::NONE && ast.nodes[n.c].kind == ps::LOCAL_VAR) {
            live = ast.nodes[n.c].count;
        }
        return live + max_live(ast, n.b);
    }
    return 0;
}

uint32_t max_live_list(const ps::Ast& ast, ps::Items stmts) {
    uint32_t live = 0;
    uint32_t best = 0;
    for (uint32_t s : stmts) {
        const ps::Node& n = ast.nodes[s];
        if (n.kind == ps::LOCAL_VAR) {
            live += n.count;
            if (live > best) {
                best = live;
            }
        } else {
            const uint32_t m = live + max_live(ast, s);
            if (m > best) {
                best = m;
            }
        }
    }
    return best;
}

uint32_t locals_word(const ps::Ast& ast, uint32_t m) {
    if (!has_code(ast, m)) {
        return 0;
    }
    const ps::Node& n = ast.nodes[m];
    const uint32_t base = n.flags & ps::ACC_STATIC ? 0 : 1;
    return base + n.count + max_live(ast, n.b);
}

Rows names_and_rows(Pool& pool, const ps::Ast& ast, uint32_t class_node, const ClassInfo& info) {
    Rows rows;
    const ps::Node& cls = ast.nodes[class_node];
    rows.interface_flag = cls.flags & ps::F_INTERFACE ? 1 : 0;
    rows.name = pool.cls(info.name);
    if (info.has_super) {
        rows.super = pool.cls(info.super);
    }
    for (const std::string& i : info.interfaces) {
        rows.interfaces.push_back(pool.cls(i));
    }
    size_t fi = 0;
    size_t mi = 0;
    for (uint32_t member : ast.items(cls)) {
        const ps::Node& m = ast.nodes[member];
        const uint32_t flags = m.flags & ps::ACC_MASK;
        const bool is_static = (flags & ps::ACC_STATIC) != 0;
        if (m.kind == ps::FIELD_DECL) {
            for (uint32_t item : ast.items(m)) {
                const ps::Node& d = ast.nodes[item];
                const classes::FieldInfo& f = info.fields[fi];
                fi += 1;
                uint32_t tree = NONE;
                if (d.a != ps::NONE) {
                    tree = static_cast<uint32_t>(rows.owners.size());
                    rows.owners.push_back(TreeOwner{item, member, true, is_static});
                }
                // two statements: the name is interned before the descriptor
                const uint32_t name = pool.utf8(f.name);
                const uint32_t desc = pool.utf8(f.desc);        // in the pool since step 2
                const FieldRow row = {flags, name, desc, tree};
                if (is_static) {
                    rows.static_fields.push_back(row);
                } else {
                    rows.instance_fields.push_back(row);
                }
            }
        } else {
            const classes::MethodInfo& f = info.methods[mi];
            mi += 1;
            // two statements: the descriptor is interned before the name
            const uint32_t desc = pool.utf8(f.desc);
            const uint32_t name = pool.utf8(f.name);
            uint32_t tree = NONE;
            if (!(flags & ps::ACC_ABSTRACT)) {
                tree = static_cast<uint32_t>(rows.owners.size());
                rows.owners.push_back(TreeOwner{member, member, false, is_static});
            }
            const MethodRow row = {flags, name, desc, tree, locals_word(ast, member)};
            if (is_static) {
                rows.static_methods.push_back(row);
            } else {
                rows.instance_methods.push_back(row);
            }
        }
    }
    return rows;
}

ClassBuild declare_class(UnitScope& scope, uint32_t class_node, const ClassInfo& info,
                         bool first_in_file) {
    ClassBuild build;
    build.node = class_node;
    build.info = &info;
    build.first_in_file = first_in_file;
    Pool& pool = build.pool;
    seed(pool, info);
    const uint32_t n_seed = static_cast<uint32_t>(pool.entries.size());
    type_walk(pool, scope, class_node, info, first_in_file);
    const uint32_t n_types = static_cast<uint32_t>(pool.entries.size());
    build.rows = names_and_rows(pool, scope.ast, class_node, info);
    build.pool_steps.push_back(n_seed);
    build.pool_steps.push_back(n_types);
    build.pool_steps.push_back(static_cast<uint32_t>(pool.entries.size()));
    return build;
}

}  // namespace decl
}  // namespace slrrc
