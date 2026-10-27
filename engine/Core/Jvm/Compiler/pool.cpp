// pool.cpp - the constant pool of one class: an interning table.  (Reference:
// slrrc/pool.py.)
//
// The two indices are ordered maps; neither is walked, so nothing depends on
// their order.  An index is a position in `entries`, which holds fewer than
// 2**32 entries on any machine the compiler runs on (an entry takes more than
// one byte of memory, and the pool of a class file is counted in a u32).
//
// to_cons() of the reference (the pool as the tuples of lib/tufa.py) is for
// its tests and has no counterpart here.
#include "pool.hpp"

namespace slrrc {
namespace pool {

uint32_t Pool::utf8(std::string_view text) {
    const std::map<std::string, uint32_t, std::less<>>::const_iterator hit = utf8_index_.find(text);
    if (hit != utf8_index_.end()) {
        return hit->second;
    }
    const uint32_t i = static_cast<uint32_t>(entries.size());
    utf8_index_.emplace(std::string(text), i);
    entries.push_back(PoolEntry{UTF8, 0, 0, std::string(text)});
    return i;
}

uint32_t Pool::entry(uint32_t kind, uint32_t a, uint32_t b) {
    const std::array<uint32_t, 3> key = {kind, a, b};
    const std::map<std::array<uint32_t, 3>, uint32_t>::const_iterator hit = index_.find(key);
    if (hit != index_.end()) {
        return hit->second;
    }
    const uint32_t i = static_cast<uint32_t>(entries.size());
    index_.emplace(key, i);
    entries.push_back(PoolEntry{kind, a, b, std::string()});
    return i;
}

uint32_t Pool::cls(std::string_view name) {
    const uint32_t n = utf8(name);
    return entry(CLASS, n, 0);
}

uint32_t Pool::nat(std::string_view name, std::string_view desc) {
    const uint32_t n = utf8(name);
    const uint32_t d = utf8(desc);
    return entry(NAT, n, d);
}

uint32_t Pool::ref(std::string_view cls_name, std::string_view name, std::string_view desc) {
    const uint32_t c = cls(cls_name);
    const uint32_t n = nat(name, desc);
    return entry(REF, c, n);
}

uint32_t Pool::rid(std::string_view pack, uint32_t local_id) {
    const uint32_t p = utf8(pack);
    return entry(RID, p, local_id);
}

uint32_t Pool::find_utf8(std::string_view text) const {
    const std::map<std::string, uint32_t, std::less<>>::const_iterator hit = utf8_index_.find(text);
    if (hit != utf8_index_.end()) {
        return hit->second;
    }
    return NONE;
}

}  // namespace pool
}  // namespace slrrc
