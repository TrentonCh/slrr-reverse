// slrrc.cpp - the driver: the stages of a compilation and the entry points of
// the public interface.  (Reference: slrrc/__init__.py; the functions carry
// its names: _parse, _front, _emit, _finish, front, emit_trees,
// declared_names, declare_source.)
//
// WHERE THE C++ DIFFERS IN FORM FROM THE REFERENCE
//
//   - front() fills a FrontResult the caller made, and the Diagnostics of the
//     compilation is a member of it (compilation.hpp): the reference keeps
//     both alive by reference, and the scope refers to them.
//   - finish() copies the three lists for front() and emit_trees(), whose
//     scope lives on in the result, and moves them for declare(), whose scope
//     ends there.  It is the one place where either happens.
//   - A float_mode that is none of the four settings is an error of the
//     result (E_UNSUPPORTED, line 1, column 1), where the reference raises
//     ValueError: nothing is thrown across the interface.
//   - _text() and _bytes_only() have no counterpart: the compiler is given
//     bytes.  Neither has _too_deep(), the reference's net under the
//     interpreter's recursion limit: the one limit on nesting is
//     parser::MAX_DEPTH.
#include "slrrc.hpp"

#include <utility>

#include "compilation.hpp"
#include "emit.hpp"
#include "floatlit.hpp"
#include "packs.hpp"
#include "parser.hpp"
#include "scanner.hpp"
#include "write.hpp"

namespace slrrc {

namespace {

// Scan and parse; true when the source has a tree with a class in it.
bool parse(std::string_view source, Diagnostics& diags, FrontResult& res) {
    std::vector<scanner::Token> toks = scanner::scan(source, diags);
    if (diags.errors != 0) {
        return false;
    }
    res.ast = parser::parse(source, std::move(toks), diags);
    res.has_ast = true;
    if (diags.errors != 0) {
        return false;
    }
    if (classes::class_nodes(res.ast).empty()) {
        const std::vector<scanner::Token>& all = res.ast.toks;
        diags.error(E_SYNTAX, all[all.size() - 1].line, 1, "the source declares no class");
        return false;
    }
    return true;
}

// The work of front(): fills res.ast, res.scope and res.classes; true when
// there was no error.  Appends to res.diags; finish() copies.
bool front_stage(std::string_view source, classes::ClassProvider& provider, FrontResult& res) {
    Diagnostics& diags = res.diags;
    if (!parse(source, diags, res)) {
        return false;
    }
    res.scope = classes::declare_unit(res.ast, provider, diags);
    classes::UnitScope& scope = *res.scope;
    if (diags.errors != 0) {
        return false;
    }
    std::vector<decl::ClassBuild> builds;
    const std::vector<uint32_t> nodes = classes::class_nodes(res.ast);
    bool first = true;
    // (without an error every class node has its ClassInfo: zip() of the
    // reference pairs lists of one length)
    for (size_t k = 0; k < nodes.size() && k < scope.own.size(); ++k) {
        builds.push_back(decl::declare_class(scope, nodes[k], scope.own[k], first));
        first = false;
    }
    if (diags.errors != 0) {            // also: a type inside a body, in strict mode
        return false;
    }
    res.classes = std::move(builds);
    return true;
}

// The work of emit_trees() behind the front half: every ClassBuild of res
// gets its trees; true when there was no error.
bool emit_stage(FrontResult& res, packs::PackNames& pack_names, const Options& options) {
    packs::CachedPackNames names(pack_names);   // one question per pack and compilation
    for (decl::ClassBuild& build : res.classes) {
        build.trees = emit::emit_class(*res.scope, build, names, options.float_mode);
    }
    if (res.diags.errors != 0) {
        // no output on error: the trees of a source with an unknown name
        // are not handed out
        res.classes.clear();
        return false;
    }
    return true;
}

// Is options.float_mode one of the four settings?  When it is not, the
// mistake is reported and the compilation does not start.
bool float_mode_exists(const Options& options, Diagnostics& diags) {
    if (floatlit::is_mode(options.float_mode)) {
        return true;
    }
    std::string text("float_mode 0x");
    text += hex(static_cast<unsigned char>(options.float_mode), 2, true);
    text += ": one of A, B, C, D";
    diags.error(E_UNSUPPORTED, 1, 1, std::move(text));
    return false;
}

// The one place where a result gets its lists: what the compilation
// collected, taken when the stage that returns the result has ended.  scope
// is null when the source did not get as far as its declarations.  The lists
// are copied when the compilation goes on behind the result (keep true: a
// later stage appends to them) and moved when it ends with it.
void finish(std::vector<Diagnostic>& diagnostics, std::vector<std::string>& consulted,
            std::vector<std::string>& missed, classes::UnitScope* scope, Diagnostics& diags,
            bool keep) {
    if (keep) {
        diagnostics = diags.items;
        if (scope != nullptr) {
            consulted = scope->consulted;
            missed = scope->missed;
        }
    } else {
        diagnostics = std::move(diags.items);
        if (scope != nullptr) {
            consulted = std::move(scope->consulted);
            missed = std::move(scope->missed);
        }
    }
}

}  // namespace

void front(std::string_view source, std::string_view name, classes::ClassProvider& provider,
           const Options& options, FrontResult& res) {
    res.name = name;
    res.diags.strict = options.strict;
    res.ok = front_stage(source, provider, res);
    finish(res.diagnostics, res.consulted, res.missed, res.scope.get(), res.diags, true);
}

void emit_trees(std::string_view source, std::string_view name, classes::ClassProvider& provider,
                packs::PackNames& pack_names, const Options& options, FrontResult& res) {
    res.name = name;
    res.diags.strict = options.strict;
    bool ok = float_mode_exists(options, res.diags);
    if (ok) {
        ok = front_stage(source, provider, res);
    }
    if (ok) {
        ok = emit_stage(res, pack_names, options);
    }
    res.ok = ok;
    finish(res.diagnostics, res.consulted, res.missed, res.scope.get(), res.diags, true);
}

std::vector<std::string> declared_names(std::string_view source) {
    std::vector<std::string> out;
    FrontResult tmp;
    if (!parse(source, tmp.diags, tmp)) {
        return out;
    }
    const parser::Ast& ast = tmp.ast;
    std::string package;
    const parser::Node& unit = ast.nodes[ast.root];
    if (unit.a != parser::NONE) {
        const parser::Node& q = ast.nodes[unit.a];
        package = ast.dotted(q.tok0, q.tok1);
    }
    for (uint32_t n : classes::class_nodes(ast)) {
        const std::string_view simple = ast.text(ast.nodes[n].name_tok);
        std::string full;
        if (!package.empty()) {
            full = package;
            full += '.';
        }
        full += simple;
        out.push_back(std::move(full));
    }
    return out;
}

DeclareResult declare(std::string_view source, std::string_view file_name,
                      classes::ClassProvider& provider, const Options& options) {
    DeclareResult res;
    FrontResult tmp;
    tmp.name = file_name;
    tmp.diags.strict = options.strict;
    Diagnostics& diags = tmp.diags;
    if (parse(source, diags, tmp)) {
        tmp.scope = classes::declare_unit(tmp.ast, provider, diags);
    }
    classes::UnitScope* scope = tmp.scope.get();
    if (scope != nullptr && diags.errors == 0) {
        res.infos = std::move(scope->own);
        res.ok = true;
    }
    finish(res.diagnostics, res.consulted, res.missed, scope, diags, false);
    return res;
}

Result compile(std::string_view source, std::string_view file_name,
               classes::ClassProvider& provider, packs::PackNames& packs,
               const Options& options) {
    Result res;
    FrontResult stage;
    emit_trees(source, file_name, provider, packs, options, stage);
    if (stage.ok) {
        for (decl::ClassBuild& build : stage.classes) {
            write::finish_pool(build);
            res.class_names.push_back(build.info->name);
        }
        res.class_file = write::class_file(stage.classes, options.build);
        res.ok = true;
    } else {
        res.class_names.clear();
    }
    res.diagnostics = std::move(stage.diagnostics);
    res.consulted = std::move(stage.consulted);
    res.missed = std::move(stage.missed);
    return res;
}

}  // namespace slrrc
