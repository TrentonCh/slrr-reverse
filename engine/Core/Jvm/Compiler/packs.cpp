// packs.cpp - the pack of a resource id: the name and the id the pool gets.
// (Reference: slrrc/packs.py; the functions stand in the order of that file
// and carry its names.)
//
// WHERE THE C++ DIFFERS IN FORM FROM THE REFERENCE
//
//   - "A name or None" is a bool and a text (canonical, PackFiles::externals,
//     pool_entry) or a pointer (name_of); the pair (why, name) of external()
//     is a PackWhy and a text.
//   - PackTable::load() collects the record numbers of the external
//     references in a local vector and stores it when all are there:
//     find_or_add() appends to `records`, so no reference to a record is held
//     across it (the reference appends to the list of the record object).
//     For the same reason external() reads the number of the k-th reference
//     before it loads that pack.
//   - Texts are bytes: a "character" of a name is a byte, the 63 characters a
//     record keeps are 63 bytes.
#include "packs.hpp"

#include <algorithm>

namespace slrrc {
namespace packs {

const char* origin_name(uint32_t origin) {
    switch (origin) {
    case O_BOOT: return "boot";
    case O_OPEN: return "openLib";
    case O_EXTERNAL: return "external table";
    case O_LITERAL: return "literal";
    case O_NODE: return "node";
    case O_DATA: return "data file";
    default: return "?";
    }
}

bool PackNames::canonical(std::string_view written_path, std::string& file_name) {
    file_name = written_path;
    return true;
}

PackWhy PackNames::external(std::string_view written_path, uint32_t k, std::string& file_name) {
    (void)written_path;
    (void)k;
    file_name.clear();
    return PackWhy::Unknown;
}

Answer resolve(PackNames& names, std::string_view written_path, uint32_t number) {
    std::string name;
    if (!names.canonical(written_path, name)) {
        return Answer{NO_FILE_PACK, NO_FILE_ID, PackWhy::NoFile};
    }
    const uint32_t lo = number & 0xFFFF;
    const uint32_t hi = (number >> 16) & 0xFFFF;
    if (lo == 0) {
        return Answer{NO_FILE_PACK, NO_FILE_ID, PackWhy::Zero};
    }
    if (hi == 0) {
        return Answer{std::move(name), lo, PackWhy::Ok};
    }
    std::string other;
    const PackWhy why = names.external(written_path, hi, other);
    if (why == PackWhy::External) {
        return Answer{std::move(other), lo, PackWhy::External};
    }
    return Answer{NO_FILE_PACK, NO_FILE_ID, why};
}

CachedPackNames::CachedPackNames(PackNames& inner_names) : inner(inner_names) {}

bool CachedPackNames::canonical(std::string_view written_path, std::string& file_name) {
    const std::map<std::string, Name, std::less<>>::const_iterator known = answers.find(written_path);
    if (known != answers.end()) {
        file_name = known->second.name;
        return known->second.found;
    }
    Name answer;
    answer.found = inner.canonical(written_path, answer.name);
    if (!answer.found) {
        answer.name.clear();
    }
    file_name = answer.name;
    const bool found = answer.found;
    answers.emplace(std::string(written_path), std::move(answer));
    return found;
}

PackWhy CachedPackNames::external(std::string_view written_path, uint32_t k, std::string& file_name) {
    std::pair<std::string, uint32_t> key(std::string(written_path), k);
    const std::map<std::pair<std::string, uint32_t>, External>::const_iterator known =
        externals.find(key);
    if (known != externals.end()) {
        file_name = known->second.name;
        return known->second.why;
    }
    External answer;
    answer.why = inner.external(written_path, k, answer.name);
    file_name = answer.name;
    const PackWhy why = answer.why;
    externals.emplace(std::move(key), std::move(answer));
    return why;
}

std::string pack_path(std::string_view written) {
    std::string out(written);
    for (char& ch : out) {
        if (ch == '.') {
            ch = '\\';
        }
    }
    out += PACK_EXT;
    return out;
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch + 32);
        }
    }
    return out;
}

std::string file_key(std::string_view name) {
    std::string out = lower(name);
    for (char& ch : out) {
        if (ch == '/') {
            ch = '\\';
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// the files, as the host answers for them
// ---------------------------------------------------------------------------

bool PackFiles::externals(std::string_view name, std::vector<std::string>& names) {
    (void)name;
    names.clear();
    return false;
}

ListedPackFiles::ListedPackFiles(const std::vector<std::string>& files,
                                 const std::vector<ListedTable>& tables) {
    for (const std::string& p : files) {
        tables_[file_key(p)].clear();
    }
    for (const ListedTable& t : tables) {
        tables_[file_key(t.first)] = t.second;
    }
}

bool ListedPackFiles::externals(std::string_view name, std::vector<std::string>& names) {
    const std::map<std::string, std::vector<std::string>, std::less<>>::const_iterator hit =
        tables_.find(file_key(name));
    if (hit == tables_.end()) {
        names.clear();
        return false;
    }
    names = hit->second;
    return true;
}

// ---------------------------------------------------------------------------
// the pack table of the resource engine
// ---------------------------------------------------------------------------

PackTable::PackTable(PackFiles& pack_files) : files(pack_files) {
    find_or_add(SYSTEM_PACK, O_BOOT);
}

uint32_t PackTable::find(std::string_view name) const {
    const std::map<std::string, uint32_t, std::less<>>::const_iterator hit = index.find(lower(name));
    return hit != index.end() ? hit->second : NONE;
}

uint32_t PackTable::find_or_add(std::string_view name, uint32_t origin) {
    uint32_t i = find(name);
    if (i != NONE) {
        return i;
    }
    i = static_cast<uint32_t>(records.size());
    std::string_view kept = name;
    if (kept.size() > RECORD_NAME_MAX) {
        kept = kept.substr(0, RECORD_NAME_MAX);
    }
    PackRecord rec;
    rec.name = kept;
    rec.origin = origin;
    records.push_back(std::move(rec));
    index.emplace(lower(kept), i);      // (the first record of a name stays)
    return i;
}

bool PackTable::load(uint32_t i) {
    if (!records[i].loaded) {
        std::vector<std::string> names;
        if (!files.externals(records[i].name, names)) {
            return false;
        }
        records[i].loaded = true;
        std::vector<uint32_t> ext;
        ext.push_back(i);
        for (const std::string& n : names) {
            ext.push_back(find_or_add(n, O_EXTERNAL));
        }
        records[i].ext = std::move(ext);
    }
    return true;
}

uint32_t PackTable::open_lib(std::string_view name) {
    const uint32_t i = find_or_add(name, O_OPEN);
    load(i);
    return i;
}

const std::string* PackTable::name_of(std::string_view lookup) const {
    const uint32_t i = find(lookup);
    if (i == NONE) {
        return nullptr;
    }
    return &records[i].name;
}

// -- the questions of the compiler -------------------------------------------

bool PackTable::canonical(std::string_view written_path, std::string& file_name) {
    const uint32_t i = find_or_add(written_path, O_LITERAL);
    if (!load(i)) {
        file_name.clear();
        return false;
    }
    file_name = records[i].name;
    return true;
}

PackWhy PackTable::external(std::string_view written_path, uint32_t k, std::string& file_name) {
    file_name.clear();
    const uint32_t i = find_or_add(written_path, O_LITERAL);
    if (!load(i)) {
        return PackWhy::NoFile;
    }
    if (!(k < records.size() && k < records[i].ext.size())) {
        return PackWhy::NoExternal;
    }
    const uint32_t j = records[i].ext[k];
    if (!load(j)) {
        file_name = records[j].name;
        return PackWhy::Fault;
    }
    file_name = records[j].name;
    return PackWhy::External;
}

// -- the same as the exe has it, for the tests -------------------------------

uint32_t PackTable::global_id(std::string_view path, uint32_t number) {
    const uint32_t i = find_or_add(path, O_LITERAL);
    if (!load(i)) {
        return 0;
    }
    const uint32_t lo = number & 0xFFFF;
    const uint32_t hi = (number >> 16) & 0xFFFF;
    if (lo == 0 || !(hi < records.size())) {
        return 0;
    }
    if (hi == 0) {
        return (i << 16) | lo;
    }
    const std::vector<uint32_t>& ext = records[i].ext;
    if (hi < ext.size()) {
        return (ext[hi] << 16) | lo;
    }
    return 0;
}

bool PackTable::pool_entry(uint32_t gid, std::string& name, uint32_t& id) {
    const uint32_t j = gid >> 16;
    if (j >= records.size()) {
        return false;                   // no such record: see the header
    }
    if (j != 0 && !load(j)) {
        return false;
    }
    name = records[j].name;
    id = gid & 0xFFFF;
    return true;
}

// ---------------------------------------------------------------------------
// a session without the game: boot and the scans of GameLogic
// ---------------------------------------------------------------------------

namespace {

const size_t EXT_LENGTH = sizeof(PACK_EXT) - 1;

bool ends_with_ext(std::string_view name) {
    const size_t n = name.size();
    return n >= EXT_LENGTH && lower(name.substr(n - EXT_LENGTH)) == PACK_EXT;
}

// path.replace('/', '\\')
std::string with_backslashes(std::string_view path) {
    std::string out(path);
    for (char& ch : out) {
        if (ch == '/') {
            ch = '\\';
        }
    }
    return out;
}

// sorted(out, key=lower): by the lower-case names; names that are equal that
// way keep their order.
void sort_by_lower(std::vector<std::string>& names) {
    std::stable_sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        return lower(a) < lower(b);
    });
}

}  // namespace

std::vector<std::string> listed_packs(const std::vector<std::string>& listing,
                                      std::string_view directory) {
    const std::string d = file_key(directory);
    std::vector<std::string> out;
    for (const std::string& path : listing) {
        const std::string p = with_backslashes(path);
        if (p.size() > d.size() && lower(std::string_view(p).substr(0, d.size())) == d) {
            const std::string_view rest = std::string_view(p).substr(d.size());
            if (rest.find('\\') == std::string_view::npos && ends_with_ext(rest)) {
                out.push_back(std::string(rest));
            }
        }
    }
    sort_by_lower(out);
    return out;
}

std::vector<std::string> listed_directories(const std::vector<std::string>& listing,
                                            std::string_view directory) {
    const std::string d = file_key(directory);
    std::map<std::string, bool> seen;
    std::vector<std::string> out;
    for (const std::string& path : listing) {
        const std::string p = with_backslashes(path);
        if (p.size() > d.size() && lower(std::string_view(p).substr(0, d.size())) == d) {
            const std::string_view rest = std::string_view(p).substr(d.size());
            size_t cut = 0;                     // the length of the first component
            while (cut < rest.size() && rest[cut] != '\\') {
                cut += 1;
            }
            if (cut > 0 && cut < rest.size()) {
                const std::string_view sub = rest.substr(0, cut);
                if (seen.find(lower(sub)) == seen.end()) {
                    seen[lower(sub)] = true;
                    out.push_back(std::string(sub));
                }
            }
        }
    }
    sort_by_lower(out);
    return out;
}

uint32_t rpk_scan(PackTable& table, const std::vector<std::string>& listing,
                  std::string_view directory) {
    const std::vector<std::string> names = listed_packs(listing, directory);
    for (const std::string& name : names) {
        std::string full(directory);
        full += name;
        table.open_lib(full);
    }
    return static_cast<uint32_t>(names.size());
}

PackTable session_940(PackFiles& files, const std::vector<std::string>& listing, bool scans) {
    PackTable table(files);
    if (scans) {
        for (const char* d : SCANS_940) {
            rpk_scan(table, listing, d);
        }
        for (const char* base : SUB_SCANS_940) {
            for (const std::string& sub : listed_directories(listing, base)) {
                std::string dir(base);
                dir += sub;
                dir += '\\';
                rpk_scan(table, listing, dir);
            }
        }
    }
    return table;
}

}  // namespace packs
}  // namespace slrrc
