#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace inv {

struct RpakEntry {
  bool is_dir = false;
  // Parent resource local id (or filetype when high bits set, e.g. 0x10008).
  int32_t kind = 0;
  // Resource local id (u32 after kind in the entry header). Catalog RIDs use this.
  int32_t type_id = 0;
  std::string name;
  std::string path;
  uint32_t offset = 0;
  uint32_t size = 0;
  // Tree links store resource local ids (type_id), not table indices.
  int32_t parent_local = -1;
  int32_t first_child_local = -1;
  int32_t next_sibling_local = -1;
  // PE TOC record +8 / +9, the descriptor the ResourceNode ctor copies
  // (940 FUN_0051eaa0: node+0x7C restype, node+0x84 flags). restype:
  // 1 game instance, 2 physics instance, 3 render instance, 5 mesh,
  // 7 texture, 8 game type, 13 light type, 14 render object type,
  // 0x12 viewport. rec_flags bit 0 = twelve floats follow the name,
  // bit 0x10 = RIF_WORLDTREEROOT (31 of the 33 map instances carry 0x11).
  uint8_t restype = 0;
  uint8_t rec_flags = 0;
  // The twelve floats of rec_flags bit 0. For a game instance: box centre
  // xyz, box half extents xyz (metres), six unused. Zero without the bit.
  float bounds[12] = {};
};

// Soft PE EnsureIndex dep record @ 0x54425E (0x40B) → TOC slot @ +0x5C.
struct RpakDep {
  uint16_t id_hi = 0;   // PE HIWORD(id) → table[+0] @ 0x54426A..0x54426D
  uint32_t trailer = 0; // PE dword → table[+0x3E] @ 0x544292
  std::string name;     // PE name[56] → table[+4] @ 0x544283
};

// Soft PE remap TOC slot — ResourcePack+0x5C stride 66 (0x42) @ 0x544217.
// RemapLocalId @ 0x5445E2 reads u16 at slot+2 as ext pack HIWORD.
struct RpakRemapSlot {
  uint16_t id_hi = 0;    // +0
  uint16_t pack_hi = 0;  // +2 — LoadPack AX / own pack idx (slot0)
  uint32_t trailer = 0;  // +0x3E
};

struct RpakPack {
  // Small integer pack id (1, 2, …). Full resource ids are (pack_id<<16)|local.
  // Matches Catalog: (parts.id() >> 16) == System.openLib(...).
  int32_t pack_id = 0;
  std::string path;
  std::string name;
  uint32_t version = 0;
  // PE file +8/+0xC → pack+0x54/+0x58; RemapLocalId lim = a+b @ 0x5445CC.
  uint32_t toc_lim_a = 0;
  uint32_t toc_lim_b = 0;
  std::vector<RpakDep> deps;
  // Soft PE *(pack+0x5C): slot0=self, slots1..=deps (EnsureIndex @ 0x544221).
  std::vector<RpakRemapSlot> remap_toc;
  std::vector<RpakEntry> entries;
  bool is_registry = false;
  bool parsed_entries = false;
};

void rpak_set_game_root(const char* root);

// Resolve lib/asset path against game root (and CWD fallback).
std::string rpak_resolve_path(const char* path);

// Load/open a .rpk. Returns pack_id (>0) or 0 on failure. Idempotent.
int32_t rpak_open(const char* lib_path);

const RpakPack* rpak_get(int32_t pack_id);
const RpakPack* rpak_find_by_name(const char* basename);  // "frontend.rpk" or "frontend"
// Fork: match by relative path ("multibot/maps/A1Ring/t_data.rpk").
const RpakPack* rpak_find_by_path(const char* rel_path);
size_t rpak_count();

// Decode resource id = (pack_id << 16) | type_id (entry.type_id).
const RpakEntry* rpak_find_entry(int32_t res_id);
const RpakPack* rpak_find_pack_for_res(int32_t res_id);

// Soft PE ResPack_RemapLocalId @ 0x544590 — pack_id ≡ slot+8;
// hi!=0 → remap_toc[hi].pack_hi << 16 | lo (table @ +0x5C).
uint32_t rpak_remap_local_id(int32_t pack_id, uint32_t local_id);

// Parent key for hierarchy: scripted car nodes use kind=0x2xxxx → parent is low 16 bits
// (e.g. Baiern_VT kind=0x21000 → parent cars:0x1000).
inline int32_t rpak_parent_key(int32_t kind) {
  if ((static_cast<uint32_t>(kind) >> 16) == 0x2u)
    return kind & 0xFFFF;
  return kind;
}

// Cross-pack world-tree walk (VehicleType roots live in racer packs under cars:0x1000).
int32_t rpak_first_child_id(int32_t parent_res_id);
int32_t rpak_next_sibling_id(int32_t child_res_id);
int32_t rpak_parent_id(int32_t child_res_id);

// Read raw entry bytes (re-opens pack file). Returns false if missing/dir/empty.
bool rpak_read_entry(int32_t res_id, std::vector<uint8_t>* out);

// Read `size` bytes at the absolute file offset `offset` of an open pack.
// The nested nodes of a game instance carry file offsets, not ids (940
// FUN_00519940 child records). Re-opens the pack file like rpak_read_entry,
// so a reader that walks many nodes asks for one range that covers them.
// A range that runs past the end of the file is cut there. Returns false
// when the pack is unknown or no byte could be read.
bool rpak_read_range(int32_t pack_id, uint32_t offset, uint32_t size,
                     std::vector<uint8_t>* out);

inline int32_t rpak_make_id(int32_t pack_id, uint16_t local) {
  return (pack_id << 16) | static_cast<int32_t>(local);
}
inline int32_t rpak_id_pack(int32_t res_id) { return (res_id >> 16) & 0xFFFF; }
inline uint16_t rpak_id_local(int32_t res_id) {
  return static_cast<uint16_t>(res_id & 0xFFFF);
}

}  // namespace inv
