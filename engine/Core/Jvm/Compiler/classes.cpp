// classes.cpp - what the compiler knows about classes.  (Reference:
// slrrc/classes.py; the functions stand in the order of that file and carry
// its names.)
//
// WHERE THE C++ DIFFERS IN FORM FROM THE REFERENCE
//
//   - "None" for a text.  ClassInfo::has_super (super is None), Resolved::found
//     and the nullptr of resolve_type (a name that resolves nowhere),
//     ArgType::known (an argument without a type), the bool of class_of_desc.
//   - The dicts of UnitScope are ordered maps that are looked up with a view of
//     the key.  No map is walked: nothing depends on its order.
//   - own_by_simple and own_by_full hold the index of the class in `own`, not
//     the object: `own` is a vector that grows while they are filled.  Behind
//     the first loop of declare_unit() it never grows again, so a pointer to
//     an element (UnitScope::find, the chains) stays good; the member caches
//     are filled only after the members of every own class are complete.
//   - class_chain() keeps the name it is at in a local text.  The reference
//     holds the object; here the name of the next class is a field of a
//     ClassInfo of the provider, and the question that follows goes to the
//     provider.
#include "classes.hpp"

#include <utility>

#include "parser.hpp"
#include "scanner.hpp"

namespace slrrc {
namespace classes {

namespace sc = scanner;
namespace ps = parser;

std::string declaration_text(const ClassInfo& info) {
    std::string out("class ");
    out += info.name;
    out += "\nsuper ";
    if (info.has_super) {
        out += info.super;
    } else {
        out += '-';
    }
    out += "\ninterface ";
    out += info.is_interface ? '1' : '0';
    out += '\n';
    for (const std::string& i : info.interfaces) {
        out += "implements ";
        out += i;
        out += '\n';
    }
    for (const FieldInfo& f : info.fields) {
        if (f.is_static) {
            out += "field s ";
            out += f.name;
            out += ' ';
            out += f.desc;
            out += '\n';
        }
    }
    for (const FieldInfo& f : info.fields) {
        if (!f.is_static) {
            out += "field i ";
            out += f.name;
            out += ' ';
            out += f.desc;
            out += '\n';
        }
    }
    for (const MethodInfo& m : info.methods) {
        std::string_view desc = m.desc;
        if (m.name == INIT) {
            desc = params_part(desc);
        }
        out += "method ";
        out += m.is_static ? 's' : 'i';
        out += ' ';
        out += m.name;
        out += ' ';
        out += desc;
        out += '\n';
    }
    return out;
}

std::string_view params_part(std::string_view desc) {
    const size_t n = desc.size();
    size_t i = 0;
    while (i < n && desc[i] != ')') {
        i += 1;
    }
    // desc[:i + 1]: without a ")" that is the whole text
    return i < n ? desc.substr(0, i + 1) : desc;
}

// ---------------------------------------------------------------------------
// descriptors
// ---------------------------------------------------------------------------

size_t type_desc_end(std::string_view desc, size_t i) {
    const size_t n = desc.size();
    while (i < n && desc[i] == '[') {
        i += 1;
    }
    if (i >= n) {
        return 0;
    }
    const char ch = desc[i];
    if (ch == 'I' || ch == 'F') {
        return i + 1;
    }
    if (ch == 'L') {
        size_t j = i + 1;
        while (j < n && desc[j] != ';') {
            j += 1;
        }
        if (j >= n) {
            return 0;
        }
        return j + 1;
    }
    return 0;
}

uint32_t desc_kind(std::string_view desc) {
    const size_t n = desc.size();
    if (n == 0) {
        return DESC_BAD;
    }
    if (desc[0] != '(') {
        if (type_desc_end(desc, 0) == n) {
            return DESC_TYPE;
        }
        return DESC_BAD;
    }
    size_t i = 1;
    while (i < n && desc[i] != ')') {
        i = type_desc_end(desc, i);
        if (i == 0) {
            return DESC_BAD;
        }
    }
    if (i >= n) {
        return DESC_BAD;                // no ")"
    }
    i += 1;
    if (i == n) {
        return DESC_CTOR;
    }
    if (i + 1 == n && desc[i] == 'V') {
        return DESC_METHOD;
    }
    if (type_desc_end(desc, i) == n) {
        return DESC_METHOD;
    }
    return DESC_BAD;
}

std::string check_class_info(const ClassInfo& info, std::string_view fqn) {
    if (info.name != fqn) {
        return "it carries the name \"" + info.name + "\"";
    }
    if (info.has_super && info.super.empty()) {
        return "its superclass has an empty name";
    }
    for (const std::string& i : info.interfaces) {
        if (i.empty()) {
            return "one of its interfaces has an empty name";
        }
    }
    for (const FieldInfo& f : info.fields) {
        if (f.name.empty()) {
            return "a field has an empty name";
        }
        if (desc_kind(f.desc) != DESC_TYPE) {
            return "field " + f.name + " has the descriptor \"" + f.desc + "\"";
        }
    }
    for (const MethodInfo& m : info.methods) {
        if (m.name.empty()) {
            return "a method has an empty name";
        }
        const uint32_t k = desc_kind(m.desc);
        if (m.name == INIT) {
            // with or without a return part: the compiler reads neither
            if (k != DESC_CTOR && k != DESC_METHOD) {
                return "a constructor has the descriptor \"" + m.desc + "\"";
            }
        } else if (k != DESC_METHOD) {
            return "method " + m.name + " has the descriptor \"" + m.desc + "\"";
        }
    }
    return std::string();
}

// ---------------------------------------------------------------------------
// the scope of one compilation
// ---------------------------------------------------------------------------

UnitScope::UnitScope(const parser::Ast& unit_ast, ClassProvider& class_provider,
                     Diagnostics& diagnostics)
    : ast(unit_ast), provider(class_provider), diags(diagnostics) {}

const ClassInfo* UnitScope::own_class(std::string_view fqn) const {
    const NameMap<uint32_t>::const_iterator hit = own_by_full.find(fqn);
    if (hit == own_by_full.end()) {
        return nullptr;
    }
    return &own[hit->second];
}

const ClassInfo* UnitScope::find(std::string_view fqn) {
    const ClassInfo* info = own_class(fqn);
    if (info != nullptr) {
        return info;
    }
    const NameMap<const ClassInfo*>::const_iterator known = answers.find(fqn);
    if (known != answers.end()) {
        return known->second;
    }
    info = provider.find(fqn);
    if (info != nullptr) {
        const std::string why = check_class_info(*info, fqn);
        if (!why.empty()) {
            std::string text("class ");
            text += fqn;
            text += " as the provider declares it cannot be used: ";
            text += why;
            text += "; it is treated as not found";
            diags.warning(W_CLASS, 1, 1, std::move(text));
            info = nullptr;
        }
    }
    answers.emplace(std::string(fqn), info);
    if (info != nullptr) {
        consulted.push_back(std::string(fqn));
    } else {
        missed.push_back(std::string(fqn));
    }
    return info;
}

namespace {

// Full name of a type name that has not been looked up yet; false when it is
// unknown.
bool lookup(UnitScope& scope, std::string_view name, std::string& full) {
    if (name.find('.') != std::string_view::npos) {
        // written with its package (no shipped source does): taken as it is
        if (scope.find(name) != nullptr) {
            full = name;
            return true;
        }
        return false;
    }
    const NameMap<uint32_t>::const_iterator own = scope.own_by_simple.find(name);
    if (own != scope.own_by_simple.end()) {
        full = scope.own[own->second].name;
        return true;
    }
    if (!scope.package.empty()) {
        full = scope.package;
        full += '.';
        full += name;
    } else {
        full = name;
    }
    if (scope.find(full) != nullptr) {
        return true;
    }
    for (const Import& imp : scope.imports) {
        if (imp.is_star) {
            full = imp.name;
            full += '.';
            full += name;
        } else if (imp.name == name ||
                   (imp.name.size() > name.size() &&
                    imp.name[imp.name.size() - name.size() - 1] == '.' &&
                    std::string_view(imp.name).substr(imp.name.size() - name.size()) == name)) {
            // imp == name or imp.endswith('.' + name)
            full = imp.name;
        } else {
            continue;
        }
        if (scope.find(full) != nullptr) {
            return true;
        }
    }
    full = "java.lang.";
    full += name;
    if (scope.find(full) != nullptr) {
        return true;
    }
    return false;
}

// Full name of a superclass or an interface; an unknown one is an error in
// either mode (and the name is then empty).
std::string class_name(UnitScope& scope, const ps::Node& type_node, const char* what) {
    const ps::Ast& ast = scope.ast;
    const std::string name = ast.type_name(type_node);
    const std::string* full = resolve_type(scope, name);
    if (full == nullptr) {
        const sc::Token& t = ast.toks[type_node.tok0];
        std::string text("unknown ");
        text += what;
        text += ' ';
        text += name;
        scope.diags.error(E_TYPE, t.line, sc::column_at(ast.source, t.pos), std::move(text));
        return std::string();
    }
    return *full;
}

}  // namespace

const std::string* resolve_type(UnitScope& scope, std::string_view name) {
    const NameMap<Resolved>::const_iterator known = scope.resolved.find(name);
    if (known != scope.resolved.end()) {
        return known->second.found ? &known->second.full : nullptr;
    }
    Resolved r;
    r.found = lookup(scope, name, r.full);
    if (!r.found) {
        r.full.clear();
    }
    const NameMap<Resolved>::const_iterator at =
        scope.resolved.emplace(std::string(name), std::move(r)).first;
    return at->second.found ? &at->second.full : nullptr;
}

std::string descriptor(UnitScope& scope, const ps::Node& type_node, uint32_t extra_dims) {
    const ps::Ast& ast = scope.ast;
    const uint32_t tok = type_node.tok0;
    const uint8_t k = ast.toks[tok].kind;
    std::string out(static_cast<size_t>(type_node.dims()) + extra_dims, '[');
    if (k == sc::KW_INT) {
        out += 'I';
    } else if (k == sc::KW_FLOAT) {
        out += 'F';
    } else if (k == sc::KW_VOID) {
        out += 'V';
    } else {
        const std::string name = ast.type_name(type_node);
        const std::string* full = resolve_type(scope, name);
        if (full == nullptr) {
            if (scope.reported.find(name) == scope.reported.end()) {
                scope.reported.insert(name);
                const sc::Token& t = ast.toks[tok];
                scope.diags.unresolved_type(t.line, sc::column_at(ast.source, t.pos), name);
            }
            out += "L;";
        } else {
            out += 'L';
            out += *full;
            out += ';';
        }
    }
    return out;
}

std::string method_descriptor(UnitScope& scope, const ps::Node& method_node) {
    const ps::Ast& ast = scope.ast;
    std::string d("(");
    for (uint32_t p : ast.items(method_node)) {
        const ps::Node& param = ast.nodes[p];
        d += descriptor(scope, ast.nodes[param.a], param.dims());
    }
    d += ')';
    if (method_node.a != ps::NONE) {
        d += descriptor(scope, ast.nodes[method_node.a], 0);
    }
    return d;
}

void report_cycle(UnitScope& scope, std::string_view name) {
    if (scope.cycles.find(name) != scope.cycles.end()) {
        return;
    }
    scope.cycles.insert(std::string(name));
    const ps::Ast& ast = scope.ast;
    const sc::Token& t = ast.toks[scope.own_name_toks.find(name)->second];
    std::string text("class ");
    text += name;
    text += " is its own superclass";
    scope.diags.error(E_TYPE, t.line, sc::column_at(ast.source, t.pos), std::move(text));
}

std::unique_ptr<UnitScope> declare_unit(const ps::Ast& ast, ClassProvider& provider,
                                        Diagnostics& diags) {
    std::unique_ptr<UnitScope> owner(new UnitScope(ast, provider, diags));
    UnitScope& scope = *owner;
    const ps::Node& unit = ast.nodes[ast.root];
    if (unit.a != ps::NONE) {
        const ps::Node& q = ast.nodes[unit.a];
        scope.package = ast.dotted(q.tok0, q.tok1);
    }
    std::vector<uint32_t> nodes;
    for (uint32_t index : ast.items(unit)) {
        const ps::Node& n = ast.nodes[index];
        if (n.kind == ps::IMPORT) {
            // "import parts:part.root.BodyPart.*;" (2 sources) names nothing
            if (!(n.flags & ps::F_COLON)) {
                const ps::Node& q = ast.nodes[n.a];
                scope.imports.push_back(Import{ast.dotted(q.tok0, q.tok1), (n.flags & ps::F_STAR) != 0});
            }
            continue;
        }
        const std::string_view simple = ast.text(n.name_tok);
        if (scope.own_by_simple.find(simple) != scope.own_by_simple.end()) {
            const sc::Token& t = ast.toks[n.name_tok];
            std::string text("type ");
            text += simple;
            text += " declared twice in one source";
            diags.error(E_UNSUPPORTED, t.line, sc::column_at(ast.source, t.pos), std::move(text));
            continue;
        }
        std::string full;
        if (!scope.package.empty()) {
            full = scope.package;
            full += '.';
        }
        full += simple;
        const uint32_t at = static_cast<uint32_t>(scope.own.size());
        scope.own.push_back(ClassInfo(full));
        scope.own_by_simple.emplace(std::string(simple), at);
        // (two simple names give two full names: nothing is replaced here)
        scope.own_by_full.emplace(full, at);
        scope.own_name_toks.emplace(std::move(full), n.name_tok);
        nodes.push_back(index);
    }
    // every class name the source writes as a type, wherever it stands (the
    // emitter tells a variable of such a name from the class)
    for (const ps::Node& n : ast.nodes) {
        if (n.kind == ps::TYPE && !ps::is_primitive(ast, n)) {
            scope.type_names.insert(ast.type_name(n));
        }
    }
    for (size_t k = 0; k < nodes.size(); ++k) {
        const ps::Node& n = ast.nodes[nodes[k]];
        ClassInfo& info = scope.own[k];
        info.is_interface = (n.flags & ps::F_INTERFACE) != 0;
        if (n.a != ps::NONE) {
            info.super = class_name(scope, ast.nodes[n.a], "superclass");
            info.has_super = true;
        } else if (!info.is_interface && info.name != OBJECT) {
            info.super = OBJECT;
            info.has_super = true;
        }
        if (n.b != ps::NONE) {
            for (uint32_t t : ast.items(n.b)) {
                info.interfaces.push_back(class_name(scope, ast.nodes[t], "interface"));
            }
        }
        for (uint32_t member : ast.items(n)) {
            const ps::Node& m = ast.nodes[member];
            const bool is_static = (m.flags & ps::ACC_STATIC) != 0;
            if (m.kind == ps::FIELD_DECL) {
                for (uint32_t d : ast.items(m)) {
                    const ps::Node& decl = ast.nodes[d];
                    std::string desc = descriptor(scope, ast.nodes[m.a], decl.dims());
                    info.fields.push_back(
                        FieldInfo{std::string(ast.text(decl.name_tok)), std::move(desc), is_static});
                }
            } else {
                std::string name(m.flags & ps::F_CTOR ? std::string_view(INIT) : ast.text(m.name_tok));
                std::string desc = method_descriptor(scope, m);
                info.methods.push_back(MethodInfo{std::move(name), std::move(desc), is_static});
            }
        }
    }
    // The superclasses of every class of the source are walked once, here: a
    // class that is its own superclass, directly or through classes of the
    // source or of the provider, is an error whatever its bodies look up
    // (class_chain reports it).  Not when a superclass is unknown: that is an
    // error already, and its name is empty.
    if (diags.errors == 0) {
        for (size_t k = 0; k < scope.own.size(); ++k) {
            class_chain(scope, scope.own[k].name);
        }
    }
    return owner;
}

std::vector<uint32_t> class_nodes(const ps::Ast& ast) {
    std::vector<uint32_t> out;
    for (uint32_t n : ast.items(ast.root)) {
        if (ast.nodes[n].kind == ps::CLASS) {
            out.push_back(n);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// member lookup
// ---------------------------------------------------------------------------

const std::vector<const ClassInfo*>& class_chain(UnitScope& scope, std::string_view name) {
    const NameMap<std::vector<const ClassInfo*>>::const_iterator known = scope.chains.find(name);
    if (known != scope.chains.end()) {
        return known->second;
    }
    std::vector<const ClassInfo*> chain;
    NameSet seen;
    std::string n(name);
    bool has_n = true;                  // n is not None
    while (has_n && seen.find(n) == seen.end()) {
        const ClassInfo* info = scope.find(n);
        if (info == nullptr) {
            break;
        }
        seen.insert(n);
        chain.push_back(info);
        has_n = info->has_super;
        if (has_n) {
            n = info->super;
        }
    }
    if (has_n && seen.find(n) != seen.end()) {
        // the ring is the part of the chain from the class named n on
        bool in_ring = false;
        for (const ClassInfo* info : chain) {
            if (info->name == n) {
                in_ring = true;
            }
            if (in_ring && scope.own_by_full.find(info->name) != scope.own_by_full.end()) {
                report_cycle(scope, info->name);
            }
        }
    }
    return scope.chains.emplace(std::string(name), std::move(chain)).first->second;
}

namespace {

const NameMap<const FieldInfo*>& field_map(UnitScope& scope, const ClassInfo& info) {
    const NameMap<NameMap<const FieldInfo*>>::const_iterator known = scope.field_maps.find(info.name);
    if (known != scope.field_maps.end()) {
        return known->second;
    }
    NameMap<const FieldInfo*> m;
    for (const FieldInfo& f : info.fields) {
        m.emplace(f.name, &f);          // the first field of a name stays
    }
    return scope.field_maps.emplace(info.name, std::move(m)).first->second;
}

// Method name -> the methods of that name in the order the lookup scans
// them: the instance methods from the last to the first, then the static
// methods from the last to the first.
const NameMap<std::vector<const MethodInfo*>>& method_map(UnitScope& scope, const ClassInfo& info) {
    const NameMap<NameMap<std::vector<const MethodInfo*>>>::const_iterator known =
        scope.method_maps.find(info.name);
    if (known != scope.method_maps.end()) {
        return known->second;
    }
    NameMap<std::vector<const MethodInfo*>> m;
    size_t k = info.methods.size();
    while (k > 0) {
        k -= 1;
        const MethodInfo& x = info.methods[k];
        if (!x.is_static) {
            m[x.name].push_back(&x);
        }
    }
    k = info.methods.size();
    while (k > 0) {
        k -= 1;
        const MethodInfo& x = info.methods[k];
        if (x.is_static) {
            m[x.name].push_back(&x);
        }
    }
    return scope.method_maps.emplace(info.name, std::move(m)).first->second;
}

// desc[1:len(desc) - 1]: the name inside "L" name ";".
std::string_view inside(std::string_view desc) {
    if (desc.size() < 2) {
        return std::string_view();
    }
    return desc.substr(1, desc.size() - 2);
}

}  // namespace

FieldHit find_field(UnitScope& scope, std::string_view cls, std::string_view name) {
    for (const ClassInfo* info : class_chain(scope, cls)) {
        const NameMap<const FieldInfo*>& m = field_map(scope, *info);
        const NameMap<const FieldInfo*>::const_iterator f = m.find(name);
        if (f != m.end()) {
            return FieldHit{f->second, &info->name};
        }
    }
    return FieldHit();
}

std::vector<std::string_view> split_params(std::string_view desc) {
    std::vector<std::string_view> out;
    const size_t n = desc.size();
    size_t i = 1;
    while (i < n && desc[i] != ')') {
        const size_t j = type_desc_end(desc, i);
        if (j == 0) {
            break;                      // no descriptor: not reached, see the header
        }
        out.push_back(desc.substr(i, j - i));
        i = j;
    }
    return out;
}

bool class_of_desc(std::string_view desc, std::string& cls) {
    if (desc.empty()) {
        return false;
    }
    const size_t n = desc.size();
    if (desc[0] == 'L' && desc[n - 1] == ';') {
        cls = desc.substr(1, n - 2);
        return true;
    }
    if (desc[0] == '[') {
        cls = "[";
        if (n >= 3 && desc[1] == 'L' && desc[n - 1] == ';') {
            cls += desc.substr(2, n - 3);
        } else {
            cls += desc.substr(1);
        }
        return true;
    }
    return false;
}

uint32_t distance(UnitScope& scope, std::string_view arg_cls, std::string_view par_cls) {
    uint32_t d = 0;
    for (const ClassInfo* info : class_chain(scope, arg_cls)) {
        if (info->name == par_cls) {
            return d;
        }
        for (const std::string& i : info->interfaces) {
            if (i == par_cls) {
                return d + 1;
            }
        }
        d += 1;
    }
    return NO_FIT;
}

uint32_t type_score(UnitScope& scope, std::string_view par, const ArgType& arg) {
    if (arg.known && par == arg.desc) {
        return 0;
    }
    if (!arg.known) {
        return NO_FIT;
    }
    if (par.empty() || arg.desc.empty()) {
        // Not reached: a parameter is a type descriptor (check_class_info), an
        // argument a descriptor or NULL_TYPE.  (The reference would raise.)
        return NO_FIT;
    }
    const char p0 = par[0];
    // null, and a value whose type name resolved nowhere ("L;"), fit every
    // object or array parameter
    char a0;
    if (arg.desc == NULL_TYPE || arg.desc == UNKNOWN_DESC) {
        a0 = 'N';
    } else {
        a0 = arg.desc[0];
    }
    if (p0 == 'F') {
        if (a0 == 'I') {
            return 101;
        }
        return NO_FIT;
    }
    if (p0 == 'I') {
        if (a0 == 'F') {
            return 100;
        }
        return NO_FIT;
    }
    if (p0 == 'L' || p0 == '[') {
        if (a0 == 'I' || a0 == 'F') {
            if (par == STRING_DESC) {
                return 100;
            }
            return NO_FIT;
        }
        if (a0 == 'N') {
            return 100;
        }
        if (a0 == 'L' || a0 == '[') {
            if (p0 == '[' || a0 == '[') {
                // an array is passed for its own type (score 0 above) or for
                // java.lang.Object
                if (par == OBJECT_DESC) {
                    return 1;
                }
                return NO_FIT;
            }
            return distance(scope, inside(arg.desc), inside(par));
        }
    }
    return NO_FIT;
}

MethodHit lookup_method(UnitScope& scope, std::string_view cls, std::string_view name,
                        const std::vector<ArgType>& arg_types) {
    MethodHit hit;
    const size_t argc = arg_types.size();
    for (const ClassInfo* info : class_chain(scope, cls)) {
        const NameMap<std::vector<const MethodInfo*>>& methods = method_map(scope, *info);
        const NameMap<std::vector<const MethodInfo*>>::const_iterator cands = methods.find(name);
        if (cands == methods.end()) {
            continue;
        }
        for (const MethodInfo* m : cands->second) {
            const std::vector<std::string_view> params = split_params(m->desc);
            if (params.size() != argc) {
                continue;
            }
            // (a sum of scores: 101 at most per argument, and a source has
            // fewer than 2**31 bytes, so 32 bits hold it)
            uint32_t score = 0;
            bool fits = true;
            size_t k = 0;
            while (k < argc) {
                const uint32_t s = type_score(scope, params[k], arg_types[k]);
                if (s == NO_FIT) {
                    fits = false;
                    break;
                }
                score += s;
                k += 1;
            }
            if (!fits) {
                continue;
            }
            if (score == 0) {
                hit.method = m;
                hit.score = 0;
                hit.tie = false;
                return hit;
            }
            if (score < hit.score) {
                hit.method = m;
                hit.score = score;
                hit.tie = false;
            } else if (score == hit.score && hit.method != nullptr && m->desc != hit.method->desc) {
                // (hit.method is tested because a score can be NO_METHOD
                // itself, with 10,000 arguments of 100 each; the reference
                // raises there, see notes/port_2-declarations.md)
                hit.tie = true;
            }
        }
    }
    return hit;
}

const MethodInfo* find_method(UnitScope& scope, std::string_view cls, std::string_view name,
                              const std::vector<ArgType>& arg_types) {
    return lookup_method(scope, cls, name, arg_types).method;
}

bool has_method(UnitScope& scope, std::string_view cls, std::string_view name, size_t argc) {
    for (const ClassInfo* info : class_chain(scope, cls)) {
        const NameMap<std::vector<const MethodInfo*>>& methods = method_map(scope, *info);
        const NameMap<std::vector<const MethodInfo*>>::const_iterator cands = methods.find(name);
        if (cands == methods.end()) {
            continue;
        }
        for (const MethodInfo* m : cands->second) {
            if (split_params(m->desc).size() == argc) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace classes
}  // namespace slrrc
