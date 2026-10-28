// Fragment 7 of NativeTypes.cpp - included, not compiled on its own.
//
// What the scripts ask of a part or car object. The natives of
// java.game.parts.Part, java.game.parts.bodypart.Chassis and
// java.game.parts.WheelRef (Runtime/Parts) call these on the script boot;
// each returns false when the receiver has no part or car object, and the
// native then answers from the legacy tables as before (the old build and
// the unit smoke have no such object).
//
// In the exe every one of these natives walks from the script instance to
// its node (Native.ptr -> node payload, FUN_0051f4f0 / vt+0x0C) and reads
// or writes the game object at payload+0x4C; the type object is behind
// payload+0x44. The host has the part / car object of the node
// (part_of) and its type desc.
//
// Mirrors: Part.getSlotID 0x4CCAA0 (index < 0), partOnSlot 0x4CD600,
// slotIDOnSlot 0x4CE770, getWheelID 0x4CD2E0, setSlotPos 0x4CDDC0,
// disableSlot 0x4CE4A0, isSlotDisabled 0x4CE660, getSlotDamage 0x4CCD10,
// setSlotDamage 0x4CDC30, getRenderType 0x4CC9A0, setRenderType 0x4CD9E0 ->
// FUN_004c7470, getMesh 0x4CC8A0, setMesh 0x4CD840 -> FUN_004c72d0,
// getTexture 0x4CD0D0, setTexture 0x4CE1F0 -> FUN_004c75a0;
// Chassis.getWheels 0x4B4280, getMass 0x4B2B90, getMaxSteer 0x4B3110,
// setMaxSteer 0x4B5030, setAckermann 0x4B47B0, forceUpdate 0x4B5890 ->
// FUN_004aebf0; WheelRef.setRadius 0x4B6E50, setRestLen 0x4B6EF0, setMinLen
// 0x4B6990, setMaxLen 0x4B68B0, setForce 0x4B6380, setDamping 0x4B6280 /
// 0x4B61D0, setSteer 0x4B7010, setYpr 0x4B70E0, setPos 0x4B6AF0, setArm
// 0x4B5F50, setHub 0x4B65D0, setOppWheel 0x4B6A30, getRadius 0x4B5DC0,
// getSteer 0x4B5E00, getYpr 0x4B5E40, getPos 0x4B5B90.

namespace {

// payload+0x50 of a node: its script instance, null when the node's type
// has no script class. The node object of a scripted node is that instance.
InvObject* part_script_instance(InvObject* node) {
  if (!node || part_node_id(node) == 0) return nullptr;
  std::lock_guard<std::mutex> lock(g_gr_mu);
  auto it = g_refs.find(node);
  return (it != g_refs.end() && !it->second.script_class.empty()) ? node
                                                                  : nullptr;
}

// The car object of a chassis script instance; null for everything else.
std::shared_ptr<CarObject> car_of(InvObject* chassis) {
  std::shared_ptr<PartObject> p = part_of(chassis);
  if (!p || p->type_index != kNativeCar) return nullptr;
  return std::static_pointer_cast<CarObject>(p);
}

}  // namespace

// ---------------------------------------------------------------------------
// Part: the slots
// ---------------------------------------------------------------------------
// PE 940 Part.getSlotID 0x4CCAA0 for an index below 0: the slot records of
// the instance are walked in the order of the type's list and the id of
// the first slot whose record has flag 1 (the part is installed through
// it) is returned; 0 when no record has it.
bool native_part_mount_slot(InvObject* self, int32_t* id) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  *id = 0;
  for (const PartObject::Slot& s : p->slots_)
    if (s.flags & 1u) {
      *id = s.id;
      break;
    }
  return true;
}

// PE 940 Part.partOnSlot 0x4CD600: -1 gives the script instance of the
// part itself (payload+0x50); any other number the script instance of the
// node the handle of that slot record is bound to. The record's flags are
// not looked at: the slot a part is installed through gives the part it
// sits on. Null for an unknown slot, an unbound handle or a node without
// script instance.
bool native_part_on_slot(InvObject* self, int32_t slot_id, InvObject** part) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  *part = nullptr;
  if (slot_id == -1) {
    *part = part_script_instance(p->key_);
    return true;
  }
  if (const PartObject::Slot* s = p->slot(slot_id))  // FUN_004c6fd0
    if (s->other) *part = part_script_instance(s->other);
  return true;
}

// PE 940 Part.slotIDOnSlot 0x4CE770: record+0x10, the id of the slot on
// the other side; 0 for an unknown slot. The number is read whether or not
// the handle is still bound.
bool native_part_slot_id_on_slot(InvObject* self, int32_t slot_id,
                                 int32_t* other_slot) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  const PartObject::Slot* s = p->slot(slot_id);
  *other_slot = s ? s->other_slot : 0;
  return true;
}

// PE 940 Part.getWheelID 0x4CD2E0: (part+0xD0 - 101) % 10 when part+0xD0
// is 101..400, else -1.
bool native_part_wheel_id(InvObject* self, int32_t* id) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  *id = part_wheel_of(p->d0_);
  return true;
}

// PE 940 Part.setSlotPos 0x4CDDC0, in this order:
//   - the slot record takes position and rotation of the type's slot again
//     and loses flag 0x20;
//   - `pos` (null = none) is ADDED to the position, flag |= 0x20;
//   - `ypr` (null = none): rotation = matrix(ypr) x rotation (FUN_005666d0,
//     then FUN_00566a70 with the record's matrix as the receiver, asm
//     0x4ce020..0x4ce02f), flag |= 0x20;
//   - when the slot's handle is bound: this part is put on its slot again
//     when it is installed THROUGH the slot (flag 1), else the part on the
//     slot is (FUN_004cb8d0), and the parts installed on that part follow
//     (FUN_004cb6c0).
// Fork: a flap record of the slot (record+0x28) would move by the same
// position delta; flaps are not ported.
bool native_part_set_slot_pos(InvObject* self, int32_t slot_id,
                              const float* pos, const float* ypr) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  PartObject::Slot* rec = p->slot(slot_id);                  // FUN_004c6fd0
  const PartSlot* ts = part_type_slot(*p->type_, slot_id);  // FUN_004c6f90
  if (!rec || !ts) return true;
  rec->pos = ts->pos;
  rec->rot = ts->rot;
  rec->flags &= ~0x20u;
  if (pos) {
    rec->pos = rec->pos + cam_v(pos[0], pos[1], pos[2]);
    rec->flags |= 0x20u;
  }
  if (ypr) {
    rec->rot = cam_mm(cam_ypr_to_matrix(ypr[0], ypr[1], ypr[2]), rec->rot);
    rec->flags |= 0x20u;
  }
  if (rec->other) {
    std::shared_ptr<PartObject> who =
        (rec->flags & 1u) ? p : part_of(rec->other);
    if (who) {
      who->pose_on_slot(0);
      part_adopt_subtree(who.get(), 0);
    }
  }
  return true;
}

// PE 940 Part.disableSlot 0x4CE4A0: the record of the slot loses the bits
// 0x50 and gets 0x10 for status 1, 0x50 for status 2 (any other status
// leaves it enabled); when the slot's handle is bound, the record on the
// other side (slot record+0x10 of that part) gets the same.
bool native_part_disable_slot(InvObject* self, int32_t slot_id,
                              int32_t status) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  PartObject::Slot* rec = p->slot(slot_id);
  if (!rec) return true;
  auto apply = [status](PartObject::Slot* s) {
    s->flags &= ~0x50u;
    if (status == 1)
      s->flags |= 0x10u;
    else if (status == 2)
      s->flags |= 0x50u;
  };
  apply(rec);
  if (rec->other)
    if (std::shared_ptr<PartObject> o = part_of(rec->other))
      if (PartObject::Slot* mate = o->slot(rec->other_slot)) apply(mate);
  return true;
}

// PE 940 Part.isSlotDisabled 0x4CE660 (asm 0x4ce741..0x4ce75c): 2 when
// the record has one of the bits 0x50, else bit 0x10 (which is 0 then); 0
// for an unknown slot.
bool native_part_slot_disabled(InvObject* self, int32_t slot_id,
                               int32_t* state) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  *state = 0;
  if (const PartObject::Slot* s = p->slot(slot_id))
    *state = (s->flags & 0x50u) ? 2 : static_cast<int32_t>((s->flags >> 4) & 1u);
  return true;
}

// PE 940 Part.getSlotDamage 0x4CCD10: for the slot record with that INDEX
// (0 .. slots of the type) that has flag 0x20 the text
// "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f" of its position and of the Ypr of its
// rotation (FUN_00569970); the empty string otherwise.
bool native_part_slot_damage(InvObject* self, int32_t index, char* out,
                             size_t cap) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  if (cap) out[0] = 0;
  if (index < 0 || static_cast<size_t>(index) >= p->slots_.size()) return true;
  const PartObject::Slot& s = p->slots_[static_cast<size_t>(index)];
  if (!(s.flags & 0x20u)) return true;
  float yaw = 0.f, pitch = 0.f, roll = 0.f;
  cam_matrix_to_ypr(s.rot, &yaw, &pitch, &roll);
  std::snprintf(out, cap, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
                static_cast<double>(s.pos.x), static_cast<double>(s.pos.y),
                static_cast<double>(s.pos.z), static_cast<double>(yaw),
                static_cast<double>(pitch), static_cast<double>(roll));
  return true;
}

// PE 940 Part.setSlotDamage 0x4CDC30: for the slot record with that index,
// a text longer than one character is scanned with "%f,%f,%f,%f,%f,%f"
// into the position and (FUN_005666d0) the rotation of the record, flag |=
// 0x20; a shorter one clears flag 0x20. The part on the slot is not moved:
// Part.load sets the record before it creates and installs that part.
// Fork: numbers the text lacks are 0 (the exe leaves them unset).
bool native_part_set_slot_damage(InvObject* self, int32_t index,
                                 const char* text) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  if (index < 0 || static_cast<size_t>(index) >= p->slots_.size()) return true;
  PartObject::Slot& s = p->slots_[static_cast<size_t>(index)];
  if (!text) text = "";
  if (std::strlen(text) > 1) {
    float v[6] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    engine_sscanf(text, "%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4],
                  &v[5]);
    s.pos = cam_v(v[0], v[1], v[2]);
    s.rot = cam_ypr_to_matrix(v[3], v[4], v[5]);
    s.flags |= 0x20u;
  } else {
    s.flags &= ~0x20u;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Part: paint (the texture line and the mesh line of the main render type)
// ---------------------------------------------------------------------------
namespace {

// What Part.setTexture / setMesh and the `texture` / `mesh` commands came to
// in this run (native_part_paint_dump).
struct PaintStats {
  int tex_calls = 0, tex_replaced = 0, tex_no_slot = 0, tex_same = 0;
  int tex_no_line = 0, tex_no_record = 0;
  int mesh_calls = 0, mesh_replaced = 0, mesh_no_slot = 0, mesh_same = 0;
  int mesh_no_line = 0, mesh_no_record = 0;
  int dropped = 0;
};
PaintStats g_paint;

// The render instance FUN_004c75a0, FUN_004c72d0 and FUN_004c7470 switch to
// another render type: the first one whose entry names the type's main
// render type (entry+0x50 == type+0x1110).
PartObject::Inst* part_main_instance(PartObject* p) {
  const PartTypeDesc& d = *p->type_;
  if (!d.main_render) return nullptr;
  for (PartObject::Inst& i : p->insts_)
    if (i.r && d.entries[static_cast<size_t>(i.entry)].res_id == d.main_render)
      return &i;
  return nullptr;
}

// Texture line `slot` of the part's render type as the part has it.
int32_t part_texture_line(const PartObject* p, const RenderObjectDesc* desc,
                          size_t slot) {
  for (const auto& o : p->tex_over_)
    if (o.first == static_cast<int32_t>(slot)) return o.second;
  return desc->textures[slot];
}

// The record an id names, when it is of pack type `restype` (7 texture, 5
// mesh).
bool part_names_record(int32_t id, int restype) {
  // M7: a runtime texture of the paint booth (Resources_part5.inc).
  if (restype == 7 && id && paint_texture_handle(id)) return true;
  // The mesh of a car file's unsaved row (Resources_part5.inc).
  if (restype == 5 && id && side_mesh_bytes(id)) return true;
  const RpakEntry* e = id ? rpak_find_entry(id) : nullptr;
  return e && e->restype == restype;
}

// PE 940 FUN_004c75a0(type, part, texture handle), behind the test its
// three callers make first (Part.setTexture 0x4CE1F0, the `texture` command
// of a part in FUN_004c4400 and of a car in FUN_004a2e60): part+0x84 != 0.
//   - nothing unless the id differs from part+0x84 and the type has a cfg
//     `texture` line (type+0x10F0);
//   - while the part still uses the type's main render type (part+0xA4 ==
//     type+0x1110) that render type is duplicated into part+0x9C
//     (FUN_004e9470 -> FUN_0051ee30: a new node with the payload of the
//     old one) and the first render instance whose entry names the main
//     render type is switched to the copy (FUN_0051e090);
//   - the data object of part+0x9C replaces the old texture by the new one
//     (vt+8 FUN_00522b00: the FIRST texture line whose id is part+0x84)
//     and marks its template for a rebind (FUN_0046f2d0);
//   - part+0x7C is bound to the new texture: Part.getTexture answers its
//     id from now on, whether a line was found or not.
// Host: the part keeps the lines of that copy which differ from the pack's
// render object (tex_over_) and gives them to the render instance of the
// main entry (renderref_set_line_overrides); nothing is copied.
// Fork: Part.getRenderType keeps answering the id of the pack's render
//   type; in the exe part+0xA4 is the id of the copy from here on (a node
//   id of the script-made allocator, FUN_00518a40).
// Fork: after Part.setRenderType the part uses a render type of the pack
//   again and the exe replaces the line in THAT shared object, for every
//   instance made from it; the host replaces it for this part only.
// Fork: (M3 spec Q14) an id that names no texture record is ignored and the
//   part keeps its texture. The exe binds an empty handle there: the line
//   loses its texture and part+0x84 becomes 0. The ids concerned are 0 and
//   the unsaved resources of a car file (a painted skin in a side file
//   `<car>.<n>`), for which the host's File.readResID answers 0.
// Not ported: FUN_004e9450, which releases a replaced runtime texture (id
//   0xFFFFxxxx, the private texture the `paint` command draws into).
void part_set_texture(PartObject* p, int32_t id) {
  ++g_paint.tex_calls;
  const PartTypeDesc& d = *p->type_;
  if (!p->texture_ || !d.texture_id) {
    ++g_paint.tex_no_line;
    return;
  }
  if (p->texture_ == id) {
    ++g_paint.tex_same;
    return;
  }
  if (!part_names_record(id, 7)) {
    ++g_paint.tex_no_record;
    if (native_type_log())
      std::fprintf(stderr,
                   "[native-type] Part.setTexture(0x%08X) on '%s': not a "
                   "texture record, ignored (the part keeps 0x%08X)\n",
                   static_cast<unsigned>(id), car_type_alias(d.type_id),
                   static_cast<unsigned>(p->texture_));
    return;
  }
  const RenderObjectDesc* desc = render_object_desc(p->render_type_);
  int slot = -1;
  if (desc->found)
    for (size_t i = 0; i < desc->textures.size() && slot < 0; ++i)
      if (part_texture_line(p, desc, i) == p->texture_)
        slot = static_cast<int>(i);
  if (slot >= 0) {
    for (size_t k = p->tex_over_.size(); k-- > 0;)
      if (p->tex_over_[k].first == slot)
        p->tex_over_.erase(p->tex_over_.begin() +
                           static_cast<std::ptrdiff_t>(k));
    if (desc->textures[static_cast<size_t>(slot)] != id)
      p->tex_over_.emplace_back(slot, id);
    ++g_paint.tex_replaced;
    if (PartObject::Inst* i = part_main_instance(p)) part_lines_to(p, i->r);
  } else {
    ++g_paint.tex_no_slot;
    if (native_type_log())
      std::fprintf(stderr,
                   "[native-type] Part.setTexture(0x%08X) on '%s': render "
                   "type 0x%08X has no texture line 0x%08X (%zu line(s)); "
                   "only the handle changes\n",
                   static_cast<unsigned>(id), car_type_alias(d.type_id),
                   static_cast<unsigned>(p->render_type_),
                   static_cast<unsigned>(p->texture_), desc->textures.size());
  }
  p->texture_ = id;
}

// PE 940 FUN_004c72d0(type, part, mesh handle), behind the test of its
// callers (Part.setMesh 0x4CD840, the `mesh` command of a part and of a
// car): part+0x94 != 0. The same shape as FUN_004c75a0 with the cfg `mesh`
// line (type+0x1100) and the handle part+0x8C: the private copy of the main
// render type, in which FUN_00522b00 replaces the mesh when the render
// object's mesh is the part's old one (it tests the mesh line first, then
// the texture lines, which a mesh id never matches), and part+0x8C bound to
// the new mesh: Part.getMesh answers its id.
// Host: the mesh line of the copy is mesh_over_; the render instance of
// the main entry is made again from that mesh (the pending list).
// Fork: (M3 spec Q14) an id that names no mesh record is ignored, as for a
//   texture: the unsaved resources of a car file are bent panels here.
// The Forks of part_set_texture hold here too.
void part_set_mesh(PartObject* p, int32_t id) {
  ++g_paint.mesh_calls;
  const PartTypeDesc& d = *p->type_;
  if (!p->mesh_ || !d.mesh_id) {
    ++g_paint.mesh_no_line;
    return;
  }
  if (p->mesh_ == id) {
    ++g_paint.mesh_same;
    return;
  }
  if (!part_names_record(id, 5)) {
    ++g_paint.mesh_no_record;
    if (native_type_log())
      std::fprintf(stderr,
                   "[native-type] Part.setMesh(0x%08X) on '%s': not a mesh "
                   "record, ignored (the part keeps 0x%08X)\n",
                   static_cast<unsigned>(id), car_type_alias(d.type_id),
                   static_cast<unsigned>(p->mesh_));
    return;
  }
  // Fork: the mesh of a car file's side file (a bent panel) is the part's
  // handle, so Part.getMesh answers it and the part saves it again, but the
  // render line keeps the pack mesh: the host does not read the exe
  // writer's INVO layout (Resources_part5.inc).
  if (side_mesh_bytes(id)) {
    ++g_paint.mesh_no_slot;
    if (native_type_log())
      std::fprintf(stderr,
                   "[native-type] Part.setMesh(0x%08X) on '%s': a mesh side "
                   "file; drawn with 0x%08X\n",
                   static_cast<unsigned>(id), car_type_alias(d.type_id),
                   static_cast<unsigned>(p->mesh_));
    p->mesh_ = id;
    return;
  }
  const RenderObjectDesc* desc = render_object_desc(p->render_type_);
  const int32_t line = p->mesh_over_set_ ? p->mesh_over_ : desc->mesh_id;
  if (desc->found && line == p->mesh_) {
    p->mesh_over_set_ = desc->mesh_id != id;
    p->mesh_over_ = p->mesh_over_set_ ? id : 0;
    ++g_paint.mesh_replaced;
    if (PartObject::Inst* i = part_main_instance(p)) part_lines_to(p, i->r);
  } else {
    ++g_paint.mesh_no_slot;
    if (native_type_log())
      std::fprintf(stderr,
                   "[native-type] Part.setMesh(0x%08X) on '%s': the mesh of "
                   "render type 0x%08X is 0x%08X, not the part's 0x%08X; only "
                   "the handle changes\n",
                   static_cast<unsigned>(id), car_type_alias(d.type_id),
                   static_cast<unsigned>(p->render_type_),
                   static_cast<unsigned>(line),
                   static_cast<unsigned>(p->mesh_));
  }
  p->mesh_ = id;
}

}  // namespace

// PE 940 Part.getTexture 0x4CD0D0 = part+0x84, the id of the texture handle
// part+0x7C.
bool native_part_texture(InvObject* self, int32_t* id) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  *id = p->texture_;
  return true;
}

// PE 940 Part.setTexture 0x4CE1F0: a handle on the typeof-7 record `id`
// (FUN_0051fa40(id, 7, 0)) and, when part+0x84 != 0, FUN_004c75a0(type,
// part, handle). The native answers 1 whatever happened.
bool native_part_set_texture(InvObject* self, int32_t id) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  part_set_texture(p.get(), id);
  return true;
}

// `[render-dbg]`: what the paint calls of the run came to, then one line
// per replaced line of a part's or car's render type: the cfg texture
// (type+0x10E8), the current one (part+0x84), the render type, the slot,
// the line of the pack's render object and what replaced it, and whether
// the render instance of the main entry shows it (has_mesh: the instance
// was made; loaded: the texture is there; bound: texture entries of its
// sections that use it).
void native_part_paint_dump(FILE* out) {
  if (!out || !native_script_boot()) return;
  std::vector<std::shared_ptr<NativeObject>> objs;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    for (const auto& kv : g_objects) {
      if (!kv.second || (kv.second->type_index != kNativePart &&
                         kv.second->type_index != kNativeCar))
        continue;
      bool seen = false;
      for (const auto& o : objs)
        if (o == kv.second) seen = true;
      if (!seen) objs.push_back(kv.second);
    }
  }
  size_t with_tex = 0, with_mesh = 0;
  for (const auto& o : objs) {
    const PartObject* p = static_cast<const PartObject*>(o.get());
    if (!p->tex_over_.empty()) ++with_tex;
    if (p->mesh_over_set_) ++with_mesh;
  }
  const PaintStats& s = g_paint;
  std::fprintf(out,
               "[render-dbg] paint: %zu part(s) with a replaced texture line, "
               "%zu with a replaced mesh line; texture: %d call(s) = %d "
               "replaced a line + %d found no line with the current id + %d "
               "same id + %d type without a cfg texture line + %d id without "
               "a texture record (ignored); mesh: %d call(s) = %d replaced "
               "the mesh line + %d render type has another mesh + %d same id "
               "+ %d type without a cfg mesh line + %d id without a mesh "
               "record (ignored); %d part(s) left their lines by "
               "setRenderType\n",
               with_tex, with_mesh, s.tex_calls, s.tex_replaced, s.tex_no_slot,
               s.tex_same, s.tex_no_line, s.tex_no_record, s.mesh_calls,
               s.mesh_replaced, s.mesh_no_slot, s.mesh_same, s.mesh_no_line,
               s.mesh_no_record, s.dropped);
  for (const auto& o : objs) {
    PartObject* p = static_cast<PartObject*>(o.get());
    if (p->tex_over_.empty() && !p->mesh_over_set_) continue;
    const PartTypeDesc& d = *p->type_;
    const RenderObjectDesc* desc = render_object_desc(p->render_type_);
    const PartObject::Inst* mi = part_main_instance(p);
    const bool has_mesh = mi && render_d3d9_mesh_ready(mi->r) &&
                          !render_d3d9_mesh_pending_waiting(mi->r);
    const unsigned car =
        (p->car_ && p->car_ != p->key_)
            ? static_cast<unsigned>(part_node_id(p->car_))
            : 0u;
    char inst[24];
    if (mi)
      std::snprintf(inst, sizeof inst, "%p", static_cast<void*>(mi->r));
    else
      std::snprintf(inst, sizeof inst, "-");
    for (const auto& ov : p->tex_over_) {
      const RpakEntry* te = rpak_find_entry(ov.second);
      // The texture is asked for only where an instance shows it: a part
      // no camera has seen has loaded nothing.
      void* key = has_mesh ? renderref_line_texture_key(ov.second) : nullptr;
      const char* label = key ? render_d3d9_texture_label(key) : "";
      const char* base = label;
      for (const char* c = label; *c; ++c)
        if (*c == '/' || *c == '\\') base = c + 1;
      const size_t slot = static_cast<size_t>(ov.first);
      std::fprintf(out,
                   "[render-dbg]   paint %p node=0x%08X '%s' car=0x%08X "
                   "cfg_texture=0x%08X texture=0x%08X render=0x%08X slot=%d "
                   "was=0x%08X now=0x%08X '%s' instance=%s has_mesh=%d "
                   "loaded=%d bound=%d file='%s'\n",
                   static_cast<void*>(p->key_),
                   static_cast<unsigned>(part_node_id(p->key_)),
                   car_type_alias(d.type_id), car,
                   static_cast<unsigned>(d.texture_id),
                   static_cast<unsigned>(p->texture_),
                   static_cast<unsigned>(p->render_type_), ov.first,
                   static_cast<unsigned>(
                       slot < desc->textures.size() ? desc->textures[slot] : 0),
                   static_cast<unsigned>(ov.second),
                   te ? te->name.c_str() : "", inst, has_mesh ? 1 : 0,
                   key ? 1 : 0,
                   key ? render_d3d9_mesh_texture_uses(mi->r, key) : 0, base);
    }
    if (p->mesh_over_set_) {
      const RpakEntry* me = rpak_find_entry(p->mesh_over_);
      std::fprintf(out,
                   "[render-dbg]   paint %p node=0x%08X '%s' car=0x%08X "
                   "cfg_mesh=0x%08X mesh=0x%08X render=0x%08X mesh_was=0x%08X "
                   "mesh_now=0x%08X '%s' instance=%s has_mesh=%d\n",
                   static_cast<void*>(p->key_),
                   static_cast<unsigned>(part_node_id(p->key_)),
                   car_type_alias(d.type_id), car,
                   static_cast<unsigned>(d.mesh_id),
                   static_cast<unsigned>(p->mesh_),
                   static_cast<unsigned>(p->render_type_),
                   static_cast<unsigned>(desc->mesh_id),
                   static_cast<unsigned>(p->mesh_over_),
                   me ? me->name.c_str() : "", inst, has_mesh ? 1 : 0);
    }
  }
}

// ---------------------------------------------------------------------------
// Part: mesh and render type
// ---------------------------------------------------------------------------
// PE 940 Part.getRenderType 0x4CC9A0 = part+0xA4, Part.getMesh 0x4CC8A0 =
// part+0x94: the ids of the handles part+0x9C / +0x8C.
bool native_part_render_type(InvObject* self, int32_t* id) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  *id = p->render_type_;
  return true;
}

bool native_part_mesh(InvObject* self, int32_t* id) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  *id = p->mesh_;
  return true;
}

// PE 940 Part.setRenderType 0x4CD9E0: a handle on the typeof-14 record
// `id` (FUN_0051fa40(id, 0xe, 0)), and, when the part has a main render
// type (part+0xA4 != 0), FUN_004c7470(type, part, handle):
//   - nothing when the part's main render type has that id already;
//   - the first render instance of the part whose ENTRY names the type's
//     main render type (entry+0x50 == type+0x1110) is switched to the new
//     type (FUN_0051e090);
//   - part+0x8C is bound to the mesh of the new render type (render type
//     payload+0x18), part+0x9C to the new render type: Part.getMesh and
//     getRenderType answer with their ids from now on (a saved car carries
//     the mesh id: the right front and both rear brake discs of a stock
//     car).
// Fork: an id that names no typeof-14 record is ignored (the exe would bind
// null handles: the instance loses its type and part+0xA4 becomes 0). The
// instance keeps its host object: it is put back on the pending list with
// the new type, which a camera's walk loads (the exe requests it there
// too).
// A part with a private copy of its render type (Part.setTexture / setMesh)
// leaves the copy here: part+0x9C is bound to the render type of the pack
// and the instance is made from that, so the replaced lines are gone.
// Fork: the test above compares with the id of the pack's render type the
// part was last given; with a private copy the exe has the id of the copy
// in part+0xA4, so that setRenderType(the type's own main render type) on a
// painted part takes the paint off there and does nothing here.
bool native_part_set_render_type(InvObject* self, int32_t id) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  if (!p->render_type_ || p->render_type_ == id) return true;
  const RenderObjectDesc* desc = render_object_desc(id);
  if (!desc->found) {
    if (native_type_log())
      std::fprintf(stderr,
                   "[native-type] Part.setRenderType(0x%08X) on '%s': not a "
                   "render object, ignored\n",
                   static_cast<unsigned>(id),
                   car_type_alias(p->type_->type_id));
    return true;
  }
  const PartTypeDesc& d = *p->type_;
  for (PartObject::Inst& i : p->insts_) {
    if (!i.r || d.entries[static_cast<size_t>(i.entry)].res_id != d.main_render)
      continue;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      R(i.r).type_id = id;
    }
    renderref_clear_line_overrides(i.r);
    render_d3d9_mesh_set_pending_type(i.r, id);
    i.res_id = id;
    break;
  }
  if (!p->tex_over_.empty() || p->mesh_over_set_) {
    p->tex_over_.clear();
    p->mesh_over_set_ = false;
    p->mesh_over_ = 0;
    ++g_paint.dropped;
  }
  p->mesh_ = desc->mesh_id;
  p->render_type_ = id;
  return true;
}

// PE 940 Part.setMesh 0x4CD840: a handle on the typeof-5 record `id`
// (FUN_0051fa40(id, 5, 0)) and, when part+0x94 != 0, FUN_004c72d0(type,
// part, handle) (part_set_mesh). The native answers 1 whatever happened.
bool native_part_set_mesh(InvObject* self, int32_t id) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  part_set_mesh(p.get(), id);
  return true;
}

// ---------------------------------------------------------------------------
// Chassis
// ---------------------------------------------------------------------------
// PE 940 Chassis.getWheels 0x4B4280 = type+0x1FB4, the number of wheels of
// the cfg (`type` lines).
bool native_car_wheel_count(InvObject* chassis, int32_t* count) {
  std::shared_ptr<CarObject> car = car_of(chassis);
  if (!car) return false;
  *count = static_cast<int32_t>(car->wheels_.size());
  return true;
}

// PE 940 Chassis.getMass 0x4B2B90 = 1 / inverse mass of the chassis
// compound (car+0x13EC): the car's bodies and every part installed on a
// slot of the reference body, with everything on those parts.
bool native_car_mass(InvObject* chassis, float* kg) {
  std::shared_ptr<CarObject> car = car_of(chassis);
  if (!car) return false;
  *kg = car->compound_mass();
  return true;
}

// ---------------------------------------------------------------------------
// The chassis compound's box and mass centre (m5 step 5: Chassis.getMin
// 0x4B3220, getMax 0x4B2DC0, getCM 0x4B2680, read for car info)
// ---------------------------------------------------------------------------
namespace {

// The world pose of a part's cfg frame: x_world = pos + rc * (x - ref_at).
CamM3 part_cfg_rot(const PartObject& p) {
  return cam_mm(p.rot_, part_transpose(p.type_->ref_rot));
}

void box_grow(CamV3* lo, CamV3* hi, bool* any, const CamV3& c) {
  if (!*any) {
    *lo = c;
    *hi = c;
    *any = true;
    return;
  }
  *lo = cam_v((std::min)(lo->x, c.x), (std::min)(lo->y, c.y), (std::min)(lo->z, c.z));
  *hi = cam_v((std::max)(hi->x, c.x), (std::max)(hi->y, c.y), (std::max)(hi->z, c.z));
}

// PE 940 FUN_005008c0 over a compound of shape kind 5 / 6 with children:
// the box around every child body's points grown by the child's margin, in
// the compound's frame. Here for the compound of part p, in p's cfg frame:
// the type's own children (PartTypeDesc::has_box) and, for every part
// installed on p, the eight corners of that part's box (its compound is a
// child with children, so its margin is shape+0x10 = 0), each corner taken
// through the child's frame. `bones` (the car only) leaves out the parts on
// a slot that hangs on a wheel or the steering body: FUN_004c7d20 attaches
// those to that body (flag 0x8000), not to the compound.
// Fork: the exe rebuilds a compound's box in FUN_005032d0 whenever a child
// is attached or detached (FUN_004c7d20, FUN_00501d00, FUN_004cb8d0) and
// expresses it relative to the mass centre of that moment; the host builds
// it from the poses at the time of the query, in the cfg frame (the same
// box for a tree that has not moved since).
bool part_tree_box(const PartObject& p, CamV3* lo, CamV3* hi, int depth,
                   const std::vector<CarObject::SlotBone>* bones) {
  const PartTypeDesc& d = part_type_box(*p.type_);
  bool any = d.has_box;
  if (any) {
    *lo = d.box_lo;
    *hi = d.box_hi;
  }
  if (depth > 64) return any;
  const CamM3 rcp_t = part_transpose(part_cfg_rot(p));
  for (size_t i = 0; i < p.slots_.size(); ++i) {
    const PartObject::Slot& s = p.slots_[i];
    if (!s.other || !(s.flags & 2u)) continue;
    if (bones && i < bones->size() && (*bones)[i].body != CarObject::kBodyRef) continue;
    std::shared_ptr<PartObject> q = part_of(s.other);
    if (!q) continue;
    CamV3 qlo, qhi;
    if (!part_tree_box(*q, &qlo, &qhi, depth + 1, nullptr)) continue;
    const CamM3 rcq = part_cfg_rot(*q);
    for (int k = 0; k < 8; ++k) {
      const CamV3 c = cam_v(k & 1 ? qhi.x : qlo.x, k & 2 ? qhi.y : qlo.y,
                            k & 4 ? qhi.z : qlo.z);
      const CamV3 w = q->pos_ + cam_mul(rcq, c - q->type_->ref_at);
      box_grow(lo, hi, &any, d.ref_at + cam_mul(rcp_t, w - p.pos_));
    }
  }
  return any;
}

// PE 940 FUN_005032d0 on the chassis compound: its origin goes to the mass
// centre of its children, each weighted with 1 / its inverse mass: the
// car's own bodies (PartTypeDesc::com_raw) and every part tree on a slot of
// the reference body. In the car's cfg frame. Fork: the exe moves the
// origin only when the shift is more than 3.2 mm (|C|^2 > 1e-5,
// DAT_005936f8), so it trails the mass centre by up to that; the host
// takes the mass centre itself.
CamV3 car_compound_com(const CarObject& car) {
  const PartTypeDesc& d = *car.type_;
  float mass = d.total_mass;
  CamV3 sum = d.com_raw * d.total_mass;
  const CamM3 rc_t = part_transpose(part_cfg_rot(car));
  for (size_t i = 0; i < car.slots_.size(); ++i) {
    const PartObject::Slot& s = car.slots_[i];
    if (!s.other || !(s.flags & 2u)) continue;
    if (i < car.slot_bones_.size() && car.slot_bones_[i].body != CarObject::kBodyRef) continue;
    std::shared_ptr<PartObject> c = part_of(s.other);
    if (!c) continue;
    float m = 0.f;
    CamV3 sw;
    part_tree_mass(c.get(), &m, &sw, 1);
    if (m <= 0.f) continue;
    // sum of m * (ref_at + rc^T (w - pos)) over the tree's bodies
    sum = sum + d.ref_at * m + cam_mul(rc_t, sw - car.pos_ * m);
    mass += m;
  }
  return mass > 0.f ? sum * (1.f / mass) : CamV3();
}

}  // namespace

// PE 940 Chassis.getCM 0x4B2680: minus the type's `center` (type+0x1F1C)
// through the reference body's world frame (FUN_005060d0), brought into
// the chassis compound's frame (car+0x13EC: minus its position, through
// the transpose of its rotation), negated, less the compound shape's
// +0x40 (FUN_005008c0 leaves there minus the shift of the compound's last
// recentring). In the car's cfg frame that is
//   C - ref_at + refRot * center + s_last
// with C the compound's origin (its mass centre) and ref_at the reference
// body. Fork: s_last is taken as 0: which recentring of the compound was
// the last one depends on the order of the installs and on the callers of
// FUN_004cb8d0 (install, repair, setSlotPos, damage), which the host does
// not replay; its size is one part's mass share times that part's offset
// (Q31).
bool native_car_cm(InvObject* chassis, float out[3]) {
  std::shared_ptr<CarObject> car = car_of(chassis);
  if (!car || !car->car_type_) return false;
  const PartTypeDesc& d = *car->type_;
  const CamV3 c = car_compound_com(*car) - d.ref_at +
                  cam_mul(d.ref_rot, car->car_type_->center);
  out[0] = c.x;
  out[1] = c.y;
  out[2] = c.z;
  return true;
}

// PE 940 Chassis.getMin 0x4B3220 / getMax 0x4B2DC0: the type's `center`
// through the local frames from the reference body up to the chassis
// compound (body+0x1C, while body+0x58 has bit 4 and the body is not
// car+0x13EC) = p = ref_at - C + refRot * center in the compound's frame;
// then the smallest / largest coordinates of the compound shape's hull
// points (shape+0x90 -> +0x24, count +0x20: the eight corners FUN_0050e550
// makes of the box of FUN_005008c0), starting from 0; the result is
//   min: p + min(0, box_lo - C) + 0.1,   max: p + max(0, box_hi - C) - 0.1
// (_DAT_005935d4 = 0.1). In the car's cfg frame, C the mass centre
// (car_compound_com). A car without points answers p + 0.1 / p - 0.1.
bool native_car_bounds(InvObject* chassis, bool want_max, float out[3]) {
  std::shared_ptr<CarObject> car = car_of(chassis);
  if (!car || !car->car_type_) return false;
  const PartTypeDesc& d = *car->type_;
  const CamV3 c = car_compound_com(*car);
  const CamV3 p = d.ref_at - c + cam_mul(d.ref_rot, car->car_type_->center);
  CamV3 lo = CamV3(), hi = CamV3();
  const bool any = part_tree_box(*car, &lo, &hi, 0, &car->slot_bones_);
  constexpr float kPad = 0.1f;  // _DAT_005935d4
  float v[3];
  for (int k = 0; k < 3; ++k) {
    const float pk = k == 0 ? p.x : k == 1 ? p.y : p.z;
    const float ck = k == 0 ? c.x : k == 1 ? c.y : c.z;
    float h = 0.f;
    if (any) {
      const float b = want_max ? (k == 0 ? hi.x : k == 1 ? hi.y : hi.z) - ck
                               : (k == 0 ? lo.x : k == 1 ? lo.y : lo.z) - ck;
      h = want_max ? (std::max)(0.f, b) : (std::min)(0.f, b);
    }
    v[k] = want_max ? pk + h - kPad : pk + h + kPad;
  }
  out[0] = v[0];
  out[1] = v[1];
  out[2] = v[2];
  if (native_type_log()) {
    static int s_logged = 0;
    if (s_logged < 8) {
      ++s_logged;
      const PartTypeDesc& own = part_type_box(d);
      std::fprintf(stderr,
                   "[native-type] Chassis.%s of car '%s': mass centre (%.4f, %.4f, %.4f), box "
                   "(%.4f, %.4f, %.4f) .. (%.4f, %.4f, %.4f)%s (the car's own bodies and click: "
                   "(%.4f, %.4f, %.4f) .. (%.4f, %.4f, %.4f)) -> (%.4f, %.4f, %.4f)\n",
                   want_max ? "getMax" : "getMin",
                   car->type_->cfg_path.c_str(), c.x, c.y, c.z, lo.x, lo.y, lo.z, hi.x,
                   hi.y, hi.z, any ? "" : " (no points)", own.box_lo.x, own.box_lo.y,
                   own.box_lo.z, own.box_hi.x, own.box_hi.y, own.box_hi.z, v[0], v[1], v[2]);
    }
  }
  return true;
}

// PE 940 Chassis.getMaxSteer 0x4B3110 / setMaxSteer 0x4B5030 = header+0xA20
// and Chassis.setAckermann 0x4B47B0 = header+0xA30 of the drive-train
// header the car points to (car+0x2050): the largest steer angle and the
// Ackermann length FUN_004aa830 turns the front wheels with.
// Fork: the header is the TYPE's until the first forceUpdate clones it
// (FUN_004b0910), so a write before that changes every car of the type in
// the exe; the host keeps the two numbers per car from the create on. The
// one shipped caller before the clone, the Chassis constructor, writes back
// what it read.
bool native_car_max_steer(InvObject* chassis, float* value) {
  std::shared_ptr<CarObject> car = car_of(chassis);
  if (!car) return false;
  *value = car->max_steer_;
  return true;
}

bool native_car_set_max_steer(InvObject* chassis, float value) {
  std::shared_ptr<CarObject> car = car_of(chassis);
  if (!car) return false;
  car->max_steer_ = value;
  return true;
}

bool native_car_set_ackermann(InvObject* chassis, float value) {
  std::shared_ptr<CarObject> car = car_of(chassis);
  if (!car) return false;
  car->ackermann_ = value;
  return true;
}

// PE 940 Chassis.forceUpdate 0x4B5890 -> FUN_004aebf0(type, car), the part
// that belongs to the car object. `begin`: car+0x70 &= ~0x800000 (the flag
// the `updatevariables` command sets), in every call. `updatevariables`:
// FUN_0047dd50(node, "updatevariables"), the method of the chassis' script
// instance, which the exe calls when the script field suspend_update is 0,
// after it copied the type's tyre numbers into every wheel. What the
// method reaches writes the wheel records and the slot records
// (Chassis.updatevariables and the updatevariables of brake, rim, tyre,
// spring, shock and suspension of every wheel); no body moves: the wheel
// bodies follow at the next setMatrix, `reset`, `setsteer` or `stop`.
bool native_car_force_update_begin(InvObject* chassis) {
  std::shared_ptr<CarObject> car = car_of(chassis);
  if (!car) return false;
  ++g_part_install_stats.force_updates;
  car->flags_ &= ~0x800000u;
  return true;
}

bool native_car_updatevariables(InvObject* chassis) {
  std::shared_ptr<CarObject> car = car_of(chassis);
  if (!car) return false;
  ++g_part_install_stats.force_scripts;
  part_script_call(car->key_, "updatevariables", "()V", {});
  return true;
}

// ---------------------------------------------------------------------------
// WheelRef
// ---------------------------------------------------------------------------
// The native pointer of a WheelRef is the wheel record w of its car
// (Chassis.getWheel 0x4B4150: car+0x1414 + index x 0x2E4); every native
// below reads or writes that record. None moves a wheel body or marks the
// bodies stale: they follow at the next setMatrix, `reset`, `setsteer` or
// `stop`.
// Fork: the setters also re-arm the trigger spring of the wheel
// (FUN_004f4280: stiffness, damping, the three lengths plus the radius);
// the host has no spring object and reads the record where the exe reads
// the spring (CarWheel::spring_max, CarObject::static_length).
bool native_car_wheel_set(InvObject* chassis, int32_t wheel, int32_t field,
                          const float* v) {
  std::shared_ptr<CarObject> car = car_of(chassis);
  if (!car) return false;
  if (wheel < 0 || static_cast<size_t>(wheel) >= car->wheels_.size())
    return true;
  CarWheel& w = car->wheels_[static_cast<size_t>(wheel)];
  const CarWheelDesc& wd = car->car_type_->wheels[static_cast<size_t>(wheel)];
  // (w+0x3C + w+0x38): the two spring numbers of the cfg, as the create
  // copied them into the record.
  const float droop = w.spring[1] + w.spring[0];
  switch (field) {
    case kNativeWheelRadius:  // 0x4B6E50: w+0x60 (and w+0x5C = rest + r)
      w.radius = v[0];
      break;
    case kNativeWheelRestLen:  // 0x4B6EF0: w+0x50 (and w+0x5C)
      w.rest = v[0];
      break;
    case kNativeWheelMinLen:  // 0x4B6990: w+0x58
      w.min_len = v[0];
      break;
    case kNativeWheelMaxLen:  // 0x4B68B0: w+0x54
      w.max_len = v[0];
      break;
    case kNativeWheelForce:  // 0x4B6380: w+0x44
      w.k = v[0];
      break;
    case kNativeWheelDamping:
      // 0x4B6280 (one number: bound v, rebound 0.7 v, DAT_0059383c) and
      // 0x4B61D0 (both): w+0x48 / w+0x4C. The caller passes both.
      w.damping = v[0];
      w.rebound = v[1];
      break;
    case kNativeWheelSteer:  // 0x4B7010: w+0xC8
      w.steer_factor = v[0];
      break;
    case kNativeWheelYpr:
      // 0x4B70E0: w90 = matrix(ypr) (FUN_005666d0), null = the Ypr of the
      // cfg `wheel` line (desc+0x0C); copied to w+0x110, the steered matrix
      // of the tyre model (Fork: not kept).
      if (v)
        w.w90 = cam_ypr_to_matrix(v[0], v[1], v[2]);
      else
        w.w90 = cam_ypr_to_matrix(wd.ypr[0], wd.ypr[1], wd.ypr[2]);
      break;
    case kNativeWheelPos: {
      // 0x4B6AF0: mount = desc position + (0, desc+0x3C + desc+0x38, 0) +
      // the vector (null = none). The two instant centres move with it and
      // the spring is placed again (Fork: dynamics, not kept). The exe
      // writes the sum as it is, without the place of the reference body
      // that the create takes off (0 in 92 of the 95 car cfgs; no shipped
      // script calls this native).
      CamV3 m = wd.pos;
      m.y = wd.spring[1] + wd.spring[0] + m.y;
      if (v) m = m + cam_v(v[0], v[1], v[2]);
      w.mount = m;
      break;
    }
    case kNativeWheelArm: {
      // 0x4B5F50 (length, p, n): w+0x234 = length; pivot w+0x244 = mount +
      // p with (w+0x3C + w+0x38) taken off y; axis w+0x238 = n normalised
      // (kept as it was when n is the zero vector).
      w.arm_len = v[0];
      w.arm_pivot = cam_v(w.mount.x + v[1], (w.mount.y + v[2]) - droop,
                          w.mount.z + v[3]);
      const float l = std::sqrt(v[4] * v[4] + v[5] * v[5] + v[6] * v[6]);
      if (l != 0.f) {
        const float k = 1.f / l;
        w.arm_axis = cam_v(v[4] * k, v[5] * k, k * v[6]);
      }
      break;
    }
    case kNativeWheelHub: {
      // 0x4B65D0 (length, p1, p2, pc): w+0x254, w+0x258, w+0x264; w+0x270 =
      // mount + pc with (w+0x3C + w+0x38) taken off y. For length 0 (a
      // strut): p2 is normalised and p1 is taken off w+0x270.
      w.hub_len = v[0];
      w.hub_p1 = cam_v(v[1], v[2], v[3]);
      w.hub_p2 = cam_v(v[4], v[5], v[6]);
      w.hub_pc = cam_v(v[7] + w.mount.x, (w.mount.y + v[8]) - droop,
                       w.mount.z + v[9]);
      if (v[0] <= 0.f && v[0] == 0.f) {
        const float l = std::sqrt(cam_len2(w.hub_p2));
        if (l != 0.f) w.hub_p2 = w.hub_p2 * (1.f / l);
        w.hub_pc = w.hub_pc - w.hub_p1;
      }
      break;
    }
    case kNativeWheelOpp: {
      // 0x4B6A30: w+0x250 = the record of wheel n of the same car, none
      // for a number below 0. Fork: the exe does not check the upper end.
      const int n = static_cast<int>(v[0]);
      w.opp = (n >= 0 && static_cast<size_t>(n) < car->wheels_.size()) ? n : -1;
      break;
    }
    default:
      break;
  }
  return true;
}

bool native_car_wheel_get(InvObject* chassis, int32_t wheel, int32_t field,
                          float* v) {
  std::shared_ptr<CarObject> car = car_of(chassis);
  if (!car) return false;
  if (wheel < 0 || static_cast<size_t>(wheel) >= car->wheels_.size())
    return false;
  const CarWheel& w = car->wheels_[static_cast<size_t>(wheel)];
  const CarWheelDesc& wd = car->car_type_->wheels[static_cast<size_t>(wheel)];
  switch (field) {
    case kNativeWheelRadius:  // 0x4B5DC0: w+0x60
      v[0] = w.radius;
      return true;
    case kNativeWheelSteer:  // 0x4B5E00: w+0xC8
      v[0] = w.steer_factor;
      return true;
    case kNativeWheelYpr:  // 0x4B5E40: FUN_00569970(w+0x90)
      cam_matrix_to_ypr(w.w90, &v[0], &v[1], &v[2]);
      return true;
    case kNativeWheelPos:
      // 0x4B5B90: (desc x, w+0x7C - desc y - (desc+0x3C + desc+0x38),
      // desc z): how far the mount was moved up from its cfg place.
      v[0] = wd.pos.x;
      v[1] = (w.mount.y - wd.pos.y) - (wd.spring[1] + wd.spring[0]);
      v[2] = wd.pos.z;
      return true;
    default:
      return false;
  }
}
