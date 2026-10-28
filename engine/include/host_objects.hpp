#pragma once

#include "natives.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace inv {

// Host helpers to bind rewrite-side native state onto opaque InvObject*.
InvObject* file_new(const char* path);
InvObject* findfile_new();
InvObject* thread_new(const char* name);
InvObject* vec3_new(float x, float y, float z);
InvObject* ypr_new(float y, float p, float r);
void vec3_get(InvObject* o, float* x, float* y, float* z);
// Fork: scripted key presses for unattended runs (SLRR_PE_BOOT_KEYS); the
// overlay is read by the keyboard axis samplers in IO.cpp. dik = DirectInput
// key code (ENTER 0x1C, UP 0xC8, DOWN 0xD0, LEFT 0xCB, RIGHT 0xCD, ESC 0x01).
void input_inject_key(int32_t dik, bool down);
// Fork: PE Engine_DispatchAnimateEvents — CallNamedMethod("animate") on every
// GameType that registered GII_ANIMATE (GameType.enableAnimateHook), once per
// frame from the main loop. Osd.animate is the only path to Input.lastKey.
void engine_dispatch_animate_events();
// PE Engine_SimulateFrame (also declared in System_internal.hpp): tick sim
// objects + EVENT_TIME timers; the PE boot loop calls it before the pump.
void engine_simulate_frame();
// PE 940 FUN_0047e050, the game tick of the loop pass FUN_0047d760, with the
// seconds since the tick before: the loop's 15 ms gate measures them and
// hands the same value to the input tick. The script-boot loop calls this;
// engine_simulate_frame() measures its own GetTickCount difference and
// stays the legacy callers'.
void engine_simulate_frame_dt(float tick_dt);
void ypr_get(InvObject* o, float* y, float* p, float* r);
void vec3_set(InvObject* o, float x, float y, float z);
void ypr_set(InvObject* o, float y, float p, float r);
bool vec3_is(InvObject* o);
bool ypr_is(InvObject* o);
InvObject* gameref_new();
InvObject* resref_new();
void gameref_on_res_bound(InvObject* self);
void gameref_on_destroy(InvObject* self);
void resref_ensure(InvObject* self);
InvObject* resref_find_by_id(int32_t id);
// PE 940 FUN_0047f200(id, type): a handle on the node with that id when the
// node is of that type (0x12 = a viewport). Host: an object bound to the id
// whose node type (ResState::restype) is `restype`; null when there is none.
InvObject* resref_find_by_id_restype(int32_t id, int32_t restype);
// M7: the render instance (type 3) with id `id` whose parent is one of the
// `count` handles in `parents`; null when there is none.
InvObject* resref_find_render_instance(int32_t id, InvObject* const* parents,
                                       int count);
// The id an object is bound to as a handle; 0 for an object that is no
// handle (or no longer one). Never creates an entry.
int32_t resref_peek_id(InvObject* o);
void resref_set_parent(InvObject* self, InvObject* parent);

// PE Native.ptr field (dword_62E008): ResourceRef_newNative @ 0x47CEA0
// Engine_malloc(16) — +0/+4 links, +8 alive, +0xC node*.
// Host side-map widens with attach block (≥0x40) for CameraCtrl
// setParent_inner(block+0x30) @ 0x4385A2; mid+0x4C → attach (≠ handle*).
struct HostNativeHandle;  // opaque; PE 16B head + host attach emb
HostNativeHandle* native_ptr_get(InvObject* self);
HostNativeHandle* native_ptr_ensure(InvObject* self);  // alloc + alive=1
void native_ptr_clear(InvObject* self);
// PE ResHandle_getPayload @ 0x00419860 — tag 0xA0000000 (= -1610612736).
// Mid (≥0x17C): +0x0C leaf, +0x40 type(53), +0x44 nest, +0x4C block,
// +0x84 fog, +0xBC bone flags, +0xF8 stamp, +0x12C bone-id walk,
// +0x168/+0x178 own-list sentinel/head. Node +0xCC = type53 WT.
// Leaf vtbl+0x20 = CameraCtrl/Chassis_attachSetParent.
void* res_handle_get_payload(void* node, int32_t tag);
void* native_ptr_node(InvObject* self);  // *(handle+0xC) or null
// PE mid+0x4C CameraCtrl arg: attach block +0xC node / +0x30 emb handle.
// Distinct from Native.ptr* so GameRef type1 setParent_inner(block+0x30).
void* native_ptr_attach_block(InvObject* self);
// PE ResHandle_orPayloadFlags @ 0x0048D1F0 / andNot @ 0x0048D210:
// thiscall on Native.ptr handle; *(node+0x54) |= / &= ~flags. voidEvent
// add_light/rem_light use 0x400000 on the type handle (not setLight/setFlare).
int32_t res_handle_or_payload_flags(InvObject* self, int32_t flags);
int32_t res_handle_and_not_payload_flags(InvObject* self, int32_t flags);
int32_t res_handle_get_payload_flags(InvObject* self);
// PE RenderRef_applyLight @ 0x0048C9D0 / applyFlare @ 0x0048CB40 — Soft TREE
// unit floats / flare stamp (getPayload tag 0x80000001 mid poke OOS).
void render_ref_apply_light_soft(InvObject* self, int32_t diffuse,
                                 int32_t ambient, int32_t specular);
void render_ref_apply_flare_soft(InvObject* self, InvObject* glowtexture,
                                 int32_t glowColor, float glowMinSize,
                                 float glowMaxSize, int32_t flareCount,
                                 int32_t rayCount);
// PE Text_createRText2Inst @ 0x0048C670 — type "r_text2" (malloc owned buf).
// Soft: D3D text + TREE kind=2; charset null → false (PE a5+8==0).
bool text_create_rtext2_inst_soft(InvObject* self, InvObject* parent,
                                  InvObject* charset, float x, float z,
                                  int32_t color, int32_t align, float scale);
// PE RenderRef_applyBoneId @ 0x0048BD30 — after RenderRef_bindBone @ 0x48BC40
// (host getBoneId). add_linked/rem_linked bone00/01 path.
int32_t render_ref_apply_bone_id(InvObject* self, int32_t bone_id);
// PE Class_boxObject @ 0x00404E20 — fresh Class shell (not script instance).
InvObject* class_box_object(const char* fqn);

// GameLogic.initVehicleTypes host path (create+init all *_VT under cars:0x1000).
int32_t game_logic_init_vehicle_types();
InvObject* game_logic_vehicle_types();  // java.util.Vector of VehicleType hosts

// Weighted pick + descriptor build (mirrors VehicleType.getVehicleDescriptor).
InvObject* game_logic_get_vehicle_type(int32_t set);
InvObject* vehicle_type_get_vehicle_descriptor(InvObject* vt, int32_t set,
                                               float param);
InvObject* game_logic_get_vehicle_descriptor(int32_t set, float param);

// Input test hooks (no DirectInput yet).
void input_set_axis(int32_t device, int32_t axis, float value);
void input_set_last_key(int32_t key, bool edge_enqueue = true);
// PE Input_cheatRing @ 0x00640924 (16 bytes). lastKey writes ASCII; kismajomCheck
// matches encoded[c]-1 walking backward and zeros the last byte on hit.
void input_cheat_clear();
const char* input_cheat_buffer();
int32_t input_cheat_try_match_encoded(const char* enc);
int32_t input_dik_from_letter(char letter);

// Phase 2.14 — physical→logical axis maps (Input.mapAxis / Controller.user_*).
void input_map_add(InvObject* inst, int32_t vaxis, int32_t device, int32_t paxis,
                   float i_from, float i_to, float l_from, float l_to);
int32_t input_map_del(InvObject* inst, int32_t vaxis, int32_t device,
                      int32_t paxis);
void input_map_reset(InvObject* inst);
float input_map_get_logical(InvObject* inst, int32_t vaxis);
int32_t input_map_count(InvObject* inst);
// Phase 2.32 — override logical axis (Controller.user_SetAxisForce).
void input_map_set_force(InvObject* inst, int32_t vaxis, float value);
void input_map_clear_force(InvObject* inst, int32_t vaxis);
// Phase 2.107 — VirtualAxisSmoothProperties rate filter.
void input_map_set_smooth(InvObject* inst, int32_t vaxis, float center_range,
                          float factor_center, float factor_opposite,
                          float factor_same, float power);
void input_map_set_speed(InvObject* inst, int32_t vaxis, float speed);

// Phase 2.110 — Config mirror for System.getConfigOptions.
InvObject* system_config_host();
InvObject* system_config_host_for_test();
// Phase 2.113 — LoadingScreen / openLib isLoading depth probes.
int32_t system_loading_peak_for_test();
int32_t system_loading_opens_for_test();
int32_t system_ld_priority_for_test();
// PE Engine_MainLoop frame pulse: Engine_SimulateFrame @ 0x00428450
// (Physics_Step → physics_integrate) → Sfx_ListenerSetPose @ 0x005508F0 →
// Sfx_UpdateVoices @ 0x00550980 → GfxEngine_PresentFrame @ 0x00428CFE
// (render_d3d9_flush) → SleepMs when ldWorkScale<=0 → PollWindowQuit
// @ 0x005522A0 → EndFrame @ 0x00554DC0 (FileAsync drain stand-in).
void system_mainloop_sfx_listener_update();
// PE Sfx_ListenerSetPose writer for flt_768720/24/28 (Sound.cpp).
void sfx_listener_set_pos(float x, float y, float z);
// PE Sfx_UpdateVoices @ 0x00550980 — host stand-in (no DS voice table).
void sfx_update_voices();

// Phase 2.18 — host physics body (PhysicsRef + Vehicle speed).
void physics_set_velocity(InvObject* self, float vx, float vy, float vz);
void physics_set_ang_vel(InvObject* self, float wx, float wy, float wz);
void physics_set_asleep(InvObject* self, int32_t asleep);
int32_t physics_is_asleep(InvObject* self);
void physics_integrate(float dt);
float physics_speed_square(InvObject* self);
int32_t physics_shape(InvObject* self);  // 0 none, 1 box, 2 sphere
void physics_extents(InvObject* self, float* a, float* b, float* c);
// Cursor_tick short EVENT_CURSOR: OSD gadgets are PhysicsRef.createBox(z=0.001)
// parented to Group. Map NDC through Osd.convertTextCoordinates (SCALE_3D=1.469)
// and AABB vs half-extents. out_* may be null.
bool physics_pick_osd_gadget(float ndc_x, float ndc_y, InvObject** out_phy,
                             InvObject** out_group, float* out_px, float* out_py,
                             float* out_pz);
// M4 step 2: the bodies the cursor's pick tests. Every physics instance the
// scripts made with PhysicsRef.createBox or createSphere (PE 940
// FUN_004e16a0 and its sphere twin -> FUN_004e8a50: a body and a type-2 node
// under the owner's node) that was not destroyed since: its id
// (ResourceRef.id() of the PhysicsRef: body+0x50, the `phy` of the cursor's
// event strings), its owner (the node object of the `parent` argument:
// body+0x44), the shape, and the pose PhysicsRef.setMatrix gave it (identity
// at the origin without one). No test on thickness, on the Osd or on any
// state: which bodies a camera reaches is the walk's business.
// Fork: a body made by PhysicsRef.create from a body type has no shape on
// the host and is not listed.
struct PhysicsPickBody {
  InvObject* phy = nullptr;
  InvObject* owner = nullptr;
  int32_t id = 0;
  uint64_t seq = 0;   // creation order of the PhysicsRef object
  int32_t shape = 0;  // 1 box, 2 sphere
  float half[3] = {0.f, 0.f, 0.f};  // box: half extents; sphere: [0] = radius
  float pos[3] = {0.f, 0.f, 0.f};
  float ypr[3] = {0.f, 0.f, 0.f};
};
void physics_pick_bodies(std::vector<PhysicsPickBody>* out);
// M6 step 1: the click bodies of the part and car objects
// (NativeTypes_part11.inc). With the shipped data a part, and the car
// itself, is picked only through its click body (PE 940 FUN_004c1f90: the
// mesh the type's `click` record names, at the click line's offset in the
// part's cfg frame; exe_mech X 1.1-1.4): one per object whose type has a
// click shape and whose click body is not hidden (the exe's body flag
// 0x4000); main thread only. `phy` is the host id that stands for the
// body's "click" node (the `phy` of the cursor's events), `owner` the
// object's node, `seq` the creation order of that node and `owner_id` its
// id (both filled by click_pick_resolve), `pos` / `rot` the body's world
// frame (world = rot * local + pos, rows), `radius` its shape's
// shape+0xC, `shape` the type's click shape (opaque here).
struct ClickPickBody {
  InvObject* owner = nullptr;
  int32_t owner_id = 0;  // the node id of `owner`
  int32_t phy = 0;
  uint64_t seq = 0;
  float pos[3] = {0.f, 0.f, 0.f};
  float rot[3][3] = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
  float radius = 0.f;
  const void* shape = nullptr;
};
void click_pick_bodies(std::vector<ClickPickBody>* out);
void click_pick_resolve(std::vector<ClickPickBody>* bodies);
// FUN_004fc140 kinds 6 / 7 on one click body: the segment from `far_pt`
// to `near_pt` (world, metres) in the body's frame against every triangle
// (FUN_004fd6d0); the smallest t (0 = the near point) with 0 < t < best,
// or 0 for no hit. `tri` gets the hit triangle's index.
float click_pick_test(const ClickPickBody& body, const float far_pt[3],
                      const float near_pt[3], float best, int32_t* tri);
// The node of the object a click body's phy id belongs to (null for any
// other id): what ResourceRef.getParent of the exe's "click" node answers.
InvObject* click_phy_owner(int32_t phy);
// Phase 2.19 — flat ground + arcade drive from Controller axes.
void physics_set_ground_y(float y);
float physics_ground_y();
void physics_drive(InvObject* self, InvObject* controller, float dt);
// Phase 2.66 — WheelRef aggregate: steer (rad-ish axis), drive [0..2], radius m.
void physics_set_wheel_params(InvObject* self, float steer, float drive,
                              float radius);
// Phase 2.67 — friction×sliction → grip; brake/hbrake 0..1; roll_res → drag.
void physics_set_wheel_contact(InvObject* self, float friction, float sliction,
                               float brake, float hbrake, float roll_res);
// Phase 2.68 — Pacejka B/C/D (Wheel.java setPacejka 4/2/0) → lateral grip.
void physics_set_wheel_pacejka(InvObject* self, float b, float c, float d);
// Phase 2.69 — spring N/m, damp N/(m/s), rest_len m, arm_len m.
void physics_set_wheel_suspension(InvObject* self, float spring, float damp,
                                  float rest_len, float arm_len);

// Phase 2.81 — feed Chassis/DynoData Nm into arcade accel; RPM estimate.
void physics_set_drive_torque(InvObject* self, float nm);
float physics_get_engine_rpm(InvObject* self);
int32_t physics_is_airborne(InvObject* self);

// Phase 2.67 — read WheelRef contact fields (host; no Java getters).
float wheelref_get_friction(InvObject* self);
float wheelref_get_sliction(InvObject* self);
float wheelref_get_brake(InvObject* self);
float wheelref_get_hbrake(InvObject* self);
float wheelref_get_roll_res(InvObject* self);
float wheelref_get_pacejka(InvObject* self, int32_t i);
bool wheelref_get_arm(InvObject* self, float out[7]);
bool wheelref_get_hub(InvObject* self, float out[10]);
float wheelref_get_force(InvObject* self);
float wheelref_get_damp_bound(InvObject* self);
float wheelref_get_rest_len(InvObject* self);
float wheelref_get_arm_len(InvObject* self);
// Phase 2.33 — arcade gearbox (-1..5); clutch from controller axes.
int32_t physics_get_gear(InvObject* self);
void physics_set_gear(InvObject* self, int32_t gear);

// Phase 2.25 — body–body collision (AABB / sphere).
void physics_set_collide_active(InvObject* self, int32_t on);
int32_t physics_collide_active(InvObject* self);
int32_t physics_collide_events();  // contacts resolved in last integrate

// Phase 2.22 — host road polyline for GroundRef.alignToRoad.
void physics_road_clear();
void physics_road_add_segment(float x0, float y0, float z0, float x1, float y1,
                              float z1);
int32_t physics_road_count();
// Traffic_trySpawnOnRandomPath @ 0x0057B420: pick a random empty polyline
// (path+196==0), place along it. All occupied / empty graph → false.
bool physics_road_random_spawn(float* out_x, float* out_y, float* out_z,
                               float* out_yaw);
// haltTrafficPath zeros all +196 then marks corridor paths occupied.
void physics_road_clear_occupied();
void physics_road_mark_occupied_at(float x, float y, float z);
int32_t physics_road_occupied_count();
// Project (x,z) onto nearest segment; returns false if no roads.
bool physics_road_project(float x, float z, float* out_x, float* out_y,
                          float* out_z, float* out_dx, float* out_dy,
                          float* out_dz);

// Phase 2.23 — road graph queries (junctions / path).
InvObject* physics_road_nearest_cross(float ax, float ay, float az,
                                      float min_dist);
InvObject* physics_road_start_direction(float fx, float fy, float fz, float tx,
                                        float ty, float tz);
float physics_road_route_length(float x0, float y0, float z0, float x1,
                                float y1, float z1, bool* pe_ok = nullptr);
// Phase 2.88 — last findRoute polyline (for plotRoute / getRoutePos).
int32_t physics_road_last_route_count();
bool physics_road_last_route_point(int32_t i, float* x, float* y, float* z);
float physics_road_last_route_length();
bool physics_road_route_sample(float t, float* x, float* y, float* z);
float physics_road_route_param(float x, float y, float z);
int32_t render_line_point_count(InvObject* self);
bool render_line_point_at(InvObject* self, int32_t i, float* x, float* y,
                          float* z);
int32_t render_line_color(InvObject* self);

// Phase 2.24 — seed Valocity club-garage spine (placeholder until map parse).
int32_t physics_road_seed_valocity();
// Phase 2.29 — add centerline segment(s) from an SCX road mesh (PCA on XZ).
int32_t physics_road_add_from_scx(const char* path);
// Phase 2.30 — scan RPAK `sourcefile` → road*.scx; remap missing city meshes
// to objects/meshes/<stem>_egyedi/<stem>_egyedi.scx when present.
// Returns segments added; out_meshes (optional) = SCX files ingested.
int32_t physics_road_seed_from_rpak(const char* pack_name, int32_t* out_meshes);
// Phase 2.41 — load visual city SCX (identity pose; verts often world-space).
// Phase 2.52 — also seed `mesh 0x` ground-map instance recipes.
// Phase 2.57 — instance recipes draw first (budget); sourcefiles fill remainder.
int32_t city_mesh_seed_from_rpak(const char* pack_name, int32_t max_meshes);
int32_t city_mesh_count();
int32_t city_mesh_vertex_total();
int32_t city_instance_count();
int32_t city_instance_drawn();
void city_mesh_clear();

// Boot: one default Controller so Input.isPlayerActive(0)==1.
InvObject* input_init_controllers();
int32_t input_is_player_active(int32_t n);
InvObject* input_get_controller(int32_t n);

// Controller.reset / activateState (ControlSetState host).
InvObject* controller_reset(InvObject* ctrl);
void controller_activate_state(InvObject* ctrl, int32_t group, int32_t new_state);
int32_t controller_css_get(InvObject* ctrl, int32_t group);

// Boot slice after initVehicleTypes: host Player + Garage (+ controller).
InvObject* game_logic_boot_player_garage();
InvObject* game_logic_player();
InvObject* game_logic_garage();

// Frontend.loadingScreen + GfxEngine (Frontend.render) + changeActiveSection.
InvObject* frontend_loading_screen();
void frontend_loading_screen_show();
void frontend_loading_screen_hide();
int32_t frontend_loading_screen_visible();
// Phase 2.118: Frontend.render singleton; flush Present → Object.notify.
InvObject* frontend_gfx_engine();
// Fork: GameRef parent link (nullptr when unbound).
InvObject* gameref_get_parent(InvObject* o);
// Fork: GameRef flag word (PE 940 node+0x84, GameRef.setFlags FUN_004db900:
// WORLDTREEROOT 0x10, WORLDTREELEAF 0x40); 0 for an object that is no GameRef.
int32_t gameref_get_flags(InvObject* o);
// (m6 step 5) Fork: true when `node` is the node of a part (or car) that
// `filter .. 2` hid; the camera pass skips the render instances under it
// (PE 940: node flag 0x400000 on each instance, FUN_004ebdd0). Script boot.
bool native_filter_hidden(void* node);
// Fork: the flag word a pack node starts with (PE 940 FUN_0051eaa0: node+0x84
// = the record's flags byte; 0x10 WORLDTREEROOT, 0x01 has bounds) for a handle
// bound to a game instance record (typeof 1); 0 for any other object.
int32_t resref_node_flags(InvObject* o);
// Fork: gives a render instance the mesh of its render type when the type
// was not loaded at RenderRef.create (PE 940: the camera walk requests the
// type, FUN_00462b40 -> FUN_0046d890 / FUN_0046bfe0). Called by the camera
// pass (draw_viewport_cameras) for an instance a bound camera sees.
// Returns 1 when the instance has its mesh now, 0 when the type is not a
// mesh recipe (a light type), -1 when the type could not be loaded.
int32_t renderref_materialise_pending(InvObject* instance, int32_t type_id);
// Fork: the lines a render instance replaces in its render object (PE 940:
// Part.setTexture / setMesh make a private copy of the part's main render
// type, FUN_004e9470, and FUN_00522b00 replaces a line of the copy; the
// host keeps the render object and gives the instance the difference).
// `slots[i]` is the zero-based index of a `texture` line, `texture_ids[i]`
// the record that line now names; `mesh_set` / `mesh_id` the mesh in place
// of the render object's. The call replaces what the instance had (count 0
// and mesh_set false: nothing replaced). The pending list applies the
// lines after the clone (renderref_materialise_pending); an instance that
// has its mesh already gets the textures bound again at once and, when the
// mesh changed, goes back on the pending list. Script boot only.
void renderref_set_line_overrides(InvObject* instance, const int32_t* slots,
                                  const int32_t* texture_ids, int32_t count,
                                  bool mesh_set, int32_t mesh_id);
// The same without touching the instance's mesh: the lines are forgotten
// (Part.setRenderType: the instance leaves the private copy for a render
// type of the pack and is put on the pending list by the caller).
void renderref_clear_line_overrides(InvObject* instance);
// What the instance shows of a replaced texture line, for the log: the
// renderer key of the texture of record `texture_id` as the replacements
// use it (null when it did not load) and the number of texture entries of
// the instance's sections that are bound to it.
void* renderref_line_texture_key(int32_t texture_id);
// M7, the paint booth (Resources_part5.inc). Runtime textures: id
// 0xFFFF0000 | n from the node allocator, restype 7. Script boot only.
// paint_texture_copy: FUN_004e9470 -> FUN_0051ee30 for the texture record
//   `source_id`: a paintable (render target) copy; 0 when the texture does
//   not load.
// paint_texture_from_file: the texture of an unsaved row of a car file
//   (`sourcefile <file>.<k>` with `flags 1`, FUN_004d5d60); 0 on failure.
// paint_texture_handle: the handle (renderer key) of such an id, else null.
// paint_texture_target: FUN_0044d590 for such an id: a render target from
//   now on (a texture read from a side file becomes one in place).
// PE 940 Painter natives (NativeTypes_part12.inc). native_painter_send:
// doPaint 0x4E0560 (`bigdecal`, kind 0) / paintPart 0x4E0800 (`paint`, kind
// 1): the pick at the cursor `cursor` (FUN_004eb000), the picked node of
// category 9 (part) or 5 (car), the command to it. False when the pick finds
// no part or car (or the script boot is off).
bool native_painter_send(InvObject* cursor, int kind, int32_t color,
                         int32_t brush, int32_t temp, float rot, float size,
                         int32_t flip);
// The exe's '%f' (Lang.cpp, PE 940 FUN_00527a20): truncates at the precision.
bool exe940_format_f(const char* fmt, double v, std::string* out);
int32_t paint_texture_copy(int32_t source_id);
int32_t paint_texture_from_file(const char* path, const char* payload = nullptr);
InvObject* paint_texture_handle(int32_t id);
// The payload text a runtime texture was made from (node+0x88; "" for a
// paintable copy): FUN_004d8510 writes it inline in a car file's unsaved
// row unless it starts `sourcefile save\` or `sourcefile save/`.
std::string paint_texture_payload(int32_t id);
bool paint_texture_target(int32_t id);
// ResourceRef.makeTexture (FUN_00539170): the runtime texture `id` that
// `ref` holds, read from `path`, findable by its id (a Decal the player
// bought is `new Decal(id)`: another handle on the same resource).
void paint_texture_register_file(int32_t id, InvObject* ref, const char* path,
                                 const char* payload);
// Fork: the texture of the global envmap (PE 940 FUN_00421920: the engine
// asks the texture node of GfxEngine.setGlobalEnvmap for its texture every
// frame). One host handle per resource id, loaded on first use; returns the
// renderer's texture key, or null while the texture is not there. Called
// by the camera pass once per flushed frame. Script boot only.
void* renderref_envmap_texture(int32_t res_id);
// PE 940 FUN_00461980: the detail a camera gives the nodes it walks, here
// the part and car objects below `root` (NativeTypes_part8.inc). Called by
// the camera pass once per camera and frame, before any camera draws, with
// the eye, cot(aov / 2) and the camera's lodAmp / lodBias; then
// native_camera_detail_end once: the level of detail of the parts that
// were seen follows (FUN_0051a5f0). Script boot only.
void native_camera_detail(void* root, const float eye[3], float cot_half_aov,
                          float lod_amp, float lod_bias, uint32_t frame);
void native_camera_detail_end(uint32_t frame);
// PE 940: the owner id of the render instances of a part or car object
// (part+0xAC; FUN_004c1f90 / FUN_004c2b10 write it into payload+0x1B4 /
// +0x1B8 of every instance they make, FUN_004ca390 again after an install:
// the id of the car the part now sits on). `node` is the node a render
// instance hangs under; 0 when it has no part or car object. The shadow
// pass draws the instances of one owner as one (render_d3d9_part7.inc).
// Script boot only.
int32_t native_render_owner(void* node);
// Fork: one `[render-dbg]` line per native camera (NativeTypes.cpp), printed
// by render_d3d9_debug_dump.
void native_camera_debug_dump(FILE* out);
// Fork: one `[render-dbg] light type` line per light type payload the host
// read or copied (NativeTypes_part4.inc), printed after the camera lines.
void native_light_type_debug_dump(FILE* out);
// Fork: `[cursor] state ...`, one line per native cursor object
// (NativeTypes_part9.inc), printed by render_d3d9_debug_dump beside the
// camera line when SLRR_PE_NATIVE_PROF is set.
void native_cursor_debug_dump(FILE* out);
// M7: `[paint] commands` at loop end (NativeTypes_part12.inc).
void native_paint_debug_dump(FILE* out);
// Fork: `[render-dbg]` lines for the instances of the native `part` and
// `car` types (NativeTypes_part6.inc): a count, the cars with their wheels,
// and the parts with their render instances.
void native_part_debug_dump(FILE* out);
// The Part natives on the script boot: the answer of the native part or car
// object of the receiver (NativeTypes_part6.inc; PE 940 Part.getCar
// 0x4CC640, getCarRef 0x4CC550, getSlots 0x4CCED0, getSlotID 0x4CCAA0,
// getSlotIndex 0x4CCBF0). Each returns false when the receiver has no such
// object (always on the legacy path and in the unit smoke): the native then
// answers from the legacy slot tables as before.
//   native_part_car: the node id of the car the part sits on (the part
//     itself until it is installed, a car itself) and the script instance
//     of that node (null when its type has no script class);
//   native_part_slot_count / _slot_id / _slot_index: the slot list of the
//     type in cfg order; an index past the end gives id 0, an unknown id
//     index 0.
// (m6 step 6) PE 940 Part.flap(I) 0x4CC440 = FUN_004c5ed0 on the part
// object; false when `self` has none (the legacy body answers).
bool native_part_flap(InvObject* self, int32_t mode, int32_t* ret);
bool native_part_car(InvObject* self, int32_t* node_id,
                     InvObject** script_instance);
bool native_part_slot_count(InvObject* self, int32_t* count);
bool native_part_slot_id(InvObject* self, int32_t index, int32_t* id);
bool native_part_slot_index(InvObject* self, int32_t slot_id, int32_t* index);
// The rest of the script-facing half (NativeTypes_part7.inc), same rule:
// false = the receiver has no part / car object, answer from the legacy
// tables.
//   native_part_mount_slot: Part.getSlotID 0x4CCAA0 for an index below 0:
//     the id of the slot the part is installed through, 0 when none;
//   native_part_on_slot: Part.partOnSlot 0x4CD600: the script instance on
//     the other side of the slot (-1: the part itself);
//   native_part_slot_id_on_slot: Part.slotIDOnSlot 0x4CE770;
//   native_part_wheel_id: Part.getWheelID 0x4CD2E0;
//   native_part_set_slot_pos: Part.setSlotPos 0x4CDDC0; `pos` / `ypr` are
//     three floats each, null where the script passed null;
//   native_part_disable_slot / _slot_disabled: Part.disableSlot 0x4CE4A0,
//     isSlotDisabled 0x4CE660;
//   native_part_slot_damage / _set_slot_damage: Part.getSlotDamage
//     0x4CCD10, setSlotDamage 0x4CDC30 (the pose text of a moved slot);
//   native_part_render_type / _set_render_type: Part.getRenderType
//     0x4CC9A0, setRenderType 0x4CD9E0;
//   native_part_mesh / _set_mesh: Part.getMesh 0x4CC8A0, setMesh 0x4CD840;
//   native_part_texture / _set_texture: Part.getTexture 0x4CD0D0,
//     setTexture 0x4CE1F0.
bool native_part_mount_slot(InvObject* self, int32_t* id);
bool native_part_on_slot(InvObject* self, int32_t slot_id, InvObject** part);
bool native_part_slot_id_on_slot(InvObject* self, int32_t slot_id,
                                 int32_t* other_slot);
bool native_part_wheel_id(InvObject* self, int32_t* id);
bool native_part_set_slot_pos(InvObject* self, int32_t slot_id,
                              const float* pos, const float* ypr);
bool native_part_disable_slot(InvObject* self, int32_t slot_id,
                              int32_t status);
bool native_part_slot_disabled(InvObject* self, int32_t slot_id,
                               int32_t* state);
bool native_part_slot_damage(InvObject* self, int32_t index, char* out,
                             size_t cap);
bool native_part_set_slot_damage(InvObject* self, int32_t index,
                                 const char* text);
bool native_part_render_type(InvObject* self, int32_t* id);
bool native_part_set_render_type(InvObject* self, int32_t id);
bool native_part_mesh(InvObject* self, int32_t* id);
bool native_part_set_mesh(InvObject* self, int32_t id);
bool native_part_texture(InvObject* self, int32_t* id);
bool native_part_set_texture(InvObject* self, int32_t id);
// (m6 step 2) Part.install_OK 0x4CD3A0 on the script boot
// (NativeTypes_part5.inc): the probe of the part or car object. False when
// the receiver has none (the legacy stand-in answers). Binds the part's root
// handle to `dest` and searches as the `install` command does (`hint` =
// pos x y z or null); `*my_slot` 0 = null, else the int[2] {my_slot,
// target_slot}; with `*bind_out` the caller binds the out GameRef to
// `*target` (null = unbound). Installs nothing.
bool native_part_install_ok(InvObject* self, InvObject* dest, int32_t slot2,
                            const float* hint, int32_t* my_slot,
                            int32_t* target_slot, InvObject** target,
                            bool* bind_out);
// Fork (m6 step 2 rig): one call of the legacy install_OK's part_install
// on the script boot (counted; printed at loop end).
void native_part_install_ok_legacy();
// (m6 step 3) GameRef.getInfo 0x47DDF0 on the script boot for a node with a
// part or car object (NativeTypes_part5.inc): the object's answer (the
// type's vt+0x3C, FUN_004c3da0 / FUN_004a15c0). `arg` the int of
// getInfo(II)I, `arg_str` the String of getInfo(ILString;)I (null for the
// int form). Returns 0 when GameRef.getInfo's switch answers (no such
// object, or a query left to the switch), 1 a part answered, 2 a car;
// the answer in *answer.
int native_part_getinfo(InvObject* self, int32_t query, int32_t arg,
                        const char* arg_str, int32_t* answer);
// Fork (m6 step 3 rig): true with SLRR_PE_NATIVE_TYPE_LOG=1, the switch of
// the step's diagnostic lines (NativeTypes_part5.inc).
bool native_getinfo_log();
// Fork (m6 step 3 rig, SLRR_PE_NATIVE_TYPE_LOG): one answer of a part or
// car object (kind 1 / 2) and the switch's for the same call: counted for
// the loop-end census, the first four per query and kind printed.
void native_part_getinfo_note(InvObject* self, int32_t query, int kind,
                              int32_t answer, int32_t host,
                              const char* arg_str);
// `[render-dbg]` lines of the paint: the calls of the run and one line per
// replaced line of a part's or car's render type (NativeTypes_part7.inc).
void native_part_paint_dump(FILE* out);
// The Chassis natives on the script boot: the answer of the car object of
// the chassis (false = it has none).
//   native_car_wheel_count: Chassis.getWheels 0x4B4280, the wheels of the cfg;
//   native_car_mass: Chassis.getMass 0x4B2B90, the mass of the chassis
//     compound in kg.
bool native_car_wheel_count(InvObject* chassis, int32_t* count);
bool native_car_mass(InvObject* chassis, float* kg);
//   native_car_cm: Chassis.getCM 0x4B2680; native_car_bounds:
//     Chassis.getMin 0x4B3220 (want_max false) / getMax 0x4B2DC0, from the
//     box of the chassis compound (m5 step 5, NativeTypes_part7.inc).
bool native_car_cm(InvObject* chassis, float out[3]);
bool native_car_bounds(InvObject* chassis, bool want_max, float out[3]);
//   native_car_max_steer / _set_max_steer: Chassis.getMaxSteer 0x4B3110,
//     setMaxSteer 0x4B5030; native_car_set_ackermann: setAckermann 0x4B47B0;
//   native_car_force_update_begin: the first line of FUN_004aebf0
//     (Chassis.forceUpdate 0x4B5890): the car loses the flag the
//     `updatevariables` command sets; native_car_updatevariables: its call
//     of the script instance's updatevariables().
bool native_car_max_steer(InvObject* chassis, float* value);
bool native_car_set_max_steer(InvObject* chassis, float value);
bool native_car_set_ackermann(InvObject* chassis, float value);
bool native_car_force_update_begin(InvObject* chassis);
bool native_car_updatevariables(InvObject* chassis);
// The WheelRef natives on the script boot: the wheel record of the car
// object of the chassis the WheelRef was made for. `v` holds as many floats
// as the field says. Set returns false when the chassis has no car object;
// get also when the field has no getter or the wheel does not exist.
enum NativeWheelField : int32_t {
  kNativeWheelRadius = 0,  // setRadius 0x4B6E50 / getRadius 0x4B5DC0; 1
  kNativeWheelRestLen,     // setRestLen 0x4B6EF0; 1
  kNativeWheelMinLen,      // setMinLen 0x4B6990; 1
  kNativeWheelMaxLen,      // setMaxLen 0x4B68B0; 1
  kNativeWheelForce,       // setForce 0x4B6380; 1
  kNativeWheelDamping,     // setDamping 0x4B6280 / 0x4B61D0; 2: bound, rebound
  kNativeWheelSteer,       // setSteer 0x4B7010 / getSteer 0x4B5E00; 1
  kNativeWheelYpr,         // setYpr 0x4B70E0 / getYpr 0x4B5E40; 3, set: null = the cfg's
  kNativeWheelPos,         // setPos 0x4B6AF0 / getPos 0x4B5B90; 3, set: null = no offset
  kNativeWheelArm,         // setArm 0x4B5F50; 7: length, p, n
  kNativeWheelHub,         // setHub 0x4B65D0; 10: length, p1, p2, pc
  kNativeWheelOpp,         // setOppWheel 0x4B6A30; 1: the wheel index, below 0 = none
};
bool native_car_wheel_set(InvObject* chassis, int32_t wheel, int32_t field,
                          const float* v);
bool native_car_wheel_get(InvObject* chassis, int32_t wheel, int32_t field,
                          float* v);
InvObject* gametype_get_parent(InvObject* o);  // GameType.createNativeInstance parent
// True when GameType.createNativeInstance made a node for this script object
// (its node's script instance is the object itself).
bool gametype_native_created(InvObject* o);
void frontend_gfx_engine_frame_notify();
// Phase 2.119: stock LoadingScreen.run pacing loop (host).
void frontend_loading_screen_run(InvObject* self);
// Phase 2.121: userWait / track / display + SoftTimer / LoadingDialog.
void frontend_loading_screen_track(InvObject* self, int32_t wait_for_user);
void frontend_loading_screen_user_wait(InvObject* self, float sec);
void frontend_loading_screen_display(InvObject* self, InvObject* dlg,
                                    float wait_limit);
void frontend_loading_screen_show_dialog(InvObject* self, InvObject* dlg);
void frontend_soft_timer_run(InvObject* self);
void frontend_flash_text_run(InvObject* self);
void frontend_text_change_text(InvObject* self, InvObject* text);
// Phase 2.120: Frontend.init / setFonts / HotkeyWatcher / static getters.
void frontend_init();
void frontend_start_hotkey_watcher();
void frontend_set_fonts();
void frontend_destroy();
void frontend_hotkey_watcher_run(InvObject* self);
InvObject* frontend_large_font();
InvObject* frontend_medium_font();
InvObject* frontend_small_font();
InvObject* frontend_pointers();
InvObject* frontend_def_loading_pic();
InvObject* frontend_input_queue();
int32_t frontend_inited();
InvObject* frontend_hotkey_thread();

// Phase 2.124 — Input.cursor singleton + MouseCursor.enable (DF_LEAVEPOINTER).
InvObject* java_io_Input_cursor();
int32_t java_io_MouseCursor_enable(InvObject* self, int32_t state);
// PE Cursor_tick @ 0x00460140 SysCursor path: copy WndProc NDC → +0xA4/+0xA8,
// clamp [-1,1]. Java Config.SysCursor default 1.
void java_io_MouseCursor_tickSysCursor();
// PE Cursor_tick EVENT_CURSOR 0x10000 → handleEvent(GameRef,int,String) /
// addNotification custmethod (Osd.event_handler).
void java_lang_GameType_dispatchCursor(InvObject* obj_ref, InvObject* param);
// PE Engine_queueEvent(dest=+0xEC watch list): physics auto-sends to parent
// Group (setEventMask EVENT_CURSOR). Direct handleEvent on dest.
void java_lang_GameType_dispatchCursorTo(InvObject* dest, InvObject* obj_ref,
                                         InvObject* param);
// M4 step 2: PE 940 FUN_0047d550 -> FUN_0047c760(target, sender, event,
// text, 0), the one way an event with a text reaches a game node (script
// boot; the two dispatchers above stay the legacy tick's). `target` and
// `sender` are node objects (gameref_node_object); `sender` may be null.
//  - when the target's event mask has the event: the handler of its native
//    game object first (a handler that returns true ends it), else the
//    script's handleEvent / handleEventAsync(GameRef, int, String) on a
//    fresh VM thread that runs at once; the GameRef is the sender, or the
//    target itself without one;
//  - then, whatever the mask said, every notification on the target whose
//    mask has the event forwards it to its listener, newest entry first
//    (FUN_004e78e0 links at the head), with the listener's own mask, the
//    entry's event (EVENT_SAME keeps it), custom text and method, and the
//    sender, or the target itself when there was none.
// With `log_tag` one line goes to stderr before anything is called:
// `<tag> to=0x<id> '<class>' mask=<0|1> listeners=<n> sender=0x<id> "<text>"`.
// Runs script code: call it with no host lock held.
void gametype_send_event(InvObject* target, InvObject* sender, int32_t event,
                         const char* text, const char* log_tag);

InvObject* game_logic_change_active_section(InvObject* state);
InvObject* game_logic_actual_state();
// Java GameLogic.changeActiveSection(GameState) — null or a GameState
// implementor. TREE leftovers (String/Dialog/empty class) are not sections.
bool game_logic_is_section(InvObject* state);
// Full mid-boot: MENUSET + hide loading + SplashScreen(frontend:0x5).
InvObject* game_logic_boot_splash();
// Splash timer / AXIS_CANCEL → MainMenu (dialog shown).
InvObject* game_logic_finish_splash();

// GameLogic.autoSave / autoSaveQuiet (modals skipped; returns 1 = OK).
int32_t game_logic_auto_save();
void game_logic_auto_save_quiet();
int32_t game_logic_auto_save_calls();
int32_t game_logic_auto_save_quiet_calls();
int32_t game_logic_load_defaults_calls();

// MainMenuDialog CMD_NEW core (no modals) + JVM dispatch via osdCommand(50).
void game_logic_load_defaults();
int32_t game_logic_game_mode();
int32_t game_logic_career_in_progress();
void game_logic_set_career_in_progress(int32_t v);
int32_t game_logic_day();
float game_logic_time();
void game_logic_set_time(float t);
void game_logic_spend_time(float dt);
InvObject* garage_enter(InvObject* garage, InvObject* prev_state);
void garage_ensure_map(InvObject* garage);
void garage_exit(InvObject* garage, InvObject* next_state);
bool garage_try_create_osd_objects(InvObject* garage);
bool racesetup_try_create_osd_objects(InvObject* rs);
bool racesetup_try_cmd_race(InvObject* rs);
void game_logic_set_played(int32_t v);
int32_t game_logic_played();
InvObject* main_menu_apply_new_career(const char* player_name);
InvObject* main_menu_cmd_new(const char* player_name);
InvObject* main_menu_cmd_freeride();
InvObject* main_menu_cmd_quickrace();
InvObject* main_menu_cmd_demo();
InvObject* main_menu_cmd_back_to_garage();
bool main_menu_cmd_exit();
bool main_menu_cmd_options();
bool main_menu_cmd_credits();
void main_menu_hub_begin_tree();
void main_menu_hub_end_tree();
bool main_menu_hub_deferring();
void main_menu_hub_note_cas(InvObject* next);
void main_menu_cmd_new_begin_tree();
void main_menu_cmd_new_end_tree();
bool main_menu_cmd_new_cas_pending();
bool main_menu_cmd_freeride_cas_pending();
bool main_menu_cmd_exit_cas_pending();
void main_menu_cmd_new_note_garage_cas();
bool main_menu_cmd_new_deferring_osd();
void game_logic_set_game_mode(int32_t mode);
int32_t game_logic_timeout();
void game_logic_set_timeout(int32_t t);
InvObject* game_logic_racesetup();
void game_logic_set_racesetup(InvObject* rs);
// Dialog.display host (stock show/wait/hide). Smoke primes StringRequester input.
void dialog_set_smoke_string(const char* s);
// TREE often misbinds Dialog.display onto MainMenuDialog — track NEW modals.
void dialog_note_constructed(InvObject* dialog, const char* class_fqn);

// Player vehicle + Garage OSD commands.
InvObject* player_spawn_starter_car();  // STOCK/DEMO descriptor → player.car
void garage_lock_car(InvObject* garage);
const char* vehicle_is_driveable(InvObject* car);  // nullptr = OK
// Phase 2.132 — Mechanic inventory (list-only, no VisualInventory UI).
InvObject* inventory_new(InvObject* player);
int32_t inventory_size(InvObject* inv);
InvObject* inventory_ensure_player_parts(InvObject* player);
void inventory_add_part_item(InvObject* inv, InvObject* part);
void inventory_move_to(InvObject* src, int32_t index, InvObject* dst);
InvObject* garage_ensure_mechanic(InvObject* garage);
InvObject* garage_ensure_painter(InvObject* garage);
void garage_painter_show(InvObject* garage);
void garage_painter_hide(InvObject* garage);
bool garage_painter_select_can(InvObject* garage, int32_t index);
bool garage_paint_car(InvObject* garage, int32_t color);
void mechanic_flush_inventory(InvObject* mechanic);
void mechanic_filter_inventory(InvObject* mechanic, int32_t filter_engine,
                               int32_t filter_body, int32_t filter_rgear);
// Phase 2.135 — VisualInventory panels (Mechanic parts strip).
InvObject* visual_inventory_new(InvObject* player, float left, float top,
                                float width, float height);
void visual_inventory_init(InvObject* inv, float left, float top, float width,
                           float height);
void visual_inventory_update(InvObject* inv);
void visual_inventory_show(InvObject* inv);
void visual_inventory_hide(InvObject* inv);
int32_t visual_inventory_panel_count(InvObject* inv);
int32_t visual_inventory_attached_count(InvObject* inv);
int32_t visual_inventory_preview_count(InvObject* inv);
bool visual_inventory_focus_hook(InvObject* inv, int32_t panel_index);
void mechanic_tick_preview(InvObject* mechanic);
void visual_inventory_scroll_up(InvObject* inv);
void visual_inventory_scroll_down(InvObject* inv);
// Phase 2.137 — inventory → car install (panel click).
const char* inventory_install_to_car(InvObject* inv, int32_t index,
                                    InvObject* car);
bool visual_inventory_panel_left_click(InvObject* inv, int32_t panel_index);
bool inventory_panel_osd_command(InvObject* panel, int32_t cmd);
bool mechanic_click_actual(InvObject* mechanic);
void mechanic_tick_click(InvObject* mechanic);
void mechanic_begin_drag_object(InvObject* mechanic, InvObject* part);
void mechanic_set_look_part(InvObject* mechanic, InvObject* part);
bool mechanic_lclick_part(InvObject* mechanic, InvObject* part);
bool part_is_tuneable(InvObject* part);
bool part_flap_toggle(InvObject* part);
bool mechanic_tune_part(InvObject* mechanic, InvObject* part, int32_t choice);
InvObject* mechanic_open_tune_dialog(InvObject* mechanic, InvObject* part);
bool mechanic_tune_set_slider(InvObject* mechanic, int32_t cmd, float value);
bool mechanic_tune_menu_command(InvObject* mechanic, int32_t cmd);
InvObject* mechanic_pick_part_at(InvObject* mechanic, float nx, float ny);
bool mechanic_hover_car_at(InvObject* mechanic, float nx, float ny);
bool mechanic_drop_object_at(InvObject* mechanic, InvObject* part, float nx,
                             float ny);
bool mechanic_drag_panel_to(InvObject* mechanic, float x0, float y0, float x1,
                            float y1);
// Phase 2.138 — car → inventory (panel drag-drop).
bool part_uninstall(InvObject* part);
bool visual_inventory_panel_drag_drop(InvObject* inv, int32_t panel_index,
                                     InvObject* part);
// Phase 2.143 — panel↔panel swap + Mechanic actualPanel focus.
void inventory_swap(InvObject* inv, int32_t index_a, int32_t index_b);
int32_t visual_inventory_item_index_by_button(InvObject* inv,
                                              InvObject* button);
bool visual_inventory_panel_swap(InvObject* inv, int32_t panel_index_a,
                                 InvObject* dropped_button);
bool visual_inventory_panel_swap_panels(InvObject* inv, int32_t panel_a,
                                        int32_t panel_b);
void mechanic_set_actual_panel(InvObject* mechanic, int32_t panel_index);
void mechanic_clear_actual_panel(InvObject* mechanic);
InvObject* mechanic_actual_panel(InvObject* mechanic);
int32_t visual_inventory_panel_at(InvObject* inv, float nx, float ny);
bool mechanic_hover_at(InvObject* mechanic, float nx, float ny);
void mechanic_tick_hover(InvObject* mechanic);
// Phase 2.149 — Garage EC_RDRAG orbit camera (RMB + look axes).
InvObject* garage_ensure_camera(InvObject* garage);
void garage_rdrag_begin(InvObject* garage);
void garage_rdrag_end(InvObject* garage);
bool garage_rdrag_orbit(InvObject* garage, float dyaw, float dpitch);
void garage_tick_rdrag(InvObject* garage);
// Phase 2.136 — Mechanic OSD chrome (scroll / category filters).
void mechanic_ensure_chrome(InvObject* mechanic);
void mechanic_osd_command(InvObject* mechanic, int32_t cmd);
int32_t mechanic_chrome_button_count(InvObject* mechanic);
// CMD_TIME=116, CMD_TESTTRACK=110, CMD_CARLOT=111, CMD_CATALOG=113,
// CMD_MECHANIC=117, CMD_HITTHESTREET=109 — returns new section or garage.
InvObject* garage_osd_command(InvObject* garage, int32_t cmd);
// Catalog / CarLot / TestTrack → Garage.
InvObject* game_state_return_to_garage(InvObject* state);
// Phase 2.133 — ensure Dialog OSD buttons after TREE show skips createButton.
void dialog_ensure_osd_buttons(InvObject* dialog);
// Stock Dialog.display(): show → pump until osdCommand sets result → hide.
int32_t dialog_display(InvObject* dialog);
// Modal currently inside dialog_display (null if none) — boot skips parent tick.
InvObject* dialog_modal_active();
// Phase 2.158 — MainMenuDialog host show skips video/3D; blit RID_GENERALBG +
// menu labels so --game is not a black window.
void mainmenu_dialog_ensure_chrome(InvObject* dialog);
// Osd leaves (stock Java createBG/createHotkey) — avoid TREE packing SO.
void osd_ensure_defaults(InvObject* osd);
InvObject* osd_create_bg(InvObject* osd, InvObject* texture);
InvObject* osd_create_rectangle(InvObject* osd, float x, float y, float w,
                                float h, int32_t pri, InvObject* texture);
InvObject* osd_create_hotkey(InvObject* osd, int32_t key, int32_t flags,
                             int32_t command, InvObject* handler, int32_t ef);
InvObject* osd_create_text(InvObject* osd, const char* text, InvObject* font,
                           int32_t align, float x, float y);
InvObject* osd_create_header(InvObject* osd, const char* title);
InvObject* osd_create_menu(InvObject* osd, InvObject* style, float x, float y,
                           float spc, int32_t ori);
InvObject* osd_create_button(InvObject* osd, InvObject* style, float x, float y,
                             const char* label, int32_t cmd);
InvObject* menu_add_item(InvObject* menu, const char* text, int32_t cmd);
InvObject* menu_add_item_gfx(InvObject* menu, InvObject* gfx, int32_t cmd,
                             const char* tooltip);

// Stock Garage.createOSDObjects street strip when TREE packing fails.
void garage_create_osd_street_menu(InvObject* garage);
void menu_add_separator(InvObject* menu);
int32_t osd_begin_group(InvObject* osd);
int32_t osd_end_group(InvObject* osd);
void osd_hide_group(InvObject* osd, int32_t gid);
void osd_show_group(InvObject* osd, int32_t gid);
// OptionsDialog.show groups (options/video/…) for CMD_OPTIONS navigation.
void options_dialog_ensure_groups(InvObject* dialog);
void options_dialog_change_mode(InvObject* dialog, int32_t group);
// MainMenuDialog credits scroll (stock animate CREDITS_*).
void mainmenu_build_credits(InvObject* dialog);
void mainmenu_credits_reset(InvObject* dialog);
void mainmenu_credits_tick(InvObject* dialog, float dt);
// Phase 2.162 — OSD pointer nav (stock Group EC_HOVER / Gadget.focus stand-in).
void osd_tick_pointer(InvObject* osd);
// Phase 2.134 — Osd.createButton host call counter (TREE path proof).
void osd_create_button_calls_reset();
int32_t osd_create_button_calls();

// Valocity ctor-shaped prepare (map/nav) + enter host/finalize.
void valocity_prepare(InvObject* city);
InvObject* valocity_enter(InvObject* city, InvObject* prev_state);
void valocity_finalize_enter(InvObject* city);
void valocity_exit(InvObject* city, InvObject* next_state);
// Phase 2.34 — host frame: ensure car PhysicsRef + arcade drive + sync TREE.
void valocity_ensure_car_physics(InvObject* car);
void valocity_simulate(InvObject* city, float dt);
// Phase 2.42 — player chassis SCX visual mesh synced to car pose.
void valocity_ensure_car_mesh(InvObject* car);
void valocity_sync_car_mesh(InvObject* car);
// Phase 2.70 — wheel mesh steer (front yaw) + roll spin from speed/radius.
void valocity_sync_wheel_visuals(InvObject* car, float dt);
// Phase 2.43 — body shell parts parented under chassis (hood/bumpers/…).
void valocity_ensure_car_parts(InvObject* car);
int32_t valocity_car_part_count(InvObject* car);
int32_t valocity_car_part_slotted(InvObject* car);
int32_t valocity_car_wheel_count(InvObject* car);
int32_t valocity_car_tyre_count(InvObject* car);
// Phase 2.50 — Part slot poses (setSlotPos) + visual mesh bind.
void part_bind_slot_visual(InvObject* part, int32_t slot_id, InvObject* visual,
                           float px_m, float py_m, float pz_m, float oy,
                           float op, float or_);
void part_set_slot_pos(InvObject* part, int32_t slot_id, InvObject* pos,
                       InvObject* ypr);
InvObject* part_slot_visual(InvObject* part, int32_t slot_id);
int32_t part_slot_count(InvObject* part);
int32_t part_slot_id_at(InvObject* part, int32_t index);
// Read slot pose (metres + YPR). Returns false if slot missing.
bool part_slot_get_pose(InvObject* part, int32_t slot_id, float* px, float* py,
                        float* pz, float* oy, float* op, float* or_);
// Phase 2.53 — install graph: parent.slot ↔ child.mateSlot.
bool part_install(InvObject* parent, int32_t parent_slot_id, InvObject* child,
                  int32_t child_slot_id);
bool part_ensure_chassis_cfg_slots(InvObject* car);
bool part_find_cfg_install(InvObject* car, InvObject* part, int32_t* parent_slot,
                           int32_t* child_slot);
InvObject* part_on_slot(InvObject* part, int32_t slot_id);
int32_t part_slot_id_on_slot(InvObject* part, int32_t slot_id);
// Phase 2.80 — garage slot lock (Chassis suspension chain / camshaft).
void part_disable_slot(InvObject* part, int32_t slot_id, int32_t status);
bool part_slot_is_disabled(InvObject* part, int32_t slot_id);
// Phase 2.60 — walk part_parent to chassis/vehicle root.
InvObject* part_car_root(InvObject* part);
// Phase 2.62 — chassis wheel corner 0..3 from install slot 101..104.
int32_t part_wheel_id(InvObject* part);
// Phase 2.35 — chase camera behind player car (host D3D look-at).
void* valocity_camera_key();
void valocity_update_camera(InvObject* city);
// Phase 2.36 — OSD speed/gear HUD (simple20, CarInfo KPH factor).
void* valocity_hud_speed_key();
void* valocity_hud_gear_key();
float valocity_speed_kph(InvObject* car);
void valocity_update_hud(InvObject* city);
// Phase 2.37 — Navigator minimap (vp/cam + OSD tile follow).
void navigator_paint(InvObject* nav);
InvObject* navigator_viewport(InvObject* nav);
InvObject* navigator_camera(InvObject* nav);
int32_t navigator_current_tile(InvObject* nav);
InvObject* navigator_add_marker_static(InvObject* nav, int32_t rtype_id, float px,
                                       float pz, int32_t pri);
InvObject* navigator_add_marker_dynamic(InvObject* nav, int32_t rtype_id,
                                        InvObject* obj);
void navigator_rem_marker(InvObject* nav, InvObject* m);
// Phase 2.38 — Scene daytime fog + skydome + envmap (Scene.java).
int32_t scene_time2config(float time_sec, float rnd);
void valocity_apply_scene(InvObject* city);
int32_t valocity_scene_config(InvObject* city);
// Garage trigger ON/OFF (clubGarage 1..3). Returns activeTrigger.
int32_t valocity_fire_garage_trigger(InvObject* city, int32_t clubGarage,
                                    int32_t event_on);
// 1s EVENT_TIME tick — may return Garage after YesNo accept.
InvObject* valocity_tick(InvObject* city);
// Simulate garage trigger YesNoDialog accept → Garage.enter.
InvObject* valocity_return_to_garage(InvObject* city);

// ControlSet host (binary CTRL file).
InvObject* control_set_new();
int32_t control_set_load(InvObject* cs, const char* path);
int32_t control_set_nitems(InvObject* cs);
int32_t control_set_ndevices(InvObject* cs);
int32_t control_set_count_group(InvObject* cs, int32_t group);
int32_t control_set_file_check(const char* path);

}  // namespace inv
