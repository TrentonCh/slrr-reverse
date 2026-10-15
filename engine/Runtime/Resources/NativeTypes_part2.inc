// Fragment 2 of NativeTypes.cpp - included, not compiled on its own.
//
// The `camera` type: row 1 of the native type table, `_type_camera`
// 0x59B13C, factory FUN_0048cb30. It is not a posed camera. An instance is
//   - a small rigid body (sphere r 0.15 m, mass 0.05, no gravity) that the
//     type steers with forces on every physics step;
//   - a camera params node (the factory Camera.create uses, FUN_00524dc0)
//     that the renderer reads every frame;
//   - a render instance `r_camera` under the camera's world-tree root whose
//     bone00 follows the body; `render <vp id> ..` hooks it on a viewport.
// Mirrors: FUN_00489d30 create, FUN_0048bce0 commands, FUN_004897b0 control
// (once per game tick), FUN_0048a430 drive (once per physics step),
// FUN_0048c940 setMatrix, FUN_0048bbe0 getinfo, FUN_0048a3c0 destroy.
// The body step (FUN_004f8460 drag, FUN_00506530 integration, FUN_00502530
// sleep test, FUN_00507060 wake) is the reference transcription
// m2/cam_sim.py of the M2 mapping.
// Host stand-ins: the body lives in the camera object (no physics world
// instance), the params node and `r_camera` are one renderer camera keyed
// by the GameRef the script holds.
// Units: metres, seconds, radians; the field of view is the full vertical
// angle in degrees. The exe's renderer units are metres x 10 (DAT_0059373c).

namespace {

// ---- vectors (single precision, as the exe's SSE code) ---------------------
struct CamV3 {
  float x = 0.f, y = 0.f, z = 0.f;
};
inline CamV3 cam_v(float x, float y, float z) {
  CamV3 r;
  r.x = x;
  r.y = y;
  r.z = z;
  return r;
}
inline CamV3 operator+(CamV3 a, CamV3 b) {
  return cam_v(a.x + b.x, a.y + b.y, a.z + b.z);
}
inline CamV3 operator-(CamV3 a, CamV3 b) {
  return cam_v(a.x - b.x, a.y - b.y, a.z - b.z);
}
inline CamV3 operator*(CamV3 a, float s) {
  return cam_v(a.x * s, a.y * s, a.z * s);
}
inline float cam_dot(CamV3 a, CamV3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline CamV3 cam_cross(CamV3 a, CamV3 b) {
  return cam_v(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
               a.x * b.y - a.y * b.x);
}
inline float cam_len2(CamV3 a) { return cam_dot(a, a); }

// A body rotation: three rows, world = M * local (the exe keeps three rows
// of four floats at body+0xA4 / +0xB4 / +0xC4, FUN_005060d0).
struct CamM3 {
  float m[3][3];
};
inline CamM3 cam_identity() {
  CamM3 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r.m[i][j] = (i == j) ? 1.f : 0.f;
  return r;
}
inline CamV3 cam_mul(const CamM3& M, CamV3 v) {
  return cam_v(M.m[0][0] * v.x + M.m[0][1] * v.y + M.m[0][2] * v.z,
               M.m[1][0] * v.x + M.m[1][1] * v.y + M.m[1][2] * v.z,
               M.m[2][0] * v.x + M.m[2][1] * v.y + M.m[2][2] * v.z);
}
inline CamV3 cam_col(const CamM3& M, int j) {
  return cam_v(M.m[0][j], M.m[1][j], M.m[2][j]);
}
inline CamM3 cam_mm(const CamM3& A, const CamM3& B) {
  CamM3 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      r.m[i][j] = A.m[i][0] * B.m[0][j] + A.m[i][1] * B.m[1][j] +
                  A.m[i][2] * B.m[2][j];
  return r;
}

// PE 940 FUN_005666d0 (Ypr_toMatrix): R = Ry(yaw) Rx(pitch) Rz(roll). The
// renderer's build_local_world is the same product.
CamM3 cam_ypr_to_matrix(float yaw, float pitch, float roll) {
  const float cy = static_cast<float>(std::cos(static_cast<double>(yaw)));
  const float sy = static_cast<float>(std::sin(static_cast<double>(yaw)));
  const float cp = static_cast<float>(std::cos(static_cast<double>(pitch)));
  const float sp = static_cast<float>(std::sin(static_cast<double>(pitch)));
  const float cr = static_cast<float>(std::cos(static_cast<double>(roll)));
  const float sr = static_cast<float>(std::sin(static_cast<double>(roll)));
  CamM3 ry = cam_identity(), rx = cam_identity(), rz = cam_identity();
  ry.m[0][0] = cy;
  ry.m[0][2] = sy;
  ry.m[2][0] = -sy;
  ry.m[2][2] = cy;
  rx.m[1][1] = cp;
  rx.m[1][2] = -sp;
  rx.m[2][1] = sp;
  rx.m[2][2] = cp;
  rz.m[0][0] = cr;
  rz.m[0][1] = -sr;
  rz.m[1][0] = sr;
  rz.m[1][1] = cr;
  return cam_mm(cam_mm(ry, rx), rz);
}

// PE 940 FUN_00569970 (the inverse): yaw = atan2(M02, M22), roll =
// atan2(M10, M11), pitch = atan2(-M12 cos roll, M11), with the two special
// cases of the function. The exe uses the x87 FPATAN and stores floats.
void cam_matrix_to_ypr(const CamM3& M, float* yaw, float* pitch, float* roll) {
  auto at2 = [](float y, float x) {
    return static_cast<float>(
        std::atan2(static_cast<double>(y), static_cast<double>(x)));
  };
  if (M.m[2][2] == 0.f && M.m[0][2] == 0.f) {
    *yaw = at2(M.m[0][0], -M.m[0][1]);
    *pitch = 3.1415927f;
    *roll = 0.f;
    return;
  }
  *yaw = at2(M.m[0][2], M.m[2][2]);
  const float r = at2(M.m[1][0], M.m[1][1]);
  *roll = r;
  const float c = static_cast<float>(std::cos(static_cast<double>(r)));
  if (c > 0.f)
    *pitch = at2(-(M.m[1][2] * c), M.m[1][1]);
  else
    *pitch = at2(M.m[1][2] * c, -M.m[1][1]);
}

// Rotation by the vector `aa` (axis x angle), cam_sim.py rot().
CamM3 cam_rot_vector(CamV3 aa) {
  const float th = std::sqrt(cam_len2(aa));
  if (th < 1e-12f) return cam_identity();
  const CamV3 k = aa * (1.f / th);
  CamM3 K;
  K.m[0][0] = 0.f;
  K.m[0][1] = -k.z;
  K.m[0][2] = k.y;
  K.m[1][0] = k.z;
  K.m[1][1] = 0.f;
  K.m[1][2] = -k.x;
  K.m[2][0] = -k.y;
  K.m[2][1] = k.x;
  K.m[2][2] = 0.f;
  const CamM3 K2 = cam_mm(K, K);
  const float s = std::sin(th);
  const float c = 1.f - std::cos(th);
  CamM3 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      r.m[i][j] = ((i == j) ? 1.f : 0.f) + s * K.m[i][j] + c * K2.m[i][j];
  return r;
}

// cam_sim.py orthonormalise(): columns 0 and 1 by Gram-Schmidt, 2 = 0 x 1.
CamM3 cam_orthonormalise(const CamM3& R) {
  CamV3 x = cam_col(R, 0);
  CamV3 y = cam_col(R, 1);
  x = x * (1.f / std::sqrt(cam_len2(x)));
  y = y - x * cam_dot(x, y);
  y = y * (1.f / std::sqrt(cam_len2(y)));
  const CamV3 z = cam_cross(x, y);
  CamM3 r;
  r.m[0][0] = x.x;
  r.m[1][0] = x.y;
  r.m[2][0] = x.z;
  r.m[0][1] = y.x;
  r.m[1][1] = y.y;
  r.m[2][1] = y.z;
  r.m[0][2] = z.x;
  r.m[1][2] = z.y;
  r.m[2][2] = z.z;
  return r;
}

// ---- constants (file bytes of the exe, M2 map exe_camera.md section 11) ----
// The body type FUN_0048a2d0 makes once: FUN_004f7d60(0.05, 1 = sphere,
// 0.15, 0, 0, 0).
constexpr float kCamMass = 0.05f;    // 0x3d4ccccd
constexpr float kCamRadius = 0.15f;  // 0x3e19999a
constexpr float kCamInvMass = 1.f / kCamMass;  // type+0x2C
// type+0x98 = mass * DAT_005a7278 (-0.005), then x DAT_0059373c (10).
constexpr float kCamLinDrag = kCamMass * -0.005f * 10.f;
// type+0x9C = DAT_0059b19c (-0.2) / invMass; FUN_00506530 scales the angular
// momentum by 1 + invMass * type+0x9C * dt * 100 = 1 - 20 dt.
constexpr float kCamAngDamp = -0.2f / kCamInvMass;
// Sphere, FUN_00505760 case 1: inverse inertia = DAT_00593f4c (2.5) / (r r m).
constexpr float kCamInvInertia = 2.5f / (kCamRadius * kCamRadius * kCamMass);
constexpr float kWorldStep = 0.02f;      // world+8, FUN_004fffb0 (0x3ca3d70a)
constexpr float kWorldMinStep = 1e-4f;   // DAT_005938c4
constexpr float kTickDtCap = 0.1f;       // DAT_005da468
constexpr float kRollSpring = -0.3f;     // DAT_0059b1a0
constexpr float kRollDampA = 0.03f;      // DAT_0059b190
constexpr float kRollDampB = 0.3f;       // DAT_00593704
constexpr float kRollTorqueMin2 = 9.999999974752427e-07f;  // DAT_0059a450
constexpr float kCtlForceMin2 = 1.0000001111620804e-06f;   // _DAT_0059b188
// FUN_004897b0, a controller bound and the camera not activated:
constexpr float kCtlForceX = -400.f;    // _DAT_0059b1a4: axis 7, AXIS_LOOK_LEFTRIGHT
constexpr float kCtlForceY = 400.f;     // _DAT_0059abdc: axis 8, AXIS_LOOK_UPDOWN
constexpr float kCtlForceCap2 = 100.f;  // DAT_0059385c
constexpr float kCtlForceCap = 10.f;    // DAT_0059373c
constexpr float kCtlTorqueY = 0.009999999776482582f;   // DAT_005935d0
constexpr float kCtlTorqueX = -0.009999999776482582f;  // DAT_0059adf4
constexpr float kCtlDistRate = 100.f;   // DAT_0059385c: axis 10, AXIS_LOOK_FWDBACK
constexpr float kCtlTorqueMin2 = 9.999999439624929e-11f;   // _DAT_0059b180
constexpr float kHeightErrMin = 0.001f;  // DAT_00593584
constexpr float kHeightSpring = 30.f;    // _DAT_00593858
constexpr float kBrakeMin2 = 0.001f;     // DAT_00593584
constexpr float kLead = 5.5555552535224706e-05f;  // _DAT_0059b18c
constexpr float kPitchRate = 0.1f;       // _DAT_005935d4
constexpr float kPitchLimitGain = -0.1f; // DAT_00599bf4
constexpr float kLookTorqueMin2 = 9.99999993922529e-09f;  // _DAT_0059b184
constexpr float kAovMin = 5.f;           // DAT_00593738
constexpr float kAovMax = 120.f;         // DAT_0059b198
constexpr float kAovFreeMin = 1.f;       // DAT_00593598 (free flight)
constexpr float kAutozoomDeg = 114.591552734375f;  // _DAT_0059b194 (2 x 180/pi)
constexpr float kAutozoomBase = 90.f;    // DAT_0059abb4
constexpr float kOrbitForceMin2 = 9.999999974752427e-07f;  // DAT_0059a450
constexpr float kPitchHoldMin = 0.009999999776482582f;     // DAT_005935d0
constexpr float kMoveDeltaMin2 = 9.999999747378752e-05f;   // DAT_005938c4
constexpr float kMoveForceMin2 = 9.999999747378752e-05f;   // DAT_005938c4
constexpr float kPi = 3.1415927410125732f;     // DAT_005935a4
constexpr float kTwoPi = 6.2831854820251465f;  // DAT_005935a8
constexpr float kHalfPi = 1.5707963705062866f; // DAT_005938e0
constexpr float kMaxPitchDefault = 1.3f;       // DAT_0059ab6c (0x3fa66666)
constexpr float kMaxPitchHeight = 0.3f;        // DAT_00593704
constexpr float kAovToTorque = 0.011111111380159855f;  // _DAT_00595b14 (1/90)
constexpr float kSleepV2 = 0.1f;          // _DAT_005935d4
constexpr float kSleepW2 = 0.001f;        // DAT_00593584
constexpr float kSleepForce2 = 0.009999999776482582f;  // DAT_005935d0
constexpr float kSleepFilter = 0.9f;      // DAT_0059370c
constexpr float kSleepFilterIn = 0.1f;    // _DAT_005935d4
constexpr float kWakeFilterY = 100000.f;  // 0x47c35000 (FUN_00507060)

// cam+0x1C flag bits.
constexpr uint32_t kCamPitchHold = 0x100;
constexpr uint32_t kCamAutozoom = 0x200;
constexpr uint32_t kCamCollide = 0x400;
constexpr uint32_t kCamSnapLook = 0x800;
constexpr uint32_t kCamSnapMove = 0x1000;
constexpr uint32_t kCamSimulate = 0x2000;

// PE 940 FUN_0047c080 ("GetConfigData"): the named field of the script
// instance java.util.Config, not an engine cvar.
// Fork: the host's Config object (system_config_host), which carries the
// field initialisers of Config.class until Config.<init> runs on the VM.
float cam_config(const char* name) {
  InvObject* cfg = system_config_host();
  return cfg ? tree_field_get_float(cfg, name) : 0.f;
}

// The eleven values the factory FUN_0048cb30 reads into the type object.
struct CameraTypeValues {
  float aov = 0.f;          // type+0x34: camera_AOV (x camera_FOV)
  float torque = 0.f;       // +0x3C
  float force = 0.f;        // +0x40
  float damping = 0.f;      // +0x44
  float perpdamping = 0.f;  // +0x48: -camera_perpdamping
  float max_force = 1.f;    // +0x4C; <= 0 becomes 1
  float near_plane = 0.f;   // +0x50: camera_ext_clipplane
  float far_plane = 0.f;    // +0x54: camera_ext_viewrange
  bool collide = false;     // +0x58: camera_nocollision == 0 (no reader found)
};

// One per type record, read when the type is first used (the exe: when the
// typeof-8 node's payload is parsed and its type object made).
const CameraTypeValues& camera_type_values(int32_t type_id) {
  static std::unordered_map<int32_t, CameraTypeValues> s_types;  // main thread
  auto it = s_types.find(type_id);
  if (it != s_types.end()) return it->second;
  CameraTypeValues t;
  t.aov = cam_config("camera_AOV");
  t.torque = cam_config("camera_torque");
  t.force = cam_config("camera_force");
  t.damping = cam_config("camera_damping");
  t.perpdamping = -cam_config("camera_perpdamping");
  t.near_plane = cam_config("camera_ext_clipplane");
  t.far_plane = cam_config("camera_ext_viewrange");
  t.max_force = cam_config("camera_max_force");
  t.collide = (cam_config("camera_nocollision") == 0.f);
  // `if (FUN_0047cff0("camera_FOV", 0) != 0) aov *= camera_FOV`.
  const float fov = cam_config("camera_FOV");
  if (fov != 0.f) t.aov = fov * t.aov;
  if (t.max_force <= 0.f) t.max_force = 1.f;
  if (native_type_log())
    std::fprintf(stderr,
                 "[native-type] camera type 0x%08X: aov %g torque %g force %g "
                 "damping %g perpdamping %g max_force %g near %g far %g\n",
                 static_cast<unsigned>(type_id), t.aov, t.torque, t.force,
                 t.damping, t.perpdamping, t.max_force, t.near_plane,
                 t.far_plane);
  return s_types.emplace(type_id, t).first->second;
}

// PE 940 FUN_0047f200(id, 0x12): a handle on the viewport node with that id.
// The type is part of the match: a handle of another kind that carries the
// same number is not the viewport.
InvObject* camera_viewport_by_id(int32_t id) {
  return resref_find_by_id_restype(id, 0x12);
}

// What the drive needs of a `move` / `look` target.
struct CamTargetPose {
  bool ok = false;    // the target node exists
  bool body = false;  // it has a body whose rotation applies
  CamV3 pos;          // getinfo 2
  CamV3 body_pos;     // the position of that body (getinfo 1)
  CamM3 rot;          // the body's rotation
  CamV3 vel;          // getinfo 3
};

// A target handle (cam+0x50 `move`, cam+0x60 `look`): bound by id when the
// command arrives (FUN_0051f9b0).
struct CamTarget {
  int32_t id = 0;            // handle+8
  // Fork: the host object found for the id when the command came. Used only
  // for an id that is neither a script-made node (found by its id on every
  // step) nor a pack instance, and only while it is still bound to the id.
  InvObject* obj = nullptr;
};

inline bool cam_id_is_script_node(int32_t id) {
  return (static_cast<uint32_t>(id) >> 16) == 0xFFFFu;  // FUN_00518a40
}

// The host object behind a target at this moment; null when there is none.
InvObject* camera_target_object(const CamTarget& t) {
  if (!t.id) return nullptr;
  if (cam_id_is_script_node(t.id)) return script_node_object(t.id);
  return (t.obj && resref_peek_id(t.obj) == t.id) ? t.obj : nullptr;
}

// PE 940 FUN_0051f970: the destructor of a node unbinds every handle on it,
// so the camera's target handle reads id 0 afterwards. True when the node a
// target was bound to is gone: a script-made node that left the table, or
// (Fork: a host object that stands for a node without a pack record) an
// object that was unbound or bound to another id since. The node of a pack
// record outlives the handles on it.
bool camera_target_gone(const CamTarget& t) {
  if (!t.id) return false;
  if (cam_id_is_script_node(t.id)) return script_node_object(t.id) == nullptr;
  if (rpak_find_entry(t.id)) return false;
  return t.obj != nullptr && resref_peek_id(t.obj) != t.id;
}

// The exe reads a type-1 target (a game instance: the map, a car) through
// its game object: getinfo 1 = its physics instance (pose of the body),
// getinfo 2 = position, getinfo 3 = velocity (FUN_0048a430 steps 6 and 7).
// Host: the game object of a pack instance by its id (the map), else the
// native object of the host object bound to the id.
// A car answers getinfo 2 with the point on the ground under its cfg
// origin and getinfo 1 with its reference body (FUN_004a15c0): `move`
// keeps its distance from the first, `look` aims at the second.
// Fork: a target whose type has no host game object yet (`object`, ...)
// answers with the host's generic GameRef pose; a target that is a physics
// or render instance is not resolved.
CamTargetPose camera_target_pose(const CamTarget& t) {
  CamTargetPose r;
  r.rot = cam_identity();
  if (!t.id) return r;
  InvObject* const obj = camera_target_object(t);
  std::shared_ptr<NativeObject> no;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    auto ni = g_nodes.find(t.id);
    if (ni != g_nodes.end() && ni->second) no = ni->second->object;
    if (!no && obj) {
      auto oi = g_objects.find(obj);
      if (oi != g_objects.end()) no = oi->second;
    }
  }
  if (no) {
    NativeInfo info;
    if (no->getinfo(2, &info)) {
      r.ok = true;
      r.pos = cam_v(info.v[0], info.v[1], info.v[2]);
    }
    float ypr[3] = {0.f, 0.f, 0.f};
    if (r.ok && no->get_ori(ypr)) {
      r.body = true;
      r.rot = cam_ypr_to_matrix(ypr[0], ypr[1], ypr[2]);
    }
    // The body of getinfo 1 has a position of its own for the ground
    // (FUN_004bb570 answers query 2 with zeros); for every other host type
    // getinfo 2 is that position.
    r.body_pos = r.pos;
    float bp[3] = {0.f, 0.f, 0.f};
    if (r.body && no->get_body_pos(bp)) r.body_pos = cam_v(bp[0], bp[1], bp[2]);
    NativeInfo vel;
    if (r.ok && no->getinfo(3, &vel))
      r.vel = cam_v(vel.v[0], vel.v[1], vel.v[2]);
    return r;
  }
  if (obj) {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto gi = g_refs.find(obj);
    if (gi != g_refs.end() && !gi->second.empty) {
      const GameRefState& g = gi->second;
      r.ok = true;
      r.body = true;
      r.pos = cam_v(g.px, g.py, g.pz);
      r.body_pos = r.pos;
      r.rot = cam_ypr_to_matrix(g.oy, g.op, g.or_);
      r.vel = cam_v(g.vx, g.vy, g.vz);
    }
  }
  return r;
}

// PE 940 FUN_004892b0: the 0x1B4-byte camera object. Field comments give
// the exe offset.
class CameraObject final : public NativeObject {
 public:
  CameraTypeValues type_;

  // The body (the exe: a physics instance, cam+0x40, of the type's body
  // type; state slot at body + [body+0x20C] * 0xD4).
  CamV3 pos_;                      // slot+0x64
  CamV3 vel_;                      // slot+0x70
  CamM3 rot_ = cam_identity();     // slot+0xA4
  CamV3 ang_momentum_;             // slot+0x7C
  CamV3 ang_vel_;                  // slot+0xD4
  CamV3 force_filter_;             // body+0x0C
  bool asleep_ = false;            // body+0x58 bit 0x80

  bool activated_ = false;         // +0x10 bit 0: free flight
  uint32_t flags_ = 0;             // +0x1C
  int32_t controller_id_ = 0;      // +0x20 handle
  CamTarget move_;                 // +0x50
  CamTarget look_;                 // +0x60
  float distance_ = 0.f;           // +0x90
  float look_distance_ = 0.f;      // +0x94
  float torque_ = 0.f;             // +0x98
  float spring_ = 0.f;             // +0x9C
  float damping_ = 0.f;            // +0xA0
  float perpdamping_ = 0.f;        // +0xA4 (negative)
  float height_ = 0.f;             // +0xA8
  float look_smooth_ = 0.5f;       // +0xAC
  float move_smooth_ = 0.5f;       // +0xB0
  float roll_ = 0.f;               // +0xB4
  float pitch_ = -0.3f;            // +0xB8 (0xbe99999a)
  float maxpitch_ = kMaxPitchDefault;  // +0xBC
  float autozoom_radius_ = 0.f;    // +0xC0
  float autozoom_blend_ = 1.f;     // +0xC4
  float autozoom_speed_ = 0.f;     // +0xC8
  CamV3 move_offset_ = cam_v(0.f, 1.f, 3.f);    // +0xCC
  CamV3 look_offset_ = cam_v(0.f, 0.f, -5.f);   // +0xD8
  CamV3 move_ref_;                 // +0xF0: camera-local reference of `move`
  CamV3 look_origin_;              // +0xFC: never written, zero
  CamV3 look_axis_ = cam_v(0.f, 0.f, -1.f);     // +0x108
  CamV3 move_smoothed_;            // +0x114
  CamV3 look_smoothed_;            // +0x120
  CamV3 ctl_force_;                // +0x12C (camera-local)
  CamV3 ctl_torque_;               // +0x138 (camera-local)
  CamV3 last_delta_;               // +0x144
  CamM3 target_rot_ = cam_identity();  // +0x150
  float aov_ = 0.f;                // +0x180
  float aov_target_ = 0.f;         // +0x184
  float zoom_rate_ = 3.f;          // +0x188
  float near_ = 0.f;               // +0x18C
  float far_ = 0.f;                // +0x190
  float dist_min_ = 2.5f;          // +0x194
  float dist_max_ = 10.f;          // +0x198
  float angle_ = 0.f;              // +0x19C
  float angle_gain_ = 0.f;         // +0x1A0
  float angle_speed_ = 0.f;        // +0x1A4
  float angle_base_ = 0.f;         // +0x1A8
  int32_t create_flag1_ = 0;       // +0x1AC
  int32_t create_flag2_ = 0;       // +0x1B0
  float lod_amp_ = 1.f;            // camera params +0x38
  float lod_bias_ = 0.f;           // camera params +0x34

  // Fork: the renderer camera (params node + `r_camera` + CameraNode) is
  // keyed by the GameRef the script holds; its parent is the node the
  // camera was created under (the pass takes the world-tree root of that).
  InvObject* key_ = nullptr;
  InvObject* root_parent_ = nullptr;
  // Fork: the clock of the exe's physics world (world+0, PTR_DAT_005dadf0),
  // read by `angle`. Only differences are used, so the camera counts its own
  // steps.
  float clock_ = 0.f;
  uint32_t steps_ = 0, wakes_ = 0, sleeps_ = 0;
  // Fork (diagnostic): what the last `[camera] ctl` line said.
  bool ctl_logged_ = false;
  int32_t ctl_log_id_ = 0;
  float ctl_log_v_[3] = {0.f, 0.f, 0.f};

  bool registered() const {
    return render_d3d9_camera_get(key_, nullptr, nullptr, nullptr, nullptr,
                                  nullptr, nullptr);
  }

  // The body pose is a world pose: `r_camera` hangs under the world-tree
  // root and its bone00 follows the body (FUN_004ec3b0). FUN_00569970 gives
  // the Ypr the renderer builds its matrix from.
  void push_pose() {
    float y = 0.f, p = 0.f, r = 0.f;
    cam_matrix_to_ypr(rot_, &y, &p, &r);
    render_d3d9_mesh_set_transform(key_, pos_.x, pos_.y, pos_.z, y, p, r, 1.f,
                                   1.f, 1.f);
  }

  // FUN_0048a430 tail: params+0x10 = aov, +0x14 = near x 10, +0x18 = far x
  // 10 (host units are metres).
  void push_params() {
    render_d3d9_camera_set_aov(key_, aov_);
    render_d3d9_camera_set_planes(key_, near_, far_);
  }

  // PE 940 FUN_0048ca50 -> FUN_00507060: a sleeping body is woken and its
  // force filter set to (0, 100000, 0), which keeps it awake for about 160
  // steps.
  void wake() {
    if (!asleep_) return;
    asleep_ = false;
    force_filter_ = cam_v(0.f, kWakeFilterY, 0.f);
    ++wakes_;
  }

  // PE 940 FUN_00506d90: flag 0x80, velocities, momentum and accumulators
  // zeroed.
  void sleep() {
    asleep_ = true;
    vel_ = CamV3();
    ang_vel_ = CamV3();
    ang_momentum_ = CamV3();
    ++sleeps_;
  }

  CamV3 world_point(CamV3 local) const {  // FUN_005060d0
    return pos_ + cam_mul(rot_, local);
  }
  CamV3 point_velocity(CamV3 local) const {  // FUN_005062e0
    return vel_ + cam_cross(ang_vel_, cam_mul(rot_, local));
  }

  // ---- vt+0x10 FUN_0048a3c0 ------------------------------------------------
  void destroy() override {
    // The exe releases the four handles of the object; `r_camera` goes with
    // the node and takes its CameraNode off the viewport.
    render_d3d9_camera_destroy(key_);
    render_d3d9_mesh_destroy(key_);  // the pose entry of the key
  }

  // ---- vt+0x1C FUN_0048c940 ("ffffff") ---------------------------------------
  bool set_matrix(const float pos[3], const float ypr[3]) override {
    // FUN_004eac50: the body pose; then the velocity and the angular
    // momentum are zeroed (FUN_00506ce0) and an awake body is put to sleep
    // (FUN_00506d90).
    pos_ = cam_v(pos[0], pos[1], pos[2]);
    rot_ = cam_ypr_to_matrix(ypr[0], ypr[1], ypr[2]);
    vel_ = CamV3();
    ang_vel_ = CamV3();
    ang_momentum_ = CamV3();
    if (!asleep_) sleep();
    push_pose();
    return true;
  }

  // ---- vt+0x3C FUN_0048bbe0 ----------------------------------------------------
  bool getinfo(int32_t query, NativeInfo* out) override {
    switch (query) {
      case 2:  // the body position
        out->v[0] = pos_.x;
        out->v[1] = pos_.y;
        out->v[2] = pos_.z;
        return true;
      case 3:  // Fork: answered here; the exe asks the physics instance
        out->v[0] = vel_.x;
        out->v[1] = vel_.y;
        out->v[2] = vel_.z;
        return true;
      case 8:
      case 9:
      case 0x1c:
      case 0x30:
        return true;
      case 10:  // create flag 1 (its reader was not found)
        out->i = create_flag1_;
        return create_flag1_ != 0;
      case 0x22:  // the `r_camera` handle. Fork: the renderer key
        out->obj = key_;
        return key_ != nullptr;
      default:
        return false;  // 1: Fork, the body is no physics instance
    }
  }

  bool get_ori(float ypr[3]) override {
    cam_matrix_to_ypr(rot_, &ypr[0], &ypr[1], &ypr[2]);
    return true;
  }

  // *FUN_0047c640(cam+0x20, 0x43, n): logical axis n of the bound
  // controller, getinfo 0x43 of the `player` object (java.io.Controller).
  // Fork: 0 when the controller's node has no game object (the exe reads
  // through the pointer the query returns).
  float ctl_axis(int32_t n) const {
    InvObject* ctrl = script_node_object(controller_id_);
    NativeInfo info;
    info.arg = n;
    return (ctrl && native_getinfo(ctrl, 0x43, &info)) ? info.v[0] : 0.f;
  }

  // Fork (diagnostic, SLRR_PE_NATIVE_PROF): `[camera] ctl t=<s>
  // controller=0x<id> a7=<v> a8=<v> a10=<v> set=<distance> dt=<s>` when the
  // controller or one of the three axis values differs from the tick
  // before. t = the loop clock of the key and mouse scripts, set = the
  // distance after this tick, dt = the tick's own.
  void ctl_log(float v7, float v8, float v10, float dt) {
    if (!native_type_log()) return;
    if (ctl_logged_ && ctl_log_id_ == controller_id_ && ctl_log_v_[0] == v7 &&
        ctl_log_v_[1] == v8 && ctl_log_v_[2] == v10)
      return;
    ctl_logged_ = true;
    ctl_log_id_ = controller_id_;
    ctl_log_v_[0] = v7;
    ctl_log_v_[1] = v8;
    ctl_log_v_[2] = v10;
    std::fprintf(stderr,
                 "[camera] ctl t=%.3f controller=0x%08X a7=%g a8=%g a10=%g "
                 "set=%.4f dt=%.4f\n",
                 native_loop_time(), static_cast<unsigned>(controller_id_),
                 v7, v8, v10, distance_, dt);
  }

  // ---- vt+0x2C FUN_004897b0, once per game tick ------------------------------
  void control(float dt) override {
    if (!activated_) {
      ctl_force_ = CamV3();
      ctl_torque_ = CamV3();
      if (controller_id_ != 0) {
        // A controller bound with `control <id>` (cam+0x28 != 0): its axes
        // 7 / 8 are a camera-local force on the body, +x right and +y up,
        // while the `move` spring holds the distance and the `look` spring
        // keeps aiming: an orbit. Axis 10 scales the distance.
        const float v7 = ctl_axis(7);
        const float v8 = ctl_axis(8);
        const float a7 = v7 * kCtlForceX;
        const float a8 = v8 * kCtlForceY;
        ctl_force_ = cam_v(a7, a8, 0.f);
        const float len2 = cam_len2(ctl_force_);
        if (kCtlForceCap2 < len2)
          ctl_force_ = ctl_force_ * (kCtlForceCap / std::sqrt(len2));
        // With a look target (cam+0x68 != 0) also a torque, from the values
        // before the cap, times the `torque` gain cam+0x98.
        if (look_.id != 0)
          ctl_torque_ = cam_v(a8 * kCtlTorqueX * torque_,
                              a7 * kCtlTorqueY * torque_, 0.f);
        // distance *= 1 + axis10 * 100 * dt, kept inside the `dist` limits
        // (the bound it passed).
        const float v10 = ctl_axis(10);
        const float d = (v10 * kCtlDistRate * dt + 1.f) * distance_;
        distance_ = d;
        if (d < dist_min_)
          distance_ = dist_min_;
        else if (dist_max_ < d)
          distance_ = dist_max_;
        ctl_log(v7, v8, v10, dt);
      }
    } else {
      // Free flight from the controller axes 1..8, 10 and 11. Fork: all
      // axes read 0 here (no script of M4 activates a camera). Axis 11
      // scales the aov target, which is then kept in [1, 120].
      ctl_force_ = CamV3();
      ctl_torque_ = CamV3();
      float a = (0.f + 1.f) * aov_target_;
      if (a <= kAovFreeMin) a = kAovFreeMin;
      if (kAovMax <= a) a = kAovMax;
      aov_target_ = a;
    }
    // Always: aov += (target - aov) * rate * dt; the control torque is
    // scaled by aov / 90.
    aov_ = (aov_target_ - aov_) * zoom_rate_ * dt + aov_;
    ctl_torque_ = ctl_torque_ * (aov_ * kAovToTorque);
    // The exe writes the params node at the end of the next drive step,
    // which is before the frame is drawn. Fork: also here, because the host
    // skips the drive call on a frame whose dt is 0.
    push_params();
  }

  // ---- vt+0x30 FUN_0048a430, once per step of the exe's world ----------------
  // The exe's game tick FUN_0047e050 cuts its dt (capped at 0.1) into world
  // steps of h = 0.02 at most: `rem` when rem < h, rem / 2 when rem <= 2h,
  // else h (FUN_00502060 caller). Each step: drag (FUN_004f8460), this
  // type's forces, integration (FUN_005001c0), sleep test (FUN_00502530).
  // Fork: the host's soft world keeps its own 0.05 s substep, so the camera
  // cuts the frame dt into the exe's steps itself.
  void drive(float frame_dt) override {
    float rem = frame_dt;
    if (rem > kTickDtCap) rem = kTickDtCap;
    while (rem > 0.f) {
      float st = rem;
      if (rem >= kWorldStep) {
        st = kWorldStep;
        if (rem - kWorldStep <= kWorldStep) st = rem * 0.5f;
      }
      if (st < kWorldMinStep) break;
      rem -= st;
      step(st);
    }
    push_pose();
    push_params();
  }

  void step(float dt) {
    CamV3 F, T;  // the body's force and torque accumulators (slot+0x8C / +0xEC)
    // FUN_004f8460, awake bodies only: F += type+0x98 * v. No gravity (body
    // flag 2).
    if (!asleep_) F = F + vel_ * kCamLinDrag;
    forces(&F, &T);
    if (!asleep_) integrate(dt, F, T);
    clock_ += dt;
    ++steps_;
  }

  // FUN_0048a430. `F` and `T` are world vectors.
  void forces(CamV3* F, CamV3* T) {
    float yaw = 0.f, pitch = 0.f, roll = 0.f;
    cam_matrix_to_ypr(rot_, &yaw, &pitch, &roll);
    const float w_x = cam_dot(cam_col(rot_, 0), ang_vel_);
    const float w_z = cam_dot(cam_col(rot_, 2), ang_vel_);

    // 1. Roll spring (asm 0x48a48d..0x48a56e): a camera-local torque about
    // z that takes the Ypr roll to cam+0xB4.
    const float tz = (roll - roll_) * kRollSpring - w_z * kRollDampA * kRollDampB;
    if (kRollTorqueMin2 < tz * tz) {
      *T = *T + cam_mul(rot_, cam_v(0.f, 0.f, tz));  // FUN_00505090
      wake();
    }

    // 2. Control force and torque of FUN_004897b0, camera-local.
    if (kCtlForceMin2 < cam_len2(ctl_force_) ||
        kCtlTorqueMin2 < cam_len2(ctl_torque_)) {
      *F = *F + cam_mul(rot_, ctl_force_);   // FUN_00504e70
      *T = *T + cam_mul(rot_, ctl_torque_);  // FUN_00505090
      wake();
    }

    // A target whose node was destroyed: the handle reads id 0
    // (FUN_0051f970), which the steps below take as "no move target"
    // (distance 0, the body is braked) and "no look target" (no torque).
    // nodes_gone does this when the node is retired; here for a node that
    // left the table without a destroy.
    if (camera_target_gone(move_)) move_ = CamTarget();
    if (camera_target_gone(look_)) look_ = CamTarget();

    const CamTargetPose move_t = camera_target_pose(move_);

    // 3. `height`: a vertical spring to the height (above the move target
    // when there is one).
    if (height_ != 0.f) {
      float h = height_;
      if (move_.id != 0 && move_t.ok) h = height_ + move_t.pos.y;
      const float err = h - pos_.y;
      if (kHeightErrMin < std::fabs(err)) {
        *F = *F + cam_v(0.f, err * kHeightSpring - (vel_.y + vel_.y), 0.f);
        wake();
      }
    }

    // 4. Free flight: only a brake.
    if (activated_) {
      const CamV3 brake = vel_ * -1.f;
      if (kBrakeMin2 < cam_len2(brake)) {
        *F = *F + brake;  // FUN_005051e0
        wake();
      }
      return;
    }

    // 5. A target whose world-tree root differs from the camera's is
    // released (Fork: not ported; the host has no node identity before M2
    // step 6). Without a move target the distance is 0, and without a
    // distance the body is braked.
    if (0.f < distance_ && move_.id == 0) distance_ = 0.f;
    if (distance_ <= 0.f) {
      const CamV3 brake = vel_ * -1.f;
      if (kBrakeMin2 < cam_len2(brake)) {
        *F = *F + brake;
        wake();
      }
    }

    // 6. LOOK: a torque that turns the camera-local axis cam+0x108 onto the
    // target point.
    if (look_.id != 0) {
      const CamTargetPose look_t = camera_target_pose(look_);
      // Type-1 target: with a body (getinfo 1) the offset rotated by the
      // body plus the body's position, else the offset plus getinfo 2; the
      // velocity is getinfo 3.
      CamV3 target = look_offset_;
      if (look_t.body) target = cam_mul(look_t.rot, target);
      target = target + (look_t.body ? look_t.body_pos : look_t.pos);
      const CamV3 target_vel = look_t.vel;
      if ((flags_ & kCamSnapLook) == 0) {
        look_smoothed_ = look_smoothed_ + (target - look_smoothed_) * look_smooth_;
        target = look_smoothed_;
      } else {
        look_smoothed_ = target;
        flags_ &= ~kCamSnapLook;
      }
      CamV3 d = target - world_point(look_origin_);
      // Lead: d += aov * |d| * 5.5555e-5 * target velocity.
      const float lead = aov_ * std::sqrt(cam_len2(d)) * kLead;
      d = d + target_vel * lead;  // Fork: x DAT_005fe10c (1) with `simulate`
      // Pitch limit: a camera-local torque about x past cam+0xBC.
      const float pit = w_x * kPitchRate + pitch;
      if (maxpitch_ < std::fabs(pit)) {
        const float lim = (pit < 0.f) ? -maxpitch_ : maxpitch_;
        *T = *T + cam_mul(rot_, cam_v((pit - lim) * kPitchLimitGain, 0.f, 0.f));
      }
      const float len = std::sqrt(cam_len2(d));
      look_distance_ = len;
      if (len > 0.f) {  // Fork: guard; the exe divides
        const CamV3 n = d * (1.f / len);
        const CamV3 a = cam_mul(rot_, look_axis_);
        const CamV3 tq = cam_cross(a, n) * torque_;  // asm 0x48af78..0x48b03d
        if (kLookTorqueMin2 < cam_len2(tq)) {
          *T = *T + tq;  // FUN_00505180, world torque
          wake();
        }
      }
      // Autozoom: the aov target follows the angle the target's radius
      // (plus its speed) subtends.
      if ((flags_ & kCamAutozoom) != 0 && 0.f < autozoom_radius_) {
        const float phi = static_cast<float>(std::atan2(
            static_cast<double>(autozoom_speed_ * std::sqrt(cam_len2(target_vel)) +
                                autozoom_radius_),
            static_cast<double>(look_distance_)));
        float a = (1.f - autozoom_blend_) * (kAutozoomBase - phi * kAutozoomDeg) +
                  phi * kAutozoomDeg;
        if (a <= kAovMin) a = kAovMin;
        if (kAovMax <= a) a = kAovMax;
        aov_target_ = a;
      }
    }

    // 7. MOVE: a spring that keeps the body cam+0x90 from the target point,
    // along whatever direction it is in.
    if (move_.id != 0) {
      target_rot_ = cam_identity();  // FUN_005663c0
      const CamV3 ref = world_point(move_ref_);
      const CamV3 ref_vel = point_velocity(move_ref_);
      // Type-1 target: offset + position, NOT rotated; velocity 0; cam+0x150
      // takes the rotation of the target's body.
      CamV3 target = move_offset_ + move_t.pos;
      const CamV3 target_vel = CamV3();
      if (move_t.body) target_rot_ = move_t.rot;
      // The water plane of the world (world+0x80; off by default) pushes
      // the point 1.0 off it. Fork: no water plane on the host.
      if ((flags_ & kCamSnapMove) == 0) {
        move_smoothed_ = move_smoothed_ + (target - move_smoothed_) * move_smooth_;
        target = move_smoothed_;
      } else {
        move_smoothed_ = target;
        flags_ &= ~kCamSnapMove;
      }
      const CamV3 delta = target - ref;
      const CamV3 rel = target_vel - ref_vel;
      last_delta_ = delta;

      // `angle`: orbit around the target, a tangential force from the
      // bearing error.
      if (0.f < angle_gain_) {
        if (angle_speed_ != 0.f) {
          float a = (clock_ - angle_base_) * angle_speed_;
          // FUN_0048cae0(a, 2 pi), then into (-pi, pi].
          a = a - static_cast<float>(static_cast<int>(a / kTwoPi)) * kTwoPi;
          while (kPi < a) a -= kTwoPi;
          while (a < -kPi) a += kTwoPi;
          angle_ = a;
        }
        float ty = 0.f, tp = 0.f, tr = 0.f;
        cam_matrix_to_ypr(target_rot_, &ty, &tp, &tr);
        const float bearing = static_cast<float>(std::atan2(
            static_cast<double>(-delta.x), static_cast<double>(-delta.z)));
        float e = (angle_ + ty) - bearing;
        while (kPi < e) e -= kTwoPi;
        while (e < -kPi) e += kTwoPi;
        e *= angle_gain_;
        const CamV3 f = cam_mul(cam_ypr_to_matrix(bearing, 0.f, 0.f),
                                cam_v(e, 0.f, 0.f));
        if (kOrbitForceMin2 < cam_len2(f)) {
          *F = *F + f;
          wake();
        }
      }

      if (0.f <= distance_) {
        // Pitch hold: a vertical force from the pitch error.
        if ((flags_ & kCamPitchHold) != 0) {
          const float e = pitch_ - pitch;
          if (kPitchHoldMin < std::fabs(e)) {
            *F = *F + cam_v(0.f, -e, 0.f);  // FUN_00505360
            wake();
          }
        }
        // The move force (asm 0x48b8ff..0x48bb34).
        const float len2 = cam_len2(delta);
        if (0.f < distance_ && kMoveDeltaMin2 < len2) {
          const float len = std::sqrt(len2);
          const CamV3 n = delta * (1.f / len);
          const float spring = (len - distance_) * spring_;
          const float damp = cam_dot(n, rel) * damping_;
          CamV3 f = n * damp + n * spring;
          const float f2 = cam_len2(f);
          if (type_.max_force * type_.max_force < f2)
            f = f * (type_.max_force / std::sqrt(f2));
          const float vn = -cam_dot(vel_, n);
          f = f + (n * vn + vel_) * perpdamping_;
          if (kMoveForceMin2 < cam_len2(f)) {
            *F = *F + f;  // FUN_005051e0
            wake();
          }
        }
      }
    }
  }

  // FUN_00506530 with the accumulated force and torque held over the step,
  // then the sleep test FUN_00502530 (m2/cam_sim.py).
  void integrate(float dt, CamV3 F, CamV3 T) {
    const CamV3 acc = F * kCamInvMass;
    const CamV3 p2 = pos_ + vel_ * dt + acc * (0.5f * dt * dt);
    const CamV3 v2 = vel_ + acc * dt;
    const CamM3 r2 =
        cam_orthonormalise(cam_mm(cam_rot_vector(ang_vel_ * dt), rot_));
    // L *= 1 + invMass * type+0x9C * dt * 100 (angular damping).
    const float ang_fac = kCamInvMass * kCamAngDamp * dt * 100.f + 1.f;
    CamV3 l2 = (ang_momentum_ + T * dt) * ang_fac;
    CamV3 w2 = l2 * kCamInvInertia;
    // Scaled down above 20 rad/s.
    const float wsq = cam_len2(w2) * 0.0025f;
    if (wsq > 1.f) {
      l2 = l2 * (1.f / wsq);
      w2 = l2 * kCamInvInertia;
    }
    // filt = 0.9 filt + 0.1 F; asleep when the new velocities, the torque
    // and the filtered force are all small. The step is then discarded.
    force_filter_ = force_filter_ * kSleepFilter + F * kSleepFilterIn;
    const float im2 = kCamInvMass * kCamInvMass;
    if (cam_len2(v2) < kSleepV2 && cam_len2(w2) < kSleepW2 &&
        cam_len2(T) * im2 < kSleepForce2 &&
        cam_len2(force_filter_) * im2 < kSleepForce2) {
      sleep();
      return;
    }
    pos_ = p2;
    vel_ = v2;
    rot_ = r2;
    ang_momentum_ = l2;
    ang_vel_ = w2;
  }

  // PE 940 FUN_0051f970: cam+0x20 (controller), cam+0x50 (`move`) and
  // cam+0x60 (`look`) are handles on their nodes like any other.
  void nodes_gone(const std::vector<int32_t>& ids) override {
    auto gone = [&ids](int32_t id) {
      return id != 0 && std::binary_search(ids.begin(), ids.end(), id);
    };
    if (gone(move_.id)) move_ = CamTarget();
    if (gone(look_.id)) look_ = CamTarget();
    if (gone(controller_id_)) controller_id_ = 0;
  }

  // ---- vt+0x38 FUN_0048bce0: commands --------------------------------------
  void bind_target(CamTarget* t, int32_t id) {  // FUN_0051f9b0
    t->id = id;
    t->obj = (id && !cam_id_is_script_node(id)) ? resref_find_by_id(id)
                                                : nullptr;
  }

  // `render <vp id> <a> <b> <pri> <flags>` (asm 0x48bd50..0x48beca). Only
  // the viewport id and the priority are used; the second, third and fifth
  // number are parsed and never read.
  void cmd_render(const char* text) {
    int32_t vp_id = 0, b = 0, c = 0, pri = -1, e = -1;
    engine_sscanf(text, "%*s %d %d %d %d %d", &vp_id, &b, &c, &pri, &e);
    if (static_cast<uint32_t>(pri) > 0xffff0000u) pri = 0;  // absent: 0
    if (vp_id != 0) {
      // FUN_00420900 finds or makes the CameraNode of `r_camera` on that
      // viewport (bound or not); FUN_00420080 -> FUN_00464f60 sets its
      // priority and puts it on the viewport's active camera list.
      // Fork: a freshly hooked CameraNode is not drawn for 4 frames and the
      // Present is skipped meanwhile (FUN_00460840, cam+0x73B0,
      // _DAT_005da398); the host draws it from the next frame on.
      InvObject* vp = camera_viewport_by_id(vp_id);
      if (vp) {
        if (registered())
          render_d3d9_camera_set_viewport(key_, vp, pri);
        else
          render_d3d9_camera_create(key_, root_parent_, vp, pri, aov_, near_,
                                    far_, lod_bias_, lod_amp_, 1, 0);
      }
      if (native_type_log())
        std::fprintf(stderr,
                     "[native-type] camera %p render: viewport id %d -> %p, "
                     "priority %d\n",
                     static_cast<void*>(key_), vp_id, static_cast<void*>(vp),
                     pri);
      // Create flag 2: the body joins the two-row table at 0x5FE120
      // (FUN_0047b560; inferred: the audio listeners). Fork: not ported.
    }
    // Always: far = Config.camera_ext_viewrange, the fog of the world-tree
    // root (its getinfo 0x4A; Fork: the ground type answers none, fog off)
    // and the aspect override zeroed (asm 0x48bebe), so the projection uses
    // the viewport's pixel width / height of the frame.
    far_ = cam_config("camera_ext_viewrange");
    render_d3d9_camera_set_planes(key_, near_, far_);
    render_d3d9_camera_set_aspect(key_, 0.f);
  }

  // `hide <vp id>`: FUN_00420ad0 + FUN_00420f60 take the CameraNode off that
  // viewport and delete it.
  void cmd_hide(const char* text) {
    int32_t vp_id = 0;
    engine_sscanf(text, "%*s %d", &vp_id);
    if (vp_id == 0) return;
    InvObject* vp = camera_viewport_by_id(vp_id);
    void* cur = nullptr;
    if (vp && render_d3d9_camera_get(key_, &cur, nullptr, nullptr, nullptr,
                                     nullptr, nullptr) &&
        cur == vp)
      render_d3d9_camera_destroy(key_);
    if (native_type_log())
      std::fprintf(stderr, "[native-type] camera %p hide: viewport id %d -> %p\n",
                   static_cast<void*>(key_), vp_id, static_cast<void*>(vp));
    // Create flag 2: the listener row goes (FUN_0047b8c0). Fork: not ported.
  }

  bool event(int32_t type, const char* text) override {
    // The handler returns 1 for every event; only type 0x10 (GameRef.command)
    // is read. A command the camera does not know is dropped by the exe too.
    if (type != 0x10) return true;
    if (!text) text = "";
    char word[64];
    word[0] = 0;
    engine_sscanf(text, "%63s", word);  // "%s" into a 64-byte buffer
    if (word_is(word, "render")) {
      cmd_render(text);
    } else if (word_is(word, "fog")) {
      // "%*s %d &f &f" (sic): the `&f` are literals and the test is for
      // four conversions, which cannot happen, so this always turns the
      // camera's fog off (params+0x24 = 0). Fork: the camera pass has no
      // fog of its own.
    } else if (word_is(word, "hide")) {
      cmd_hide(text);
    } else if (word_is(word, "activate")) {
      // Free flight: controller = id, both targets released, `r_camera`
      // re-bound to the body with a zero offset (FUN_004ec540).
      int32_t id = 0;
      engine_sscanf(text, "%*s %d", &id);
      controller_id_ = id;
      activated_ = true;
      move_ = CamTarget();
      look_ = CamTarget();
    } else if (word_is(word, "deactivate")) {
      controller_id_ = 0;
      activated_ = false;
    } else if (word_is(word, "control")) {
      // Sent by the `player` type: its `camera <id>` command answers with
      // `control <player id>` (FUN_004d2d20).
      int32_t id = 0;
      engine_sscanf(text, "%*s %d", &id);
      controller_id_ = id;
    } else if (word_is(word, "look")) {
      int32_t id = 0;
      float off[3] = {0.f, 0.f, 0.f};
      float axis[3] = {0.f, 0.f, -1.f};
      float gain = torque_;
      float dropped[3] = {0.f, 0.f, 0.f};
      engine_sscanf(text, "%*s %d %f,%f,%f %f,%f,%f %f %f,%f,%f", &id, &off[0],
                    &off[1], &off[2], &axis[0], &axis[1], &axis[2], &gain,
                    &dropped[0], &dropped[1], &dropped[2]);
      if (look_.id == 0) flags_ |= kCamSnapLook;  // the first target ever
      bind_target(&look_, id);
      look_offset_ = cam_v(off[0], off[1], off[2]);
      const float a2 =
          axis[1] * axis[1] + axis[0] * axis[0] + axis[2] * axis[2];
      if (a2 <= 0.f) {
        look_axis_ = cam_v(0.f, 0.f, -1.f);
      } else {
        // FUN_0048ca70: normalised.
        const float inv = 1.f / std::sqrt(a2);
        look_axis_ = cam_v(axis[0] * inv, axis[1] * inv, axis[2] * inv);
      }
      angle_gain_ = 0.f;
      angle_speed_ = 0.f;
      if (0.f < gain) torque_ = gain;
      flags_ &= ~kCamPitchHold;
      autozoom_radius_ = 0.f;
    } else if (word_is(word, "move")) {
      int32_t id = 0;
      float off[3] = {0.f, 0.f, 0.f};
      float dist = 0.f;
      float ref[3] = {0.f, 0.f, 0.f};
      engine_sscanf(text, "%*s %d %f,%f,%f %f %f,%f,%f", &id, &off[0], &off[1],
                    &off[2], &dist, &ref[0], &ref[1], &ref[2]);
      if (move_.id == 0) flags_ |= kCamSnapMove;
      bind_target(&move_, id);
      flags_ &= ~kCamPitchHold;
      move_offset_ = cam_v(off[0], off[1], off[2]);
      move_ref_ = cam_v(ref[0], ref[1], ref[2]);
      angle_gain_ = 0.f;
      angle_speed_ = 0.f;
      distance_ = dist;
      if (dist < dist_min_) dist_min_ = dist;
      if (dist_max_ < dist) dist_max_ = dist;
    } else if (word_is(word, "dist")) {
      engine_sscanf(text, "%*s %f %f", &dist_min_, &dist_max_);
    } else if (word_is(word, "smooth")) {
      engine_sscanf(text, "%*s %f %f", &look_smooth_, &move_smooth_);
    } else if (word_is(word, "torque")) {
      engine_sscanf(text, "%*s %f", &torque_);
    } else if (word_is(word, "force")) {
      engine_sscanf(text, "%*s %f %f %f", &spring_, &damping_, &perpdamping_);
    } else if (word_is(word, "zoom")) {
      engine_sscanf(text, "%*s %f %f", &aov_target_, &zoom_rate_);
      float a = aov_target_;
      if (a <= kAovMin) a = kAovMin;
      if (kAovMax <= a) a = kAovMax;
      aov_target_ = a;
      if (zoom_rate_ < 0.f) {
        // At once: the aov, the control torque scale and the params node.
        aov_ = a;
        ctl_torque_ = ctl_torque_ * (a * kAovToTorque);
        render_d3d9_camera_set_aov(key_, aov_);
      }
    } else if (word_is(word, "autozoom")) {
      int32_t on = 0;
      engine_sscanf(text, "%*s %d %f %f %f", &on, &autozoom_radius_,
                    &autozoom_blend_, &autozoom_speed_);
      flags_ &= ~kCamAutozoom;
      if (on != 0) flags_ |= kCamAutozoom;
    } else if (word_is(word, "angle")) {
      // Orbit: angle, gain, angular speed, time base (-1 = start from the
      // current bearing).
      angle_ = 0.f;
      angle_base_ = -1.f;
      angle_gain_ = 0.f;
      angle_speed_ = 0.f;
      engine_sscanf(text, "%*s %f %f %f %f", &angle_, &angle_gain_,
                    &angle_speed_, &angle_base_);
      if (angle_speed_ != 0.f && angle_base_ < 0.f) {
        float ty = 0.f, tp = 0.f, tr = 0.f;
        cam_matrix_to_ypr(target_rot_, &ty, &tp, &tr);
        const float bearing = static_cast<float>(
            std::atan2(static_cast<double>(-last_delta_.x),
                       static_cast<double>(-last_delta_.z)));
        // Fork: x DAT_005fe10c (the game's time scale, 1) with `simulate`.
        angle_base_ = -((bearing - ty) / angle_speed_) + clock_;
      }
    } else if (word_is(word, "roll")) {
      engine_sscanf(text, "%*s %f", &roll_);
    } else if (word_is(word, "height")) {
      engine_sscanf(text, "%*s %f", &height_);
      maxpitch_ = (height_ != 0.f) ? kMaxPitchHeight : kMaxPitchDefault;
    } else if (word_is(word, "pitch")) {
      engine_sscanf(text, "%*s %f", &pitch_);
      flags_ &= ~kCamPitchHold;
      if (std::fabs(pitch_) < kHalfPi) flags_ |= kCamPitchHold;
    } else if (word_is(word, "maxpitch")) {
      engine_sscanf(text, "%*s %f", &maxpitch_);
    } else if (word_is(word, "follow")) {
      int32_t id = 0;
      engine_sscanf(text, "%*s %d", &id);
      bind_target(&look_, id);
      bind_target(&move_, id);
      look_offset_ = CamV3();
      move_offset_ = CamV3();
      angle_gain_ = 0.f;
      angle_speed_ = 0.f;
      autozoom_radius_ = 0.f;
    } else if (word_is(word, "nearplane")) {
      engine_sscanf(text, "%*s %f", &near_);
    } else if (word_is(word, "farplane")) {
      engine_sscanf(text, "%*s %f", &far_);
    } else if (word_is(word, "simulate")) {
      // Flag 0x2000: the body is also stepped by vt+0x34 (FUN_004896c0)
      // with (1 - DAT_005fe10c) * dt, so it keeps moving when game time is
      // scaled down. Fork: stored only; the host steps the camera with the
      // frame dt and has no separate game time scale here.
      int32_t on = 0;
      engine_sscanf(text, "%*s %d", &on);
      flags_ &= ~kCamSimulate;
      if (on != 0) flags_ |= kCamSimulate;
    }
    return true;
  }
};

// PE 940 FUN_00489d30 (type vt+0x0C).
std::shared_ptr<NativeObject> camera_create(const NativeCreate& c) {
  auto cam = std::make_shared<CameraObject>();
  cam->type_ = camera_type_values(c.type ? c.type->type_id : 0);
  const CameraTypeValues& type = cam->type_;
  // "%f,%f,%f,%f,%f,%f,%d,%f,%f,%f,%f": position, Ypr, flag word, lodAmp,
  // lodBias, near, far. The engine's scanner: `%d` takes 0x, a missing
  // field keeps its default.
  float pos[3] = {0.f, 0.f, 0.f};
  float ypr[3] = {0.f, 0.f, 0.f};
  int32_t flag_word = 0;
  float lod_amp = 1.f;
  float lod_bias = 0.f;
  float near_plane = type.near_plane;
  float far_plane = type.far_plane;
  engine_sscanf(c.params ? c.params : "", "%f,%f,%f,%f,%f,%f,%d,%f,%f,%f,%f",
                &pos[0], &pos[1], &pos[2], &ypr[0], &ypr[1], &ypr[2],
                &flag_word, &lod_amp, &lod_bias, &near_plane, &far_plane);
  // Object defaults (the rest are the member initialisers).
  cam->aov_ = type.aov;
  cam->aov_target_ = type.aov;
  cam->near_ = near_plane;
  cam->far_ = far_plane;
  cam->torque_ = type.torque;
  cam->spring_ = type.force;
  cam->damping_ = type.damping;
  cam->perpdamping_ = type.perpdamping;
  cam->create_flag1_ = flag_word & 1;
  cam->create_flag2_ = flag_word & 2;
  if (flag_word & 4) cam->flags_ |= kCamAutozoom;
  if (flag_word & 8) cam->flags_ |= kCamPitchHold;
  if (flag_word & 0x10) cam->flags_ |= kCamCollide;
  cam->lod_amp_ = lod_amp;
  cam->lod_bias_ = lod_bias;
  // The physics instance (FUN_004e8a50 -> FUN_004f7830) at the parsed pose,
  // awake. Body flags: 0x4002 always (2 = no gravity), 0x20 = no body-body
  // collision unless create flag 0x10 (the garage camera has it and
  // collides).
  // Fork: the body is this object's own state and collides with nothing;
  // its collision response in the exe was not traced.
  cam->pos_ = cam_v(pos[0], pos[1], pos[2]);
  cam->rot_ = cam_ypr_to_matrix(ypr[0], ypr[1], ypr[2]);
  // The camera params node FUN_00524dc0(.., 0.75, aov, near, far, 0, 1.0, 1)
  // and the render instance `r_camera` under the camera's world-tree root,
  // bone00 on the body. Fork: the renderer camera is registered by the
  // first `render` command, which is also where the exe makes the
  // CameraNode; until then only the pose is kept there.
  // With Config.camera_render a second render instance `cam_mesh`
  // (system.rpk:3) shows the body. Fork: not ported (a debug aid).
  cam->key_ = c.handle;
  cam->root_parent_ = c.parent;
  cam->owns_create_pose = true;
  cam->push_pose();
  // FUN_004ebd90(cam, 0x14, 0): on the control and drive callback lists.
  cam->ticks = true;
  if (native_type_log())
    std::fprintf(stderr,
                 "[native-type] camera %p: pos (%g, %g, %g) ypr (%g, %g, %g) "
                 "flags 0x%X lodAmp %g lodBias %g near %g far %g aov %g\n",
                 static_cast<void*>(c.handle), pos[0], pos[1], pos[2], ypr[0],
                 ypr[1], ypr[2], static_cast<unsigned>(flag_word), lod_amp,
                 lod_bias, near_plane, far_plane, type.aov);
  return cam;
}

}  // namespace

// One `[render-dbg]` line per native camera: the pose the pass uses, the
// values of the renderer camera, the distance to the move target.
void native_camera_debug_dump(FILE* out) {
  if (!out || !native_script_boot()) return;
  std::vector<std::shared_ptr<NativeObject>> cams;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    for (const auto& kv : g_objects) {
      if (!kv.second || kv.second->type_index != kNativeCamera) continue;
      bool seen = false;
      for (const auto& c : cams)
        if (c == kv.second) seen = true;
      if (!seen) cams.push_back(kv.second);
    }
  }
  for (const auto& o : cams) {
    CameraObject* cam = static_cast<CameraObject*>(o.get());
    float yaw = 0.f, pitch = 0.f, roll = 0.f;
    cam_matrix_to_ypr(cam->rot_, &yaw, &pitch, &roll);
    void* vp = nullptr;
    int32_t pri = 0;
    float aov = cam->aov_, dmin = cam->near_, dmax = cam->far_, aspect = 0.f;
    const bool reg =
        render_d3d9_camera_get(cam->key_, &vp, &pri, &aov, &dmin, &dmax, &aspect);
    float dist = 0.f;
    CamV3 target;
    const CamTargetPose mt = camera_target_pose(cam->move_);
    if (cam->move_.id != 0 && mt.ok) {
      target = cam->move_offset_ + mt.pos;
      dist = std::sqrt(cam_len2(target - cam->world_point(cam->move_ref_)));
    }
    std::fprintf(out,
                 "[render-dbg] native camera %p registered=%d vp=%p pri=%d "
                 "eye=(%.4f, %.4f, %.4f) ypr=(%.5f, %.5f, %.5f) aov=%.2f "
                 "near=%g far=%g aspect=%g distance=%.4f move=0x%08X at "
                 "(%g, %g, %g) set %g look=0x%08X asleep=%d steps=%u wakes=%u "
                 "sleeps=%u\n",
                 static_cast<void*>(cam->key_), reg ? 1 : 0, vp, pri,
                 cam->pos_.x, cam->pos_.y, cam->pos_.z, yaw, pitch, roll, aov,
                 dmin, dmax, aspect, dist,
                 static_cast<unsigned>(cam->move_.id), target.x, target.y,
                 target.z, cam->distance_,
                 static_cast<unsigned>(cam->look_.id), cam->asleep_ ? 1 : 0,
                 cam->steps_, cam->wakes_, cam->sleeps_);
  }
}
