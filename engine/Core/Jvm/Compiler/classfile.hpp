// classfile.hpp - a strict reader of class files.  (Reference:
// slrrc/classfile.py.)
//
//     ReadResult res = read(data);                    // ok, error, blobs
//     ClassInfo info;
//     bool usable = class_info(res.blobs[0], info);
//
// The compiler writes class files (write) and never reads one.  Its host does:
// the declarations of a class that exists as a class file reach the compiler
// through a ClassProvider, and a provider gets them from here (the provider
// of the command line tool does: tool/install.cpp).  The layout is the one
// the writer writes (notes/foundation.md 3): a flat stream of chunks {tag,
// size, payload}; per class TUFA, CONS, [FILD], [MTHD], CLSS, TREE; little
// endian, no padding.
//
// The reader is strict: every byte has to fit the layout, every chunk has to
// be read to its last byte, and a tag, pool kind or op byte outside the known
// set is an error.  A file that does not fit gives ok false and one line that
// says where; nothing is guessed.  (The engine's own loader decodes the pool
// heuristically; a compiler must not build on that.)
//
// Bad input never throws out of read(): the reader unwinds with a private
// exception that is thrown where the layout does not fit and caught in
// read(), and nowhere else.  The code behind a check relies on not being
// reached, as in the parser.  Nothing here opens a file.
//
// BOUNDS.  Every length, count and chunk size is a u32 taken from the file.
// No check adds such a number to a position: with 32-bit integers (the engine
// is built for 32 bits) `pos + n > end` wraps for an n near 2**32 and passes.
// A check compares the number with the bytes that are left, `n > end - pos`;
// pos never exceeds end, so that difference cannot wrap.  A position is moved
// on only behind such a check.
//
// By default the nodes of the TREE chunk are skipped, since a provider needs
// declarations only; the chunk is still checked for its node count and size
// when with_trees is true, and its bytes are always inside the chunk bounds.
//
// This header does not include parser.hpp and can stand behind <windows.h>.
#ifndef SLRRC_CLASSFILE_HPP
#define SLRRC_CLASSFILE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "classes.hpp"
#include "decl.hpp"
#include "pool.hpp"

namespace slrrc {
namespace classfile {

inline constexpr uint32_t NONE = 0xFFFFFFFF;

// Which op bytes are followed by a u32 (notes/opcodes.md; the 48-entry table
// of the VM, engine/include/tree_op_flags.inc: the high nibble of the 16-bit
// opcode is 1, 2 or 4).  The index is the op byte.
inline constexpr uint32_t N_OPS = 48;
inline constexpr bool OP_HAS_OPERAND[N_OPS] = {
    false, true, true, true, false, false, true, true,          // 0x00
    true, true, true, true, false, true, true, true,            // 0x08
    false, true, true, false, true, true, true, true,           // 0x10
    false, true, true, true, true, false, true, true,           // 0x18
    false, false, false, false, false, true, true, true,        // 0x20
    false, false, false, false, false, false, true, false,      // 0x28
};

// One node of a tree: (op, line, operand or NONE).
struct TreeNode {
    uint8_t op;
    uint16_t line;
    uint32_t operand;
};

// One class of a class file, as its chunks hold it.
struct BlobData {
    uint32_t build = 0;
    size_t offset = 0;                              // where the blob starts in the file
    size_t size = 0;                                // bytes it occupies
    std::vector<pool::PoolEntry> pool;
    bool has_fild = false;
    bool has_mthd = false;
    std::vector<decl::FieldRow> static_fields;
    std::vector<decl::FieldRow> instance_fields;
    std::vector<decl::MethodRow> static_methods;
    std::vector<decl::MethodRow> instance_methods;
    uint32_t interface_flag = 0;                    // CLSS: 1 for an interface (the reference's
    //                                                 `interface`)
    uint32_t name = NONE;                           // CLSS: class entry
    uint32_t super = NONE;                          // CLSS: class entry, or NONE
    std::vector<uint32_t> interfaces;               // CLSS: class entries
    uint32_t tree_count = 0;
    bool has_trees = false;                         // with_trees (the reference: trees is not None)
    std::vector<std::vector<TreeNode>> trees;
};

struct ReadResult {
    bool ok = false;
    std::string error;                              // one line: what does not fit, and where
    std::vector<BlobData> blobs;
};

// Read the bytes of a class file: one BlobData per class.  with_trees: also
// decode the nodes (blob.trees).
ReadResult read(std::string_view data, bool with_trees = false);

// ---------------------------------------------------------------------------
// what a class file declares
// ---------------------------------------------------------------------------

// The ClassInfo of one class of a class file; false (and info in no usable
// state) when a row or the CLSS record points at an entry of the wrong kind.
//
// Fields: the static vector, then the instance vector (each in source order).
// Methods: in source order, which is the order of their tree numbers; a row
// without a tree (abstract) stands behind the others in row order.  The
// lookup of classes.cpp scans methods in an order derived from this one.
bool class_info(const BlobData& blob, classes::ClassInfo& info);

}  // namespace classfile
}  // namespace slrrc

#endif
