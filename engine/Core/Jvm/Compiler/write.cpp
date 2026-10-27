// write.cpp - the writer (slrrc/write.py, function by function).
#include "write.hpp"

#include <string>
#include <string_view>

#include "classes.hpp"
#include "emit.hpp"
#include "pool.hpp"

namespace slrrc {
namespace write {

namespace {

using emit::SymNode;
using emit::Tree;

constexpr uint32_t NONE = 0xFFFFFFFF;

// ---------------------------------------------------------------------------
// step 4: resolve
// ---------------------------------------------------------------------------

// The static types of the locals a tree starts with: `this` (not in static
// code; "" stands for it here), then the parameters in reverse order.
// has_desc false: a field initialiser.  A slot without a type is `unknown`.
struct LocalType {
    bool known = false;
    std::string desc;
};

std::vector<LocalType> frame_types(bool is_static, bool has_desc, std::string_view desc) {
    std::vector<LocalType> types;
    if (!is_static) types.push_back(LocalType{true, std::string()});
    if (has_desc) {
        const std::vector<std::string_view> params = classes::split_params(desc);
        size_t k = params.size();
        while (k > 0) {             // the last parameter first
            --k;
            types.push_back(LocalType{true, std::string(params[k])});
        }
    }
    return types;
}

// A member is about to be looked up on a value of this static type: the class
// entry of the type comes first.  Only a class type has one here.
void touch_receiver(pool::Pool& pool, const LocalType& t) {
    if (!t.known) return;
    const std::string& d = t.desc;
    const size_t n = d.size();
    if (n >= 2 && d[0] == 'L' && d[n - 1] == ';') pool.cls(std::string_view(d).substr(1, n - 2));
}

void resolve_tree(pool::Pool& pool, const Tree& tree, bool is_static, bool has_desc,
                  std::string_view desc, const std::string& own_type) {
    const std::vector<SymNode>& nodes = tree.nodes;
    const std::vector<emit::Operand>& ops = tree.operands;
    const size_t n = nodes.size();
    std::vector<LocalType> types = frame_types(is_static, has_desc, desc);
    const bool has_this = !is_static;
    size_t i = 0;
    while (i < n) {
        const SymNode& nd = nodes[i];
        const uint8_t op = nd.op;
        if (op == emit::OP_DECLARE) {
            // declare k, type T: local k has the static type T from here on
            if (i + 1 < n && nodes[i + 1].op == emit::OP_TYPE) {
                const uint32_t k = nd.value;
                while (types.size() <= k) types.push_back(LocalType{});
                types[k] = LocalType{true, ops[nodes[i + 1].value].text};
            }
            ++i;
            continue;
        }
        if (op == emit::OP_PATH || op == emit::OP_CALL) {
            // a name path, or a call followed by its callee
            size_t j = i + 1;
            size_t count = 0;
            if (op == emit::OP_CALL) {
                if (j < n && nodes[j].op == emit::OP_METHOD) {
                    const emit::Operand& o = ops[nodes[j].value];
                    pool.ref(o.text, o.name, o.desc);
                    i = j + 1;
                    continue;
                }
                if (j < n && nodes[j].op == emit::OP_PATH) {
                    count = nodes[j].value;
                    ++j;
                } else {
                    ++i;
                    continue;
                }
            } else {
                count = nd.value;
            }
            size_t end = j + count;
            if (end > n) end = n;
            LocalType reached;                      // static type of the path so far
            for (size_t k = j; k < end; ++k) {
                const SymNode& seg = nodes[k];
                const uint8_t sop = seg.op;
                const bool first = k == j;
                if (sop == emit::OP_LOCAL) {
                    if (has_this && seg.value == 0) {
                        reached = LocalType{true, own_type};
                        pool.utf8(own_type);
                    } else if (seg.value < types.size()) {
                        reached = types[seg.value];
                    } else {
                        reached = LocalType{};
                    }
                } else if (sop == emit::OP_CLASS) {
                    pool.cls(ops[seg.value].text);
                    reached = LocalType{};
                } else if (sop == emit::OP_FIELD) {
                    if (!first) touch_receiver(pool, reached);
                    const emit::Operand& o = ops[seg.value];
                    pool.ref(o.text, o.name, o.desc);
                    reached = LocalType{true, o.desc};
                } else if (sop == emit::OP_METHOD) {
                    if (!first) touch_receiver(pool, reached);
                    const emit::Operand& o = ops[seg.value];
                    pool.ref(o.text, o.name, o.desc);
                    reached = LocalType{};
                } else if (sop == emit::OP_NAME) {
                    if (!first) touch_receiver(pool, reached);
                    reached = LocalType{};
                }
            }
            i = j + count;
            continue;
        }
        if (op == emit::OP_CLASS) {
            pool.cls(ops[nd.value].text);
        } else if (op == emit::OP_METHOD || op == emit::OP_FIELD) {
            const emit::Operand& o = ops[nd.value];
            pool.ref(o.text, o.name, o.desc);
        } else if (op == emit::OP_LOCAL && has_this && nd.value == 0) {
            pool.utf8(own_type);
        }
        ++i;
    }
}

// Step 4 for a class: the trees by row vector.
void resolve_pass(decl::ClassBuild& build) {
    pool::Pool& pool = build.pool;
    const decl::Rows& rows = build.rows;
    const std::string own_type = "L" + build.info->name + ";";
    for (const decl::MethodRow& r : rows.static_methods)
        if (r.tree != NONE)
            resolve_tree(pool, build.trees[r.tree], true, true, pool.entries[r.desc].text, own_type);
    for (const decl::MethodRow& r : rows.instance_methods)
        if (r.tree != NONE)
            resolve_tree(pool, build.trees[r.tree], false, true, pool.entries[r.desc].text, own_type);
    for (const decl::FieldRow& r : rows.static_fields)
        if (r.tree != NONE) resolve_tree(pool, build.trees[r.tree], true, false, {}, own_type);
    for (const decl::FieldRow& r : rows.instance_fields)
        if (r.tree != NONE) resolve_tree(pool, build.trees[r.tree], false, false, {}, own_type);
}

// ---------------------------------------------------------------------------
// step 5: emit
// ---------------------------------------------------------------------------

void emit_pass(decl::ClassBuild& build) {
    pool::Pool& pool = build.pool;
    for (const Tree& tree : build.trees) {
        for (const SymNode& nd : tree.nodes) {
            const uint8_t op = nd.op;
            if (op == emit::OP_STR) {
                pool.utf8(tree.operands[nd.value].text);
            } else if (op == emit::OP_NAME || op == emit::OP_GETFIELD || op == emit::OP_CALL_ON ||
                       op == emit::OP_CALL_SUPER) {
                pool.utf8(tree.operands[nd.value].text);
            } else if (op == emit::OP_RID) {
                const emit::Operand& o = tree.operands[nd.value];
                pool.rid(o.text, o.number);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// bytes
// ---------------------------------------------------------------------------

void put_u16(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void put_chunk(std::vector<uint8_t>& out, const char* tag, const std::vector<uint8_t>& payload) {
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>(tag[k]));
    put_u32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

std::vector<uint8_t> cons_payload(const pool::Pool& pool) {
    std::vector<uint8_t> out;
    put_u32(out, static_cast<uint32_t>(pool.entries.size()));
    for (const pool::PoolEntry& e : pool.entries) {
        put_u32(out, e.kind);
        if (e.kind == pool::UTF8) {
            put_u32(out, static_cast<uint32_t>(e.text.size()));
            out.insert(out.end(), e.text.begin(), e.text.end());
            out.push_back(0);
        } else if (e.kind == pool::CLASS) {
            put_u32(out, e.a);
        } else {                        // NAT, REF, RID: two words
            put_u32(out, e.a);
            put_u32(out, e.b);
        }
    }
    return out;
}

bool has_fild(const decl::Rows& rows) {
    return rows.static_fields.size() + rows.instance_fields.size() > 0;
}

bool has_mthd(const decl::Rows& rows) {
    return rows.static_methods.size() + rows.instance_methods.size() > 0;
}

void field_rows(std::vector<uint8_t>& out, const std::vector<decl::FieldRow>& rows) {
    put_u32(out, static_cast<uint32_t>(rows.size()));
    for (const decl::FieldRow& r : rows) {
        put_u32(out, r.flags);
        put_u32(out, r.name);
        put_u32(out, r.type);
        put_u32(out, r.tree);
    }
}

void method_rows(std::vector<uint8_t>& out, const std::vector<decl::MethodRow>& rows) {
    put_u32(out, static_cast<uint32_t>(rows.size()));
    for (const decl::MethodRow& r : rows) {
        put_u32(out, r.flags);
        put_u32(out, r.name);
        put_u32(out, r.desc);
        put_u32(out, r.tree);
        put_u32(out, r.locals);
    }
}

std::vector<uint8_t> clss_payload(const decl::Rows& rows) {
    std::vector<uint8_t> out;
    put_u32(out, 0xFFFFFFFF);
    put_u32(out, rows.interface_flag);
    put_u32(out, rows.name);
    put_u32(out, rows.super);
    put_u32(out, static_cast<uint32_t>(rows.interfaces.size()));
    for (uint32_t i : rows.interfaces) put_u32(out, i);
    return out;
}

// The u32 behind a node: the number, or the index of the pool entry its
// operand names (after finish_pool every such entry exists).
uint32_t operand_index(pool::Pool& pool, const Tree& tree, const SymNode& nd) {
    const uint8_t k = nd.kind;
    if (k == emit::K_IMM) return nd.value;
    const emit::Operand& o = tree.operands[nd.value];
    if (k == emit::K_UTF8) return pool.utf8(o.text);
    if (k == emit::K_CLASS) return pool.cls(o.text);
    if (k == emit::K_REF) return pool.ref(o.text, o.name, o.desc);
    return pool.rid(o.text, o.number);      // K_RID
}

std::vector<uint8_t> tree_payload(pool::Pool& pool, const std::vector<Tree>& trees) {
    std::vector<uint8_t> out;
    put_u32(out, static_cast<uint32_t>(trees.size()));
    for (const Tree& tree : trees) {
        put_u32(out, static_cast<uint32_t>(tree.nodes.size()));
        for (const SymNode& nd : tree.nodes) {
            out.push_back(nd.op);
            put_u16(out, nd.line);
            if (nd.kind != emit::K_NONE) put_u32(out, operand_index(pool, tree, nd));
        }
    }
    return out;
}

}  // namespace

void finish_pool(decl::ClassBuild& build) {
    resolve_pass(build);
    build.pool_steps.push_back(static_cast<uint32_t>(build.pool.entries.size()));
    emit_pass(build);
    build.pool_steps.push_back(static_cast<uint32_t>(build.pool.entries.size()));
}

std::vector<uint8_t> class_blob(decl::ClassBuild& build, uint32_t build_number) {
    const decl::Rows& rows = build.rows;
    // the trees first: CONS stands in front of TREE in the file, but only the
    // encoded trees prove that the pool is complete
    const std::vector<uint8_t> tree = tree_payload(build.pool, build.trees);
    std::vector<uint8_t> out;
    std::vector<uint8_t> number;
    put_u32(number, build_number);
    put_chunk(out, "TUFA", number);
    put_chunk(out, "CONS", cons_payload(build.pool));
    if (has_fild(rows)) {
        std::vector<uint8_t> p;
        field_rows(p, rows.static_fields);
        field_rows(p, rows.instance_fields);
        put_chunk(out, "FILD", p);
    }
    if (has_mthd(rows)) {
        std::vector<uint8_t> p;
        method_rows(p, rows.static_methods);
        method_rows(p, rows.instance_methods);
        put_chunk(out, "MTHD", p);
    }
    put_chunk(out, "CLSS", clss_payload(rows));
    put_chunk(out, "TREE", tree);
    return out;
}

std::vector<uint8_t> class_file(std::vector<decl::ClassBuild>& builds, uint32_t build_number) {
    std::vector<uint8_t> out;
    for (decl::ClassBuild& b : builds) {
        const std::vector<uint8_t> blob = class_blob(b, build_number);
        out.insert(out.end(), blob.begin(), blob.end());
    }
    return out;
}

}  // namespace write
}  // namespace slrrc
