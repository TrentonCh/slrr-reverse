// Fragment 11 of NativeTypes.cpp - included, not compiled on its own.
//
// M6 step 1: what the cursor's pick finds of the part and car objects, and
// GameRef.getInfo(GII_SIZE) of such an object.
// With the shipped data a part, and the car itself, is picked only through
// its click body (exe_mech X 1.1): the mesh the type's `click` record names
// (PartTypeDesc::click_shape, NativeTypes_part4.inc), placed by the click
// line in the part's cfg frame. The cfg's `body` entries of a type with a
// click line carry body flag 0x4000 and are never hit; the compound, the
// car's `car_root` and its wheel bodies have no triangles (kind 6 without
// any) or carry 0x8020 (X 1.1, Q10).
// Mirrors: FUN_004c1f90 (the click body's pose and owner, asm
// 0x4c24b1..0x4c267a), FUN_004fc140 kinds 6 / 7 (the sphere gate, then
// FUN_004fd6d0 per triangle with the axes of FUN_00504360), FUN_004c3da0
// case 0xB with FUN_005008c0 (GII_SIZE).

namespace {

// The world frame of an object's click body (FUN_004c1f90):
//   - a type with body entries (type+0xFF0 >= 0): a child body of the
//     compound at createPos + R * clickOffset, turned by R * clickRot, with
//     no shape-centre term (0x4c24be..0x4c257a, FUN_00566c10). With the
//     object's pose that is P = W(click_off), R = R_cfg * click_rot, where
//     W(p) = pos_ + R_cfg (p - ref_at) and R_cfg = rot_ * ref_rot^T: the cfg
//     frame PartObject::compound_pos uses;
//   - a type without: the click body IS the compound (FUN_004e8a50 into
//     obj+0x1C), made at createPos turned by R * clickRot; the click offset
//     is not used (0x4c2634..0x4c2664). The host's reference body of such a
//     type is that body (ref_rot = click_rot, ref_at = 0, part_type_derive),
//     so P = pos_, R = rot_.
// The car is a part on its own cfg (FUN_004992e0 runs FUN_004c1f90 first).
void part_click_frame(const PartObject& p, CamV3* at, CamM3* rot) {
  const PartTypeDesc& d = *p.type_;
  if (d.bone_index < 0) {
    *at = p.pos_;
    *rot = p.rot_;
    return;
  }
  const CamM3 rc = cam_mm(p.rot_, part_transpose(d.ref_rot));
  *at = p.pos_ + cam_mul(rc, d.click_off - d.ref_at);
  *rot = cam_mm(rc, d.click_rot);
}

// PE 940 FUN_00504360(normal, &i, &j): the two axes left when the dominant
// component of the normal is dropped.
inline void click_axes(const float* n, int* i, int* j) {
  const float ax = std::fabs(n[0]);
  const float ay = std::fabs(n[1]);
  const float az = std::fabs(n[2]);
  if (ay < ax) {
    if (az <= ax) {
      *i = 1;
      *j = 2;
      return;
    }
    *i = 0;
  } else {
    *i = 0;
    if (az <= ay) {
      *j = 2;
      return;
    }
  }
  *j = 1;
}

// PE 940 FUN_004fd6d0(vertices, normal, indices, segment, best), single
// precision as the exe: the far point A' and the near point B' (the body's
// frame) against one triangle with its stored normal. The near point must
// lie strictly in front (the side the normal points to), the far point on
// or behind; t = |dB / (dA - dB)| from the near point, not above best; no
// hit when both ends are within 2 cm of the plane (DAT_00594790); the
// point on the plane, relative to v0, through the 2D barycentric test on
// the two axes FUN_00504360 keeps, the edges included. 0 = no hit.
float click_tri_t(const float* v, const float* n, const uint32_t* idx,
                  const float* a, const float* b, float best) {
  const float* v0 = v + static_cast<size_t>(idx[0]) * 3;
  const float bx = b[0] - v0[0];
  const float by = b[1] - v0[1];
  const float bz = b[2] - v0[2];
  const float ax = a[0] - v0[0];
  const float ay = a[1] - v0[1];
  const float az = a[2] - v0[2];
  const float d_near = n[0] * bx + n[1] * by + n[2] * bz;
  const float d_far = n[1] * ay + n[0] * ax + n[2] * az;
  if (d_near <= 0.f || 0.f < d_far) return 0.f;
  const float t = std::fabs(d_near / (d_far - d_near));
  if (best < t) return 0.f;
  constexpr float kPlane = 0.02f;  // DAT_00594790
  if (std::fabs(d_far) <= kPlane && std::fabs(d_near) <= kPlane) return 0.f;
  const float p[3] = {(ax - bx) * t + bx, (ay - by) * t + by,
                      (az - bz) * t + bz};
  int i = 0, j = 0;
  click_axes(n, &i, &j);
  const float* v1 = v + static_cast<size_t>(idx[1]) * 3;
  const float* v2 = v + static_cast<size_t>(idx[2]) * 3;
  const float e2j = v2[j] - v0[j];
  const float e2i = v2[i] - v0[i];
  const float e1i = v1[i] - v0[i];
  const float e1j = v1[j] - v0[j];
  const float inv = 1.f / (e2j * e1i - e2i * e1j);  // DAT_00593598 / ..
  const float u = (e2j * p[i] - e2i * p[j]) * inv;
  if (!(0.f <= u && u <= 1.f)) return 0.f;
  const float w = (e1i * p[j] - e1j * p[i]) * inv;
  if (!(0.f <= w && w + u <= 1.f)) return 0.f;
  return t;
}

}  // namespace

// M6 step 1 (host_objects.hpp): every live part and car object whose type
// has a click shape and whose click body is not hidden, with the body's
// world frame from the object's pose of now. Fork: the exe's click body is
// a body of the object's compound and moves with it; the host computes its
// frame from the reference body's pose (the parts on a car are posed on
// their slots, PartObject::set_ref_pose).
// Fork (X 1.7 item 7, m6_spec step 1 item 4, follow-up 2): an object whose
// type has no `click` line has no body here (phy_id_ 0, part_init). The exe
// picks such a type through its cfg `body` entries instead (spheres, boxes,
// meshes recentred by FUN_00508d20; X 1.1: 80 cfgs, traffic and police
// cars, the fake racers; none on the player's path).
// The owners' creation order and node ids are left to click_pick_resolve.
bool native_filter_hidden(void* node) {
  return node && !g_filter_hidden.empty() && g_filter_hidden.count(node) != 0;
}

// M7: the click bodies of the `button` objects (NativeTypes_part13.inc).
void button_click_bodies(std::vector<ClickPickBody>* out);

void click_pick_bodies(std::vector<ClickPickBody>* out) {
  if (!out) return;
  out->clear();
  if (!native_script_boot()) return;
  button_click_bodies(out);
  if (g_parts_live.empty()) return;
  out->reserve(g_parts_live.size());
  for (PartObject* p : g_parts_live) {
    if (p->gone_ || !p->key_ || !p->phy_id_ || p->click_hidden_) continue;
    const ClickShape* s = p->type_->click_shape.get();
    // FUN_004fc140 tests the triangles of kinds 6 and 7 only (a record whose
    // `type` names a sphere, a box or a rod: none is shipped, Q12).
    if (!s || (s->kind != 6 && s->kind != 7) || s->tri.empty()) continue;
    CamV3 at;
    CamM3 r = cam_identity();
    part_click_frame(*p, &at, &r);
    ClickPickBody b;
    b.owner = p->key_;
    b.phy = p->phy_id_;
    b.pos[0] = at.x;
    b.pos[1] = at.y;
    b.pos[2] = at.z;
    for (int i = 0; i < 3; ++i)
      for (int k = 0; k < 3; ++k) b.rot[i][k] = r.m[i][k];
    b.radius = s->radius;
    b.shape = s;
    out->push_back(b);
  }
}

// The creation order of each body's owner node, the order the pick meets
// the bodies in (cursor_pick), and the node's id (0: no live node).
void click_pick_resolve(std::vector<ClickPickBody>* bodies) {
  if (!bodies || bodies->empty()) return;
  std::lock_guard<std::mutex> lock(g_mu);
  for (ClickPickBody& b : *bodies) {
    auto it = g_res.find(b.owner);
    b.seq = it == g_res.end() ? 0 : it->second.seq;
    b.owner_id = it == g_res.end() ? 0 : it->second.id;
  }
}

// FUN_004fc140, kinds 6 / 7 (asm 0x4fc224..0x4fc320): A and B into the
// body's frame (FUN_004af9a0: R^T (p - P)), then every triangle in order
// against the best t so far; a result above 0 and below it is kept, so of
// two equal ones the first stays.
float click_pick_test(const ClickPickBody& body, const float far_pt[3],
                      const float near_pt[3], float best, int32_t* tri) {
  const ClickShape* s = static_cast<const ClickShape*>(body.shape);
  if (!s) return 0.f;
  float a[3], b[3];
  for (int k = 0; k < 3; ++k) {
    a[k] = body.rot[0][k] * (far_pt[0] - body.pos[0]) +
           body.rot[1][k] * (far_pt[1] - body.pos[1]) +
           body.rot[2][k] * (far_pt[2] - body.pos[2]);
    b[k] = body.rot[0][k] * (near_pt[0] - body.pos[0]) +
           body.rot[1][k] * (near_pt[1] - body.pos[1]) +
           body.rot[2][k] * (near_pt[2] - body.pos[2]);
  }
  const size_t ntri = s->tri.size() / 3;
  float got = 0.f;
  for (size_t i = 0; i < ntri; ++i) {
    const float t = click_tri_t(s->xyz.data(), &s->nrm[i * 3], &s->tri[i * 3],
                                a, b, best);
    if (0.f < t && t < best) {
      best = t;
      got = t;
      if (tri) *tri = static_cast<int32_t>(i);
    }
  }
  if (native_type_log()) g_cursor_prof.click_tris += ntri;
  return got;
}

InvObject* click_phy_owner(int32_t phy) {
  if (!phy) return nullptr;
  auto it = g_click_phy.find(phy);
  return it == g_click_phy.end() ? nullptr : it->second;
}

namespace {

// PE 940 FUN_004c3da0 case 0xB (asm 0x4c3ee6..0x4c3f34; X 4.3): r is
// shape+0xC of the compound (obj+0x1C -> body -> type -> shape), the return
// 1 - (int)(r * -100.0) (DAT_0059de04, CVTTSS2SI): 1 + trunc(100 r). A
// compound's shape+0xC is written by FUN_005008c0 when FUN_005032d0 runs on
// it (at the create, and when a child body is attached or detached): the
// box around its children's points (each grown by its margin; children
// with body flag 0x20 left out), starting from the compound's origin, and
// r = sqrt(max(|lo|^2, |hi|^2)) measured from the origin the compound had
// then, BEFORE FUN_005032d0 moves it to the new mass centre. So:
//   - a part with nothing installed on it: the box of its own bodies and
//     its click body in the cfg frame, from the cfg origin (the create
//     position, where the compound was made); detaching it from a car runs
//     FUN_005032d0 on the old parent only (FUN_00501d00);
//   - a type without body entries: its compound IS the click body, a kind-7
//     shape FUN_005008c0 leaves alone: r = the click shape's radius;
//   - a part with parts installed on it, and a car: the box of the whole
//     tree. Fork: measured from the mass centre of the tree now (the exe's
//     origin is the mass centre before the last attach); the car from the
//     chassis compound's (car_compound_com, NativeTypes_part7.inc).
// Only a part without a compound falls through to query 0x27 (the free
// slots): a type with neither body entries nor a click body (asm
// 0x4c219b: type+0xFF0 >= 0 always makes the compound at 0x4c21d4; else
// 0x4c262b makes one only from the click body type, type+0x1040).
// Fork: a type with body entries none of which gives the host points (a
// typeof-9 record on a `body` line, part_body_shape: not ported, none
// shipped; a missing or invalid mesh: "Fatal!" in the exe) keeps the box
// FUN_005008c0 starts at the origin: r = 0, the answer 1.
bool part_gii_size(PartObject* p, NativeInfo* out) {
  const PartTypeDesc& d = *p->type_;
  float r = -1.f;
  const char* how = "no points: getinfo 0x27";
  CamV3 log_lo, log_hi, log_o;
  if (d.bone_index < 0) {
    if (d.click_shape) {
      r = d.click_shape->radius;
      how = "no body: the click shape's radius";
    }
  } else {
    CamV3 lo, hi, o;
    bool any = false;
    if (p->type_index == kNativeCar) {
      const CarObject* car = static_cast<const CarObject*>(p);
      any = part_tree_box(*car, &lo, &hi, 0, &car->slot_bones_);
      o = car_compound_com(*car);
    } else {
      any = part_tree_box(*p, &lo, &hi, 0, nullptr);
      bool tree = false;
      for (const PartObject::Slot& s : p->slots_)
        if (s.other && (s.flags & 2u)) tree = true;
      if (tree)
        o = d.ref_at + cam_mul(part_transpose(part_cfg_rot(*p)),
                               p->compound_pos() - p->pos_);
    }
    if (!any) {
      r = 0.f;
      how = "Fork: body entries without points: the box at the origin";
    } else {
      // From the origin, the box starting there (FUN_005008c0 starts its
      // six bounds at 0).
      const CamV3 l = lo - o, h = hi - o;
      const float lx = (std::min)(l.x, 0.f), ly = (std::min)(l.y, 0.f);
      const float lz = (std::min)(l.z, 0.f);
      const float hx = (std::max)(h.x, 0.f), hy = (std::max)(h.y, 0.f);
      const float hz = (std::max)(h.z, 0.f);
      float m2 = hy * hy + hx * hx + hz * hz;
      const float n2 = ly * ly + lx * lx + lz * lz;
      if (m2 <= n2) m2 = n2;
      r = std::sqrt(m2);
      how = "the box from the origin";
      log_lo = lo;
      log_hi = hi;
      log_o = o;
    }
  }
  if (native_type_log()) {
    static int s_logged = 0;
    if (s_logged < 32) {
      ++s_logged;
      std::fprintf(stderr,
                   "[native-type] getinfo 0xB '%s' (node 0x%08X): r=%.4f -> %d "
                   "(%s; box (%.4f, %.4f, %.4f) .. (%.4f, %.4f, %.4f), origin "
                   "(%.4f, %.4f, %.4f))\n",
                   car_type_alias(d.type_id),
                   static_cast<unsigned>(part_node_id(p->key_)), r,
                   r < 0.f ? static_cast<int32_t>(p->slots_.size()) - p->installed_
                           : 1 - static_cast<int32_t>(r * -100.f),
                   how, log_lo.x, log_lo.y, log_lo.z, log_hi.x, log_hi.y,
                   log_hi.z, log_o.x, log_o.y, log_o.z);
    }
  }
  if (r < 0.f) {
    out->i = static_cast<int32_t>(p->slots_.size()) - p->installed_;
    return true;
  }
  constexpr float kNegCm = -100.f;  // DAT_0059de04
  out->i = 1 - static_cast<int32_t>(r * kNegCm);
  out->v[0] = r;  // the optional float out pointer
  return true;
}

}  // namespace
