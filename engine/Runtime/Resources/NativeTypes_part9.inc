// Fragment 9 of NativeTypes.cpp - included, not compiled on its own.
//
// M4 step 1: the `player` type (row 6 of the native type table,
// `_type_player` 0x59AA9C, factory FUN_004d3c30) and the `cursor` type (row
// 8, `_type_cursor` 0x59E18C, factory FUN_004b9b80).
//
// Mirrors:
//   player   FUN_004d2370 create, FUN_004d28d0 getinfo
//   cursor   FUN_004b7a10 create, FUN_004b7d70 commands (vt+0x38),
//            FUN_004b7c60 getinfo, FUN_004b7c30 / FUN_004b7510 destroy,
//            FUN_004b78f0 the animate tick (vt+0x34) with the software
//            pointer, the head of FUN_004b8430 (the position),
//            FUN_00426c70 (which Windows shape), FUN_00427870 /
//            FUN_00425a20 (ShowCursor), FUN_004262c0 / FUN_00425520
//            (ClipCursor), FUN_00426cb0 (SetCursorPos), FUN_004e8f40 (the
//            pointer text)
// M4 step 3: the player's `camera <id>` command (FUN_004d2d20), which
// answers `control <player id>` to that camera; the camera's control(dt)
// then reads the player's axes 7, 8 and 10 (NativeTypes_part2.inc).
// M4 step 2: the pick and the events.
//   pick     FUN_004eb000 (viewports, cameras, the segment; the list walks
//            are in the renderer, render_d3d9_pick_rays), FUN_004fe1a0 (the
//            segment and its result), FUN_004fddf0 (which nodes the walk
//            reaches), FUN_004fc140 (a body), FUN_005007c0 (sphere),
//            FUN_00500580 (box)
//   events   the rest of FUN_004b8430 (hover, the two buttons, the eighteen
//            FUN_00528030 formats); each is sent with FUN_0047d550
//            (gametype_send_event, GameType.cpp)
// Units: the cursor position is normalised device coordinates of the
// WINDOW, x in [-1, 1] left to right, y in [-1, 1] TOP to BOTTOM.

namespace {

// ---------------------------------------------------------------------------
// The `player` type (java.io.Controller, system.rpk:0x32)
// ---------------------------------------------------------------------------

// Defined with the cursor below: FUN_0051fa40(id, 1, 0), a handle bound to
// a game instance node by id, and the node such a handle names.
int32_t cursor_bind_instance(int32_t id);
InvObject* cursor_node(int32_t id);

// PE 940 DAT_005fe3ac: the one float getinfo 0x43 answers through (the query
// returns its address, so a query that does not write it leaves the value
// of the query before).
float g_player_axis_out = 0.f;

// PE 940: the 0x2510-byte player object.
// Fork: the exe keeps a 0x2148-byte axis blob at player+0x1C (152 map
// entries, one 0x30-byte record per logical axis) and evaluates an axis
// once per input tick with an analog / digital rule and rates (FUN_00565b60,
// M4 spec Q5). The host keeps the map entries the Controller natives make
// (Controller.user_Add and its kin, keyed by the script object) and sums
// them on every read. Not ported either: the player's display, its
// controllable list and the flag player+0x10 bit 1 that makes the two axis
// queries read a table of forced values (player+0x21DC).
class PlayerObject final : public NativeObject {
 public:
  // The create string "%d %d %d": player+0x21D8, an id a handle is bound to
  // (FUN_0051f9b0), and the player number (player+0x2428, "Player%d"). None
  // of the three is read by anything the host has.
  int32_t create_a_ = 0;
  int32_t create_id_ = 0;
  int32_t number_ = 0;
  // player+0x00: the handle on the player's own node; its id (+0x08) is the
  // number of the `control %d` the player sends.
  int32_t self_id_ = 0;
  // player+0x2184 (id +0x218C): the camera bound with `camera <id>`.
  int32_t camera_id_ = 0;

  // PE 940 FUN_00565b60(blob, axis). `instance` is the Controller script
  // object: the receiver of Controller.user_Add.
  float axis_value(int32_t axis) const {
    return input_map_get_logical(instance, axis);
  }

  // ---- vt+0x3C FUN_004d28d0 ------------------------------------------------
  bool getinfo(int32_t query, NativeInfo* out) override {
    // The axis number is the query's argument; the exe tests it as a
    // pointer, `arg != 0 && arg < 0x4c` unsigned.
    const uint32_t axis = static_cast<uint32_t>(out->arg);
    switch (query) {
      case 8:     // GII_CONTROL
      case 0x1c:  // GII_ANIMATE
        // Fork: answered as in the exe, but the host object is on neither
        // list: its control and animate work (display, controllables) is
        // not ported.
        out->i = 1;
        return true;
      case 0x19:  // GII_AXIS: (int)(value * 127.0) (DAT_0059abb8)
        if (axis == 0 || axis >= 0x4c) {
          out->i = 0;
          return false;
        }
        out->i = static_cast<int32_t>(
            static_cast<double>(axis_value(out->arg)) * 127.0);
        return true;
      case 0x43:  // the value itself
        if (axis != 0) g_player_axis_out = axis < 0x4c ? axis_value(out->arg) : 0.f;
        out->v[0] = g_player_axis_out;
        return true;
      default:
        // 0x15 (player+0x242C), 0x1A (a handle at player+0x21AC), 0x1B (the
        // axis blob) and 0x44 (axis smoothing): no reader on the host.
        return false;
    }
  }

  // ---- vt+0x38 FUN_004d2d20 --------------------------------------------------
  // The exe's handler returns 1 for every event and knows the commands
  // `device`, `setcontrol`, `viewport`, `renderinstance`, `camera`,
  // `controllable` and more (type 0x10, the first word through "%s" and
  // FUN_0057c11e).
  // Fork: only `camera` is taken here; every other command falls to the
  // host's generic command chain as before this object existed.
  bool event(int32_t type, const char* text) override {
    if (type != 0x10 || !text) return false;
    char word[64];
    word[0] = 0;
    engine_sscanf(text, "%63s", word);
    if (!word_is(word, "camera")) return false;
    // `camera <id>` (asm 0x4d301a..0x4d30ab), sent by
    // MouseCursor.enableCameraControl: "%*s %d" into a number that starts
    // 0; the camera handle player+0x2184 is bound to that node
    // (FUN_0051fa40(id, 1, 0); 0 releases it); "control %d" with the
    // player's own id goes to that handle with EVENT_COMMAND
    // (FUN_0047d550), which is how the camera learns whose axes its
    // control(dt) reads (FUN_0048bce0, `control`).
    // FUN_004d3b60, called next, sends nothing for a `player` (M4 Q4,
    // confirmed), and the re-enter of a controlled object (FUN_004d2ab0)
    // needs one: the garage's player controls none. Neither is ported.
    // Fork: the command goes to the camera's native handler directly. The
    // exe's sender tests the camera node's event mask (it has
    // EVENT_COMMAND from its create) and then walks the node's
    // notification list; no script listens for commands on a camera.
    int32_t id = 0;
    engine_sscanf(text, "%*s %d", &id);
    camera_id_ = cursor_bind_instance(id);
    if (InvObject* cam = cursor_node(camera_id_)) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "control %d", self_id_);
      native_event(cam, 0x10, buf);
    }
    return true;
  }

  // PE 940 FUN_0051f970: the camera handle reads id 0 once its node is gone.
  void nodes_gone(const std::vector<int32_t>& ids) override {
    if (camera_id_ != 0 &&
        std::binary_search(ids.begin(), ids.end(), camera_id_))
      camera_id_ = 0;
  }
};

// PE 940 FUN_004d2370 (type vt+0x0C).
std::shared_ptr<NativeObject> player_create(const NativeCreate& c) {
  auto p = std::make_shared<PlayerObject>();
  engine_sscanf(c.params ? c.params : "", "%d %d %d", &p->create_a_,
                &p->create_id_, &p->number_);
  p->self_id_ = java_util_resource_ResourceRef_id(c.handle);
  // FUN_004ebd90(player, 0x10, 0): event mask |= EVENT_COMMAND.
  // Not made here: the axis blob (FUN_00566010 resets it; the host's map
  // table is the Controller natives'), the font handle frontend.rpk:0x25
  // and the Config values player_display, player_damage_multiplier,
  // player_change_distance, player_change_velocity.
  return p;
}

// ---------------------------------------------------------------------------
// The `cursor` type (frontend.rpk:0x2A)
// ---------------------------------------------------------------------------

// Values of the exe's .data: the process has one of each, they are not
// fields of the cursor object.
//
// DAT_005e9398. 0 in the image; every cursor create copies the live static
// Config.SysCursor into it (asm 0x4b7beb..0x4b7c0b) and nothing ever resets
// it (M4 Q1). Not 0 = the pointer is the Windows cursor and the position
// comes from the window procedure; 0 = the software pointer.
int32_t g_sys_cursor = 0;
// _DAT_005daa78 (x) and DAT_005daa7c (y): Config.mouseSensitivity x 1000
// (_DAT_0059746c), written by create and by `sens`. 1000.0 in the image.
float g_cursor_sens_x = 1000.f;
float g_cursor_sens_y = 1000.f;
// Which entry of the shape table DAT_005e93d4 the current Windows cursor
// DAT_005e93a0 is. Entry 0 (resource 116, the arrow) after the ten
// LoadCursorA calls of FUN_004223a0 (asm 0x42275f).
int32_t g_cursor_shape = 0;

// _DAT_0059b188 (0x358637BE): the squared step above which the pointer
// "moved" in a tick.
constexpr float kCursorMovedEps = 1.0000001e-6f;

// Fork: SLRR_SYSCURSOR=0|1 replaces the value copied from Config.SysCursor
// (tests of the software pointer). -1 = not given.
int cursor_sys_override() {
  static const int v = [] {
    const char* e = std::getenv("SLRR_SYSCURSOR");
    if (!e || !e[0]) return -1;
    return e[0] == '0' ? 0 : 1;
  }();
  return v;
}

// Fork: SLRR_PE_CURSOR_DRAW=1 builds the pointer text of the software mode
// while the pointer is the Windows cursor, and hides it while the cursor is
// disabled: the pointer of a frame dump (an OS cursor is never in one).
// Without it nothing is drawn in that mode, as in the stock game.
bool cursor_draw_fork() {
  static const bool on = [] {
    const char* e = std::getenv("SLRR_PE_CURSOR_DRAW");
    return e && e[0] == '1';
  }();
  return on;
}

// PE 940 FUN_0047c110 (int) / FUN_0047c080 (float): a field of the script
// class java.util.Config by name. The exe reads the LIVE static (FUN_004ece80
// on the script object of the "Config" instance, system.rpk:0x47): no cached
// copy, no table of defaults (M4 Q1).
// Host: the static slot of the loaded class; false when the class is not
// loaded or has no such static.
bool cursor_config_static(const char* name, JvmValue* out) {
  Jvm* j = jvm_active();
  const JvmClass* cls = j ? j->find_class("java.util.Config") : nullptr;
  JvmValue* slot = cls ? tree_static_slot(cls, name, false) : nullptr;
  if (!slot) return false;
  *out = *slot;
  return true;
}

// FUN_0047cff0("SysCursor", 0) != 0 -> DAT_005e9398 = FUN_0047c110("SysCursor").
// `source` says where the value came from (the create line of the log).
void cursor_read_sys_cursor(const char** source) {
  JvmValue v;
  if (cursor_config_static("SysCursor", &v) && v.tag == JvmTag::Int) {
    g_sys_cursor = v.v.i;
    *source = "Config.SysCursor";
  } else if (InvObject* cfg = system_config_host()) {
    // Fork: without the script class the host's Config object answers (it
    // carries the class initialiser, 1).
    g_sys_cursor = tree_field_get_int(cfg, "SysCursor");
    *source = "host Config object";
  } else {
    *source = "kept";  // the field does not exist: the global is not written
  }
  if (cursor_sys_override() >= 0) {
    g_sys_cursor = cursor_sys_override();
    *source = "SLRR_SYSCURSOR";
  }
}

// DAT_005daa7c = _DAT_005daa78 = Config.mouseSensitivity * 1000.0.
void cursor_read_sens() {
  float s = 0.f;
  JvmValue v;
  if (cursor_config_static("mouseSensitivity", &v) &&
      (v.tag == JvmTag::Float || v.tag == JvmTag::Int)) {
    s = v.tag == JvmTag::Float ? v.v.f : static_cast<float>(v.v.i);
  } else if (InvObject* cfg = system_config_host()) {
    s = tree_field_get_float(cfg, "mouseSensitivity");
  }
  g_cursor_sens_y = s * 1000.f;
  g_cursor_sens_x = g_cursor_sens_y;
}

// The jump table at 0x4b83d4: the Windows shape of a pointer letter.
int32_t cursor_shape_of(char c) {
  switch (c) {
    case '1':
    case 'G':
      return 1;
    case '2':
    case 'M':
      return 2;
    case '3':
    case 'W':
      return 3;
    case '4':
    case 'A':
      return 4;
    case '5':
    case 'D':
      return 5;
    case '6':
      return 6;
    case '7':
      return 7;
    case '8':
      return 8;
    case '9':
      return 9;
    default:
      return 0;  // `J`, `B`, `+`, ...
  }
}

// PE 940 FUN_00426c70(n): DAT_005e93a0 = table[n], then SetCursor of it
// (SetCursor(NULL) while SysCursor is 0); WM_SETCURSOR applies it again.
// Host: the ten shapes sit in the renderer's slots 2..11
// (render_d3d9_cursor_shapes_load) and render_d3d9_set_stock_cursor makes
// one current for the window procedure.
// Fork: while SysCursor is 0 the Windows cursor is left as it is (the exe
// hides it). And in an unattended run nothing calls SetCursor: the index is
// kept for the log only.
// Fork (M4 Q1 point 6): before the first cursor is created the exe's
// WM_SETCURSOR finds DAT_005e9398 = 0 and calls SetCursor(NULL), so no
// pointer shows over the client area during the boot and the splash. The
// host's WM_SETCURSOR (render_d3d9_part1.inc, apply_stock_cursor_to_window)
// shows its class cursor until the first `mode` selects a shape here; that
// file is not M4's to change.
void cursor_set_shape(int32_t n) {
  if (n < 0 || n > 9) n = 0;
  g_cursor_shape = n;
  if (g_sys_cursor == 0 || input_mouse_unattended()) return;
  render_d3d9_set_stock_cursor(n + 2);
}

// PE 940 FUN_004223a0 (asm 0x4226c3..0x42275a) loads the ten shapes from its
// own module when the renderer starts.
// Fork: the host loads them from the game's exe when the first cursor is
// created (render_d3d9_cursor_shapes_load); once per process.
void cursor_load_shapes() {
  static bool tried = false;
  if (tried) return;
  tried = true;
  const std::string exe = rpak_resolve_path("StreetLegal_Redline.exe");
  int32_t from_bits = 0;
  int32_t hot[20];
  const int32_t n =
      render_d3d9_cursor_shapes_load(exe.c_str(), &from_bits, hot);
  if (!native_type_log()) return;
  std::string spots;
  for (int i = 0; i < 10; ++i) {
    char b[32];
    std::snprintf(b, sizeof b, " (%d,%d)", hot[2 * i], hot[2 * i + 1]);
    spots += b;
  }
  std::fprintf(stderr,
               "[cursor] shapes loaded=%d of 10 from '%s' (%d through the "
               "resource bits) hot spots%s\n",
               n, exe.c_str(), from_bits, spots.c_str());
}

// The charset of a pointer text: one host handle per render object id, kept
// for the life of the process (Fork: the exe binds the handle cursor+0x2C to
// the node; the renderer keys a font by a host object).
struct CursorFont {
  InvObject* key = nullptr;
  bool failed = false;
};
std::unordered_map<int32_t, CursorFont> g_cursor_fonts;

InvObject* cursor_font_key(int32_t id) {
  if (id == 0) return nullptr;
  CursorFont& f = g_cursor_fonts[id];
  if (f.failed) return nullptr;
  if (!f.key) {
    f.key = resref_new();
    if (f.key) java_util_resource_ResourceRef_set(f.key, id);
  }
  if (f.key && (render_d3d9_font_ready(f.key) ||
                render_d3d9_font_load_charset(f.key, id)))
    return f.key;
  f.failed = true;  // asked once
  if (native_type_log())
    std::fprintf(stderr,
                 "[cursor] font 0x%08X is no charset the renderer can load: "
                 "no pointer text\n",
                 static_cast<unsigned>(id));
  return nullptr;
}

// PE 940 FUN_0051fa40(id, 1, 0): a handle bound to a game instance node by
// id. Host: the id while it names a live node (a script-made one or a pack
// instance), else 0 (inferred: the bind of a number that names no such node
// was not read).
int32_t cursor_bind_instance(int32_t id) {
  if (id == 0) return 0;
  if (script_node_object(id) || instance_node_handle(id)) return id;
  return 0;
}

// ---------------------------------------------------------------------------
// The pick (M4 step 2)
// ---------------------------------------------------------------------------

// type+0x20 of `_type_cursor` (set by its factory FUN_004b9b80): how far
// from the press position, in cursor units, a held button becomes a drag.
constexpr float kCursorDragStep = 0.02f;
// DAT_00594798 (0x3D4CCCCD): what a body type adds to the radius of its
// bounding sphere (FUN_004f7d60).
constexpr float kBodyBoundMargin = 0.05f;
constexpr int32_t kNodeWorldTreeLeaf = 0x40;  // GameRef.WORLDTREELEAF

// The exe's static pick block PTR_DAT_005daa74 = 0x5DA9F0, which the cursor
// hands to FUN_004eb000 field by field (asm 0x4b861b..0x4b863d). One per
// process, zero in the image. A handle of the exe is an id here.
struct CursorPickBlock {
  float far_pt[3] = {0.f, 0.f, 0.f};   // +0x00 A, of the last pick that hit
  float near_pt[3] = {0.f, 0.f, 0.f};  // +0x0C B, of the last pick that hit
  float t = 0.f;                       // +0x48
  float point[3] = {0.f, 0.f, 0.f};    // +0x4C the hit point, body-local
  int32_t tri = 0;                     // +0x64 triangle index
  int32_t body_id = 0;                 // +0x6C body ref: body+0x50, the id of its physics instance
  int32_t obj_id = 0;                  // +0x74 handle (id +0x7C): the owner node of the body
  // Host, for the log: the viewport and the camera that made the hit.
  int32_t vp_pri = 0;
  int32_t cam_pri = 0;
};
CursorPickBlock g_pick;

// Host: what the cursor's tick costs, for `[cursor] prof` at loop end
// (SLRR_PE_NATIVE_PROF). Wall time; `events_us` holds the script handlers
// the sends ran.
struct CursorProf {
  uint64_t ticks = 0;    // animate calls
  uint64_t picks = 0;    // of them with a pick (the cursor enabled)
  double rays_us = 0.0;  // render_d3d9_pick_rays
  double bodies_us = 0.0;  // physics_pick_bodies, the owner chains
  double walk_us = 0.0;  // the tests
  double tick_us = 0.0;  // the whole animate call
};
CursorProf g_cursor_prof;
inline double cursor_us_since(const std::chrono::steady_clock::time_point& t0) {
  return std::chrono::duration<double, std::micro>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

// The node a handle of the cursor names: a node GameRef.create,
// create_native or createNativeInstance made, or a pack instance (the map).
// Null when the id names no live node: a handle on a destroyed node reads
// id 0 in the exe (FUN_0051f970), and FUN_0047c760 sends nothing to a
// handle without a game node.
InvObject* cursor_node(int32_t id) {
  if (id == 0) return nullptr;
  if (InvObject* o = script_node_object(id)) return o;
  return instance_node_handle(id);
}
int32_t cursor_live_id(int32_t id) { return cursor_node(id) ? id : 0; }

// PE 940 FUN_005007c0(A, dir, len, centre, radius): where the segment from
// B = A + dir x len back to A enters the sphere, as the part of the way
// from B (0) to A (1); -1.0 (DAT_005935ac) when it does not. `dir` points
// from A (the far point) to B (the near point).
float pick_sphere_t(const float a[3], const float dir[3], float len,
                    const float c[3], float radius) {
  const float vx = c[0] - a[0];
  const float vy = c[1] - a[1];
  const float vz = c[2] - a[2];
  const float proj = dir[1] * vy + dir[0] * vx + dir[2] * vz;
  if (-radius <= proj && proj <= len + radius) {
    const float r2 = radius * radius;
    const float perp2 = (vx * vx + vy * vy + vz * vz) - proj * proj;
    if (perp2 <= r2 && (0.f <= proj || proj * proj + perp2 <= r2) &&
        (proj <= len || (proj - len) * (proj - len) + perp2 <= r2)) {
      float t = ((len - proj) - std::sqrt(r2 - perp2)) / len;
      if (t <= 0.f) t = 0.f;
      return t;
    }
  }
  return -1.f;
}

// PE 940 FUN_00500580(A', B', hx, hy, hz): the far point A' and the near
// point B' in body-local coordinates against the box of half extents h. Per
// axis the entry (-s h - B) / (A - B) and the exit (s h - B) / (A - B) with
// s the sign of A - B (entry 0, exit 1 when the two are equal); the result
// is the largest entry when it is not above the smallest exit, 0 when that
// is negative (a segment that starts inside), -1.0 on a miss.
float pick_box_t(const float a[3], const float b[3], const float h[3]) {
  for (int i = 0; i < 3; ++i) {
    if (!(a[i] <= h[i] || b[i] <= h[i])) return -1.f;
    if (!(-h[i] <= a[i] || -h[i] <= b[i])) return -1.f;
  }
  float ent[3], ext[3];
  for (int i = 0; i < 3; ++i) {
    const float d = a[i] - b[i];
    if (d == 0.f) {
      ent[i] = 0.f;
      ext[i] = 1.f;
    } else {
      const float sh = (d < 0.f ? -1.f : 1.f) * h[i];  // FUN_004b1ef0: the sign
      const float inv = 1.f / d;
      ent[i] = (-sh - b[i]) * inv;
      ext[i] = (sh - b[i]) * inv;
    }
  }
  float out = ext[1];
  if (ext[0] <= out) out = ext[0];
  if (ext[2] <= out) out = ext[2];
  float in = ent[1];
  if (in <= ent[0]) in = ent[0];
  if (in <= ent[2]) in = ent[2];
  if (in <= out) {
    if (in <= 0.f) in = 0.f;
    return in;
  }
  return -1.f;
}

// PE 940 FUN_004eb000(&position, ...) into the static block. The bound
// viewports from the top, in each the cameras from the top
// (render_d3d9_pick_rays); per camera FUN_004fe1a0 casts the segment from
// the near to the far plane into the physics below the camera's world-tree
// root, and the first camera that hits anything ends the search.
// What a camera's walk FUN_004fddf0 reaches of a script-made body: the
// type-2 node of the body hangs under its owner's node, and the walk
// descends from the root through every game node that has no WORLDTREELEAF
// (0x40), the root included. That is how the boxes of a hidden Group drop
// out. The event mask is not looked at, nor the body's thickness.
// Fork: the bounds test of child nodes (node+0xb0) is skipped, it only
// saves work; collision triangles of nodes and the bodies of part and car
// objects are M4 step 4.
bool cursor_pick(float x, float y) {
  // At entry, before any viewport is looked at: the hit handle and the
  // body released, the point zeroed, the triangle index -1, t 0. The far
  // and the near point are written by a camera that hits only (M4 Q11).
  g_pick.obj_id = 0;
  g_pick.body_id = 0;
  g_pick.point[0] = g_pick.point[1] = g_pick.point[2] = 0.f;
  g_pick.tri = -1;
  g_pick.t = 0.f;
  constexpr int32_t kMaxRays = 64;
  RenderPickRay rays[kMaxRays];
  const bool prof = native_type_log();
  auto t0 = std::chrono::steady_clock::now();
  const int32_t n = render_d3d9_pick_rays(x, y, rays, kMaxRays);
  if (prof) {
    ++g_cursor_prof.picks;
    g_cursor_prof.rays_us += cursor_us_since(t0);
    t0 = std::chrono::steady_clock::now();
  }
  if (n <= 0) return false;
  std::vector<PhysicsPickBody> bodies;
  physics_pick_bodies(&bodies);
  if (bodies.empty()) return false;
  // Fork: the exe meets the bodies in the order of its tree walk, and of
  // two bodies at the same t the first met wins (FUN_004fc140: `t < best`).
  // The host takes them in the order they were created.
  std::sort(bodies.begin(), bodies.end(),
            [](const PhysicsPickBody& a, const PhysicsPickBody& b) {
              return a.seq != b.seq ? a.seq < b.seq : a.id < b.id;
            });
  // The way up from an owner, once per owner and pick: every node from the
  // owner to the top, each with "a WORLDTREELEAF node at or below it".
  struct Up {
    void* node;
    bool leaf;
  };
  struct Chain {
    InvObject* owner = nullptr;
    int32_t owner_id = 0;  // 0: the owner is no live node
    std::vector<Up> up;
  };
  std::vector<Chain> chains;
  auto chain_of = [&chains](InvObject* owner) -> size_t {
    for (size_t k = 0; k < chains.size(); ++k)
      if (chains[k].owner == owner) return k;
    Chain c;
    c.owner = owner;
    // Fork: a PhysicsRef the script made under a node that was destroyed
    // since stays in the host's table (the exe deletes it with the node).
    const int32_t id = java_util_resource_ResourceRef_id(owner);
    c.owner_id = cursor_node(id) == owner ? id : 0;
    if (c.owner_id != 0) {
      bool leaf = false;
      void* cur = owner;
      for (int guard = 0; cur && guard < 64; ++guard) {
        if (gameref_get_flags(static_cast<InvObject*>(cur)) & kNodeWorldTreeLeaf)
          leaf = true;
        c.up.push_back({cur, leaf});
        cur = render_d3d9_camera_walk_parent(cur);
      }
    }
    chains.push_back(std::move(c));
    return chains.size() - 1;
  };
  std::vector<size_t> chain_ix(bodies.size());
  for (size_t k = 0; k < bodies.size(); ++k)
    chain_ix[k] = chain_of(bodies[k].owner);
  if (prof) {
    g_cursor_prof.bodies_us += cursor_us_since(t0);
    t0 = std::chrono::steady_clock::now();
  }
  struct WalkTime {
    bool on;
    std::chrono::steady_clock::time_point t0;
    ~WalkTime() {
      if (on) g_cursor_prof.walk_us += cursor_us_since(t0);
    }
  } walk_time{prof, t0};
  for (int32_t i = 0; i < n; ++i) {
    const RenderPickRay& ray = rays[i];
    if (!ray.root) continue;
    // FUN_004fe1a0: the segment (0x5DADF8 A, 0x5DAE04 B), its direction
    // from A to B (0x5DAE10), its length (0x5DAE1C), best t = 1.0.
    const float* a = ray.far_pt;
    const float* b = ray.near_pt;
    const float dx = b[0] - a[0];
    const float dy = b[1] - a[1];
    const float dz = b[2] - a[2];
    const float len = std::sqrt(dy * dy + dx * dx + dz * dz);
    float inv = len;
    if (len <= 9.99999997e-07f) inv = 9.99999997e-07f;  // DAT_0059a450
    inv = 1.f / inv;
    const float dir[3] = {inv * dx, inv * dy, inv * dz};
    float best = 1.f;  // _DAT_005947d0
    const PhysicsPickBody* got = nullptr;
    int32_t got_owner = 0;
    CamM3 got_rot = cam_identity();
    for (size_t bi = 0; bi < bodies.size(); ++bi) {
      const PhysicsPickBody& body = bodies[bi];
      const Chain& c = chains[chain_ix[bi]];
      if (c.owner_id == 0) continue;
      bool reached = false;
      for (const Up& u : c.up)
        if (u.node == ray.root) {
          reached = !u.leaf;
          break;
        }
      if (!reached) continue;
      // FUN_004fc140(body): the bounding sphere of the body type first
      // (shape+0xC: the half diagonal of a box, the radius of a sphere, each
      // plus 0.05), entered at 0 <= t <= best.
      float radius = body.half[0];
      if (body.shape == 1)
        radius = std::sqrt(body.half[0] * body.half[0] +
                           body.half[1] * body.half[1] +
                           body.half[2] * body.half[2]);
      radius += kBodyBoundMargin;
      const float ts = pick_sphere_t(a, dir, len, body.pos, radius);
      if (!(0.f <= ts) || best < ts) continue;
      float t = ts;  // shape kind 1: the sphere entry is the hit
      const CamM3 rot =
          cam_ypr_to_matrix(body.ypr[0], body.ypr[1], body.ypr[2]);
      if (body.shape == 1) {
        // Shape kind 5: A and B into body-local coordinates (FUN_004af9a0:
        // R^T (p - position)), then the box; a result of 0 or less is no
        // hit (DAT_00593580), so a segment that starts inside does not hit.
        float la[3], lb[3];
        for (int k = 0; k < 3; ++k) {
          la[k] = rot.m[0][k] * (a[0] - body.pos[0]) +
                  rot.m[1][k] * (a[1] - body.pos[1]) +
                  rot.m[2][k] * (a[2] - body.pos[2]);
          lb[k] = rot.m[0][k] * (b[0] - body.pos[0]) +
                  rot.m[1][k] * (b[1] - body.pos[1]) +
                  rot.m[2][k] * (b[2] - body.pos[2]);
        }
        t = pick_box_t(la, lb, body.half);
        if (t <= 0.f) continue;
      }
      if (t < best) {
        best = t;
        got = &body;
        got_owner = c.owner_id;
        got_rot = rot;
      }
    }
    if (got && best < 1.f) {
      // The tail of FUN_004fe1a0: the world point (A - B) t + B, into the
      // frame of the body that was hit.
      const float wx = (a[0] - b[0]) * best + b[0] - got->pos[0];
      const float wy = (a[1] - b[1]) * best + b[1] - got->pos[1];
      const float wz = (a[2] - b[2]) * best + b[2] - got->pos[2];
      for (int k = 0; k < 3; ++k)
        g_pick.point[k] = got_rot.m[1][k] * wy + got_rot.m[0][k] * wx +
                          got_rot.m[2][k] * wz;
      for (int k = 0; k < 3; ++k) {
        g_pick.far_pt[k] = a[k];
        g_pick.near_pt[k] = b[k];
      }
      g_pick.t = best;
      g_pick.tri = 0;  // DAT_005dae5c of a box or sphere hit
      g_pick.body_id = got->id;
      g_pick.obj_id = got_owner;
      g_pick.vp_pri = ray.vp_pri;
      g_pick.cam_pri = ray.cam_pri;
      return true;
    }
  }
  return false;
}

// Fork: SLRR_PE_CURSOR_PICK_DUMP=1 prints, with every `[cursor] pick` line,
// the cameras asked and every body the pick could test: a diagnostic for
// "why was this not hit".
bool cursor_pick_dump_on() {
  static const bool on = [] {
    const char* e = std::getenv("SLRR_PE_CURSOR_PICK_DUMP");
    return e && e[0] == '1';
  }();
  return on;
}

void cursor_pick_dump(float x, float y) {
  constexpr int32_t kMaxRays = 64;
  RenderPickRay rays[kMaxRays];
  const int32_t n = render_d3d9_pick_rays(x, y, rays, kMaxRays);
  for (int32_t i = 0; i < n; ++i) {
    const RenderPickRay& r = rays[i];
    InvObject* root = static_cast<InvObject*>(r.root);
    const char* cn = root ? tree_host_class(root) : nullptr;
    std::fprintf(stderr,
                 "[cursor] pickdbg ray %d vp=%d cam=%d root=%p 0x%08X '%s' "
                 "flags=0x%X far=(%.4f, %.4f, %.4f) near=(%.4f, %.4f, %.4f)\n",
                 i, r.vp_pri, r.cam_pri, r.root,
                 static_cast<unsigned>(
                     root ? java_util_resource_ResourceRef_id(root) : 0),
                 cn ? cn : "", root ? gameref_get_flags(root) : 0, r.far_pt[0],
                 r.far_pt[1], r.far_pt[2], r.near_pt[0], r.near_pt[1],
                 r.near_pt[2]);
  }
  std::vector<PhysicsPickBody> bodies;
  physics_pick_bodies(&bodies);
  std::sort(bodies.begin(), bodies.end(),
            [](const PhysicsPickBody& a, const PhysicsPickBody& b) {
              return a.seq != b.seq ? a.seq < b.seq : a.id < b.id;
            });
  for (const PhysicsPickBody& b : bodies) {
    const int32_t oid = java_util_resource_ResourceRef_id(b.owner);
    std::string up;
    void* cur = b.owner;
    for (int guard = 0; cur && guard < 8; ++guard) {
      InvObject* o = static_cast<InvObject*>(cur);
      const char* cn = tree_host_class(o);
      char part[160];
      std::snprintf(part, sizeof part, " <- %p 0x%08X '%s' flags=0x%X", cur,
                    static_cast<unsigned>(java_util_resource_ResourceRef_id(o)),
                    cn ? cn : "", gameref_get_flags(o));
      up += part;
      cur = render_d3d9_camera_walk_parent(cur);
    }
    std::fprintf(stderr,
                 "[cursor] pickdbg body %d shape=%d live=%d pos=(%.4f, %.4f, "
                 "%.4f) half=(%.4f, %.4f, %.4f) ypr=(%.3f, %.3f, %.3f)%s\n",
                 b.id, b.shape, cursor_node(oid) == b.owner ? 1 : 0, b.pos[0],
                 b.pos[1], b.pos[2], b.half[0], b.half[1], b.half[2], b.ypr[0],
                 b.ypr[1], b.ypr[2], up.c_str());
  }
}

// PE 940 FUN_004b72f0 / FUN_004b7a10: the 0x1BC-byte cursor object. Field
// comments give the exe offset. A handle of the exe is an id here; a handle
// on a node that is gone reads 0 (nodes_gone).
class CursorObject final : public NativeObject {
 public:
  int32_t self_id_ = 0;           // +0x00 handle (id +0x08): the cursor's own node
  // Fork: set by destroy(). The exe frees the object; the host's tick may
  // still hold it when a handler it called destroys the cursor.
  bool dead_ = false;
  bool activated_ = false;        // +0x10 bit 0: set by `activate`. Only
                                  // vt+0x2C reads it, which is never called
                                  // (the cursor is on the animate list only)
  int32_t controller_id_ = 0;     // +0x1C handle (id +0x24)
  int32_t font_id_ = 0;           // +0x2C handle: the pointer font, a render object
  uint32_t color_ = 0xFFFFFFFFu;  // +0x4C
  // +0x50: bit 0 enabled, bit 2 standby (bit 1 is set at create and never
  // tested).
  uint32_t state_ = 3;
  int32_t camera_id_ = 0;         // +0x54 handle of `cursor` (not read by the tick)
  int32_t viewport_id_ = 0;       // +0x64 handle of `cursor` (not read by the tick)
  int32_t parent_id_ = 0;         // +0x74 handle of `cursor`: the parent of the pointer text
  // +0x84 handle: the pointer text. Fork: a host RenderRef object that keys
  // the renderer's text and its place in the tree.
  InvObject* text_ = nullptr;
  bool text_shown_ = false;
  float x_ = 0.f;                 // +0xA4
  float y_ = 0.f;                 // +0xA8 (+0xAC z is always 0)
  // One per button: left (+0xD4, +0xE0, +0xB0, +0xEC, +0x10C, +0x11C) and
  // right (+0xD8, +0xE4, +0xBC, +0xFC, +0x114, +0x128). Written by step 2.
  struct Button {
    bool down = false;
    bool drag = false;                        // a drag has started
    float press_pos[3] = {0.f, 0.f, 0.f};     // the position at the press
    int32_t target_id = 0;                    // the owner node under the press
    int32_t body_id = 0;                      // its body
    float point[3] = {0.f, 0.f, 0.f};         // the hit point, body-local
  };
  Button left_;
  Button right_;
  // +0x138: getinfo 7 (the kind of object) of what was under a right
  // press. Written at the press, read by nothing in the tick.
  int32_t right_kind_ = 0;
  // The hover (+0x13C handle, +0x14C body ref, +0x154 point, +0x160
  // triangle index) and the pick (+0x164 position, +0x170 its t).
  // `disable` and `standby` clear the hover.
  int32_t hover_id_ = 0;
  int32_t hover_body_ = 0;
  float hover_point_[3] = {0.f, 0.f, 0.f};
  int32_t hover_tri_ = 0;
  float picked_[3] = {0.f, 0.f, 0.f};
  float picked_t_ = 0.f;
  int32_t second_id_ = 0;         // +0x180: the second handle of `activate` (never read)
  int32_t mode_ = 1;              // +0x1A8
  char pointer_[4] = {'+', 0, 0, 0};  // +0x1AC: one character and its NUL
  // "The pointer moved in this tick" (the tick's local_20): the fifth token
  // of HOVER and LEAVE.
  bool moved_ = false;
  // Host, for the log: the pick the last `[cursor] pick` line told.
  bool logged_hit_ = false;
  int32_t logged_obj_ = 0;
  int32_t logged_body_ = 0;
  int32_t logged_vp_ = 0;
  int32_t logged_cam_ = 0;

  // ---- vt+0x10 FUN_004b7c30 -> FUN_004b7510 ---------------------------------
  // Every handle and body ref is released and the object freed. No
  // ShowCursor, ClipCursor(NULL) or SetCursor; the pointer text goes with
  // the node tree.
  void destroy() override {
    dead_ = true;
    release_lock_fork();
    destroy_text();
  }

  // Fork: the exe keeps the clip of `lock` (FUN_004262c0) until `unlock`
  // (FUN_00425520) or the end of the process; `disable`, `standby`,
  // `deactivate` and the destructor leave it. A cursor that goes while the
  // right button orbits the garage camera (a left click on the X during the
  // drag: Garage.exit sends `enable 0` and drops its handler, and no
  // `unlock` follows) would leave the user's pointer pinned to one pixel
  // until the window closes. The host frees it at those four. In an
  // unattended run that clears the flag only (input_syscursor_unlock).
  void release_lock_fork() {
    if (g_sys_cursor != 0 && input_syscursor_locked()) input_syscursor_unlock();
  }

  // PE 940 FUN_0051f970 for the handles of the object.
  void nodes_gone(const std::vector<int32_t>& ids) override {
    auto gone = [&ids](int32_t id) {
      return id != 0 && std::binary_search(ids.begin(), ids.end(), id);
    };
    if (gone(controller_id_)) controller_id_ = 0;
    if (gone(second_id_)) second_id_ = 0;
    if (gone(camera_id_)) camera_id_ = 0;
    if (gone(hover_id_)) hover_id_ = 0;
    if (gone(left_.target_id)) left_.target_id = 0;
    if (gone(right_.target_id)) right_.target_id = 0;
    if (gone(parent_id_)) {
      parent_id_ = 0;
      // The text is a child of that node and went with it.
      // Fork: only its renderer entries are dropped here (this runs while
      // nodes are being retired); the host object of the text stays.
      if (text_) {
        render_d3d9_text_destroy(text_);
        render_d3d9_mesh_destroy(text_);
        text_ = nullptr;
        text_shown_ = false;
      }
    }
  }

  // *FUN_0047c640(cursor+0x1C, 0x43, n): logical axis n of the bound
  // controller (getinfo 0x43 of the `player` object). Fork: 0 when the
  // controller's node has no game object (the exe reads through a null
  // pointer there).
  float axis(int32_t n) const {
    InvObject* ctrl = script_node_object(controller_id_);
    NativeInfo info;
    info.arg = n;
    return (ctrl && native_getinfo(ctrl, 0x43, &info)) ? info.v[0] : 0.f;
  }

  static float clamp1(float v) {
    if (1.f < v) v = 1.f;    // DAT_00593598
    if (v < -1.f) v = -1.f;  // DAT_005935ac
    return v;
  }

  // ---- The pointer text ------------------------------------------------------
  // PE 940 FUN_004e8f40(cursor+0x84, parent cursor+0x74, text cursor+0x1AC,
  // position cursor+0xA4, font cursor+0x2C, colour, align 2, scale 1.0): a
  // type-3 render instance `r_text` under the parent instance, its resource
  // the font render object, bone00 at (x, y, 0), flag 0x8000. The text path
  // takes the bone00 world translation as the NDC anchor with y down, so
  // the glyph's pen is the cursor position.
  // Host: a text of the renderer keyed by a host RenderRef under the
  // parent's node object, as java_render_Text_create sets one up; the pass
  // of the priority-127 camera draws it (its root, the MouseCursor instance,
  // is above the cursor's node).
  void build_text() {
    InvObject* parent = script_node_object(parent_id_);
    if (!parent) return;
    InvObject* font = cursor_font_key(font_id_);
    if (!font) return;
    InvObject* r = tree_host_new("java.util.resource.RenderRef");
    if (!r) return;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      auto& rs = R(r);
      rs.id = resref_next_runtime_id();
      rs.type = 3;  // INSTANCE_RENDER
      rs.restype = 3;
      rs.type_id = font_id_;
      rs.parent = parent;
      rs.alias = "r_text";
      rs.loaded = true;
    }
    tree_field_set_obj(r, "alias", string_new("r_text"));
    render_d3d9_text_create(r, font, x_, y_);
    render_d3d9_text_set_tree(r, 1);
    render_d3d9_text_set_advance_scale(r, 1.f);
    render_d3d9_text_set_align(r, 2);
    render_d3d9_text_set_color(r, color_);
    render_d3d9_mesh_set_parent(r, parent);
    render_d3d9_mesh_set_attach_bone(
        r, render_d3d9_mesh_get_bone_id(parent, "bone00"));
    text_ = r;
    text_shown_ = true;  // a new text of the renderer is visible
    move_text();
    render_d3d9_text_set_string(r, pointer_);
    if (native_type_log())
      std::fprintf(stderr,
                   "[cursor] pointer text '%s' font=0x%08X colour=0x%08X "
                   "under node 0x%08X at (%.4f, %.4f)\n",
                   pointer_, static_cast<unsigned>(font_id_),
                   static_cast<unsigned>(color_),
                   static_cast<unsigned>(parent_id_), x_, y_);
  }

  // FUN_004e9450 on cursor+0x84.
  void destroy_text() {
    if (!text_) return;
    InvObject* r = text_;
    text_ = nullptr;
    text_shown_ = false;
    render_d3d9_text_destroy(r);
    java_util_resource_ResourceRef_destroy(r);
    java_util_resource_ResourceRef_deleteNative(r);
  }

  // FUN_004ec470(cursor+0x84, "bone00", 0, &position, identity). Nothing
  // without a text.
  void move_text() {
    if (!text_) return;
    const int32_t bone = render_d3d9_mesh_get_bone_id(text_, "bone00");
    render_d3d9_mesh_set_bone_local(text_, bone, x_, y_, 0.f, 0.f, 0.f, 0.f);
  }

  // Fork (SLRR_PE_CURSOR_DRAW): the drawn stand-in of the Windows cursor is
  // shown exactly while the cursor is enabled, like the Windows cursor
  // (`enable` / `disable` are ShowCursor there). The software pointer of the
  // exe (SysCursor 0) is never hidden by the cursor: the script unbinds the
  // priority-127 viewport instead.
  void sync_text() {
    if (!text_) return;
    const bool want = g_sys_cursor == 0 || (state_ & 1u) != 0;
    if (want == text_shown_) return;
    text_shown_ = want;
    render_d3d9_text_set_visible(text_, want ? 1 : 0);
  }

  // ---- The head of FUN_004b8430 (asm 0x4b845c..0x4b8606) -----------------------
  // Returns true when the cursor is enabled: the pick and the events
  // (tick_events) follow from there.
  bool tick_head(float dt) {
    moved_ = false;
    if (controller_id_ == 0) return false;  // no controller: nothing at all
    if (g_sys_cursor == 0) {
      // The software pointer integrates the rate axes 42 / 43.
      const float dx = axis(42) * g_cursor_sens_x * dt;
      const float dy = g_cursor_sens_y * axis(43) * dt;
      if (kCursorMovedEps < dy * dy + dx * dx) {
        moved_ = true;
        if (state_ & 4u) state_ |= 1u;  // standby wakes up, without a ShowCursor
      }
      x_ = x_ + dx;
      y_ = y_ + dy;
    } else {
      // The window procedure's position (DAT_005e93b8 / DAT_005e93bc): (0,
      // 0), the middle of the window, until the first mouse message.
      float wx = 0.f, wy = 0.f;
      input_syscursor_ndc(&wx, &wy);
      const float ex = x_ - wx;
      const float ey = y_ - wy;
      if (kCursorMovedEps < ex * ex + ey * ey) {
        moved_ = true;
        if (state_ & 4u) state_ |= 1u;
      }
      x_ = wx;
      y_ = wy;
    }
    x_ = clamp1(x_);
    y_ = clamp1(y_);
    move_text();
    return (state_ & 1u) != 0;  // disabled: no pick, no events
  }

  // FUN_0047c640(cursor+0x1C, 0x19, n) x _DAT_0059ab38 > DAT_00593838:
  // logical axis n of the bound controller as the player's getinfo 0x19
  // gives it, (int)(value x 127), times 1 / 127 (0x3C010204), above 0.2
  // (0x3E4CCCCD). Fork: not pressed when the controller's node has no game
  // object (the exe reads through a null pointer there).
  bool button(int32_t n) const {
    InvObject* ctrl = script_node_object(controller_id_);
    NativeInfo info;
    info.arg = n;
    const int32_t v = (ctrl && native_getinfo(ctrl, 0x19, &info)) ? info.i : 0;
    return 0.2f < static_cast<float>(v) * 0.0078740157f;
  }

  // The id field of one of the object's handles: 0 once its node is gone.
  static int32_t hid(int32_t& id) {
    if (id != 0 && !cursor_node(id)) id = 0;
    return id;
  }

  // FUN_0047d550(handle, sender handle or 0, 0x10000, text, 0). Every send
  // runs script code that may do anything to this object (a command, the
  // destroy of the node under the pointer or of the cursor itself): the
  // tick reads its fields again after each, and ids are resolved here.
  void send(int32_t target_id, int32_t sender_id, const char* text) {
    if (dead_) return;
    InvObject* target = cursor_node(target_id);
    if (!target) return;  // no game node behind the handle: nothing is sent
    // Fork: a sender handle whose node is gone counts as no sender (the exe
    // wraps the dead handle in the GameRef it hands the script).
    gametype_send_event(target, cursor_node(sender_id), 0x10000, text,
                        native_type_log() ? "[cursor] ev" : nullptr);
  }

  // `[cursor] pick ...`, on every change of what the pick found.
  void log_pick(bool hit) {
    if (!native_type_log()) return;
    if (hit == logged_hit_ && g_pick.obj_id == logged_obj_ &&
        g_pick.body_id == logged_body_ &&
        (!hit || (g_pick.vp_pri == logged_vp_ && g_pick.cam_pri == logged_cam_)))
      return;
    logged_hit_ = hit;
    logged_obj_ = g_pick.obj_id;
    logged_body_ = g_pick.body_id;
    logged_vp_ = g_pick.vp_pri;
    logged_cam_ = g_pick.cam_pri;
    if (cursor_pick_dump_on()) cursor_pick_dump(x_, y_);
    if (!hit) {
      std::fprintf(stderr, "[cursor] pick none\n");
      return;
    }
    InvObject* obj = cursor_node(g_pick.obj_id);
    const char* cn = obj ? tree_host_class(obj) : nullptr;
    std::fprintf(stderr,
                 "[cursor] pick vp=%d cam=%d obj=0x%08X '%s' phy=%d t=%.4f "
                 "local=(%.4f, %.4f, %.4f)\n",
                 g_pick.vp_pri, g_pick.cam_pri,
                 static_cast<unsigned>(g_pick.obj_id), cn ? cn : "",
                 g_pick.body_id, g_pick.t, g_pick.point[0], g_pick.point[1],
                 g_pick.point[2]);
  }

  // ---- The rest of FUN_004b8430 (asm 0x4b860b..0x4b9b71) -----------------------
  // The pick, then hover / leave, the left button, the right button.
  // Everything is edge triggered. "target" = what was under the pointer
  // when the button went down, "hit" = what the pick of this tick found.
  // The strings (formats at 0x59e054 .. 0x59dee8): first token the EC code,
  // second the cursor's id; ids with %d, points with %.3f.
  void tick_events() {
    const bool hit = cursor_pick(x_, y_);
    // 0x4b864e..0x4b86fd, with no test of the result: the picked position
    // (A - B) t + B and its t. After a miss t is 0 and A, B are those of
    // the last pick that hit: the near point of that pick (M4 Q11).
    for (int i = 0; i < 3; ++i) {
      picked_[i] = g_pick.far_pt[i] - g_pick.near_pt[i];
      picked_[i] = g_pick.t * picked_[i];
      picked_[i] = g_pick.near_pt[i] + picked_[i];
    }
    picked_t_ = g_pick.t;
    log_pick(hit);
    const int32_t moved = moved_ ? 1 : 0;
    char text[256];
    char text2[256];

    // ---- HOVER 15 / LEAVE 16: on a change of (object, body) --------------------
    if (hid(hover_id_) != g_pick.obj_id || hover_body_ != g_pick.body_id) {
      if (hover_id_ != 0) {
        std::snprintf(text, sizeof text, "%d %d %d %d %d", 16, self_id_,
                      hover_id_, hover_body_, moved);
        send(hover_id_, 0, text);  // the old hover object,
        send(self_id_, 0, text);   // then self
      }
      // The hover handle and body are bound to the pick block as it is now.
      hover_id_ = cursor_live_id(g_pick.obj_id);
      hover_body_ = g_pick.body_id;
      if (hover_id_ != 0) {
        std::snprintf(text, sizeof text, "%d %d %d %d %d", 15, self_id_,
                      hover_id_, hover_body_, moved);
        send(hover_id_, 0, text);
        send(self_id_, 0, text);
      }
    }
    hover_tri_ = g_pick.tri;
    hover_point_[0] = g_pick.point[0];
    hover_point_[1] = g_pick.point[1];
    hover_point_[2] = g_pick.point[2];
    // (Config.MouseHelp: the debug text and the box lines of the hover
    // object are drawn here in the exe; not ported.)

    // ---- LEFT: axis 45 (asm 0x4b8f3f..) -------------------------------------------
    const float thr2 = kCursorDragStep * kCursorDragStep;
    if (!button(45)) {
      if (left_.down) {
        left_.down = false;
        // LUP 2: self, then the target.
        std::snprintf(text, sizeof text, "%d %d %d %d", 2, self_id_,
                      hid(left_.target_id), left_.body_id);
        send(self_id_, 0, text);
        if (hid(left_.target_id) != 0) send(left_.target_id, 0, text);
        if (!left_.drag) {
          if (hid(left_.target_id) != 0) {
            // LCLICK 5, both forms: the press point to the target; the
            // press point and the picked position of now to self, with the
            // target as sender.
            std::snprintf(text, sizeof text, "%d %d %d %.3f %.3f %.3f", 5,
                          self_id_, left_.body_id, left_.point[0],
                          left_.point[1], left_.point[2]);
            send(left_.target_id, 0, text);
            std::snprintf(text, sizeof text,
                          "%d %d %d %.3f %.3f %.3f %.3f %.3f %.3f", 5, self_id_,
                          hid(left_.target_id), left_.point[0], left_.point[1],
                          left_.point[2], picked_[0], picked_[1], picked_[2]);
            send(self_id_, left_.target_id, text);
          }
        } else {
          // LDRAGEND 10: self, then the target.
          std::snprintf(text2, sizeof text2, "%d %d %d", 10, self_id_,
                        left_.body_id);
          send(self_id_, 0, text2);
          if (hid(left_.target_id) != 0) {
            send(left_.target_id, 0, text2);
            if (hit) {
              // LDROP 11, both forms: to the object under the pointer now;
              // to self with that object as sender.
              std::snprintf(text, sizeof text, "%d %d %d %d %d", 11, self_id_,
                            g_pick.body_id, hid(left_.target_id),
                            left_.body_id);
              send(g_pick.obj_id, 0, text);
              std::snprintf(text, sizeof text,
                            "%d %d %d %d %.3f %.3f %.3f %d", 11, self_id_,
                            cursor_live_id(g_pick.obj_id),
                            hid(left_.target_id), picked_[0], picked_[1],
                            picked_[2], left_.body_id);
              send(self_id_, g_pick.obj_id, text);
            }
          }
        }
        left_.target_id = 0;  // FUN_00488080, FUN_00501ca0: target and body released
        left_.body_id = 0;
      }
    } else if (!left_.down) {
      left_.down = true;
      left_.drag = false;
      left_.press_pos[0] = x_;
      left_.press_pos[1] = y_;
      left_.press_pos[2] = 0.f;
      if (!hit) {
        left_.target_id = 0;
        left_.body_id = 0;  // the press point keeps what it had
      } else {
        left_.target_id = cursor_live_id(g_pick.obj_id);
        left_.body_id = g_pick.body_id;
        left_.point[0] = g_pick.point[0];
        left_.point[1] = g_pick.point[1];
        left_.point[2] = g_pick.point[2];
      }
      // LDOWN 1: the target, then self.
      std::snprintf(text, sizeof text, "%d %d %d %d %.3f %.3f %.3f", 1,
                    self_id_, left_.target_id, left_.body_id, left_.point[0],
                    left_.point[1], left_.point[2]);
      if (left_.target_id != 0) send(left_.target_id, 0, text);
      send(self_id_, 0, text);
    } else if (!left_.drag) {
      const float ex = left_.press_pos[0] - x_;
      const float ey = left_.press_pos[1] - y_;
      const float ez = left_.press_pos[2] - 0.f;
      if (thr2 <= ex * ex + ey * ey + ez * ez) {
        left_.drag = true;
        // LDRAGBEGIN 9: self, then the target.
        std::snprintf(text, sizeof text, "%d %d %d", 9, self_id_,
                      left_.body_id);
        send(self_id_, 0, text);
        if (hid(left_.target_id) != 0) send(left_.target_id, 0, text);
      }
    }

    // ---- RIGHT: axis 46 (asm 0x4b9433..); the tick ends in each branch ---------
    if (button(46)) {
      if (!right_.down) {
        right_.down = true;
        right_.drag = false;
        right_.press_pos[0] = x_;
        right_.press_pos[1] = y_;
        right_.press_pos[2] = 0.f;
        if (!hit) {
          right_.target_id = 0;
          right_.body_id = 0;
        } else {
          right_.target_id = cursor_live_id(g_pick.obj_id);
          right_.body_id = g_pick.body_id;
          right_.point[0] = g_pick.point[0];
          right_.point[1] = g_pick.point[1];
          right_.point[2] = g_pick.point[2];
          NativeInfo kind;
          InvObject* obj = cursor_node(right_.target_id);
          right_kind_ = (obj && native_getinfo(obj, 7, &kind)) ? kind.i : 0;
        }
        // RDOWN 3: the target, then self.
        std::snprintf(text2, sizeof text2, "%d %d %d %d", 3, self_id_,
                      right_.target_id, right_.body_id);
        if (right_.target_id != 0) send(right_.target_id, 0, text2);
        send(self_id_, 0, text2);
        return;
      }
      if (right_.drag) return;
      const float ex = right_.press_pos[0] - x_;
      const float ey = right_.press_pos[1] - y_;
      const float ez = right_.press_pos[2] - 0.f;
      if (ex * ex + ey * ey + ez * ez < thr2) return;
      right_.drag = true;
      left_.drag = true;  // asm 0x4b96ef: the LEFT drag flag as well
      // RDRAGBEGIN 12: self, then the target.
      std::snprintf(text2, sizeof text2, "%d %d %d", 12, self_id_,
                    right_.body_id);
      send(self_id_, 0, text2);
      if (hid(right_.target_id) == 0) return;
      send(right_.target_id, 0, text2);
      return;
    }
    if (!right_.down) return;
    right_.down = false;
    // RUP 4: the target, then self.
    std::snprintf(text2, sizeof text2, "%d %d %d %d", 4, self_id_,
                  hid(right_.target_id), right_.body_id);
    if (right_.target_id != 0) send(right_.target_id, 0, text2);
    send(self_id_, 0, text2);
    if (!right_.drag) {
      if (hid(right_.target_id) != 0) {
        // RCLICK 6, both forms.
        std::snprintf(text, sizeof text, "%d %d %d %.3f %.3f %.3f", 6,
                      self_id_, right_.body_id, right_.point[0],
                      right_.point[1], right_.point[2]);
        send(right_.target_id, 0, text);
        // The press point goes out in the pressed body's frame, for every
        // target. When that body is not the object's main body (getinfo 1
        // -> FUN_004eabc0 -> +0xb4) the exe converts a point local -> world
        // -> main-body local (FUN_004a2b40, FUN_004af9a0; asm 0x4b98c4..
        // 0x4b9950), but in place on its local [EBP-0x28] (local_2c), while
        // the format reads the unconverted copy of cursor+0x128 at
        // [EBP-0x18..-0x10] (0x4b98a4..0x4b98b9, pushed at 0x4b999b..
        // 0x4b99bf): the conversion never reaches the text.
        std::snprintf(text, sizeof text,
                      "%d %d %d %.3f %.3f %.3f %.3f %.3f %.3f", 6, self_id_,
                      hid(right_.target_id), right_.point[0], right_.point[1],
                      right_.point[2], picked_[0], picked_[1], picked_[2]);
        send(self_id_, right_.target_id, text);
      }
    } else {
      // RDRAGEND 13: self, then the target.
      std::snprintf(text, sizeof text, "%d %d %d", 13, self_id_,
                    right_.body_id);
      send(self_id_, 0, text);
      if (hid(right_.target_id) != 0) {
        send(right_.target_id, 0, text);
        if (hit) {
          // RDROP 14, both forms.
          std::snprintf(text, sizeof text, "%d %d %d %d %d", 14, self_id_,
                        g_pick.body_id, hid(right_.target_id),
                        right_.body_id);
          send(g_pick.obj_id, 0, text);
          std::snprintf(text, sizeof text, "%d %d %d %d %.3f %.3f %.3f", 14,
                        self_id_, cursor_live_id(g_pick.obj_id),
                        hid(right_.target_id), picked_[0], picked_[1],
                        picked_[2]);
          send(self_id_, g_pick.obj_id, text);
        }
      }
    }
    right_.target_id = 0;
    right_.body_id = 0;
  }

  // ---- vt+0x34 FUN_004b78f0, once per main-loop pass ---------------------------
  void animate(float dt) override {
    struct TickTime {
      bool on;
      std::chrono::steady_clock::time_point t0;
      ~TickTime() {
        if (!on) return;
        ++g_cursor_prof.ticks;
        g_cursor_prof.tick_us += cursor_us_since(t0);
      }
    } tick_time{native_type_log(), std::chrono::steady_clock::now()};
    if (tick_head(dt)) tick_events();
    if (dead_) return;  // a handler destroyed the cursor
    if (parent_id_ != 0) {
      // The exe builds the text here only while SysCursor is 0.
      if ((g_sys_cursor == 0 || cursor_draw_fork()) && !text_) build_text();
      // Not ported: the Config.MouseHelp debug text (FUN_004e8df0 at (-0.95,
      // 0.95) in the font cursor+0x3C); the host's Config has MouseHelp 0.
    }
    sync_text();
  }

  // ---- vt+0x3C FUN_004b7c60 ------------------------------------------------------
  // `out->i` is the value the exe's function returns.
  bool getinfo(int32_t query, NativeInfo* out) override {
    switch (query) {
      case 2:  // GII_POS: x, y, and the pick's t in the third slot
        out->v[0] = x_;
        out->v[1] = y_;
        out->v[2] = picked_t_;
        out->i = 1;
        return true;
      case 0x1c:  // GII_ANIMATE
        out->i = 1;
        return true;
      case 0x2a:  // GII_MODE
        out->i = mode_;
        return true;
      case 0x2c:  // the hover object
        out->obj = hover_id_ ? script_node_object(hover_id_) : nullptr;
        out->i = hover_id_;
        return true;
      case 0x3b:  // the picked position; returns the hover object's id
        out->v[0] = picked_[0];
        out->v[1] = picked_[1];
        out->v[2] = picked_[2];
        out->i = hover_id_;
        return true;
      default:
        out->i = 0;
        return false;
    }
  }

  // `disable` and `standby`: the hover handle and body are released and the
  // hover point zeroed. NO leave event is sent.
  void release_hover() {
    hover_id_ = 0;
    hover_body_ = 0;
    hover_tri_ = 0;
    hover_point_[0] = hover_point_[1] = hover_point_[2] = 0.f;
  }

  // ---- vt+0x38 FUN_004b7d70: commands -------------------------------------------
  bool event(int32_t type, const char* text) override {
    // The handler returns 1 for every event and reads type 0x10
    // (GameRef.command) only. GameRef.queueEvent calls the sender directly,
    // so a command runs inside the script call.
    if (type != 0x10) return true;
    if (!text) text = "";
    if (native_type_log()) std::fprintf(stderr, "[cursor] cmd '%s'\n", text);
    char word[64];
    word[0] = 0;
    engine_sscanf(text, "%63s", word);  // "%s" into a 64-byte buffer
    // The words are compared with FUN_0057c11e (_stricmp) in this order.
    if (word_is(word, "cursor")) {
      // "%*s %d %d %d": the viewport (a handle of type 0x12), the camera
      // (FUN_0051f9b0: any type) and the game instance the pointer text
      // hangs under. MouseCursor sends its own cursor instance as the third.
      int32_t vp = 0, cam = 0, inst = 0;
      engine_sscanf(text, "%*s %d %d %d", &vp, &cam, &inst);
      viewport_id_ = camera_viewport_by_id(vp) ? vp : 0;
      camera_id_ = cam;
      parent_id_ = cursor_bind_instance(inst);
    } else if (word_is(word, "mode")) {
      // "%*s %d %1s %d": the mode, the pointer letter and a font id that
      // starts as the current one; not 0 = the font handle is bound to it
      // (type 0xE, a render object). The pointer text is destroyed so that
      // the tick builds it again, then the Windows shape follows the letter.
      int32_t font = font_id_;
      engine_sscanf(text, "%*s %d %1s %d", &mode_, pointer_, &font);
      pointer_[1] = 0;
      if (font != 0) font_id_ = render_object_desc(font)->found ? font : 0;
      destroy_text();
      cursor_set_shape(cursor_shape_of(pointer_[0]));
    } else if (word_is(word, "color")) {
      // "%*s %d" into cursor+0x4C, then the colour of the text
      // (FUN_004ebd20: render payload +0x17C).
      int32_t c = static_cast<int32_t>(color_);
      engine_sscanf(text, "%*s %d", &c);
      color_ = static_cast<uint32_t>(c);
      if (text_) render_d3d9_text_set_color(text_, color_);
    } else if (word_is(word, "activate")) {
      // "%*s %d %d": the controller, and a second instance handle (the
      // camera of MouseCursor.enableCameraControl) that nothing reads.
      int32_t a = 0, b = 0;
      engine_sscanf(text, "%*s %d %d", &a, &b);
      controller_id_ = cursor_bind_instance(a);
      second_id_ = cursor_bind_instance(b);
      activated_ = true;
    } else if (word_is(word, "deactivate")) {
      controller_id_ = 0;  // the tick then returns at once
      activated_ = false;
      release_lock_fork();
    } else if (word_is(word, "enable")) {
      if (g_sys_cursor != 0 && (state_ & 1u) == 0) input_syscursor_show(true);
      state_ |= 1u;
    } else if (word_is(word, "disable")) {
      if (g_sys_cursor != 0 && (state_ & 1u) != 0) input_syscursor_show(false);
      state_ &= ~5u;
      release_hover();
      release_lock_fork();
    } else if (word_is(word, "standby")) {
      // As `disable`, with bit 2: the next pointer movement enables the
      // cursor again (the tick). No sender in the shipped scripts.
      if (g_sys_cursor != 0 && (state_ & 1u) != 0) input_syscursor_show(false);
      state_ = (state_ & ~1u) | 4u;
      release_hover();
      release_lock_fork();
    } else if (word_is(word, "lock")) {
      // FUN_004262c0: GetCursorPos, ClipCursor to that one point.
      if (g_sys_cursor != 0) input_syscursor_lock();
    } else if (word_is(word, "unlock")) {
      // FUN_00425520: ClipCursor(NULL).
      if (g_sys_cursor != 0) input_syscursor_unlock();
    } else if (word_is(word, "sens")) {
      cursor_read_sens();
    } else if (word_is(word, "relmove") || word_is(word, "move")) {
      // "%*s %f,%f" (`move 0, 0.2` parses: %f skips the blank). The
      // position, each part kept inside [-1, 1], then bone00 of the text.
      // Fork: a number the text lacks reads 0 (the exe leaves its local as
      // it was: the event type and the text pointer seen as floats).
      float fx = 0.f, fy = 0.f;
      engine_sscanf(text, "%*s %f,%f", &fx, &fy);
      if (word_is(word, "relmove")) {
        fx = fx + x_;
        fy = fy + y_;
      }
      x_ = clamp1(fx);
      y_ = clamp1(fy);
      move_text();
    } else if (word_is(word, "setpos")) {
      // FUN_00426cb0: SetCursorPos. No sender in the shipped scripts.
      float fx = 0.f, fy = 0.f;
      engine_sscanf(text, "%*s %f,%f", &fx, &fy);
      input_syscursor_set_pos(fx, fy);
    }
    return true;
  }
};

// PE 940 FUN_004b7a10 (type vt+0x0C). The params string ("0,0,0" from
// MouseCursor.<init>) is never read; no node and no render instance is made.
std::shared_ptr<NativeObject> cursor_create(const NativeCreate& c) {
  auto cur = std::make_shared<CursorObject>();
  // cursor+0x2C: the pointer font, frontend.rpk:0x23 (console10) until
  // `mode` names another. (cursor+0x3C, the font of the MouseHelp debug
  // text, is the same record; that text is not ported.)
  if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
    cur->font_id_ = rpak_make_id(fe->pack_id, 0x23);
  cursor_read_sens();
  // Colour 0xFFFFFFFF, pointer string "+", position and press state 0, mode
  // 1, state 3: the member initialisers. (cursor+0x190 = 3.0 has no reader,
  // M4 Q14.)
  // FUN_004ebd90(cursor, 0x10, 0): event mask |= EVENT_COMMAND. Host:
  // GameRef.queueEvent hands a command to the object without a mask test.
  const char* sys_source = "";
  cursor_read_sys_cursor(&sys_source);
  // cursor+0x00: the handle on the cursor's own node; its id (+0x08) is the
  // second token of every event string.
  cur->self_id_ = java_util_resource_ResourceRef_id(c.handle);
  cur->owns_create_pose = true;
  // getinfo 0x1C answers 1 and 8 / 9 answer 0: FUN_0047d560 puts the
  // instance on the ANIMATE list only.
  cur->animates = true;
  if (native_type_log())
    std::fprintf(stderr,
                 "[cursor] create node=0x%08X sys=%d (%s) sens=%g font=0x%08X "
                 "draw=%d unattended=%d\n",
                 static_cast<unsigned>(
                     java_util_resource_ResourceRef_id(c.handle)),
                 g_sys_cursor, sys_source, g_cursor_sens_x,
                 static_cast<unsigned>(cur->font_id_),
                 cursor_draw_fork() ? 1 : 0, input_mouse_unattended() ? 1 : 0);
  cursor_load_shapes();
  return cur;
}

}  // namespace

// `[cursor] state ...`: one line per cursor object at loop end
// (SLRR_PE_NATIVE_PROF), printed beside the `native camera` line.
void native_cursor_debug_dump(FILE* out) {
  if (!out || !native_script_boot() || !native_type_log()) return;
  std::vector<std::shared_ptr<NativeObject>> curs;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    for (const auto& kv : g_objects) {
      if (!kv.second || kv.second->type_index != kNativeCursor) continue;
      bool seen = false;
      for (const auto& c : curs)
        if (c == kv.second) seen = true;
      if (!seen) curs.push_back(kv.second);
    }
  }
  const float w = static_cast<float>(render_d3d9_width());
  const float h = static_cast<float>(render_d3d9_height());
  for (const auto& o : curs) {
    const CursorObject* c = static_cast<const CursorObject*>(o.get());
    std::fprintf(out,
                 "[cursor] state pos=(%.4f, %.4f) px=(%ld, %ld) enabled=%d "
                 "standby=%d ctrl=0x%08X letter=%s mode=%d font=0x%08X "
                 "shape=%d sys=%d locked=%d text=%d hover=0x%08X/%d ldown=%d "
                 "rdown=%d ldrag=%d rdrag=%d picked=(%.4f, %.4f, %.4f) "
                 "t=%.4f\n",
                 c->x_, c->y_, std::lround((c->x_ + 1.f) * 0.5f * w),
                 std::lround((c->y_ + 1.f) * 0.5f * h),
                 (c->state_ & 1u) ? 1 : 0, (c->state_ & 4u) ? 1 : 0,
                 static_cast<unsigned>(c->controller_id_),
                 c->pointer_[0] ? c->pointer_ : "?", c->mode_,
                 static_cast<unsigned>(c->font_id_), g_cursor_shape,
                 g_sys_cursor, input_syscursor_locked() ? 1 : 0,
                 (c->text_ && c->text_shown_) ? 1 : 0,
                 static_cast<unsigned>(c->hover_id_), c->hover_body_,
                 c->left_.down ? 1 : 0, c->right_.down ? 1 : 0,
                 c->left_.drag ? 1 : 0, c->right_.drag ? 1 : 0, c->picked_[0],
                 c->picked_[1], c->picked_[2], c->picked_t_);
  }
  // What the tick cost, over all cursor objects (there is one).
  const CursorProf& p = g_cursor_prof;
  if (p.ticks != 0) {
    const double np = p.picks ? static_cast<double>(p.picks) : 1.0;
    std::fprintf(out,
                 "[cursor] prof ticks=%llu tick=%.1f ms (%.1f us each, script "
                 "handlers included) picks=%llu rays=%.1f bodies=%.1f "
                 "walk=%.1f us each\n",
                 static_cast<unsigned long long>(p.ticks), p.tick_us / 1000.0,
                 p.tick_us / static_cast<double>(p.ticks),
                 static_cast<unsigned long long>(p.picks), p.rays_us / np,
                 p.bodies_us / np, p.walk_us / np);
  }
}
