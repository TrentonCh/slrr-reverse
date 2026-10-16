// Fragment 10 of NativeTypes.cpp - included, not compiled on its own.
//
// M5 step 3: the `object` type, row 2 of the native type table
// (`_type_object` 0x59AC60, factory FUN_004bf190, which keeps the cfg
// buffer as the part type's factory does). An object is a prop: physics
// bodies made from its `body` lines and render instances `obj_r_o` made
// from its `render` and `light` lines, which follow the reference body. The
// trade-in stand (misc.dealer3:0x32 `stand.cfg`, CarMarket.java:388-395) is
// one, made by GameRef.create_native through MapObject (MapObject.java:
// 30-75): no script instance; GameRef.setMatrix and setState pose it.
//
// Mirrors:
//   init     FUN_004be2b0 (vt+0x04): FUN_004be370 (reset), the line parser
//            FUN_004f6240 with the keyword table 0x5DAAE0 (20 words; a word
//            not in it is read past, `bounds` among them), FUN_004be330
//            (a compound when more than one body)
//   words    FUN_004bbe50 body, FUN_004bc650 render / light, FUN_004bc940
//            nocollision, FUN_004bca00 damage, FUN_004bba10 flags,
//            FUN_004bba70 lod, FUN_004bbb00 lods, FUN_004bbbb0 colors,
//            FUN_004bbcb0 rendertypes; the other words of the table
//            (trigger, part, friction, buoy, material, powerup, init,
//            basetexture, sfx_loop, sfx_random) are read past here
//   create   FUN_004bd610 -> FUN_004bd040 (vt+0x0C), FUN_004bdb90 (the
//            render instances of a level)
//   pose     FUN_004befc0 (vt+0x1C, "ffffffffffff") -> FUN_004eac50 (the
//            node's physics instance to the pose)
//   event    FUN_004be600 (vt+0x38), getinfo FUN_004be540 (vt+0x3C),
//            destroy FUN_004be3f0 (vt+0x10)
//
// Where the meshes hang (decompile and asm of FUN_004bd040, FUN_004bdb90,
// FUN_004befc0, FUN_004eac50, FUN_004e9730, FUN_004f7d60; notes/m5/gh q11,
// q12, q25, q26 and m4/gh q07):
//   - create puts the body of a `body` line at  create position + R x line
//     offset, turned by R x line rotation: no shape centre is added there
//     (the part type adds one, FUN_004c1f90);
//   - bone00 of a render instance follows the reference body (the first
//     `body` line, type+0x8B0) at the offset
//         R_body^T x (render offset - body offset) - shapeCentre,
//     shapeCentre = *(*(entry+0x40) + 0x90) + 0x40: the shape a
//     rigidbody_type keeps at +0x90 (FUN_004f7d60), whose +0x40 is the
//     centroid of the closed surface of a mesh body with a mass (FUN_004f5b90
//     and FUN_00505760, M2 Q9; part_body_shape computes it). Without a body
//     line the instance stands at create pose x entry pose and follows
//     nothing (FUN_004ec470 with no node);
//   - set_matrix moves the node's physics instance, the body, to the pose
//     it is given (FUN_004eac50: pose minus R x the offset FUN_004e9730
//     gathers up a compound, which is 0 for a body that is no compound
//     child). An object without a body has no physics instance and does
//     not move.
//   The stand: one 5000 kg mesh body 0.26 m above the object origin, one
//   render instance at the origin; setMatrix((15.9, 0.5, 8.3), 0) puts the
//   body there and the origin of `stand.SCX` 0.26 m plus the centroid's
//   height of `_phys_stand.SCX` below it, which brings the top of the stand
//   to the height the cars on it stand at. (M5 spec Q12, read here; the
//   spec's stand-in was a centre of 0.)
//
// Fork, in short (each is marked again where it applies):
//   - no physics: the bodies are poses; the velocities are stored, not
//     simulated; nothing wakes or falls asleep; `nocollision`, the mass and
//     the shape kind are kept and not used;
//   - a compound (two or more body lines) is posed as its reference body
//     alone (no object of the M5 scenes has two);
//   - the level of detail is chosen at create from the detail the node
//     inherits (part_init's rule: 0 under a map) and does not change later
//     (vt+0x18 FUN_004bcf40 is not ported; no object of the M5 scenes has a
//     `lods` line);
//   - commands (event 0x10) are answered and ignored as in the exe; damage,
//     pickups, triggers, sounds and the per-frame tick are not ported.

namespace {

// One 0x74-byte entry of the array at type+0x128 (at most 16, count
// type+0x868).
struct ObjectEntry {
  int kind = 0;                 // +0x3C: 1 body, 3 render / light
  CamV3 off;                    // +0x00
  float ypr[3] = {0.f, 0.f, 0.f};
  CamM3 rot = cam_identity();   // +0x0C, FUN_005666d0 of the angles
  // +0x58: `flags` & 0x1FF and the level range, first in bits 16..19 and
  // last in bits 20..23; a `render` / `light` line sets the range 0..15.
  uint32_t flags = 0;
  // kind 3: the handle +0x48 on the render object (typeof 14) or the light
  // type (typeof 13); `colors` +0x5C / +0x60; `rendertypes` +0x64 / +0x68.
  int32_t res_id = 0;
  bool is_light = false;
  std::vector<int32_t> colors;
  std::vector<int32_t> rendertypes;
  // kind 1: the rigidbody_type +0x40 (mass, shape kind, the shape's centre).
  PartEntry shape;
  bool nocollision = false;     // shape flag 8 (FUN_004bc940)
};

// What the type's init leaves (type+0x128 .. +0x980).
struct ObjectTypeDesc {
  int32_t type_id = 0;
  std::string cfg_path;
  bool cfg_found = false;
  std::vector<ObjectEntry> entries;
  int ref_index = -1;          // type+0x8B0: the entry of the first `body`
  bool compound = false;       // type+0x8B4 (FUN_004be330)
  uint32_t type_flags = 1;     // type+0x8B8: bit 0 the body sleeps at create
  float damage_a = -1.f;       // type+0x8BC
  float damage_b = -1.f;       // type+0x8C0: the health at create
  int lod_count = 0;           // type+0x86C, after the `lods` tail
  std::vector<float> lod_t;    // type+0x870
  int bodies = 0;
  int renders = 0;
  int read_past = 0;           // lines whose word this type does not read
  int unported_shapes = 0;     // part_body_shape's counts
  int missing_shapes = 0;
};

std::unordered_map<int32_t, std::unique_ptr<ObjectTypeDesc>> g_object_types;

void parse_object_cfg(const std::string& text, int32_t pack,
                      ObjectTypeDesc* d) {
  PartTypeDesc counts;  // part_body_shape counts unported / missing shapes
  part_cfg_lines(text, [&](const PartCfgLine& l) {
    const size_t n = d->entries.size();
    ObjectEntry* last = n ? &d->entries.back() : nullptr;
    if (word_is(l.word, "body")) {
      // FUN_004bbe50, "%f %f %f %f %f %f %f %s %f %f %f": offset, angles,
      // mass, shape (a body record by "%d", `ball` / `sphere`, `box`,
      // `rodx` / `rody` / `rodz`, else a mesh file), three sizes. The first
      // body line is the reference entry. Fork: an entry whose shape the exe
      // could not make (a record that names no body) is kept.
      if (n > 15) return;
      if (d->ref_index < 0) d->ref_index = static_cast<int>(n);
      ObjectEntry e;
      e.kind = 1;
      float mass = 0.f;
      float dims[3] = {0.f, 0.f, 0.f};
      char shape[1025];
      shape[0] = 0;
      engine_sscanf(l.rest, "%f %f %f %f %f %f %f %1024s %f %f %f", &e.off.x,
                    &e.off.y, &e.off.z, &e.ypr[0], &e.ypr[1], &e.ypr[2], &mass,
                    shape, &dims[0], &dims[1], &dims[2]);
      e.rot = cam_ypr_to_matrix(e.ypr[0], e.ypr[1], e.ypr[2]);
      e.shape.dims[0] = dims[0];
      e.shape.dims[1] = dims[1];
      e.shape.dims[2] = dims[2];
      part_body_shape(shape, mass, &e.shape, &counts);
      d->entries.push_back(e);
      ++d->bodies;
    } else if (word_is(l.word, "render") || word_is(l.word, "light")) {
      // FUN_004bc650, "%x %f %f %f %f %f %f": the id (local to the cfg's
      // pack, FUN_0051e860) with the 0x prefix the scanner's %x needs,
      // offset, angles. Bound to a typeof-14 record unless the line starts
      // with `light`; a typeof-13 light type when that fails.
      if (n > 15) return;
      ObjectEntry e;
      e.kind = 3;
      int32_t local = 0;
      engine_sscanf(l.rest, "%x %f %f %f %f %f %f", &local, &e.off.x, &e.off.y,
                    &e.off.z, &e.ypr[0], &e.ypr[1], &e.ypr[2]);
      e.rot = cam_ypr_to_matrix(e.ypr[0], e.ypr[1], e.ypr[2]);
      const int32_t id = local ? static_cast<int32_t>(rpak_remap_local_id(
                                     pack, static_cast<uint32_t>(local)))
                               : 0;
      const RpakEntry* re = id ? rpak_find_entry(id) : nullptr;
      const bool as_light = l.starts_line && word_is(l.word, "light");
      if (!as_light && re && re->restype == 14) {
        e.res_id = id;
      } else if (re && re->restype == 13) {
        e.res_id = id;
        e.is_light = true;
      }
      e.flags = (e.flags & ~0xF0000u) | 0xF00000u;  // levels 0..15
      d->entries.push_back(e);
      ++d->renders;
    } else if (word_is(l.word, "nocollision")) {
      // FUN_004bc940: shape flag 8 on the entry before the line.
      if (last && last->kind == 1) last->nocollision = true;
    } else if (word_is(l.word, "damage")) {
      // FUN_004bca00, "%f %f": type+0x8BC = a, +0x8C0 = b, or a for b 0.
      float a = 0.f, b = 0.f;
      engine_sscanf(l.rest, "%f %f", &a, &b);
      d->damage_a = a;
      d->damage_b = (b == 0.f) ? a : b;
    } else if (word_is(l.word, "flags")) {
      // FUN_004bba10, "%d": OR'd into the entry before the line, & 0x1FF.
      if (last) {
        int32_t v = 0;
        engine_sscanf(l.rest, "%d", &v);
        last->flags |= static_cast<uint32_t>(v) & 0x1ffu;
      }
    } else if (word_is(l.word, "lod")) {
      // FUN_004bba70, "%d %d": the level range of the entry before the line.
      if (last) {
        int32_t a = 0, b = 0;
        engine_sscanf(l.rest, "%d %d", &a, &b);
        last->flags = (last->flags & ~0xf0000u) |
                      ((static_cast<uint32_t>(a) << 16) & 0xf0000u);
        last->flags = (last->flags & ~0xf00000u) |
                      ((static_cast<uint32_t>(b) << 20) & 0xf00000u);
      }
    } else if (word_is(l.word, "lods")) {
      // FUN_004bbb00: "%#%d" into type+0x86C, that many "%#%f" behind it;
      // the tail as the part type's (FUN_004c0880): a last number below 1.0
      // gets a 1.0 behind it, else the count drops by one.
      int32_t count = 0;
      const char* p = l.rest;
      p += engine_sscanf(p, "%#%d", &count);
      std::vector<float> t;
      for (int32_t i = 0; i < count && i < 16; ++i) {
        float v = 0.f;
        const int used = engine_sscanf(p, "%#%f", &v);
        if (used <= 0) break;
        p += used;
        t.push_back(v);
      }
      d->lod_t = t;
      d->lod_count = static_cast<int>(t.size());
      if (d->lod_count > 0) {
        if (t.back() < 1.f)
          d->lod_t.push_back(1.f);
        else
          --d->lod_count;
      }
    } else if (word_is(l.word, "colors")) {
      // FUN_004bbbb0: "%#%d", that many "%#%d" colours of the entry before
      // the line.
      if (last) {
        int32_t count = 0;
        const char* p = l.rest;
        p += engine_sscanf(p, "%#%d", &count);
        last->colors.clear();
        for (int32_t i = 0; i < count && i < 4096; ++i) {
          int32_t v = 0;
          const int used = engine_sscanf(p, "%#%d", &v);
          if (used <= 0) break;
          p += used;
          last->colors.push_back(v);
        }
      }
    } else if (word_is(l.word, "rendertypes")) {
      // FUN_004bbcb0: "%#%d", that many "%#%d" ids, each local to the cfg's
      // pack and bound to a typeof-14 record: the render types the colour
      // index chooses from (FUN_004bdb90).
      if (last) {
        int32_t count = 0;
        const char* p = l.rest;
        p += engine_sscanf(p, "%#%d", &count);
        last->rendertypes.clear();
        for (int32_t i = 0; i < count && i < 4096; ++i) {
          int32_t v = 0;
          const int used = engine_sscanf(p, "%#%d", &v);
          if (used <= 0) break;
          p += used;
          const int32_t id = v ? static_cast<int32_t>(rpak_remap_local_id(
                                     pack, static_cast<uint32_t>(v)))
                               : 0;
          const RpakEntry* re = id ? rpak_find_entry(id) : nullptr;
          last->rendertypes.push_back((re && re->restype == 14) ? id : 0);
        }
      }
    } else {
      ++d->read_past;
    }
  });
  d->unported_shapes = counts.unported_shapes;
  d->missing_shapes = counts.missing_shapes;
  // FUN_004be330: a compound when more than one entry is a body (kind 1 or
  // 2; the host makes kind 1 only).
  d->compound = d->bodies > 1;
}

// The type's init (vt+4 FUN_004be2b0), once per type.
const ObjectTypeDesc* object_type_desc(const GameTypeDesc* type) {
  static const ObjectTypeDesc kNone;
  if (!type || !type->found) return &kNone;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    auto it = g_object_types.find(type->type_id);
    if (it != g_object_types.end()) return it->second.get();
  }
  auto d = std::make_unique<ObjectTypeDesc>();
  d->type_id = type->type_id;
  d->cfg_path = type->cfg_path;
  std::string text;
  if (type->cfg_is_inline) {
    text = type->cfg_inline;
    d->cfg_found = true;
  } else {
    std::vector<uint8_t> file;
    if (part_read_file(type->cfg_path, &file)) {
      text.assign(reinterpret_cast<const char*>(file.data()), file.size());
      d->cfg_found = true;
    }
  }
  if (d->cfg_found)
    parse_object_cfg(text, rpak_id_pack(type->type_id), d.get());
  if (native_type_log()) {
    const RpakEntry* e = rpak_find_entry(type->type_id);
    std::fprintf(stderr,
                 "[native-type] object type 0x%08X '%s' cfg '%s'%s: %zu "
                 "entries (%d render, %d body, reference %d%s), damage %g / "
                 "%g, lods %d, %d line(s) read past%s%s\n",
                 static_cast<unsigned>(type->type_id),
                 e ? e->name.c_str() : "", d->cfg_path.c_str(),
                 d->cfg_found ? "" : " NOT FOUND", d->entries.size(),
                 d->renders, d->bodies, d->ref_index,
                 d->compound ? ", compound" : "", d->damage_a, d->damage_b,
                 d->lod_count, d->read_past,
                 d->unported_shapes ? ", body shape record not ported" : "",
                 d->missing_shapes ? ", body mesh NOT LOADED" : "");
    for (const ObjectEntry& en : d->entries) {
      if (en.kind == 1)
        std::fprintf(stderr,
                     "[native-type]   body (%g, %g, %g) ypr (%g, %g, %g) "
                     "mass %g shape kind %d '%s' centre (%.4f, %.4f, %.4f)"
                     "%s\n",
                     en.off.x, en.off.y, en.off.z, en.ypr[0], en.ypr[1],
                     en.ypr[2], en.shape.mass, en.shape.shape_kind,
                     en.shape.shape_path.c_str(), en.shape.com.x,
                     en.shape.com.y, en.shape.com.z,
                     en.nocollision ? " nocollision" : "");
      else
        std::fprintf(stderr,
                     "[native-type]   %s 0x%08X (%g, %g, %g) ypr (%g, %g, "
                     "%g) flags 0x%X colors %zu rendertypes %zu\n",
                     en.is_light ? "light" : "render",
                     static_cast<unsigned>(en.res_id), en.off.x, en.off.y,
                     en.off.z, en.ypr[0], en.ypr[1], en.ypr[2],
                     static_cast<unsigned>(en.flags), en.colors.size(),
                     en.rendertypes.size());
    }
  }
  std::lock_guard<std::mutex> lock(g_nt_mu);
  auto& slot = g_object_types[type->type_id];
  if (!slot) slot = std::move(d);
  return slot.get();
}

// ---------------------------------------------------------------------------
// The instance (0x1A0 bytes, FUN_004877f0)
// ---------------------------------------------------------------------------
class ObjectObject final : public NativeObject {
 public:
  const ObjectTypeDesc* type_ = nullptr;
  // Fork: the renderer key the render instances hang under: the node
  // object (for create_native the handle it was called on). Its transform
  // is the frame bone00 of the instances is posed in: the reference body,
  // or for a type without a body the object's create pose.
  InvObject* key_ = nullptr;
  CamV3 obj_pos_;                      // obj+0x1C
  float obj_ypr_[3] = {0.f, 0.f, 0.f};  // obj+0x28
  CamM3 obj_rot_ = cam_identity();
  int32_t colour_index_ = 0;           // obj+0x34
  // The node's physics instance (obj+0x38 / +0x44 / +0x48): the body of
  // the reference entry. Fork: a pose, no body.
  bool has_body_ = false;
  CamV3 body_pos_;
  CamM3 body_rot_ = cam_identity();
  float body_ypr_[3] = {0.f, 0.f, 0.f};
  // The two velocity buffers of the root body (FUN_004befc0). Fork:
  // stored, not simulated.
  float vel_[3] = {0.f, 0.f, 0.f};
  float ang_[3] = {0.f, 0.f, 0.f};
  float health_ = 0.f;                 // obj+0x58
  int level_ = 0;
  struct Inst {
    InvObject* r = nullptr;  // a handle of the list obj+0x5C (count +0x19C)
    int entry = 0;           // obj+0x15C
    int32_t res_id = 0;
    CamV3 at;                // bone00 in the frame of key_
    CamM3 rot = cam_identity();
  };
  std::vector<Inst> insts_;
  bool gone_ = false;
  double pose_logged_ = -1e9;  // Fork (diagnostic): the last pose line
  bool posed_ = false;         // a pose came after the create

  void push_pose() {
    if (has_body_)
      render_d3d9_mesh_set_transform(key_, body_pos_.x, body_pos_.y,
                                     body_pos_.z, body_ypr_[0], body_ypr_[1],
                                     body_ypr_[2], 1.f, 1.f, 1.f);
    else
      render_d3d9_mesh_set_transform(key_, obj_pos_.x, obj_pos_.y, obj_pos_.z,
                                     obj_ypr_[0], obj_ypr_[1], obj_ypr_[2],
                                     1.f, 1.f, 1.f);
  }
  // Where bone00 of an instance is in the world.
  CamV3 inst_world(const Inst& i, CamM3* rot) const {
    const CamV3& p = has_body_ ? body_pos_ : obj_pos_;
    const CamM3& r = has_body_ ? body_rot_ : obj_rot_;
    if (rot) *rot = cam_mm(r, i.rot);
    return p + cam_mul(r, i.at);
  }
  void log_pose(bool force);

  // vt+0x10 FUN_004be3f0: the handles of the bodies and of every render
  // instance are released (FUN_004e9450), which takes the instances.
  void destroy() override {
    if (gone_) return;
    gone_ = true;
    for (Inst& i : insts_) {
      if (!i.r) continue;
      java_util_resource_ResourceRef_destroy(i.r);
      java_util_resource_ResourceRef_deleteNative(i.r);
      i.r = nullptr;
    }
    insts_.clear();
  }

  bool set_matrix(const float pos[3], const float ypr[3]) override {
    // GameRef.setMatrix sends "ffffff": the scan of FUN_004befc0 reads
    // zeros for the velocities.
    const float zero[3] = {0.f, 0.f, 0.f};
    return set_state(pos, ypr, zero, zero);
  }

  // vt+0x1C FUN_004befc0, "ffffffffffff": FUN_004eac50 puts the node's
  // physics instance at the pose; the velocity and the angular velocity go
  // into both buffers of the root body, which wakes when either square
  // exceeds 1e-4 (DAT_005938c4). Without a body there is no physics
  // instance and nothing moves (the event is still the type's).
  bool set_state(const float pos[3], const float ypr[3], const float vel[3],
                 const float ang[3]) override {
    if (gone_ || !has_body_) return true;
    body_pos_ = cam_v(pos[0], pos[1], pos[2]);
    body_rot_ = cam_ypr_to_matrix(ypr[0], ypr[1], ypr[2]);
    for (int k = 0; k < 3; ++k) {
      body_ypr_[k] = ypr[k];
      vel_[k] = vel[k];
      ang_[k] = ang[k];
    }
    push_pose();
    log_pose(!posed_);
    posed_ = true;
    return true;
  }

  // vt+0x3C FUN_004be540: 1 the body's node handle (obj+0x44), 6 the type
  // (type+0x0C), 7 the kind (1; 8 with type+0x930, the `part` word, not
  // ported), 15 the health in per cent of `damage` a; every other query 0.
  // Fork: query 1 gives the object's own node (no physics instance node);
  // query 2 (GameRef.getPos, which the type does not answer and the
  // generic channel FUN_0047e450, not read, answers from the physics
  // instance) gives the body's position, as get_ori its rotation.
  bool getinfo(int32_t query, NativeInfo* out) override {
    switch (query) {
      case 1:
        out->obj = key_;
        return key_ != nullptr;
      case 2: {
        const CamV3& p = has_body_ ? body_pos_ : obj_pos_;
        out->v[0] = p.x;
        out->v[1] = p.y;
        out->v[2] = p.z;
        return true;
      }
      case 6:
        out->i = type_->type_id;
        return true;
      case 7:
        out->i = 1;
        return true;
      case 15:
        out->i = type_->damage_a != 0.f
                     ? static_cast<int32_t>(health_ * 100.f /  // DAT_0059385c
                                            type_->damage_a)
                     : 0;
        return true;
      default:
        return false;
    }
  }
  bool get_ori(float ypr[3]) override {
    const float* y = has_body_ ? body_ypr_ : obj_ypr_;
    ypr[0] = y[0];
    ypr[1] = y[1];
    ypr[2] = y[2];
    return true;
  }

  // vt+0x38 FUN_004be600: event 0x10 (a command string) scans its first
  // word and does nothing with it; answered. Fork: the damage (4), pickup
  // and timer events are not ported.
  bool event(int32_t type, const char* /*text*/) override {
    return type == 0x10;
  }
};

void ObjectObject::log_pose(bool force) {
  if (!native_type_log()) return;
  const double t = native_loop_time();
  if (!force && t - pose_logged_ < 1.0) return;
  pose_logged_ = t;
  char insts[512];
  insts[0] = 0;
  size_t used = 0;
  for (const Inst& i : insts_) {
    CamM3 r = cam_identity();
    const CamV3 w = inst_world(i, &r);
    float y = 0.f, p = 0.f, ro = 0.f;
    cam_matrix_to_ypr(r, &y, &p, &ro);
    const int k = std::snprintf(
        insts + used, sizeof(insts) - used,
        " inst 0x%08X at (%.4f, %.4f, %.4f) ypr (%.5f, %.5f, %.5f) mesh=%d",
        static_cast<unsigned>(i.res_id), w.x, w.y, w.z, y, p, ro,
        render_d3d9_mesh_ready(i.r) ? 1 : 0);
    if (k <= 0 || static_cast<size_t>(k) >= sizeof(insts) - used) break;
    used += static_cast<size_t>(k);
  }
  std::fprintf(stderr,
               "[native-type] object pose %p node=0x%08X t=%.3f body=(%.4f, "
               "%.4f, %.4f) ypr=(%.5f, %.5f, %.5f) vel=(%.3f, %.3f, %.3f) "
               "ang=(%.3f, %.3f, %.3f)%s\n",
               static_cast<void*>(key_),
               static_cast<unsigned>(java_util_resource_ResourceRef_id(key_)),
               t, body_pos_.x, body_pos_.y, body_pos_.z, body_ypr_[0],
               body_ypr_[1], body_ypr_[2], vel_[0], vel_[1], vel_[2], ang_[0],
               ang_[1], ang_[2], insts);
}

// PE 940 FUN_00524840(obj, handle, "obj_r_o", 0): a render instance under
// the object's node, bone00 at `at` / `rot` in the frame of key_.
// Fork: a host RenderRef with its own id under key_ in the renderer's
// tree; it waits on the pending list like every instance whose type is not
// loaded (part_new_render_instance).
InvObject* object_new_render_instance(ObjectObject* o, int32_t res_id,
                                      const CamV3& at, const CamM3& rot) {
  InvObject* r = tree_host_new("java.util.resource.RenderRef");
  if (!r) return nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& rs = R(r);
    rs.id = resref_next_runtime_id();
    rs.type = 3;  // INSTANCE_RENDER
    rs.restype = 3;
    rs.type_id = res_id;
    rs.parent = o->key_;
    rs.alias = "obj_r_o";
    rs.loaded = true;
  }
  tree_field_set_obj(r, "alias", string_new("obj_r_o"));
  float yaw = 0.f, pitch = 0.f, roll = 0.f;
  if (!part_is_identity(rot)) cam_matrix_to_ypr(rot, &yaw, &pitch, &roll);
  render_d3d9_mesh_set_tree_parent(r, o->key_);
  render_d3d9_mesh_set_transform(r, at.x, at.y, at.z, yaw, pitch, roll, 1.f,
                                 1.f, 1.f);
  render_d3d9_mesh_set_pending_type(r, res_id);
  return r;
}

// PE 940 FUN_004bdb90(obj, level), at create: one render instance per
// `render` / `light` entry whose level range holds the level. An entry with
// flag 4 is a light state: without flag 0x40 it is made only at night, with
// it only by day, night being the world clock outside 6:00..21:00
// (`DAT_006024c0 - 0x5460 > 53999` unsigned; the host's clock of
// System.syncGameTime). The render type is the entry's, or with
// `rendertypes` the one the colour index picks; with `colors` the instance
// gets colors[index % count] (FUN_004ebd20).
void object_make_render_instances(ObjectObject* o, int level) {
  const ObjectTypeDesc& d = *o->type_;
  const uint32_t tod = static_cast<uint32_t>(time_game_day_seconds());
  const bool night = (tod - 0x5460u) > 53999u;
  const ObjectEntry* ref =
      (d.ref_index >= 0 && d.ref_index < static_cast<int>(d.entries.size()) &&
       d.entries[static_cast<size_t>(d.ref_index)].kind == 1)
          ? &d.entries[static_cast<size_t>(d.ref_index)]
          : nullptr;
  for (size_t i = 0; i < d.entries.size(); ++i) {
    const ObjectEntry& e = d.entries[i];
    if (e.kind != 3) continue;
    const int first = static_cast<int>((e.flags >> 16) & 0xfu);
    const int last = static_cast<int>((e.flags >> 20) & 0xfu);
    if (level < first || last < level) continue;
    if ((e.flags & 4u) != 0 && ((e.flags & 0x40u) != 0) == night) continue;
    int32_t res = e.res_id;
    if (!e.rendertypes.empty()) {
      const int32_t n = static_cast<int32_t>(e.rendertypes.size());
      res = e.rendertypes[static_cast<size_t>(o->colour_index_ % n)];
    }
    if (!res) continue;  // the handle names nothing
    CamV3 at;
    CamM3 rot = cam_identity();
    if (!ref) {
      // FUN_004ec470(bone00, no node, object pose x entry pose): key_
      // carries the object pose.
      at = e.off;
      rot = e.rot;
    } else {
      // R_body^T x (render offset - body offset) - shapeCentre, following
      // the body; the rotation as the part type composes it
      // (part_render_local: entry rotation x body rotation^T).
      const CamM3 bt = part_transpose(ref->rot);
      at = cam_mul(bt, e.off - ref->off) - ref->shape.com;
      rot = cam_mm(e.rot, bt);
    }
    InvObject* r = object_new_render_instance(o, res, at, rot);
    if (!r) continue;
    if (!e.colors.empty()) {
      const int32_t n = static_cast<int32_t>(e.colors.size());
      render_d3d9_mesh_set_color(
          r, e.colors[static_cast<size_t>(o->colour_index_ % n)]);
    }
    ObjectObject::Inst inst;
    inst.r = r;
    inst.entry = static_cast<int>(i);
    inst.res_id = res;
    inst.at = at;
    inst.rot = rot;
    o->insts_.push_back(inst);
  }
}

// PE 940 FUN_004bd040 (type vt+0x0C through FUN_004bd610).
void object_init(ObjectObject* o, const NativeCreate& c) {
  o->type_ = object_type_desc(c.type);
  const ObjectTypeDesc& d = *o->type_;
  // "%f,%f,%f,%f,%f,%f,%d": position, angles, colour index; the index is -1
  // before the scan and a negative one becomes (int)(x + y + z) &
  // 0x7FFFFFFF (a sum out of the int range gives the CRT's 0x80000000).
  float pos[3] = {0.f, 0.f, 0.f};
  float ypr[3] = {0.f, 0.f, 0.f};
  int32_t colour = -1;
  engine_sscanf(c.params ? c.params : "", "%f,%f,%f,%f,%f,%f,%d", &pos[0],
                &pos[1], &pos[2], &ypr[0], &ypr[1], &ypr[2], &colour);
  if (colour < 0) {
    const float s = pos[0] + pos[1] + pos[2];
    const int32_t v = (s > -2147483648.f && s < 2147483648.f)
                          ? static_cast<int32_t>(s)
                          : static_cast<int32_t>(0x80000000u);
    colour = v & 0x7fffffff;
  }
  o->colour_index_ = colour;
  o->obj_pos_ = cam_v(pos[0], pos[1], pos[2]);
  o->obj_rot_ = cam_ypr_to_matrix(ypr[0], ypr[1], ypr[2]);
  for (int k = 0; k < 3; ++k) o->obj_ypr_[k] = ypr[k];
  o->key_ = c.instance ? c.instance : c.handle;
  // The level: the first threshold of `lods` above the node's detail, else
  // their count; 0 without the line. The new node has the detail of the
  // node it is made under (FUN_00523c80 / FUN_005240e0). Fork: as
  // part_init, known only for a part or car parent; 0 otherwise.
  float detail = 0.f;
  if (c.parent) {
    std::shared_ptr<NativeObject> up = native_object_of(c.parent);
    if (up && (up->type_index == kNativePart || up->type_index == kNativeCar))
      detail = static_cast<PartObject*>(up.get())->detail_;
  }
  int level = 0;
  if (d.lod_count > 0) {
    level = d.lod_count;
    for (int i = 0; i < d.lod_count; ++i)
      if (detail < d.lod_t[static_cast<size_t>(i)]) {
        level = i;
        break;
      }
  }
  o->level_ = level;
  // The body of the reference entry at create position + R x line offset,
  // turned by R x line rotation (FUN_004e8a50 with local_10 / local_70).
  // Fork: a compound is posed as its reference body (see the head).
  if (d.ref_index >= 0 && d.ref_index < static_cast<int>(d.entries.size()) &&
      d.entries[static_cast<size_t>(d.ref_index)].kind == 1) {
    const ObjectEntry& b = d.entries[static_cast<size_t>(d.ref_index)];
    o->has_body_ = true;
    o->body_pos_ = o->obj_pos_ + cam_mul(o->obj_rot_, b.off);
    o->body_rot_ = cam_mm(o->obj_rot_, b.rot);
    cam_matrix_to_ypr(o->body_rot_, &o->body_ypr_[0], &o->body_ypr_[1],
                      &o->body_ypr_[2]);
    if (part_is_identity(b.rot))
      for (int k = 0; k < 3; ++k) o->body_ypr_[k] = ypr[k];
  }
  object_make_render_instances(o, level);
  o->health_ = d.damage_b;  // obj+0x58 = type+0x8C0
  // The create string is the type's alone; GameRef.create_native must not
  // send it once more through setMatrix (that would place the body).
  o->owns_create_pose = true;
  o->push_pose();
  if (native_type_log()) {
    const RpakEntry* e = rpak_find_entry(d.type_id);
    std::fprintf(stderr,
                 "[native-type] object %p type 0x%08X '%s': create (%g, %g, "
                 "%g) ypr (%g, %g, %g) colour %d level %d/%d, %s, %zu render "
                 "instance(s), health %g\n",
                 static_cast<void*>(o->key_), static_cast<unsigned>(d.type_id),
                 e ? e->name.c_str() : "", pos[0], pos[1], pos[2], ypr[0],
                 ypr[1], ypr[2], colour, level, d.lod_count,
                 o->has_body_ ? "a body" : "no body", o->insts_.size(),
                 o->health_);
    o->log_pose(true);
  }
}

// PE 940 FUN_004bf190 -> type vt+0x0C (FUN_004bd610 -> FUN_004bd040).
std::shared_ptr<NativeObject> object_create(const NativeCreate& c) {
  auto o = std::make_shared<ObjectObject>();
  // Fork: native_object_create writes the index when the factory returns.
  o->type_index = kNativeObject;
  object_init(o.get(), c);
  return o;
}

}  // namespace
