// install.cpp - a game tree on disk.  (Reference: slrrc/install.py.)
#include "install.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <system_error>
#include <utility>

#include "classfile.hpp"
#include "slrrc.hpp"

namespace slrrc {
namespace install {

namespace fs = std::filesystem;

namespace {

const char SEP = static_cast<char>(fs::path::preferred_separator);

fs::path to_path(std::string_view utf8) {
    return fs::u8path(utf8.begin(), utf8.end());
}

std::string from_path(const fs::path& p) {
    return p.u8string();
}

bool ends_with(std::string_view text, std::string_view tail) {
    return text.size() >= tail.size() && text.substr(text.size() - tail.size()) == tail;
}

bool starts_with(std::string_view text, std::string_view head) {
    return text.size() >= head.size() && text.substr(0, head.size()) == head;
}

std::string replaced(std::string_view text, char from, char to) {
    std::string out(text);
    for (char& ch : out) {
        if (ch == from) {
            ch = to;
        }
    }
    return out;
}

bool is_separator(char ch) {
    return ch == '/' || ch == '\\';
}

// No separator at the end, except behind a root ("D:\", "\").
void strip_last_separator(std::string& s) {
    while (s.size() > 1 && is_separator(s[s.size() - 1]) &&
           !(s.size() == 3 && s[1] == ':')) {
        s.pop_back();
    }
}

// os.path.join for a relative second part.
std::string join(std::string_view dir, std::string_view rest) {
    std::string out(dir);
    if (!out.empty() && !is_separator(out[out.size() - 1])) {
        out += SEP;
    }
    out += rest;
    return out;
}

// os.path.normcase(os.path.normpath(os.path.abspath(p))), with the lower
// case of ASCII only.
std::string norm_key(std::string_view path) {
    return lower(replaced(abspath(path), '/', SEP));
}

// The path with its links resolved, in the form norm_key gives.
std::string real_key(std::string_view path) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::absolute(to_path(path), ec), ec);
    if (ec) {
        return norm_key(path);
    }
    std::string s = from_path(p.lexically_normal());
    strip_last_separator(s);
    return lower(replaced(s, '/', SEP));
}

// The order of the index: directory, then name, both lower case.
typedef std::pair<std::string, std::string> IndexKey;

IndexKey index_key(std::string_view class_dir, std::string_view name) {
    return IndexKey(lower(class_dir), lower(name));
}

struct FoundClass {
    IndexKey key;
    std::string rel;
};

struct FoundSource {
    IndexKey key;
    SourceFile sf;
};

struct Walk {
    Install& inst;
    std::vector<FoundClass> found_classes;
    std::vector<FoundSource> found_sources;
    std::map<IndexKey, std::string> by_key;         // index key -> class file

    // One directory, then the directories in it: os.walk, top down, with
    // the names sorted.  A directory that cannot be listed is passed over; a
    // link to a directory is listed and not followed.
    void directory(const fs::path& dir, const std::string& dirpath, const std::string& rel_dir) {
        std::vector<std::string> dirnames;
        std::vector<std::string> filenames;
        std::error_code ec;
        fs::directory_iterator it(dir, ec);
        if (ec) {
            return;
        }
        const fs::directory_iterator end;
        while (it != end) {
            std::error_code ec2;
            bool entry_is_dir = it->is_directory(ec2);
            if (ec2) {
                entry_is_dir = false;
            }
            std::string name = from_path(it->path().filename());
            if (entry_is_dir) {
                dirnames.push_back(std::move(name));
            } else {
                filenames.push_back(std::move(name));
            }
            it.increment(ec);
            if (ec) {
                return;
            }
        }
        std::vector<std::string> kept;
        for (std::string& d : dirnames) {
            if (!inst.skipped(d)) {
                kept.push_back(std::move(d));
            }
        }
        std::sort(kept.begin(), kept.end());
        std::sort(filenames.begin(), filenames.end());
        for (const std::string& f : filenames) {
            const std::string low = lower(f);
            const std::string rel = rel_dir.empty() ? f : rel_dir + '/' + f;
            if (ends_with(low, CLASS_EXT)) {
                IndexKey key = index_key(rel_dir, std::string_view(f).substr(0, f.size() - (sizeof(CLASS_EXT) - 1)));
                if (by_key.find(key) == by_key.end()) {
                    by_key[key] = rel;
                }
                found_classes.push_back(FoundClass{std::move(key), rel});
            } else if (ends_with(low, PACK_EXT)) {
                inst.pack_files.push_back(replaced(rel, '/', '\\'));
            } else if (ends_with(low, SOURCE_EXT)) {
                // head, _, last = rel_dir.rpartition('/')
                const size_t slash = rel_dir.rfind('/');
                const std::string head = slash == std::string::npos ? std::string() : rel_dir.substr(0, slash);
                const std::string last = slash == std::string::npos ? rel_dir : rel_dir.substr(slash + 1);
                if (lower(last) != SOURCE_DIR) {
                    inst.stray_sources.push_back(rel);
                    continue;
                }
                SourceFile sf;
                sf.path = join(dirpath, f);
                sf.rel = rel;
                sf.class_dir = head;
                sf.name = f.substr(0, f.size() - (sizeof(SOURCE_EXT) - 1));
                IndexKey key = index_key(head, sf.name);
                found_sources.push_back(FoundSource{std::move(key), std::move(sf)});
            }
        }
        for (const std::string& d : kept) {
            const fs::path sub = dir / to_path(d);
            std::error_code ec3;
            const fs::file_status st = fs::symlink_status(sub, ec3);
            if (!ec3 && st.type() == fs::file_type::symlink) {
                continue;
            }
            directory(sub, join(dirpath, d), rel_dir.empty() ? d : rel_dir + '/' + d);
        }
    }
};

bool key_less_class(const FoundClass& a, const FoundClass& b) {
    return a.key < b.key;
}

bool key_less_source(const FoundSource& a, const FoundSource& b) {
    return a.key < b.key;
}

}  // namespace

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

std::string abspath(std::string_view path) {
    std::error_code ec;
    fs::path p = fs::absolute(to_path(path), ec);
    if (ec) {
        p = to_path(path);
    }
    std::string s = from_path(p.lexically_normal());
    strip_last_separator(s);
    return s;
}

std::string join_path(std::string_view dir, std::string_view path) {
    return from_path(to_path(dir) / to_path(path));
}

bool is_inside(std::string_view path, std::string_view root) {
    const std::string p = real_key(path);
    std::string r = real_key(root);
    if (p == r) {
        return true;
    }
    if (r.empty() || r[r.size() - 1] != SEP) {
        r += SEP;
    }
    return starts_with(p, r);
}

bool is_dir(std::string_view path) {
    std::error_code ec;
    return fs::is_directory(to_path(path), ec) && !ec;
}

bool is_file(std::string_view path) {
    std::error_code ec;
    return fs::is_regular_file(to_path(path), ec) && !ec;
}

bool read_file(std::string_view path, std::string& data, std::string& why) {
    data.clear();
    std::ifstream f(to_path(path), std::ios::binary);
    if (!f) {
        why = "cannot open ";
        why += path;
        return false;
    }
    std::vector<char> buf(65536);
    for (;;) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = f.gcount();
        if (got > 0) {
            data.append(buf.data(), static_cast<size_t>(got));
        }
        if (!f) {
            break;
        }
    }
    if (f.bad()) {
        why = "cannot read ";
        why += path;
        return false;
    }
    return true;
}

std::string SourceFile::output_rel() const {
    if (!class_dir.empty()) {
        return class_dir + '/' + name + CLASS_EXT;
    }
    return name + CLASS_EXT;
}

Install::Install(std::string_view root_dir, const std::vector<std::string>& exclude_prefixes)
    : root(abspath(root_dir)) {
    for (const std::string& x : exclude_prefixes) {
        exclude.push_back(lower(x));
    }
}

bool Install::skipped(std::string_view dir_name) const {
    const std::string low = lower(dir_name);
    for (const std::string& x : exclude) {
        if (starts_with(low, x)) {
            return true;
        }
    }
    return false;
}

void Install::scan() {
    Walk walk{*this, {}, {}, {}};
    walk.directory(to_path(root), root, std::string());
    // the sorts are stable: files with the same key keep the order of the walk
    std::stable_sort(walk.found_classes.begin(), walk.found_classes.end(), key_less_class);
    std::stable_sort(walk.found_sources.begin(), walk.found_sources.end(), key_less_source);
    for (FoundClass& c : walk.found_classes) {
        class_files.push_back(std::move(c.rel));
    }
    for (FoundSource& s : walk.found_sources) {
        const std::map<IndexKey, std::string>::const_iterator hit = walk.by_key.find(s.key);
        if (hit != walk.by_key.end()) {
            s.sf.has_class = true;
            s.sf.class_rel = hit->second;
        }
        sources.push_back(std::move(s.sf));
    }
}

std::string Install::path(std::string_view rel) const {
    return join(root, replaced(rel, '/', SEP));
}

bool Install::read(std::string_view rel, std::string& data, std::string& why) const {
    return read_file(path(rel), data, why);
}

SourceFile Install::external_source(std::string_view path_given) const {
    SourceFile sf;
    sf.path = abspath(path_given);
    size_t cut = sf.path.size();
    while (cut > 0 && !is_separator(sf.path[cut - 1])) {
        cut -= 1;
    }
    sf.name = sf.path.substr(cut);
    if (ends_with(lower(sf.name), SOURCE_EXT)) {
        sf.name.resize(sf.name.size() - (sizeof(SOURCE_EXT) - 1));
    }
    sf.rel = replaced(sf.path, '\\', '/');
    sf.external = true;
    return sf;
}

const SourceFile* Install::find_source(std::string_view path_given) const {
    const std::string want0 = norm_key(path_given);
    const std::string want1 = norm_key(join_path(root, path_given));
    for (const SourceFile& sf : sources) {
        const std::string have = lower(replaced(sf.path, '/', SEP));
        if (have == want0 || have == want1) {
            return &sf;
        }
    }
    return nullptr;
}

bool read_source(const SourceFile& sf, std::string& data, std::string& why) {
    return read_file(sf.path, data, why);
}

// ---------------------------------------------------------------------------
// the classes of a tree
// ---------------------------------------------------------------------------

namespace {

// rel.rpartition('/')[2][:-len(CLASS_EXT)]: the file name of a class file
// without its extension.
std::string_view class_file_name(std::string_view rel) {
    const size_t slash = rel.rfind('/');
    std::string_view name = slash == std::string_view::npos ? rel : rel.substr(slash + 1);
    const size_t ext = sizeof(CLASS_EXT) - 1;
    return name.size() >= ext ? name.substr(0, name.size() - ext) : std::string_view();
}

// name.rpartition('.')[2]: the simple name of a class.
std::string_view simple_name(std::string_view name) {
    const size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(dot + 1);
}

}  // namespace

InstallProvider::InstallProvider(Install& tree, bool strict_mode) : install(tree), strict(strict_mode) {}

void InstallProvider::load() {
    Install& inst = install;
    std::map<std::string, bool> rejected;       // class files that define less than they should
    std::string data;
    std::string why;
    for (const std::string& rel : inst.class_files) {
        if (!inst.read(rel, data, why)) {
            unreadable.push_back(Unreadable{rel, "cannot be read: " + why});
            rejected[rel] = true;
            continue;
        }
        const classfile::ReadResult res = classfile::read(data);
        if (!res.ok) {
            unreadable.push_back(Unreadable{rel, res.error});
            rejected[rel] = true;
            continue;
        }
        builds[rel] = res.blobs[0].build;
        const std::string_view file_name = class_file_name(rel);
        for (const classfile::BlobData& blob : res.blobs) {
            classes::ClassInfo info;
            if (!classfile::class_info(blob, info)) {
                unreadable.push_back(Unreadable{
                    rel, "a row or the CLSS record names a pool entry of the wrong kind"});
                rejected[rel] = true;
                continue;
            }
            class_count += 1;
            const std::map<std::string, std::string, std::less<>>::const_iterator old =
                origin.find(info.name);
            if (old != origin.end()) {
                const std::string_view simple = simple_name(info.name);
                const std::string_view old_name = class_file_name(old->second);
                if (!(file_name == simple && old->second != rel && old_name != simple)) {
                    continue;
                }
            }
            origin[info.name] = rel;
            const std::string name = info.name;
            known[name] = std::move(info);
        }
    }
    for (size_t k = 0; k < inst.sources.size(); ++k) {
        SourceFile& sf = inst.sources[k];
        if (sf.has_class) {
            if (rejected.find(sf.class_rel) == rejected.end()) {
                continue;
            }
            // the class file beside the source defines nothing (or not
            // everything): the classes it lacks are known from the source
            sf.class_unreadable = true;
        }
        sf.has_declared = true;
        sf.declared.clear();
        if (read_source(sf, data, why)) {
            sf.declared = declared_names(data);
        } else {
            unreadable_sources.push_back(Unreadable{sf.rel, why});
        }
        for (const std::string& full : sf.declared) {
            if (known.find(full) == known.end() && pending.find(full) == pending.end()) {
                pending[full] = k;
            }
        }
    }
}

const classes::ClassInfo* InstallProvider::find(std::string_view fqn) {
    const std::map<std::string, classes::ClassInfo, std::less<>>::const_iterator have = known.find(fqn);
    if (have != known.end()) {
        return &have->second;
    }
    const std::map<std::string, classes::ClassInfo, std::less<>>::const_iterator stub =
        in_progress.find(fqn);
    if (stub != in_progress.end()) {
        return &stub->second;
    }
    const std::map<std::string, size_t, std::less<>>::const_iterator wanted = pending.find(fqn);
    if (wanted == pending.end()) {
        return nullptr;
    }
    const size_t index = wanted->second;
    const SourceFile& sf = install.sources[index];
    std::vector<std::string> mine;
    for (const std::string& n : sf.declared) {
        const std::map<std::string, size_t, std::less<>>::const_iterator p = pending.find(n);
        if (p != pending.end() && p->second == index) {
            mine.push_back(n);
        }
    }
    // (a source that declares one name twice has it twice in `mine`; taking a
    // name out a second time changes nothing here, where the reference ends
    // with a KeyError: notes/port_2-declarations.md)
    for (const std::string& n : mine) {
        pending.erase(n);
    }
    std::string data;
    std::string why;
    if (!read_source(sf, data, why)) {
        unreadable_sources.push_back(Unreadable{sf.rel, why});
        return nullptr;
    }
    for (const std::string& n : mine) {
        in_progress.emplace(n, classes::ClassInfo(n));
    }
    Options options;
    options.strict = strict;
    DeclareResult res = declare(data, sf.rel, *this, options);
    for (const std::string& n : mine) {
        in_progress.erase(n);
    }
    if (!res.ok) {
        failed.push_back(FailedSource{sf.rel, std::move(res.diagnostics)});
        return nullptr;
    }
    for (classes::ClassInfo& ci : res.infos) {
        if (std::find(mine.begin(), mine.end(), ci.name) != mine.end()) {
            origin.emplace(ci.name, sf.rel);
            const std::string name = ci.name;
            known.emplace(name, std::move(ci));     // (never there already: the name was pending)
        }
    }
    const std::map<std::string, classes::ClassInfo, std::less<>>::const_iterator made = known.find(fqn);
    return made != known.end() ? &made->second : nullptr;
}

// ---------------------------------------------------------------------------
// the packs of a tree
// ---------------------------------------------------------------------------

namespace {

// struct.unpack_from('<I', data, at)
uint32_t u32_at(std::string_view data, size_t at) {
    return static_cast<uint32_t>(static_cast<unsigned char>(data[at])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[at + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[at + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[at + 3])) << 24);
}

bool is_pack_head(std::string_view head) {
    return head.size() >= PACK_HEAD && head.substr(0, 4) == PACK_MAGIC;
}

// n1 + n2 of a pack head: the number of records of its table (the sum of two
// 32-bit numbers does not fit 32 bits).
uint64_t record_count(std::string_view head) {
    return static_cast<uint64_t>(u32_at(head, 8)) + u32_at(head, 12);
}

// The name of one record of PACK_RECORD bytes: the bytes behind its first
// four, up to the first NUL.
std::string record_name(std::string_view record) {
    const std::string_view name = record.substr(PACK_NAME_AT, PACK_RECORD - PACK_NAME_AT);
    size_t end = 0;
    while (end < name.size() && name[end] != 0) {
        end += 1;
    }
    return std::string(name.substr(0, end));
}

}  // namespace

std::vector<std::string> external_names(std::string_view head, std::string_view records) {
    std::vector<std::string> out;
    if (!is_pack_head(head)) {
        return out;
    }
    const uint64_t count = record_count(head);
    size_t at = 0;
    uint64_t k = 0;
    while (k < count && PACK_RECORD <= records.size() - at) {
        out.push_back(record_name(records.substr(at, PACK_RECORD)));
        at += PACK_RECORD;
        k += 1;
    }
    return out;
}

DiskPackFiles::DiskPackFiles(std::string_view root_dir, const std::vector<std::string>& listing)
    : root_(root_dir) {
    for (const std::string& p : listing) {
        listed_.emplace(packs::file_key(p), p);     // (the first file of a key stays)
    }
}

bool DiskPackFiles::externals(std::string_view name, std::vector<std::string>& names) {
    const std::string key = packs::file_key(name);
    const std::map<std::string, std::vector<std::string>, std::less<>>::const_iterator known =
        tables_.find(key);
    if (known != tables_.end()) {
        names = known->second;
        return true;
    }
    names.clear();
    const std::map<std::string, std::string, std::less<>>::const_iterator listed = listed_.find(key);
    if (listed == listed_.end()) {
        return false;
    }
    std::ifstream f(to_path(join(root_, replaced(listed->second, '\\', SEP))), std::ios::binary);
    if (!f) {
        return false;                   // it cannot be opened; asked again the next time
    }
    char head_bytes[PACK_HEAD];
    f.read(head_bytes, static_cast<std::streamsize>(PACK_HEAD));
    const std::string_view head(head_bytes, static_cast<size_t>(f.gcount()));
    std::vector<std::string> found;
    if (is_pack_head(head)) {
        // never more than the file can hold.  (The reference reads the table
        // in one piece and hands it to external_names(); here it is read
        // record by record, so that a head with a wrong count costs no
        // memory.  The names are the same.)
        f.clear();
        f.seekg(0, std::ios::end);
        const std::streamoff size = f.tellg();
        uint64_t n = record_count(head);
        const uint64_t room =
            size > static_cast<std::streamoff>(PACK_HEAD)
                ? static_cast<uint64_t>(size - static_cast<std::streamoff>(PACK_HEAD)) / PACK_RECORD
                : 0;
        if (room < n) {
            n = room;
        }
        f.seekg(static_cast<std::streamoff>(PACK_HEAD), std::ios::beg);
        char record[PACK_RECORD];
        for (uint64_t k = 0; k < n; ++k) {
            f.read(record, static_cast<std::streamsize>(PACK_RECORD));
            if (static_cast<size_t>(f.gcount()) != PACK_RECORD) {
                break;
            }
            found.push_back(record_name(std::string_view(record, PACK_RECORD)));
        }
    }
    if (f.bad()) {
        return false;
    }
    names = found;
    tables_.emplace(key, std::move(found));
    return true;
}

InstallPacks::InstallPacks(const Install& install)
    : files(install.root, install.pack_files),
      table(packs::session_940(files, install.pack_files)) {}

}  // namespace install
}  // namespace slrrc
