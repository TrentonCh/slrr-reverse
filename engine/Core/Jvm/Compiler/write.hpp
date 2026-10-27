// write.hpp - the writer: the last two steps of the pool and the bytes of a
// class file (the reference's slrrc/write.py, function by function).
//
//     finish_pool(build)              steps 4 and 5 of the pool, over the trees
//     class_blob(build, number)       the bytes of one class
//     class_file(builds, number)      the blobs of a source, one behind the other
//
// Step 4 (resolve) walks the trees BY ROW VECTOR (static methods, instance
// methods, static field initialisers, instance field initialisers, each in
// row order), step 5 (emit) walks ALL trees IN TREE ORDER; see write.py for
// what each interns.  A class is the chunks TUFA, CONS, [FILD], [MTHD], CLSS,
// TREE: {tag, u32 size, payload}, little endian, nothing padded.
#ifndef SLRRC_WRITE_HPP
#define SLRRC_WRITE_HPP

#include <cstdint>
#include <vector>

#include "decl.hpp"

namespace slrrc {
namespace write {

// Steps 4 and 5 for a class whose trees are emitted; the length of the pool
// after each step goes to build.pool_steps.
void finish_pool(decl::ClassBuild& build);

// The bytes of one class; finish_pool(build) must have run.
std::vector<uint8_t> class_blob(decl::ClassBuild& build, uint32_t build_number);

// The bytes of a class file: the classes of a source in source order.
std::vector<uint8_t> class_file(std::vector<decl::ClassBuild>& builds, uint32_t build_number);

}  // namespace write
}  // namespace slrrc

#endif
