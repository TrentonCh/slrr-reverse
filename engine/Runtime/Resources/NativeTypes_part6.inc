// Fragment 6 of NativeTypes.cpp - included, not compiled on its own.
//
// The `car` type: row 3 of the native type table, `_type_car` 0x59DCD8,
// factory FUN_004b17a0. The chassis entry of a car pack IS the car: one
// typeof-8 record with `script <Chassis subclass>` and `native car <cfg>`;
// the script's Vehicle is one more handle on the same node.
//
// `_type_car` is `_type_part` plus wheels, seats and a drive train:
//   - the init FUN_0049ab20 runs the part's field reset and the same line
//     parser with a 109-word table (0x5DA528) that holds every part word;
//   - the create FUN_004992e0 calls the part's FUN_004c1f90 with a bigger
//     object (0x2284 bytes): same create string, slot table, render
//     entries, `install`, `add_part` list, car handle (= the car itself);
//   - it then makes a root body `car_root` with the chassis compound as its
//     child and, per wheel, three more children: `wheelbone` (the cfg `type`
//     shape), `hub` and `a_arm` (the `wheelbones` sphere); the body of a
//     `steering` line is `steer`;
//   - a slot hangs on the body its `slottype` names: 0 the reference body
//     `bbone`, 11 `steer`, 101+i `wheelbone`, 201+i `hub`, 301+i `a_arm` of
//     wheel i (asm 0x49a643..0x49a6db).
//
// A standing car is posed without physics, and so is every pose here:
//   - GameRef.setPos / setMatrix reach vt+0x1C FUN_004a99b0, which puts the
//     middle of the four static tyre contact points on the given position,
//     levels the body and writes every suspension length;
//   - the wheel bodies are computed in FUN_004aaaf0 from the mount point,
//     the suspension length, the steer angle (FUN_004aa830) and the arm /
//     hub geometry the scripts set;
//   - `stop` / `suspend` put the body tree to sleep.
// Mirrors: FUN_0048fb30 `type`, FUN_0048fcb0 `spring`, FUN_0048fee0 `wheel`,
// FUN_0048cfc0 `wheelbones`, FUN_0048d0e0 `bonepos`, FUN_0048cc80 `arm`,
// FUN_0048cd70 `hub`, FUN_0048daf0 `maxsteer`, FUN_0048de60 `center`,
// FUN_0048d6b0 `mass` (the `steering` and `render` handlers FUN_0048d5f0 /
// FUN_0048d560 are in the cfg reader of fragment 4), FUN_004992e0 create,
// FUN_0049a8e0 wheel renders, FUN_004a99b0 setMatrix, FUN_004aa830 steer
// angles, FUN_004aaaf0 wheel bodies (with FUN_00566400, FUN_004b1ef0),
// FUN_004a2e60 events, FUN_004a15c0 getinfo, FUN_00494910 (the update, as
// far as it runs FUN_004aebf0), Part.getCar 0x4CC640, getCarRef 0x4CC550,
// getSlots 0x4CCED0, getSlotID 0x4CCAA0, getSlotIndex 0x4CCBF0.
//
// Fork, in short (each is marked again where it applies):
//   - no bodies. The car keeps the pose of the reference body (the part
//     object's pos_ / rot_) and, in that body's frame, one pose per wheel
//     for `wheelbone`, `hub` and `a_arm` and one for `steer`. A part on a
//     slot of a wheel or steer body is put on its slot again whenever those
//     poses change (in the exe its compound is a child of that body);
//   - the wheel bodies are recomputed on the next control() after
//     setMatrix; the exe does it in its per-frame update FUN_00494910 and
//     only while the car is large enough on screen;
//   - of FUN_004aaaf0 the plain path, the arm, the zero-length hub (a
//     strut) and the two-point hub (a double wishbone) are here: what the
//     suspension scripts set with WheelRef.setArm / setHub. A wheel with an
//     opposite wheel (a rigid axle, WheelRef.setOppWheel: no shipped
//     script) takes the plain path: the centre the spring ray gives, no
//     arm, no hub;
//   - seats, cameras, the drive train, damage, lamps, flaps, wings and the
//     driving commands are not ported.
// Units: metres, radians, kilograms.

namespace {

constexpr float kCarMaxLenAdd = 0.1f;       // _DAT_005935d4
constexpr float kCarRebound = 0.7f;         // DAT_0059383c
constexpr float kCarSteerEps = 0.001f;      // DAT_00593584
constexpr float kCarLenEps = 0.01f;         // DAT_005935d0
constexpr float kCarSteerWheelTurn = 2.7f;  // header+0xA24 (init, 0x49BAB9)
constexpr float kCarUpStep = 0.02f;         // DAT_00594790

// PE 940 FUN_00566400(matrix, x, y, z, angle): the rotation by `angle`
// about the axis. An axis shorter than 0.001 (|a|^2 < DAT_0059a450) leaves
// the matrix as the caller made it, the identity in every use here; an axis
// whose squared length is not 1 within 1e-6 is normalised first.
CamM3 car_rot_axis(CamV3 a, float angle) {
  constexpr float kTiny = 9.99999997e-07f;  // DAT_0059a450
  CamM3 m = cam_identity();
  const float l2 = cam_len2(a);
  if (l2 < kTiny) return m;
  if (kTiny < std::fabs(l2 - 1.f)) a = a * (1.f / std::sqrt(l2));
  const float c = static_cast<float>(std::cos(static_cast<double>(angle)));
  const float s = static_cast<float>(std::sin(static_cast<double>(angle)));
  const float k = 1.f - c;
  m.m[0][0] = k * a.x * a.x + c;
  m.m[0][1] = k * a.x * a.y - s * a.z;
  m.m[0][2] = k * a.x * a.z + s * a.y;
  m.m[1][0] = k * a.x * a.y + s * a.z;
  m.m[1][1] = k * a.y * a.y + c;
  m.m[1][2] = k * a.y * a.z - s * a.x;
  m.m[2][0] = k * a.x * a.z - s * a.y;
  m.m[2][1] = k * a.y * a.z + s * a.x;
  m.m[2][2] = k * a.z * a.z + c;
  return m;
}

// PE 940 FUN_004b1ef0: -1, 0 or 1.
inline float car_sign(float v) {
  if (v == 0.f) return 0.f;
  return v < 0.f ? -1.f : 1.f;
}

// ---------------------------------------------------------------------------
// The car words of the cfg (keyword table 0x5DA528)
// ---------------------------------------------------------------------------
// Called by parse_part_cfg for a word of a car cfg that no part handler
// took. Every handler acts on the last wheel that a `type` line opened and
// does nothing before the first.
void car_cfg_word(const PartCfgLine& l, PartTypeDesc* d) {
  CarTypePart& c = *d->car;
  CarWheelDesc* w = c.wheels.empty() ? nullptr : &c.wheels.back();
  if (word_is(l.word, "type")) {
    // FUN_0048fb30, "%f %s %f %f %f": mass, shape and its numbers; opens
    // wheel n (at most 16). `sphere` and `box` are shape kinds 1 and 5,
    // anything else is a mesh path (kind 6). desc+0x44, the wheel radius,
    // is the first number behind the shape word.
    if (c.wheels.size() >= 16) return;
    CarWheelDesc nw;
    char shape[1025];
    shape[0] = 0;
    float dims[3] = {0.f, 0.f, 0.f};
    engine_sscanf(l.rest, "%f %1024s %f %f %f", &nw.shape_mass, shape,
                  &dims[0], &dims[1], &dims[2]);
    nw.shape_kind = word_is(shape, "sphere") ? 1 : word_is(shape, "box") ? 5 : 6;
    nw.radius = dims[0];
    c.wheels.push_back(nw);
  } else if (word_is(l.word, "spring")) {
    // FUN_0048fcb0, "%f %f %f" into desc+0x38 / +0x3C / +0x40; a value that
    // is not positive becomes 0.1 / 0.2 / 0.05 ("Invalid spring parameter").
    if (!w) return;
    float s[3] = {0.f, 0.f, 0.f};
    engine_sscanf(l.rest, "%f %f %f", &s[0], &s[1], &s[2]);
    static const float kDefault[3] = {0.1f, 0.2f, 0.05f};
    for (int i = 0; i < 3; ++i) w->spring[i] = s[i] <= 0.f ? kDefault[i] : s[i];
  } else if (word_is(l.word, "wheel")) {
    // FUN_0048fee0, "%f %f %f %f %f %f %f %f %f %f": the centre at full
    // droop, the Ypr of the wheel frame, the steer factor and three numbers
    // of the tyre model. It also presets bonepos (0, 0.4, 0), arm and hub
    // length -1 (none), no opposite wheel, and makes the wheel the "current
    // body" (type+0xFFC = 0x10000000 + index).
    // Fork: the exe leaves the position and the angles unset when the line
    // is short; the host takes zeros.
    if (!w) return;
    float f8 = 0.f, f9 = -1.f, f10 = -1.f;
    w->pos = CamV3();
    w->ypr[0] = w->ypr[1] = w->ypr[2] = 0.f;
    w->steer_factor = 0.f;
    engine_sscanf(l.rest, "%f %f %f %f %f %f %f %f %f %f", &w->pos.x,
                  &w->pos.y, &w->pos.z, &w->ypr[0], &w->ypr[1], &w->ypr[2],
                  &w->steer_factor, &f8, &f9, &f10);
    w->bonepos = cam_v(0.f, 0.4f, 0.f);
    w->arm_len = -1.f;
    w->hub_len = -1.f;
    c.cur_body =
        0x10000000u | static_cast<uint32_t>(c.wheels.size() - 1);
  } else if (word_is(l.word, "wheelbones")) {
    // FUN_0048cfc0: desc+0x50 = a sphere of radius 0.1 and mass 1, the
    // shape of this wheel's `hub` and `a_arm` bodies.
    if (w) w->bones = true;
  } else if (word_is(l.word, "bonepos")) {
    // FUN_0048d0e0, "%f %f %f" into desc+0x58.
    if (w)
      engine_sscanf(l.rest, "%f %f %f", &w->bonepos.x, &w->bonepos.y,
                    &w->bonepos.z);
  } else if (word_is(l.word, "arm")) {
    // FUN_0048cc80, "%f %f %f %f %f %f %f": length, pivot (relative to the
    // wheel position), axis. No shipped cfg has the line.
    if (!w) return;
    float len = 0.f;
    CamV3 p, n;
    engine_sscanf(l.rest, "%f %f %f %f %f %f %f", &len, &p.x, &p.y, &p.z,
                  &n.x, &n.y, &n.z);
    w->arm_len = len;
    w->arm_pivot = w->pos + p;
    w->arm_axis = n;
  } else if (word_is(l.word, "hub")) {
    // FUN_0048cd70, "%f" x 10: length, p1, p2, pc (relative to the wheel
    // position). No shipped cfg has the line.
    if (!w) return;
    float len = 0.f;
    CamV3 p1, p2, pc;
    engine_sscanf(l.rest, "%f %f %f %f %f %f %f %f %f %f", &len, &p1.x,
                  &p1.y, &p1.z, &p2.x, &p2.y, &p2.z, &pc.x, &pc.y, &pc.z);
    w->hub_len = len;
    w->hub_p1 = p1;
    w->hub_p2 = p2;
    w->hub_pc = w->pos + pc;
  } else if (word_is(l.word, "maxsteer")) {
    // FUN_0048daf0, "%f %f": the largest steer angle (header+0xA20, default
    // 0.7) and the Ackermann length (header+0xA30, default 2.0).
    engine_sscanf(l.rest, "%f %f", &c.max_steer, &c.ackermann);
  } else if (word_is(l.word, "center")) {
    // FUN_0048de60, "%f %f %f" into type+0x1F1C (0 in every shipped cfg).
    c.center = CamV3();
    engine_sscanf(l.rest, "%f %f %f", &c.center.x, &c.center.y, &c.center.z);
  } else if (word_is(l.word, "mass")) {
    // FUN_0048d6b0, "%f" into type+0x1F34 (in no shipped cfg).
    engine_sscanf(l.rest, "%f", &c.mass_word);
  }
  // Read past (Fork): wheelpars, wheeldeform, instant_center, camera,
  // rollbar, wing, flap, flapsfx, friction, skid, sw_radius, collision_ps,
  // the drive train words (rpm_idle .. clutch), the seat words (driver ..
  // handbrake), the cockpit and osd words, the sfx words, deformdmgcap,
  // material, damagedef, velfactor, turn, roll, pitch, steerhelp,
  // cheat_control, notify.
}

// PE 940 PTR_DAT_005dadf0+0x1C4: the length of the world's gravity vector,
// which the ground type sets from the script field Config.gravity (9.81).
float car_gravity() {
  return std::fabs(cam_config("gravity"));
}

// ---------------------------------------------------------------------------
// The instance (0x2284 bytes, FUN_00490100 over the part object)
// ---------------------------------------------------------------------------
// One 0x2E4-byte wheel record (car+0x1414).
struct CarWheel {
  CamV3 mount;                   // +0x78, frame of the reference body
  float spring[3] = {0.f, 0.f, 0.f};  // +0x38 / +0x3C / +0x40
  float k = 0.f;                 // +0x44
  float damping = 0.f;           // +0x48
  float rebound = 0.f;           // +0x4C
  float rest = 0.f;              // +0x50
  float max_len = 0.f;           // +0x54
  float min_len = -1.f;          // +0x58
  float radius = 0.f;            // +0x60
  CamV3 bonepos;                 // +0x84
  CamM3 w90 = cam_identity();    // +0x90
  float steer_factor = 0.f;      // +0xC8
  float steer_angle = 0.f;       // +0xE4
  float spin = 0.f;              // +0xE8
  // The lower arm (WheelRef.setArm, or the cfg `arm` line): its length
  // (not positive = none), its axis and its pivot, a point of the reference
  // body's frame like the mount.
  float arm_len = -1.f;          // +0x234
  CamV3 arm_axis;                // +0x238
  CamV3 arm_pivot;               // +0x244
  // +0x250: the record of the opposite wheel of a rigid axle
  // (WheelRef.setOppWheel; no shipped script), -1 = none.
  int opp = -1;
  // The hub (WheelRef.setHub, or the cfg `hub` line): its length (below 0 =
  // none, 0 = a strut: the hub turns about the lower ball joint p1 so that
  // its direction p2 points at the strut top pc), p1 and p2 in the hub's
  // frame, pc in the reference body's (for length 0 with p1 taken off).
  float hub_len = -1.f;          // +0x254
  CamV3 hub_p1;                  // +0x258
  CamV3 hub_p2;                  // +0x264
  CamV3 hub_pc;                  // +0x270
  // The spring s (w+0x30, a c_triggerspring): s+0xF0, the current length
  // from the mount to the contact point.
  float length = 0.f;
  // s+0x50, the longest the spring gets, as FUN_004f4280 keeps it at every
  // WheelRef setter: max length + radius, or rest + radius when that is not
  // positive.
  float spring_max() const {
    const float m = max_len + radius;
    return m > 0.f ? m : rest + radius;
  }
  bool bones = false;            // the wheel has `hub` and `a_arm` bodies
  // Fork: the local frames of the wheel's three bodies (children of
  // `car_root`), in the frame of the reference body.
  CamV3 wb_pos, hub_pos, arm_pos;
  CamM3 wb_rot = cam_identity();
  CamM3 hub_rot = cam_identity();
  CamM3 arm_rot = cam_identity();
};

class CarObject final : public PartObject {
 public:
  const CarTypePart* car_type_ = nullptr;
  std::vector<CarWheel> wheels_;      // car+0x1414
  // The body a slot hangs on (FUN_004992e0, the loop over the slots), by
  // index into slots_.
  enum SlotBody : uint8_t { kBodyRef = 0, kBodySteer, kBodyWheel, kBodyHub, kBodyArm };
  struct SlotBone {
    SlotBody body = kBodyRef;
    uint8_t wheel = 0;
  };
  std::vector<SlotBone> slot_bones_;
  // The drive-train header (car+0x2050; the type's until forceUpdate
  // clones it): the three values a standing car reads.
  float max_steer_ = 0.7f;        // +0xA20
  float steer_turn_ = kCarSteerWheelTurn;  // +0xA24
  float ackermann_ = 2.f;         // +0xA30
  float steer_ = 0.f;             // car+0x1DDC: steer input, -1..1
  float steer_seen_ = 0.f;        // car+0x1DE0: the input the angles are for
  // car+0x70 is the part's state word (PartObject::flags_; 0x60 after the
  // create). The car's bits: 4 stopped, 0x40000 brake, 0x100000 wheel
  // bodies stale, 0x200000 only their spin, 0x800000 updatevariables asked
  // for.
  // The root of the body tree is asleep (body flag 0x80).
  bool asleep_ = true;
  // Fork: the local frame of the `steer` body in the reference body.
  bool has_steer_ = false;
  CamV3 steer_pos_;
  CamM3 steer_rot_ = cam_identity();
  // Fork: commands the host takes without acting on them yet: the numbers
  // they carried, and how often each word came.
  struct Stored {
    int32_t transmission = 0;
    float steerhelp = 0.f, asr = 0.f, abs = 0.f, esp = 0.f, cruise = 0.f;
    float difflock = 0.f;
    float damage_multiplier = 1.f;  // car+0x1F54
    int32_t team = 0;               // car+0x1F40
    int taken = 0;
  } stored_;

  // The mass of the chassis compound (1 / inverse mass of car+0x13EC).
  float compound_mass();
  float mass_kept_ = 0.f;     // Fork: the sum, good while the part trees
  uint32_t mass_gen_ = 0;     // are at this g_part_tree_gen
  // The suspension length under the car's static load.
  float static_length(const CarWheel& w, float load_per_wheel) const {
    return (w.radius + w.rest) - load_per_wheel / w.k;
  }
  void make_instances() override;
  void steer_angles();         // FUN_004aa830
  void wheel_frames();         // FUN_004aaaf0
  void wheel_frame(size_t wi, bool geometry);
  void frames_changed();
  void place_own_instances();
  SlotBone slot_bone(int32_t slot_id) const;
  CamV3 ground_point();        // getinfo 2

  void slot_frame(int32_t slot_id, CamV3* pos, CamM3* rot) override;
  bool set_matrix(const float pos[3], const float ypr[3]) override;
  bool event(int32_t type, const char* text) override;
  bool getinfo(int32_t query, NativeInfo* out) override;
  bool get_ori(float ypr[3]) override;
  bool get_body_pos(float pos[3]) override;
  void control(float dt) override;
};

// The chassis compound holds the car's own bodies and the compound of
// every part installed on a slot of the reference body, with everything
// installed on that part (FUN_004c7d20 attaches a part's compound to the
// PARENT of the slot's body, the compound, unless that body has flag
// 0x8000). The wheel bodies and `steer` carry 0x8020 from the create, so a
// part on one of their slots hangs under that body and is not in this sum.
float CarObject::compound_mass() {
  if (mass_gen_ == g_part_tree_gen) return mass_kept_;
  const uint32_t gen = g_part_tree_gen;
  float mass = type_->total_mass;
  CamV3 unused;
  for (size_t i = 0; i < slots_.size(); ++i) {
    const Slot& s = slots_[i];
    if (!s.other || !(s.flags & 2u)) continue;
    if (i < slot_bones_.size() && slot_bones_[i].body != kBodyRef) continue;
    if (std::shared_ptr<PartObject> c = part_of(s.other))
      part_tree_mass(c.get(), &mass, &unused, 1);
  }
  mass_kept_ = mass;
  mass_gen_ = gen;
  return mass;
}

CarObject::SlotBone CarObject::slot_bone(int32_t slot_id) const {
  for (size_t i = 0; i < slots_.size() && i < slot_bones_.size(); ++i)
    if (slots_[i].id == slot_id) return slot_bones_[i];
  return SlotBone();
}

// getinfo 0x2F of a car: the bone handle of the slot record, bound at the
// create by the slot's `slottype`.
void CarObject::slot_frame(int32_t slot_id, CamV3* pos, CamM3* rot) {
  const SlotBone b = slot_bone(slot_id);
  CamV3 lp;
  CamM3 lr = cam_identity();
  bool local = true;
  switch (b.body) {
    case kBodySteer:
      lp = steer_pos_;
      lr = steer_rot_;
      break;
    case kBodyWheel:
      lp = wheels_[b.wheel].wb_pos;
      lr = wheels_[b.wheel].wb_rot;
      break;
    case kBodyHub:
      lp = wheels_[b.wheel].hub_pos;
      lr = wheels_[b.wheel].hub_rot;
      break;
    case kBodyArm:
      lp = wheels_[b.wheel].arm_pos;
      lr = wheels_[b.wheel].arm_rot;
      break;
    default:
      local = false;
      break;
  }
  if (!local) {
    *pos = pos_;
    *rot = rot_;
    return;
  }
  *pos = pos_ + cam_mul(rot_, lp);
  *rot = cam_mm(rot_, lr);
}

// PE 940 FUN_004aa830: the steer angle of every wheel from the steer input
// (Ackermann: both front wheels turn about one point on the line of the
// rear axle), and the turn of the `steer` body.
void CarObject::steer_angles() {
  const float a = max_steer_ * steer_;
  const float L = ackermann_;
  for (CarWheel& w : wheels_) {
    if (std::fabs(a) <= kCarSteerEps) {
      w.steer_angle = 0.f;
    } else if (L == 0.f) {
      w.steer_angle = w.steer_factor * a;
    } else {
      // FUN_00570fcb / FUN_00570e7e: sin and cos; R = L / tan a.
      const float s = static_cast<float>(std::sin(static_cast<double>(a)));
      const float c = static_cast<float>(std::cos(static_cast<double>(a)));
      const float R = L / (s / c);
      if (R <= 0.f) {
        const float t = static_cast<float>(std::atan2(
            static_cast<double>(L), static_cast<double>(-R - w.mount.x)));
        w.steer_angle = -(t * w.steer_factor);
      } else {
        const float t = static_cast<float>(std::atan2(
            static_cast<double>(L), static_cast<double>(w.mount.x + R)));
        w.steer_angle = t * w.steer_factor;
      }
    }
    // w+0x110 = w90 * Ry(angle), the steered matrix of the tyre model, is
    // not kept (Fork: dynamics).
  }
  if (has_steer_) {
    // The local rotation of the `steer` body: the Ypr of its cfg line with
    // header+0xA24 x steer input added to the roll.
    const CarTypePart& ct = *car_type_;
    steer_rot_ = cam_ypr_to_matrix(ct.steer_ypr[0], ct.steer_ypr[1],
                                   steer_turn_ * steer_ + ct.steer_ypr[2]);
  }
  flags_ |= 0x100000u;
}

// PE 940 FUN_004aaaf0: the local frames of `a_arm`, `hub` and `wheelbone`
// of every wheel, when the car has flag 0x100000 (geometry) or 0x200000
// (spin only); it clears both. Per wheel, with C the centre the spring ray
// gives, up = column 1 of w90:
//   C = mount + up x (radius - min(length, s+0x50)); with a hub (hub length
//       >= 0) C.y += p1.y: the lower ball joint, p1 being where that joint
//       is in the hub's frame;
//   the arm (arm length > 0, no opposite wheel): v = C - pivot, t = n . v,
//       r = sqrt(|v|^2 - t^2), ang = atan2(v.y, sqrt(r^2 - v.y^2)),
//       A = rot(n, sign(v.x n.z - v.z n.x) x ang),
//       C = pivot + n t + (arm length / r) x (v - n t): the radial distance
//       from the arm's axis is forced to the arm length;
//   `a_arm` = (C, A), A the identity without an arm;
//   S = rot((0, 1, 0), steer angle);
//   no hub (hub length < 0; asm 0x4abd2e..0x4abd4d): M = S x (A x w90),
//       H = C;
//   a strut (hub length == 0): u = pc - C, c = (p1 . p2) / |p1|,
//       disc = (c^2 - 1) |p1|^2 + |u|^2; when disc > 0: q = c |p1| +
//       sqrt(disc) (minus the root when that is negative), g = p2 q - p1,
//       axis = g x u; when the axis is not 0: M = rot(u / |u|, steer
//       angle) x rot(axis / |axis|, acos((g . u) / |u|^2)) (the angle 0 for
//       a cosine >= 1, pi for one <= -1), else M = w90; H = C - M p1. With
//       disc <= 0: M = w90, H = C;
//   a two-point hub (hub length L > 0, asm 0x4ab4c7..0x4ab909; a double
//       wishbone: p2 is the upper ball joint in the hub's frame, pc the
//       pivot of an upper arm of length L): d = C - pc, e = p2 - p1,
//       h = (d.z, 0, -d.x) / sqrt(d.x^2 + d.z^2) (left as it is when that
//       root is 0), m = (d x h) / |d|, c = (|d|^2 + |e|^2 - L^2) /
//       (2 |d| |e|); when c <= 1: T = m sqrt(|e|^2 - (c |e|)^2) -
//       d (c |e| / |d|), the direction from the lower to the upper joint
//       that puts the upper one at L from pc; axis = e x T; when the axis
//       is not 0: M = S x (rot(axis / |axis|, acos((T . e) / |e|^2)) x w90)
//       (the angle 0 for a cosine >= 1, pi for one <= -1), else M = w90;
//       H = C - M p1. With c > 1: M = w90, H = C;
//   `hub` = (H, M); `wheelbone` = (H - M' x shape centre, M') with M' = M x
//       matrix(Ypr(0, spin, 0)) (asm 0x4abf8d..0x4ac0e4).
// The exe adds B, the place of the cfg origin in `car_root`, to mount,
// pivot and pc; the host's frames are those of the reference body.
// Spin only: H and M are read back from the `hub` body; a wheel without one
// computes the plain centre and Ry(steer angle) x w90.
// Fork: (M3 spec Q2) the exe has one more branch, the rigid axle of a wheel
// with an opposite wheel (WheelRef.setOppWheel, asm 0x4aae6e..0x4ab19c: no
// shipped script): the two centres are pulled together to the hub length
// and `a_arm` is turned by the tilt of the line between them. It is not
// transcribed. Such a wheel takes the plain path here, the one of a wheel
// without arm and hub: the centre the spring ray gives (no p1.y), A the
// identity, M = S x w90, H = C; one log line per car type says so.
// The two-point hub is what the four stock_suspension_* scripts set; their
// class files are not in the install, so no run reaches it.
// The exe poses the linked renders of a wheel here, right after its hub
// (FUN_004afbf0); on the host each part keeps the linked instances it owns
// on their slots (NativeTypes_part8.inc).
// Fork: the centre of the wheel body's shape is taken as (0, 0, 0): the
// shape of every shipped `type` line is a sphere.
void CarObject::wheel_frame(size_t wi, bool geometry) {
  CarWheel& w = wheels_[wi];
  CamV3 H = w.hub_pos;
  CamM3 M = w.hub_rot;
  if (geometry) {
    float len = w.spring_max();        // s+0x50
    if (w.length <= len) len = w.length;  // s+0xF0
    const CamV3 up = cam_col(w.w90, 1);
    CamV3 C = w.mount + up * (w.radius - len);
    // Fork: (Q2) a wheel of a rigid axle keeps the centre of the ray (the
    // exe lowers it by p1.y like every wheel with a hub, and then runs the
    // axle branch).
    const bool axle = w.opp >= 0;
    if (!axle && 0.f <= w.hub_len) C.y = w.hub_p1.y + C.y;
    CamM3 A = cam_identity();
    if (!axle && 0.f < w.arm_len) {
      const CamV3 P = w.arm_pivot;
      const CamV3 n = w.arm_axis;
      const CamV3 v = C - P;
      const float dist = std::sqrt(cam_len2(v));
      const float t = cam_dot(n, v);
      const float r = std::sqrt(dist * dist - t * t);
      // asm: FLD v.y, FLD sqrt(r^2 - v.y^2), FPATAN.
      const float ang = static_cast<float>(
          std::atan2(static_cast<double>(v.y),
                     static_cast<double>(std::sqrt(r * r - v.y * v.y))));
      const float side = car_sign(v.x * n.z + v.y * 0.f + v.z * -n.x);
      A = car_rot_axis(n, side * ang);
      const float k = w.arm_len / r;
      C = cam_v(k * (-t * n.x + v.x) + n.x * t + P.x,
                k * (n.y * -t + v.y) + n.y * t + P.y,
                k * (n.z * -t + v.z) + n.z * t + P.z);
    }
    w.arm_pos = C;
    w.arm_rot = A;
    const CamM3 S = car_rot_axis(cam_v(0.f, 1.f, 0.f), w.steer_angle);
    M = w.w90;
    H = C;
    if (axle || !(0.f <= w.hub_len)) {
      // No hub (asm 0x4abd2e..0x4abd4d), and (Fork, Q2) the wheel of a
      // rigid axle, whose A is the identity here.
      if (axle) {
        static std::vector<int32_t> s_said;  // main thread
        const int32_t tid = type_->type_id;
        if (std::find(s_said.begin(), s_said.end(), tid) == s_said.end()) {
          s_said.push_back(tid);
          std::fprintf(stderr,
                       "[native-type] car type 0x%08X '%s' wheel %zu: "
                       "opposite wheel set (rigid axle); that branch of "
                       "FUN_004aaaf0 is not ported, the wheel is posed on "
                       "the plain path (no arm, no hub)\n",
                       static_cast<unsigned>(tid),
                       rpak_find_entry(tid) ? rpak_find_entry(tid)->name.c_str()
                                            : "",
                       wi);
        }
      }
      M = cam_mm(S, cam_mm(A, M));
    } else if (w.hub_len == 0.f) {
      CamV3 p2 = w.hub_p2;  // FUN_004b1930: normalised
      const float l2 = std::sqrt(cam_len2(p2));
      if (l2 != 0.f) p2 = p2 * (1.f / l2);
      const CamV3 p1 = w.hub_p1;
      const CamV3 u = w.hub_pc - C;
      const float lp1 = std::sqrt(cam_len2(p1));
      const float lu = std::sqrt(cam_len2(u));
      const float lu2 = lu * lu;
      const float c = cam_dot(p1, p2) / lp1;
      const float disc = (c * c - 1.f) * lp1 * lp1 + lu2;
      if (0.f < disc) {
        const float root = std::sqrt(disc);
        float q = c * lp1 + root;
        if (q < 0.f) q = c * lp1 - root;
        const CamV3 g = p2 * q - p1;
        CamV3 axis = cam_cross(g, u);
        const float a2 = cam_len2(axis);
        if (0.f < a2) {
          axis = axis * (1.f / std::sqrt(a2));
          const float cosang = cam_dot(g, u) / lu2;
          float ang = 0.f;
          if (cosang < 1.f) {
            ang = kPi;  // DAT_005935a4
            if (-1.f < cosang)
              ang = static_cast<float>(std::acos(static_cast<double>(cosang)));
          }
          M = cam_mm(car_rot_axis(u * (1.f / lu), w.steer_angle),
                     car_rot_axis(axis, ang));
        }
        H = C - cam_mul(M, p1);
      }
    } else {
      // The two-point hub (asm 0x4ab4c7..0x4ab909). The exe divides by |d|
      // and |e| without a test, and so does this.
      const CamV3 d = C - w.hub_pc;
      const CamV3 e = w.hub_p2 - w.hub_p1;
      const float ld = std::sqrt(cam_len2(d));
      const float le = std::sqrt(cam_len2(e));
      CamV3 h = cam_v(d.z, 0.f, -d.x);
      const float lh = std::sqrt(h.z * h.z + d.z * d.z);
      if (lh != 0.f) h = h * (1.f / lh);
      const float inv_ld = 1.f / ld;
      const CamV3 m = cam_cross(d, h) * inv_ld;
      const float le2 = le * le;
      const float c =
          ((ld * ld + le2) - w.hub_len * w.hub_len) / ((ld + ld) * le);
      if (c <= 1.f) {
        const float along = c * le;
        const float back = -(along * inv_ld);
        const float side = std::sqrt(le2 - along * along);
        const CamV3 T = m * side + d * back;
        CamV3 axis = cam_cross(e, T);
        const float a2 = cam_len2(axis);
        if (0.f < a2) {
          axis = axis * (1.f / std::sqrt(a2));
          const float cosang = cam_dot(T, e) / le2;
          float ang = 0.f;
          if (cosang < 1.f) {
            ang = kPi;  // DAT_005935a4
            if (-1.f < cosang)
              ang = static_cast<float>(std::acos(static_cast<double>(cosang)));
          }
          // asm 0x4ab835..0x4ab859: the matrix that starts as w90 takes
          // the rotation and then S (FUN_00566a70 twice).
          M = cam_mm(S, cam_mm(car_rot_axis(axis, ang), M));
        }
        H = C - cam_mul(M, w.hub_p1);
      }
    }
    w.hub_pos = H;
    w.hub_rot = M;
  } else if (!w.bones) {
    // Spin only, no `hub` body to read back.
    float len = w.spring_max();
    if (w.length <= len) len = w.length;
    H = w.mount + cam_col(w.w90, 1) * (w.radius - len);
    M = cam_mm(car_rot_axis(cam_v(0.f, 1.f, 0.f), w.steer_angle), w.w90);
    w.hub_pos = H;
    w.hub_rot = M;
  }
  w.wb_rot = cam_mm(M, cam_ypr_to_matrix(0.f, w.spin, 0.f));
  w.wb_pos = H;
}

void CarObject::wheel_frames() {
  if (!(flags_ & 0x300000u)) return;
  const bool geometry = (flags_ & 0x100000u) != 0;
  for (size_t wi = 0; wi < wheels_.size(); ++wi) wheel_frame(wi, geometry);
  flags_ &= ~0x300000u;
  frames_changed();
}

// The car's own render instances that follow a wheel or the steering body:
// FUN_004c2b10 links bone00 of an entry with flag 0x8000 to the `steer`
// body and FUN_0049a8e0 that of a wheel render to the `wheelbone`, both
// with the entry's offset and rotation as written (FUN_004ec470).
// Fork: the instance hangs under the car's key, so its transform is the
// body's local frame times the entry (taken as a pose in the body's frame,
// like part_render_local).
void CarObject::place_own_instances() {
  for (const Inst& i : insts_) {
    if (!i.r) continue;
    const PartEntry& e = type_->entries[static_cast<size_t>(i.entry)];
    CamV3 at;
    CamM3 rot = cam_identity();
    if ((e.flags & 0x8000u) && has_steer_) {
      at = steer_pos_ + cam_mul(steer_rot_, e.off);
      rot = cam_mm(steer_rot_, e.rot);
    } else if (e.flags & 0x1000u) {
      const size_t wi = e.flags >> 24;
      if (wi >= wheels_.size()) continue;
      at = wheels_[wi].wb_pos + cam_mul(wheels_[wi].wb_rot, e.off);
      rot = cam_mm(wheels_[wi].wb_rot, e.rot);
    } else {
      continue;
    }
    float yaw = 0.f, pitch = 0.f, roll = 0.f;
    cam_matrix_to_ypr(rot, &yaw, &pitch, &roll);
    render_d3d9_mesh_set_transform(i.r, at.x, at.y, at.z, yaw, pitch, roll,
                                   1.f, 1.f, 1.f);
  }
}

// Fork: in the exe a part on a slot of a wheel or steer body is a child
// body of that body (FUN_004c7d20) and moves when FUN_004aaaf0 or
// FUN_004aa830 writes the body's local frame. Here each such part is put
// on its slot again (FUN_004cb8d0), which re-poses everything on it.
void CarObject::frames_changed() {
  place_own_instances();
  for (size_t i = 0; i < slots_.size(); ++i) {
    const Slot& s = slots_[i];
    if (!s.other || !(s.flags & 2u)) continue;
    if (i >= slot_bones_.size() || slot_bones_[i].body == kBodyRef) continue;
    if (std::shared_ptr<PartObject> c = part_of(s.other)) c->pose_on_slot(1);
  }
  // FUN_004aa7d0: the linked renders on the car's own list.
  part_place_linked(this);
}

// vt+0x2C FUN_00498300 runs the seat controllers. The per-frame update
// FUN_00494910 starts with `if (car+0x70 & 0x800000) FUN_004aebf0`: the
// body of Chassis.forceUpdate, which the `updatevariables` command asked
// for (a part sends it to its car when it was installed or leaves,
// FUN_004ca390 / FUN_004ca980), and recomputes the wheel bodies.
// Fork: both here, once per game tick. FUN_004aebf0 is the host's
// Chassis.forceUpdate native (it clears the flag through
// native_car_force_update_begin); a car without script instance only loses
// the flag. The wheel bodies follow when a flag asks for it (set by
// setMatrix; the commands `reset`, `setsteer` and `stop` recompute at once
// as the exe does).
void CarObject::control(float /*dt*/) {
  if (flags_ & 0x800000u) {
    flags_ &= ~0x800000u;
    bool scripted = false;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto it = g_refs.find(key_);
      scripted = it != g_refs.end() && !it->second.script_class.empty();
    }
    if (scripted) {
      ++g_part_install_stats.force_from_update;
      java_game_parts_bodypart_Chassis_forceUpdate(key_);
    }
  }
  if (flags_ & 0x300000u) wheel_frames();
}

// PE 940 FUN_004a15c0 case 2: the world position of the reference body
// and, with four or more wheels, added to its y: 1 / n times the sum over
// wheels 0..3 of the static contact height in the cfg frame,
//   mount.y - ((radius + rest) - (|g| x mass / n) / k).
// The term is added in world space without rotation: the point on the
// ground under the cfg origin.
CamV3 CarObject::ground_point() {
  CamV3 p = pos_;
  const int n = static_cast<int>(wheels_.size());
  if (n > 3) {
    const float inv_n = 1.f / static_cast<float>(n);
    const float load = car_gravity() * compound_mass() * inv_n;
    float sum = 0.f;
    for (int i = 3; i >= 0; --i) {
      const CarWheel& w = wheels_[static_cast<size_t>(i)];
      sum += w.mount.y - static_length(w, load);
    }
    p.y = inv_n * sum + p.y;
  }
  return p;
}

// vt+0x3C FUN_004a15c0. Ported: 2 the ground point, 7 the kind of object
// (5, GIR_CAT_VEHICLE); the rest from the part's table, which has the
// same rows as the car's for 6 (the type), 0xB (GII_SIZE, the chassis
// compound), 0xF, 0x27 (free slots), 0x28, 0x29 (41: FUN_004ca300), 0x2B
// (the car handle: a car answers with itself), 0x31, 0x37 (55: type+0x115C,
// 2 unless the cfg says otherwise) and 0x3F (m6 step 3). Fork: 0x2F of a
// car also fills a wheel-typed slot's frame (exe_car 6.1), not ported;
// the car has no 0x17, 0x47, 0x49 row in the exe, so GameRef.getInfo does
// not ask the car object for those (native_part_getinfo,
// NativeTypes_part5.inc).
bool CarObject::getinfo(int32_t query, NativeInfo* out) {
  switch (query) {
    case 2: {
      const CamV3 p = ground_point();
      out->v[0] = p.x;
      out->v[1] = p.y;
      out->v[2] = p.z;
      return true;
    }
    case 7:
      out->i = 5;
      return true;
    default:
      return PartObject::getinfo(query, out);
  }
}

// getinfo 1 of a car is the `bbone` node (car+0x34 / +0x38), so
// GameRef.getOri and the camera's `look` read the reference body: its Ypr
// with the levelling pitch and roll of setMatrix, and its position, the
// cfg origin.
bool CarObject::get_ori(float ypr[3]) {
  ypr[0] = ypr_[0];
  ypr[1] = ypr_[1];
  ypr[2] = ypr_[2];
  return true;
}

bool CarObject::get_body_pos(float pos[3]) {
  pos[0] = pos_.x;
  pos[1] = pos_.y;
  pos[2] = pos_.z;
  return true;
}

// vt+0x1C FUN_004a99b0 ("fffffffffffffffi": position, Ypr, velocity,
// angular velocity, steer, spin add, pitch add, an int; GameRef.setMatrix
// and setPos send the first six and the rest are 0).
// With four or more wheels the position is where the middle of the static
// contact points of wheels 0..3 goes, and the body is tilted so that those
// four points are level.
bool CarObject::set_matrix(const float pos[3], const float ypr_in[3]) {
  float ypr[3] = {ypr_in[0], ypr_in[1], ypr_in[2]};
  const int n = static_cast<int>(wheels_.size());
  // local_80 = |g| x (1 / inverse mass of the chassis compound).
  const float weight = car_gravity() * compound_mass();
  const CamV3 center = car_type_->center;
  CamM3 R = cam_identity();
  CamV3 P;
  if (n < 4) {
    R = cam_ypr_to_matrix(ypr[0], ypr[1], ypr[2]);
    P = cam_v(pos[0], pos[1], pos[2]) + cam_mul(R, center);
  } else {
    const float load = weight / static_cast<float>(n);
    const CarWheel& w0 = wheels_[0];
    const CarWheel& w1 = wheels_[1];
    const CarWheel& w2 = wheels_[2];
    const CarWheel& w3 = wheels_[3];
    // The contact height of each wheel under the static load, cfg frame.
    const float y0 = w0.mount.y - static_length(w0, load);
    const float y1 = w1.mount.y - static_length(w1, load);
    const float y2 = w2.mount.y - static_length(w2, load);
    const float y3 = w3.mount.y - static_length(w3, load);
    const float cx = (w3.mount.x + w2.mount.x + w1.mount.x + w0.mount.x) * 0.25f;
    const float cz = (w1.mount.z + w0.mount.z + w2.mount.z + w3.mount.z) * 0.25f;
    const float cy = (y3 + y2 + y0 + y1) * 0.25f;
    const float dz = ((w3.mount.z + (w2.mount.z - w0.mount.z)) - w1.mount.z) * 0.5f;
    const float dy_lr = ((y3 + (y1 - y0)) - y2) * 0.5f;
    const float dx = ((w3.mount.x + (w1.mount.x - w0.mount.x)) - w2.mount.x) * 0.5f;
    const float dy_fr = ((y3 + (y2 - y0)) - y1) * 0.5f;
    // asm 0x4a9db6..0x4a9deb: FLD dy, FLD d, FPATAN.
    ypr[1] += static_cast<float>(
        std::atan2(static_cast<double>(dy_fr), static_cast<double>(dz)));
    ypr[2] -= static_cast<float>(
        std::atan2(static_cast<double>(dy_lr), static_cast<double>(dx)));
    R = cam_ypr_to_matrix(ypr[0], ypr[1], ypr[2]);
    const CamV3 c = cam_mul(R, cam_v(cx, cy, cz));
    // min(c.y, -(R x center).y): asm 0x4a9ecd..0x4a9ed9.
    float drop = -cam_mul(R, center).y;
    if (c.y <= drop) drop = c.y;
    P = cam_v(pos[0] - c.x, pos[1] - drop, pos[2] - c.z);
  }
  // FUN_004eac50(car+0x2C, &P, &R): `bbone` gets that pose (the root of its
  // body tree is moved); velocity and angular velocity of the root body
  // are the event's (0).
  set_ref_pose(P, R, ypr, 0);
  // car+0x12C = the given position + 0.02 x the body's up axis.
  create_pos_ = cam_v(pos[0], pos[1], pos[2]) + cam_col(R, 1) * kCarUpStep;
  // The steer input of the event (0), and the angles when it changed.
  const float steer = 0.f;
  steer_ = steer;
  if (kCarSteerEps < std::fabs(steer_seen_ - steer)) {
    steer_angles();
    steer_seen_ = steer;
  }
  // The body was placed: every suspension length goes to its static value
  // when it is more than 1 cm off (the pitch add of the event is 0).
  for (CarWheel& w : wheels_) {
    const float target = static_length(w, weight / static_cast<float>(n));
    if (kCarLenEps < std::fabs(w.length - target)) {
      w.length = target;
      flags_ |= 0x100000u;
    }
  }
  // FUN_00506d90: the root body is put to sleep.
  asleep_ = true;
  flags_ &= 0xfffcffffu;
  return true;
}

// vt+0x38 FUN_004a2e60. The car has its own handler and does not chain to
// the part's. Events 4 (damage), 1 (collision), 0x20 (a wheel trigger),
// 0x80, 0x200 and 0x400000 are not ported; of the commands (event 0x10)
// the ones a standing car receives are.
bool CarObject::event(int32_t type, const char* text) {
  if (type != 0x10) return false;
  if (!text) text = "";
  char word[1025];
  word[0] = 0;
  engine_sscanf(text, "%1024s", word);
  const int n = static_cast<int>(wheels_.size());
  auto is = [&](const char* w) { return word_is(word, w); };
  // Fork (m6 step 3 rig, SLRR_PE_NATIVE_TYPE_LOG): the commands that wake,
  // reset or suspend the car, one line each (the Mechanic sends `reset`
  // instead of `wakeup` while the car is on the lift, Mechanic.java:244).
  if (native_getinfo_log() && (is("reset") || is("wakeup") || is("suspend")))
    std::fprintf(stderr, "[native-type] car command '%s' (node 0x%08X) "
                 "t=%.2f\n", word,
                 static_cast<unsigned>(part_node_id(key_)),
                 native_loop_time());

  if (is("stop")) {
    // flags |= 4, the wheels' spin state zeroed, the wheel bodies now, the
    // root body asleep (FUN_004ae5a0 -> FUN_00506d90).
    flags_ |= 4u;
    wheel_frames();
    asleep_ = true;
    return true;
  }
  if (is("start")) {
    flags_ &= 0xfffbfffbu;
    return true;
  }
  if (is("suspend")) {
    asleep_ = true;
    return true;
  }
  if (is("wakeup")) {
    asleep_ = false;  // FUN_0048ca50 -> FUN_00507060
    return true;
  }
  if (is("reset")) {
    // Velocities 0, steer 0, every suspension length to its static value
    // (radius + rest) - (|g| x mass / n) / k, asleep, then the steer angles
    // (which flag the wheel bodies) and the wheel bodies. The body itself
    // does not move.
    steer_ = 0.f;
    steer_seen_ = 0.f;
    const float load =
        n > 0 ? car_gravity() * compound_mass() / static_cast<float>(n) : 0.f;
    for (CarWheel& w : wheels_) w.length = static_length(w, load);
    asleep_ = true;
    steer_angles();
    wheel_frames();
    return true;
  }
  if (is("setsteer")) {
    // "%*s %f": nothing when the input is the same number.
    float v = 0.f;
    engine_sscanf(text, "%*s %f", &v);
    if (steer_ == v) return true;
    steer_ = v;
    steer_seen_ = v;
    steer_angles();
    wheel_frames();
    return true;
  }
  if (is("install")) {
    // The car itself as a part: "%*s %d %d %d %d %d" -> FUN_004c96c0
    // without a place, FUN_004cb1d0.
    return part_cmd_install(this, text, false);
  }
  if (is("remove")) {
    // "%*s %d %d" -> FUN_004cb510, the same code as in the part's handler.
    return part_cmd_remove(this, text);
  }
  if (is("add_part")) {
    // "%*s %d" -> FUN_004942b0: the list car+0xDC and the script's
    // addpart(GameRef), as the part's FUN_004c17b0.
    // Fork: not acted on: a part on slot 401 becomes the engine (its
    // dynodata is bound), and FUN_004a8200 makes a flap record for a part
    // on a slot with a `flap` line.
    int32_t id = 0;
    engine_sscanf(text, "%*s %d", &id);
    part_add_part(this, id);
    return true;
  }
  if (is("rem_part")) {
    // "%*s %d" -> FUN_004accb0 (the flap record), FUN_004acdf0 (the list
    // car+0xDC and the script's rempart(GameRef)).
    int32_t id = 0;
    engine_sscanf(text, "%*s %d", &id);
    part_rem_part(this, id);
    return true;
  }
  if (is("filter")) return part_cmd_filter(this, text);
  // (m6 step 4) FUN_004a2e60 `repair` and `cancel` (part_cmd_repair).
  if (is("repair")) return part_cmd_repair(this, text, true);
  if (is("cancel")) return part_cmd_cancel(this, true);
  if (is("texture")) {
    // "%*s %d %d": texture id, recurse. A handle on the texture record
    // (FUN_00488930), FUN_004c75a0 on the car when car+0x84 != 0, and with
    // a non-zero second number the same string to every part on the list
    // car+0xDC (FUN_0047d550(part, 0, 0x10, text, 0)); the part's handler
    // reads the id from it.
    int32_t id = 0, recurse = 0;
    engine_sscanf(text, "%*s %d %d", &id, &recurse);
    part_set_texture(this, id);
    if (recurse) {
      const std::vector<InvObject*> listed = attached_;
      for (InvObject* node : listed)
        if (std::shared_ptr<PartObject> part = part_of(node))
          if (part.get() != this) part->event(0x10, text);
    }
    return true;
  }
  if (is("mesh")) {
    // "%*s %d": FUN_004c72d0 on the car when car+0x94 != 0.
    int32_t id = 0;
    engine_sscanf(text, "%*s %d", &id);
    part_set_mesh(this, id);
    return true;
  }

  // Taken and stored. Fork: not acted on (each needs the drive train, the
  // seats or the lamp, wing and flap records of the car object):
  //   transmission  car+0x70 bits 0x10 / 0x20 / 0x40
  //   abs asr esp   bits 0x80 / 0x100 / 0x4000000, header+0xA3C..0xA44
  //   difflock      header+0xA38        steerhelp  bit 0x200, header+0xA34
  //   cruise        bit 0x1000 and event bit 0x400000
  //   reload idle   nitro and idle rpm from the header
  //   add_light rem_light add_wing rem_wing add_rollbar rem_rollbar
  //   add_mslot rem_mslot                          the car's record lists
  //   add_linked rem_linked                        the two numbers behind the
  //                 wheel index are addresses of slot records in the exe
  //                 (asm 0x4a56bb..0x4a586c); a host part does not send the
  //                 words, it keeps the record with its render instance
  //                 (part_link_instances, NativeTypes_part8.inc)
  //   autopilot_on autopilot_off ai_cheat          the seats and the AI
  if (is("transmission")) {
    engine_sscanf(text, "%*s %d", &stored_.transmission);
  } else if (is("steerhelp")) {
    engine_sscanf(text, "%*s %f", &stored_.steerhelp);
  } else if (is("asr")) {
    engine_sscanf(text, "%*s %f", &stored_.asr);
  } else if (is("abs")) {
    engine_sscanf(text, "%*s %f", &stored_.abs);
  } else if (is("esp")) {
    engine_sscanf(text, "%*s %f", &stored_.esp);
  } else if (is("cruise")) {
    engine_sscanf(text, "%*s %f", &stored_.cruise);
  } else if (is("difflock")) {
    engine_sscanf(text, "%*s %f", &stored_.difflock);
  } else if (is("damage_multiplier")) {
    engine_sscanf(text, "%*s %f", &stored_.damage_multiplier);
  } else if (is("team")) {
    engine_sscanf(text, "%*s %d", &stored_.team);
  } else if (is("brake")) {
    flags_ |= 0x40000u;
  } else if (is("updatevariables")) {
    flags_ |= 0x800000u;
  } else if (!(is("reload") || is("idle") || is("add_light") ||
               is("rem_light") || is("add_wing") ||
               is("rem_wing") || is("add_rollbar") || is("rem_rollbar") ||
               is("add_mslot") || is("rem_mslot") || is("add_linked") ||
               is("rem_linked") || is("autopilot_on") ||
               is("autopilot_off") || is("ai_cheat"))) {
    // Left to the host's generic command chain as before (the driving
    // milestone): render, hide, activate, deactivate, next, prev, osd,
    // corpse, setvel, sethorn, setpitch, addroll, decal, paint, bigdecal,
    // pickup; and any word the exe's handler does not know.
    return false;
  }
  ++stored_.taken;
  return true;
}

// PE 940 FUN_0049a8e0(type, car, level): one render instance `r_wheel` per
// render entry with flag 0x1000 (a `render` line behind a `wheel` line)
// whose `lod` range holds the level, under the car's node; its bone00
// follows the `wheelbone` of wheel (flags >> 24). This is how a car without
// scripts draws its wheels; the wheels of a scripted racer are parts.
// The instances of another level went in part_make_render_instances.
void car_make_wheel_instances(CarObject* car, int level) {
  const PartTypeDesc& d = *car->type_;
  for (size_t i = 0; i < d.entries.size(); ++i) {
    const PartEntry& e = d.entries[i];
    if (e.kind != 3 || !(e.flags & 0x1000u)) continue;
    if (!part_entry_in_level(e, level)) continue;
    if (!e.res_id) continue;
    bool have = false;
    for (const PartObject::Inst& in : car->insts_)
      if (in.entry == static_cast<int>(i)) have = true;
    if (have) continue;
    const size_t wi = e.flags >> 24;
    if (wi >= car->wheels_.size()) continue;
    const CarWheel& w = car->wheels_[wi];
    InvObject* r = part_new_render_instance(
        car, e, e.res_id, "r_wheel", w.wb_pos + cam_mul(w.wb_rot, e.off),
        cam_mm(w.wb_rot, e.rot));
    if (!r) continue;
    PartObject::Inst inst;
    inst.r = r;
    inst.entry = static_cast<int>(i);
    inst.res_id = e.res_id;
    car->insts_.push_back(inst);
    --car->other_lod_entries_;  // part_make_render_instances counted it
  }
}

// PE 940 FUN_00498180, the car's level change: FUN_004c2b10 and
// FUN_0049a8e0 for the new level.
void CarObject::make_instances() {
  part_make_render_instances(this, level_);
  car_make_wheel_instances(this, level_);
  place_own_instances();
}

// PE 940 FUN_004992e0 (type vt+0x0C).
std::shared_ptr<NativeObject> car_create(const NativeCreate& c) {
  auto car = std::make_shared<CarObject>();
  // Fork: native_object_create writes the index when the factory returns;
  // part_link_instances below asks whether the object is a car.
  car->type_index = kNativeCar;
  // FUN_004c1f90: the create string "%f,%f,%f,%f,%f,%f,%d", the compound
  // with one body per `body` line, `bbone`, the slot table, the car handle
  // on the car's own node, the render instances of the entries without
  // flag 0x1000.
  part_init(car.get(), c);
  const PartTypeDesc& d = *car->type_;
  static const CarTypePart kNoCar;
  car->car_type_ = d.car ? d.car.get() : &kNoCar;
  const CarTypePart& ct = *car->car_type_;
  car->max_steer_ = ct.max_steer;
  car->ackermann_ = ct.ackermann;
  // type+0x1F34: the `mass` word, else the mass of the chassis compound at
  // the first create (the cfg bodies; no part is on the car yet).
  const float type_mass = ct.mass_word > 0.f ? ct.mass_word : d.total_mass;
  const float g = car_gravity();
  const size_t n = ct.wheels.size();
  car->wheels_.reserve(n);
  for (const CarWheelDesc& wd : ct.wheels) {
    CarWheel w;
    w.spring[0] = wd.spring[0];
    w.spring[1] = wd.spring[1];
    w.spring[2] = wd.spring[2];
    w.rest = wd.spring[1] + wd.spring[0];
    w.max_len = wd.spring[0] + kCarMaxLenAdd;
    w.min_len = -1.f;
    w.radius = wd.radius;
    w.bonepos = wd.bonepos;
    // w+0x78 = desc position + (shape+0x40 of the compound - the local
    // position of `bbone`) + (0, rest, 0). Both terms of the bracket come
    // from FUN_005032d0, which moves the compound's origin to its mass
    // centre C (shape+0x40 = -C, bbone at its cfg place - C): the bracket
    // is minus the cfg place of the reference body, (0, 0, 0) in 92 of the
    // 95 car cfgs with one. The mount is a point of the reference body's
    // frame. Fork: taken as such for a reference body without rotation (no
    // shipped car cfg has a rotated one).
    w.mount = wd.pos - d.ref_at + cam_v(0.f, w.rest, 0.f);
    w.k = (g * type_mass / static_cast<float>(n)) / w.spring[1];
    w.damping = w.k * w.spring[2];
    w.rebound = w.damping * kCarRebound;
    w.w90 = cam_ypr_to_matrix(wd.ypr[0], wd.ypr[1], wd.ypr[2]);
    w.steer_factor = wd.steer_factor;
    // The arm and the hub of the cfg lines, as the handlers left them
    // (desc+0x68..+0xB0 copied to w+0x234..+0x278); no opposite wheel.
    w.arm_len = wd.arm_len;
    w.arm_axis = wd.arm_axis;
    w.arm_pivot = wd.arm_pivot;
    w.opp = -1;
    w.hub_len = wd.hub_len;
    w.hub_p1 = wd.hub_p1;
    w.hub_p2 = wd.hub_p2;
    w.hub_pc = wd.hub_pc;
    w.length = wd.radius + w.spring[0];
    w.bones = wd.bones;
    // The bodies as the create leaves them, until FUN_004aaaf0 first runs:
    // `wheelbone` at mount + up x (radius - min(length, max + radius)) with
    // the rotation of the steer angle alone (0), `hub` and `a_arm` at the
    // wheel position of the cfg, unrotated.
    float len = w.length;
    if (w.spring_max() <= len) len = w.spring_max();
    w.wb_pos = w.mount + cam_col(w.w90, 1) * (w.radius - len);
    w.hub_pos = wd.pos;
    w.arm_pos = wd.pos;
    car->wheels_.push_back(w);
  }
  // The `steer` body (car+0x124): the entry of the `steering` line, at its
  // place in the reference body's frame with the rotation of the line.
  if (ct.steer_index >= 0 &&
      ct.steer_index < static_cast<int>(d.entries.size())) {
    const PartEntry& e = d.entries[static_cast<size_t>(ct.steer_index)];
    car->has_steer_ = true;
    car->steer_pos_ = e.off + cam_mul(e.rot, e.com) - d.ref_at;
    car->steer_rot_ = e.rot;
  }
  // The bone of every slot by its `slottype` (asm 0x49a643..0x49a6db); a
  // slot keeps `bbone` when the number names no body.
  // Fork: the exe indexes the wheel array with numbers past the wheel
  // count and binds a null handle for a wheel without `wheelbones`; the
  // host leaves such a slot on the reference body.
  car->slot_bones_.resize(car->slots_.size());
  for (size_t i = 0; i < d.slots.size() && i < car->slot_bones_.size(); ++i) {
    const uint32_t t = static_cast<uint32_t>(d.slots[i].slottype());
    CarObject::SlotBone b;
    if (t == 0xbu) {
      if (car->has_steer_) b.body = CarObject::kBodySteer;
    } else if (t >= 101u && t <= 400u) {
      const uint32_t wi = (t - 101u) % 100u;
      if (wi < n) {
        if (t <= 200u) {
          b.body = CarObject::kBodyWheel;
        } else if (car->wheels_[wi].bones) {
          b.body = t <= 300u ? CarObject::kBodyHub : CarObject::kBodyArm;
        }
        if (b.body != CarObject::kBodyRef) b.wheel = static_cast<uint8_t>(wi);
      }
    }
    car->slot_bones_[i] = b;
  }
  // car+0x70 = 0x60; the compound is put to sleep (FUN_00506d90).
  car->flags_ = 0x60u;
  car->asleep_ = true;
  // FUN_004ebd90(car, 0x20, 0) and the part's 0x10295: the car is on the
  // callback lists. The host needs control() for the wheel bodies.
  car->ticks = true;
  car_make_wheel_instances(car.get(), car->level_);
  car->place_own_instances();
  // FUN_004c2b10 applies the `linked` lines of a car's own render entries
  // at once (no shipped car cfg has one). The car is not registered under
  // its key yet: its slot records are found through the object itself
  // (part_slot_world_frame).
  part_link_instances(car.get());
  if (native_type_log()) {
    const RpakEntry* e = rpak_find_entry(d.type_id);
    int typed = 0;
    for (const CarObject::SlotBone& b : car->slot_bones_)
      if (b.body != CarObject::kBodyRef) ++typed;
    std::fprintf(stderr,
                 "[native-type] car %p type 0x%08X '%s': body (%.4f, %.4f, "
                 "%.4f), type mass %g, %zu wheel(s), steer body %d, max steer "
                 "%g ackermann %g, %zu slot(s) of which %d on a wheel or "
                 "steer body, %zu render instance(s)%s\n",
                 static_cast<void*>(car->key_),
                 static_cast<unsigned>(d.type_id), e ? e->name.c_str() : "",
                 car->pos_.x, car->pos_.y, car->pos_.z, type_mass, n,
                 ct.steer_index, ct.max_steer, ct.ackermann,
                 car->slots_.size(), typed, car->insts_.size(),
                 part_is_identity(d.ref_rot)
                     ? ""
                     : "; ROTATED reference body: wheel frames not exact");
  }
  return car;
}

// The alias of a type record, "" when it has none.
const char* car_type_alias(int32_t type_id) {
  const RpakEntry* e = rpak_find_entry(type_id);
  return e ? e->name.c_str() : "";
}

// PE 940 Part.getWheelID 0x4CD2E0: (part+0xD0 - 101) % 10 when part+0xD0
// is 101..400, else -1.
int part_wheel_of(int32_t d0) {
  return (d0 > 100 && d0 < 401) ? (d0 - 101) % 10 : -1;
}

// True when the part sits on a car (its car handle names a car object).
bool part_on_car(const PartObject* part) {
  if (!part->car_ || part->car_ == part->key_) return false;
  std::shared_ptr<PartObject> top = part_of(part->car_);
  return top && top->type_index == kNativeCar;
}

// " installed: slot <mine> on 0x<node id of the target> slot <its slot>" for
// the slot the part is installed through; "" when it is not installed.
void part_installed_text(const PartObject* part, char* out, size_t cap) {
  out[0] = 0;
  for (const PartObject::Slot& s : part->slots_)
    if (s.other && (s.flags & 1u)) {
      std::snprintf(out, cap, " installed: slot %d on 0x%08X slot %d", s.id,
                    static_cast<unsigned>(part_node_id(s.other)),
                    s.other_slot);
      return;
    }
}

bool part_linked_points(const PartObject::Inst& in, CamV3* pa, CamV3* pb);

// The instance group of a `[render-dbg]` line: the render type, bone00 in
// the part's frame and in the world, the sections, vertices and triangles
// of its mesh (0 while it has none) and, for a linked instance, the slots
// it hangs on with their world points.
void part_instance_text(const PartObject* part, const PartObject::Inst& i,
                        char* out, size_t cap) {
  float px = 0.f, py = 0.f, pz = 0.f;
  render_d3d9_mesh_get_transform(i.r, &px, &py, &pz, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr);
  const CamV3 world = part->pos_ + cam_mul(part->rot_, cam_v(px, py, pz));
  int32_t verts = 0, tris = 0;
  const bool ready = render_d3d9_mesh_ready(i.r);
  if (ready) render_d3d9_mesh_counts(i.r, &verts, &tris);
  const int n = std::snprintf(
      out, cap,
      " instance %p render 0x%08X '%s' local=(%.4f, %.4f, %.4f) "
      "mesh_origin=(%.4f, %.4f, %.4f) sections=%d verts=%d tris=%d",
      static_cast<void*>(i.r), static_cast<unsigned>(i.res_id),
      car_type_alias(i.res_id), px, py, pz, world.x, world.y, world.z,
      ready ? render_d3d9_mesh_submesh_count(i.r) : 0, verts, tris);
  if (!i.link || n < 0 || static_cast<size_t>(n) >= cap) return;
  CamV3 pa, pb;
  if (!part_linked_points(i, &pa, &pb)) return;
  if (i.link == 2)
    std::snprintf(out + n, cap - static_cast<size_t>(n),
                  " linked A=%d (%.4f, %.4f, %.4f) B=%d (%.4f, %.4f, %.4f) "
                  "AB=%.4f",
                  i.link_a, pa.x, pa.y, pa.z, i.link_b, pb.x, pb.y, pb.z,
                  std::sqrt(cam_len2(pb - pa)));
  else
    std::snprintf(out + n, cap - static_cast<size_t>(n),
                  " linked A=%d (%.4f, %.4f, %.4f)", i.link_a, pa.x, pa.y,
                  pa.z);
}

}  // namespace

// ---------------------------------------------------------------------------
// The Part natives that answer from the objects (script boot)
// ---------------------------------------------------------------------------
// PE 940 Part.getCar 0x4CC640 = the id of the car handle part+0xC8;
// Part.getCarRef 0x4CC550 = the script instance (payload+0x50) of that
// node. The handle is on the part's own node until the part is installed
// (FUN_004c1f90), and a car's is on the car itself.
bool native_part_car(InvObject* self, int32_t* node_id,
                     InvObject** script_instance) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  InvObject* const car = p->car_;
  if (node_id) *node_id = car ? part_node_id(car) : 0;
  if (script_instance) {
    *script_instance = nullptr;
    if (car && part_node_id(car) != 0) {
      // payload+0x50 is null for a node whose type has no script class.
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto it = g_refs.find(car);
      if (it != g_refs.end() && !it->second.script_class.empty())
        *script_instance = car;
    }
  }
  return true;
}

// PE 940 Part.getSlots 0x4CCED0 = type+0x1160, the number of `slot` lines.
bool native_part_slot_count(InvObject* self, int32_t* count) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  *count = static_cast<int32_t>(p->type_->slots.size());
  return true;
}

// PE 940 Part.getSlotID 0x4CCAA0 for an index >= 0: FUN_004c7020 walks the
// type's slot list (cfg order) that many steps and the id (+0x60) of the
// node is returned; 0 past the end.
bool native_part_slot_id(InvObject* self, int32_t index, int32_t* id) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  const std::vector<PartSlot>& slots = p->type_->slots;
  *id = (index >= 0 && static_cast<size_t>(index) < slots.size())
            ? slots[static_cast<size_t>(index)].id
            : 0;
  return true;
}

// PE 940 Part.getSlotIndex 0x4CCBF0: the place in the type's slot list of
// the slot with that id; 0 when there is none (and for the first slot).
bool native_part_slot_index(InvObject* self, int32_t slot_id, int32_t* index) {
  std::shared_ptr<PartObject> p = part_of(self);
  if (!p) return false;
  const std::vector<PartSlot>& slots = p->type_->slots;
  *index = 0;
  for (size_t i = 0; i < slots.size(); ++i)
    if (slots[i].id == slot_id) {
      *index = static_cast<int32_t>(i);
      break;
    }
  return true;
}

// ---------------------------------------------------------------------------
// `[render-dbg]`
// ---------------------------------------------------------------------------
void native_part_install_dump(FILE* out);  // NativeTypes_part5.inc

// `[render-dbg]` lines for the parts and the cars: a count; per car one
// line, one per wheel and one per render instance of its own; one line per
// part that sits on a car (per render instance, or one without the
// instance group for a part that has none); for every other part one line
// per render instance that has its mesh (a camera saw it). `node` is the id
// of the part's node, `under` the id of the node it hangs under, `d0`
// part+0xD0 and `wheel` what Part.getWheelID makes of it, `detail` the
// detail of the node (0 = no camera has seen it) and `lod` its level of
// detail over the highest level of its type, and
// `installed: slot <mine> on 0x<node id> slot <its slot>` the mount.
void native_part_debug_dump(FILE* out) {
  if (!out || !native_script_boot()) return;
  std::vector<std::shared_ptr<NativeObject>> parts, cars;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    for (const auto& kv : g_objects) {
      if (!kv.second) continue;
      std::vector<std::shared_ptr<NativeObject>>* list = nullptr;
      if (kv.second->type_index == kNativePart) list = &parts;
      if (kv.second->type_index == kNativeCar) list = &cars;
      if (!list) continue;
      bool seen = false;
      for (const auto& p : *list)
        if (p == kv.second) seen = true;
      if (!seen) list->push_back(kv.second);
    }
  }
  size_t instances = 0, with_mesh = 0;
  for (const auto& o : parts) {
    const PartObject* part = static_cast<const PartObject*>(o.get());
    instances += part->insts_.size();
    for (const PartObject::Inst& i : part->insts_)
      if (render_d3d9_mesh_ready(i.r)) ++with_mesh;
  }
  std::fprintf(out,
               "[render-dbg] native parts=%zu render_instances=%zu "
               "with_mesh=%zu cars=%zu\n",
               parts.size(), instances, with_mesh, cars.size());
  char text[768];
  char on[96];
  for (const auto& o : cars) {
    CarObject* car = static_cast<CarObject*>(o.get());
    const PartTypeDesc& d = *car->type_;
    const CamV3 gp = car->ground_point();
    int used = 0;
    for (const PartObject::Slot& s : car->slots_)
      if (s.other) ++used;
    std::fprintf(out,
                 "[render-dbg] native car %p node=0x%08X type=0x%08X '%s' "
                 "asleep=%d steer=%.4f bbone=(%.4f, %.4f, %.4f) ypr=(%.5f, "
                 "%.5f, %.5f) getinfo2=(%.4f, %.4f, %.4f) mass own=%.3f "
                 "compound=%.3f slots=%d/%zu parts=%zu maxsteer=%.4f "
                 "ack=%.4f flags=0x%X detail=%.4f lod=%d/%d under=0x%08X\n",
                 static_cast<void*>(car->key_),
                 static_cast<unsigned>(part_node_id(car->key_)),
                 static_cast<unsigned>(d.type_id), car_type_alias(d.type_id),
                 car->asleep_ ? 1 : 0, car->steer_, car->pos_.x, car->pos_.y,
                 car->pos_.z, car->ypr_[0], car->ypr_[1], car->ypr_[2], gp.x,
                 gp.y, gp.z, d.total_mass, car->compound_mass(), used,
                 car->slots_.size(), car->attached_.size(), car->max_steer_,
                 car->ackermann_, static_cast<unsigned>(car->flags_),
                 car->detail_, car->level_, d.lod_count,
                 static_cast<unsigned>(part_node_id(gameref_get_parent(car->key_))));
    for (size_t wi = 0; wi < car->wheels_.size(); ++wi) {
      const CarWheel& w = car->wheels_[wi];
      // centre = the hub body in the world; contact_y = the world height
      // of the end of the spring ray, mount - (0, length, 0).
      const CamV3 centre = car->pos_ + cam_mul(car->rot_, w.hub_pos);
      const CamV3 contact =
          car->pos_ +
          cam_mul(car->rot_, w.mount - cam_v(0.f, w.length, 0.f));
      const char* on_wheel = "-";
      if (const PartObject::Slot* s =
              car->slot(101 + static_cast<int32_t>(wi)))
        if (s->other)
          if (std::shared_ptr<PartObject> p = part_of(s->other))
            on_wheel = car_type_alias(p->type_->type_id);
      // The records the scripts set: arm pivot and axis (w+0x244, +0x238),
      // the hub's p1, p2 and pc (w+0x258, +0x264, +0x270), the Ypr of w90
      // and the steer factor (w+0xC8).
      float wy = 0.f, wp = 0.f, wr = 0.f;
      cam_matrix_to_ypr(w.w90, &wy, &wp, &wr);
      std::fprintf(out,
                   "[render-dbg]   wheel %zu: mount=(%.4f, %.4f, %.4f) "
                   "radius=%.4f rest=%.4f min=%.4f max=%.4f k=%.3f len=%.4f "
                   "steer=%.4f arm=%.4f hub=%.4f centre=(%.4f, %.4f, %.4f) "
                   "contact_y=%.4f on=%s armp=(%.5f, %.5f, %.5f) armn=(%.5f, "
                   "%.5f, %.5f) p1=(%.5f, %.5f, %.5f) p2=(%.5f, %.5f, %.5f) "
                   "pc=(%.5f, %.5f, %.5f) wypr=(%.6f, %.6f, %.6f) sf=%.4f "
                   "damping=%.3f rebound=%.3f\n",
                   wi, w.mount.x, w.mount.y, w.mount.z, w.radius, w.rest,
                   w.min_len, w.max_len, w.k, w.length, w.steer_angle,
                   w.arm_len, w.hub_len, centre.x, centre.y, centre.z,
                   contact.y, on_wheel, w.arm_pivot.x, w.arm_pivot.y,
                   w.arm_pivot.z, w.arm_axis.x, w.arm_axis.y, w.arm_axis.z,
                   w.hub_p1.x, w.hub_p1.y, w.hub_p1.z, w.hub_p2.x, w.hub_p2.y,
                   w.hub_p2.z, w.hub_pc.x, w.hub_pc.y, w.hub_pc.z, wy, wp, wr,
                   w.steer_factor, w.damping, w.rebound);
    }
    for (const PartObject::Inst& i : car->insts_) {
      part_instance_text(car, i, text, sizeof(text));
      std::fprintf(out, "[render-dbg]  %s\n", text);
    }
  }
  for (const auto& o : parts) {
    const PartObject* part = static_cast<const PartObject*>(o.get());
    const PartTypeDesc& d = *part->type_;
    const bool on_car = part_on_car(part);
    part_installed_text(part, on, sizeof(on));
    const unsigned under =
        static_cast<unsigned>(part_node_id(gameref_get_parent(part->key_)));
    const int wheel = part_wheel_of(part->d0_);
    int lines = 0;
    for (const PartObject::Inst& i : part->insts_) {
      if (!on_car && !render_d3d9_mesh_ready(i.r)) continue;
      part_instance_text(part, i, text, sizeof(text));
      std::fprintf(out,
                   "[render-dbg] native part %p '%s' body=(%.4f, %.4f, %.4f) "
                   "ypr=(%.4f, %.4f, %.4f) mass_centre=(%.4f, %.4f, %.4f) "
                   "node=0x%08X under=0x%08X wheel=%d d0=%d detail=%.4f "
                   "lod=%d/%d%s%s\n",
                   static_cast<void*>(part->key_), car_type_alias(d.type_id),
                   part->pos_.x, part->pos_.y, part->pos_.z, part->ypr_[0],
                   part->ypr_[1], part->ypr_[2], d.com.x, d.com.y, d.com.z,
                   static_cast<unsigned>(part_node_id(part->key_)), under,
                   wheel, part->d0_, part->detail_, part->level_, d.lod_count,
                   text, on);
      ++lines;
    }
    if (lines == 0 && on_car)
      std::fprintf(out,
                   "[render-dbg] native part %p '%s' body=(%.4f, %.4f, %.4f) "
                   "ypr=(%.4f, %.4f, %.4f) mass_centre=(%.4f, %.4f, %.4f) "
                   "node=0x%08X under=0x%08X wheel=%d d0=%d detail=%.4f "
                   "lod=%d/%d%s\n",
                   static_cast<void*>(part->key_), car_type_alias(d.type_id),
                   part->pos_.x, part->pos_.y, part->pos_.z, part->ypr_[0],
                   part->ypr_[1], part->ypr_[2], d.com.x, d.com.y, d.com.z,
                   static_cast<unsigned>(part_node_id(part->key_)), under,
                   wheel, part->d0_, part->detail_, part->level_, d.lod_count,
                   on);
  }
  native_part_install_dump(out);
  native_part_paint_dump(out);
}
