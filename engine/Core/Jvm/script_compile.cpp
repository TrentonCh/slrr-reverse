// script_compile.cpp - compile on demand for the class loader
// (script_compiler.hpp; compiler_spec.md 2.10).
#include "script_compiler.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "Compiler/classes.hpp"
#include "Compiler/floatlit.hpp"
#include "Compiler/install.hpp"
#include "Compiler/slrrc.hpp"
#include "tufa.hpp"

namespace inv {

bool native_script_boot();

namespace {

uint64_t fnv1a(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string hex64(uint64_t v) {
  char b[17];
  std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
  return b;
}

std::string lower_slash(std::string s) {
  for (char& c : s) {
    if (c == '\\') c = '/';
    else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

// One per process: the install, the classes it declares, its pack table.
struct Session {
  bool ready = false;
  bool off = false;
  std::string root;       // absolute, as Install made it
  std::string cache_dir;
  std::unique_ptr<slrrc::install::Install> inst;
  std::unique_ptr<slrrc::install::InstallProvider> provider;
  std::unique_ptr<slrrc::install::InstallPacks> packs;
  std::map<std::string, size_t> by_output;   // lower(output_rel) -> sources index
};

Session& session() {
  static Session* s = new Session();  // never destroyed: used until exit
  return *s;
}

std::string default_cache_dir(const std::string& root) {
  if (const char* e = std::getenv("SLRR_SCRIPT_CACHE_DIR"); e && e[0]) return e;
  const char* base = std::getenv("LOCALAPPDATA");
  std::string b = (base && base[0]) ? base : ".";
  return b + "/slrr-reverse/script-cache/" + hex64(fnv1a(lower_slash(root)));
}

bool start(Session& s, const std::string& game_root) {
  if (s.ready) return !s.off;
  s.ready = true;
  if (const char* e = std::getenv("SLRR_SCRIPT_COMPILE"); e && e[0] == '0') {
    s.off = true;
    return false;
  }
  // floatlit.hpp: a build whose float arithmetic differs from the reference
  // writes no class file.
  const std::vector<slrrc::floatlit::BadRow> bad = slrrc::floatlit::self_test();
  if (!bad.empty()) {
    std::fprintf(stderr, "[jvm] script compiler off: the float self test fails (%zu rows)\n",
                 bad.size());
    s.off = true;
    return false;
  }
  const auto t0 = std::chrono::steady_clock::now();
  s.inst.reset(new slrrc::install::Install(game_root.empty() ? "." : game_root));
  s.inst->scan();
  s.root = s.inst->root;
  s.provider.reset(new slrrc::install::InstallProvider(*s.inst, false));
  s.provider->load();
  s.packs.reset(new slrrc::install::InstallPacks(*s.inst));
  for (size_t i = 0; i < s.inst->sources.size(); ++i)
    s.by_output[lower_slash(s.inst->sources[i].output_rel())] = i;
  s.cache_dir = default_cache_dir(s.root);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::fprintf(stderr,
               "[jvm] script compiler ready: %zu sources, %zu class files, %u classes read, cache %s "
               "(%.0f ms)\n",
               s.inst->sources.size(), s.inst->class_files.size(), s.provider->class_count,
               s.cache_dir.c_str(), ms);
  return true;
}

// The class file's path relative to the install root, lower case with "/";
// empty when it is outside the root.
std::string rel_of(const Session& s, const std::string& class_path) {
  std::error_code ec;
  const std::filesystem::path p = std::filesystem::absolute(std::filesystem::u8path(class_path), ec);
  if (ec) return {};
  std::string a = lower_slash(p.lexically_normal().u8string());
  std::string r = lower_slash(std::filesystem::u8path(s.root).lexically_normal().u8string());
  while (!r.empty() && r.back() == '/') r.pop_back();
  if (a.size() <= r.size() + 1 || a.compare(0, r.size(), r) != 0 || a[r.size()] != '/') return {};
  return a.substr(r.size() + 1);
}

std::string manifest_text(const Session& s, const std::string& source,
                          const std::vector<std::string>& consulted,
                          const std::vector<std::string>& missed) {
  std::ostringstream o;
  o << "slrrc " << slrrc::VERSION << "\n";
  o << "float " << slrrc::floatlit::mode_letter(slrrc::floatlit::DEFAULT_MODE) << "\n";
  o << "source " << source.size() << " " << hex64(fnv1a(source)) << "\n";
  for (const std::string& name : consulted) {
    const slrrc::classes::ClassInfo* info = s.provider->find(name);
    o << "consulted " << name << " "
      << (info ? hex64(fnv1a(slrrc::classes::declaration_text(*info))) : std::string("-")) << "\n";
  }
  for (const std::string& name : missed) o << "missed " << name << "\n";
  return o.str();
}

bool read_all(const std::string& path, std::string* out) {
  std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

// Is the entry's manifest what this source and these classes give now?
bool entry_valid(Session& s, const std::string& manifest_path, const std::string& source) {
  std::string text;
  if (!read_all(manifest_path, &text)) return false;
  std::vector<std::string> consulted, missed;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("consulted ", 0) == 0) {
      const size_t sp = line.find(' ', 10);
      consulted.push_back(line.substr(10, sp == std::string::npos ? std::string::npos : sp - 10));
    } else if (line.rfind("missed ", 0) == 0) {
      missed.push_back(line.substr(7));
    }
  }
  for (const std::string& name : missed)
    if (s.provider->find(name) != nullptr) return false;  // a class that now exists
  return manifest_text(s, source, consulted, missed) == text;
}

bool write_atomic(const std::string& path, const std::string& bytes) {
  std::error_code ec;
  const std::filesystem::path p = std::filesystem::u8path(path);
  std::filesystem::create_directories(p.parent_path(), ec);
  const std::filesystem::path tmp = std::filesystem::u8path(path + ".tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) return false;
  }
  std::filesystem::rename(tmp, p, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
}

// Self test (SLRR_SCRIPT_COMPILE_ALL=1, once, when the compiler starts):
// every source of the install without a usable class file is compiled
// through script_compile_missing and its cache file read back with the
// engine's TUFA reader (compiler_spec.md M8: "635/635 compiled and loaded").
bool g_in_self_test = false;

}  // namespace

bool script_compile_missing(const std::string& game_root, const std::string& class_path,
                            std::string* cache_path);

namespace {

void self_test(Session& s, const std::string& game_root) {
  const char* e = std::getenv("SLRR_SCRIPT_COMPILE_ALL");
  if (!e || e[0] != '1' || g_in_self_test) return;
  g_in_self_test = true;
  int asked = 0, compiled = 0, loaded = 0, classes = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (const slrrc::install::SourceFile& sf : s.inst->sources) {
    if (!sf.lacks_class()) continue;
    ++asked;
    std::string out;
    if (!script_compile_missing(game_root, s.root + "/" + sf.output_rel(), &out)) continue;
    ++compiled;
    std::vector<JvmClass> all;
    std::string err;
    if (tufa_load_file_all(out.c_str(), &all, &err) && !all.empty()) {
      ++loaded;
      classes += static_cast<int>(all.size());
    } else {
      std::fprintf(stderr, "[jvm] self test: %s does not load: %s\n", out.c_str(), err.c_str());
    }
  }
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::fprintf(stderr,
               "[jvm] script compile all: %d/%d compiled, %d/%d loaded by the TUFA reader (%d "
               "classes) in %.0f ms\n",
               compiled, asked, loaded, asked, classes, ms);
  g_in_self_test = false;
}

}  // namespace

bool script_compile_missing(const std::string& game_root, const std::string& class_path,
                            std::string* cache_path) {
  if (!native_script_boot() || !cache_path) return false;
  Session& s = session();
  if (!start(s, game_root)) return false;
  self_test(s, game_root);
  const std::string rel = rel_of(s, class_path);
  if (rel.empty()) return false;
  auto it = s.by_output.find(rel);
  if (it == s.by_output.end()) return false;
  const slrrc::install::SourceFile& sf = s.inst->sources[it->second];
  std::string source, why;
  if (!slrrc::install::read_source(sf, source, why)) {
    std::fprintf(stderr, "[jvm] cannot read %s: %s\n", sf.rel.c_str(), why.c_str());
    return false;
  }
  // The install-relative layout, unless that path (with ".slrrc.tmp") would
  // pass the 260 characters fopen and the Win32 file calls take: then a
  // short name made from the relative path, in the same cache.
  std::string out = s.cache_dir + "/" + sf.output_rel();
  if (out.size() + 10 >= 255) out = s.cache_dir + "/long/" + hex64(fnv1a(lower_slash(sf.output_rel()))) + ".class";
  const std::string manifest = out + ".slrrc";
  if (std::filesystem::exists(std::filesystem::u8path(out)) && entry_valid(s, manifest, source)) {
    std::fprintf(stderr, "[jvm] class from the script cache %s\n", out.c_str());
    *cache_path = out;
    return true;
  }
  const auto t0 = std::chrono::steady_clock::now();
  slrrc::Result res = slrrc::compile(source, sf.rel, *s.provider, s.packs->table);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (!res.ok) {
    std::fprintf(stderr, "[jvm] compile %s failed: %s\n", sf.rel.c_str(),
                 res.diagnostics.empty() ? "no class"
                                         : res.diagnostics.front().format(sf.rel).c_str());
    return false;
  }
  const std::string bytes(res.class_file.begin(), res.class_file.end());
  if (!write_atomic(out, bytes) ||
      !write_atomic(manifest, manifest_text(s, source, res.consulted, res.missed))) {
    std::fprintf(stderr, "[jvm] compiled %s but cannot write %s\n", sf.rel.c_str(), out.c_str());
    return false;
  }
  std::fprintf(stderr, "[jvm] compiled %s -> %s (%zu bytes, %zu classes, %.1f ms)\n",
               sf.rel.c_str(), out.c_str(), bytes.size(), res.class_names.size(), ms);
  *cache_path = out;
  return true;
}

}  // namespace inv
