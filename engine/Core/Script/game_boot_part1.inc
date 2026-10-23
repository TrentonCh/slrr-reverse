#include "tree_interp_internal.hpp"
#include "Resources_internal.hpp"  // m5 rig: g_res / g_mu (the car's node id)
#include <atomic>
#include <chrono>
#include <cstdlib>
// Fragment 1/2 of game_boot.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


namespace inv {
namespace {

#ifdef _WIN32
// Fork: the legacy smoke tests press a REAL key. keybd_event goes to the
// foreground window of the desktop, not to this process, so with the host
// window in the background (SLRR_WINDOW_NOACTIVATE=1, or the user working in
// another window) the key landed in the user's editor or chat. The host reads
// no key while its window is not in front (the foreground gate of
// input_win32.cpp), so such a press never reached the test either. Send it
// only while the host window is in front; send the key-up only after a
// key-down that was sent, so no key is left held down.
bool g_smoke_key_down = false;
void smoke_key(BYTE vk, bool up) {
  if (!up) {
    g_smoke_key_down =
        GetForegroundWindow() == static_cast<HWND>(inv::render_d3d9_hwnd());
    if (g_smoke_key_down) keybd_event(vk, 0, 0, 0);
  } else if (g_smoke_key_down) {
    keybd_event(vk, 0, KEYEVENTF_KEYUP, 0);
    g_smoke_key_down = false;
  }
}
#endif

// Soft PE ResHandle (16B on stock): +8 remapped key, +0xC owner.
// Host callers (GameRef_core) use a 16B scratch with int32 key @ +8 —
// Engine_LoadGameInit gates *(wt+8)!=0 @ 0x53A640.
using SoftBootRh = uint8_t[16];

static void soft_boot_rh_set_key(SoftBootRh rh, int32_t key) {
  std::memset(rh, 0, 16);
  *reinterpret_cast<int32_t*>(rh + 8) = key;
}

// Soft PE Engine_boot@0x58C700 gate helpers (InitState Config vs MainLoop GameInit).
// PE Engine_InitState@0x427980 (pre-FMV): Remap(system.rpk,4) →
// g_WorldTreeRoot@636460 Bind type=1; Remap(system.rpk,0x47=71) Bind type=8 →
// LoadGameInit(...,"Config")@0x53A5E0 → link EngineState+0xF8..+0x104.
// PE Engine_MainLoop@0x428960 (post-FMV): Config.init → Bind type=8;
// Engine_LoadGameInit(wt, typeRH, unk_63C560, "GameInit")@0x53A5E0 →
// ResHandle_Link into EngineState+0xE8..+0xF4 before while(!quit).
// Host: soft Remap + engine_load_game_init stand-in. Config.init→GameInit
// type RH unknown without Config GameType payload — GameInit type_rh=0
// (node+alias still created; THRD-CREATE attach when type mid present).
struct GameInitPrologue {
  uint32_t wt_key = 0;
  uint32_t config_type_key = 0;
  void* config_gi = nullptr;
  void* gameinit_gi = nullptr;
  int32_t config_rid = 0;
  int32_t gameinit_rid = 0;
};

// Soft ≈ Engine_InitState Config slice @ 0x427980 (before Engine_PlayBootFMV).
GameInitPrologue game_boot_initstate_config_prologue() {
  GameInitPrologue out{};
  out.wt_key = resource_engine_remap_from_path("system.rpk", /*local=*/4);
  out.config_type_key =
      resource_engine_remap_from_path("system.rpk", /*local=*/71);
  if (out.wt_key == 0) {
    std::printf(
        "  Soft InitState@0x427980: wt Remap(system.rpk,4)=0 "
        "(skip LoadGameInit Config)\n");
    return out;
  }

  alignas(4) SoftBootRh wt{};
  soft_boot_rh_set_key(wt, static_cast<int32_t>(out.wt_key));

  alignas(4) SoftBootRh config_type{};
  soft_boot_rh_set_key(config_type, static_cast<int32_t>(out.config_type_key));
  out.config_gi = engine_load_game_init(
      wt,
      out.config_type_key != 0 ? static_cast<void*>(config_type) : nullptr, "",
      "Config");
  if (out.config_gi) {
    out.config_rid =
        *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(out.config_gi) +
                                    0x50);
  }

  std::printf(
      "  Soft InitState@0x427980 Config: wt=0x%X gi=%p rid=0x%X type=0x%X "
      "— PE LoadGameInit@0x53A5E0 (pre-FMV)\n",
      out.wt_key, out.config_gi, out.config_rid, out.config_type_key);
  return out;
}

// Soft ≈ Engine_MainLoop GameInit slice @ 0x428960 (after FMV triad).
void game_boot_mainloop_gameinit_load(GameInitPrologue& out) {
  if (out.wt_key == 0) {
    out.wt_key = resource_engine_remap_from_path("system.rpk", /*local=*/4);
  }
  if (out.wt_key == 0) {
    std::printf(
        "  Soft MainLoop@0x428960 GameInit: wt Remap(system.rpk,4)=0 "
        "(skip LoadGameInit)\n");
    return;
  }

  alignas(4) SoftBootRh wt{};
  soft_boot_rh_set_key(wt, static_cast<int32_t>(out.wt_key));

  // Stock type RH = Config script field "init" (MainLoop@0x42899E..0x4289CD).
  // Soft: nullptr type — InstanceNode alias "GameInit" still allocated.
  out.gameinit_gi = engine_load_game_init(wt, nullptr, "", "GameInit");
  if (out.gameinit_gi) {
    out.gameinit_rid = *reinterpret_cast<int32_t*>(
        reinterpret_cast<char*>(out.gameinit_gi) + 0x50);
  }

  std::printf(
      "  Soft MainLoop@0x428960 GameInit: wt=0x%X gi=%p rid=0x%X "
      "— PE LoadGameInit@0x53A5E0 (post-FMV)\n",
      out.wt_key, out.gameinit_gi, out.gameinit_rid);
}

// --boot (no FMV): Config+GameInit back-to-back stand-in for InitState+MainLoop.
GameInitPrologue game_boot_mainloop_gameinit_prologue() {
  GameInitPrologue out = game_boot_initstate_config_prologue();
  game_boot_mainloop_gameinit_load(out);
  return out;
}

// PE Engine_MainLoop frame tail: ResourceEngine_PumpLoadQueue@0x5378D0 +
// PumpUnloadQueue@0x537B40 (after Sfx; before Present). Soft each host frame.
void game_boot_mainloop_resource_pumps() {
  (void)resource_engine_pump_load_queue();
  (void)resource_engine_pump_unload_queue();
}

bool game_boot_warmup(Jvm& jvm, const char* game_root) {
  if (!game_root || !game_root[0]) {
    std::fprintf(stderr, "[boot] missing game_root\n");
    return false;
  }

  if (!jvm.find_class("java.lang.System") &&
      !jvm.load_class("java.lang.System")) {
    std::printf("FAIL load System\n");
    return false;
  }

  jvm.load_class("java.game.GameLogic");
  jvm.load_class("java.game.SplashScreen");
  jvm.load_class("java.game.MainMenu");
  jvm.load_class("java.game.Garage");
  jvm.load_class("java.game.Valocity");
  jvm.load_class("java.lang.Object");
  jvm.load_class("java.lang.String");
  jvm.load_class("java.io.FindFile");
  jvm.load_class("java.io.File");

  {
    const char* dir = "tree_rpk_scan_boot";
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    auto write_min = [](const char* path) {
      unsigned char hdr[24] = {'R', 'P', 'A', 'K', 0x00, 0x02, 0, 0, 0, 0, 0,
                               0,   0,   0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0};
      if (FILE* tf = std::fopen(path, "wb")) {
        std::fwrite(hdr, 1, sizeof(hdr), tf);
        std::fclose(tf);
      }
    };
    write_min((std::string(dir) + "/a.rpk").c_str());
    write_min((std::string(dir) + "/b.rpk").c_str());
    jvm.invoke("java.lang.System", "rpkScan", "(Ljava.lang.String;)I",
               {JvmValue::make_obj(string_new((std::string(dir) + "/").c_str()))},
               true);
  }

  const char* dirs[] = {"parts/engines/", "parts/", "cars/racers/",
                        "cars/fake_racers/", "maps/"};
  int scanned = 0;
  for (const char* d : dirs) {
    JvmValue n = jvm.invoke("java.lang.System", "rpkScan",
                            "(Ljava.lang.String;)I",
                            {JvmValue::make_obj(string_new(d))}, true);
    const int got = (n.tag == JvmTag::Int) ? n.v.i : 0;
    scanned += got > 0 ? got : 0;
    std::printf("  rpkScan %s -> %d\n", d, got);
  }
  java_lang_System_openLib(string_new("system.rpk"));
  java_lang_System_openLib(string_new("frontend.rpk"));
  java_lang_System_openLib(string_new("cars.rpk"));
  java_lang_System_openLib(string_new("humans.rpk"));
  std::printf("  packs=%zu scanned_hits=%d\n", rpak_count(), scanned);

  if (!jvm.load_class("java.game.GameLogic")) {
    std::printf("FAIL load GameLogic\n");
    return false;
  }
  jvm.invoke("java.game.GameLogic", "initVehicleTypes", "()V", {}, true);
  InvObject* vts = game_logic_vehicle_types();
  const int nvt = tree_vector_size(vts);
  std::printf("  vehicleTypes=%d\n", nvt);
  if (nvt < 9) {
    std::printf("FAIL initVehicleTypes\n");
    return false;
  }
  frontend_init();
  return true;
}

// Fork (m5 test rig; the exe has nothing of this, and nothing below runs
// without its variables). The time of an entry of SLRR_PE_BOOT_KEYS,
// SLRR_PE_BOOT_MOUSE or SLRR_PE_BOOT_END: `t` = seconds after loop start
// (as before), `@<State>[#n][+secs]` = secs after the n-th return of the
// state's `enter` (the `[career] entered <State>` line; <State> = the simple
// class name of GameLogic.actualState, n from 1), or a bare `+secs` after an
// anchored entry = the anchor of the entry before it, secs after it. An
// anchored entry has no time until its anchor is reached. Returns the
// character after the time, or nullptr when there is none to read.
struct BootAnchor {
  std::string state;  // empty: an absolute time
  int32_t n = 1;
  double secs = 0.0;
};

bool boot_name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '$';
}

const char* boot_entry_time(const char* p, const BootAnchor* prev,
                            BootAnchor* a, double* t) {
  *a = BootAnchor{};
  *t = 0.0;
  char* e = nullptr;
  if (*p == '@') {
    const char* q = p + 1;
    while (boot_name_char(*q)) ++q;
    if (q == p + 1) return nullptr;
    a->state.assign(p + 1, q);
    if (*q == '#') {
      const long n = std::strtol(q + 1, &e, 10);
      if (e == q + 1 || n < 1) return nullptr;
      a->n = static_cast<int32_t>(n);
      q = e;
    }
    if (*q == '+' || *q == '-') {
      a->secs = std::strtod(q, &e);
      if (e == q) return nullptr;
      q = e;
    }
    return q;
  }
  if (*p == '+' && prev && !prev->state.empty()) {
    a->secs = std::strtod(p, &e);
    if (e == p) return nullptr;
    a->state = prev->state;
    a->n = prev->n;
    return e;
  }
  *t = std::strtod(p, &e);
  return e == p ? nullptr : e;
}

std::string boot_anchor_text(const BootAnchor& a) {
  char b[192];
  std::snprintf(b, sizeof b, "@%s#%d%+.2f", a.state.c_str(), a.n, a.secs);
  return b;
}

// Fork (m5 test rig): the `[career]` lines (SLRR_PE_CAREER_LOG=1) and the
// anchors of the entry times. Field reads only: the GameLogic statics as
// they stand (no class initializer run), instance fields by name (no slot
// made), the car's node id from its resource record; no VM call.
struct RigAnchor {
  std::string what;  // "key#3", "mouse#4", "end"
  BootAnchor a;
  bool due = false;
  double t = 0.0;
};

struct CareerRig {
  bool log = false;    // SLRR_PE_CAREER_LOG=1
  bool watch = false;  // log, or an `@` in one of the three scripts
  InvObject* state = nullptr;
  std::string lot;  // the last `lot` text printed
  std::unordered_map<std::string, std::vector<double>> entered;
  std::vector<std::string> returns;
  std::vector<RigAnchor> anchors;
};

const JvmValue* rig_static(const char* name) {
  const auto bit = g_tree_statics.find("java.game.GameLogic");
  if (bit == g_tree_statics.end()) return nullptr;
  const auto it = bit->second.by_name.find(name);
  if (it == bit->second.by_name.end()) return nullptr;
  return &bit->second.slots[it->second];
}

InvObject* rig_obj(const JvmValue* v) {
  return (v && v->tag == JvmTag::Obj) ? v->v.o : nullptr;
}

const char* rig_simple(const char* fqn) {
  if (!fqn || !fqn[0]) return "null";
  const char* d = std::strrchr(fqn, '.');
  return d ? d + 1 : fqn;
}

std::string rig_int_text(const JvmValue* v) {
  if (v && v->tag == JvmTag::Int) return std::to_string(v->v.i);
  if (v && v->tag == JvmTag::Float) return std::to_string(static_cast<int64_t>(v->v.f));
  return "?";
}

// m6 rig: the size of a java.util.Vector, a host bag (tree_vector_*) or
// an object of the script class system/scripts/util/Vector.class, whose
// count is its `elementCount` field.
std::string rig_vector_size(InvObject* v) {
  if (!v) return "null";
  if (tree_vector_is(v)) return std::to_string(tree_vector_size(v));
  const JvmValue* n = tree_field_slot(v, "elementCount", false);
  // A field never written has no slot: the script reads the int's 0.
  const char* cn = tree_host_class(v);
  if (!n && cn && std::strcmp(cn, "java.util.Vector") == 0) return "0";
  return rig_int_text(n);
}

}  // namespace

// m5 rig: jvm_vmthread_part1.inc.
void vmthread_set_enter_watch(bool on);
std::size_t vmthread_take_enter_returns(std::vector<std::string>* out);
void vmthread_set_enter_call_hook(void (*cb)(const char* cls, const char* method));
JvmValue vm_array_elem_get(InvObject* arr, int32_t idx);
// m5 step 2 diagnostic (jvm_vmthread_part2.inc; SLRR_PE_NOINIT_REPORT=1).
void jvm_noinit_report();

// Fork (m6 test rig): the boot loop's clock for log lines printed in
// other files (`[text]`, RenderNatives.cpp): seconds since the loop's t0,
// the `t=` of the `[career]` and `[pe-boot] mouse` lines; -1 before the
// loop has started.
static std::atomic<int64_t> g_pe_log_t0{0};

double pe_boot_log_time() {
  const int64_t t0 = g_pe_log_t0.load(std::memory_order_relaxed);
  if (t0 == 0) return -1.0;
  const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::chrono::duration<double>(std::chrono::steady_clock::duration(now - t0)).count();
}

namespace {

// <slots>/<curcar> of player.carlot (the slots whose carSlots flag is set,
// `-` for none), or `null`.
std::string rig_lot_text() {
  InvObject* pl = rig_obj(rig_static("player"));
  InvObject* lot = pl ? rig_obj(tree_field_slot(pl, "carlot", false)) : nullptr;
  if (!lot) return "null";
  std::string s;
  InvObject* slots = rig_obj(tree_field_slot(lot, "carSlots", false));
  const int32_t n = slots ? tree_vector_size(slots) : 0;
  bool any = false;
  for (int32_t i = 0; i < n; ++i) {
    const JvmValue v = vm_array_elem_get(slots, i);
    if ((v.tag == JvmTag::Int && v.v.i != 0) || (v.tag == JvmTag::Obj && v.v.o)) {
      if (any) s += ',';
      s += std::to_string(i);
      any = true;
    }
  }
  if (!any) s += slots ? "-" : "null";
  return s + "/" + rig_int_text(tree_field_slot(lot, "curcar", false));
}

// The `[career] state` line at the call of the state's own `enter` (the
// VM's callback, vmthread_set_enter_call_hook): the start of the scene
// build, which a dealer click's VM pump runs to its userWait before the
// loop end could poll it. The loop's poll stays for an inherited `enter`.
CareerRig* g_rig_cb = nullptr;
std::chrono::steady_clock::time_point g_rig_cb_t0;
std::string rig_fields();

void rig_enter_call(const char* cls, const char* method) {
  if (!g_rig_cb) return;
  if (std::strcmp(method, "autoSaveQuiet") == 0) {
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_rig_cb_t0).count();
    std::printf("[career] t=%.2f autosave %s\n", el, rig_fields().c_str());
    return;
  }
  InvObject* so = rig_obj(rig_static("actualState"));
  if (!so || so == g_rig_cb->state) return;
  g_rig_cb->state = so;
  const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_rig_cb_t0).count();
  std::printf("[career] t=%.2f state %s %s\n", el, rig_simple(cls), rig_fields().c_str());
}

// money=, car=<chassis class>/0x<node id>, lot=<slots>/<curcar>,
// time=<h:mm>, day=, mode= (Racer.money and .car, Player.carlot, CarLot
// .carSlots and .curcar, GameLogic.time, .day, .gameMode).
std::string rig_fields() {
  std::string s;
  char b[256];
  InvObject* pl = rig_obj(rig_static("player"));
  s += "money=";
  s += pl ? rig_int_text(tree_field_slot(pl, "money", false)) : "null";
  InvObject* car = pl ? rig_obj(tree_field_slot(pl, "car", false)) : nullptr;
  if (car) {
    InvObject* ch = rig_obj(tree_field_slot(car, "chassis", false));
    const char* cc = ch ? tree_host_class(ch) : nullptr;
    int32_t id = 0;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      const auto it = g_res.find(car);
      if (it != g_res.end()) id = it->second.id;
    }
    std::snprintf(b, sizeof b, " car=%s/0x%08X", cc && cc[0] ? cc : "null",
                  static_cast<uint32_t>(id));
    s += b;
  } else {
    s += " car=null";
  }
  s += " lot=" + rig_lot_text();
  const JvmValue* tv = rig_static("time");
  if (tv && (tv->tag == JvmTag::Float || tv->tag == JvmTag::Int)) {
    const int64_t ts = static_cast<int64_t>(tv->tag == JvmTag::Float ? tv->v.f : tv->v.i);
    std::snprintf(b, sizeof b, " time=%d:%02d", static_cast<int>(ts / 3600),
                  static_cast<int>(ts / 60 % 60));
    s += b;
  } else {
    s += " time=?";
  }
  s += " day=" + rig_int_text(rig_static("day"));
  s += " mode=" + rig_int_text(rig_static("gameMode"));
  // m6 rig: bin=<items>/<cline> of GameLogic.garage.mechanic.inventory,
  // parts=<items> of GameLogic.player.parts, gmode=<Garage.mode>, and
  // secs=<GameLogic.time> in seconds (time= is h:mm). `null` where an
  // object of the chain is not there.
  InvObject* ga = rig_obj(rig_static("garage"));
  InvObject* mech = ga ? rig_obj(tree_field_slot(ga, "mechanic", false)) : nullptr;
  InvObject* binv = mech ? rig_obj(tree_field_slot(mech, "inventory", false)) : nullptr;
  InvObject* bits = binv ? rig_obj(tree_field_slot(binv, "items", false)) : nullptr;
  s += " bin=";
  s += bits ? rig_vector_size(bits) + "/" +
                  rig_int_text(tree_field_slot(binv, "cline", false))
            : std::string("null");
  InvObject* pinv = pl ? rig_obj(tree_field_slot(pl, "parts", false)) : nullptr;
  InvObject* pits = pinv ? rig_obj(tree_field_slot(pinv, "items", false)) : nullptr;
  s += " parts=";
  s += pits ? rig_vector_size(pits) : std::string("null");
  s += " gmode=";
  s += ga ? rig_int_text(tree_field_slot(ga, "mode", false)) : std::string("null");
  if (tv && (tv->tag == JvmTag::Float || tv->tag == JvmTag::Int)) {
    std::snprintf(b, sizeof b, " secs=%.2f",
                  tv->tag == JvmTag::Float ? static_cast<double>(tv->v.f)
                                           : static_cast<double>(tv->v.i));
    s += b;
  } else {
    s += " secs=?";
  }
  return s;
}

}  // namespace

int game_boot_run(Jvm& jvm, const char* game_root, const char* player_name,
                  bool wait_enter) {
  const char* name =
      (player_name && player_name[0]) ? player_name : "Player";

  std::printf("=== SLRR rewrite boot (script-driven CAS) ===\n");
  std::printf("root=%s player='%s'\n", game_root, name);
  std::printf(
      "note: SplashScreen.enter TREE-first; other GameState host-shim\n");

  if (!game_boot_warmup(jvm, game_root)) return 3;

  // Soft Engine_boot@0x58C700 without FMV: InitState Config + MainLoop GameInit
  // back-to-back (PE separates them around Engine_PlayBootFMV@0x55C470).
  (void)game_boot_mainloop_gameinit_prologue();

  InvObject* ls_boot = frontend_loading_screen();
  if (Jvm* j = jvm_active()) {
    std::vector<JvmValue> a = {JvmValue::make_obj(ls_boot)};
    j->invoke("java.render.LoadingScreen", "show", "()V", a, false);
  } else {
    frontend_loading_screen_show();
  }

  InvObject* splash = game_logic_boot_splash();
  std::printf("  state=%s\n",
              splash && tree_host_class(splash) ? tree_host_class(splash)
                                               : "?");

  InvObject* menu = game_logic_finish_splash();
  std::printf("  state=%s\n",
              menu && tree_host_class(menu) ? tree_host_class(menu) : "?");

  InvObject* garage = main_menu_cmd_new(name);
  InvObject* player = game_logic_player();
  const char* pname =
      player ? string_cstr(tree_field_get_obj(player, "name")) : "?";
  std::printf(
      "  state=%s name='%s' money=%d career=%d map=0x%X loading=%d\n",
      garage && tree_host_class(garage) ? tree_host_class(garage) : "?",
      pname ? pname : "?", player ? tree_field_get_int(player, "money") : -1,
      game_logic_career_in_progress(),
      garage ? tree_field_get_int(garage, "map_id") : 0,
      frontend_loading_screen_visible());

  if (!garage || game_logic_actual_state() != garage) {
    std::printf("FAIL boot did not reach Garage\n");
    return 5;
  }

  InvObject* car = player_spawn_starter_car();
  const char* cname =
      car ? string_cstr(tree_field_get_obj(car, "vehicleName")) : "?";
  std::printf("  car='%s' id=0x%X locked=%d\n", cname ? cname : "?",
              car ? java_util_resource_ResourceRef_id(car) : 0,
              car ? tree_field_get_int(car, "stopped") : 0);
  if (!car || !java_util_resource_ResourceRef_id(car) ||
      vehicle_is_driveable(car)) {
    std::printf("FAIL starter car\n");
    return 5;
  }

  constexpr int32_t CMD_MECHANIC = 117;
  constexpr int32_t CMD_HITTHESTREET = 109;
  garage_osd_command(garage, CMD_MECHANIC);
  std::printf("  CMD_MECHANIC mode=%d\n", tree_field_get_int(garage, "mode"));

  constexpr int32_t CMD_TESTTRACK = 110;
  constexpr int32_t CMD_CARLOT = 111;
  constexpr int32_t CMD_CATALOG = 113;
  InvObject* cat = garage_osd_command(garage, CMD_CATALOG);
  std::printf("  CMD_CATALOG -> %s\n",
              cat && tree_host_class(cat) ? tree_host_class(cat) : "FAIL");
  if (!cat || !std::strstr(tree_host_class(cat), "Catalog") ||
      game_logic_actual_state() != cat) {
    std::printf("FAIL Catalog\n");
    return 5;
  }
  InvObject* back_cat = game_state_return_to_garage(cat);
  if (!back_cat || game_logic_actual_state() != back_cat) {
    std::printf("FAIL Catalog→Garage\n");
    return 5;
  }

  InvObject* lot = garage_osd_command(garage, CMD_CARLOT);
  std::printf("  CMD_CARLOT -> %s map=0x%X\n",
              lot && tree_host_class(lot) ? tree_host_class(lot) : "FAIL",
              lot ? tree_field_get_int(lot, "map_id") : 0);
  if (!lot || !std::strstr(tree_host_class(lot), "CarLot") ||
      tree_field_get_int(lot, "map_id") == 0) {
    std::printf("FAIL CarLot\n");
    return 5;
  }
  if (!game_state_return_to_garage(lot) ||
      game_logic_actual_state() != garage) {
    std::printf("FAIL CarLot→Garage\n");
    return 5;
  }

  InvObject* track = garage_osd_command(garage, CMD_TESTTRACK);
  InvObject* tnav = track ? tree_field_get_obj(track, "nav") : nullptr;
  std::printf("  CMD_TESTTRACK -> %s map=0x%X tiles=%d dyn=%d\n",
              track && tree_host_class(track) ? tree_host_class(track) : "FAIL",
              track ? tree_field_get_int(track, "map_id") : 0,
              tnav ? tree_field_get_int(tnav, "tiles_count") : 0,
              tnav ? tree_field_get_int(tnav, "dynamarker_count") : 0);
  if (!track || !std::strstr(tree_host_class(track), "TestTrack") ||
      tree_field_get_int(track, "map_id") == 0 || !tnav ||
      tree_field_get_int(tnav, "tiles_count") != 48 ||
      tree_field_get_int(tnav, "dynamarker_count") < 1 ||
      tree_field_get_int(tnav, "update_count") < 1) {
    std::printf("FAIL TestTrack/nav\n");
    return 5;
  }
  if (!game_state_return_to_garage(track) ||
      game_logic_actual_state() != garage ||
      tree_field_get_int(car, "stopped") != 1) {
    std::printf("FAIL TestTrack→Garage\n");
    return 5;
  }

  constexpr int32_t CMD_BUYCARS = 112;
  constexpr int32_t CMD_CLUBINFO = 114;
  constexpr int32_t CMD_CARINFO = 115;
  constexpr int32_t CMD_BUYCARSUSED = 122;
  InvObject* market = garage_osd_command(garage, CMD_BUYCARS);
  std::printf("  CMD_BUYCARS -> %s used=%d cars=%d map=0x%X\n",
              market && tree_host_class(market) ? tree_host_class(market) : "FAIL",
              market ? tree_field_get_int(market, "used") : -1,
              market ? tree_field_get_int(market, "cars_for_sale") : 0,
              market ? tree_field_get_int(market, "map_id") : 0);
  if (!market || !std::strstr(tree_host_class(market), "CarMarket") ||
      tree_field_get_int(market, "used") != 0 ||
      tree_field_get_int(market, "cars_for_sale") != 4 ||
      tree_field_get_int(market, "map_id") == 0) {
    std::printf("FAIL CarMarket new\n");
    return 5;
  }
  game_state_return_to_garage(market);

  InvObject* usedm = garage_osd_command(garage, CMD_BUYCARSUSED);
  if (!usedm || tree_field_get_int(usedm, "used") != 1 ||
      tree_field_get_int(usedm, "cars_for_sale") != 4) {
    std::printf("FAIL CarMarket used\n");
    return 5;
  }
  game_state_return_to_garage(usedm);

  InvObject* club = garage_osd_command(garage, CMD_CLUBINFO);
  std::printf("  CMD_CLUBINFO club=%d ranking=%d\n",
              club ? tree_field_get_int(club, "club") : -1,
              club ? tree_field_get_int(club, "ranking") : -1);
  if (!club || !std::strstr(tree_host_class(club), "ClubInfo") ||
      tree_field_get_int(club, "club") != 0) {
    std::printf("FAIL ClubInfo\n");
    return 5;
  }
  game_state_return_to_garage(club);

  InvObject* cinfo = garage_osd_command(garage, CMD_CARINFO);
  const char* ciname =
      cinfo && tree_field_get_obj(cinfo, "car_name")
          ? string_cstr(tree_field_get_obj(cinfo, "car_name"))
          : nullptr;
  std::printf("  CMD_CARINFO name='%s' id=0x%X\n", ciname ? ciname : "?",
              cinfo ? tree_field_get_int(cinfo, "car_id") : 0);
  if (!cinfo || !std::strstr(tree_host_class(cinfo), "CarInfo") ||
      !ciname || !ciname[0] || tree_field_get_int(cinfo, "car_id") == 0) {
    std::printf("FAIL CarInfo\n");
    return 5;
  }
  game_state_return_to_garage(cinfo);

  InvObject* city = garage_osd_command(garage, CMD_HITTHESTREET);
  const int32_t traffic = city ? tree_field_get_int(city, "traffic_count") : 0;
  const int32_t peds = city ? tree_field_get_int(city, "pedestrian_types") : 0;
  const int32_t trigs = city ? tree_field_get_int(city, "trigger_count") : 0;
  InvObject* cnav = city ? tree_field_get_obj(city, "nav") : nullptr;
  const int32_t ntiles = cnav ? tree_field_get_int(cnav, "tiles_count") : 0;
  const int32_t nmark = cnav ? tree_field_get_int(cnav, "marker_count") : 0;
  const int32_t ndyn = cnav ? tree_field_get_int(cnav, "dynamarker_count") : 0;
  const int32_t nupd = cnav ? tree_field_get_int(cnav, "update_count") : 0;
  std::printf(
      "  CMD_HITTHESTREET -> %s map=0x%X daytime=%d traffic=%d peds=%d "
      "trigs=%d nav tiles=%d mark=%d dyn=%d upd=%d\n",
      city && tree_host_class(city) ? tree_host_class(city) : "FAIL",
      city ? tree_field_get_int(city, "map_id") : 0,
      city ? tree_field_get_int(city, "daytime") : -1, traffic, peds, trigs,
      ntiles, nmark, ndyn, nupd);
  if (!city || !std::strstr(tree_host_class(city), "Valocity") ||
      game_logic_actual_state() != city ||
      tree_field_get_int(city, "map_id") == 0 || traffic != 847 || peds != 6 ||
      trigs != 3 || ntiles != 64 || nmark != 3 || ndyn < 1 || nupd < 1) {
    std::printf("FAIL hit the street / Valocity traffic/nav\n");
    return 5;
  }
  std::printf("  addTrafficP ok=%d\n", tree_field_get_int(city, "traffic_p_smoke"));
  if (tree_field_get_int(city, "traffic_p_smoke") != 1) {
    std::printf("FAIL addTrafficP near-cross\n");
    return 5;
  }
  std::printf("  setTrafficCarBehaviour ok=%d\n",
              tree_field_get_int(city, "traffic_bh_ok"));
  if (tree_field_get_int(city, "traffic_bh_ok") != 1) {
    std::printf("FAIL setTrafficCarBehaviour\n");
    return 5;
  }
  std::printf("  haltTrafficCross ok=%d\n",
              tree_field_get_int(city, "halt_smoke"));
  if (tree_field_get_int(city, "halt_smoke") != 1) {
    std::printf("FAIL haltTrafficCross\n");
    return 5;
  }
  std::printf("  haltTrafficPath ok=%d\n",
              tree_field_get_int(city, "halt_path_smoke"));
  if (tree_field_get_int(city, "halt_path_smoke") != 1) {
    std::printf("FAIL haltTrafficPath\n");
    return 5;
  }
  std::printf("  remTrafficCar ok=%d\n",
              tree_field_get_int(city, "rem_car_smoke"));
  if (tree_field_get_int(city, "rem_car_smoke") != 1) {
    std::printf("FAIL remTrafficCar\n");
    return 5;
  }
  std::printf("  delTraffic ok=%d\n",
              tree_field_get_int(city, "del_traffic_smoke"));
  if (tree_field_get_int(city, "del_traffic_smoke") != 1) {
    std::printf("FAIL delTraffic\n");
    return 5;
  }
  std::printf("  setPedestrianDensityN ok=%d\n",
              tree_field_get_int(city, "ped_dens_smoke"));
  if (tree_field_get_int(city, "ped_dens_smoke") != 1) {
    std::printf("FAIL setPedestrianDensityN\n");
    return 5;
  }
  std::printf("  addPedestrianType ok=%d\n",
              tree_field_get_int(city, "ped_type_smoke"));
  if (tree_field_get_int(city, "ped_type_smoke") != 1) {
    std::printf("FAIL addPedestrianType\n");
    return 5;
  }
  std::printf("  remPedestrianType ok=%d\n",
              tree_field_get_int(city, "ped_rem_smoke"));
  if (tree_field_get_int(city, "ped_rem_smoke") != 1) {
    std::printf("FAIL remPedestrianType\n");
    return 5;
  }
  std::printf("  pedestrianDistance ok=%d\n",
              tree_field_get_int(city, "ped_dist_smoke"));
  if (tree_field_get_int(city, "ped_dist_smoke") != 1) {
    std::printf("FAIL pedestrianDistance\n");
    return 5;
  }
  std::printf("  setFog ok=%d\n", tree_field_get_int(city, "fog_smoke"));
  if (tree_field_get_int(city, "fog_smoke") != 1) {
    std::printf("FAIL setFog\n");
    return 5;
  }
  std::printf("  setLight ok=%d\n", tree_field_get_int(city, "light_smoke"));
  if (tree_field_get_int(city, "light_smoke") != 1) {
    std::printf("FAIL setLight\n");
    return 5;
  }
  std::printf("  setFlare ok=%d\n", tree_field_get_int(city, "flare_smoke"));
  if (tree_field_get_int(city, "flare_smoke") != 1) {
    std::printf("FAIL setFlare\n");
    return 5;
  }
  std::printf("  duplicate ok=%d\n", tree_field_get_int(city, "dup_smoke"));
  if (tree_field_get_int(city, "dup_smoke") != 1) {
    std::printf("FAIL duplicate\n");
    return 5;
  }
  std::printf("  create ok=%d\n", tree_field_get_int(city, "create_smoke"));
  if (tree_field_get_int(city, "create_smoke") != 1) {
    std::printf("FAIL create\n");
    return 5;
  }
  std::printf("  changeResource ok=%d\n", tree_field_get_int(city, "chg_smoke"));
  if (tree_field_get_int(city, "chg_smoke") != 1) {
    std::printf("FAIL changeResource\n");
    return 5;
  }
  std::printf("  setColor ok=%d\n", tree_field_get_int(city, "color_smoke"));
  if (tree_field_get_int(city, "color_smoke") != 1) {
    std::printf("FAIL setColor\n");
    return 5;
  }
  std::printf("  getTypeID ok=%d\n", tree_field_get_int(city, "type_smoke"));
  if (tree_field_get_int(city, "type_smoke") != 1) {
    std::printf("FAIL getTypeID\n");
    return 5;
  }
  std::printf("  scaleMesh ok=%d\n", tree_field_get_int(city, "scale_smoke"));
  if (tree_field_get_int(city, "scale_smoke") != 1) {
    std::printf("FAIL scaleMesh\n");
    return 5;
  }
  std::printf("  plotRoute ok=%d\n", tree_field_get_int(city, "plot_smoke"));
  if (tree_field_get_int(city, "plot_smoke") != 1) {
    std::printf("FAIL plotRoute\n");
    return 5;
  }
  std::printf("  getRoutePos ok=%d\n", tree_field_get_int(city, "routepos_smoke"));
  if (tree_field_get_int(city, "routepos_smoke") != 1) {
    std::printf("FAIL getRoutePos\n");
    return 5;
  }
  std::printf("  setMatrix ok=%d\n", tree_field_get_int(city, "setmatrix_smoke"));
  if (tree_field_get_int(city, "setmatrix_smoke") != 1) {
    std::printf("FAIL setMatrix\n");
    return 5;
  }
  std::printf("  setPos ok=%d\n", tree_field_get_int(city, "setpos_smoke"));
  if (tree_field_get_int(city, "setpos_smoke") != 1) {
    std::printf("FAIL setPos\n");
    return 5;
  }
  std::printf("  setState ok=%d\n", tree_field_get_int(city, "setstate_smoke"));
  if (tree_field_get_int(city, "setstate_smoke") != 1) {
    std::printf("FAIL setState\n");
    return 5;
  }
  std::printf("  getPos ok=%d\n", tree_field_get_int(city, "getpos_smoke"));
  if (tree_field_get_int(city, "getpos_smoke") != 1) {
    std::printf("FAIL getPos\n");
    return 5;
  }
  std::printf("  getVel ok=%d\n", tree_field_get_int(city, "getvel_smoke"));
  if (tree_field_get_int(city, "getvel_smoke") != 1) {
    std::printf("FAIL getVel\n");
    return 5;
  }
  std::printf("  setParent ok=%d\n", tree_field_get_int(city, "setparent_smoke"));
  if (tree_field_get_int(city, "setparent_smoke") != 1) {
    std::printf("FAIL setParent\n");
    return 5;
  }
  std::printf("  getSpeedSquare ok=%d\n",
              tree_field_get_int(city, "getspeedsq_smoke"));
  if (tree_field_get_int(city, "getspeedsq_smoke") != 1) {
    std::printf("FAIL getSpeedSquare\n");
    return 5;
  }
  std::printf("  isEmpty ok=%d\n", tree_field_get_int(city, "isempty_smoke"));
  if (tree_field_get_int(city, "isempty_smoke") != 1) {
    std::printf("FAIL isEmpty\n");
    return 5;
  }
  std::printf("  isScripted ok=%d\n",
              tree_field_get_int(city, "isscripted_smoke"));
  if (tree_field_get_int(city, "isscripted_smoke") != 1) {
    std::printf("FAIL isScripted\n");
    return 5;
  }
  std::printf("  getScriptInstance ok=%d\n",
              tree_field_get_int(city, "getscript_smoke"));
  if (tree_field_get_int(city, "getscript_smoke") != 1) {
    std::printf("FAIL getScriptInstance\n");
    return 5;
  }

  valocity_fire_garage_trigger(city, 2, 1);
  InvObject* denied = valocity_tick(city);
  if (denied || tree_field_get_int(city, "garage_denied") != 1 ||
      game_logic_actual_state() != city) {
    std::printf("FAIL higher-club garage deny\n");
    return 5;
  }

  valocity_fire_garage_trigger(city, 1, 1);
  InvObject* back = valocity_tick(city);
  std::printf("  trigger return garage entered=%d car_locked=%d traffic=%d\n",
              back ? tree_field_get_int(back, "entered") : 0,
              car ? tree_field_get_int(car, "stopped") : 0,
              city ? tree_field_get_int(city, "traffic_count") : -1);
  if (!back || game_logic_actual_state() != back ||
      !std::strstr(tree_host_class(back), "Garage") ||
      tree_field_get_int(city, "traffic_count") != 0 ||
      tree_field_get_int(car, "stopped") != 1) {
    std::printf("FAIL Valocity → Garage\n");
    return 5;
  }

  constexpr int32_t CMD_TIME = 116;
  for (int i = 0; i < 11; ++i) garage_osd_command(garage, CMD_TIME);
  std::printf("  CMD_TIME x11 time=%.0fs day=%d\n", game_logic_time(),
              game_logic_day());
  InvObject* night = garage_osd_command(garage, CMD_HITTHESTREET);
  const int32_t ntraffic =
      night ? tree_field_get_int(night, "traffic_count") : 0;
  std::printf("  night Valocity daytime=%d traffic=%d\n",
              night ? tree_field_get_int(night, "daytime") : -1, ntraffic);
  if (!night || tree_field_get_int(night, "daytime") != 0 || ntraffic != 183) {
    std::printf("FAIL night traffic\n");
    return 5;
  }
  valocity_fire_garage_trigger(night, 1, 1);
  InvObject* back2 = valocity_tick(night);
  if (!back2 || game_logic_actual_state() != back2 ||
      tree_field_get_int(night, "traffic_count") != 0) {
    std::printf("FAIL night → Garage\n");
    return 5;
  }

  InvObject* closed = garage_osd_command(garage, CMD_BUYCARS);
  const char* warn =
      tree_field_get_obj(garage, "last_warning")
          ? string_cstr(tree_field_get_obj(garage, "last_warning"))
          : nullptr;
  std::printf("  night dealer closed=%d warn='%s'\n", closed ? 0 : 1,
              warn ? warn : "");
  if (closed || !warn || !std::strstr(warn, "closed")) {
    std::printf("FAIL dealer hours\n");
    return 5;
  }

  std::printf("\n*** RUNNING (host) — market + club/car info ok ***\n");
  std::printf("    car='%s' day=%d time=%.0fs\n", cname ? cname : "?",
              game_logic_day(), game_logic_time());
  if (wait_enter) {
    std::printf("Press Enter to quit.\n");
    std::fflush(stdout);
    (void)std::getchar();
  }
  std::printf("boot exit ok\n");
  return 0;
}

// Fork: display size of the script boot path. The exe applies Config before
// it opens the display (Engine_InitState LoadGameInit "Config" ->
// ApplyConfigGfx, then GfxEngine_InitDisplay; see the order in
// game_interactive_run), so its window has the size stored in
// <game>/save/game/options: SDAT container, magic 0xFEDCBA98 at +8, version
// at +12, then u32 windowed, video_x, video_y, depth at +16 / +20 / +24 /
// +28. The scripts lay out from the same file through Config.video_x /
// video_y, so the back buffer has to match it.
// SLRR_WINDOW_SIZE=WxH overrides the size for tests (the scripts still lay
// out from their own Config). Without SLRR_PE_BOOT_INIT=1 (legacy host
// path) the size stays 1024x768.
void game_boot_display_size(const char* game_root, int32_t* w, int32_t* h) {
  *w = 1024;
  *h = 768;
  const char* pe_boot = std::getenv("SLRR_PE_BOOT_INIT");
  if (!pe_boot || pe_boot[0] != '1') return;
  auto sane = [](long x, long y) {
    return x >= 320 && x <= 7680 && y >= 240 && y <= 4320;
  };
  if (const char* e = std::getenv("SLRR_WINDOW_SIZE"); e && e[0]) {
    char* end = nullptr;
    const long x = std::strtol(e, &end, 10);
    const long y = (end && (*end == 'x' || *end == 'X')) ? std::strtol(end + 1, nullptr, 10) : 0;
    if (sane(x, y)) {
      *w = static_cast<int32_t>(x);
      *h = static_cast<int32_t>(y);
      std::printf("[pe-boot] display %dx%d (SLRR_WINDOW_SIZE)\n", *w, *h);
      return;
    }
    std::printf("[pe-boot] SLRR_WINDOW_SIZE='%s' ignored\n", e);
  }
  std::string path = (game_root && game_root[0]) ? game_root : ".";
  path += "/save/game/options";
  uint8_t hdr[32] = {};
  size_t got = 0;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    got = std::fread(hdr, 1, sizeof(hdr), f);
    std::fclose(f);
  } else {
    std::printf("[pe-boot] display %dx%d (no %s)\n", *w, *h, path.c_str());
    return;
  }
  uint32_t magic = 0, version = 0, windowed = 0, vx = 0, vy = 0, depth = 0;
  std::memcpy(&magic, hdr + 8, 4);
  std::memcpy(&version, hdr + 12, 4);
  std::memcpy(&windowed, hdr + 16, 4);
  std::memcpy(&vx, hdr + 20, 4);
  std::memcpy(&vy, hdr + 24, 4);
  std::memcpy(&depth, hdr + 28, 4);
  if (got != sizeof(hdr) || std::memcmp(hdr, "SDAT", 4) != 0 ||
      magic != 0xFEDCBA98u || !sane(static_cast<long>(vx), static_cast<long>(vy))) {
    std::printf("[pe-boot] display %dx%d (%s malformed)\n", *w, *h, path.c_str());
    return;
  }
  *w = static_cast<int32_t>(vx);
  *h = static_cast<int32_t>(vy);
  std::printf("[pe-boot] display %dx%d (save/game/options: version=%u windowed=%u depth=%u)\n",
              *w, *h, version, windowed, depth);
}

// Fork (M3 spec Q15): the three shadow options on the script boot path.
// The exe's Config constructor runs loadConfig("save/game/options") before
// the display opens, and the engine reads Config.shadows / shadow_size /
// shadow_detail from the script instance (FUN_0047c240; FUN_00421280 makes
// the shadow targets). The host does not run that constructor yet; it
// takes the three values from the same file at the places loadConfig reads
// them. The order is that of loadConfig and saveConfig in
// system/scripts/util/Config.class (tufa_dump): after the 8 bytes of the
// SDAT container SAVEFILEID 0xFEDCBA98, the version, video_windowed,
// video_x, video_y, video_depth, video_gamma (float), texture_size, then
// shadow_size (+40), shadow_detail (float, +44) and shadows (+48).
// loadConfig reads on when the first number is SAVEFILEID and the version
// is 12 or more (tree 144: `readInt() == SAVEFILEID`, `version >= 12`);
// the game writes version 13.
// Fork: a missing, older or implausible file (a size above 16384, a
// negative number) leaves the field initialisers of Config.class: shadows
// 2, shadow_size 256, shadow_detail 0.25. loadConfig has no such test of
// the values.
// SLRR_SHADOWS=<shadows>[,<size>[,<detail>]] overrides them for tests (in
// place of the OPTIONS screen). The values go to the host's Config object
// and to the renderer.
static void game_boot_shadow_options(const char* game_root) {
  int32_t shadows = 2, size = 256;
  float detail = 0.25f;
  std::string source = "class defaults";
  std::string path = (game_root && game_root[0]) ? game_root : ".";
  path += "/save/game/options";
  uint8_t hdr[52] = {};
  size_t got = 0;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    got = std::fread(hdr, 1, sizeof(hdr), f);
    std::fclose(f);
    uint32_t magic = 0, version = 0;
    int32_t f_size = 0, f_shadows = 0;
    float f_detail = 0.f;
    std::memcpy(&magic, hdr + 8, 4);
    std::memcpy(&version, hdr + 12, 4);
    std::memcpy(&f_size, hdr + 40, 4);
    std::memcpy(&f_detail, hdr + 44, 4);
    std::memcpy(&f_shadows, hdr + 48, 4);
    const bool plausible = f_size >= 0 && f_size <= 16384 && f_shadows >= 0 &&
                           f_shadows <= 16 && f_detail >= 0.f &&
                           f_detail <= 1000.f;
    if (got == sizeof(hdr) && std::memcmp(hdr, "SDAT", 4) == 0 &&
        magic == 0xFEDCBA98u && version >= 12 && plausible) {
      shadows = f_shadows;
      size = f_size;
      detail = f_detail;
      source = "save/game/options";
    } else {
      source = "class defaults: save/game/options not usable";
    }
  } else {
    source = "class defaults: no save/game/options";
  }
  if (const char* e = std::getenv("SLRR_SHADOWS"); e && e[0]) {
    char* end = nullptr;
    const long a = std::strtol(e, &end, 10);
    if (end != e) {
      shadows = static_cast<int32_t>(a);
      if (*end == ',') {
        const char* s2 = end + 1;
        const long b = std::strtol(s2, &end, 10);
        if (end != s2) {
          size = static_cast<int32_t>(b);
          if (*end == ',') {
            const char* s3 = end + 1;
            const float c = std::strtof(s3, &end);
            if (end != s3) detail = c;
          }
        }
      }
      source = "SLRR_SHADOWS";
    }
  }
  if (InvObject* cfg = system_config_host()) {
    tree_field_set_int(cfg, "shadow_size", size);
    tree_field_set_float(cfg, "shadow_detail", detail);
    tree_field_set_int(cfg, "shadows", shadows);
  }
  render_d3d9_set_shadow_options(shadows, size, detail, source.c_str());
  std::printf("[pe-boot] shadows=%d shadow_size=%d shadow_detail=%g (%s)\n",
              shadows, size, detail, source.c_str());
}

int game_interactive_run(Jvm& jvm, const char* game_root,
                         const char* player_name, bool auto_new,
                         int32_t max_frames) {
  const char* name =
      (player_name && player_name[0]) ? player_name : "Player";

  std::printf("=== SLRR rewrite --game (FMV→Splash→MainMenu) ===\n");
  std::printf("root=%s player='%s' auto_new=%d max_frames=%d\n", game_root,
              name, auto_new ? 1 : 0, max_frames);

  if (!game_boot_warmup(jvm, game_root)) return 3;

  if (!render_d3d9_ready()) {
    int32_t dw = 1024, dh = 768;
    game_boot_display_size(game_root, &dw, &dh);
    if (!render_d3d9_open(dw, dh, "SLRR — MainMenu")) {
      std::printf("FAIL --game render open\n");
      return 6;
    }
  }
  render_d3d9_clear_quit();

  // Soft PE Engine_boot @ 0x0058C700 size 0x2D2 (722). Xref: WinMain@0x5514A8.
  // PE order (IDA):
  //   DiagLog → CheckDiskCached@0x401000 → ResourceEngine_Init@0x535E70 →
  //   LoadPack("system.rpk")@0x538380 (fail AX==0xFFFF → Shutdown+Fatal) →
  //   Engine_BootPostLoadPackStub@0x497FD0 (return 1) →
  //   Engine_InitState@0x427980 (JVM + Natives_Register* + LoadGameInit
  //     "Config" + ApplyConfigGfx; BEFORE FMV) →
  //   Config acceptBuild/version/FILD gates →
  //   GfxEngine_InitDisplay@0x4FC1B0 + AllocPools@0x4FBCE0 →
  //   Input_ResetGlobals@0x54DD50 → Sound_InitDirectSound@0x5598C0 + HW caps →
  //   FMV triad File_PathExists@0x54C170 → Engine_PlayBootFMV@0x55C470
  //     (NOT GfxEngine.openVideo@0x47C330): Activision → Invictus →
  //     StreetLegal.avi (SL1 leftover; PathExists skip, no error) →
  //   Engine_MainLoop@0x428960 (LoadGameInit "GameInit"@0x53A5E0 + while) →
  //   TextureLog_FlushWrite@0x53C940 AFTER loop → teardown
  //     (shutdownMainHandles → DestroyOwnedAt11C → clearSimCallbacks →
  //      Sound/Input/Gfx shutdown → Engine_BootPreShutdownStub@0x497FE0 →
  //      ResourceEngine_Shutdown).
  // Soft map: warmup≈RE_Init+LoadPack+JVM/natives; render_d3d9_open≈
  // InitDisplay; initstate_config≈InitState Config; video_fmv_play_boot_intros
  // ≈PlayBootFMV triad; mainloop_gameinit_load≈MainLoop GameInit; harness≈
  // MainLoop while (splash/menu/world). FAIL fmv_n<2 is host smoke for shipped
  // Activision+Invictus, not a PE PathExists error.
  std::printf(
      "  Soft Engine_boot@0x58C700 gate: warmup packs+JVM → Gfx open → "
      "InitState Config → FMV triad → MainLoop GameInit+harness\n");

  // Soft ≈ InitState LoadGameInit("Config") — PE places this before FMV.
  GameInitPrologue gi_boot = game_boot_initstate_config_prologue();

  const int32_t fmv_cap = (auto_new || max_frames > 0) ? 8 : 0;
  const int32_t fmv_n = video_fmv_play_boot_intros(fmv_cap);
  std::printf(
      "  Soft Engine_PlayBootFMV@0x55C470 clips=%d (Activision+Invictus; "
      "StreetLegal PathExists-skip)\n",
      fmv_n);
  // Fork: Build 940 ships no Activision/Invictus intros (only the menu
  // background_motion.avi). PE PathExists-skips missing clips, so do we.
  if (fmv_n < 2) {
    std::printf("WARN --game boot FMV intros missing (%d/2) - continuing\n",
                fmv_n);
  }

  // Soft ≈ MainLoop prologue (stock post-FMV, pre-while): GameInit under wtroot.
  game_boot_mainloop_gameinit_load(gi_boot);
  if (!gi_boot.gameinit_gi) {
    std::printf(
        "WARN PATH-TO-WORLD: GameInit LoadGameInit returned null "
        "(wt=0x%X) — continuing Splash harness\n",
        gi_boot.wt_key);
  }

  // Stock GameLogic ctor end: Frontend.loadingScreen.hide() — do not show() a
  // SimpleLoadingDialog (SoftTimer/FlashText path crashes without full Dialog).
  frontend_loading_screen_hide();
  if (Jvm* j = jvm_active()) {
    InvObject* ls = frontend_loading_screen();
    std::vector<JvmValue> a = {JvmValue::make_obj(ls)};
    j->invoke("java.render.LoadingScreen", "hide", "()V", a, false);
  }

  // Fork: SLRR_PE_BOOT_INIT=1 — boot through the script's own Init(int)
  // on the VM (PE Engine_MainLoop → LoadGameInit "GameInit" → new Init(id))
  // instead of the Soft splash/menu shims below. Reports the GameLogic
  // state the scripts reach and pumps frames until max_frames / quit.
  if (const char* pe_boot = std::getenv("SLRR_PE_BOOT_INIT");
      pe_boot && pe_boot[0] == '1') {
    std::printf("[pe-boot] java.game.Init.<init>(I) on the VM\n");
    // Fork: Config.video_x / video_y. The exe's Engine_InitState runs
    // LoadGameInit("Config") = new Config(id), whose constructor calls
    // loadConfig("save/game/options") before the display opens
    // (system/scripts/util/Config.class <init>(I)). The host does not run
    // that constructor yet, so the statics keep their 800x600 initialisers
    // while the Osd / Text scripts lay out from them; seed the two from the
    // back buffer (game_boot_display_size read the same file).
    if (!jvm.find_class("java.util.Config")) jvm.load_class("java.util.Config");
    if (const JvmClass* cfg = jvm.find_class("java.util.Config")) {
      if (render_d3d9_width() > 0 && render_d3d9_height() > 0) {
        if (JvmValue* s = tree_static_slot(cfg, "video_x", false))
          *s = JvmValue::make_int(render_d3d9_width());
        if (JvmValue* s = tree_static_slot(cfg, "video_y", false))
          *s = JvmValue::make_int(render_d3d9_height());
      }
    }
    game_boot_shadow_options(game_root);
    // Fork (m5 test rig): the `[career]` log and the anchors (see CareerRig).
    CareerRig rig;
    {
      const char* cl = std::getenv("SLRR_PE_CAREER_LOG");
      rig.log = cl && cl[0] == '1';
      bool at = false;
      for (const char* v : {"SLRR_PE_BOOT_KEYS", "SLRR_PE_BOOT_MOUSE", "SLRR_PE_BOOT_END",
                            "SLRR_PE_BOOT_SHOTS"}) {
        const char* s = std::getenv(v);
        if (s && std::strchr(s, '@')) at = true;
      }
      rig.watch = rig.log || at;
      if (rig.watch) vmthread_set_enter_watch(true);
    }
    // Fork (m5 test rig): SLRR_PE_BOOT_END=<time> (an entry time, usually
    // `@<State>[#n]+secs`) ends the loop then; SLRR_PE_BOOT_SECONDS stays
    // the hard limit.
    int32_t end_anchor = -1;
    bool end_set = false;
    double end_t = 0.0;
    if (const char* be = std::getenv("SLRR_PE_BOOT_END"); be && be[0]) {
      BootAnchor a;
      const char* q = boot_entry_time(be, nullptr, &a, &end_t);
      if (!q || *q) {
        std::printf("[pe-boot] end not read: '%s': SLRR_PE_BOOT_SECONDS ends the loop\n", be);
      } else {
        end_set = true;
        if (!a.state.empty()) {
          end_anchor = static_cast<int32_t>(rig.anchors.size());
          rig.anchors.push_back({"end", a, false, 0.0});
        }
      }
    }
    if (!jvm.find_class("java.game.Init")) jvm.load_class("java.game.Init");
    InvObject* init_obj = tree_host_new("java.game.Init");
    jvm.invoke("java.game.Init", "<init>", "(I)V",
               {JvmValue::make_obj(init_obj), JvmValue::make_int(0)}, false);
    const auto boot_t0 = std::chrono::steady_clock::now();
    g_pe_log_t0.store(boot_t0.time_since_epoch().count(), std::memory_order_relaxed);  // m6 rig
    if (rig.log) {  // Fork (m5 test rig): rig_enter_call
      g_rig_cb = &rig;
      g_rig_cb_t0 = boot_t0;
      vmthread_set_enter_call_hook(&rig_enter_call);
    }
    auto report = [&](const char* when, int32_t nframes) {
      const double el_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - boot_t0).count();
      std::printf("[pe-boot] elapsed %.0f ms\n", el_ms);
      const JvmClass* gl = jvm.find_class("java.game.GameLogic");
      JvmValue* st = gl ? tree_static_slot(gl, "actualState", false) : nullptr;
      InvObject* so = (st && st->tag == JvmTag::Obj) ? st->v.o : nullptr;
      std::printf("[pe-boot] %s (frames=%d): GameLogic.actualState='%s' host_state='%s' osd=%d txt=%d\n",
                  when, nframes,
                  so && tree_host_class(so) ? tree_host_class(so) : "null",
                  game_logic_actual_state() && tree_host_class(game_logic_actual_state())
                      ? tree_host_class(game_logic_actual_state()) : "null",
                  render_d3d9_osd_count(), render_d3d9_osd_text_count());
      // Fork: the scripts lay out from Config.video_x / video_y; they have
      // to agree with the back buffer (game_boot_display_size).
      const JvmClass* cfg = jvm.find_class("java.util.Config");
      JvmValue* cvx = cfg ? tree_static_slot(cfg, "video_x", false) : nullptr;
      JvmValue* cvy = cfg ? tree_static_slot(cfg, "video_y", false) : nullptr;
      std::printf("[pe-boot] display %dx%d Config.video=%dx%d\n",
                  render_d3d9_width(), render_d3d9_height(),
                  (cvx && cvx->tag == JvmTag::Int) ? cvx->v.i : -1,
                  (cvy && cvy->tag == JvmTag::Int) ? cvy->v.i : -1);
    };
    report("Init done", 0);
    input_live_enable(true);
    int32_t pe_frames = 0;
    double pe_t_render = 0.0, pe_t_vm = 0.0;
    int32_t pe_max_frames = max_frames;
    if (const char* fenv = std::getenv("SLRR_PE_BOOT_FRAMES"))
      pe_max_frames = static_cast<int32_t>(std::strtol(fenv, nullptr, 10));
    // PE 940 FUN_0047d760: the game tick's clock, whole milliseconds of the
    // loop's own counter (`local_3c = (int)FUN_00569730()` before the loop;
    // here: since boot_t0). The rest is for `[pe-boot] ticks` at loop end.
    struct TickClock {
      int64_t last_ms = 0;
      int32_t n = 0;
      double sum_ms = 0.0;
      int32_t min_ms = 0, max_ms = 0, over100 = 0;
      int32_t hist[258] = {};  // ticks per whole millisecond, 257 = longer
      int32_t due = 0;         // ticks accounted at their due time
      double late_ms = 0.0;    // how long after it those ran, summed
    } pe_tick;
    while (!render_d3d9_quit_requested()) {
      // PE Engine_MainLoop presents every iteration; the Present path wakes
      // Frontend.render.wait() (LoadingScreen pacing).
      const auto t_a = std::chrono::steady_clock::now();
      // Fork: per-section wall time for the heartbeat (ms since last beat).
      static double sec_ms[6] = {};
      auto lap = [](const std::chrono::steady_clock::time_point& a) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
      };
      auto s0 = std::chrono::steady_clock::now(); render_d3d9_flush(); sec_ms[0] += lap(s0);
      s0 = std::chrono::steady_clock::now(); render_d3d9_pump(0); sec_ms[1] += lap(s0);
      s0 = std::chrono::steady_clock::now(); input_live_poll(); sec_ms[2] += lap(s0);
      // PE 940 FUN_0047d760, after the message pump FUN_0056a1c0:
      //   if (0xf < now - last) {            // whole ms of FUN_00569730 (QPC)
      //     dt = (now - last) * 0.001;
      //     FUN_00566160(dt);                // input tick: ONE device poll, the axes
      //     FUN_0047e050();                  // game tick: control list, physics steps
      //     FUN_0047b700();                  // timers
      //     last = now;
      //   }
      // so the two run at most 62.5 times a second with the same dt, and a
      // mouse delta or a wheel notch (relative DirectInput data, returned
      // once) lives for exactly one game tick: the one whose control(dt)
      // reads it. input_live_poll above polls no device on the script boot.
      // The game tick caps its dt at 0.1 s itself (FUN_0047e050); the input
      // tick gets the raw value. The timers run inside the game tick's steps
      // on the host (engine_tick_timers); the exe's extra call after the
      // tick finds the same world clock.
      // Fork: the VM pump (the exe: when 7 ms have passed) and the present
      // (fps_lock) still run in every pass.
      // Fork: the host draws and presents the frame in every pass, 2 to 3
      // ms; the exe draws only behind its `fps_lock` gate (FUN_0047b5b0,
      // Config default 1 ms), so where a frame draws in well under a
      // millisecond it looks at this gate about every millisecond and its
      // tick is 16 ms long [I: the stock frame time was not measured]. With
      // the exe's arithmetic alone the host finds each tick up to a pass
      // late and hands it 16 to 19 ms (mean 16.4): the garage lift's arms
      // then come to rest in another pose than in the stock frame (m2_check
      // B1 0.74 against 0.30, lift box 1.42 against 0.38; the camera itself
      // moves by 2 mm). So a tick that is found less than one tick length
      // after it was due is accounted at its due time: dt = 16 ms and last
      // += 16, which is what a loop with short passes does. A tick found
      // later than that (a loading pass, long in the exe too) keeps the
      // exe's arithmetic: dt = the whole difference, last = now.
      {
        const auto now = std::chrono::steady_clock::now();
        const double now_s = std::chrono::duration<double>(now - boot_t0).count();
        const int64_t now_ms = static_cast<int64_t>(now_s * 1000.0);
        int64_t d_ms = now_ms - pe_tick.last_ms;
        if (d_ms > 15) {
          if (d_ms < 32) {
            ++pe_tick.due;
            pe_tick.late_ms += static_cast<double>(d_ms - 16);
            pe_tick.last_ms += 16;
            d_ms = 16;
          } else {
            pe_tick.last_ms = now_ms;
          }
          const float dt = static_cast<float>(static_cast<double>(d_ms) * 0.001);
          // Fork: the loop clock of the `[camera] ctl` lines (the clock of
          // the key and mouse scripts).
          native_set_loop_time(now_s);
          s0 = std::chrono::steady_clock::now(); input_tick(dt); sec_ms[2] += lap(s0);
          s0 = std::chrono::steady_clock::now(); engine_simulate_frame_dt(dt); sec_ms[3] += lap(s0);
          const int32_t ms = d_ms > 0x7fffffff ? 0x7fffffff : static_cast<int32_t>(d_ms);
          if (pe_tick.n == 0 || ms < pe_tick.min_ms) pe_tick.min_ms = ms;
          if (pe_tick.n == 0 || ms > pe_tick.max_ms) pe_tick.max_ms = ms;
          ++pe_tick.n;
          pe_tick.sum_ms += static_cast<double>(ms);
          if (ms > 100) ++pe_tick.over100;
          ++pe_tick.hist[ms > 257 ? 257 : ms];
        }
      }
      s0 = std::chrono::steady_clock::now(); game_boot_mainloop_resource_pumps(); sec_ms[4] += lap(s0);
      const auto t_b = std::chrono::steady_clock::now();
      jvm_run_threads_budgeted(10.f);  // PE MainLoop: Jvm_PumpFrame(10.0)
      // PE 940 FUN_0047ba10(min(DAT_005f0c88, 0.1)), the animate walk of
      // the loop pass FUN_0047d760, with the time since the pass before:
      // the native objects on the animate list first (the cursor, made at
      // boot before any Osd), then the script animate() calls.
      {
        static auto pass_prev = t_a;
        float pass_dt = std::chrono::duration<float>(t_a - pass_prev).count();
        pass_prev = t_a;
        if (pass_dt > 0.1f) pass_dt = 0.1f;
        native_tick_animate(pass_dt);
      }
      engine_dispatch_animate_events();  // PE Engine_DispatchAnimateEvents
      const auto t_c = std::chrono::steady_clock::now();
      sec_ms[5] += std::chrono::duration<double, std::milli>(t_c - t_b).count();
      pe_t_render += std::chrono::duration<double, std::milli>(t_b - t_a).count();
      pe_t_vm += std::chrono::duration<double, std::milli>(t_c - t_b).count();
      ++pe_frames;
      // Fork (m5 test rig): the state as the loop end reads it, the returns
      // of its `enter` the VM noted, the anchors they reach.
      if (rig.watch) {
        const double el_r = std::chrono::duration<double>(std::chrono::steady_clock::now() - boot_t0).count();
        InvObject* so = rig_obj(rig_static("actualState"));
        if (so != rig.state) {
          rig.state = so;
          if (rig.log)
            std::printf("[career] t=%.2f state %s %s\n", el_r,
                        rig_simple(so ? tree_host_class(so) : nullptr), rig_fields().c_str());
        }
        if (rig.log) {
          std::string lot = rig_lot_text();
          if (lot != rig.lot) {
            std::printf("[career] t=%.2f lot %s\n", el_r, lot.c_str());
            rig.lot.swap(lot);
          }
        }
        rig.returns.clear();
        if (vmthread_take_enter_returns(&rig.returns) > 0) {
          for (const std::string& cn : rig.returns) {
            const char* sn = rig_simple(cn.c_str());
            std::vector<double>& v = rig.entered[sn];
            v.push_back(el_r);
            if (rig.log)
              std::printf("[career] t=%.2f entered %s #%zu %s\n", el_r, sn, v.size(),
                          rig_fields().c_str());
          }
        }
        for (RigAnchor& r : rig.anchors) {
          if (r.due) continue;
          const auto it = rig.entered.find(r.a.state);
          if (it == rig.entered.end() || static_cast<int32_t>(it->second.size()) < r.a.n) continue;
          r.due = true;
          r.t = it->second[static_cast<size_t>(r.a.n - 1)] + r.a.secs;
          std::printf("[pe-boot] anchor %s %s -> t=%.2f\n", r.what.c_str(),
                      boot_anchor_text(r.a).c_str(), r.t);
        }
      }
      // Fork: SLRR_PE_BOOT_KEYS="t:dik[:secs][,t:dik[:secs]...]" presses
      // DirectInput key `dik` at `t` seconds after loop start and holds it
      // for `secs` (150 ms without the third field) (unattended runs).
      {
        // Fork (m5 test rig): an entry time may be anchored
        // (boot_entry_time); an entry never fires before the one before
        // it (a script of absolute times in time order: as before).
        struct KeyAt { double t; int32_t dik; double hold; int state; double down_at; BootAnchor a; int32_t anchor; };  // state 0 pending, 1 down, 2 done
        static std::vector<KeyAt> keys = [&] {
          std::vector<KeyAt> v;
          const char* e = std::getenv("SLRR_PE_BOOT_KEYS");
          while (e && *e) {
            char* end = nullptr;
            BootAnchor a;
            double t = 0.0;
            const char* te = boot_entry_time(e, v.empty() ? nullptr : &v.back().a, &a, &t);
            if (!te || *te != ':') break;
            const long dik = std::strtol(te + 1, &end, 0);
            double hold = 0.15;
            if (*end == ':') {
              const char* h0 = end + 1;
              const double secs = std::strtod(h0, &end);
              if (end != h0 && secs > 0.0) hold = secs;
            }
            v.push_back({t, static_cast<int32_t>(dik), hold, 0, 0.0, a, -1});
            if (*end != ',') break;
            e = end + 1;
          }
          for (std::size_t i = 0; i < v.size(); ++i) {
            if (v[i].a.state.empty()) continue;
            v[i].anchor = static_cast<int32_t>(rig.anchors.size());
            rig.anchors.push_back({"key#" + std::to_string(i + 1), v[i].a, false, 0.0});
          }
          return v;
        }();
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - boot_t0).count();
        {
          // Fork: heartbeat every 2 s (SLRR_PE_STREAM_TRACE / SLRR_PE_HEARTBEAT).
          static double next_beat = 2.0;
          if (el >= next_beat) {
            next_beat = el + 2.0;
            if (std::getenv("SLRR_PE_STREAM_TRACE") || std::getenv("SLRR_PE_HEARTBEAT"))
              std::fprintf(stderr, "[pe-boot] heartbeat t=%.1f s frame=%u  ms: flush=%.0f pump0=%.0f input=%.0f sim=%.0f res=%.0f vm=%.0f\n",
                           el, pe_frames, sec_ms[0], sec_ms[1], sec_ms[2], sec_ms[3], sec_ms[4], sec_ms[5]);
            for (double& m : sec_ms) m = 0.0;
          }
        }
        bool key_before = true;  // the entry before this one has fired
        for (KeyAt& k : keys) {
          // Press when the clock crosses t; release on the first frame at least
          // the hold time (150 ms by default) later (a slow frame must not
          // skip the press).
          const bool due = k.anchor < 0 || rig.anchors[static_cast<size_t>(k.anchor)].due;
          const double kt = k.anchor < 0 ? k.t : rig.anchors[static_cast<size_t>(k.anchor)].t;
          const bool before = key_before;
          key_before = k.state != 0;
          if (k.state == 0 && before && due && el >= kt) {
            key_before = true;
            k.state = 1; k.down_at = el;
            input_inject_key(k.dik, true);
            std::printf("[pe-boot] key 0x%02x down at %.2f s\n", k.dik, el);
          } else if (k.state == 1 && el >= k.down_at + k.hold) {
            k.state = 2;
            input_inject_key(k.dik, false);
            std::printf("[pe-boot] key 0x%02x up at %.2f s\n", k.dik, el);
          }
        }
        // Fork: SLRR_PE_BOOT_MOUSE="t:x,y[,op...];t:x,y[,op...];..." moves the
        // mouse of an unattended run; same clock as the keys. x, y are
        // back-buffer pixels. Ops, applied in order once (x, y) is reached:
        // L / l left button down / up, R / r right button down / up, W<n>
        // wheel by n notches (signed, +1 = away from the user), G<secs>
        // glide: reach (x, y) linearly over that many seconds, one sub-move
        // per loop pass, the last one exactly on the target. Entries run in
        // the order written; one starts when its time has come and the one
        // before it is done. input_inject_mouse (input_win32.cpp) is the
        // hand, the OS and the device. One log line per entry; a glide has
        // one when it starts and one when it ends, the second with the
        // counts of the whole glide.
        {
          struct MouseAt {
            double t = 0.0;
            int32_t x = 0, y = 0;
            std::vector<InputInjectMouseOp> ops;
            double glide = 0.0;
            int state = 0;  // 0 pending, 1 gliding, 2 done
            double t0 = 0.0;
            int32_t x0 = 0, y0 = 0, sum_dx = 0, sum_dy = 0;
            BootAnchor a;       // Fork (m5 rig): an anchored entry time
            int32_t anchor = -1;
          };
          static std::vector<MouseAt> mouse = [&] {
            std::vector<MouseAt> v;
            const char* e = std::getenv("SLRR_PE_BOOT_MOUSE");
            while (e && *e) {
              MouseAt m;
              char* end = nullptr;
              const char* te = boot_entry_time(e, v.empty() ? nullptr : &v.back().a, &m.a, &m.t);
              end = const_cast<char*>(te ? te : e);
              bool ok = te != nullptr && *end == ':';
              if (ok) {
                const char* p = end + 1;
                const double fx = std::strtod(p, &end);
                ok = end != p && *end == ',';
                if (ok) {
                  p = end + 1;
                  const double fy = std::strtod(p, &end);
                  ok = end != p;
                  m.x = static_cast<int32_t>(std::lround(fx));
                  m.y = static_cast<int32_t>(std::lround(fy));
                }
              }
              while (ok && *end == ',') {
                const char op = end[1];
                if (op == 0) { ok = false; break; }
                const char* p = end + 2;
                end = const_cast<char*>(p);
                if (op == 'L' || op == 'l' || op == 'R' || op == 'r') {
                  m.ops.push_back({op, 0});
                } else if (op == 'W') {
                  const long n = std::strtol(p, &end, 10);
                  ok = end != p;
                  m.ops.push_back({'W', static_cast<int32_t>(n)});
                } else if (op == 'G') {
                  m.glide = std::strtod(p, &end);
                  ok = end != p && m.glide >= 0.0;
                } else {
                  ok = false;
                }
              }
              if (!ok || (*end != ';' && *end != 0)) {
                // A script that cannot be read is not run in part.
                std::printf("[pe-boot] mouse script not read: entry %d at '%s': nothing is injected\n",
                            static_cast<int>(v.size()) + 1, e);
                v.clear();
                break;
              }
              v.push_back(m);
              if (*end != ';') break;
              e = end + 1;
            }
            for (std::size_t i = 0; i < v.size(); ++i) {
              if (v[i].a.state.empty()) continue;
              v[i].anchor = static_cast<int32_t>(rig.anchors.size());
              rig.anchors.push_back({"mouse#" + std::to_string(i + 1), v[i].a, false, 0.0});
            }
            return v;
          }();
          auto mouse_line = [&](const InputInjectMouseState& s, int32_t dx, int32_t dy, int32_t wheel) {
            std::printf("[pe-boot] mouse t=%.2f pos=(%d,%d) d=(%d,%d) wheel=%d buttons=%s locked=%d\n",
                        el, s.x, s.y, dx, dy, wheel,
                        s.left ? (s.right ? "LR" : "L") : (s.right ? "R" : "-"), s.locked ? 1 : 0);
          };
          // An entry with ops (a button level, the wheel) is followed by the
          // next entry only once the device slot has been polled since:
          // the poll runs behind the 15 ms gate, and a press and its release
          // applied in one pass (a loading pass covering both times) would
          // be over before any poll, so the cursor would see no click.
          static bool wait_poll = false;
          static uint32_t wait_mark = 0;
          for (MouseAt& m : mouse) {
            if (m.state == 2) continue;
            if (wait_poll) {
              InputInjectMouseState w;
              input_inject_mouse_peek(&w);
              if (w.polls == wait_mark) break;
              wait_poll = false;
            }
            if (m.state == 0) {
              if (m.anchor >= 0) {
                const RigAnchor& r = rig.anchors[static_cast<size_t>(m.anchor)];
                if (!r.due || el < r.t) break;
              } else if (el < m.t) {
                break;
              }
              InputInjectMouseState s;
              input_inject_mouse_peek(&s);
              if (m.glide > 0.0 && s.placed) {
                // The glide starts where the hand is; its sub-moves begin
                // with the next loop pass.
                m.state = 1;
                m.t0 = el;
                m.x0 = s.hand_x;
                m.y0 = s.hand_y;
                mouse_line(s, 0, 0, 0);
                break;
              }
            }
            double f = 1.0;
            if (m.state == 1) {
              f = (el - m.t0) / m.glide;
              if (f > 1.0) f = 1.0;
            }
            const bool last = f >= 1.0;
            const int32_t tx = last ? m.x : m.x0 + static_cast<int32_t>(std::lround((m.x - m.x0) * f));
            const int32_t ty = last ? m.y : m.y0 + static_cast<int32_t>(std::lround((m.y - m.y0) * f));
            InputInjectMouseState s;
            input_inject_mouse(tx, ty, last ? m.ops.data() : nullptr,
                               last ? static_cast<int32_t>(m.ops.size()) : 0, &s);
            m.sum_dx += s.dx;
            m.sum_dy += s.dy;
            if (!last) break;
            int32_t wheel = 0;
            for (const InputInjectMouseOp& o : m.ops)
              if (o.op == 'W') wheel += o.n;
            mouse_line(s, m.sum_dx, m.sum_dy, wheel);
            m.state = 2;
            if (!m.ops.empty()) {
              wait_poll = true;
              wait_mark = s.polls;
            }
          }
        }
        // Fork (m5 test rig): SLRR_PE_BOOT_SHOTS="<time>:<file.bmp>[;...]"
        // writes the frame presented at the top of the pass in which an
        // entry is due, as SLRR_PE_BOOT_SHOT does at the loop end; the
        // times as in the key and mouse scripts (boot_entry_time), in the
        // order written. One line per shot with the camera passes' counts of
        // that frame. The exe has nothing of this.
        {
          struct ShotAt {
            double t = 0.0;
            std::string path;
            BootAnchor a;
            int32_t anchor = -1;
            bool done = false;
          };
          static std::vector<ShotAt> shots = [&] {
            std::vector<ShotAt> v;
            const char* e = std::getenv("SLRR_PE_BOOT_SHOTS");
            while (e && *e) {
              ShotAt s;
              const char* te = boot_entry_time(e, v.empty() ? nullptr : &v.back().a, &s.a, &s.t);
              const char* q = te && *te == ':' ? std::strchr(te + 1, ';') : nullptr;
              if (te && *te == ':')
                s.path.assign(te + 1, q ? static_cast<size_t>(q - te - 1) : std::strlen(te + 1));
              if (s.path.empty()) {
                std::printf("[pe-boot] shot script not read: entry %d at '%s': no shot is taken\n",
                            static_cast<int>(v.size()) + 1, e);
                v.clear();
                break;
              }
              v.push_back(s);
              if (!q) break;
              e = q + 1;
            }
            for (std::size_t i = 0; i < v.size(); ++i) {
              if (v[i].a.state.empty()) continue;
              v[i].anchor = static_cast<int32_t>(rig.anchors.size());
              rig.anchors.push_back({"shot#" + std::to_string(i + 1), v[i].a, false, 0.0});
            }
            return v;
          }();
          for (std::size_t i = 0; i < shots.size(); ++i) {
            ShotAt& s = shots[i];
            if (s.done) continue;
            if (s.anchor >= 0) {
              const RigAnchor& r = rig.anchors[static_cast<size_t>(s.anchor)];
              if (!r.due || el < r.t) break;
            } else if (el < s.t) {
              break;
            }
            s.done = true;
            int32_t vps = 0, inst = 0, txt = 0;
            render_d3d9_cam_pass_counts(&vps, &inst, &txt);
            const bool ok = render_d3d9_print_screen(s.path.c_str());
            std::printf("[pe-boot] shot#%zu t=%.2f %s -> %d (bound_vps=%d cam_instances=%d cam_texts=%d)\n",
                        i + 1, el, s.path.c_str(), ok ? 1 : 0, vps, inst, txt);
            // m6 rig: the career fields of the pass this frame was taken in.
            if (rig.log)
              std::printf("[career] t=%.2f shot#%zu %s\n", el, i + 1, rig_fields().c_str());
          }
        }
      }
      if (pe_max_frames > 0 && pe_frames >= pe_max_frames) break;
      // Fork: SLRR_PE_BOOT_SECONDS=N bounds the loop by wall time instead.
      static const double boot_secs = [] {
        const char* e = std::getenv("SLRR_PE_BOOT_SECONDS");
        return e ? std::strtod(e, nullptr) : 0.0;
      }();
      if (boot_secs > 0.0 &&
          std::chrono::duration<double>(std::chrono::steady_clock::now() - boot_t0).count() >= boot_secs)
        break;
      if (end_set) {
        const bool due = end_anchor < 0 || rig.anchors[static_cast<size_t>(end_anchor)].due;
        const double et = end_anchor < 0 ? end_t : rig.anchors[static_cast<size_t>(end_anchor)].t;
        const double el_e = std::chrono::duration<double>(std::chrono::steady_clock::now() - boot_t0).count();
        if (due && el_e >= et) {
          std::printf("[pe-boot] end at %.2f s (SLRR_PE_BOOT_END)\n", el_e);
          break;
        }
      }
    }
    // Fork: SLRR_PE_BOOT_SHOT=<file.bmp> dumps the last presented frame.
    if (const char* shot = std::getenv("SLRR_PE_BOOT_SHOT"); shot && shot[0]) {
      render_d3d9_flush();
      std::printf("[pe-boot] print screen %s -> %d\n", shot,
                  render_d3d9_print_screen(shot) ? 1 : 0);
    }
    report("loop end", pe_frames);
    // Fork (m5 test rig).
    if (rig.watch) {
      for (const RigAnchor& r : rig.anchors)
        if (!r.due)
          std::printf("[pe-boot] anchor %s %s never reached\n", r.what.c_str(),
                      boot_anchor_text(r.a).c_str());
      vmthread_set_enter_watch(false);
      vmthread_set_enter_call_hook(nullptr);
      g_rig_cb = nullptr;
    }
    if (rig.log) {
      InvObject* so = rig_obj(rig_static("actualState"));
      const char* sc = so ? tree_host_class(so) : nullptr;
      std::printf("[career] end t=%.2f state=%s %s\n",
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - boot_t0).count(),
                  sc && sc[0] ? sc : "null", rig_fields().c_str());
    }
    jvm_noinit_report();  // Fork diagnostic: SLRR_PE_NOINIT_REPORT=1 (m5 step 2, Q5)
    std::printf("[pe-boot] loop time: render+pumps %.0f ms, vm %.0f ms, vm steps %llu\n",
                pe_t_render, pe_t_vm, static_cast<unsigned long long>(vmthread_step_count()));
    input_debug_dump(stdout);  // Fork: `[input] mouse axes` (SLRR_PE_NATIVE_PROF)
    // Fork: the game ticks of the loop (SLRR_PE_NATIVE_PROF): how many, and
    // their dt in milliseconds. `over100` = ticks the game tick capped at
    // 0.1 s (a loading pass); `mean100` = the mean of the others; `due` =
    // ticks accounted at their due time (16 ms) and `late` = how long after
    // it the host ran them, mean milliseconds.
    if (native_type_log()) {
      int32_t median = 0;
      for (int32_t i = 0, seen = 0; i < 258; ++i) {
        seen += pe_tick.hist[i];
        if (seen * 2 >= pe_tick.n) { median = i; break; }
      }
      double sum100 = 0.0;
      int32_t n100 = 0;
      for (int32_t i = 0; i <= 100; ++i) {
        sum100 += static_cast<double>(i) * pe_tick.hist[i];
        n100 += pe_tick.hist[i];
      }
      std::printf("[pe-boot] ticks n=%d mean=%.3f min=%d max=%d median=%d over100=%d mean100=%.3f due=%d late=%.2f\n",
                  pe_tick.n, pe_tick.n > 0 ? pe_tick.sum_ms / pe_tick.n : 0.0,
                  pe_tick.min_ms, pe_tick.max_ms, median, pe_tick.over100,
                  n100 > 0 ? sum100 / n100 : 0.0, pe_tick.due,
                  pe_tick.due > 0 ? pe_tick.late_ms / pe_tick.due : 0.0);
    }
    vmthread_native_prof_dump(stdout);
    vmthread_pump_stats_dump(stdout);
    render_d3d9_debug_dump(stdout);
    return 0;
  }

  InvObject* splash = game_logic_boot_splash();
  const char* splash_cn =
      splash && tree_host_class(splash) ? tree_host_class(splash) : "?";
  std::printf("  splash state='%s' entered=%d\n", splash_cn,
              splash ? tree_field_get_int(splash, "entered") : -1);
  if (!splash || !std::strstr(splash_cn, "SplashScreen")) {
    std::printf("FAIL --game SplashScreen\n");
    return 5;
  }

  InvObject* menu = game_logic_finish_splash();
  const int32_t splash_mask = tree_field_get_int(splash, "event_mask");
  constexpr int32_t kEventTime = 0x00000080;
  const int32_t splash_time_off = (splash_mask & kEventTime) == 0 ? 1 : 0;
  std::printf("  splash exit mask=0x%x time_off=%d\n", splash_mask,
              splash_time_off);
  InvObject* mmd = menu ? tree_field_get_obj(menu, "mmd") : nullptr;
  const int32_t mmd_shown = mmd ? tree_field_get_int(mmd, "shown") : 0;
  // Prefer TREE chrome (createHeader/createMenu/addItem leaves). Fallback host
  // chrome only if TREE did not materialize enough labels/buttons.
  if (mmd && !tree_field_get_int(mmd, "menu_chrome")) {
    InvObject* osd = tree_field_get_obj(mmd, "osd");
    const int32_t btn =
        osd ? tree_field_get_int(osd, "button_count") : 0;
    const int32_t txt = render_d3d9_osd_text_count();
    const int32_t osdn = render_d3d9_osd_count();
    std::printf("[script] MainMenu TREE chrome probe btn=%d txt=%d osd=%d\n",
                btn, txt, osdn);
    if (btn >= 12 && txt >= 12 && osdn >= 1) {
      tree_field_set_int(mmd, "menu_chrome", 1);
      std::printf("[script] MainMenuDialog chrome via TREE btn=%d txt=%d osd=%d\n",
                  btn, txt, osdn);
    } else {
      mainmenu_dialog_ensure_chrome(mmd);
    }
  }
  const int32_t menu_chrome = mmd ? tree_field_get_int(mmd, "menu_chrome") : 0;
  const int32_t menu_osd_n = render_d3d9_osd_count();
  const int32_t menu_txt_n = render_d3d9_osd_text_count();
  const int32_t acg_host =
      mmd ? tree_field_get_int(mmd, "addCustomGroups_via_host") : 0;
  const int32_t btn_n =
      mmd && tree_field_get_obj(mmd, "osd")
          ? tree_field_get_int(tree_field_get_obj(mmd, "osd"), "button_count")
          : 0;
  std::printf(
      "  splash→menu state='%s' mmd_shown=%d chrome=%d osd=%d txt=%d "
      "acg_host=%d btn=%d\n",
      menu && tree_host_class(menu) ? tree_host_class(menu) : "?", mmd_shown,
      menu_chrome, menu_osd_n, menu_txt_n, acg_host, btn_n);
  if (!menu || !std::strstr(tree_host_class(menu), "MainMenu") ||
      game_logic_actual_state() != menu || mmd_shown != 1) {
    std::printf("FAIL --game MainMenu\n");
    return 5;
  }
  // Phase 2.161: GENERALBG/header rects + ≥12 labels (or buttons).
  if (menu_chrome != 1 || menu_txt_n < 12 || menu_osd_n < 1) {
    std::printf("FAIL --game MainMenu chrome (chrome=%d osd=%d txt=%d)\n",
                menu_chrome, menu_osd_n, menu_txt_n);
    return 5;
  }
  if (!splash_time_off) {
    std::printf("FAIL --game SplashScreen.exit EVENT_TIME still armed\n");
    return 5;
  }

  // Stock Frontend.init starts HotkeyWatcher; we deferred past Splash TREE.
  frontend_start_hotkey_watcher();

  InvObject* player = game_logic_player();
  InvObject* ctrl = player ? tree_field_get_obj(player, "controller") : nullptr;
  InvObject* cursor = java_io_Input_cursor();
  if (ctrl) {
    tree_field_set_obj(cursor, "controller", ctrl);
    java_io_Input_mapAxis(ctrl, kAxisSelect, /*device*/ 0, kDikReturn, 0.f, 1.f,
                          0.f, 1.f);
    java_io_Input_mapAxis(ctrl, kAxisThrottle, /*device*/ 0, kDikUp, 0.f, 1.f,
                          0.f, 1.f);
    java_io_Input_mapAxis(ctrl, kAxisCancel, /*device*/ 0, kDikEscape, 0.f, 1.f,
                          0.f, 1.f);
  }
  java_io_MouseCursor_enable(cursor, 1);
  input_live_enable(true);

  InvObject* osd = mmd ? tree_field_get_obj(mmd, "osd") : nullptr;
  if (osd) tree_field_set_int(osd, "visible", 1);

  std::printf(
      "  live: ENTER=New Career  close window=quit  (ESC skips FMV only)\n");
  if (auto_new)
    std::printf(
        "  smoke PATH-TO-WORLD: auto_new → Garage → Hit the Street → "
        "Valocity\n");

  // Drain residual ESC from FMV skip before AXIS_CANCEL is live.
#ifdef _WIN32
  while ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
    render_d3d9_pump(0);
    Sleep(1);
    if (render_d3d9_quit_requested()) break;
  }
#endif

  float prev_select = 0.f;
  float prev_cancel = 1.f;  // require ESC edge (up→down), not sticky hold
  int32_t frames = 0;
  int32_t garage_ok = 0;
  bool fired_new = false;
  bool fired_freeride = false;
  int32_t hub_freeride_ok = 0;
  int32_t hub_quickrace_ok = 0;
  int32_t hub_demo_ok = 0;
  int32_t hub_back_ok = 0;
  int32_t hub_exit_ok = 0;
  int32_t hub_options_ok = 0;
  int32_t hub_credits_ok = 0;
  bool garage_phase = false;
  bool city_phase = false;
  InvObject* garage = nullptr;
  InvObject* city = nullptr;
  InvObject* starter_car = nullptr;
  InvObject* install_probe = nullptr;
  InvObject* garage_painter = nullptr;
  int32_t garage_smoke_step = 0;
  int32_t mode_mech = -1;
  int32_t mode_paint = -1;
  int32_t mode_none = -1;
  int32_t car_ok = 0;
  int32_t install_ok = 0;
  int32_t paint_ok = 0;
  int32_t painter_ux_ok = 0;
  int32_t mech_inv_ok = 0;
  int32_t mech_vis_ok = 0;
  int32_t mech_chrome_ok = 0;
  int32_t mech_install_ok = 0;
  int32_t mech_drop_ok = 0;
  int32_t mech_preview_ok = 0;
  int32_t mech_rotate_ok = 0;
  int32_t mech_swap_ok = 0;
  int32_t mech_hover_ok = 0;
  int32_t mech_click_ok = 0;
  int32_t mech_drag_ok = 0;
  int32_t mech_lclick_ok = 0;
  int32_t mech_pick_ok = 0;
  int32_t mech_rdrag_ok = 0;
  int32_t cursor_lock_ok = 0;
  int32_t cursor_ldrag_ok = 0;
  int32_t mech_tune_ok = 0;
  int32_t valo_entered = 0;
  float valo_dist = 0.f;
  float valo_spd = 0.f;
  int32_t valo_shape = 0;
  int32_t valo_denied = 0;
  int32_t valo_back = 0;
  int32_t valo_cam = 0;
  int32_t valo_vp = 0;
  int32_t valo_nav_upd = 0;
  int32_t valo_hud = 0;
  float valo_cam_behind = 0.f;
  // Soft phys/aero/vehicle probes (post city_drive_smoke side-bands only).
  int32_t valo_phys = 0;
  int32_t valo_aero = 0;
  int32_t valo_vehicle = 0;
  float valo_phys_spd = 0.f;
  float valo_aero_fmag = 0.f;
  float valo_aero_cd = 0.f;
  int32_t valo_aero_applied = 0;
  int32_t valo_chassis_shape = 0;
  int32_t valo_driveable = 0;
  int32_t traffic_p_ok = 0;
  int32_t traffic_bh_ok = 0;
  int32_t traffic_halt_ok = 0;
  int32_t traffic_halt_path_ok = 0;
  int32_t traffic_rem_ok = 0;
  int32_t traffic_del_ok = 0;
  int32_t traffic_ped_ok = 0;
  int32_t traffic_ped_type_ok = 0;
  int32_t traffic_ped_rem_ok = 0;
  int32_t traffic_ped_dist_ok = 0;
  int32_t traffic_fog_ok = 0;
  int32_t traffic_light_ok = 0;
  int32_t traffic_flare_ok = 0;
  int32_t traffic_dup_ok = 0;
  int32_t traffic_create_ok = 0;
  int32_t traffic_chg_ok = 0;
  int32_t traffic_color_ok = 0;
  int32_t traffic_type_ok = 0;
  int32_t traffic_scale_ok = 0;
  int32_t traffic_plot_ok = 0;
  int32_t traffic_routepos_ok = 0;
  int32_t traffic_setmatrix_ok = 0;
  int32_t traffic_setpos_ok = 0;
  int32_t traffic_setstate_ok = 0;
  int32_t traffic_getpos_ok = 0;
  int32_t traffic_getvel_ok = 0;
  int32_t traffic_setparent_ok = 0;
  int32_t traffic_getspeedsq_ok = 0;
  int32_t traffic_isempty_ok = 0;
  int32_t traffic_isscripted_ok = 0;
  int32_t traffic_getscript_ok = 0;

  if (ctrl && osd) {
    java_io_Input_flushHotkeys();
    java_io_Input_checkHotkeys(ctrl, osd);
  }

  constexpr int32_t kCmdMechanic = 117;
  constexpr int32_t kCmdPaint = 118;
  constexpr int32_t kCmdEscape = 119;
  constexpr int32_t kCmdHitTheStreet = 109;
  constexpr int32_t kProbeSlot = 199;
  constexpr int32_t kPaintColor = static_cast<int32_t>(0xFFAABBCC);
  constexpr int32_t kDikM = 0x32;
  constexpr int32_t kDikP = 0x19;
  constexpr int32_t kDikT = 0x14;
  constexpr int32_t kDikU = 0x16;
  constexpr int32_t kDikN = 0x31;
  constexpr int32_t kDikI = 0x17;  // install probe
  constexpr int32_t kDikK = 0x25;  // paintPart
  constexpr int32_t kDikH = 0x23;  // Hit the Street
  constexpr int32_t kDikG = 0x22;  // return Garage trigger

  auto garage_do_install = [&]() {
    if (!starter_car) return;
    if (!install_probe) {
      install_probe = gameref_new();
      java_util_resource_ResourceRef_set(install_probe, 0x7E0300AA);
    }
    part_disable_slot(starter_car, kProbeSlot, 0);
    const bool ok = part_install(starter_car, kProbeSlot, install_probe, 1);
    install_ok =
        (ok && part_on_slot(starter_car, kProbeSlot) == install_probe) ? 1 : 0;
    std::printf("  garage install slot=%d ok=%d\n", kProbeSlot, install_ok);
  };

  auto garage_do_paint = [&]() {
    if (!starter_car || !garage) return;
    // Phase 2.157 — select stock red can then paintPart from cans.paintColor.
    constexpr int32_t kCanIdx = 5;
    constexpr int32_t kExpect =
        static_cast<int32_t>(0xFF000000u | 0xDC191Au);
    const bool sel = garage_painter_select_can(garage, kCanIdx);
    const bool painted = garage_paint_car(garage, 0);
    garage_painter = tree_field_get_obj(garage, "painter");
    InvObject* cans =
        garage_painter ? tree_field_get_obj(garage_painter, "paintCans")
                       : nullptr;
    InvObject* items = cans ? tree_field_get_obj(cans, "items") : nullptr;
    const int32_t n_cans = items ? tree_vector_size(items) : 0;
    paint_ok =
        (sel && painted && garage_painter && n_cans >= 12 &&
         tree_field_get_int(garage_painter, "paint_part_fills") >= 1 &&
         tree_field_get_int(starter_car, "part_texture") == kExpect &&
         tree_field_get_int(garage, "paint_car_count") >= 1 &&
         tree_field_get_int(garage, "last_can_id") == kCanIdx)
            ? 1
            : 0;
    std::printf("  garage paint ok=%d fills=%d cans=%d can=%d ux=%d\n",
                paint_ok,
                garage_painter
                    ? tree_field_get_int(garage_painter, "paint_part_fills")
                    : 0,
                n_cans, kCanIdx, painter_ux_ok);
  };

  auto garage_go_street = [&]() -> bool {
    if (!garage) return false;
    InvObject* next = garage_osd_command(garage, kCmdHitTheStreet);
    if (!next || !std::strstr(tree_host_class(next), "Valocity")) {
      const char* warn =
          tree_field_get_obj(garage, "last_warning")
              ? string_cstr(tree_field_get_obj(garage, "last_warning"))
              : "?";
      std::printf("  Hit the Street FAIL warn='%s'\n", warn ? warn : "?");
      return false;
    }
    city = next;
    city_phase = true;
    garage_phase = false;
    valo_entered = tree_field_get_int(city, "entered") ? 1 : 0;
    if (!valo_entered) valo_entered = (game_logic_actual_state() == city) ? 1 : 0;
    traffic_p_ok = tree_field_get_int(city, "traffic_p_smoke");
    traffic_bh_ok = tree_field_get_int(city, "traffic_bh_ok");
    traffic_halt_ok = tree_field_get_int(city, "halt_smoke");
    traffic_halt_path_ok = tree_field_get_int(city, "halt_path_smoke");
    traffic_rem_ok = tree_field_get_int(city, "rem_car_smoke");
    traffic_del_ok = tree_field_get_int(city, "del_traffic_smoke");
    traffic_ped_ok = tree_field_get_int(city, "ped_dens_smoke");
    traffic_ped_type_ok = tree_field_get_int(city, "ped_type_smoke");
    traffic_ped_rem_ok = tree_field_get_int(city, "ped_rem_smoke");
    traffic_ped_dist_ok = tree_field_get_int(city, "ped_dist_smoke");
    traffic_fog_ok = tree_field_get_int(city, "fog_smoke");
    traffic_light_ok = tree_field_get_int(city, "light_smoke");
    traffic_flare_ok = tree_field_get_int(city, "flare_smoke");
    traffic_dup_ok = tree_field_get_int(city, "dup_smoke");
    traffic_create_ok = tree_field_get_int(city, "create_smoke");
    traffic_chg_ok = tree_field_get_int(city, "chg_smoke");
    traffic_color_ok = tree_field_get_int(city, "color_smoke");
    traffic_type_ok = tree_field_get_int(city, "type_smoke");
    traffic_scale_ok = tree_field_get_int(city, "scale_smoke");
    traffic_plot_ok = tree_field_get_int(city, "plot_smoke");
    traffic_routepos_ok = tree_field_get_int(city, "routepos_smoke");
    traffic_setmatrix_ok = tree_field_get_int(city, "setmatrix_smoke");
    traffic_setpos_ok = tree_field_get_int(city, "setpos_smoke");
    traffic_setstate_ok = tree_field_get_int(city, "setstate_smoke");
    traffic_getpos_ok = tree_field_get_int(city, "getpos_smoke");
    traffic_getvel_ok = tree_field_get_int(city, "getvel_smoke");
    traffic_setparent_ok = tree_field_get_int(city, "setparent_smoke");
    traffic_getspeedsq_ok = tree_field_get_int(city, "getspeedsq_smoke");
    traffic_isempty_ok = tree_field_get_int(city, "isempty_smoke");
    traffic_isscripted_ok = tree_field_get_int(city, "isscripted_smoke");
    traffic_getscript_ok = tree_field_get_int(city, "getscript_smoke");
    std::printf(
        "  PATH-TO-WORLD: Valocity entered=%d map=0x%X (Garage CMD_HITTHESTREET "
        "→ load into world)\n",
        valo_entered, tree_field_get_int(city, "map_id"));
    std::printf(
        "  Valocity live: UP=throttle G=garage ESC=quit  entered=%d "
        "via_tree=%d map=0x%X\n",
        valo_entered, tree_field_get_int(city, "enter_via_tree"),
        tree_field_get_int(city, "map_id"));
    return true;
  };

  auto city_stop_car = [&]() {
    if (!starter_car) return;
    if (ctrl) {
      java_io_Controller_user_SetAxisForce(ctrl, kAxisThrottle, 0.f, 0.f);
      java_io_Controller_user_SetAxisForce(ctrl, kAxisBrake, 0.f, 0.f);
    }
    InvObject* body = tree_field_get_obj(starter_car, "chassis");
    if (!body) body = starter_car;
    if (physics_shape(body) != 0) physics_set_velocity(body, 0.f, 0.f, 0.f);
    tree_field_set_float(starter_car, "speed_sq", 0.f);
  };

  auto city_try_garage = [&](int32_t club) -> InvObject* {
    if (!city) return nullptr;
    city_stop_car();
    valocity_fire_garage_trigger(city, club, 1);
    return valocity_tick(city);
  };

  auto city_return_garage = [&]() -> bool {
    InvObject* back = city_try_garage(1);
    if (!back || !std::strstr(tree_host_class(back), "Garage")) {
      std::printf("  Valocity→Garage FAIL denied=%d state='%s'\n",
                  city ? tree_field_get_int(city, "garage_denied") : -1,
                  back && tree_host_class(back) ? tree_host_class(back) : "?");
      return false;
    }
    garage = back;
    garage_phase = true;
    city_phase = false;
    valo_back = 1;
    osd = tree_field_get_obj(garage, "osd");
    std::printf("  Valocity→Garage ok traffic=%d car_locked=%d\n",
                city ? tree_field_get_int(city, "traffic_count") : -1,
                starter_car ? tree_field_get_int(starter_car, "stopped") : -1);
    if (ctrl && osd) {
      java_io_Input_flushHotkeys();
      java_io_Input_checkHotkeys(ctrl, osd);
    }
    return true;
  };

  auto city_drive_smoke = [&]() {
    if (!city || !starter_car || !ctrl) return;
    const float x0 = tree_field_get_float(starter_car, "pos_x");
    const float z0 = tree_field_get_float(starter_car, "pos_z");
    java_io_Controller_user_SetAxisForce(ctrl, kAxisThrottle, 0.f, 1.f);
    for (int i = 0; i < 40; ++i) valocity_simulate(city, 0.05f);
    java_io_Controller_user_SetAxisForce(ctrl, kAxisThrottle, 0.f, 0.f);
    const float x1 = tree_field_get_float(starter_car, "pos_x");
    const float z1 = tree_field_get_float(starter_car, "pos_z");
    valo_dist = std::sqrt((x1 - x0) * (x1 - x0) + (z1 - z0) * (z1 - z0));
    valo_spd = tree_field_get_float(starter_car, "speed_sq");
    // Soft deepen: observe phys/aero/vehicle side-bands after simulate.
    // Body key mirrors Vehicle.getSpeedSquare @ 0x480500 + city_stop_car
    // (chassis Native.ptr after Vehicle.set; no invent solve/drive).
    InvObject* chassis = tree_field_get_obj(starter_car, "chassis");
    InvObject* body = chassis ? chassis : starter_car;
    if (physics_shape(body) == 0 && physics_shape(starter_car) != 0)
      body = starter_car;
    valo_chassis_shape = chassis ? physics_shape(chassis) : 0;
    valo_shape = physics_shape(body);
    if (valo_shape == 0) valo_shape = physics_shape(starter_car);
    valo_phys_spd = (physics_shape(body) != 0) ? physics_speed_square(body) : 0.f;
    if (valo_phys_spd <= 0.f && physics_shape(starter_car) != 0)
      valo_phys_spd = physics_speed_square(starter_car);
    const float live_spd = java_game_Vehicle_getSpeedSquare(starter_car);
    if (live_spd > valo_spd) valo_spd = live_spd;
    if (valo_phys_spd > valo_spd) valo_spd = valo_phys_spd;
    // Chassis_physWheelTick aero soft @ 0x456a6b..0x456d2d TREE band.
    valo_aero_applied =
        chassis ? tree_field_get_int(chassis, "aero_applied") : 0;
    valo_aero_fmag =
        chassis ? tree_field_get_float(chassis, "aero_phys_20f4") : 0.f;
    valo_aero_cd =
        chassis ? tree_field_get_float(chassis, "force_update_drag_c") : 0.f;
    if (valo_aero_cd == 0.f && chassis)
      valo_aero_cd = tree_field_get_float(chassis, "C_drag");
    const int32_t flags70 =
        chassis ? tree_field_get_int(chassis, "force_update_flags70") : 0;
    valo_driveable = vehicle_is_driveable(starter_car) ? 0 : 1;
    valo_phys =
        (valo_shape != 0 && valo_phys_spd > 1.f && valo_dist > 2.f) ? 1 : 0;
    // Soft aero ok: applied⇒|F|>0; else PE skip gates (flags70&1 / cd<=0 /
    // |v|^2<=1 @ 0x456a61/83/ac8) still coherent — no invent force.
    if (!chassis) {
      valo_aero = 0;
    } else if (valo_aero_applied == 1) {
      valo_aero = (valo_aero_fmag > 0.f) ? 1 : 0;
    } else if ((flags70 & 1) != 0 || valo_aero_cd <= 0.f ||
               valo_phys_spd <= 1.f) {
      valo_aero = 1;
    } else {
      valo_aero = 0;
    }
    valo_vehicle =
        (chassis && valo_driveable == 1 && live_spd > 1.f) ? 1 : 0;
    std::printf(
        "  Valocity drive dist=%.1f spd=%.1f shape=%d ch_shape=%d "
        "phys_spd=%.1f aero_app=%d |F|=%.1f cd=%.3f driveable=%d\n",
        valo_dist, valo_spd, valo_shape, valo_chassis_shape, valo_phys_spd,
        valo_aero_applied, valo_aero_fmag, valo_aero_cd, valo_driveable);
  };

  auto city_cam_smoke = [&]() {
    if (!city || !starter_car) return;
    // Chase/HUD/nav already advanced by simulate; refresh cam once like boot.
    valocity_update_camera(city);
    void* vcam = valocity_camera_key();
    float ex = 0.f, ey = 0.f, ez = 0.f, ax = 0.f, ay = 0.f, az = 0.f;
    const bool got =
        render_d3d9_camera_get_lookat(vcam, &ex, &ey, &ez, &ax, &ay, &az);
    const float cpx = tree_field_get_float(starter_car, "pos_x");
    const float cpy = tree_field_get_float(starter_car, "pos_y");
    const float cpz = tree_field_get_float(starter_car, "pos_z");
    const float coy = tree_field_get_float(starter_car, "ori_y");
    const float cfx = std::sin(coy);
    const float cfz = std::cos(coy);
    valo_cam_behind = (ex - cpx) * cfx + (ez - cpz) * cfz;
    const float look_ahead = (ax - cpx) * cfx + (az - cpz) * cfz;
    InvObject* nav = tree_field_get_obj(city, "nav");
    valo_nav_upd = nav ? tree_field_get_int(nav, "update_count") : 0;
    const int32_t nvis = nav ? tree_field_get_int(nav, "visible") : 0;
    const char* speed_txt =
        render_d3d9_text_get_string(valocity_hud_speed_key());
    const float kph = valocity_speed_kph(starter_car);
    valo_hud =
        (speed_txt && std::strstr(speed_txt, "KPH") && kph > 5.f) ? 1 : 0;
    valo_cam =
        (got && render_d3d9_camera_active() == vcam && valo_cam_behind < -2.f &&
         ey > cpy + 1.f && look_ahead > 0.5f && valo_nav_upd >= 40 &&
         nvis == 1 && valo_hud == 1)
            ? 1
            : 0;
    // Latch while chase cam is live — teardown clears g_active_vp/cam before
    // the end-of-smoke render catalog probe.
    valo_vp = render_d3d9_viewport_active() ? 1 : 0;
    std::printf(
        "  Valocity cam behind=%.1f eye_y=%.1f active=%d vp=%d nav_upd=%d "
        "vis=%d hud=%d kph=%.0f\n",
        valo_cam_behind, ey, render_d3d9_camera_active() == vcam ? 1 : 0,
        valo_vp, valo_nav_upd, nvis, valo_hud, kph);
  };

  auto city_return_smoke = [&]() {
    // Higher-club garage must deny, then club-1 accepts.
    InvObject* denied = city_try_garage(2);
    valo_denied =
        (!denied && city && tree_field_get_int(city, "garage_denied") == 1) ? 1
                                                                           : 0;
    std::printf("  garage deny club2 ok=%d\n", valo_denied);
    city_return_garage();
  };

  while (!render_d3d9_quit_requested()) {
    render_d3d9_pump(0);
    input_live_poll();
    // Nested Dialog.display pumps its own OSD — skip parent tick.
    if (!dialog_modal_active()) {
      if (ctrl && osd) java_io_Input_checkHotkeys(ctrl, osd);
      // Phase 2.162: pointer hover/click for OSD buttons (MainMenu, dialogs…).
      if (osd) osd_tick_pointer(osd);
      if (mmd) mainmenu_credits_tick(mmd, 0.05f);
    }
    // PE Engine_tickTimers @ 0x00427160 — EVENT_TIME oneshot queue.
    java_lang_GameType_pollTimers();
    // PE MainLoop RE pumps @ 0x428CC9 / 0x428CD4 (soft stand-in).
    game_boot_mainloop_resource_pumps();
    // Stock EXIT → changeActiveSection(null).
    if (!game_logic_actual_state()) break;

    float select = 0.f;
    if (ctrl) select = java_io_Controller_user_GetAxisVal(ctrl, kAxisSelect);
    const bool select_edge = (prev_select < 0.5f && select >= 0.5f);
    prev_select = select;

    if (city_phase && city) {
      valocity_simulate(city, 0.05f);

      const int32_t lk = java_io_Input_lastKey() & 0xFF;
      if (!auto_new && lk == kDikG) city_return_garage();

      if (auto_new && garage_smoke_step == 7) {
        city_drive_smoke();
        city_cam_smoke();
        garage_smoke_step = 8;
      } else if (auto_new && garage_smoke_step == 8) {
        city_return_smoke();
        garage_smoke_step = 9;
        break;
      }
    } else if (!garage_phase) {
      if (auto_new && !fired_new && frames == 8) {
#ifdef _WIN32
        smoke_key(VK_RETURN, false);
#endif
        input_live_poll();
      }
      if (auto_new && !fired_new && frames == 9) {
#ifdef _WIN32
        smoke_key(VK_RETURN, true);
#endif
        input_live_poll();
        if (ctrl) select = java_io_Controller_user_GetAxisVal(ctrl, kAxisSelect);
      }

      // Hub FREERIDE is exercised after career smoke (see post-garage), not
      // before CMD_NEW — loadDefaults mid-hub poisons Garage OSD TREE.

      const bool smoke_force = auto_new && !fired_new && frames == 10;
      if (!fired_new && (select_edge || smoke_force ||
                         (auto_new && frames == 9 && select >= 0.5f))) {
        InvObject* st = game_logic_actual_state();
        if (st && std::strstr(tree_host_class(st), "MainMenu")) {
          std::printf(
              "  PATH-TO-WORLD: CMD_NEW (MainMenuDialog.osdCommand=50) → "
              "loadDefaults + career + Garage\n");
          main_menu_cmd_new(name);
          fired_new = true;
        }
      }

      InvObject* st = game_logic_actual_state();
      if (st && std::strstr(tree_host_class(st), "Garage")) {
        garage_ok = 1;
        garage = st;
        garage_phase = true;
        starter_car = player_spawn_starter_car();
        car_ok =
            (starter_car && java_util_resource_ResourceRef_id(starter_car)) ? 1
                                                                            : 0;
        osd = tree_field_get_obj(garage, "osd");
        if (!osd && mmd) osd = tree_field_get_obj(mmd, "osd");
        std::printf(
            "  PATH-TO-WORLD: Garage career=%d money=%d car=%d — next Hit "
            "the Street → Valocity\n",
            game_logic_career_in_progress(),
            game_logic_player() ? tree_field_get_int(game_logic_player(), "money")
                                : -1,
            car_ok);
        std::printf(
            "  Garage live: M=mechanic P=paint T=test U=tune N=none "
            "I=install K=paintPart H=street ESC=quit  car=%d\n",
            car_ok);
        if (ctrl && osd) {
          java_io_Input_flushHotkeys();
          java_io_Input_checkHotkeys(ctrl, osd);
        }
      }
    } else if (garage) {
      // Phase 2.149: RMB EC_RDRAG orbit camera (locks panel hover/click).
      garage_tick_rdrag(garage);
      // Phase 2.144/2.145/2.150: Mechanic szereles (1) or tune (4).
      const int32_t gmode = tree_field_get_int(garage, "mode");
      if ((gmode == 1 || gmode == 4) && !tree_field_get_int(garage, "rdrag")) {
        InvObject* mech = tree_field_get_obj(garage, "mechanic");
        if (mech) {
          mechanic_tick_hover(mech);
          mechanic_tick_click(mech);
          mechanic_tick_preview(mech);
        }
      }
      const int32_t lk = java_io_Input_lastKey() & 0xFF;
      int32_t gcmd = 0;
      bool want_install = false;
      bool want_paint = false;
      bool want_street = false;
      // auto_new owns garage_smoke_step; live DIK must not steal mode_mech.
      if (!auto_new) {
        if (lk == kDikM)
          gcmd = kCmdMechanic;
        else if (lk == kDikP)
          gcmd = kCmdPaint;
        else if (lk == kDikT)
          gcmd = 123;  // CMD_TEST
        else if (lk == kDikU)
          gcmd = 124;  // CMD_TUNE
        else if (lk == kDikN)
          gcmd = kCmdEscape;
        else if (lk == kDikI)
          want_install = true;
        else if (lk == kDikK)
          want_paint = true;
        else if (lk == kDikH)
          want_street = true;
      }

      // Smoke: modes → install → paint → Hit the Street → drive.
      if (auto_new) {
        if (garage_smoke_step == 0 && frames >= 12) {
          gcmd = kCmdMechanic;
          garage_smoke_step = 1;
        } else if (garage_smoke_step == 1 && frames >= 14) {
          mode_mech = tree_field_get_int(garage, "mode");
          InvObject* mech = tree_field_get_obj(garage, "mechanic");
          if (!mech) mech = garage_ensure_mechanic(garage);
          InvObject* minv = mech ? tree_field_get_obj(mech, "inventory") : nullptr;
          mech_inv_ok = (minv && inventory_size(minv) >= 6) ? 1 : 0;
          const int32_t panels = visual_inventory_panel_count(minv);
          const int32_t attached = visual_inventory_attached_count(minv);
          const int32_t vis_upd =
              minv ? tree_field_get_int(minv, "visualsUpdated") : 0;
          const int32_t shown = minv ? tree_field_get_int(minv, "shown") : 0;
          InvObject* p0 =
              (minv && tree_field_get_obj(minv, "panels"))
                  ? tree_vector_element_at(tree_field_get_obj(minv, "panels"), 0)
                  : nullptr;
          const int32_t btn_ok =
              p0 && tree_field_get_obj(p0, "button") &&
              tree_field_get_obj(p0, "osd")
                  ? 1
                  : 0;
          mech_vis_ok = (panels >= 5 && attached >= 1 && vis_upd == 1 &&
                         shown == 1 && btn_ok == 1)
                            ? 1
                            : 0;

          // Phase 2.139 — InventoryPanel Camera + light preview.
          const int32_t preview = visual_inventory_preview_count(minv);
          InvObject* p0cam = p0 ? tree_field_get_obj(p0, "cam") : nullptr;
          const float p0_aov =
              p0cam ? render_d3d9_camera_half_aov(p0cam) : 0.f;
          mech_preview_ok =
              (preview >= 1 && attached >= 1 && preview == attached &&
               p0cam && p0_aov > 50.f &&
               p0 && tree_field_get_obj(p0, "light") &&
               tree_field_get_obj(p0, "osd") &&
               tree_field_get_obj(tree_field_get_obj(p0, "osd"), "vp"))
                  ? 1
                  : 0;
          std::printf("  mechanic preview ok=%d n=%d/%d aov=%.1f cam=%d "
                      "light=%d\n",
                      mech_preview_ok, preview, attached, p0_aov,
                      p0cam ? 1 : 0,
                      p0 && tree_field_get_obj(p0, "light") ? 1 : 0);

          // Phase 2.141 — focusHook rotates catalog yaw + cam lookat.
          float yaw0 = 0.f, pitch0 = 0.f, roll0 = 0.f;
          float ex0 = 0.f, ey0 = 0.f, ez0 = 0.f, ax0 = 0.f, ay0 = 0.f, az0 = 0.f;
          InvObject* ypr0 = p0 ? tree_field_get_obj(p0, "ypr") : nullptr;
          if (ypr0) ypr_get(ypr0, &yaw0, &pitch0, &roll0);
          if (p0cam)
            render_d3d9_camera_get_lookat(p0cam, &ex0, &ey0, &ez0, &ax0, &ay0,
                                          &az0);
          int32_t rot_n = 0;
          for (int i = 0; i < 10; ++i) {
            if (visual_inventory_focus_hook(minv, 0)) ++rot_n;
          }
          float yaw1 = yaw0, pitch1 = pitch0, roll1 = roll0;
          float ex1 = ex0, ey1 = ey0, ez1 = ez0;
          if (ypr0) ypr_get(ypr0, &yaw1, &pitch1, &roll1);
          if (p0cam)
            render_d3d9_camera_get_lookat(p0cam, &ex1, &ey1, &ez1, &ax0, &ay0,
                                          &az0);
          const int32_t ticks =
              p0 ? tree_field_get_int(p0, "focus_ticks") : 0;
          const float dyaw = yaw1 - yaw0;
          const float dpos =
              std::fabs(ex1 - ex0) + std::fabs(ez1 - ez0);
          mech_rotate_ok =
              (rot_n == 10 && ticks >= 10 && dyaw > 0.25f && dpos > 0.01f)
                  ? 1
                  : 0;
          std::printf("  mechanic rotate ok=%d ticks=%d dyaw=%.3f dpos=%.3f\n",
                      mech_rotate_ok, ticks, dyaw, dpos);

          // Phase 2.143 — panelSwap + actualPanel focus.
          InvObject* items0 = minv ? tree_field_get_obj(minv, "items") : nullptr;
          InvObject* before0 =
              items0 ? tree_vector_element_at(items0, 0) : nullptr;
          InvObject* before1 =
              items0 ? tree_vector_element_at(items0, 1) : nullptr;
          const bool swapped =
              minv ? visual_inventory_panel_swap_panels(minv, 0, 1) : false;
          InvObject* after0 =
              items0 ? tree_vector_element_at(items0, 0) : nullptr;
          InvObject* after1 =
              items0 ? tree_vector_element_at(items0, 1) : nullptr;
          mechanic_set_actual_panel(mech, 1);
          InvObject* ap_obj = mechanic_actual_panel(mech);
          const int32_t ap_idx =
              mech ? tree_field_get_int(mech, "actualPanel") : -1;
          const int32_t ticks1 =
              ap_obj ? tree_field_get_int(ap_obj, "focus_ticks") : 0;
          mechanic_tick_preview(mech);
          mechanic_tick_preview(mech);
          const int32_t ticks2 =
              ap_obj ? tree_field_get_int(ap_obj, "focus_ticks") : 0;
          InvObject* info = mech ? tree_field_get_obj(mech, "infoline") : nullptr;
          const char* info_txt = nullptr;
          if (info) {
            if (InvObject* t = tree_field_get_obj(info, "text"))
              info_txt = string_cstr(t);
          }
          mech_swap_ok =
              (swapped && before0 && before1 && after0 == before1 &&
               after1 == before0 &&
               (minv ? tree_field_get_int(minv, "swap_count") : 0) >= 1 &&
               ap_idx == 1 && ap_obj && ticks2 > ticks1 && info_txt &&
               info_txt[0])
                  ? 1
                  : 0;
          std::printf("  mechanic swap ok=%d swaps=%d ap=%d ticks=%d->%d "
                      "info='%s'\n",
                      mech_swap_ok,
                      minv ? tree_field_get_int(minv, "swap_count") : 0, ap_idx,
                      ticks1, ticks2, info_txt ? info_txt : "");

          // Phase 2.144 — EC_HOVER hit-test → actualPanel (norm [0,1]).
          auto panel_center = [&](int32_t pi, float* ox, float* oy) {
            *ox = *oy = -1.f;
            if (!minv) return;
            InvObject* ps = tree_field_get_obj(minv, "panels");
            InvObject* p =
                (ps && pi < tree_vector_size(ps))
                    ? tree_vector_element_at(ps, pi)
                    : nullptr;
            if (!p) return;
            *ox = tree_field_get_float(p, "x") +
                  tree_field_get_float(p, "width") * 0.5f;
            *oy = tree_field_get_float(p, "y") +
                  tree_field_get_float(p, "height") * 0.5f;
          };
          float hx0 = 0.f, hy0 = 0.f, hx1 = 0.f, hy1 = 0.f;
          panel_center(0, &hx0, &hy0);
          panel_center(1, &hx1, &hy1);
          mechanic_clear_actual_panel(mech);
          const bool hit0 = mechanic_hover_at(mech, hx0, hy0);
          const int32_t hover0 =
              mech ? tree_field_get_int(mech, "actualPanel") : -9;
          InvObject* hover0_obj = mechanic_actual_panel(mech);
          const bool miss = !mechanic_hover_at(mech, 0.99f, 0.99f);
          const int32_t hover_miss =
              mech ? tree_field_get_int(mech, "actualPanel") : -9;
          const bool hit1 = mechanic_hover_at(mech, hx1, hy1);
          const int32_t hover1 =
              mech ? tree_field_get_int(mech, "actualPanel") : -9;
          const int32_t at0 = visual_inventory_panel_at(minv, hx0, hy0);
          const int32_t at1 = visual_inventory_panel_at(minv, hx1, hy1);
          mech_hover_ok =
              (hit0 && hover0 == 0 && hover0_obj && miss && hover_miss < 0 &&
               hit1 && hover1 == 1 && at0 == 0 && at1 == 1 &&
               hx0 > 0.f && hy0 > 0.f)
                  ? 1
                  : 0;
          std::printf("  mechanic hover ok=%d at=%d/%d ap=%d->miss->%d "
                      "xy0=%.2f,%.2f\n",
                      mech_hover_ok, at0, at1, hover0, hover1, hx0, hy0);