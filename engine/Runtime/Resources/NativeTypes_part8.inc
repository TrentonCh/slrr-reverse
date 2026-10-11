// Fragment 8 of NativeTypes.cpp - included, not compiled on its own.
//
// What is drawn of a part or a car (M3 step 4): the level of detail the
// cameras give a node, and the render entries that hang on slots instead of
// the part's own body (`linked`).
//
// Level of detail (FUN_00461980, FUN_0051a6f0, FUN_0051a5f0, FUN_004c2950,
// FUN_004c1e90, FUN_00498180):
//   - every node a camera walks gets a detail value from it,
//       d = min(1, lodAmp x cot(aov / 2) x size / (distance x
//           Config.object_detail_amp)) + lodBias
//     (`1 + lodBias` for a node without bounds), and keeps the largest of
//     `lod_amp x d` of the frame (lod_amp of the type payload, node+0xEC);
//   - the level of a part or car is the first threshold of its `lods` list
//     above that value, else their count (part_level_for), and it has the
//     render instances of the entries whose `lod first last` holds the
//     level (part_make_render_instances).
//   The garage camera is made with lodAmp 1 and lodBias 1: everything it
//   sees gets a value in (1, 2] and with it the highest level.
//
// `linked a [b [c]]` behind a `render` line (FUN_004c0fc0; applied by the
// tails of FUN_004c2b10 and FUN_004ca390 for a part that is mounted, or is a
// car): with the wheel index i of the part ((part+0xD0 - 101) % 10, else
// -1; added to a and b when it is not negative) and the slot records A =
// a + i and B = b + i, each looked for in the part and then up its mount
// chain (FUN_004c7bc0); `c` not 0 limits the line to a part mounted on slot
// c of what carries it.
//   - b == 0: bone00 of the render instance follows the body of slot A with
//     the position and rotation of slot A (FUN_004ec470);
//   - b != 0: the part sends its car `add_linked <instance id> <wheel index>
//     <A> <B>` (the two numbers are the addresses of the slot records). The
//     car (FUN_004a2e60, asm 0x4a56bb..0x4a586c) keeps a record with the two
//     slot records, the bodies they are on and the vector w = (0, 0, 1), on
//     the list of that wheel (w+0x2BC) or its own (car+0x21FC), and poses
//     the instance whenever it computes the wheel bodies (FUN_004aaaf0 after
//     the hub of the wheel, FUN_004aa7d0 for its own list): FUN_004afbf0
//     takes pA, pB = the world points of the two slots, axis1 = unit(pB -
//     pA), axis0 = unit(W x axis1) with W = w turned by the body of slot A
//     (asm 0x4afc39..0x4afcf6), axis2 = unit(axis0 x axis1), M = the matrix
//     with these columns (FUN_00567250; the identity for pA == pB), and
//     gives bone00 the pose (pA, M) and bone01 the pose (pB, M), both
//     following the car's reference body.
// The shipped lines: `linked 301 501` in spring.cfg and shock_absorber.cfg
// (two-bone meshes stretched between the wheel side and the body side),
// `linked 351` on the lower arm of the two McPherson strut cfgs, `linked
// 551` and `linked 531 541` (control arm, half-shaft) in the two trailing
// arm cfgs.
//
// Fork, in short (each is marked again where it applies):
//   - a level change drops the instances of the old level at once;
//   - a new node takes the detail of the node it is made under
//     (FUN_00523c80, M3 spec Q3); the host knows that value only when that
//     node is a part or a car, and uses 0 otherwise (part_init);
//   - the bounds of a part node are a sphere around its body shapes (Q4);
//   - a linked instance stays a child of its part's key in the renderer's
//     tree (that is what a camera's walk finds it by); its local pose is
//     set so that its world pose is the one above, when the part is linked
//     or moved and once per frame while a camera sees the part (the exe's
//     bones follow their bodies in the instance update FUN_0051c990);
//   - the record of a two-point entry is kept with the instance of the
//     part, not on a list of the car;
//   - lamp entries (cfg `flags` with a bit of 0x13F and not 0x40) are still
//     not instanced and `add_light` is not sent.

namespace {

// PE 940 FUN_004c7bc0(type, part, slot id): the slot record with that id,
// in the part itself or, through the slot it is installed by (flag 1), in
// what it sits on, and so on up to the car. Returns the key of the object
// that owns the record; null when the id is 0 or nothing up the chain has
// such a slot.
InvObject* part_slot_owner(PartObject* p, int32_t slot_id, int depth) {
  if (!p || slot_id == 0) return nullptr;
  if (p->slot(slot_id)) return p->key_;
  if (depth > 64) return nullptr;
  for (const PartObject::Slot& s : p->slots_) {
    if (!(s.flags & 1u) || !s.other) continue;
    std::shared_ptr<PartObject> up = part_of(s.other);
    return up ? part_slot_owner(up.get(), slot_id, depth + 1) : nullptr;
  }
  return nullptr;
}

// The world frame of a slot record: the body the slot is on (getinfo 0x2F:
// the reference body of a part; by `slottype` on a car) and the slot's
// position and rotation on it (FUN_005060d0 for the point).
// `self` is the part that asks, or null: a record of its own is read
// through the object. Fork: the exe holds the address of the record; the
// host finds the owner by its key, which fails for an object whose create
// is still running (a car's own `linked` entries, car_create).
bool part_slot_world_frame(PartObject* self, InvObject* owner_key,
                           int32_t slot_id, CamM3* body_rot, CamV3* pos,
                           CamM3* rot) {
  std::shared_ptr<PartObject> keep;
  PartObject* o = nullptr;
  if (self && owner_key && owner_key == self->key_) {
    o = self;
  } else {
    keep = part_of(owner_key);
    o = keep.get();
  }
  if (!o) return false;
  const PartObject::Slot* s = o->slot(slot_id);
  if (!s) return false;
  CamV3 p;
  CamM3 r = cam_identity();
  o->slot_frame(slot_id, &p, &r);
  if (body_rot) *body_rot = r;
  *pos = p + cam_mul(r, s->pos);
  *rot = cam_mm(r, s->rot);
  return true;
}

inline CamV3 part_unit(CamV3 v) {
  const float l = std::sqrt(cam_len2(v));
  return l != 0.f ? v * (1.f / l) : v;
}

// PE 940 FUN_004afbf0, the rotation: see the head of this fragment. `w` is
// the record's vector (0, 0, 1) turned by the body of slot A.
CamM3 part_linked_rotation(CamV3 pa, CamV3 pb, CamV3 w) {
  CamV3 a1 = pb - pa;
  if (cam_len2(a1) <= 0.f) return cam_identity();  // FUN_005663c0
  a1 = part_unit(a1);
  const CamV3 a0 = part_unit(cam_cross(w, a1));
  const CamV3 a2 = part_unit(cam_cross(a0, a1));
  CamM3 m;
  m.m[0][0] = a0.x;
  m.m[1][0] = a0.y;
  m.m[2][0] = a0.z;
  m.m[0][1] = a1.x;
  m.m[1][1] = a1.y;
  m.m[2][1] = a1.z;
  m.m[0][2] = a2.x;
  m.m[1][2] = a2.y;
  m.m[2][2] = a2.z;
  return m;
}

// The world points a linked instance hangs on: slot A, and slot B of a
// two-point entry. False while a record cannot be found.
bool part_linked_points(const PartObject::Inst& in, CamV3* pa, CamV3* pb) {
  CamM3 r = cam_identity();
  if (!in.link ||
      !part_slot_world_frame(nullptr, in.link_a_on, in.link_a, nullptr, pa,
                             &r))
    return false;
  if (in.link == 2 &&
      !part_slot_world_frame(nullptr, in.link_b_on, in.link_b, nullptr, pb,
                             &r))
    return false;
  return true;
}

// The pose of the linked instances of a part, as local poses under the
// part's key.
void part_place_linked(PartObject* part) {
  bool any = false;
  for (const PartObject::Inst& in : part->insts_)
    if (in.link && in.r) any = true;
  if (!any) return;
  const CamM3 inv = part_transpose(part->rot_);
  for (PartObject::Inst& in : part->insts_) {
    if (!in.link || !in.r) continue;
    CamV3 pa;
    CamM3 ra = cam_identity(), body = cam_identity();
    if (!part_slot_world_frame(part, in.link_a_on, in.link_a, &body, &pa,
                               &ra))
      continue;
    CamM3 M = ra;
    if (in.link == 2) {
      CamV3 pb;
      CamM3 rb = cam_identity();
      if (!part_slot_world_frame(part, in.link_b_on, in.link_b, nullptr, &pb,
                                 &rb))
        continue;
      M = part_linked_rotation(pa, pb, cam_col(body, 2));
      // bone01 = (pB, M): in the frame of bone00 a pure offset. Fork: the
      // renderer keeps the other bones of an instance relative to bone00.
      const CamV3 rel = cam_mul(part_transpose(M), pb - pa);
      render_d3d9_mesh_set_bone_local(
          in.r, render_d3d9_mesh_get_bone_id(in.r, "bone01"), rel.x, rel.y,
          rel.z, 0.f, 0.f, 0.f);
    }
    const CamV3 lp = cam_mul(inv, pa - part->pos_);
    const CamM3 lr = cam_mm(inv, M);
    float yaw = 0.f, pitch = 0.f, roll = 0.f;
    cam_matrix_to_ypr(lr, &yaw, &pitch, &roll);
    render_d3d9_mesh_set_transform(in.r, lp.x, lp.y, lp.z, yaw, pitch, roll,
                                   1.f, 1.f, 1.f);
  }
}

// `rem_linked <instance id> <wheel index>` (the car's handler unbinds
// bone00 and bone01 and frees the record), and the same for a one-slot
// link. Fork: the instance goes back on the part's reference body.
void part_unlink_instance(PartObject* part, PartObject::Inst* in) {
  if (!in->link) return;
  const int was = in->link;
  in->link = 0;
  in->link_a = in->link_b = 0;
  in->link_a_on = in->link_b_on = nullptr;
  if (!in->r) return;
  if (was == 2)
    render_d3d9_mesh_erase_bone(in->r,
                                render_d3d9_mesh_get_bone_id(in->r, "bone01"));
  const PartTypeDesc& d = *part->type_;
  const PartEntry& e = d.entries[static_cast<size_t>(in->entry)];
  // A car places its steering and wheel renders itself
  // (CarObject::place_own_instances).
  if (e.flags & 0x9000u) return;
  CamV3 at;
  CamM3 rot = cam_identity();
  if (!part_render_local(d, e, &at, &rot)) return;
  float yaw = 0.f, pitch = 0.f, roll = 0.f;
  cam_matrix_to_ypr(rot, &yaw, &pitch, &roll);
  render_d3d9_mesh_set_transform(in->r, at.x, at.y, at.z, yaw, pitch, roll,
                                 1.f, 1.f, 1.f);
}

void part_unlink_instances(PartObject* part) {
  for (PartObject::Inst& in : part->insts_) part_unlink_instance(part, &in);
}

// PE 940 the tails of FUN_004c2b10 and FUN_004ca390: the `linked` lines of
// the part's render instances are applied. Nothing for a part that is
// neither mounted nor a car.
void part_link_instances(PartObject* part) {
  const PartTypeDesc& d = *part->type_;
  bool any = false;
  for (const PartObject::Inst& in : part->insts_)
    if (in.link ||
        (d.entries[static_cast<size_t>(in.entry)].flags & 0x4000u) != 0)
      any = true;
  if (!any) return;
  // FUN_004c6e20: the slot the part is installed through.
  const PartObject::Slot* mount = nullptr;
  for (const PartObject::Slot& s : part->slots_)
    if (s.flags & 1u) {
      mount = &s;
      break;
    }
  const bool is_car = part->type_index == kNativeCar;
  // Where `add_linked` goes: part+0xC0, the car the part sits on; a car is
  // its own. Only the car's handler knows the word.
  std::shared_ptr<PartObject> car = is_car ? nullptr : part_of(part->car_);
  const bool on_car = is_car || (car && car->type_index == kNativeCar);
  const int wheel = part_wheel_of(part->d0_);
  const int32_t add = wheel > 0 ? wheel : 0;
  for (PartObject::Inst& in : part->insts_) {
    part_unlink_instance(part, &in);
    const PartEntry& e = d.entries[static_cast<size_t>(in.entry)];
    if (!(e.flags & 0x4000u) || (!mount && !is_car)) continue;
    for (const PartEntry::Linked& l : e.linked) {
      if (l.c != 0 && !(mount && mount->other_slot == l.c)) continue;
      InvObject* const a_on = part_slot_owner(part, l.a + add, 0);
      if (l.b == 0) {
        if (a_on) {
          in.link = 1;
          in.link_a = l.a + add;
          in.link_a_on = a_on;
        }
      } else {
        InvObject* const b_on = part_slot_owner(part, l.b + add, 0);
        // The car's handler returns without a record when either slot
        // record is null (asm 0x4a5708..0x4a5714).
        if (on_car && a_on && b_on) {
          in.link = 2;
          in.link_a = l.a + add;
          in.link_a_on = a_on;
          in.link_b = l.b + add;
          in.link_b_on = b_on;
        }
      }
      break;  // the first line that applies
    }
  }
  part_place_linked(part);
}

}  // namespace

// ---------------------------------------------------------------------------
// The detail a camera gives the nodes it walks
// ---------------------------------------------------------------------------
// PE 940 FUN_00461980(camera, node, hidden), called for every node of the
// camera's walk (FUN_0045eb10), then the node's FUN_0051a6f0:
//   size = the node's bounds: the radius of a sphere, the length of the
//          half extents of a box; a node without bounds (kind 2) gets
//          1 + lodBias;
//   q    = |eye - bounds centre|^2 x Config.object_detail_amp^2
//   d    = q < 1e-4 ? 1 + lodBias
//                   : min(1, sqrt(lodAmp^2 x cot(aov / 2)^2 x size^2 / q))
//                     + lodBias
//   node+0xA0 = the largest `lod_amp x d` of the frame (node+0xEC x d +
//               node+0xE8; the second term is 0 for a game node).
// Host: the camera pass calls this once per camera before it draws
// (draw_viewport_cameras), with the root of the camera's walk; every part
// and car object whose node hangs below that root is a node of the walk.
// Fork: (M3 spec Q4) the bounds are the sphere of the type's body shapes
// around the reference body (PartTypeDesc::bound_r), none for a type
// without a body. The exe's are the box FUN_00503c20 puts around the
// node's physics-instance children (part_type_derive says what was read).
// Fork: a node below a WORLDTREELEAF container is not descended to; the exe
// passes such a node through the same function with `hidden` set, which
// gives it 0. The host leaves its detail as it is: the instances are not
// drawn either way, and a part that is shown again need not load its
// meshes a second time.
void native_camera_detail(void* root, const float eye[3], float cot_half_aov,
                          float lod_amp, float lod_bias, uint32_t frame) {
  if (!native_script_boot() || !root || g_parts_live.empty()) return;
  constexpr float kNear = 9.99999975e-05f;  // DAT_005938c4
  float amp = cam_config("object_detail_amp");  // DAT_005da348
  if (!(amp > 0.f)) amp = 1.f;  // the exe's value before a Config is read
  static std::vector<std::pair<void*, bool>> s_below;  // main thread
  s_below.clear();
  for (PartObject* p : g_parts_live) {
    if (p->gone_ || !p->key_) continue;
    bool sees = static_cast<void*>(p->key_) == root;
    if (!sees) {
      if (p->walk_frame_ != frame) {
        p->walk_parent_ = render_d3d9_camera_walk_parent(p->key_);
        p->walk_frame_ = frame;
      }
      void* const up = p->walk_parent_;
      if (!up) continue;
      bool known = false;
      for (const auto& b : s_below)
        if (b.first == up) {
          sees = b.second;
          known = true;
          break;
        }
      if (!known) {
        sees = render_d3d9_camera_sees_below(root, up);
        s_below.emplace_back(up, sees);
      }
    }
    if (!sees) continue;
    const PartTypeDesc& d = *p->type_;
    float v = 1.f + lod_bias;
    if (d.bound_r >= 0.f) {
      const float dx = eye[0] - p->pos_.x, dy = eye[1] - p->pos_.y;
      const float dz = eye[2] - p->pos_.z;
      const float q = (dx * dx + dy * dy + dz * dz) * amp * amp;
      if (kNear <= q) {
        v = std::sqrt((lod_amp * lod_amp * cot_half_aov * cot_half_aov *
                       d.bound_r * d.bound_r) /
                      q);
        if (1.f <= v) v = 1.f;
        v += lod_bias;
      }
    }
    v *= d.lod_amp;
    if (p->detail_frame_ < frame || p->detail_ < v) p->detail_ = v;
    p->detail_frame_ = frame;
  }
}

// PE 940 FUN_0051a5f0, the per-frame update of a game node: when the
// node's detail is outside the range of the level its instances were made
// for, the type's level change runs (vt+0x18: FUN_004c1e90 for a part,
// FUN_00498180 for a car).
// Host: called by the camera pass when every camera of the frame has given
// its detail and before any of them draws, for the parts a camera saw in
// this frame. The linked instances of those parts get their poses here too
// (see the head of this fragment).
void native_camera_detail_end(uint32_t frame) {
  if (!native_script_boot()) return;
  for (size_t i = 0; i < g_parts_live.size(); ++i) {
    PartObject* const p = g_parts_live[i];
    if (p->gone_ || p->detail_frame_ != frame) continue;
    const int level = part_level_for(*p->type_, p->detail_);
    if (level != p->level_) {
      p->level_ = level;
      p->make_instances();
      part_link_instances(p);
    } else {
      part_place_linked(p);
    }
  }
}
