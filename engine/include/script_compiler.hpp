// script_compiler.hpp - the class loader's use of the script compiler
// (engine/Core/Jvm/Compiler, the C++ port of tools/slrrc, equal to the
// reference on all 3,454 sources of the game at every level, bytes included).
//
// Fork (compiler_spec.md 2.10): the exe compiles X\src\Name.java into
// X\Name.class inside the install when the class file is missing or stale.
// The host never writes into the install: a class file that is missing is
// compiled from its source into a cache directory, and the loader reads it
// from there. Script boot only.
#ifndef INV_SCRIPT_COMPILER_HPP
#define INV_SCRIPT_COMPILER_HPP

#include <string>

namespace inv {

// `class_path` is the class file the loader looked for (absolute, or
// relative to the working directory). When it is missing and the install
// has its source (X\src\Name.java, the directory spelled in any case), the
// source is compiled into the cache, or a valid cache entry is reused, and
// *cache_path names the class file to load. False when there is no source,
// the compiler reports an error, or compiling is off.
//
// Cache: SLRR_SCRIPT_CACHE_DIR, else %LOCALAPPDATA%\slrr-reverse\
// script-cache\<install key>; per entry `<file>.class` and `<file>.class.slrrc`
// (compiler version, float mode, the source's size and hash, and the hash of
// the declarations of every class the compilation consulted). An entry is
// valid when all of them still hold. SLRR_SCRIPT_COMPILE=0 turns compiling off.
bool script_compile_missing(const std::string& game_root, const std::string& class_path,
                            std::string* cache_path);

}  // namespace inv

#endif
