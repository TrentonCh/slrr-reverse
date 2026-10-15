// Native game types of Build 940 and the game instances made from them.
// See include/NativeTypes.hpp for what stands for what. Script boot only.
//
// Mirrors, by section:
//   engine_sscanf        FUN_00569380 -> FUN_00527140 (+ FUN_00527420 decimal,
//                        FUN_00527490 hex, FUN_00527530 float)
//   gametype_desc        FUN_0051fcc0 (typeof-8 payload), table 0x5DA3E8
//   native objects       the type vftables (`_type_ground` 0x59E464,
//                        `_type_camera` 0x59B13C) and FUN_00523c80
//   instance_tree_read   FUN_00519940 (typeof-1 payload) + FUN_0051e6b0
//   instance_node_*      FUN_005195a0 (game object of a pack instance)
//   ground               FUN_004bade0 (create), FUN_004bb570 (getinfo)
//   render objects       NativeTypes_part3.inc: FUN_00522d90 (payload),
//                        FUN_0046d890 (template class)
//   ground mesh          NativeTypes_part3.inc: FUN_0046df30, FUN_004658b0,
//                        FUN_00441c40
//   camera               NativeTypes_part2.inc: FUN_00489d30 (create),
//                        FUN_0048bce0 (commands), FUN_004897b0 (control),
//                        FUN_0048a430 (drive)
//   part                 NativeTypes_part4.inc: FUN_004f6240 (cfg lines),
//                        FUN_004c1f90 (create), FUN_004c2b10 (render
//                        instances), FUN_004c6370 (setMatrix)
//   car                  NativeTypes_part6.inc: FUN_0049ab20 (init, the car
//                        words of the cfg), FUN_004992e0 (create),
//                        FUN_004a99b0 (setMatrix), FUN_004aa830 (steer
//                        angles), FUN_004aaaf0 (wheel bodies), FUN_004a2e60
//                        (commands), FUN_004a15c0 (getinfo)
//   script natives       NativeTypes_part7.inc: the Part, Chassis and
//                        WheelRef natives that answer from a part or car
//                        object (Part.setSlotPos 0x4CDDC0, partOnSlot
//                        0x4CD600, setRenderType 0x4CD9E0, ...)
//   level of detail,     NativeTypes_part8.inc: FUN_00461980 (the detail a
//   linked renders       camera gives a node), FUN_0051a5f0 (level change),
//                        the tails of FUN_004c2b10 / FUN_004ca390 and
//                        FUN_004afbf0 (`linked` render entries)
//   light types          NativeTypes_part4.inc: FUN_005219c0 (typeof-13
//                        payload), FUN_004ebef0 (RenderRef.setLight)
//   player, cursor       NativeTypes_part9.inc: FUN_004d2370 / FUN_004d28d0
//                        (the player's create and getinfo), FUN_004b7a10
//                        (the cursor's create), FUN_004b7d70 (commands),
//                        FUN_004b78f0 and the head of FUN_004b8430 (the
//                        animate tick), FUN_004b7c60 (getinfo)
//   animate walk         FUN_0047ba10 (native_tick_animate)

#include "NativeTypes.hpp"

#include "host_objects.hpp"
#include "input_win32.hpp"
#include "jvm.hpp"
#include "natives.hpp"
#include "render_d3d9.hpp"
#include "rpak.hpp"
#include "runtime.hpp"
#include "tree_interp.hpp"
#include "tree_interp_internal.hpp"  // tree_static_slot (Config statics by name)
#include "GameRef_internal.hpp"
#include "Resources.h"
#include "Resources_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace inv {

bool native_script_boot() {
  // The gate of GameRef.create's script branch (GameRef_core.cpp): the legacy
  // C++ harness shares every native this file hooks and must not change.
  static const bool on = [] {
    const char* e = std::getenv("SLRR_PE_BOOT_INIT");
    return e && e[0] == '1';
  }();
  return on;
}

bool native_type_log() {
  static const bool on = [] {
    const char* e = std::getenv("SLRR_PE_NATIVE_PROF");
    return e && e[0] && e[0] != '0';
  }();
  return on;
}

// ---------------------------------------------------------------------------
// The engine's scanner
// ---------------------------------------------------------------------------
namespace {

// PE 940 FUN_00527140 tests `'\b' < c && c < '\x0e' || c == ' '` on a signed
// char, so bytes above 0x7F are never white space.
inline bool scan_ws(unsigned char c) { return (c > 8 && c < 14) || c == 0x20; }
inline bool scan_digit(unsigned char c) { return c >= '0' && c <= '9'; }

// PE 940 FUN_00527420: optional '-', then at least one digit; the value
// wraps in 32 bits; %u stores it without the sign.
bool scan_dec(const unsigned char** ps, bool is_unsigned, int32_t* dest) {
  const unsigned char* p = *ps;
  const bool neg = (*p == '-');
  if (neg) ++p;
  if (!scan_digit(*p)) return false;
  uint32_t v = 0;
  while (scan_digit(*p)) {
    v = v * 10u + static_cast<uint32_t>(*p - '0');
    ++p;
  }
  if (dest) {
    if (is_unsigned || !neg)
      *dest = static_cast<int32_t>(v);
    else
      *dest = static_cast<int32_t>(0u - v);
  }
  *ps = p;
  return true;
}

// PE 940 FUN_00527490: "0x" / "0X" and at least one hex digit, or no match.
bool scan_hex(const unsigned char** ps, int32_t* dest) {
  const unsigned char* p = *ps;
  auto hex = [](unsigned char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  if (!(p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))) return false;
  p += 2;
  if (hex(*p) < 0) return false;
  uint32_t v = 0;
  while (hex(*p) >= 0) {
    v = v * 16u + static_cast<uint32_t>(hex(*p));
    ++p;
  }
  if (dest) *dest = static_cast<int32_t>(v);
  *ps = p;
  return true;
}

// PE 940 FUN_00527530: optional '-', digits (as a 32-bit integer), then an
// optional '.' and up to `prec` digits, each added as digit * 0.1f^n in
// single precision; digits past `prec` are skipped. No '+', no exponent.
bool scan_float(const unsigned char** ps, float* dest, int prec) {
  const unsigned char* p = *ps;
  const bool neg = (*p == '-');
  if (neg) ++p;
  if (!scan_digit(*p)) return false;
  uint32_t iv = 0;
  while (scan_digit(*p)) {
    iv = iv * 10u + static_cast<uint32_t>(*p - '0');
    ++p;
  }
  // (double)(int)iv + (iv < 0 ? 2^32 : 0): the unsigned value.
  float v = static_cast<float>(static_cast<double>(iv));
  if (*p == '.') {
    ++p;
    float scale = 0.1f;  // DAT_005935d4
    while (scan_digit(*p) && prec > 0) {
      const float term = static_cast<float>(*p - '0') * scale;
      scale = scale * 0.1f;
      v = v + term;
      ++p;
      --prec;
    }
    while (scan_digit(*p)) ++p;
  }
  if (neg) v = -v;
  if (dest) *dest = v;
  *ps = p;
  return true;
}

int engine_vsscanf(const char* input, const char* format, va_list ap) {
  if (!input || !format) return 0;
  const unsigned char* s = reinterpret_cast<const unsigned char*>(input);
  const unsigned char* f = reinterpret_cast<const unsigned char*>(format);
  int count = 0;
  bool report_consumed = false;  // "%#": return the number of bytes read
  while (*f) {
    if (scan_ws(*f)) {
      while (scan_ws(*s)) ++s;
      while (scan_ws(*f)) ++f;
    }
    if (*f != '%') {
      if (*f != *s) return count;
      // Fork: a format that ends in white space meets the end of the input
      // here; the exe steps over both terminators and reads on.
      if (*f == 0) break;
      ++f;
      ++s;
      continue;
    }
    if (f[1] == '#') {
      f += 2;
      report_consumed = true;
      continue;
    }
    const bool suppress = (f[1] == '*');
    const unsigned char* p = f + (suppress ? 2 : 1);
    int width = 0;
    while (scan_digit(*p)) {
      width = width * 10 + (*p - '0');
      ++p;
    }
    if (width == 0) width = 0x400;
    int prec = 0;
    if (*p == '.') {
      ++p;
      while (scan_digit(*p)) {
        prec = prec * 10 + (*p - '0');
        ++p;
      }
    }
    if (prec == 0) prec = 0x400;
    if (*p == 'h' || *p == 'l' || *p == 'L') ++p;
    if (*p == 0) return count;  // Fork: a format cut inside a conversion
    const unsigned char conv = static_cast<unsigned char>(*p | 0x20);
    f = p + 1;
    void* dest = suppress ? nullptr : va_arg(ap, void*);
    switch (conv) {
      case 'c':
        if (dest) *static_cast<char*>(dest) = static_cast<char>(*s);
        if (*s) ++s;  // Fork: the exe also steps over the terminator
        ++count;
        break;
      case 'f':
        while (scan_ws(*s)) ++s;
        if (!scan_float(&s, static_cast<float*>(dest), prec)) return count;
        ++count;
        break;
      case 's': {
        while (scan_ws(*s)) ++s;
        char* d = static_cast<char*>(dest);
        // The word also ends at the format character after the conversion.
        while (!scan_ws(*s) && *s != 0 && width > 0 && *s != *f) {
          if (d) *d++ = static_cast<char>(*s);
          ++s;
          --width;
        }
        if (d) *d = 0;
        ++count;
        break;
      }
      case 'u':
      case 'd': {
        while (scan_ws(*s)) ++s;
        bool ok;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
          ok = scan_hex(&s, static_cast<int32_t*>(dest));
        else
          ok = scan_dec(&s, conv == 'u', static_cast<int32_t*>(dest));
        if (!ok) return count;
        ++count;
        break;
      }
      case 'x':
        while (scan_ws(*s)) ++s;
        if (!scan_hex(&s, static_cast<int32_t*>(dest))) return count;
        ++count;
        break;
      default:
        break;
    }
  }
  if (report_consumed)
    return static_cast<int>(s - reinterpret_cast<const unsigned char*>(input));
  return count;
}

// PE 940 FUN_005281b0: one line into `line`, the '\n' included; stops at the
// NUL. Returns the bytes consumed (terminator included). Fork: bounded by
// `end`; the exe trusts a NUL after the payload.
size_t read_line(const uint8_t* p, const uint8_t* end, std::string* line) {
  line->clear();
  size_t n = 0;
  while (p + n < end) {
    const char c = static_cast<char>(p[n]);
    ++n;
    if (c == '\0') return n;
    line->push_back(c);
    if (c == '\n') return n;
  }
  return n;
}

// PE 940 FUN_0057c11e is the CRT's _stricmp: keywords and type names match
// without case.
bool word_is(const char* a, const char* b) {
#ifdef _WIN32
  return _stricmp(a, b) == 0;
#else
  for (; *a && *b; ++a, ++b) {
    const int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
    const int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
    if (ca != cb) return false;
  }
  return *a == *b;
#endif
}

inline uint32_t rd32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

}  // namespace

int engine_sscanf(const char* input, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  const int n = engine_vsscanf(input, format, ap);
  va_end(ap);
  return n;
}

// ---------------------------------------------------------------------------
// Game types
// ---------------------------------------------------------------------------
namespace {

std::shared_ptr<NativeObject> ground_create(const NativeCreate& c);
std::shared_ptr<NativeObject> camera_create(const NativeCreate& c);
std::shared_ptr<NativeObject> part_create(const NativeCreate& c);
std::shared_ptr<NativeObject> car_create(const NativeCreate& c);
std::shared_ptr<NativeObject> player_create(const NativeCreate& c);
std::shared_ptr<NativeObject> cursor_create(const NativeCreate& c);

using NativeFactory = std::shared_ptr<NativeObject> (*)(const NativeCreate&);
struct NativeTypeRow {
  const char* name;
  NativeFactory create;  // null = no host implementation: behaves as before
};
// PE 940 table at 0x5DA3E8 ({name, factory} pairs, count DAT_005da460 = 15):
// ground 0x4bb9a0, camera 0x48cb30, object 0x4bf190, car 0x4b17a0, part
// 0x4ca1f0, trigger 0x4d4c90, player 0x4d3c30, button 0x489200, cursor
// 0x4b9b80, bot 0x484990, road 0x4d45a0, walk 0x4d59b0, trlight 0x4d5460,
// ped 0x4cf190, building 0x4883f0.
const NativeTypeRow kNativeTypes[kNativeTypeCount] = {
    {"ground", &ground_create},
    {"camera", &camera_create},
    {"object", nullptr},
    {"car", &car_create},
    {"part", &part_create},
    {"trigger", nullptr},
    {"player", &player_create},
    {"button", nullptr},
    {"cursor", &cursor_create},
    {"bot", nullptr},
    {"road", nullptr},
    {"walk", nullptr},
    {"trlight", nullptr},
    {"ped", nullptr},
    {"building", nullptr},
};

int native_type_find(const char* name) {
  for (int i = 0; i < kNativeTypeCount; ++i)
    if (word_is(name, kNativeTypes[i].name)) return i;
  return -1;
}

std::mutex g_nt_mu;  // guards the maps below; never held across a call out
std::unordered_map<int32_t, std::unique_ptr<GameTypeDesc>> g_type_descs;

// PE 940 FUN_0051fcc0: the payload is walked as chunks {tag, size, data}
// for the five binary tags and as one text line anywhere else.
void parse_gametype_payload(const std::vector<uint8_t>& blob, GameTypeDesc* d) {
  constexpr uint32_t kICLA = 0x414C4349, kICFG = 0x47464349;
  constexpr uint32_t kXCLA = 0x414C4358, kXCFG = 0x47464358;
  constexpr uint32_t kLODA = 0x41444F4C;
  // The exe copies the payload and puts a NUL after it.
  std::vector<uint8_t> buf(blob);
  buf.push_back(0);
  const size_t n = blob.size();
  const uint8_t* base = buf.data();
  char word[1025];
  char arg[1025];
  size_t pos = 0;
  std::string line;
  while (pos < n) {
    const uint32_t tag = (pos + 4 <= n) ? rd32(base + pos) : 0u;
    const bool is_chunk = tag == kICLA || tag == kICFG || tag == kXCLA ||
                          tag == kXCFG || tag == kLODA;
    if (is_chunk) {
      if (pos + 8 > n) break;
      const uint32_t size = rd32(base + pos + 4);
      const size_t data = pos + 8;
      if (size > n - data) break;  // Fork: a cut chunk ends the walk
      // Chunk text is NUL-terminated inside the chunk in the shipped packs;
      // a copy with its own terminator keeps the scans inside the chunk.
      const std::string text(reinterpret_cast<const char*>(base + data), size);
      if (tag == kICLA) {
        // FUN_00554ee0(data, size): a class from memory -> type+0x10.
        // Fork: not loaded (no shipped pack has the chunk).
        d->has_script = true;
        d->script_is_inline = true;
      } else if (tag == kXCLA) {
        // "%s" -> FUN_0055add0(name): a class by name -> type+0x10.
        word[0] = 0;
        engine_sscanf(text.c_str(), "%s", word);
        if (word[0]) {
          d->has_script = true;
          d->script_is_inline = false;
          d->script_arg = word;
        }
      } else if (tag == kICFG || tag == kXCFG) {
        // "%s" -> the type name, looked up in the table. ICFG: the cfg text
        // follows the name's terminator. XCFG: a cfg file; the exe scans
        // its path with "%*s %*s %s" from the line buffer of the last text
        // line (DAT_007a7798), not from the chunk. Fork: the host takes the
        // second word of the chunk (objects.rpk:0x70 `object objects\
        // scripts\area_175_egyedi.cfg`, the only XCFG shipped).
        word[0] = 0;
        engine_sscanf(text.c_str(), "%s", word);
        const int row = word[0] ? native_type_find(word) : -1;
        if (row >= 0) {
          d->has_native = true;
          d->native_index = row;
          d->native_name = word;
          d->cfg_path.clear();
          d->cfg_inline.clear();
          d->cfg_is_inline = false;
          if (tag == kICFG) {
            const size_t skip = std::strlen(text.c_str()) + 1;
            if (skip < size) {
              d->cfg_is_inline = true;
              d->cfg_inline.assign(text, skip, std::string::npos);
            }
          } else {
            arg[0] = 0;
            engine_sscanf(text.c_str(), "%*s %s", arg);
            d->cfg_path = arg;
          }
        }
      } else {  // LODA: the float is stored as it is -> type+0x18
        if (size >= 4) {
          std::memcpy(&d->lod_amp, base + data, 4);
          d->has_lod_amp = true;
        }
      }
      pos = data + size;
      continue;
    }
    pos += read_line(base + pos, base + n + 1, &line);
    if (line.empty()) continue;
    word[0] = 0;
    engine_sscanf(line.c_str(), "%s", word);
    if (word_is(word, "script")) {
      // "%*s %s": a path ending in ".class" is split into directory and
      // class name (FUN_0055aca0), anything else is a class name
      // (FUN_0055add0). The host resolves both in script_fqn_from_script_arg.
      arg[0] = 0;
      engine_sscanf(line.c_str(), "%*s %s", arg);
      if (arg[0]) {  // else the exe logs "Missing class: "
        d->has_script = true;
        d->script_is_inline = false;
        d->script_arg = arg;
      }
    } else if (word_is(word, "native")) {
      arg[0] = 0;
      engine_sscanf(line.c_str(), "%*s %s", arg);
      if (arg[0]) {
        d->has_native = true;
        d->native_name = arg;
        d->native_index = native_type_find(arg);
        d->cfg_is_inline = false;
        d->cfg_inline.clear();
        d->cfg_path.clear();
        if (d->native_index >= 0) {
          // "%*s %*s %s": the cfg file, read whole by the exe and handed to
          // the factory as "- <buffer> <size>" ("Missing .cfg: %s" when the
          // file is not there). The host types read the file themselves.
          arg[0] = 0;
          engine_sscanf(line.c_str(), "%*s %*s %s", arg);
          d->cfg_path = arg;
        }
      }
    } else if (word_is(word, "lod_amp")) {
      if (engine_sscanf(line.c_str(), "%*s %f", &d->lod_amp) == 2)
        d->has_lod_amp = true;
    }
    // "description": kept by the exe at type+0x14, not used by the host.
  }
}

}  // namespace

const char* native_type_name(int index) {
  if (index < 0 || index >= kNativeTypeCount) return "";
  return kNativeTypes[index].name;
}

const GameTypeDesc* gametype_desc(int32_t type_id) {
  static const GameTypeDesc kNone;
  if (type_id == 0) return &kNone;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    auto it = g_type_descs.find(type_id);
    if (it != g_type_descs.end()) return it->second.get();
  }
  const RpakEntry* e = rpak_find_entry(type_id);
  if (!e || e->restype != 8) return &kNone;
  auto d = std::make_unique<GameTypeDesc>();
  d->found = true;
  d->type_id = type_id;
  std::vector<uint8_t> blob;
  if (rpak_read_entry(type_id, &blob)) parse_gametype_payload(blob, d.get());
  // One line for the first type of each native row (a new career makes a
  // car out of about ninety `native part` types before the menu shows), and
  // for the first native name the table lacks.
  bool log_type = false;
  if (native_type_log() && d->has_native) {
    static bool seen[kNativeTypeCount + 1] = {};
    const int row = d->native_index < 0 ? kNativeTypeCount : d->native_index;
    std::lock_guard<std::mutex> lock(g_nt_mu);
    log_type = !seen[row];
    seen[row] = true;
  }
  if (log_type)
    std::fprintf(stderr,
                 "[native-type] first type of native '%s': 0x%08X '%s' "
                 "(row %d, %s) cfg '%s'%s script '%s'%s\n",
                 d->native_name.c_str(),
                 static_cast<unsigned>(type_id), e->name.c_str(),
                 d->native_index,
                 d->native_index < 0
                     ? "none"
                     : (kNativeTypes[d->native_index].create ? "host object"
                                                             : "no host object"),
                 d->cfg_path.c_str(), d->cfg_is_inline ? " (inline)" : "",
                 d->script_arg.c_str(), d->script_is_inline ? " (inline)" : "");
  std::lock_guard<std::mutex> lock(g_nt_mu);
  auto& slot = g_type_descs[type_id];
  if (!slot) slot = std::move(d);
  return slot.get();
}

// ---------------------------------------------------------------------------
// Native objects
// ---------------------------------------------------------------------------
namespace {

// Keyed by both objects of a GameRef.create (Fork: the exe keeps the game
// object at the one node's payload+0x4C).
std::unordered_map<InvObject*, std::shared_ptr<NativeObject>> g_objects;

struct InstanceNodeState {
  int32_t id = 0;
  bool building = false;
  bool failed = false;   // the payload did not parse: not asked again
  InvObject* handle = nullptr;
  InstanceTree tree;
  std::shared_ptr<NativeObject> object;  // payload+0x4C
};
std::unordered_map<int32_t, std::unique_ptr<InstanceNodeState>> g_nodes;

// The pack instance a handle is bound to: its id when the record is typeof 1.
int32_t handle_instance_id(InvObject* handle) {
  if (!handle) return 0;
  int32_t id = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_res.find(handle);
    if (it == g_res.end() || it->second.restype != 1) return 0;
    id = it->second.id;
  }
  // A node GameRef.create made is restype 1 as well, with no pack record
  // and no payload to parse (its game object comes from the create).
  if (script_node_object(id)) return 0;
  return id;
}

// The objects that asked for the control and drive callbacks (the exe sets
// instance flags 0x14 with FUN_004ebd90 and walks the registered instances
// in FUN_0047bc20 / FUN_0047be30).
std::vector<std::shared_ptr<NativeObject>> g_ticking;
// The objects on the animate list (the exe: getinfo 0x1C at GameRef.create,
// FUN_0047d560; appended at the tail and walked from the head by
// FUN_0047ba10, so in creation order).
std::vector<std::shared_ptr<NativeObject>> g_animating;

// Caller holds g_nt_mu.
void ticking_add(const std::shared_ptr<NativeObject>& o) {
  if (o && o->ticks) g_ticking.push_back(o);
  if (o && o->animates) g_animating.push_back(o);
}
void ticking_remove(const std::shared_ptr<NativeObject>& o) {
  for (size_t i = 0; i < g_ticking.size(); ++i)
    if (g_ticking[i] == o) {
      g_ticking.erase(g_ticking.begin() + static_cast<std::ptrdiff_t>(i));
      break;
    }
  for (size_t i = 0; i < g_animating.size(); ++i)
    if (g_animating[i] == o) {
      g_animating.erase(g_animating.begin() + static_cast<std::ptrdiff_t>(i));
      break;
    }
}

std::vector<std::shared_ptr<NativeObject>> ticking_snapshot() {
  std::lock_guard<std::mutex> lock(g_nt_mu);
  return g_ticking;
}

std::vector<std::shared_ptr<NativeObject>> animating_snapshot() {
  std::lock_guard<std::mutex> lock(g_nt_mu);
  return g_animating;
}

}  // namespace

void native_object_create(const GameTypeDesc* type, InvObject* handle,
                          InvObject* instance, InvObject* parent,
                          const char* params) {
  if (!native_script_boot() || !type || !handle) return;
  if (type->native_index < 0 || type->native_index >= kNativeTypeCount) return;
  const NativeFactory make = kNativeTypes[type->native_index].create;
  if (!make) return;  // no host implementation: the GameRef stand-in as before
  NativeCreate c;
  c.type = type;
  c.handle = handle;
  c.instance = instance ? instance : handle;
  c.parent = parent;
  c.node_id = 0;
  c.params = params ? params : "";
  std::shared_ptr<NativeObject> obj = make(c);
  if (!obj) return;
  obj->type_index = type->native_index;
  obj->handle = c.handle;
  obj->instance = c.instance;
  obj->parent = parent;
  obj->node_id = 0;
  // A handle that is created on again leaves its old node; the old object
  // goes when neither of its two objects names it any more.
  std::vector<std::shared_ptr<NativeObject>> dropped;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    for (InvObject* key : {c.handle, c.instance}) {
      auto it = g_objects.find(key);
      if (it != g_objects.end() && it->second != obj) {
        std::shared_ptr<NativeObject> old = it->second;
        g_objects.erase(it);
        bool still = false;
        for (InvObject* other : {old->handle, old->instance}) {
          auto oi = g_objects.find(other);
          if (oi != g_objects.end() && oi->second == old) still = true;
        }
        if (!still) {
          ticking_remove(old);
          dropped.push_back(old);
        }
      }
      g_objects[key] = obj;
    }
    ticking_add(obj);
  }
  for (auto& o : dropped) o->destroy();
  if (native_type_log())
    std::fprintf(stderr,
                 "[native-type] create %s type=0x%08X handle=%p instance=%p "
                 "parent=%p params='%s'\n",
                 native_type_name(type->native_index),
                 static_cast<unsigned>(type->type_id),
                 static_cast<void*>(c.handle), static_cast<void*>(c.instance),
                 static_cast<void*>(parent), c.params);
}

std::shared_ptr<NativeObject> native_object_of(InvObject* handle) {
  if (!handle || !native_script_boot()) return nullptr;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    if (g_objects.empty() && g_nodes.empty()) return nullptr;
    auto it = g_objects.find(handle);
    if (it != g_objects.end()) return it->second;
  }
  // The game object belongs to the node (payload+0x4C): a handle bound to
  // the id of a node GameRef.create made (`new GameRef(id)`) finds it
  // through the node object.
  InvObject* const node = gameref_node_object(handle);
  if (node != handle) {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    auto it = g_objects.find(node);
    if (it != g_objects.end()) return it->second;
  }
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    if (g_nodes.empty()) return nullptr;
  }
  // The game object of a pack instance belongs to the node: any handle bound
  // to the node's id finds it.
  const int32_t id = handle_instance_id(handle);
  if (!id) return nullptr;
  std::lock_guard<std::mutex> lock(g_nt_mu);
  auto it = g_nodes.find(id);
  if (it == g_nodes.end() || !it->second) return nullptr;
  return it->second->object;
}

bool native_event(InvObject* handle, int32_t type, const char* text) {
  std::shared_ptr<NativeObject> o = native_object_of(handle);
  return o ? o->event(type, text ? text : "") : false;
}

bool native_set_matrix(InvObject* handle, const float pos[3],
                       const float ypr[3]) {
  std::shared_ptr<NativeObject> o = native_object_of(handle);
  return o ? o->set_matrix(pos, ypr) : false;
}

bool native_getinfo(InvObject* handle, int32_t query, NativeInfo* out) {
  if (!out) return false;
  std::shared_ptr<NativeObject> o = native_object_of(handle);
  return o ? o->getinfo(query, out) : false;
}

bool native_get_pos(InvObject* handle, float pos[3]) {
  // PE GameRef.getPos: query channel 2 of the game object (getinfo 2).
  NativeInfo info;
  if (!native_getinfo(handle, 2, &info)) return false;
  pos[0] = info.v[0];
  pos[1] = info.v[1];
  pos[2] = info.v[2];
  return true;
}

bool native_get_ori(InvObject* handle, float ypr[3]) {
  std::shared_ptr<NativeObject> o = native_object_of(handle);
  return o ? o->get_ori(ypr) : false;
}

bool native_owns_create_pose(InvObject* handle) {
  std::shared_ptr<NativeObject> o = native_object_of(handle);
  return o ? o->owns_create_pose : false;
}

void native_on_destroy(InvObject* handle) {
  if (!handle || !native_script_boot()) return;
  std::shared_ptr<NativeObject> obj;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    auto it = g_objects.find(handle);
    if (it == g_objects.end()) return;
    obj = it->second;
    // One node on the exe: destroying it through either handle ends it.
    g_objects.erase(it);
    for (InvObject* other : {obj->handle, obj->instance}) {
      auto oi = g_objects.find(other);
      if (oi != g_objects.end() && oi->second == obj) g_objects.erase(oi);
    }
    ticking_remove(obj);
  }
  if (native_type_log())
    std::fprintf(stderr, "[native-type] destroy %s handle=%p\n",
                 native_type_name(obj->type_index), static_cast<void*>(handle));
  obj->destroy();
}

void native_nodes_gone(const std::vector<int32_t>& ids) {
  if (ids.empty() || !native_script_boot()) return;
  std::vector<std::shared_ptr<NativeObject>> objs;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    objs.reserve(g_objects.size() + g_nodes.size());
    for (const auto& kv : g_objects)
      if (kv.second) objs.push_back(kv.second);
    for (const auto& kv : g_nodes)
      if (kv.second && kv.second->object) objs.push_back(kv.second->object);
  }
  // An object is listed once per host object that names it; being told
  // twice changes nothing.
  for (auto& o : objs) o->nodes_gone(ids);
}

void native_tick_control(float dt) {
  if (!native_script_boot()) return;
  for (auto& o : ticking_snapshot()) o->control(dt);
}

void native_tick_drive(float dt) {
  if (!native_script_boot()) return;
  for (auto& o : ticking_snapshot()) o->drive(dt);
}

namespace {
double g_loop_time = 0.0;  // Fork (diagnostic): native_set_loop_time
}  // namespace

void native_set_loop_time(double seconds) { g_loop_time = seconds; }
double native_loop_time() { return g_loop_time; }

void native_tick_animate(float dt) {
  if (!native_script_boot()) return;
  // Fork: an animate() of step 2 runs script handlers; one that pumps
  // frames must not start a second walk inside the first.
  static bool walking = false;
  if (walking) return;
  walking = true;
  // A copy of the list: a handler may create or destroy an object.
  for (auto& o : animating_snapshot()) o->animate(dt);
  walking = false;
}

// ---------------------------------------------------------------------------
// Game instances: the typeof-1 payload
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t kTagRSD = 0x00445352;   // "RSD\0"
constexpr uint32_t kTagPHYS = 0x53594850;  // "PHYS"
constexpr uint32_t kTagPOLY = 0x594C4F50;  // "POLY"
// "EXTP" 0x50545845 (a list of ids, then the node bounds are grown over the
// children) and any unknown tag are stepped over by their size.

struct TreeWalk {
  const std::vector<uint8_t>* file = nullptr;  // the whole pack
  int32_t pack_id = 0;
  InstanceTree* tree = nullptr;
  bool truncated = false;
};

// The `gametype` / `params` lines of an RSD chunk (or of a legacy text tail)
// of node `index`.
void read_rsd_text(TreeWalk& w, int32_t index, const uint8_t* p,
                   const uint8_t* end) {
  std::string line;
  char word[1025];
  while (p < end) {
    p += read_line(p, end, &line);
    if (line.empty()) continue;
    word[0] = 0;
    engine_sscanf(line.c_str(), "%s", word);
    if (word_is(word, "gametype")) {
      // "%*s %d" (the scanner takes 0x), FUN_0051e860 (pack-relative id:
      // high word = dependency slot), FUN_0051fa40(id, 8, 0) -> payload+0x38.
      int32_t local = 0;
      engine_sscanf(line.c_str(), "%*s %d", &local);
      InstanceTreeNode& node = w.tree->nodes[index];
      if (node.gametype_id == 0) ++w.tree->rsd_nodes;
      node.gametype_local = static_cast<uint32_t>(local);
      node.gametype_id = static_cast<int32_t>(
          rpak_remap_local_id(w.pack_id, static_cast<uint32_t>(local)));
    } else if (word_is(word, "params")) {
      // "%*s %2048s" into a 0x801-byte buffer -> payload+0x48.
      std::vector<char> buf(2049, '\0');
      engine_sscanf(line.c_str(), "%*s %2048s", buf.data());
      InstanceTreeNode& node = w.tree->nodes[index];
      node.params = buf.data();
      node.has_params = true;
    }
  }
}

// One node: child records, then chunks to the end of its payload; then the
// children. Fork: the exe makes the child nodes here and reads each one's
// payload when something asks for it (the camera walk, or at once under
// node flag 0x80000 = ground_precache); the host reads the whole tree.
void read_tree_node(TreeWalk& w, int32_t index, int depth) {
  const std::vector<uint8_t>& file = *w.file;
  const uint32_t offset = w.tree->nodes[index].offset;
  const uint32_t size = w.tree->nodes[index].size;
  if (size < 8) {
    // No room for the two leading dwords: a node without payload.
    w.tree->nodes[index].ends_on_size = true;
    return;
  }
  if (offset > file.size() || size > file.size() - offset) {
    ++w.tree->bad_nodes;
    w.truncated = true;
    return;
  }
  const uint8_t* base = file.data() + offset;
  const uint8_t* end = base + size;
  // u32 n0 (dwords to skip), u32 child count.
  const uint32_t n0 = rd32(base);
  if (n0 > (size - 8) / 4) {
    ++w.tree->bad_nodes;
    return;
  }
  const uint8_t* p = base + 4 + 4 * static_cast<size_t>(n0);
  const uint32_t child_count = rd32(p);
  p += 4;
  std::vector<int32_t> kids;
  bool ok = true;
  for (uint32_t i = 0; i < child_count; ++i) {
    // TOC layout (FUN_0051e6b0): +0 superid, +4 id, +8 restype, +9 flags,
    // +0xE file offset, +0x12 size, +0x16 name length, +0x17 name, then
    // twelve floats when flags & 1. Size 0x17 + name (+ 0x30).
    if (end - p < 0x17) {
      ok = false;
      break;
    }
    InstanceTreeNode k;
    k.superid = rd32(p);
    k.id = rpak_remap_local_id(w.pack_id, rd32(p + 4));
    k.restype = p[8];
    k.rec_flags = p[9];
    k.offset = rd32(p + 0xE);
    k.size = rd32(p + 0x12);
    const size_t name_len = p[0x16];
    size_t rec = 0x17 + name_len;
    if (static_cast<size_t>(end - p) < rec) {
      ok = false;
      break;
    }
    k.name.assign(reinterpret_cast<const char*>(p + 0x17), name_len);
    while (!k.name.empty() && k.name.back() == '\0') k.name.pop_back();
    if (k.rec_flags & 1u) {
      if (static_cast<size_t>(end - p) < rec + 48) {
        ok = false;
        break;
      }
      std::memcpy(k.bounds, p + rec, 48);
      rec += 48;
    }
    k.parent = index;
    k.depth = depth + 1;
    p += rec;
    kids.push_back(static_cast<int32_t>(w.tree->nodes.size()));
    w.tree->nodes.push_back(std::move(k));
  }
  w.tree->nodes[index].child_count = static_cast<uint32_t>(kids.size());
  // Chunks {tag, size, data} while the cursor is inside the payload.
  while (ok && p < end) {
    if (end - p < 8) {
      ok = false;
      break;
    }
    const uint32_t tag = rd32(p);
    const uint32_t csize = rd32(p + 4);
    const uint8_t* data = p + 8;
    if (tag == 0 && csize == 0) {
      // Legacy text payload: three dwords are stepped over and the rest of
      // the payload is the RSD text.
      if (end - p < 12) {
        ok = false;
        break;
      }
      read_rsd_text(w, index, p + 12, end);
      p = end;
      break;
    }
    if (csize > static_cast<size_t>(end - data)) {
      ok = false;
      break;
    }
    if (tag == kTagRSD) {
      read_rsd_text(w, index, data, data + csize);
    } else if (tag == kTagPOLY) {
      // u32 owner (the root's id, read and dropped), u32 section count, per
      // section {u32 material, u32 nVert, u32 nTri, nVert x 14 dwords,
      // nTri x 3 dwords}. The exe moves its cursor by this content and
      // never looks at the chunk size. Fork: the host steps by the size and
      // counts a chunk whose content differs (none in the install: 14,288
      // POLY and 214,263 PHYS chunks end on their size).
      const uint8_t* q = data;
      const uint8_t* cend = data + csize;
      bool cok = csize >= 8;
      const uint32_t nsec = cok ? rd32(q + 4) : 0;
      q += 8;
      uint32_t done = 0;
      for (; cok && done < nsec; ++done) {
        if (cend - q < 12) {
          cok = false;
          break;
        }
        InstancePolySection sec;
        sec.node = index;
        sec.material = rd32(q);
        const uint32_t nv = rd32(q + 4);
        const uint32_t nt = rd32(q + 8);
        q += 12;
        const uint64_t need = static_cast<uint64_t>(nv) * 56u +
                              static_cast<uint64_t>(nt) * 12u;
        if (need > static_cast<uint64_t>(cend - q)) {
          cok = false;
          break;
        }
        sec.verts.resize(nv);
        if (nv) std::memcpy(sec.verts.data(), q, static_cast<size_t>(nv) * 56u);
        q += static_cast<size_t>(nv) * 56u;
        sec.indices.resize(static_cast<size_t>(nt) * 3u);
        if (nt)
          std::memcpy(sec.indices.data(), q, static_cast<size_t>(nt) * 12u);
        q += static_cast<size_t>(nt) * 12u;
        w.tree->poly_vertices += nv;
        w.tree->poly_triangles += nt;
        w.tree->poly.push_back(std::move(sec));
      }
      w.tree->nodes[index].poly_sections += done;
      ++w.tree->poly_chunks;
      if (!cok || q != cend) ++w.tree->bad_chunks;
    } else if (tag == kTagPHYS) {
      // u32 nTri, nTri x {u32 a, u32 i0, i1, i2, float normal xyz}, u32
      // nVert, nVert x 12 bytes (the vertex list only when nTri != 0).
      // Fork: counted and stepped over; the collision lists are M6.
      const uint32_t ntri = csize >= 4 ? rd32(data) : 0;
      uint64_t content = 4;
      if (ntri != 0) {
        content += static_cast<uint64_t>(ntri) * 28u;
        if (content + 4 <= csize) {
          const uint32_t nvert = rd32(data + content);
          content += 4 + static_cast<uint64_t>(nvert) * 12u;
        } else {
          content = static_cast<uint64_t>(csize) + 1;  // cut
        }
      }
      if (content != csize) ++w.tree->bad_chunks;
      w.tree->nodes[index].phys_triangles += ntri;
      w.tree->phys_triangles += ntri;
      ++w.tree->phys_chunks;
    }
    // EXTP (a list of ids, then the node bounds grown over the children)
    // and unknown tags: stepped over by size.
    p = data + csize;
  }
  const bool fits = ok && p == end;
  w.tree->nodes[index].ends_on_size = fits;
  if (!fits) ++w.tree->bad_nodes;
  if (depth >= 64) return;  // Fork: guard against a record loop
  for (int32_t k : kids) {
    if (w.tree->nodes[k].restype != 1) continue;  // only game instances nest
    read_tree_node(w, k, depth + 1);
  }
}

}  // namespace

bool instance_tree_read(int32_t res_id, InstanceTree* out) {
  if (!out) return false;
  *out = InstanceTree{};
  const RpakEntry* e = rpak_find_entry(res_id);
  if (!e || e->restype != 1 || e->size < 8) return false;
  // Nested nodes carry file offsets anywhere in the pack: one read of the
  // file serves the whole walk (rpak_read_entry would reopen it per node).
  std::vector<uint8_t> file;
  if (!rpak_read_range(rpak_id_pack(res_id), 0, 0xFFFFFFFFu, &file))
    return false;
  out->res_id = res_id;
  InstanceTreeNode root;
  root.id = static_cast<uint32_t>(res_id);
  root.superid = static_cast<uint32_t>(e->kind);
  root.restype = e->restype;
  root.rec_flags = e->rec_flags;
  root.offset = e->offset;
  root.size = e->size;
  root.name = e->name;
  std::memcpy(root.bounds, e->bounds, sizeof(root.bounds));
  out->nodes.push_back(std::move(root));
  TreeWalk w;
  w.file = &file;
  w.pack_id = rpak_id_pack(res_id);
  w.tree = out;
  read_tree_node(w, 0, 0);
  return !w.truncated;
}

// ---------------------------------------------------------------------------
// Game instances: the game object of a pack node
// ---------------------------------------------------------------------------
bool instance_node_request(InvObject* handle) {
  if (!handle || !native_script_boot()) return false;
  const int32_t id = handle_instance_id(handle);
  if (!id) return false;
  InstanceNodeState* st = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    auto& slot = g_nodes[id];
    if (slot) return !slot->failed && !slot->building;
    slot = std::make_unique<InstanceNodeState>();
    slot->id = id;
    slot->building = true;
    slot->handle = handle;
    st = slot.get();  // stays put: the map owns it through a unique_ptr
  }
  // PE 940 FUN_005195a0: parse the payload (FUN_00519940), then, when the
  // node names a gametype and has no game object yet, the type's vt+0x0C
  // with the node and the params string.
  // Fork: only the root's game object is built. A nested node that names a
  // gametype of its own (the objects of the big maps) gets its object in
  // the exe when the camera walk first asks for that node's payload; the
  // garages have none.
  InstanceTree tree;
  const bool ok = instance_tree_read(id, &tree);
  const int32_t gametype_id =
      (ok && !tree.nodes.empty()) ? tree.nodes[0].gametype_id : 0;
  const std::string params =
      (ok && !tree.nodes.empty()) ? tree.nodes[0].params : std::string();
  std::shared_ptr<NativeObject> obj;
  const GameTypeDesc* type = nullptr;
  if (gametype_id != 0) {
    type = gametype_desc(gametype_id);
    const NativeFactory make =
        (type->native_index >= 0) ? kNativeTypes[type->native_index].create
                                  : nullptr;
    if (make) {
      NativeCreate c;
      c.type = type;
      c.handle = handle;
      c.instance = nullptr;
      c.parent = nullptr;
      c.node_id = id;
      c.params = params.c_str();
      obj = make(c);
      if (obj) {
        obj->type_index = type->native_index;
        obj->handle = handle;
        obj->node_id = id;
      }
    }
  }
  if (native_type_log()) {
    const RpakEntry* e = rpak_find_entry(id);
    const InstanceTreeNode* r = tree.nodes.empty() ? nullptr : &tree.nodes[0];
    std::fprintf(
        stderr,
        "[native-type] instance 0x%08X '%s' flags=0x%02X: %s, %u nested "
        "nodes, %u POLY sections in %u chunks, %u vertices, %u triangles, "
        "%u PHYS chunks (%u triangles), %u nodes with a gametype, nodes not "
        "ending on their size: %u, chunks not ending on their size: %u\n",
        static_cast<unsigned>(id), e ? e->name.c_str() : "",
        r ? static_cast<unsigned>(r->rec_flags) : 0u, ok ? "read" : "CUT",
        static_cast<unsigned>(tree.nodes.empty() ? 0 : tree.nodes.size() - 1),
        static_cast<unsigned>(tree.poly.size()), tree.poly_chunks,
        tree.poly_vertices, tree.poly_triangles, tree.phys_chunks,
        tree.phys_triangles, tree.rsd_nodes, tree.bad_nodes, tree.bad_chunks);
    if (r)
      std::fprintf(stderr,
                   "[native-type]   bounds centre (%.3f, %.3f, %.3f) half "
                   "(%.3f, %.3f, %.3f); gametype 0x%08X -> 0x%08X native "
                   "'%s'%s; params '%s'\n",
                   r->bounds[0], r->bounds[1], r->bounds[2], r->bounds[3],
                   r->bounds[4], r->bounds[5],
                   static_cast<unsigned>(r->gametype_local),
                   static_cast<unsigned>(gametype_id),
                   type ? type->native_name.c_str() : "",
                   obj ? "" : " (no game object)", params.c_str());
    // PE: "load: Undefined GameType Constructor! %08X" when the type has
    // neither a native type object nor a script class.
    if (gametype_id != 0 && type && type->native_index < 0 &&
        !type->has_script)
      std::fprintf(stderr,
                   "[native-type]   load: Undefined GameType Constructor! "
                   "%08X\n",
                   static_cast<unsigned>(gametype_id));
  }
  std::lock_guard<std::mutex> lock(g_nt_mu);
  st->tree = std::move(tree);
  st->object = obj;
  st->failed = !ok;
  st->building = false;
  ticking_add(obj);
  return ok;
}

void instance_node_unload(InvObject* handle) {
  if (!handle || !native_script_boot()) return;
  const int32_t id = handle_instance_id(handle);
  if (!id) return;
  std::shared_ptr<NativeObject> obj;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    auto it = g_nodes.find(id);
    if (it == g_nodes.end() || !it->second || it->second->building) return;
    obj = it->second->object;
    g_nodes.erase(it);
    if (obj) ticking_remove(obj);
  }
  if (native_type_log())
    std::fprintf(stderr, "[native-type] instance 0x%08X unloaded\n",
                 static_cast<unsigned>(id));
  if (obj) obj->destroy();
}

const InstanceTree* instance_node_tree(int32_t node_id) {
  std::lock_guard<std::mutex> lock(g_nt_mu);
  auto it = g_nodes.find(node_id);
  if (it == g_nodes.end() || !it->second || it->second->building ||
      it->second->failed)
    return nullptr;
  return &it->second->tree;
}

InvObject* instance_node_handle(int32_t node_id) {
  std::lock_guard<std::mutex> lock(g_nt_mu);
  auto it = g_nodes.find(node_id);
  if (it == g_nodes.end() || !it->second) return nullptr;
  return it->second->handle;
}

// ---------------------------------------------------------------------------
// The `ground` type (table row 0, `_type_ground` 0x59E464)
// ---------------------------------------------------------------------------
namespace {

// PE 940 FUN_004ba790: the 0x2DC-byte ground object. The host keeps what
// the garage scene reads: the render instance and the body position.
class GroundObject final : public NativeObject {
 public:
  int32_t render_object_id = 0;  // ground+0x48: params[0], typeof 14
  float body_pos[3] = {0.f, 0.f, 0.f};  // params[1..3]: `groundbody`
  std::string material_file;  // params[7], `.scm`
  std::string ai_file;        // params[8], `.trc`
  InvObject* r_ground = nullptr;  // ground+0x68: the render instance

  // vt+0x10 FUN_004bb3d0.
  void destroy() override {
    if (r_ground) {
      // The render instance goes with the ground (its mesh in the renderer,
      // from step 2 on, is keyed by this object), and so does its handle.
      java_util_resource_ResourceRef_destroy(r_ground);
      java_util_resource_ResourceRef_deleteNative(r_ground);
      r_ground = nullptr;
    }
  }

  // vt+0x3C FUN_004bb570: 1 = the `groundbody` handle, 2 = the position,
  // 0x17 = the `r_ground` handle, 0x39 / 0x3D AI objects, 0x4A fog.
  bool getinfo(int32_t query, NativeInfo* out) override {
    switch (query) {
      case 2:
        // `*param_3 = 0; param_3[1] = 0; param_3[2] = 0; return 1`: zeros
        // whatever position the params gave the body. GameRef.getPos of a
        // map and a camera's MOVE target read this; LOOK reads the body.
        out->v[0] = out->v[1] = out->v[2] = 0.f;
        return true;
      case 0x17:
        out->obj = r_ground;
        return r_ground != nullptr;
      default:
        return false;  // 1: Fork, no `groundbody`; 0x39 / 0x3D / 0x4A: later
    }
  }

  // The static body is made with the identity rotation.
  bool get_ori(float ypr[3]) override {
    ypr[0] = ypr[1] = ypr[2] = 0.f;
    return true;
  }

  // Fork: no physics body in M2; the position `groundbody` is made at
  // (FUN_004bade0: params 1..3), which is what getinfo 1 leads to.
  bool get_body_pos(float pos[3]) override {
    pos[0] = body_pos[0];
    pos[1] = body_pos[1];
    pos[2] = body_pos[2];
    return true;
  }

  // vt+0x38 FUN_004bb6b0 takes `fog ...`, `obs_add %d`, `obs_rem %d`.
  // Fork: none is ported; the host's generic chain sees the command as
  // before (fog reaches the renderer through GroundRef.setFog today).
};

// PE 940 FUN_004bade0.
std::shared_ptr<NativeObject> ground_create(const NativeCreate& c) {
  auto g = std::make_shared<GroundObject>();
  // "%x,%f,%f,%f,%f,%f,%f,%s,%s": the render object id (pack-relative),
  // the position of the static body, three floats that are never read, the
  // material file and the AI file. The two %s buffers are 256 bytes in the
  // exe and unbounded; the host bounds them.
  int32_t render_local = 0;
  float unused[3] = {0.f, 0.f, 0.f};
  char scm[1025];
  char trc[1025];
  scm[0] = trc[0] = 0;
  engine_sscanf(c.params ? c.params : "", "%x,%f,%f,%f,%f,%f,%f,%1024s,%1024s",
                &render_local, &g->body_pos[0], &g->body_pos[1],
                &g->body_pos[2], &unused[0], &unused[1], &unused[2], scm, trc);
  g->material_file = scm;  // Fork: not read (ground materials are M6)
  g->ai_file = trc;        // Fork: not read (traffic and AI are M6)
  // FUN_0051e860 with the pack of the node, then FUN_0051fa40(id, 0xe, 0):
  // the handle binds only to a render object record.
  const int32_t pack = c.node_id ? rpak_id_pack(c.node_id) : 0;
  int32_t render_id = 0;
  if (pack && render_local)
    render_id = static_cast<int32_t>(
        rpak_remap_local_id(pack, static_cast<uint32_t>(render_local)));
  const RpakEntry* re = render_id ? rpak_find_entry(render_id) : nullptr;
  const bool render_ok = re && re->restype == 14;
  if (render_ok) {
    g->render_object_id = render_id;
    // FUN_00524840(node, ground+0x48, "r_ground", 0): a render instance
    // under the map node, bone00 at the identity. It draws nothing itself
    // (the template class of a flags-0x800 render object is 0); the camera
    // walk asks the ground for it (getinfo 0x17) when it meets a POLY node.
    // Fork: a host RenderRef object with its own id (Q12), not sent through
    // RenderRef.create: nothing is loaded or drawn for it in this step.
    InvObject* r = tree_host_new("java.util.resource.RenderRef");
    {
      std::lock_guard<std::mutex> lock(g_mu);
      auto& rs = R(r);
      rs.id = resref_next_runtime_id();
      rs.type = 3;  // INSTANCE_RENDER
      rs.restype = 3;
      rs.type_id = render_id;
      rs.parent = c.handle;
      rs.parent_id = c.node_id;
      rs.alias = "r_ground";
      rs.loaded = true;
    }
    tree_field_set_obj(r, "alias", string_new("r_ground"));
    g->r_ground = r;
    // The exe gives a POLY node its geometry when a camera walk first
    // meets the node: FUN_0045eb10 queues it, FUN_00462750 asks the ground
    // for this instance (getinfo 0x17) and makes an RI_ground from the
    // template of its render object.
    // Fork: the instance hangs under the map handle in the renderer's tree
    // and waits on the pending list like any instance whose type is not
    // loaded; the camera pass builds the whole ground mesh the first time
    // a camera sees it (renderref_materialise_pending -> ground_mesh_build).
    render_d3d9_mesh_set_tree_parent(r, c.handle);
    render_d3d9_mesh_set_pending_type(r, render_id);
  }
  // Not made here (Fork): the static body `groundbody` and its surface data
  // (particles.rpk:2 and :10, sound.rpk:6), the default ground material
  // (system:3, system:9), the registration with the physics world. The exe
  // also puts the ground on the callback lists (FUN_004ebd90(ground, 0x14,
  // 0)) for its traffic tick; the host object has nothing to tick yet, so
  // `ticks` stays off.
  if (native_type_log())
    std::fprintf(stderr,
                 "[native-type] ground node=0x%08X: render object 0x%08X%s, "
                 "body position (%g, %g, %g), scm '%s', trc '%s', "
                 "r_ground=%p\n",
                 static_cast<unsigned>(c.node_id),
                 static_cast<unsigned>(render_id),
                 render_ok ? " (typeof 14)" : " (NOT a render object)",
                 g->body_pos[0], g->body_pos[1], g->body_pos[2], scm, trc,
                 static_cast<void*>(g->r_ground));
  return g;
}

}  // namespace

// The `camera` type (table row 1).
#include "NativeTypes_part2.inc"

// Render objects (typeof 14) and the ground mesh.
#include "NativeTypes_part3.inc"

// The `part` type (table row 4).
#include "NativeTypes_part4.inc"

// The part type's slots and its `install` command.
#include "NativeTypes_part5.inc"

// The `car` type: a part object with wheels, posed without physics.
#include "NativeTypes_part6.inc"

// What the Part, Chassis and WheelRef natives ask of a part or car object.
#include "NativeTypes_part7.inc"

// What is drawn of a part or car: the detail the cameras give, the levels,
// the linked render entries (FUN_00461980, FUN_004afbf0).
#include "NativeTypes_part8.inc"

// The `player` type (table row 6) and the `cursor` type (row 8).
#include "NativeTypes_part9.inc"

}  // namespace inv
