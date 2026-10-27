// pool.hpp - the constant pool of one class: an interning table.  (Reference:
// slrrc/pool.py.)
//
// A pool is a list of entries; an entry refers only to entries with a lower
// index, and no entry exists twice (notes/foundation.md 3.2, rules P1 and P3).
// Interning returns the index of the entry and appends it when it is new,
// whatever it was first interned for.  The pool is a log: entries that nothing
// refers to in the end stay in it.
//
//     utf8(text)              text
//     cls(name)               utf8 name, then the class entry
//     nat(name, desc)         utf8 name, utf8 descriptor, then the nat entry
//     ref(cls, name, desc)    the class (utf8, class), the nat (utf8, utf8, nat),
//                             then the ref entry
//     rid(pack, local_id)     utf8 pack file name, then the rid entry
//
// The inner order is proven by the shipped pools (notes/declarations.md 6.5):
// class before nat inside a ref (the other order fails 1,620 classes), name
// before descriptor inside a nat (2,811).
//
// The compiler appends to the pool in five steps: seed, types, names (decl),
// resolve, emit (the writer, over the finished trees).
#ifndef SLRRC_POOL_HPP
#define SLRRC_POOL_HPP

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace slrrc {
namespace pool {

// the kind word of an entry in the CONS chunk
inline constexpr uint32_t UTF8 = 0;
inline constexpr uint32_t RID = 3;
inline constexpr uint32_t CLASS = 4;
inline constexpr uint32_t REF = 5;
inline constexpr uint32_t NAT = 7;

inline constexpr uint32_t NONE = 0xFFFFFFFF;

struct PoolEntry {
    uint32_t kind;
    uint32_t a;             // CLASS: utf8 of the name   NAT: utf8 of the name
    //                         REF: class entry          RID: utf8 of the pack file name
    uint32_t b;             // NAT: utf8 of the descriptor   REF: nat entry   RID: local id
    std::string text;       // UTF8: the text (one character per byte)
};

class Pool {
public:
    std::vector<PoolEntry> entries;

    uint32_t utf8(std::string_view text);
    uint32_t cls(std::string_view name);
    uint32_t nat(std::string_view name, std::string_view desc);
    uint32_t ref(std::string_view cls_name, std::string_view name, std::string_view desc);
    uint32_t rid(std::string_view pack, uint32_t local_id);

    // Index of a text that is in the pool, NONE when it is not.
    uint32_t find_utf8(std::string_view text) const;

private:
    uint32_t entry(uint32_t kind, uint32_t a, uint32_t b);

    std::map<std::string, uint32_t, std::less<>> utf8_index_;       // text -> index
    std::map<std::array<uint32_t, 3>, uint32_t> index_;             // (kind, a, b) -> index
};

}  // namespace pool
}  // namespace slrrc

#endif
