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
// FUN_004c7470, getMesh 0x4CC8A0, setMesh 0x4CD840 -> FUN_004c72d0;
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
    render_d3d9_mesh_set_pending_type(i.r, id);
    i.res_id = id;
    break;
  }
  p->mesh_ = desc->mesh_id;
  p->render_type_ = id;
  return true;
}

// PE 940 Part.setMesh 0x4CD840: a handle on the typeof-5 record `id`, and,
// when part+0x94 != 0, FUN_004c72d0(type, part, handle): nothing unless the
// type has a cfg `mesh` line (type+0x1100) and the id differs from
// part+0x94; then the mesh part+0x8C is replaced by the new one in a private
// copy of the main render type (FUN_004e9470, render object vt+8) and
// part+0x8C is bound to the new mesh.
// Fork: only the handle changes (Part.getMesh, and with it the mesh id of a
// saved car); the render instance keeps the mesh of its render object. The
// copy and the replacement belong to the paint work (Part.setTexture has
// the same shape).
bool native_part_set_mesh(InvObject* self, int32_t id) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  if (!p->mesh_ || p->mesh_ == id || !p->type_->mesh_id) return true;
  p->mesh_ = id;
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
