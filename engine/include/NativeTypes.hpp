#pragma once
// Native game types of Build 940 (the `native <name> [cfg]` line of a
// typeof-8 pack record) and the game instances made from them.
//
// What the exe has, and what stands for it here:
//   - a table of 15 native type factories at 0x5DA3E8 (FUN_0051fcc0 looks the
//     name up there)                         -> the rows of NativeTypes.cpp;
//   - one type object per typeof-8 node, with a vftable shaped like
//     `_type_camera` 0x59B13C / `_type_ground` 0x59E464
//                                            -> GameTypeDesc + NativeObject;
//   - one game object per instance node (node payload+0x4C), made by the
//     type's vt+0x0C                         -> one NativeObject per instance;
//   - the binary payload of a typeof-1 pack node (FUN_00519940)
//                                            -> InstanceTree.
// Everything here runs on the script boot only (SLRR_PE_BOOT_INIT=1); on the
// legacy path every entry point returns at once and no object exists.

#include "natives.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace inv {

// True on the script boot (SLRR_PE_BOOT_INIT=1), read once.
bool native_script_boot();
// True with SLRR_PE_NATIVE_PROF set: the `[native-type]` log lines.
bool native_type_log();

// ---- The engine's scanner -------------------------------------------------
// PE 940 FUN_00569380 -> FUN_00527140, used for every payload line, command
// and create string. Not the CRT's sscanf:
//   %d %u   skip white space; text starting with 0x / 0X is read as hex
//           (FUN_00527490), else an optional '-' and decimal digits
//           (FUN_00527420);
//   %x      skip white space; needs the 0x prefix and one hex digit;
//   %f      skip white space; optional '-', digits, optional '.' and digits.
//           No '+', no leading '.', no exponent (FUN_00527530);
//   %s      skip white space; copies up to white space, the end, the width
//           (default 1024), or the character that follows the conversion in
//           the format ("%s,%s" stops the first word at a comma);
//   %c      one character;      %*  no store (still counted);
//   white space in the format skips white space in the input;
//   any other character must match or the scan stops there.
// A conversion that fails ends the scan; arguments not reached keep their
// values. Returns the number of conversions done, %* ones included.
// Arguments: int32_t* for %d %u %x, float* for %f, char* for %s %c.
int engine_sscanf(const char* input, const char* format, ...);

// ---- Game types (typeof 8) ------------------------------------------------
// Row numbers of the exe's native type table (0x5DA3E8, 15 entries).
enum NativeTypeIndex : int {
  kNativeGround = 0,
  kNativeCamera = 1,
  kNativeObject = 2,
  kNativeCar = 3,
  kNativePart = 4,
  kNativeTrigger = 5,
  kNativePlayer = 6,
  kNativeButton = 7,
  kNativeCursor = 8,
  kNativeBot = 9,
  kNativeRoad = 10,
  kNativeWalk = 11,
  kNativeTrlight = 12,
  kNativePed = 13,
  kNativeBuilding = 14,
  kNativeTypeCount = 15,
};
const char* native_type_name(int index);

// What FUN_0051fcc0 takes from a typeof-8 payload.
struct GameTypeDesc {
  bool found = false;   // the id names a typeof-8 pack record
  int32_t type_id = 0;
  // `native <name> [cfg]` (or an ICFG / XCFG chunk). native_index is the row
  // of the name in the table; -1 without a native line and for a name the
  // table lacks (`invitem`, `gamemode`, `sun` in the shipped packs: the exe
  // then has no type object either).
  bool has_native = false;
  int native_index = -1;
  std::string native_name;
  std::string cfg_path;     // as written, relative to the game root; "" none
  bool cfg_is_inline = false;
  std::string cfg_inline;   // ICFG: the cfg text carried in the payload
  // `script <arg>` (or an XCLA chunk): a `.class` path or a class name.
  bool has_script = false;
  std::string script_arg;
  bool script_is_inline = false;  // ICLA: class bytes in the payload (not loaded)
  bool has_lod_amp = false;
  float lod_amp = 0.f;
};
// Never null; `found` is false when the id is no typeof-8 record. The
// pointer stays valid for the life of the process (one desc per type, like
// the exe's one payload per type node).
const GameTypeDesc* gametype_desc(int32_t type_id);

// ---- Native objects -------------------------------------------------------
// The 16-byte fog block of a map (940 GroundRef.setFog 0x4DE640): u8 on,
// f32 near x 10, f32 far x 10, u32 colour (as the script passed it).
struct NativeFog {
  uint8_t on = 0;
  float near_x10 = 0.f;
  float far_x10 = 0.f;
  uint32_t colour = 0;
};

// The answer of a getinfo query (the exe's vt+0x3C writes through a pointer
// whose meaning depends on the query).
struct NativeInfo {
  float v[3] = {0.f, 0.f, 0.f};
  InvObject* obj = nullptr;
  int32_t i = 0;
  // In: the argument of a query that takes one (the exe passes it in the
  // place of the out pointer): the axis number of the player's 0x19 and
  // 0x43; the colour index of a part's 0x3C (0 = none: only read).
  int32_t arg = 0;
  // In (m6 step 3): the String GameRef.getInfo(ILString;)I passes where
  // the exe's handler reads its text: a part's or car's 0x28 ("%d %d")
  // and a part's 0x47 ("%d %d %d %d %f %f %f"). Null for getInfo(II)I.
  const char* arg_str = nullptr;
  // In (m6 step 3): the exe's block of a part's or car's 0x2F / 0x31 is
  // given (a host caller; GameRef.getInfo(II)I has the script's int in
  // its place, and no script asks these queries): `arg` is block[0] of
  // 0x2F (a slot id) or block[2] of 0x31 (the id of the previous entry, 0
  // for the first). Without a block both answer 0, as the exe does.
  bool block = false;
  // Query 0x4A of a ground (FUN_004bb570): with `fog_set` the block is
  // stored (the exe frees the old one and keeps the pointer); without, the
  // ground's block is copied into `fog` and the query is answered only when
  // the ground has one.
  const NativeFog* fog_set = nullptr;
  NativeFog fog;
};

struct NativeCreate {
  const GameTypeDesc* type = nullptr;
  // The node the object belongs to. GameRef.create: `handle` is the GameRef
  // the script called create on, `instance` the object create returns (the
  // script instance or its stand-in), `parent` the node it was made under.
  // Fork: the exe has one node and two handles on it; the host has two
  // objects and both name this one native object.
  // Pack instance (the map): `handle` is the handle whose request built it,
  // `instance` is null, `node_id` its resource id.
  InvObject* handle = nullptr;
  InvObject* instance = nullptr;
  InvObject* parent = nullptr;
  int32_t node_id = 0;
  const char* params = "";
};

// One per instance of a native type. The methods are the slots of the exe's
// type vftable, which all take the instance's game object.
class NativeObject {
 public:
  virtual ~NativeObject() = default;
  // vt+0x10. Called once, when the node goes (ResourceRef.destroy on either
  // object of a create, ResourceRef.unload of a pack instance).
  virtual void destroy() {}
  // vt+0x1C: the "ffffff" event GameRef.setMatrix sends. false = the type
  // does not take it and the host's generic GameRef pose is used.
  virtual bool set_matrix(const float /*pos*/[3], const float /*ypr*/[3]) {
    return false;
  }
  // vt+0x1C with all twelve floats GameRef.setState sends (0x4DBF20:
  // position, angles, velocity, angular velocity). A type that keeps no
  // velocities takes the first six as GameRef.setMatrix's event.
  virtual bool set_state(const float pos[3], const float ypr[3],
                         const float /*vel*/[3], const float /*ang*/[3]) {
    return set_matrix(pos, ypr);
  }
  // vt+0x2C, once per game tick.
  virtual void control(float /*dt*/) {}
  // vt+0x30. The exe calls it once per step of its physics world (0.02 s at
  // most). Fork: the host calls it once per frame with the frame dt and the
  // object cuts its own steps (the soft world keeps a 0.05 s substep).
  virtual void drive(float /*dt*/) {}
  // vt+0x34, once per main-loop pass (the exe's animate walk FUN_0047ba10,
  // with min(frame dt, 0.1)); only for an object that sets `animates`.
  virtual void animate(float /*dt*/) {}
  // vt+0x38: events; 0x10 = GameRef.command text. false = not taken, the
  // host's generic command chain runs.
  virtual bool event(int32_t /*type*/, const char* /*text*/) { return false; }
  // vt+0x3C. Queries used so far: 1 physics instance, 2 position,
  // 3 velocity, 7 the kind of object (9 a part, 5 a car), 0x17 the first
  // render instance (the ground's, a part's), 0x22 the camera's, 0x27 the
  // free slots and 0x2B the car of a part or car; of the player 0x19 and
  // 0x43 (a logical axis, NativeInfo::arg), of the cursor 2, 0x2A (mode),
  // 0x2C (hover object) and 0x3B (picked position).
  // GameRef.getPos is query 2 of the type (FUN_004e3890 -> FUN_004ea7a0 ->
  // FUN_0047c640(handle, 2, out)).
  virtual bool getinfo(int32_t /*query*/, NativeInfo* /*out*/) { return false; }
  // Fork: GameRef.getOri reads the rotation of the physics instance that
  // getinfo 1 returns (GameRef_GetWorldYpr); the host object answers itself.
  virtual bool get_ori(float /*ypr*/[3]) { return false; }
  // Fork: the position of that physics instance, for a type whose getinfo 2
  // is not the body's position (the ground: FUN_004bb570 case 2 writes
  // zeros, while the camera's LOOK reads the `groundbody` through getinfo
  // 1). false = getinfo 2 is the body's position.
  virtual bool get_body_pos(float /*pos*/[3]) { return false; }
  // PE 940 FUN_0051f970 for the handles a game object holds on other nodes:
  // the nodes with these ids (ascending) are destroyed, and a handle on one
  // of them reads id 0 from now on.
  virtual void nodes_gone(const std::vector<int32_t>& /*ids*/) {}

  // Set by a type's create when the object wants control() and drive()
  // (the exe: FUN_004ebd90(obj, 0x14, 0) sets the instance flags that put
  // it on the two callback lists). Read once, when the object is registered.
  bool ticks = false;
  // Set by a type's create when the object wants animate() (the exe:
  // getinfo 0x1C answers 1 and FUN_0047d560 puts the instance on the
  // animate list at GameRef.create). Read once, when the object is
  // registered.
  bool animates = false;
  // Set by a type's create when it took the instance pose from the params
  // string (the exe hands the string to vt+0x0C and nothing else reads it).
  // GameRef.create then skips the host's generic stand-in, which parses six
  // numbers and sends them through GameRef.setMatrix.
  bool owns_create_pose = false;

  int type_index = -1;
  InvObject* handle = nullptr;
  InvObject* instance = nullptr;
  InvObject* parent = nullptr;
  int32_t node_id = 0;
};

// GameRef.create on the script boot, after the node is bound and before the
// script constructor runs (940 FUN_00523c80: native vt+0x0C, THRD-CREATE,
// then vt+0x40). A type without a host implementation gets no object.
void native_object_create(const GameTypeDesc* type, InvObject* handle,
                          InvObject* instance, InvObject* parent,
                          const char* params);
// The native object behind a handle: the one made by GameRef.create for that
// object, else the game object of the pack instance the handle is bound to
// (any handle on the node finds it). Null when there is none.
std::shared_ptr<NativeObject> native_object_of(InvObject* handle);
// The hooks of the natives. Each returns false when the handle has no
// native object or the object does not take the call.
bool native_event(InvObject* handle, int32_t type, const char* text);
bool native_set_matrix(InvObject* handle, const float pos[3],
                       const float ypr[3]);
// GameRef.setState (0x4DBF20): the twelve floats to the native object.
bool native_set_state(InvObject* handle, const float pos[3], const float ypr[3],
                      const float vel[3], const float ang[3]);
bool native_get_pos(InvObject* handle, float pos[3]);   // getinfo 2
bool native_get_ori(InvObject* handle, float ypr[3]);
// True when the handle's native object took its pose from the create string.
bool native_owns_create_pose(InvObject* handle);
bool native_getinfo(InvObject* handle, int32_t query, NativeInfo* out);
void native_on_destroy(InvObject* handle);
// ResourceRef.unload of a node GameRef.create or create_native made: its
// game object goes (940 FUN_004ec800; the node stays).
void native_on_unload(InvObject* handle);
// Once per engine_simulate_frame.
void native_tick_control(float dt);
void native_tick_drive(float dt);
// PE 940 FUN_0047ba10(dt), for the native objects on the animate list, in
// creation order: once per main-loop pass, after the VM pump and before the
// script animate() calls (engine_dispatch_animate_events). The cursor is
// made at boot, before any Osd registers its animate(), so it ticks first
// as in the exe's one list.
// Fork: a call from inside a call returns at once (the exe has one call
// site in its one loop pass and no flag, M4 Q2).
void native_tick_animate(float dt);
// Fork (diagnostic): the clock of the script-boot loop, seconds after loop
// start: the clock of the key and mouse scripts. The loop sets it before
// each game tick; the `[camera] ctl` lines print it.
void native_set_loop_time(double seconds);
double native_loop_time();

// ---- Game instances (typeof 1) --------------------------------------------
// A POLY vertex as stored, 56 bytes (940 FUN_00519940 copy loop). Positions
// are metres. The ground mesh takes D3D diffuse = c0 & 0xFF000000 and
// D3D specular = (c0 & 0x00FFFFFF) | (c1 << 24) (FUN_004658b0).
struct InstancePolyVertex {
  float pos[3];
  float normal[3];
  uint32_t c0;
  uint32_t c1;
  float uv0[2];
  float uv1[2];
  float uv2[2];
};
static_assert(sizeof(InstancePolyVertex) == 56, "POLY vertex is 14 dwords");

struct InstancePolySection {
  int32_t node = 0;        // index into InstanceTree::nodes
  uint32_t material = 0;   // index into the material table of the render object
  std::vector<InstancePolyVertex> verts;
  std::vector<uint32_t> indices;  // three per triangle
};

struct InstanceTreeNode {
  uint32_t id = 0;         // resolved id (record +4 through the pack's remap)
  uint32_t superid = 0;    // record +0, as stored
  uint8_t restype = 0;     // record +8
  uint8_t rec_flags = 0;   // record +9, as stored (the exe ORs 8 into a nested node's)
  uint32_t offset = 0;     // file range of the node's payload
  uint32_t size = 0;
  std::string name;
  float bounds[12] = {};   // centre xyz, half extents xyz, six unused (flags bit 0)
  int32_t parent = -1;     // index, -1 for the root
  int32_t depth = 0;
  uint32_t child_count = 0;
  // The RSD text of the node: `gametype <id>` (pack-relative, resolved;
  // payload+0x38) and the `params` word (at most 2048 characters;
  // payload+0x48). The root of a map has `native ground`; in the big maps
  // thousands of nested nodes name a type of their own (city.rpk: 12,723).
  uint32_t gametype_local = 0;
  int32_t gametype_id = 0;
  bool has_params = false;
  std::string params;
  uint32_t poly_sections = 0;   // payload+0x30
  uint32_t phys_triangles = 0;  // payload+0x18 (the list itself is not kept: M6)
  bool ends_on_size = false;    // the walk stopped exactly at offset + size
};

struct InstanceTree {
  int32_t res_id = 0;
  std::vector<InstanceTreeNode> nodes;    // [0] the root, then depth first
  std::vector<InstancePolySection> poly;  // in walk order
  uint32_t poly_chunks = 0;
  uint32_t poly_vertices = 0;
  uint32_t poly_triangles = 0;
  uint32_t phys_chunks = 0;
  uint32_t phys_triangles = 0;
  uint32_t rsd_nodes = 0;    // nodes with a gametype of their own
  uint32_t bad_nodes = 0;    // nodes that did not end on their size
  uint32_t bad_chunks = 0;   // POLY / PHYS chunks whose content is not their size
};
// PE 940 FUN_00519940 for the pack record `res_id` and every node below it.
// False when the record is no typeof-1 pack entry or its payload is cut.
bool instance_tree_read(int32_t res_id, InstanceTree* out);

// The payload request that builds a pack instance's game object (940
// FUN_005195a0), once per node. Does nothing unless this is the script boot
// and `handle` is bound to a typeof-1 pack record. Returns true when the
// node is built (now or before).
bool instance_node_request(InvObject* handle);
// ResourceRef.unload of a pack instance: the game object is destroyed and
// the tree dropped; a later request builds it again.
void instance_node_unload(InvObject* handle);
// The tree of a built node; null when the node is not built. The pointer is
// good until the node is unloaded.
const InstanceTree* instance_node_tree(int32_t node_id);
// The handle whose request built the node (the script's `map` object): the
// parent every instance under the map names, so the root of the camera walk.
InvObject* instance_node_handle(int32_t node_id);

// ---- Nodes made by GameRef.create -----------------------------------------
// PE 940 FUN_00523c80 (create) and FUN_005240e0 (create_native) make a
// restype-1 node under the parent, with an id of its own: FUN_00518a40 takes
// a 16-bit number off the free list of the resource table (FUN_00468a70
// refills it 128 numbers at a time) and ORs 0xFFFF0000 into it. The handle
// create was called on and the script instance are two handles on that
// node, `new GameRef(id)` is one more, the script constructor <init>(I)
// gets that id, and getParentID is the id of the node it hangs under.
// (M2 spec Q12, read in Ghidra.)
// Fork: the host has one object per handle and no node. One of the two
// objects of a GameRef.create stands for the node: the script instance, or
// the handle create was called on where the type has no script class (the
// object the script holds then; the host's create returns a GameRef
// stand-in there, where the exe returns null). A table finds that node
// object, and the node's parent and type, from the id. The number comes from the
// allocator the host's LoadGameInit stand-in uses (the stand-in of
// FUN_00518a40, Resources_part4.inc): like the exe's it takes a number a
// destroyed node gave back before a fresh one, and it never hands out a
// number a live node has. Script boot only: on the legacy path every
// function here does nothing and an instance keeps the id of its type.
//
// The node of one GameRef.create: `object` is the node object, `other` the
// second host object of the create (the same object where there is one).
// Both get the id (the type id stays in ResState::type_id) and the parent's
// id. `parent` is the node object of the parent, `id` 0 takes a new number.
// Returns the id, 0 on the legacy path.
int32_t script_node_register(InvObject* object, InvObject* other,
                             InvObject* parent, int32_t type_id, int32_t id);
// PE 940 DAT_005fde50, the default root: GameRef.create, create_native and
// GameType.createNativeInstance hang a node made with a null parent under
// it, so getParentID of such a node is the root's id and never 0.
// Fork: a host object of its own that stands for that root, made on first
// use, with a node id like any other. Only its id is used: the host's
// parent links of a parentless object (which camera root it belongs to)
// stay as they were. Null on the legacy path.
InvObject* script_node_default_root();
// The node object of a script-made node id; null when the id names none.
InvObject* script_node_object(int32_t id);
// The type id of the script-made node a handle is on (the game type the
// node was created from; GameRef.getInfo(GII_TYPE)); 0 when the handle is
// on no such node, and always on the legacy path.
int32_t script_node_type_id(InvObject* handle);
// ResourceRef.destroy through any handle on a script-made node: the node
// leaves the table. `object` and `other` get the node's two host objects
// (`other` null when that handle was bound to something else since),
// `node_id` its id. False when `handle` is on no such node. The id stays
// reserved, and the handles on the node bound, until script_nodes_retire.
bool script_node_release(InvObject* handle, InvObject** object,
                         InvObject** other, int32_t* node_id);
// The ids of the live script-made nodes that hang under node `id` (the
// children the exe's TreeNode destructor FUN_00569890 deletes with a node).
// Works for an id that script_node_release just took out of the table.
void script_node_children(int32_t id, std::vector<int32_t>* out);
// PE 940 node destructor FUN_0051eca0, for every node that left the table
// since the last call: each handle on the node is unbound (FUN_0051f970:
// id 0, no node), the native objects drop the handles they hold on it
// (native_nodes_gone), and the number of the id goes back to the allocator
// (FUN_005173e0). Called when the outermost ResourceRef.destroy ends and
// before a new node id is taken; does nothing while a destroy is running.
void script_nodes_retire();
// The part of FUN_0051f970 that reaches the handles inside native game
// objects: every object is told the ids of the nodes that are gone
// (NativeObject::nodes_gone).
void native_nodes_gone(const std::vector<int32_t>& ids);
// The table half of gameref_node_set_parent. `node` is either host object
// of the node; `object` and `other` get both.
bool script_node_set_parent(InvObject* node, InvObject* parent,
                            InvObject** object, InvObject** other);
// PE 940 FUN_004ec210 for a game instance: the node is relinked under
// another node (GameRef.setParent, a part's `install`). `node` is either
// host object of the node, `parent` a node object. False when `node` is no
// script-made node (nothing is changed then).
// Fork: the renderer gets no transform parent from this. The pose of a game
// instance is a world pose (its body's); the link only says which camera
// root the instance belongs to (gameref_get_parent).
bool gameref_node_set_parent(InvObject* node, InvObject* parent);
// The object that stands for the node a handle is bound to: the node object
// of a script-made node, the handle whose request built a pack instance
// (the script's `map` object), else the handle itself. Every native that
// takes a handle as parent or as receiver starts with this, so that `new
// GameRef(id)` acts on the node as it does in the exe.
InvObject* gameref_node_object(InvObject* handle);

// ---- Render objects (typeof 14) -------------------------------------------
// What the host uses of a typeof-14 payload (940 FUN_00522d90,
// `ResourceType_Render_Object_Data`). Ids are resolved through the record's
// pack (FUN_0051e860: high word = dependency slot).
struct RenderObjectDesc {
  bool found = false;   // the id names a typeof-14 pack record
  int32_t id = 0;
  int32_t mesh_id = 0;  // `mesh` (payload+0x0C); 0 = none
  // `flags` (payload+0x34) with the class bits the `particle`, `skidmark`
  // and `welding` lines set and the 0x10000 / 0x20000 / 0x40000 bits of the
  // shd_* lines.
  uint32_t flags = 0;
  // Every `texture` line in file order (payload+0x20 count, +0x24
  // handles); 0 = a line without a texture (`texture 0x00000000`). The
  // index is the slot a material's texture entry names (FUN_00441c40).
  std::vector<int32_t> textures;
};
// Never null; `found` is false when the id is no typeof-14 record. One desc
// per record for the life of the process.
const RenderObjectDesc* render_object_desc(int32_t id);

// The template class the exe makes for a render object (940 FUN_0046d890 ->
// FUN_0046d9c0), chosen by its flags: 0x4 text, 0x100 particle, 0x2000
// skidmark, 0x100000 welding, 0x800 ground, else object. With the ground
// class the `mesh` is a material table and the geometry comes from the
// POLY chunks of the map instance.
enum RenderTemplateClass : int {
  kRenderTemplateGround = 0,
  kRenderTemplateObject = 2,
  kRenderTemplateText = 3,
  kRenderTemplateParticle = 4,
  kRenderTemplateSkidmark = 5,
  kRenderTemplateWelding = 6,
};
int render_template_class(uint32_t flags);

// The texture handles of a render object, one entry per `texture` line
// (null for an empty line); lines naming the same record share a handle.
// Bound and loaded on the first call. Main thread (camera pass).
std::vector<InvObject*> render_object_textures(int32_t id);
// The renderer keys of those textures, by slot: null for an empty line and
// for a texture that did not load, so a material entry that names such a
// slot stays unbound (940 FUN_00441c40). `missing` (optional) gets the
// number of named textures that did not load and `not_loaded` (optional) a
// " <slot>:0x<id>" item for each.
std::vector<void*> render_object_texture_keys(int32_t id, int* missing,
                                              std::string* not_loaded);

// ---- Light types (typeof 13) ----------------------------------------------
struct RenderLightDesc;  // render_d3d9.hpp
// The values of a light type record: the text payload read by 940
// FUN_005219c0 over the defaults of FUN_005213c0. Null when the id is no
// typeof-13 pack record. One per record for the life of the process.
const RenderLightDesc* light_type_payload(int32_t id);
// RenderRef.setLight(diffuse, ambient, specular) on a light type (940
// FUN_004e29f0 -> FUN_004ebef0): rgb / 256 into the Diffuse, Ambient and
// Specular of the payload; the scene lights made from the type follow.
// False when the id is no light type.
bool light_type_set_colours(int32_t id, int32_t diffuse, int32_t ambient,
                            int32_t specular);
// ResourceRef.duplicate of a light type (940 0x4E32E0 -> FUN_004e9470 ->
// FUN_0051ee30): `copy_id` names a new runtime node with a copy of the
// payload of `src_id`, which stays as it is. False when `src_id` is no light
// type (record or copy).
bool light_type_duplicate(int32_t src_id, int32_t copy_id);
// The pack record a light type copy was made from (through copies of
// copies); 0 when `id` is no copy.
int32_t light_type_source(int32_t id);
// RenderRef.setFlare on a light type (940 0x4E2950 -> FUN_004ebe00): the
// five lens flare fields and the glow texture into the payload. False when
// the id is no light type.
bool light_type_set_flare(int32_t id, int32_t texture_id, int32_t colour,
                          float min_size, float max_size, int32_t flare_count,
                          int32_t ray_count);

// ---- The ground mesh ------------------------------------------------------
// The pack node whose ground object owns this render instance (`r_ground`,
// ground+0x68); 0 when it is no ground's render instance.
int32_t ground_node_of_render_instance(InvObject* render_instance);
// Builds the ground mesh of a map in the renderer, keyed by the ground's
// render instance: the material table of the render object, the POLY
// sections of the instance tree, textures by slot. Called by the camera
// pass when a camera first sees the instance
// (renderref_materialise_pending). Returns 1 when the mesh is built, 0 when
// `render_instance` is no ground's render instance or the render object has
// no ground template, -1 when the material table could not be loaded.
int ground_mesh_build(InvObject* render_instance, int32_t render_object_id);

}  // namespace inv
