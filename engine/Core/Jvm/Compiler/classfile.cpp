// classfile.cpp - a strict reader of class files.  (Reference:
// slrrc/classfile.py; the functions stand in the order of that file and carry
// its names.)
//
// HOW THE READER STOPS AT AN ERROR.  bad() throws the private Bad with the one
// line that says what does not fit; read() catches it; nothing else throws it
// and nothing else catches it.  The code behind a check relies on not being
// reached.  Do not turn bad() into a function that returns (notes/impl_fix.md,
// finding 2).
//
// NUMBERS.  A position in the file is a size_t, a number taken from the file
// a uint32_t.  No comparison has a sum on either side (classfile.hpp,
// "Bounds"); a position is moved on only behind a check that the bytes are
// there.  A count of the file is never used to reserve memory before the
// bytes it counts are known to be there.
#include "classfile.hpp"

#include <algorithm>
#include <utility>

#include "diag.hpp"

namespace slrrc {
namespace classfile {

namespace pl = pool;
using decl::FieldRow;
using decl::MethodRow;

namespace {

// Private: unwinds the reader to read().
struct Bad {
    std::string text;
};

[[noreturn]] void bad(std::string text) {
    throw Bad{std::move(text)};
}

// "%d" and "%X" of a position or a number of bytes.
std::string dz(size_t value) {
    char buf[20];               // 2**64 - 1 has 20 digits
    unsigned n = 0;
    do {
        buf[n] = static_cast<char>('0' + value % 10);
        n += 1;
        value /= 10;
    } while (value != 0);
    std::string out;
    while (n > 0) {
        n -= 1;
        out += buf[n];
    }
    return out;
}

std::string hz(size_t value) {
    char buf[16];
    unsigned n = 0;
    do {
        buf[n] = "0123456789ABCDEF"[value & 0xF];
        n += 1;
        value >>= 4;
    } while (value != 0);
    std::string out;
    while (n > 0) {
        n -= 1;
        out += buf[n];
    }
    return out;
}

// "%r" of a bytes object, as the reference prints an unknown chunk tag:
// b'ABCD', with the escapes of Python.
std::string bytes_repr(std::string_view bytes) {
    char quote = '\'';
    if (bytes.find('\'') != std::string_view::npos && bytes.find('"') == std::string_view::npos) {
        quote = '"';
    }
    std::string out("b");
    out += quote;
    for (char ch : bytes) {
        const unsigned c = static_cast<unsigned char>(ch);
        if (ch == quote || ch == '\\') {
            out += '\\';
            out += ch;
        } else if (ch == '\t') {
            out += "\\t";
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (c < 0x20 || c >= 0x7F) {
            out += "\\x";
            out += hex(c, 2, false);
        } else {
            out += ch;
        }
    }
    out += quote;
    return out;
}

uint32_t le32(std::string_view data, size_t p) {
    return static_cast<uint32_t>(static_cast<unsigned char>(data[p])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[p + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[p + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[p + 3])) << 24);
}

// A bounded little-endian reader over one chunk payload: the bytes
// data[pos .. end), with pos <= end at all times.
struct Cursor {
    std::string_view data;
    size_t pos;
    size_t end;
    std::string what;

    // The number of bytes not yet read.
    size_t left() const { return end - pos; }

    uint32_t u32() {
        const size_t p = pos;
        if (end - p < 4) {
            bad(what + ": a u32 at 0x" + hz(p) + " runs past the end of the chunk");
        }
        pos = p + 4;
        return le32(data, p);
    }

    // The next n bytes; n comes from the file and may be any u32.
    std::string_view take(uint32_t n) {
        const size_t p = pos;
        if (n > end - p) {
            bad(what + ": " + dec(n) + " bytes at 0x" + hz(p) + " run past the end of the chunk");
        }
        pos = p + n;
        return data.substr(p, n);
    }

    void done() const {
        if (pos != end) {
            bad(what + ": " + dz(end - pos) + " unread bytes at 0x" + hz(pos));
        }
    }
};

std::vector<pl::PoolEntry> read_cons(Cursor& c) {
    const uint32_t n = c.u32();
    std::vector<pl::PoolEntry> out;
    uint32_t i = 0;
    while (i < n) {
        const uint32_t kind = c.u32();
        if (kind == pl::UTF8) {
            const uint32_t length = c.u32();
            const std::string_view raw = c.take(length);
            if (c.take(1) != std::string_view("\0", 1)) {
                bad(c.what + ": utf8 entry " + dec(i) + " is not closed by a NUL");
            }
            out.push_back(pl::PoolEntry{pl::UTF8, 0, 0, std::string(raw)});
        } else if (kind == pl::CLASS) {
            const uint32_t a = c.u32();
            out.push_back(pl::PoolEntry{pl::CLASS, a, 0, std::string()});
        } else if (kind == pl::RID || kind == pl::REF || kind == pl::NAT) {
            const uint32_t a = c.u32();
            const uint32_t b = c.u32();
            out.push_back(pl::PoolEntry{kind, a, b, std::string()});
        } else {
            bad(c.what + ": entry " + dec(i) + " has the unknown kind " + dec(kind));
        }
        i += 1;
    }
    c.done();
    return out;
}

std::vector<FieldRow> read_field_rows(Cursor& c) {
    const uint32_t n = c.u32();
    if (n > c.left() / 16) {
        bad(c.what + ": " + dec(n) + " rows of 16 bytes run past the end of the chunk");
    }
    std::vector<FieldRow> out;
    out.reserve(n);
    uint32_t i = 0;
    while (i < n) {
        const uint32_t flags = c.u32();
        const uint32_t name = c.u32();
        const uint32_t type = c.u32();
        const uint32_t tree = c.u32();
        out.push_back(FieldRow{flags, name, type, tree});
        i += 1;
    }
    return out;
}

std::vector<MethodRow> read_method_rows(Cursor& c) {
    const uint32_t n = c.u32();
    if (n > c.left() / 20) {
        bad(c.what + ": " + dec(n) + " rows of 20 bytes run past the end of the chunk");
    }
    std::vector<MethodRow> out;
    out.reserve(n);
    uint32_t i = 0;
    while (i < n) {
        const uint32_t flags = c.u32();
        const uint32_t name = c.u32();
        const uint32_t desc = c.u32();
        const uint32_t tree = c.u32();
        const uint32_t local_slots = c.u32();
        out.push_back(MethodRow{flags, name, desc, tree, local_slots});
        i += 1;
    }
    return out;
}

void read_clss(Cursor& c, BlobData& blob) {
    const uint32_t marker = c.u32();
    if (marker != NONE) {
        bad(c.what + ": the first word is 0x" + hex(marker, 1, true) + ", not 0xFFFFFFFF");
    }
    blob.interface_flag = c.u32();
    if (blob.interface_flag > 1) {
        bad(c.what + ": interface flag " + dec(blob.interface_flag));
    }
    blob.name = c.u32();
    blob.super = c.u32();
    const uint32_t n = c.u32();
    if (n > c.left() / 4) {
        bad(c.what + ": " + dec(n) + " interfaces run past the end of the chunk");
    }
    uint32_t i = 0;
    while (i < n) {
        blob.interfaces.push_back(c.u32());
        i += 1;
    }
    c.done();
}

void read_tree(Cursor& c, BlobData& blob, bool with_trees) {
    blob.tree_count = c.u32();
    if (!with_trees) {
        return;
    }
    const std::string_view data = c.data;
    std::vector<std::vector<TreeNode>> trees;
    uint32_t t = 0;
    while (t < blob.tree_count) {
        const uint32_t count = c.u32();
        std::vector<TreeNode> nodes;
        size_t p = c.pos;
        const size_t end = c.end;
        uint32_t k = 0;
        while (k < count) {
            if (end - p < 3) {
                bad(c.what + ": tree " + dec(t) + " node " + dec(k) +
                    " runs past the end of the chunk");
            }
            const uint8_t op = static_cast<uint8_t>(data[p]);
            if (op >= N_OPS) {
                bad(c.what + ": tree " + dec(t) + " node " + dec(k) + " has the op byte " + dec(op));
            }
            const uint16_t line = static_cast<uint16_t>(
                static_cast<unsigned char>(data[p + 1]) |
                (static_cast<unsigned char>(data[p + 2]) << 8));
            if (OP_HAS_OPERAND[op]) {
                if (end - p < 7) {
                    bad(c.what + ": tree " + dec(t) + " node " + dec(k) +
                        " runs past the end of the chunk");
                }
                nodes.push_back(TreeNode{op, line, le32(data, p + 3)});
                p += 7;
            } else {
                nodes.push_back(TreeNode{op, line, NONE});
                p += 3;
            }
            k += 1;
        }
        c.pos = p;
        trees.push_back(std::move(nodes));
        t += 1;
    }
    c.done();
    blob.has_trees = true;
    blob.trees = std::move(trees);
}

bool has_tag(const std::vector<std::string_view>& seen, std::string_view tag) {
    return std::find(seen.begin(), seen.end(), tag) != seen.end();
}

void read_all(std::string_view data, bool with_trees, ReadResult& res) {
    const size_t size = data.size();
    if (size == 0) {
        bad("empty file");
    }
    size_t pos = 0;
    while (pos < size) {
        const size_t start = pos;
        if (size - pos < 12) {
            bad(dz(size - pos) + " trailing bytes at 0x" + hz(pos) + ", too short for a TUFA chunk");
        }
        if (data.substr(pos, 4) != "TUFA") {
            bad("no TUFA tag at 0x" + hz(pos));
        }
        Cursor head{data, pos + 4, pos + 12, "TUFA at 0x" + hz(pos)};
        if (head.u32() != 4) {
            bad("the TUFA chunk at 0x" + hz(pos) + " does not have size 4");
        }
        BlobData blob;
        blob.offset = start;
        blob.build = head.u32();
        pos += 12;
        std::vector<std::string_view> seen;
        for (;;) {
            if (size - pos < 8) {
                bad("the class at 0x" + hz(start) + " ends at 0x" + hz(pos) + " without a TREE chunk");
            }
            const std::string_view tag = data.substr(pos, 4);
            Cursor size_word{data, pos + 4, pos + 8, "chunk at 0x" + hz(pos)};
            const uint32_t csize = size_word.u32();
            const size_t begin = pos + 8;
            if (csize > size - begin) {
                bad("the chunk at 0x" + hz(pos) + " (size " + dec(csize) +
                    ") runs past the end of the file");
            }
            const size_t end = begin + csize;
            if (has_tag(seen, tag)) {
                bad("second " + std::string(tag) + " chunk at 0x" + hz(pos));
            }
            if (seen.empty() && tag != "CONS") {
                bad("the class at 0x" + hz(start) + " does not start with CONS");
            }
            Cursor c{data, begin, end, std::string(tag) + " at 0x" + hz(pos)};
            if (tag == "CONS") {
                blob.pool = read_cons(c);
            } else if (tag == "FILD") {
                blob.has_fild = true;
                blob.static_fields = read_field_rows(c);
                blob.instance_fields = read_field_rows(c);
                c.done();
            } else if (tag == "MTHD") {
                blob.has_mthd = true;
                blob.static_methods = read_method_rows(c);
                blob.instance_methods = read_method_rows(c);
                c.done();
            } else if (tag == "CLSS") {
                read_clss(c, blob);
            } else if (tag == "TREE") {
                read_tree(c, blob, with_trees);
            } else {
                bad("unknown chunk tag " + bytes_repr(tag) + " at 0x" + hz(pos));
            }
            seen.push_back(tag);
            pos = end;
            if (tag == "TREE") {
                break;
            }
        }
        if (!has_tag(seen, "CLSS")) {
            bad("the class at 0x" + hz(start) + " has no CLSS chunk");
        }
        blob.size = pos - start;
        res.blobs.push_back(std::move(blob));
    }
}

}  // namespace

ReadResult read(std::string_view data, bool with_trees) {
    ReadResult res;
    try {
        read_all(data, with_trees, res);
        res.ok = true;
    } catch (const Bad& x) {
        res.error = x.text;
        res.blobs.clear();
    }
    return res;
}

// ---------------------------------------------------------------------------
// what a class file declares
// ---------------------------------------------------------------------------

namespace {

// The text of utf8 entry i, nullptr when i is no utf8 entry.
const std::string* utf8(const BlobData& blob, uint32_t i) {
    if (i < blob.pool.size() && blob.pool[i].kind == pl::UTF8) {
        return &blob.pool[i].text;
    }
    return nullptr;
}

// The name behind class entry i, nullptr when i is no class entry.
const std::string* class_name(const BlobData& blob, uint32_t i) {
    if (i < blob.pool.size() && blob.pool[i].kind == pl::CLASS) {
        return utf8(blob, blob.pool[i].a);
    }
    return nullptr;
}

// A method row that has a tree: (tree number, place among such rows, method).
struct WithTree {
    uint32_t tree;
    size_t place;
    classes::MethodInfo method;
};

// The order of the methods: by tree number, rows with the same number in row
// order.
bool tree_key(const WithTree& a, const WithTree& b) {
    if (a.tree != b.tree) {
        return a.tree < b.tree;
    }
    return a.place < b.place;
}

}  // namespace

bool class_info(const BlobData& blob, classes::ClassInfo& info) {
    const std::string* name = class_name(blob, blob.name);
    if (name == nullptr) {
        return false;
    }
    info = classes::ClassInfo(*name);
    info.is_interface = blob.interface_flag != 0;
    if (blob.super != NONE) {
        const std::string* super = class_name(blob, blob.super);
        if (super == nullptr) {
            return false;
        }
        info.super = *super;
        info.has_super = true;
    }
    for (uint32_t i : blob.interfaces) {
        const std::string* n = class_name(blob, i);
        if (n == nullptr) {
            return false;
        }
        info.interfaces.push_back(*n);
    }
    for (int pass = 0; pass < 2; ++pass) {
        const bool is_static = pass == 0;
        const std::vector<FieldRow>& rows = is_static ? blob.static_fields : blob.instance_fields;
        for (const FieldRow& r : rows) {
            const std::string* n = utf8(blob, r.name);
            const std::string* d = utf8(blob, r.type);
            if (n == nullptr || d == nullptr) {
                return false;
            }
            info.fields.push_back(classes::FieldInfo{*n, *d, is_static});
        }
    }
    std::vector<WithTree> with_tree;
    std::vector<classes::MethodInfo> without;
    for (int pass = 0; pass < 2; ++pass) {
        const bool is_static = pass == 0;
        const std::vector<MethodRow>& rows = is_static ? blob.static_methods : blob.instance_methods;
        for (const MethodRow& r : rows) {
            const std::string* n = utf8(blob, r.name);
            const std::string* d = utf8(blob, r.desc);
            if (n == nullptr || d == nullptr) {
                return false;
            }
            classes::MethodInfo m{*n, *d, is_static};
            if (r.tree == NONE) {
                without.push_back(std::move(m));
            } else {
                const size_t place = with_tree.size();
                with_tree.push_back(WithTree{r.tree, place, std::move(m)});
            }
        }
    }
    std::sort(with_tree.begin(), with_tree.end(), tree_key);
    for (WithTree& x : with_tree) {
        info.methods.push_back(std::move(x.method));
    }
    for (classes::MethodInfo& m : without) {
        info.methods.push_back(std::move(m));
    }
    return true;
}

}  // namespace classfile
}  // namespace slrrc
