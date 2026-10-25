// Fragment 4 of NativeTypes.cpp - included, not compiled on its own.
//
// The `part` type: row 4 of the native type table, `_type_part` 0x59DC54,
// factory FUN_004ca1f0 (it only stores the cfg buffer; the type's init
// FUN_004c36d0 parses it on the first use). An instance is
//   - a rigid body `part_comp`: a compound of one child body per `body`
//     line of the cfg (FUN_004c1f90), asleep after its creation;
//   - one render instance `part_r_o` per `render` / `light` line, a child
//     of the part's node whose bone00 follows the part's body
//     (FUN_004c2b10);
//   - the slots other parts are installed on (NativeTypes_part5.inc).
// Mirrors: FUN_004f6240 (cfg lines), FUN_004bf8e0 render, FUN_004bf4d0
// body, FUN_004bf230 click, FUN_004bfc10 slot, FUN_004bfe90 attach,
// FUN_004c0740 flags, FUN_004c07f0 lod, FUN_004c0930 colors, FUN_004bfb30
// bone, FUN_004c0880 lods, FUN_004c0fc0 linked, FUN_004c2950 -> FUN_004c1f90
// create, FUN_004c2b10 render instances, FUN_004c6370 setMatrix,
// FUN_004c3da0 getinfo, FUN_004c3a50 destroy.
//
// Level of detail. Every `render` entry carries a range `lod first last`
// and the type a list `lods n t0 t1 ..`; an instance of the type has the
// render instances of the entries whose range holds its level, and the
// level follows the detail the cameras give the node (the rule and the
// camera side are in NativeTypes_part8.inc).
//
// Where the mesh hangs (M2 spec Q9, read in Ghidra for step 4). The
// position of a part is the position of its body, and a body's origin is
// its centre of mass:
//   - a shape made from a mesh file is recentred on the centroid of its
//     closed triangle surface (FUN_004f7d60 kind 6 -> FUN_004f5b90 reads the
//     file, FUN_00505760 -> FUN_00508d20 computes the volume integrals,
//     subtracts the centroid from every vertex and keeps it at shape+0x40);
//   - FUN_004c1f90 puts the child body of a `body` line at
//     create position + R * (line offset + line rotation * shape+0x40);
//   - FUN_005032d0 then moves the compound's own origin to the mass centre
//     C of its children (when |C|^2 > 1e-5);
//   - FUN_004c2b10 links bone00 of a render instance to the part's body
//     with the offset (render offset - bodyRot^T * (body offset +
//     shape+0x40)).
// So the mesh origin of a part with one body and no rotations sits at
// body position - C + render offset. The create string places the mesh
// origin (the body lands at position + C); GameRef.setMatrix places the
// BODY (FUN_004c6370 -> FUN_004eac50), which leaves the mesh at position -
// C. The garage lift: C = (-0.119, -0.768, -0.001) m, setMatrix (11, 0.8,
// 1) -> mesh at (11.119, 1.568, 1.001), the offset the stock frame shows.
// Units: metres and radians (cfg offsets and the create string are script
// units; a shape's mesh file is centimetres, x 0.01 at load, DAT_005935d0).
//
// The `car` type (row 3, `_type_car` 0x59DCD8) is this type plus wheels:
// its init FUN_0049ab20 runs the same line parser with a keyword table
// (0x5DA528) that holds every part word, and its create FUN_004992e0 calls
// FUN_004c1f90. The cfg reader below therefore serves both; the words only
// a car has are in NativeTypes_part6.inc (car_cfg_word).
//
// The fragment also holds the payload of a light type (typeof 13, FUN_005219c0
// over the defaults of FUN_005213c0) and RenderRef.setLight on one
// (FUN_004ebef0), at its end.

namespace {

// ---------------------------------------------------------------------------
// The mesh of a physics shape and its centre
// ---------------------------------------------------------------------------
struct PartShapeMesh {
  std::vector<float> xyz;      // three per vertex, metres
  std::vector<uint32_t> tri;   // three per triangle
};

// (m6 step 1) A click shape as a type keeps it (FUN_005205e0 -> FUN_004f5b90):
// the mesh in metres as read (a click shape has no mass and is never
// recentred: FUN_005205e0 calls neither FUN_00505760 nor FUN_00508d20), per
// triangle the normal FUN_004f5b90 stores at tri+0x14, normalize((v1 - v0) x
// (v2 - v1)) (left as it is when its length is 0), the radius shape+0xC =
// sqrt(max |v|^2) + 0.05 (DAT_00594798) from the shape's origin, the record's
// `skin` (shape+0x10) and `type` (shape+8, the shape kind, default 7).
struct ClickShape {
  std::vector<float> xyz;      // three per vertex
  std::vector<uint32_t> tri;   // three per triangle
  std::vector<float> nrm;      // three per triangle
  float radius = 0.f;
  float skin = 0.f;
  int kind = 7;
};

bool part_read_file(const std::string& rel, std::vector<uint8_t>* out) {
  out->clear();
  if (rel.empty()) return false;
  const std::string resolved = rpak_resolve_path(rel.c_str());
  FILE* f = std::fopen(resolved.empty() ? rel.c_str() : resolved.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  bool ok = n >= 0;
  if (ok && n > 0) {
    out->resize(static_cast<size_t>(n));
    ok = std::fread(out->data(), 1, out->size(), f) == out->size();
  }
  std::fclose(f);
  return ok;
}

inline float part_rdf(const uint8_t* p) {
  float v;
  std::memcpy(&v, p, 4);
  return v;
}

// PE 940 FUN_004f5b90: every block of an INVO file as one vertex and one
// triangle list. Versions 1..3 are blocks {header, u32 vertex count,
// vertices, u32 triangle count, triangles of three u32}: version 1 has a
// 56-byte header and 52-byte vertices, version 2 a 52-byte header and
// 52-byte vertices, version 3 a header that starts with its own size and
// carries the vertex stride at +0x3C (a 0x38 header means stride 44); a
// block whose first dword is 0 ends the file. Version 4 is a directory of
// {u32, u32} pairs and then chunks {type, size, ...}: type 4 = vertices
// {count, format, data} (the position is read when format bit 0 is set),
// type 5 = u16 triangle indices relative to the vertex chunk before them.
// Positions are multiplied by 0.01 (DAT_005935d0).
// The exe steps from chunk to chunk by the chunk's size and reads nothing of
// a chunk that is neither type 4 nor 5 (a section chunk, type 1, is 12 bytes
// long).
// Fork: bounded by the file size (the exe trusts the file): a chunk needs
// its 8-byte header, a vertex chunk 16 bytes and an index chunk 12; the
// exe's run-time switch DAT_007a75e4 that reads versions 5 / 6 as 3 / 4 is
// not ported (no shipped shape file has those versions).
bool part_shape_mesh_read(const std::vector<uint8_t>& d, PartShapeMesh* out) {
  out->xyz.clear();
  out->tri.clear();
  if (d.size() < 12) return false;
  const uint8_t* const base = d.data();
  const size_t size = d.size();
  const uint32_t version = rd32(base + 4);
  constexpr float kScale = 0.01f;  // DAT_005935d0
  size_t p = 8;
  if (version >= 1 && version <= 3) {
    while (p + 4 <= size) {
      const uint32_t first = rd32(base + p);
      if (first == 0) break;
      size_t stride = 0;
      if (version == 1) {
        p += 56;
        stride = 52;
      } else if (version == 2) {
        p += 52;
        stride = 52;
      } else if (first == 0x38) {
        p += 56;
        stride = 44;
      } else {
        if (p + 0x40 > size) return false;
        stride = rd32(base + p + 0x3C);
        p += first;
      }
      if (stride < 12 || p + 4 > size) return false;
      const uint32_t nv = rd32(base + p);
      const uint64_t after = static_cast<uint64_t>(p) + 4 +
                             static_cast<uint64_t>(nv) * stride;
      if (after + 4 > size) return false;
      const uint32_t nt = rd32(base + static_cast<size_t>(after));
      if (after + 4 + static_cast<uint64_t>(nt) * 12 > size) return false;
      const uint32_t first_vertex = static_cast<uint32_t>(out->xyz.size() / 3);
      p += 4;
      for (uint32_t i = 0; i < nv; ++i) {
        out->xyz.push_back(part_rdf(base + p) * kScale);
        out->xyz.push_back(part_rdf(base + p + 4) * kScale);
        out->xyz.push_back(part_rdf(base + p + 8) * kScale);
        p += stride;
      }
      p += 4;
      for (uint32_t t = 0; t < nt * 3u; ++t) {
        out->tri.push_back(rd32(base + p) + first_vertex);
        p += 4;
      }
    }
  } else if (version == 4) {
    const uint32_t n = rd32(base + p);
    p += 4 + static_cast<size_t>(n) * 8;
    uint32_t first_vertex = 0;
    for (uint32_t i = 0; i < n; ++i) {
      if (p + 8 > size) return false;
      const uint32_t type = rd32(base + p);
      const uint32_t csize = rd32(base + p + 4);
      if (csize < 8 || static_cast<uint64_t>(p) + csize > size) return false;
      if (type == 4) {
        if (csize < 16) return false;
        const uint32_t count = rd32(base + p + 8);
        const uint32_t format = rd32(base + p + 12);
        if (count == 0) return false;  // the exe divides by the count
        const size_t stride = (csize - 16) / count;
        if (stride < 12 && (format & 1u)) return false;
        first_vertex = static_cast<uint32_t>(out->xyz.size() / 3);
        size_t q = p + 16;
        for (uint32_t k = 0; k < count; ++k) {
          // A chunk without positions leaves the three floats as they were
          // allocated in the exe; the host stores zeros.
          const bool has_pos = (format & 1u) != 0;
          out->xyz.push_back(has_pos ? part_rdf(base + q) * kScale : 0.f);
          out->xyz.push_back(has_pos ? part_rdf(base + q + 4) * kScale : 0.f);
          out->xyz.push_back(has_pos ? part_rdf(base + q + 8) * kScale : 0.f);
          q += stride;
        }
      } else if (type == 5) {
        if (csize < 12) return false;
        const uint32_t count = rd32(base + p + 8);
        if (12 + static_cast<uint64_t>(count / 3) * 6 > csize) return false;
        size_t q = p + 12;
        for (uint32_t k = 0; k < (count / 3) * 3u; ++k) {
          uint16_t v;
          std::memcpy(&v, base + q, 2);
          out->tri.push_back(static_cast<uint32_t>(v) + first_vertex);
          q += 2;
        }
      }
      p += csize;
    }
  } else {
    return false;
  }
  // "Fatal! Invalid mesh file" when either list is empty.
  if (out->xyz.empty() || out->tri.empty()) return false;
  const uint32_t nvert = static_cast<uint32_t>(out->xyz.size() / 3);
  for (uint32_t i : out->tri)
    if (i >= nvert) return false;  // Fork: the exe does not check
  return true;
}

// PE 940 FUN_00508d20 (FUN_00508bb0 + FUN_00508820): the volume integrals
// of the closed triangle surface, T0 = volume and T1 = first moments in
// doubles, centre = T1 / T0 stored as floats.
// Fork: the host sums signed tetrahedra against the origin, which is the
// same pair of integrals (the exe projects every face on a coordinate
// plane; the results agree to rounding). A surface without volume gives
// (0, 0, 0) here; the exe divides by zero.
CamV3 part_shape_centroid(const PartShapeMesh& m, double* volume) {
  double v6 = 0.0, mx = 0.0, my = 0.0, mz = 0.0;
  for (size_t t = 0; t + 2 < m.tri.size(); t += 3) {
    const float* a = &m.xyz[static_cast<size_t>(m.tri[t]) * 3];
    const float* b = &m.xyz[static_cast<size_t>(m.tri[t + 1]) * 3];
    const float* c = &m.xyz[static_cast<size_t>(m.tri[t + 2]) * 3];
    const double cx = static_cast<double>(b[1]) * c[2] - static_cast<double>(b[2]) * c[1];
    const double cy = static_cast<double>(b[2]) * c[0] - static_cast<double>(b[0]) * c[2];
    const double cz = static_cast<double>(b[0]) * c[1] - static_cast<double>(b[1]) * c[0];
    const double det = a[0] * cx + a[1] * cy + a[2] * cz;
    v6 += det;
    mx += det * (static_cast<double>(a[0]) + b[0] + c[0]);
    my += det * (static_cast<double>(a[1]) + b[1] + c[1]);
    mz += det * (static_cast<double>(a[2]) + b[2] + c[2]);
  }
  if (volume) *volume = v6 / 6.0;
  if (v6 == 0.0) return CamV3();
  return cam_v(static_cast<float>(mx / (4.0 * v6)),
               static_cast<float>(my / (4.0 * v6)),
               static_cast<float>(mz / (4.0 * v6)));
}

// The value FUN_004f5b90 leaves at shape+0x40: the centre of the bounding
// box of the vertices. The loop of the exe starts from vertex 0 and looks
// at count - 1 vertices, so the last vertex is never seen.
CamV3 part_shape_box_centre(const PartShapeMesh& m) {
  const size_t n = m.xyz.size() / 3;
  if (n == 0) return CamV3();
  float lo[3] = {m.xyz[0], m.xyz[1], m.xyz[2]};
  float hi[3] = {m.xyz[0], m.xyz[1], m.xyz[2]};
  for (size_t i = 0; i + 1 < n; ++i)
    for (int k = 0; k < 3; ++k) {
      const float v = m.xyz[i * 3 + static_cast<size_t>(k)];
      if (v <= lo[k]) lo[k] = v;
      if (hi[k] <= v) hi[k] = v;
    }
  return cam_v((hi[0] - lo[0]) * 0.5f + lo[0], (hi[1] - lo[1]) * 0.5f + lo[1],
               (hi[2] - lo[2]) * 0.5f + lo[2]);
}

inline CamM3 part_transpose(const CamM3& a) {  // FUN_00567380
  CamM3 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
  return r;
}
inline bool part_is_identity(const CamM3& a) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      if (a.m[i][j] != ((i == j) ? 1.f : 0.f)) return false;
  return true;
}

// ---------------------------------------------------------------------------
// The type: what the cfg gives (type+0x128 .. +0x1198)
// ---------------------------------------------------------------------------
// One 0x74-byte entry of the array at type+0x128 (at most 32).
struct PartEntry {
  int kind = 0;                // +0x3C: 1 body, 3 render / light
  CamV3 off;                   // +0x00
  float ypr[3] = {0.f, 0.f, 0.f};
  CamM3 rot = cam_identity();  // +0x0C, FUN_005666d0 of the three angles
  // +0x58: `flags` | 0x1ff, `lod` in bits 16..19 (first) and 20..23 (last),
  // 0x100 nocollision, 0x200 noclick, 0x400 bone, 0x4000 `linked`. In a car
  // cfg (FUN_0048d560): 0x1000 a wheel render with the wheel index in bits
  // 24..31, 0x8000 a render of the `steering` body with that entry's index
  // in bits 24..31.
  uint32_t flags = 0;
  // +0x6C / +0x70: one record per `linked a [b [c]]` line behind the entry
  // (FUN_004c0fc0, "%d %d %d"): the slots the render instance hangs on
  // instead of the part's own body (NativeTypes_part8.inc).
  struct Linked {
    int32_t a = 0, b = 0, c = 0;
  };
  std::vector<Linked> linked;
  // kind 3
  int32_t res_id = 0;          // +0x44: a render object or a light type
  bool is_light = false;       // the record is typeof 13
  std::vector<int32_t> colors; // +0x5C / +0x60
  // kind 1
  float mass = 0.f;            // shape+0x2C is 1 / mass, 0 for mass 0
  int shape_kind = 0;          // shape+8: 1 ball, 5 box, 2 3 4 rods, 6 mesh
  std::string shape_path;
  float dims[3] = {0.f, 0.f, 0.f};  // the numbers behind the shape word
  // Fork: (M3 spec Q4) how far the shape reaches from its centre: the
  // radius of a ball, half the diagonal of a box or rod, the farthest
  // vertex of a mesh. The bounds of the node in the detail rule.
  float reach = 0.f;
  CamV3 com;                   // shape+0x40
  double volume = 0.0;         // shape+0x4C (mesh shapes with a mass)
  // (m5 step 5) The points of the shape in the frame of its body, as
  // FUN_005008c0 reads them for the box of the compound the body is a child
  // of: a ball its centre, a rod its two ends (+-shape+0xB4 along its
  // axis), a box its eight corners (+-shape+0xB4..+0xBC), a mesh its
  // vertices (shape+0xB8; less shape+0x40 for a shape with a mass,
  // FUN_00508d20, raw without one: FUN_00505760 returns at once). Each point
  // is grown by `margin` = shape+0x10 (the ball's radius, the rod's) + 0.01
  // (DAT_005935d0, the child has no children). FUN_004f7d60, FUN_004f5b90.
  std::vector<CamV3> pts;
  float margin = 0.f;
};

// One node of the slot list at type+0x116C (0x170 bytes, FUN_004bfc10).
struct PartSlot {
  // Position (+0x24) and rotation (+0x30, FUN_005666d0 of the three angles)
  // as written in the cfg, until part_type_rebase_slots puts both on the
  // reference body as the type's init FUN_004c3780 does.
  CamV3 pos;
  float ypr[3] = {0.f, 0.f, 0.f};
  CamM3 rot = cam_identity();
  int32_t id = 0;  // +0x60
  // +0x64: the number of a `slottype` or `slotflags` line behind the slot
  // (FUN_004c0190, one handler, "%d" into the whole dword). The low word
  // names the body of a car the slot hangs on (FUN_004992e0): 0 the
  // reference body, 11 `steer`, 101.. `wheelbone`, 201.. `hub`, 301..
  // `a_arm` of wheel n.
  int32_t type_word = 0;
  int32_t slottype() const { return type_word & 0xffff; }
  // +0x6C: one per `bolt` line behind the slot (FUN_004bfd00). An install
  // whose two slots have no bolts is finished at once (FUN_004cb1d0).
  int bolts = 0;
  struct Attach {
    int32_t type_id = 0;  // resolved through the cfg's pack (FUN_0051e860)
    int32_t slot = 0;
  };
  std::vector<Attach> attach;      // +0xC4: `attach <type> <slot>`
  std::vector<Attach> compatible;  // +0x110: `compatible <type> <slot>`
  // +0x154: a `flap` line behind the slot (m6 step 6): the hinge's pivot
  // and axis (rebased with the slot), a second vector and the two limits.
  bool has_flap = false;
  CamV3 flap_pivot, flap_axis, flap_dir;
  float flap_lo = 0.f, flap_hi = 0.f;
};

// One 200-byte wheel record of a car type (type+0x1FB8 + n * 200; count
// type+0x1FB4, at most 16), as the handlers FUN_0048fb30 `type`,
// FUN_0048fcb0 `spring`, FUN_0048fee0 `wheel`, FUN_0048cfc0 `wheelbones`,
// FUN_0048d0e0 `bonepos`, FUN_0048cc80 `arm`, FUN_0048cd70 `hub` fill it.
struct CarWheelDesc {
  CamV3 pos;                             // +0x00: the centre at full droop
  float ypr[3] = {0.f, 0.f, 0.f};        // +0x0C
  float steer_factor = 0.f;              // +0x18: 1 front, 0 rear
  // +0x38 / +0x3C / +0x40. Fork: a wheel without a `spring` line has zeros
  // here in the exe (no shipped cfg: all 96 car cfgs have one line per
  // wheel); the host starts from the values the handler puts in place of a
  // non-positive number.
  float spring[3] = {0.1f, 0.2f, 0.05f};
  float radius = 0.f;                    // +0x44: first number of the shape
  int shape_kind = 0;                    // +0x48: 1 sphere, 5 box, 6 mesh
  float shape_mass = 0.f;
  bool bones = false;                    // +0x50: `wheelbones` (hub, a_arm)
  CamV3 bonepos;                         // +0x58, `wheel` presets (0, 0.4, 0)
  float arm_len = -1.f;                  // +0x68, -1 = none
  CamV3 arm_axis;                        // +0x6C
  CamV3 arm_pivot;                       // +0x78: wheel position + p
  float hub_len = -1.f;                  // +0x88, -1 = none
  CamV3 hub_p1;                          // +0x8C
  CamV3 hub_p2;                          // +0x98
  CamV3 hub_pc;                          // +0xA4: wheel position + pc
};

// What `_type_car` adds to the part type (FUN_0049ab20 and its keyword
// table 0x5DA528), as far as a standing car needs it.
struct CarTypePart {
  std::vector<CarWheelDesc> wheels;      // type+0x1FB8
  // type+0xFFC, the "current body" while the cfg is read: the index of the
  // last `body` / `steering` entry, 0x10000000 + wheel index behind a
  // `wheel` line; 0xFFFFFFFF before the first (FUN_004c3940).
  uint32_t cur_body = 0xFFFFFFFFu;
  int steer_index = -1;                  // type+0xFF4: the `steering` entry
  float steer_ypr[3] = {0.f, 0.f, 0.f};  // type+0x1208: Ypr of its rotation
  float max_steer = 0.7f;                // header+0xA20 (`maxsteer`, 1st)
  float ackermann = 2.f;                 // header+0xA30 (`maxsteer`, 2nd)
  CamV3 center;                          // type+0x1F1C (`center`)
  // type+0x1F34: the `mass` word ("%f", FUN_0048d6b0); -1 from the init,
  // and then the first create puts the mass of the chassis compound there.
  float mass_word = -1.f;
};

struct PartTypeDesc {
  int32_t type_id = 0;
  std::string cfg_path;
  bool cfg_found = false;
  std::vector<PartEntry> entries;  // type+0x128, count type+0xFA8
  int render_count = 0;            // type+0xFF8
  int bone_index = -1;             // type+0xFF0: the body the meshes follow
  bool has_click = false;          // type+0x1040
  CamV3 click_off;                 // type+0x1000
  CamM3 click_rot = cam_identity();
  // (m5 step 5) The first word of the `click` line and the cfg's pack.
  std::string click_word;
  int32_t click_pack = 0;
  // (m6 step 1) The click body's shape (the body type at type+0x1040,
  // FUN_004bf230), read at the type's init and shared by every type whose
  // `click` line names the same record or file. Null when it did not load
  // (the exe logs "Invalid clickdef" and the type gets no click body).
  std::shared_ptr<const ClickShape> click_shape;
  // (m5 step 5) The box FUN_005008c0 builds for the part's compound from
  // its own children, in the cfg frame: every body without body flag 0x20
  // (`nocollision`, the car's `steering` body) and the click body, each
  // point grown by its margin. has_box is false for a type without points.
  // Fork: the exe builds the box when the compound is made (FUN_005032d0);
  // the host builds it on the first query (part_type_box: car info and
  // GII_SIZE). Natives only, on the VM thread.
  mutable bool box_built = false;
  mutable bool has_box = false;
  mutable CamV3 box_lo, box_hi;
  std::vector<PartSlot> slots;     // type+0x116C
  std::vector<float> lods;         // the numbers of the `lods` line
  // type+0xFAC / +0xFB0 as the tail of the `lods` handler leaves them
  // (FUN_004c0880): when the last number is below 1.0 a 1.0 is stored
  // behind it and the count stays, else the count drops by one and the last
  // number is the upper bound. `lods 3 0.001 1 2` gives the thresholds
  // (0.001, 1), `lods 2 0.01 0.025` gives (0.01, 0.025): three levels
  // either way. lod_t holds lod_count thresholds and the bound; a type
  // without the line has level 0 only.
  int lod_count = 0;
  std::vector<float> lod_t;
  // node+0xEC, copied from the type payload at the create (FUN_00523c80):
  // the `lod_amp` line of the typeof-8 record, 1.0 without one. The detail
  // a camera gives the node is multiplied by it.
  float lod_amp = 1.f;
  // Fork: (M3 spec Q4) the bounds of the node in the detail rule: a sphere
  // of this radius around the reference body, reaching over every body
  // shape of the cfg; below 0 for a type without a body (unbounded).
  float bound_r = -1.f;
  // type+0x10E8 / +0x10F8: the `texture` and `mesh` lines (FUN_004c0460,
  // FUN_004c04c0: "%d", local to the cfg's pack): the texture a paint job
  // replaces and the mesh Part.setMesh replaces. 0 = no line.
  int32_t texture_id = 0;
  int32_t mesh_id = 0;
  // type+0x1108 / +0x1110: a handle on the render type of the first
  // `render` / `light` / `deformable` line whose id names a record
  // (FUN_004bf8e0 binds it while it is unbound): the type's main render
  // type, the one Part.setRenderType, setMesh and setTexture replace in an
  // instance. 0 = the cfg has no such line.
  int32_t main_render = 0;
  // type+0x115C: the `category` line (FUN_004c0520): 1 engine, 2 body, 3
  // running gear; what the `filter` command compares with.
  int32_t category = 0;
  // Set for a `native car` type before the cfg is read.
  std::unique_ptr<CarTypePart> car;
  // Derived: the mass centre of the bodies in the part's frame
  // (FUN_005032d0) and whether the compound is moved there.
  CamV3 com;
  bool com_shift = false;
  CamV3 com_raw;  // the mass centre whether the compound moves there or not
  // Derived: the reference body in the frame of the create pose (part+0x2C
  // `bbone`, FUN_004c1f90): the body of type+0xFF0 at (line offset + line
  // rotation * shape+0x40) with the rotation of its line; the click
  // instance of a type without bodies at the create position itself with
  // the click rotation. GameRef.setMatrix places this body, bone00 of the
  // render instances and the slots hang on it.
  CamV3 ref_at;
  CamM3 ref_rot = cam_identity();
  float total_mass = 0.f;
  int bodies = 0;
  int unported_shapes = 0;  // `body` lines whose shape is a physics record
  int missing_shapes = 0;   // `body` lines whose mesh file did not load
};

// The handlers get the line after its first word (the exe passes the line
// with a NUL behind the word and every handler steps over it).
struct PartCfgLine {
  const char* word;   // the keyword
  const char* rest;   // the text behind it; "" when the line ends there
  bool starts_line;   // nothing stands in front of the keyword
};

// PE 940 FUN_004f6240: a line is copied (256 bytes at most), cut at the
// first ';', '#' or '/', and its first word (delimiters " \r\n\t",
// 0x5A7220) selects the handler; the word `eof` ends the file.
template <typename Fn>
void part_cfg_lines(const std::string& text, Fn&& handle) {
  size_t pos = 0;
  const size_t n = text.size();
  while (pos < n) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = n;
    std::string line = text.substr(pos, eol - pos);
    pos = eol + 1;
    const size_t nul = line.find('\0');
    if (nul != std::string::npos) line.resize(nul);
    if (line.size() > 255) line.resize(255);  // FUN_00569340(.., 0x100)
    for (const char cut : {';', '#', '/'}) {
      const size_t at = line.find(cut);
      if (at != std::string::npos) line.resize(at);
    }
    auto is_delim = [](char c) {
      return c == ' ' || c == '\r' || c == '\n' || c == '\t';
    };
    size_t b = 0;
    while (b < line.size() && is_delim(line[b])) ++b;
    if (b == line.size()) continue;
    size_t e = b;
    while (e < line.size() && !is_delim(line[e])) ++e;
    const std::string word = line.substr(b, e - b);
    if (word_is(word.c_str(), "eof")) return;
    const std::string rest = (e < line.size()) ? line.substr(e + 1) : std::string();
    PartCfgLine l;
    l.word = word.c_str();
    l.rest = rest.c_str();
    l.starts_line = (b == 0);
    handle(l);
  }
}

// The shape of a `body` line (FUN_004bf4d0 -> FUN_004f7d60) as far as the
// placement needs it: where its centre is.
void part_body_shape(const char* shape, float mass, PartEntry* e,
                     PartTypeDesc* d) {
  e->mass = mass;
  e->com = CamV3();
  int32_t rid = 0;
  if (engine_sscanf(shape, "%d", &rid) != 0) {
    // A physics body record (typeof 9) -> FUN_004f7b50(mass, record).
    // Fork: not ported; its centre is taken as (0, 0, 0). No cfg of the
    // install names one on a `body` line.
    e->shape_kind = 0;
    ++d->unported_shapes;
    return;
  }
  if (word_is(shape, "ball") || word_is(shape, "sphere")) {
    e->shape_kind = 1;
  } else if (word_is(shape, "box")) {
    e->shape_kind = 5;
  } else if (word_is(shape, "rodx")) {
    e->shape_kind = 2;
  } else if (word_is(shape, "rody")) {
    e->shape_kind = 3;
  } else if (word_is(shape, "rodz")) {
    e->shape_kind = 4;
  } else {
    e->shape_kind = 6;
    e->shape_path = shape;
  }
  // FUN_00505760: the centre of a ball, box or rod is (0, 0, 0).
  constexpr float kLeafMargin = 0.01f;  // DAT_005935d0 (FUN_005008c0)
  if (e->shape_kind != 6) {
    e->reach = e->shape_kind == 1
                   ? std::fabs(e->dims[0])
                   : 0.5f * std::sqrt(e->dims[0] * e->dims[0] +
                                      e->dims[1] * e->dims[1] +
                                      e->dims[2] * e->dims[2]);
    // FUN_004f7d60: ball shape+0x10 = shape+0xB4 = size 1; rod shape+0xB4 =
    // size 1 (the half length), shape+0x10 = size 2 (the radius); box
    // shape+0xB4..+0xBC = the three sizes, shape+0x10 = 0.
    const float a = e->dims[0], b = e->dims[1], c = e->dims[2];
    if (e->shape_kind == 1) {
      e->pts.push_back(CamV3());
      e->margin = a + kLeafMargin;
    } else if (e->shape_kind >= 2 && e->shape_kind <= 4) {
      const CamV3 axis = e->shape_kind == 2 ? cam_v(a, 0.f, 0.f)
                         : e->shape_kind == 3 ? cam_v(0.f, a, 0.f)
                                              : cam_v(0.f, 0.f, a);
      e->pts.push_back(axis);
      e->pts.push_back(CamV3() - axis);
      e->margin = b + kLeafMargin;
    } else if (e->shape_kind == 5) {
      for (int k = 0; k < 8; ++k)
        e->pts.push_back(cam_v(k & 1 ? a : -a, k & 2 ? b : -b, k & 4 ? c : -c));
      e->margin = kLeafMargin;
    }
    return;
  }
  std::vector<uint8_t> file;
  PartShapeMesh mesh;
  if (!part_read_file(e->shape_path, &file) ||
      !part_shape_mesh_read(file, &mesh)) {
    // "Fatal! Missing mesh file" / "Fatal! Invalid mesh file" in the exe.
    // Fork: the host keeps the body with its centre at (0, 0, 0).
    ++d->missing_shapes;
    return;
  }
  if (mass == 0.f) {
    // FUN_00505760 returns at once for a shape without mass: shape+0x40
    // stays what FUN_004f5b90 wrote, the centre of the vertex box.
    e->com = part_shape_box_centre(mesh);
  } else {
    e->com = part_shape_centroid(mesh, &e->volume);
  }
  float far2 = 0.f;
  for (size_t i = 0; i + 2 < mesh.xyz.size(); i += 3) {
    const float dx = mesh.xyz[i] - e->com.x, dy = mesh.xyz[i + 1] - e->com.y;
    const float dz = mesh.xyz[i + 2] - e->com.z;
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (far2 < d2) far2 = d2;
  }
  e->reach = std::sqrt(far2);
  // (m5 step 5) The vertices in the body's frame: recentred on shape+0x40
  // for a shape with a mass (FUN_00508d20), as read without one. A mesh
  // shape's shape+0x10 stays 0 (FUN_004ba8c0; FUN_004f5b90 does not write
  // it).
  const CamV3 shift = mass == 0.f ? CamV3() : e->com;
  e->pts.reserve(mesh.xyz.size() / 3);
  for (size_t i = 0; i + 2 < mesh.xyz.size(); i += 3)
    e->pts.push_back(cam_v(mesh.xyz[i], mesh.xyz[i + 1], mesh.xyz[i + 2]) - shift);
  e->margin = kLeafMargin;
}

// (m5 step 5, m6 step 1) The click body's shape (FUN_004bf230): the first
// word of the `click` line goes through "%d"; a number (not 0) names a
// physics body record of the cfg's pack (FUN_0051e860, FUN_0051fa40(id, 9,
// 0)), whose text FUN_005205e0 reads line by line: `shape <mesh>`, `type
// %d` (default 7), `skin %f` (shape+0x10), and friction, coll_coeff, mass
// and inertia, which a body of mass 0 does not use; then FUN_004f7b50(0,
// record). Anything else is a mesh file: FUN_004f7d60(0, 7, 1, 1, 1, path).
// The mesh is FUN_004f5b90's: x 0.01, the normals, and the radius over the
// vertices but the last (the loop of FUN_004f5b90), which FUN_005205e0
// then computes again over all of them.
// One load per record or file: the payload of a record is built once
// (FUN_0051fa40). Natives only, on the VM thread.
std::unordered_map<std::string, std::shared_ptr<const ClickShape>> g_click_shapes;

std::shared_ptr<const ClickShape> part_click_shape(const char* word, int32_t pack) {
  std::string path;
  std::string key;
  auto shape = std::make_shared<ClickShape>();
  bool record = false;
  int32_t local = 0;
  if (engine_sscanf(word, "%d", &local) != 0 && local != 0) {
    const int32_t id = static_cast<int32_t>(
        rpak_remap_local_id(pack, static_cast<uint32_t>(local)));
    char k[24];
    std::snprintf(k, sizeof(k), "record %08X", static_cast<unsigned>(id));
    key = k;
    {
      std::lock_guard<std::mutex> lock(g_nt_mu);
      auto it = g_click_shapes.find(key);
      if (it != g_click_shapes.end()) return it->second;
    }
    record = true;
    std::vector<uint8_t> blob;
    if (id && rpak_read_entry(id, &blob)) {
      std::string text(reinterpret_cast<const char*>(blob.data()), blob.size());
      size_t at = 0;
      while (at < text.size()) {
        size_t eol = text.find('\n', at);
        if (eol == std::string::npos) eol = text.size();
        const std::string ln = text.substr(at, eol - at);
        at = eol + 1;
        char w[64] = {0};
        if (engine_sscanf(ln.c_str(), "%63s", w) != 1) continue;
        if (word_is(w, "shape")) {
          char p[1025] = {0};
          engine_sscanf(ln.c_str(), "%*s %1024s", p);
          path = p;
        } else if (word_is(w, "type")) {
          engine_sscanf(ln.c_str(), "%*s %d", &shape->kind);
        } else if (word_is(w, "skin")) {
          engine_sscanf(ln.c_str(), "%*s %f", &shape->skin);
        }
      }
    }
  } else {
    path = word;
    key = "file " + path;
    std::lock_guard<std::mutex> lock(g_nt_mu);
    auto it = g_click_shapes.find(key);
    if (it != g_click_shapes.end()) return it->second;
  }
  std::vector<uint8_t> file;
  PartShapeMesh mesh;
  std::shared_ptr<const ClickShape> done;
  if (!path.empty() && part_read_file(path, &file) &&
      part_shape_mesh_read(file, &mesh)) {
    shape->xyz = std::move(mesh.xyz);
    shape->tri = std::move(mesh.tri);
    const std::vector<float>& v = shape->xyz;
    shape->nrm.resize(shape->tri.size());
    for (size_t t = 0; t + 2 < shape->tri.size(); t += 3) {
      const float* v0 = &v[static_cast<size_t>(shape->tri[t]) * 3];
      const float* v1 = &v[static_cast<size_t>(shape->tri[t + 1]) * 3];
      const float* v2 = &v[static_cast<size_t>(shape->tri[t + 2]) * 3];
      const float e1x = v1[0] - v0[0], e1y = v1[1] - v0[1], e1z = v1[2] - v0[2];
      const float e2x = v2[0] - v1[0], e2y = v2[1] - v1[1], e2z = v2[2] - v1[2];
      float nx = e2z * e1y - e2y * e1z;
      float ny = e2x * e1z - e2z * e1x;
      float nz = e2y * e1x - e2x * e1y;
      const float len = std::sqrt(ny * ny + nx * nx + nz * nz);
      if (len != 0.f) {
        const float inv = 1.f / len;
        nx = inv * nx;
        ny = ny * inv;
        nz = nz * inv;
      }
      shape->nrm[t] = nx;
      shape->nrm[t + 1] = ny;
      shape->nrm[t + 2] = nz;
    }
    const size_t n = v.size() / 3;
    const size_t seen = record ? n : (n > 0 ? n - 1 : 0);
    float far2 = 0.f;
    for (size_t i = 0; i < seen; ++i) {
      float d2 = v[i * 3] * v[i * 3] + v[i * 3 + 1] * v[i * 3 + 1] +
                 v[i * 3 + 2] * v[i * 3 + 2];
      if (d2 <= far2) d2 = far2;
      far2 = d2;
    }
    constexpr float kRadiusPad = 0.05f;  // DAT_00594798
    shape->radius = std::sqrt(far2) + kRadiusPad;
    done = shape;
  }
  std::lock_guard<std::mutex> lock(g_nt_mu);
  auto& slot = g_click_shapes[key];
  if (!slot) slot = done;
  return slot;
}

// The words only the car's keyword table has (NativeTypes_part6.inc).
void car_cfg_word(const PartCfgLine& l, PartTypeDesc* d);

void parse_part_cfg(const std::string& text, int32_t pack, PartTypeDesc* d) {
  // FUN_00569b40 / FUN_00569b20 zero the locals the scans fill; a field the
  // line lacks keeps that zero.
  part_cfg_lines(text, [&](const PartCfgLine& l) {
    PartEntry* last = d->entries.empty() ? nullptr : &d->entries.back();
    if (word_is(l.word, "render") || word_is(l.word, "light") ||
        word_is(l.word, "deformable")) {
      // FUN_004bf8e0, "%d %f %f %f %f %f %f".
      if (d->entries.size() > 31 || d->render_count > 31) return;
      // A car's `render` and `light` are FUN_0048d560: this handler, then a
      // flag by the body the line stands behind. `deformable` keeps the
      // plain handler in both tables: the line IS the render entry of the
      // body shell.
      uint32_t car_flags = 0;
      if (d->car && !word_is(l.word, "deformable")) {
        const uint32_t cur = d->car->cur_body;
        const int32_t hi = static_cast<int32_t>(cur) >> 24;
        const uint32_t wheels = static_cast<uint32_t>(d->car->wheels.size());
        if (hi == 0) {
          if ((cur & 0xffu) == static_cast<uint32_t>(d->car->steer_index))
            car_flags = (static_cast<uint32_t>(d->car->steer_index) << 24) |
                        0x8000u;
        } else if (hi == 0x10 && (cur & 0xffu) == wheels - 1u) {
          car_flags = ((wheels - 1u) << 24) | 0x1000u;
        }
      }
      PartEntry e;
      e.kind = 3;
      int32_t local = 0;
      engine_sscanf(l.rest, "%d %f %f %f %f %f %f", &local, &e.off.x, &e.off.y,
                    &e.off.z, &e.ypr[0], &e.ypr[1], &e.ypr[2]);
      e.rot = cam_ypr_to_matrix(e.ypr[0], e.ypr[1], e.ypr[2]);
      // FUN_0051e860 (the id is local to the cfg's pack), then a typeof-14
      // record; a typeof-13 light when that fails or when the line is
      // `light` (the exe compares the whole line start with "light", so a
      // keyword behind white space is a `render`).
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
      // The tail of FUN_004bf8e0: `if (type+0x1110 == 0)` the handle
      // type+0x1108 is bound to the node of this entry's handle.
      if (!d->main_render && e.res_id) d->main_render = e.res_id;
      e.flags |= car_flags;
      d->entries.push_back(e);
      ++d->render_count;
    } else if (word_is(l.word, "body") ||
               (d->car && word_is(l.word, "steering"))) {
      // FUN_004bf4d0, "%f %f %f %f %f %f %f %s %f %f %f": offset, angles,
      // mass, shape, three sizes. A car's `steering` is the same handler
      // (FUN_0048d5f0) and then names the entry as the steering body.
      if (d->entries.size() > 31) return;
      if (d->bone_index < 0) d->bone_index = static_cast<int>(d->entries.size());
      PartEntry e;
      e.kind = 1;
      float mass = 0.f;
      float dims[3] = {0.f, 0.f, 0.f};
      char shape[1025];
      shape[0] = 0;
      engine_sscanf(l.rest, "%f %f %f %f %f %f %f %1024s %f %f %f", &e.off.x,
                    &e.off.y, &e.off.z, &e.ypr[0], &e.ypr[1], &e.ypr[2], &mass,
                    shape, &dims[0], &dims[1], &dims[2]);
      e.rot = cam_ypr_to_matrix(e.ypr[0], e.ypr[1], e.ypr[2]);
      e.dims[0] = dims[0];
      e.dims[1] = dims[1];
      e.dims[2] = dims[2];
      part_body_shape(shape, mass, &e, d);
      d->entries.push_back(e);
      if (d->car) {
        // FUN_004bf4d0 tail: type+0xFFC = the entry. FUN_0048d5f0: type+0xFF4
        // = the entry, type+0x1208 = the Ypr of its rotation (FUN_00569970).
        // Fork: the exe drops an entry whose shape could not be made; the
        // host keeps every body entry (part_body_shape).
        const int index = static_cast<int>(d->entries.size()) - 1;
        d->car->cur_body = static_cast<uint32_t>(index);
        if (word_is(l.word, "steering")) {
          d->car->steer_index = index;
          cam_matrix_to_ypr(e.rot, &d->car->steer_ypr[0],
                            &d->car->steer_ypr[1], &d->car->steer_ypr[2]);
        }
      }
    } else if (word_is(l.word, "click")) {
      // FUN_004bf230, "%s %f %f %f %f %f %f": a physics body record or a
      // mesh file (shape kind 7, no mass), its offset and angles.
      // Fork: the host only notes that the line is there.
      char shape[1025];
      shape[0] = 0;
      float ypr[3] = {0.f, 0.f, 0.f};
      engine_sscanf(l.rest, "%1024s %f %f %f %f %f %f", shape, &d->click_off.x,
                    &d->click_off.y, &d->click_off.z, &ypr[0], &ypr[1], &ypr[2]);
      d->click_rot = cam_ypr_to_matrix(ypr[0], ypr[1], ypr[2]);
      d->has_click = shape[0] != 0;
      // (m5 step 5) the shape, for the box of the compound (part_type_box).
      d->click_word = shape;
      d->click_pack = pack;
    } else if (word_is(l.word, "bone")) {
      // FUN_004bfb30: the entry before the line is the body bone00 follows.
      if (last) {
        last->flags |= 0x400u;
        d->bone_index = static_cast<int>(d->entries.size()) - 1;
      }
    } else if (word_is(l.word, "noclick")) {
      if (last) last->flags |= 0x200u;  // FUN_004bfb70
    } else if (word_is(l.word, "nocollision")) {
      if (last) last->flags |= 0x100u;  // FUN_004bfba0 (and shape flag 8)
    } else if (word_is(l.word, "flags")) {
      // FUN_004c0740, "%d": light and state bits of the entry before it.
      if (last) {
        int32_t v = 0;
        engine_sscanf(l.rest, "%d", &v);
        last->flags |= static_cast<uint32_t>(v) & 0x1ffu;
      }
    } else if (word_is(l.word, "lod")) {
      // FUN_004c07f0, "%d %d": first and last level of detail.
      if (last) {
        int32_t a = 0, b = 0;
        engine_sscanf(l.rest, "%d %d", &a, &b);
        last->flags = (last->flags & ~0xf0000u) |
                      ((static_cast<uint32_t>(a) << 16) & 0xf0000u);
        last->flags = (last->flags & ~0xf00000u) |
                      ((static_cast<uint32_t>(b) << 20) & 0xf00000u);
      }
    } else if (word_is(l.word, "lods")) {
      // FUN_004c0880, "%#%d" then that many "%#%f" distances.
      int32_t count = 0;
      const char* p = l.rest;
      p += engine_sscanf(p, "%#%d", &count);
      d->lods.clear();
      for (int32_t i = 0; i < count && i < 16; ++i) {
        float v = 0.f;
        const int used = engine_sscanf(p, "%#%f", &v);
        if (used <= 0) break;
        p += used;
        d->lods.push_back(v);
      }
      // The tail of the handler (asm 0x4c08f6..0x4c092a).
      d->lod_t = d->lods;
      d->lod_count = static_cast<int>(d->lods.size());
      if (d->lod_count > 0) {
        if (d->lods.back() < 1.f)
          d->lod_t.push_back(1.f);
        else
          --d->lod_count;
      }
    } else if (word_is(l.word, "linked")) {
      // FUN_004c0fc0, "%d %d %d": one more record of the entry before the
      // line, and entry flag 0x4000.
      if (last) {
        PartEntry::Linked k;
        engine_sscanf(l.rest, "%d %d %d", &k.a, &k.b, &k.c);
        last->linked.push_back(k);
        last->flags |= 0x4000u;
      }
    } else if (word_is(l.word, "colors")) {
      // FUN_004c0930, "%#%d" then that many "%#%d" colours for the entry
      // before the line.
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
    } else if (word_is(l.word, "slot")) {
      // FUN_004bfc10, "%f %f %f %f %f %f %d".
      PartSlot s;
      engine_sscanf(l.rest, "%f %f %f %f %f %f %d", &s.pos.x, &s.pos.y,
                    &s.pos.z, &s.ypr[0], &s.ypr[1], &s.ypr[2], &s.id);
      s.rot = cam_ypr_to_matrix(s.ypr[0], s.ypr[1], s.ypr[2]);
      d->slots.push_back(s);
    } else if (word_is(l.word, "flap")) {
      // (m6 step 6) srec+0x154: the flap definition of the slot before the
      // line, "%f %f %f %f %f %f %f %f %f %f %f" (84 car cfgs).
      if (!d->slots.empty()) {
        PartSlot& s = d->slots.back();
        const int n = engine_sscanf(
            l.rest, "%f %f %f %f %f %f %f %f %f %f %f", &s.flap_pivot.x, &s.flap_pivot.y,
            &s.flap_pivot.z, &s.flap_axis.x, &s.flap_axis.y, &s.flap_axis.z, &s.flap_dir.x,
            &s.flap_dir.y, &s.flap_dir.z, &s.flap_lo, &s.flap_hi);
        s.has_flap = n == 11;
      }
    } else if (word_is(l.word, "bolt")) {
      // FUN_004bfd00, "%f %f %f %f %f %f %d": a bolt of the slot before
      // the line (its place, and the render object of the bolt). The host
      // keeps the count, which decides whether an install is finished at
      // once.
      if (!d->slots.empty()) ++d->slots.back().bolts;
    } else if (word_is(l.word, "compatible")) {
      // FUN_004bffb0, "%d %d": like `attach`, on the slot's second list.
      if (!d->slots.empty()) {
        int32_t local = 0;
        PartSlot::Attach a;
        engine_sscanf(l.rest, "%d %d", &local, &a.slot);
        a.type_id = local ? static_cast<int32_t>(rpak_remap_local_id(
                                pack, static_cast<uint32_t>(local)))
                          : 0;
        d->slots.back().compatible.push_back(a);
      }
    } else if (word_is(l.word, "attach")) {
      // FUN_004bfe90, "%d %d": belongs to the slot before the line.
      if (!d->slots.empty()) {
        int32_t local = 0;
        PartSlot::Attach a;
        engine_sscanf(l.rest, "%d %d", &local, &a.slot);
        a.type_id = local ? static_cast<int32_t>(rpak_remap_local_id(
                                pack, static_cast<uint32_t>(local)))
                          : 0;
        d->slots.back().attach.push_back(a);
      }
    } else if (word_is(l.word, "slottype") || word_is(l.word, "slotflags")) {
      // FUN_004c0190, "%d": both words write slot+0x64 of the slot before
      // the line.
      if (!d->slots.empty()) {
        int32_t v = 0;
        engine_sscanf(l.rest, "%d", &v);
        d->slots.back().type_word = v;
      }
    } else if (word_is(l.word, "texture") || word_is(l.word, "mesh")) {
      // FUN_004c0460 / FUN_004c04c0, "%d": a handle on the texture (typeof
      // 7) / mesh (typeof 5) record with that id of the cfg's pack.
      int32_t local = 0;
      engine_sscanf(l.rest, "%d", &local);
      const int32_t id = local ? static_cast<int32_t>(rpak_remap_local_id(
                                     pack, static_cast<uint32_t>(local)))
                               : 0;
      if (word_is(l.word, "texture"))
        d->texture_id = id;
      else
        d->mesh_id = id;
    } else if (word_is(l.word, "category")) {
      // FUN_004c0520: one number into type+0x115C.
      engine_sscanf(l.rest, "%d", &d->category);
    } else if (d->car) {
      car_cfg_word(l, d);
    }
    // The other keywords of the table at 0x5DAB88 are read past: buoy,
    // shadow, trigger, collision_ps,
    // slotdmgmode, slotdeform, slotmove, slotturn,
    // deform, logo, damage, intdamage, material, wing,
    // rollbar, skinned, stockpart, sfx_loop.
  });
}

// PE 940 FUN_005032d0: the mass centre of the compound's children, each at
// (line offset + line rotation * shape+0x40) in the part's frame
// (FUN_004c1f90), weighted with 1 / (1 / mass). The compound moves there
// when |C|^2 > 1e-5 (DAT_005936f8).
void part_type_derive(PartTypeDesc* d) {
  constexpr float kTiny = 9.99999997e-07f;       // DAT_0059a450
  constexpr float kShiftLimit = 9.99999975e-06f;  // DAT_005936f8
  float total = 0.f;
  CamV3 c;
  d->bodies = 0;
  // The children hang in a list the newest leads (FUN_004f78d0).
  for (size_t i = d->entries.size(); i-- > 0;) {
    const PartEntry& e = d->entries[i];
    if (e.kind != 1) continue;
    ++d->bodies;
    if (e.mass == 0.f) continue;
    const float inv_mass = 1.f / e.mass;
    const float m = 1.f / inv_mass;
    const CamV3 at = e.off + cam_mul(e.rot, e.com);
    float sum = m + total;
    if (sum <= kTiny) sum = kTiny;
    const float k = 1.f / sum;
    c = cam_v((m * at.x + c.x * total) * k, (at.y * m + c.y * total) * k,
              (at.z * m + c.z * total) * k);
    total = m + total;
  }
  d->total_mass = total;
  d->com = c;
  d->com_raw = c;
  // The reference body (see PartTypeDesc::ref_at).
  d->ref_at = CamV3();
  d->ref_rot = cam_identity();
  if (d->bone_index >= 0 &&
      d->bone_index < static_cast<int>(d->entries.size()) &&
      d->entries[static_cast<size_t>(d->bone_index)].kind == 1) {
    const PartEntry& b = d->entries[static_cast<size_t>(d->bone_index)];
    d->ref_at = b.off + cam_mul(b.rot, b.com);
    d->ref_rot = b.rot;
  } else if (d->bone_index < 0 && d->has_click) {
    d->ref_rot = d->click_rot;
  }
  // Fork: a compound whose children all have mass 0 is moved by the exe to
  // the centre of the box around its children's shapes (FUN_005008c0, the
  // branch for a body without mass). Not ported: the host leaves it where
  // the create string puts it.
  d->com_shift = total > 0.f && cam_len2(c) > kShiftLimit;
  if (!d->com_shift) d->com = CamV3();
  // Fork: (M3 spec Q4) the exe keeps the bounds of a game node at node+0xB0
  // (kind) / +0xB4 (centre) / +0xC0 (radius or half extents). For a part
  // or a car they are a box (kind 1): FUN_00503c20 takes the box around
  // the bounds of the node's physics-instance children (node type 2:
  // `part_comp`, `bbone`, `click`, and of a car the wheel bodies and
  // `steer`; each is a sphere of its shape's radius at its body, written
  // by FUN_00524620 and the tail of FUN_005032d0, or the swept box of
  // FUN_00504180 while the body moves). It runs at the end of the node's
  // create (FUN_00523c80, FUN_005240e0) and whenever the frames of a body
  // tree are recomputed (FUN_005030b0), and the size the detail rule reads
  // is the length of the half extents. The sphere of radius 10 that the
  // car's create FUN_004992e0 writes on the car's node is replaced by that
  // box before GameRef.create returns.
  // Not ported: it needs the bound radius of each shape kind and of a
  // compound (FUN_005008c0), which were not read. The host takes a sphere
  // around the reference body that holds every body shape.
  d->bound_r = -1.f;
  for (const PartEntry& e : d->entries) {
    if (e.kind != 1) continue;
    const CamV3 at = e.off + cam_mul(e.rot, e.com) - d->ref_at;
    const float r = std::sqrt(cam_len2(at)) + e.reach;
    if (d->bound_r < r) d->bound_r = r;
  }
}

// (m5 step 5) The box of the compound's own children (FUN_005008c0, the
// branch for a compound of shape kind 5 / 6): every body but those with
// body flag 0x20 (the entry flag 0x100 of `nocollision`, FUN_004c1f90 at
// 0x4c2474; the car's `steering` body, 0x8020 at 0x4c2445), each at (line
// offset + line rotation * shape+0x40) with the line rotation, and the
// click body at the click line's offset and rotation (0x4c25a4). Points are
// grown by their margin. Bodies with mass 0 count too (only the mass
// centre leaves them out). Built on the first call (PartTypeDesc::box_built).
const PartTypeDesc& part_type_box(const PartTypeDesc& td) {
  if (td.box_built) return td;
  const PartTypeDesc* d = &td;
  d->box_built = true;
  d->has_box = false;
  auto grow = [d](const CamV3& c, float m) {
    const CamV3 lo = cam_v(c.x - m, c.y - m, c.z - m);
    const CamV3 hi = cam_v(c.x + m, c.y + m, c.z + m);
    if (!d->has_box) {
      d->box_lo = lo;
      d->box_hi = hi;
      d->has_box = true;
      return;
    }
    d->box_lo = cam_v((std::min)(d->box_lo.x, lo.x), (std::min)(d->box_lo.y, lo.y),
                      (std::min)(d->box_lo.z, lo.z));
    d->box_hi = cam_v((std::max)(d->box_hi.x, hi.x), (std::max)(d->box_hi.y, hi.y),
                      (std::max)(d->box_hi.z, hi.z));
  };
  for (size_t i = 0; i < d->entries.size(); ++i) {
    const PartEntry& e = d->entries[i];
    if (e.kind != 1 || e.pts.empty() || (e.flags & 0x100u)) continue;
    if (d->car && static_cast<int>(i) == d->car->steer_index) continue;
    const CamV3 at = e.off + cam_mul(e.rot, e.com);
    for (const CamV3& p : e.pts) grow(at + cam_mul(e.rot, p), e.margin);
  }
  if (d->click_shape) {
    // A leaf child: its margin is shape+0x10 plus 0.01 (DAT_005935d0).
    const ClickShape& s = *d->click_shape;
    const float margin = s.skin + 0.01f;
    for (size_t i = 0; i + 2 < s.xyz.size(); i += 3)
      grow(d->click_off + cam_mul(d->click_rot, cam_v(s.xyz[i], s.xyz[i + 1],
                                                      s.xyz[i + 2])),
           margin);
  }
  return td;
}

// PE 940 FUN_004c3780, the end of the type's init: every slot is put on
// the reference body (the body of type+0xFF0, or the `click` entry of a
// type without bodies): pos = bodyRot * (pos - (body offset + shape+0x40)),
// rot = bodyRot * rot (FUN_00566a70), with shape+0x40 only for a shape
// that has a mass. Nothing happens when the type has no entries or the
// reference has no shape. (asm 0x4c3860..0x4c3915; the function builds the
// transpose of the body rotation first and then multiplies with the
// rotation itself, which only shows for a rotated reference body.)
void part_type_rebase_slots(PartTypeDesc* d) {
  if (d->entries.empty() || d->slots.empty()) return;  // type+0xFA8 == 0
  CamV3 base;
  CamM3 rot = cam_identity();
  if (d->bone_index >= 0) {
    if (d->bone_index >= static_cast<int>(d->entries.size())) return;
    const PartEntry& b = d->entries[static_cast<size_t>(d->bone_index)];
    if (b.kind != 1) return;  // entry+0x40: no shape
    base = b.off;
    if (b.mass != 0.f) base = base + b.com;
    rot = b.rot;
  } else {
    if (!d->has_click) return;  // type+0x1040: no shape
    base = d->click_off;        // a click shape has no mass
    rot = d->click_rot;
  }
  for (PartSlot& s : d->slots) {
    s.pos = cam_mul(rot, s.pos - base);
    s.rot = cam_mm(rot, s.rot);
    if (s.has_flap) {
      s.flap_pivot = cam_mul(rot, s.flap_pivot - base);
      s.flap_axis = cam_mul(rot, s.flap_axis);
      s.flap_dir = cam_mul(rot, s.flap_dir);
    }
  }
}

std::unordered_map<int32_t, std::unique_ptr<PartTypeDesc>> g_part_types;

// The type's init (vt+4 FUN_004c36d0), once per type.
const PartTypeDesc* part_type_desc(const GameTypeDesc* type) {
  static const PartTypeDesc kNone;
  if (!type || !type->found) return &kNone;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    auto it = g_part_types.find(type->type_id);
    if (it != g_part_types.end()) return it->second.get();
  }
  auto d = std::make_unique<PartTypeDesc>();
  d->type_id = type->type_id;
  d->cfg_path = type->cfg_path;
  // `native car`: the init FUN_0049ab20 (part init, car defaults, the same
  // line parser with the car's keyword table, FUN_004c3780). It presets the
  // category to 2, body (type+0x115C = 2 right behind the part's field
  // reset FUN_004c3940, which leaves 0); a `category` line of the cfg
  // replaces it.
  if (type->native_index == kNativeCar) {
    d->car = std::make_unique<CarTypePart>();
    d->category = 2;
  }
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
    parse_part_cfg(text, rpak_id_pack(type->type_id), d.get());
  // FUN_004bf230 makes the click line's body type while the cfg is read
  // (m6 step 1).
  if (d->has_click && !d->click_word.empty()) {
    d->click_shape = part_click_shape(d->click_word.c_str(), d->click_pack);
    if (!d->click_shape && native_type_log())
      std::fprintf(stderr,
                   "[native-type] part type 0x%08X cfg '%s': click '%s' did "
                   "not load (Invalid clickdef): no click body\n",
                   static_cast<unsigned>(type->type_id), d->cfg_path.c_str(),
                   d->click_word.c_str());
  }
  // payload+0x18 of the type node: `lod_amp` (or the LODA chunk), default
  // 1.0 (FUN_0051faf0).
  d->lod_amp = type->has_lod_amp ? type->lod_amp : 1.f;
  part_type_derive(d.get());
  part_type_rebase_slots(d.get());
  if (native_type_log()) {
    // One line per type that is not the plain case, and for the first of
    // the plain ones: a new career makes about ninety part types.
    static bool s_first = true;
    const bool plain = d->cfg_found && d->unported_shapes == 0 &&
                       d->missing_shapes == 0;
    bool log = !plain || d->car != nullptr;  // every car type
    {
      std::lock_guard<std::mutex> lock(g_nt_mu);
      if (plain && s_first) {
        s_first = false;
        log = true;
      }
    }
    if (log) {
      const RpakEntry* e = rpak_find_entry(type->type_id);
      std::fprintf(stderr,
                   "[native-type] %s type 0x%08X '%s' cfg '%s'%s: %zu "
                   "entries (%d render, %d body, bone %d), click %d, %zu "
                   "slot(s), mass %g, mass centre (%.4f, %.4f, %.4f)%s%s%s\n",
                   d->car ? "car" : "part",
                   static_cast<unsigned>(type->type_id),
                   e ? e->name.c_str() : "", d->cfg_path.c_str(),
                   d->cfg_found ? "" : " NOT FOUND", d->entries.size(),
                   d->render_count, d->bodies, d->bone_index,
                   d->has_click ? 1 : 0, d->slots.size(), d->total_mass,
                   d->com.x, d->com.y, d->com.z,
                   d->com_shift ? "" : " (no shift)",
                   d->unported_shapes ? ", body shape record not ported" : "",
                   d->missing_shapes ? ", body mesh NOT LOADED" : "");
    }
  }
  std::lock_guard<std::mutex> lock(g_nt_mu);
  auto& slot = g_part_types[type->type_id];
  if (!slot) slot = std::move(d);
  return slot.get();
}

// Where bone00 of a render entry sits in the frame of the reference body
// (FUN_004c2b10, the branch for an entry without flag 0x8000):
//   offset   = entry offset + refRot^T * -(ref offset + shape+0x40)
//   rotation = entryRot * refRot^T              (FUN_00567380, FUN_00566a70)
// where ref is the body of type+0xFF0 (shape+0x40 only for a shape with a
// mass) or, in a type without a body, the `click` entry. bone00 follows
// that body (part+0x2C), whose pose is the pose the part object keeps.
// False when the reference has no shape ("Missing clickdef in %08x": the
// exe leaves bone00 unlinked): a type without a body and without a click,
// or one whose type+0xFF0 names an entry that is no body (a `bone` line
// behind a render line; the click entry is not tried then).
bool part_render_local(const PartTypeDesc& d, const PartEntry& r, CamV3* pos,
                       CamM3* rot) {
  CamV3 ref_off;
  CamM3 ref_rot = cam_identity();
  if (d.bone_index >= 0) {
    if (d.bone_index >= static_cast<int>(d.entries.size()) ||
        d.entries[static_cast<size_t>(d.bone_index)].kind != 1)
      return false;
    const PartEntry& b = d.entries[static_cast<size_t>(d.bone_index)];
    ref_rot = b.rot;
    ref_off = b.off;
    if (b.mass != 0.f) ref_off = ref_off + b.com;
  } else if (d.has_click) {
    // The `click` instance is the part's body: it stands at the create
    // position, turned by R * clickRot (FUN_004c1f90, the branch without
    // bodies; part_create).
    ref_rot = d.click_rot;
    ref_off = d.click_off;
  } else {
    return false;
  }
  const CamM3 ref_t = part_transpose(ref_rot);
  const CamV3 neg = cam_v(-ref_off.x, -ref_off.y, -ref_off.z);
  const CamV3 local = r.off + cam_mul(ref_t, neg);
  const CamM3 local_rot = cam_mm(r.rot, ref_t);
  // Fork: how FUN_004ec470 composes the bone's offset and rotation with
  // the body it follows was not read; the host takes them as a pose in the
  // body's frame. Where the reference line has no rotation (both lift
  // cfgs; all but 39 of the 2,679 cfgs of the install that have a body)
  // the readings cannot differ.
  *pos = local;
  *rot = local_rot;
  return true;
}

// ---------------------------------------------------------------------------
// The instance (0x14C bytes, FUN_00490830)
// ---------------------------------------------------------------------------
class PartObject;
// Fork: every part and car object that exists, for the walk of a camera
// over the nodes it sees (the exe walks its node tree, FUN_0045eb10). Main
// thread only, like the camera pass and the natives of the script boot.
// The list is never destroyed. The registries that own the objects
// (g_objects, g_nodes, g_ticking) are defined above this fragment, so on an
// exit that runs the static destructors they are torn down after everything
// defined here, and each ~PartObject they release still reads the list.
std::vector<PartObject*>& g_parts_live = *new std::vector<PartObject*>();
// (m6 step 1) Fork: the host ids that stand for the click bodies' "click"
// nodes (PartObject::phy_id_) and the node of the object each belongs to:
// what the exe's type-2 node has as its parent (ResourceRef.getParent,
// M6 step 4). Main thread only; never destroyed, like the list above.
std::unordered_map<int32_t, InvObject*>& g_click_phy =
    *new std::unordered_map<int32_t, InvObject*>();
// (m6 step 5) Fork: the nodes of the parts `filter .. 2` hid (PE 940 sets
// node flag 0x400000 on each render instance instead, FUN_004ebdd0).
// Never destroyed, like the two above.
std::unordered_set<void*>& g_filter_hidden = *new std::unordered_set<void*>();

class PartObject : public NativeObject {
 public:
  PartObject() { g_parts_live.push_back(this); }
  ~PartObject() override {
    if (phy_id_) g_click_phy.erase(phy_id_);
    if (key_) g_filter_hidden.erase(key_);
    for (size_t i = g_parts_live.size(); i-- > 0;)
      if (g_parts_live[i] == this) {
        g_parts_live[i] = g_parts_live.back();
        g_parts_live.pop_back();
        break;
      }
  }
  PartObject(const PartObject&) = delete;
  PartObject& operator=(const PartObject&) = delete;

  const PartTypeDesc* type_ = nullptr;
  // The pose of the reference body (part+0x2C `bbone`, see
  // PartTypeDesc::ref_at): what GameRef.setMatrix and `install` place
  // (FUN_004eac50 on part+0x2C) and what bone00 of the render instances
  // and the slots hang on. For a type with one unrotated body it is the
  // part's mass centre.
  // Fork: there are no bodies (no physics world instance); the pose never
  // moves by itself. The exe's compound `part_comp` is asleep after create
  // and after setMatrix (FUN_00506d90), so a part nothing touches stays
  // where it was put there too.
  CamV3 pos_;
  CamM3 rot_ = cam_identity();
  float ypr_[3] = {0.f, 0.f, 0.f};
  CamV3 create_pos_;               // part+0x12C
  int32_t colour_index_ = 0;       // part+0x58
  // Fork: the renderer key that carries the pose: the node object (the
  // object the script holds). The render instances hang under it.
  InvObject* key_ = nullptr;
  struct Inst {
    InvObject* r = nullptr;  // part+0x4C, a `part_r_o` render instance
    int entry = 0;           // part+0x50
    // The render type the instance is made from: the entry's, or for the
    // main render entry the part's own (part+0x9C).
    int32_t res_id = 0;
    // A `linked` entry of a mounted part (NativeTypes_part8.inc): 0 the
    // instance follows the part's reference body like every other; 1 its
    // bone00 follows the body of slot `link_a` with that slot's position
    // and rotation; 2 the car poses bone00 on slot `link_a` and bone01 on
    // slot `link_b` (its `add_linked` record). The slot records are those
    // of `link_a_on` / `link_b_on`: this part or something up its mount
    // chain (FUN_004c7bc0).
    int link = 0;
    int32_t link_a = 0, link_b = 0;
    InvObject* link_a_on = nullptr;
    InvObject* link_b_on = nullptr;
  };
  std::vector<Inst> insts_;
  int hidden_entries_ = 0;   // light-state entries (node flag 0x400000)
  int other_lod_entries_ = 0;
  bool unlinked_ = false;    // neither a body nor a click: nothing to follow
  // The level of detail the render instances are made for, and the detail
  // of the node (node+0xA0): the largest value a camera gave it in the
  // frame `detail_frame_` (node+0xAC), kept until a camera sees the part
  // again. A new node starts with the detail of the node it is made under
  // (FUN_00523c80, see part_init); `detail_frame_` 0 = no camera has seen
  // the part itself.
  int level_ = 0;
  float detail_ = 0.f;
  uint32_t detail_frame_ = 0;
  // Fork: what the part hangs under in the camera's walk, as of the frame
  // `walk_frame_` (asked once per frame, not once per camera).
  void* walk_parent_ = nullptr;
  uint32_t walk_frame_ = 0;
  bool gone_ = false;        // destroy() ran
  // (m6 step 1) The click body (part+0x3C, FUN_004c1f90): `phy_id_` the host
  // id that stands for its "click" node (0: the type has no click shape),
  // `click_hidden_` the body flag 0x4000 that takes it out of the pick:
  // set by the falloff branch of `remove` and by `filter .. 2`, cleared by
  // `filter .. 0` and `1` (X 1.6); clear at the create.
  int32_t phy_id_ = 0;
  bool click_hidden_ = false;

  // part+0x78: one 0x68-byte entry per slot of the type, in the type's
  // order (FUN_004c1f90 copies position and rotation from the type's list;
  // Part.setSlotPos and setSlotDamage rewrite them).
  struct Slot {
    int32_t id = 0;
    CamV3 pos;                   // +0x2C
    CamM3 rot = cam_identity();  // +0x38
    // +0x14: 1 this part is installed through the slot, 2 a part is
    // installed on the slot, 4 an install waits for its bolts, 8 a removal
    // waits for its bolts (FUN_004cb510, both records), 0x10 the
    // slot is disabled (Part.disableSlot), 0x20 the slot was moved
    // (Part.setSlotPos / setSlotDamage: its pose goes into a saved car),
    // 0x40 with 0x10 on an installed slot locks the chain above it
    // (FUN_004cb0b0).
    uint32_t flags = 0;
    // +0x00: a handle on the node of the part on the other side. Fork: its
    // node object. A handle goes unbound when its node is destroyed.
    InvObject* other = nullptr;
    int32_t other_slot = 0;      // +0x10
    // (m6 step 6) +0x28: the flap record of a slot whose type defines one,
    // its flags (record+0x88: bit 0 closed, bits 1 and 2 the open and
    // closing requests) and the node it was made for (Fork: made again,
    // closed, when another part sits on the slot).
    uint32_t flap_bits = 0;
    InvObject* flap_for = nullptr;
  };
  std::vector<Slot> slots_;
  int installed_ = 0;          // +0x74: slots in use, both directions
  int state_ = 0;              // +0x5C: 1 after `install`
  InvObject* root_ = nullptr;  // +0x60: the node `install` moves the part under
  // +0xC0: what the part sits on (the car, or the topmost part of its
  // tree); the part itself until it is installed (FUN_004c1f90).
  InvObject* car_ = nullptr;
  // +0xDC: the parts `add_part` announced (FUN_004c17b0); destroyed with
  // this part.
  std::vector<InvObject*> attached_;
  // +0x70: the state word. Of a part: 0x10000000 hidden by `filter .. 2`,
  // 0x20000000 set by `filter .. 1`. A car keeps its own bits here too
  // (CarObject).
  uint32_t flags_ = 0;
  // +0x8C / +0x9C: handles on the mesh and on the main render type of this
  // instance, copied from the type's at the create (type+0x10F8 `mesh`,
  // +0x1108 the first render line). Their ids (+0x94 / +0xA4) are what
  // Part.getMesh and getRenderType return; Part.setMesh and setRenderType
  // rebind them.
  int32_t mesh_ = 0;
  int32_t render_type_ = 0;
  // +0x7C: a handle on the texture of the type's `texture` line
  // (type+0x10E8), copied at the create like the two above. Its id (+0x84)
  // is what Part.getTexture returns; Part.setTexture and the `texture`
  // command replace that texture in the part's render type and rebind the
  // handle. 0 for a type without the line: such a part cannot be painted.
  int32_t texture_ = 0;
  // Fork: the lines of the part's private copy of its main render type that
  // differ from the pack's render object: texture lines by slot (slot,
  // texture id) and the mesh line. The exe duplicates the render type on
  // the first Part.setTexture / setMesh that changes something
  // (FUN_004c75a0 / FUN_004c72d0 -> FUN_004e9470) and replaces lines in the
  // copy (FUN_00522b00); the host keeps the render object and hands these
  // to the render instances made from the part's own handle
  // (part_lines_to). Emptied when Part.setRenderType gives the part a
  // render type of the pack.
  std::vector<std::pair<int32_t, int32_t>> tex_over_;
  bool mesh_over_set_ = false;
  int32_t mesh_over_ = 0;
  // +0xAC: the owner of the render instances (render instance payload
  // +0x1B4 / +0x1B8): the part's own node id until an install gives it the
  // car's (FUN_004ca390: getinfo 0x3F of the car). 0 = the own node.
  int32_t owner_ = 0;
  // +0xD0: the `slottype` of the slot at the top of the chain of slots the
  // part is installed through (FUN_004c8b10): on a car 101.. for a part on
  // a wheel, 201.. on a hub, 301.. on an arm. Part.getWheelID reads it.
  int32_t d0_ = 0;

  void push_pose() {
    render_d3d9_mesh_set_transform(key_, pos_.x, pos_.y, pos_.z, ypr_[0],
                                   ypr_[1], ypr_[2], 1.f, 1.f, 1.f);
  }

  Slot* slot(int32_t id) {  // FUN_004c6fd0
    for (Slot& s : slots_)
      if (s.id == id) return &s;
    return nullptr;
  }

  // getinfo 0x2F: the world frame of the body a slot hangs on. A part has
  // one body for all its slots, the reference body (FUN_004c1f90 binds
  // every slot's bone handle to `bbone`); a car answers by the slot's
  // `slottype` (CarObject, NativeTypes_part6.inc).
  virtual void slot_frame(int32_t /*slot_id*/, CamV3* pos, CamM3* rot) {
    *pos = pos_;
    *rot = rot_;
  }
  // The render instances of level `level_` (FUN_004c2b10; a car adds its
  // wheel renders, FUN_0049a8e0).
  virtual void make_instances();

  // The methods below are in NativeTypes_part5.inc (slots and `install`).
  // The reference body gets this pose, and every part installed on this
  // one is put on its slot again (in the exe they are bodies of the same
  // compound and move with it).
  void set_ref_pose(const CamV3& pos, const CamM3& rot, const float* ypr,
                    int depth);
  // FUN_004cb8d0: the pose the slot this part is installed through gives it.
  bool pose_on_slot(int depth);
  // The origin of the compound `part_comp`: the mass centre of the part's
  // bodies and of every part installed on it (FUN_005032d0).
  CamV3 compound_pos();

  void destroy() override;  // vt+0x10 FUN_004c3a50
  // vt+0x1C FUN_004c6370 ("ffffffffffff": position, angles, velocity,
  // angular velocity; GameRef.setMatrix sends the first six).
  bool set_matrix(const float pos[3], const float ypr[3]) override;
  // vt+0x3C FUN_004c3da0.
  bool getinfo(int32_t query, NativeInfo* out) override;
  bool get_ori(float ypr[3]) override;
  // vt+0x38 FUN_004c4400.
  bool event(int32_t type, const char* text) override;
};

// PE 940 FUN_00524840(part node, entry+0x44, alias, 0): a render instance
// under the part's node, its bone00 at `at` / `rot` in the frame of the
// part's reference body.
// Fork: a host RenderRef object with its own id (Q12) under the part's key
// in the renderer's tree; it waits on the pending list like every instance
// whose type is not loaded, so a part no camera sees loads nothing.
// `res_id` is the render type: the entry's, or the part's own for the main
// render entry (part_entry_render).
InvObject* part_new_render_instance(PartObject* part, const PartEntry& e,
                                    int32_t res_id, const char* alias,
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
    rs.parent = part->key_;
    rs.alias = alias;
    rs.loaded = true;
  }
  tree_field_set_obj(r, "alias", string_new(alias));
  float yaw = e.ypr[0], pitch = e.ypr[1], roll = e.ypr[2];
  bool same = true;
  for (int a = 0; a < 3 && same; ++a)
    for (int b = 0; b < 3 && same; ++b)
      same = rot.m[a][b] == e.rot.m[a][b];
  if (!same) cam_matrix_to_ypr(rot, &yaw, &pitch, &roll);
  render_d3d9_mesh_set_tree_parent(r, part->key_);
  render_d3d9_mesh_set_transform(r, at.x, at.y, at.z, yaw, pitch, roll, 1.f,
                                 1.f, 1.f);
  render_d3d9_mesh_set_pending_type(r, res_id);
  // FUN_004ebd20(colors[index % count]): the instance colour of the
  // create string's seventh field (render instance payload+0x17C).
  if (!e.colors.empty()) {
    const int32_t n = static_cast<int32_t>(e.colors.size());
    const int32_t pick = part->colour_index_ % n;
    render_d3d9_mesh_set_color(
        r, e.colors[static_cast<size_t>(pick < 0 ? pick + n : pick)]);
  }
  return r;
}

// PE 940 FUN_004c2b10 (asm 0x4c2dc4..0x4c2dda): an entry whose render type
// is the type's main one (type+0x1110) is instanced from the part's own
// handle part+0x9C, which Part.setRenderType rebinds; every other entry
// from its own.
int32_t part_entry_render(const PartObject* part, const PartEntry& e) {
  const PartTypeDesc& d = *part->type_;
  if (d.main_render && e.res_id == d.main_render && part->render_type_)
    return part->render_type_;
  return e.res_id;
}

// The replaced lines of the part's own render type go to a render instance
// made from it (Fork, see PartObject::tex_over_).
void part_lines_to(const PartObject* part, InvObject* r) {
  std::vector<int32_t> slots, ids;
  slots.reserve(part->tex_over_.size());
  ids.reserve(part->tex_over_.size());
  for (const auto& o : part->tex_over_) {
    slots.push_back(o.first);
    ids.push_back(o.second);
  }
  renderref_set_line_overrides(r, slots.data(), ids.data(),
                               static_cast<int32_t>(slots.size()),
                               part->mesh_over_set_, part->mesh_over_);
}

// FUN_004c75a0 / FUN_004c72d0 on a part or car object: Part.setTexture /
// setMesh and the `texture` / `mesh` commands (NativeTypes_part7.inc).
void part_set_texture(PartObject* part, int32_t id);
void part_set_mesh(PartObject* part, int32_t id);

// PE 940 FUN_004c2950 / FUN_004c1e90 / FUN_00498180, the loop over the
// thresholds: the level is the first i with detail < t[i], else the count.
int part_level_for(const PartTypeDesc& d, float detail) {
  for (int i = 0; i < d.lod_count; ++i)
    if (detail < d.lod_t[static_cast<size_t>(i)]) return i;
  return d.lod_count;
}

inline bool part_entry_in_level(const PartEntry& e, int level) {
  const int first = static_cast<int>((e.flags >> 16) & 0xfu);
  const int last = static_cast<int>((e.flags >> 20) & 0xfu);
  return first <= level && level <= last;
}

// The linked entries of the part's instances (NativeTypes_part8.inc).
void part_link_instances(PartObject* part);
void part_unlink_instance(PartObject* part, PartObject::Inst* inst);

// PE 940 FUN_004c2b10(type, part, level): the render instances whose entry
// is not of the level go (the last instance takes the freed place in the
// list), and every other render entry of the level gets one.
// Fork: the exe makes the instances of a new level when the level changes
// and swaps them in once their render types are loaded (FUN_004c34e0); the
// host drops the old ones at once and the new ones wait on the pending
// list like every instance.
void part_make_render_instances(PartObject* part, int level) {
  const PartTypeDesc& d = *part->type_;
  std::vector<char> kept(d.entries.size(), 0);
  for (size_t k = part->insts_.size(); k-- > 0;) {
    PartObject::Inst& in = part->insts_[k];
    const size_t ei = static_cast<size_t>(in.entry);
    if (ei < d.entries.size() && part_entry_in_level(d.entries[ei], level)) {
      kept[ei] = 1;
      continue;
    }
    part_unlink_instance(part, &in);
    if (in.r) {
      java_util_resource_ResourceRef_destroy(in.r);
      java_util_resource_ResourceRef_deleteNative(in.r);
    }
    if (k + 1 < part->insts_.size()) part->insts_[k] = part->insts_.back();
    part->insts_.pop_back();
  }
  part->hidden_entries_ = 0;
  part->other_lod_entries_ = 0;
  for (size_t i = 0; i < d.entries.size(); ++i) {
    const PartEntry& e = d.entries[i];
    if (e.kind != 3) continue;
    // Entry flag 0x1000 keeps an entry out of every level here: a car
    // makes its wheel renders itself (FUN_0049a8e0,
    // car_make_wheel_instances).
    if (!part_entry_in_level(e, level) || (e.flags & 0x1000u) != 0) {
      ++part->other_lod_entries_;
      continue;
    }
    if (kept[i]) continue;
    // An entry with a light-state bit (`flags` & 0x13f without 0x40: the
    // lamps and glows of a car part) gets node flag 0x400000 and is not
    // drawn until the car switches it on (`add_light`).
    // Fork: no such instance is made; the switching is not ported.
    if ((e.flags & 0x13fu) != 0 && (e.flags & 0x40u) == 0) {
      ++part->hidden_entries_;
      continue;
    }
    if (!e.res_id) continue;  // the id names neither record
    CamV3 at;
    CamM3 rot = cam_identity();
    if (!part_render_local(d, e, &at, &rot)) {
      // "Missing clickdef in %08x": the exe leaves bone00 unlinked and the
      // mesh stays at the origin of the world. Fork: no instance is made.
      part->unlinked_ = true;
      continue;
    }
    const int32_t res = part_entry_render(part, e);
    InvObject* r = part_new_render_instance(part, e, res, "part_r_o", at, rot);
    if (!r) continue;
    // The main render entry is instanced from part+0x9C (part_entry_render):
    // from the private copy when the part has one, so with its replaced
    // lines.
    if (d.main_render && e.res_id == d.main_render &&
        (!part->tex_over_.empty() || part->mesh_over_set_))
      part_lines_to(part, r);
    PartObject::Inst inst;
    inst.r = r;
    inst.entry = static_cast<int>(i);
    inst.res_id = res;
    part->insts_.push_back(inst);
  }
}

void PartObject::make_instances() { part_make_render_instances(this, level_); }

// PE 940 FUN_004c1f90: the instance of a part type. The car's create
// FUN_004992e0 calls it too, with its own bigger object (car_create).
void part_init(PartObject* part, const NativeCreate& c) {
  part->type_ = part_type_desc(c.type);
  const PartTypeDesc& d = *part->type_;
  // "%f,%f,%f,%f,%f,%f,%d": position, angles, and the index into the
  // `colors` list of every render entry.
  float pos[3] = {0.f, 0.f, 0.f};
  float ypr[3] = {0.f, 0.f, 0.f};
  int32_t colour = 0;
  engine_sscanf(c.params ? c.params : "", "%f,%f,%f,%f,%f,%f,%d", &pos[0],
                &pos[1], &pos[2], &ypr[0], &ypr[1], &ypr[2], &colour);
  part->create_pos_ = cam_v(pos[0], pos[1], pos[2]);
  part->colour_index_ = colour;
  part->key_ = c.instance ? c.instance : c.handle;
  const CamM3 rot = cam_ypr_to_matrix(ypr[0], ypr[1], ypr[2]);
  part->rot_ = rot;
  part->ypr_[0] = ypr[0];
  part->ypr_[1] = ypr[1];
  part->ypr_[2] = ypr[2];
  // With a body (type+0xFF0 >= 0): the compound is made at the position,
  // its child bodies at position + R * (line offset + line rotation *
  // shape+0x40), and FUN_005032d0 then moves the compound's own origin to
  // their mass centre without moving them. Without (a `click` shape only):
  // the click instance at the position, turned by R * clickRot. The part
  // object keeps the pose of the reference body (PartTypeDesc::ref_at).
  part->pos_ = part->create_pos_ + cam_mul(rot, d.ref_at);
  if (!part_is_identity(d.ref_rot)) {
    part->rot_ = cam_mm(rot, d.ref_rot);
    cam_matrix_to_ypr(part->rot_, &part->ypr_[0], &part->ypr_[1],
                      &part->ypr_[2]);
  }
  // FUN_004c1f90: the slot table of the instance, and the car handle
  // (part+0xC0) on the part's own node.
  part->slots_.reserve(d.slots.size());
  for (const PartSlot& s : d.slots) {
    PartObject::Slot e;
    e.id = s.id;
    e.pos = s.pos;
    e.rot = s.rot;
    part->slots_.push_back(e);
  }
  part->car_ = part->key_;
  // The handles part+0x7C / +0x8C / +0x9C: the type's texture, mesh and
  // main render type (FUN_004c1f90 binds them from type+0x10F4 / +0x1104 /
  // +0x1114).
  part->texture_ = d.texture_id;
  part->mesh_ = d.mesh_id;
  part->render_type_ = d.main_render;
  // The click body (asm 0x4c24b1..0x4c267a): a type-2 node "click" (or
  // "cbone") under the part's node, owned by it, for a type whose click
  // line made a body type. Fork: no node is made; the object keeps an id
  // that stands for it (the `phy` of the cursor's events), from the
  // sequence the physics instances of the scripts take theirs from, and the
  // pick poses the body from the object's pose (NativeTypes_part11.inc).
  // Fork (X 1.7 item 7): a type without a `click` line gets no id and no
  // pick body; the exe picks its cfg `body` entries instead (X 1.1: 80
  // cfgs, traffic, police, fake racers; m6_spec follow-up 2).
  if (d.click_shape) {
    {
      std::lock_guard<std::mutex> lock(g_mu);
      part->phy_id_ = resref_next_runtime_id();
    }
    g_click_phy[part->phy_id_] = part->key_;
  }
  part->owns_create_pose = true;
  part->push_pose();
  // PE 940 FUN_00523c80 (the node of a GameRef.create): before the type's
  // create runs, the new node takes the detail of the node at its +0x24,
  // its parent (FUN_004ea780): node vt+0x18(parent+0xA0), vt+0x14(parent
  // +0x9C), the setters FUN_0051a6f0 / FUN_0051a740. The node's own factor
  // and offset are still the 1 and 0 of its constructor there (FUN_00519260
  // -> FUN_0051e930); the type's lod_amp is copied to node+0xEC after it.
  // (M3 spec Q3.)
  // Fork: the host keeps a detail for part and car objects only; a part
  // made under any other node starts with 0, where the exe has the value
  // that node carries.
  if (c.parent) {
    std::shared_ptr<NativeObject> up = native_object_of(c.parent);
    if (up && up.get() != part &&
        (up->type_index == kNativePart || up->type_index == kNativeCar))
      part->detail_ = static_cast<PartObject*>(up.get())->detail_;
  }
  // FUN_004c2950: the level the node's detail gives, then FUN_004c2b10.
  part->level_ = part_level_for(d, part->detail_);
  part_make_render_instances(part, part->level_);
  // Not made here (Fork): the bodies (the click body is posed by the pick
  // from the object's pose, NativeTypes_part11.inc), the callback lists
  // (FUN_004ebd90(part, 0x10295, 0)): a part has nothing to tick on the
  // host yet, so `ticks` stays off.
  if (native_type_log()) {
    // One line per odd part only: a new career makes about 330 parts. The
    // parts a camera draws are listed by native_part_debug_dump.
    if (part->unlinked_) {
      const RpakEntry* e = rpak_find_entry(d.type_id);
      std::fprintf(stderr,
                   "[native-type] part %p type 0x%08X '%s': create (%g, %g, "
                   "%g) ypr (%g, %g, %g) colour %d -> body (%.4f, %.4f, "
                   "%.4f); %zu render instance(s), %d hidden, %d of another "
                   "lod%s\n",
                   static_cast<void*>(part->key_),
                   static_cast<unsigned>(d.type_id), e ? e->name.c_str() : "",
                   pos[0], pos[1], pos[2], ypr[0], ypr[1], ypr[2], colour,
                   part->pos_.x, part->pos_.y, part->pos_.z,
                   part->insts_.size(), part->hidden_entries_,
                   part->other_lod_entries_,
                   part->unlinked_ ? "; Missing clickdef (no body, no click)"
                                   : "");
    }
  }
}

// PE 940 FUN_004c2950 -> FUN_004c1f90 (type vt+0x0C).
std::shared_ptr<NativeObject> part_create(const NativeCreate& c) {
  auto part = std::make_shared<PartObject>();
  // Fork: native_object_create writes the index when the factory returns;
  // the code a create runs asks for it before that.
  part->type_index = kNativePart;
  part_init(part.get(), c);
  return part;
}

}  // namespace

// ---------------------------------------------------------------------------
// Light types (typeof 13, `ResourceType_Render_Light`)
// ---------------------------------------------------------------------------
namespace {

std::unordered_map<int32_t, std::unique_ptr<RenderLightDesc>> g_light_types;
// The light type copies ResourceRef.duplicate made: copy id -> the pack
// record at the start of the chain (a copy of a copy names the record).
std::unordered_map<int32_t, int32_t> g_light_copies;

// PE 940 FUN_005219c0: the payload is text, one keyword per line
// (`_stricmp`). The colour lines carry alpha first: "%s %f %f %f %f" into
// a, r, g, b.
void parse_light_payload(const std::vector<uint8_t>& blob, RenderLightDesc* d) {
  std::vector<uint8_t> buf(blob);
  buf.push_back(0);
  const uint8_t* p = buf.data();
  const uint8_t* const end = buf.data() + buf.size();
  char word[1025];
  char arg[1025];
  std::string line;
  while (p < end) {
    p += read_line(p, end, &line);
    if (line.empty()) continue;
    word[0] = 0;
    engine_sscanf(line.c_str(), "%1024s", word);
    const char* s = line.c_str();
    if (word_is(word, "type")) {
      arg[0] = 0;
      engine_sscanf(s, "%1024s %1024s", word, arg);
      if (word_is(arg, "directional"))
        d->type = 3;
      else if (word_is(arg, "point"))
        d->type = 1;
      else if (word_is(arg, "fakepoint"))
        d->type = 4;
      else if (word_is(arg, "fakespot"))
        d->type = 5;
      else if (word_is(arg, "spot"))
        d->type = 2;
    } else if (word_is(word, "diffuse")) {
      engine_sscanf(s, "%1024s %f %f %f %f", word, &d->diffuse[3],
                    &d->diffuse[0], &d->diffuse[1], &d->diffuse[2]);
    } else if (word_is(word, "specular")) {
      engine_sscanf(s, "%1024s %f %f %f %f", word, &d->specular[3],
                    &d->specular[0], &d->specular[1], &d->specular[2]);
    } else if (word_is(word, "ambient")) {
      engine_sscanf(s, "%1024s %f %f %f %f", word, &d->ambient[3],
                    &d->ambient[0], &d->ambient[1], &d->ambient[2]);
    } else if (word_is(word, "position")) {
      engine_sscanf(s, "%1024s %f %f %f", word, &d->position[0],
                    &d->position[1], &d->position[2]);
    } else if (word_is(word, "direction")) {
      engine_sscanf(s, "%1024s %f %f %f", word, &d->direction[0],
                    &d->direction[1], &d->direction[2]);
    } else if (word_is(word, "range")) {
      engine_sscanf(s, "%1024s %f", word, &d->range);
    } else if (word_is(word, "falloff")) {
      engine_sscanf(s, "%1024s %f", word, &d->falloff);
    } else if (word_is(word, "attenuation")) {
      engine_sscanf(s, "%1024s %f %f %f", word, &d->attenuation[0],
                    &d->attenuation[1], &d->attenuation[2]);
    } else if (word_is(word, "theta")) {
      engine_sscanf(s, "%1024s %f", word, &d->theta);
    } else if (word_is(word, "phi")) {
      engine_sscanf(s, "%1024s %f", word, &d->phi);
    } else if (word_is(word, "lit_dynamic")) {
      int32_t v = 1;
      engine_sscanf(s, "%1024s %d", word, &v);
      d->is_static = (v == 0);
    }
    // Read past (Fork): iscolorable, attH_texture, attV_texture, lit_sobj,
    // lit_dobj, the shd_* lines, plane and the lf_* lens flare lines.
  }
}

RenderLightDesc* light_type_mutable(int32_t id) {
  if (id == 0) return nullptr;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    auto it = g_light_types.find(id);
    if (it != g_light_types.end()) return it->second.get();
  }
  const RpakEntry* e = rpak_find_entry(id);
  if (!e || e->restype != 13) return nullptr;
  auto d = std::make_unique<RenderLightDesc>();  // FUN_005213c0 defaults
  std::vector<uint8_t> blob;
  if (rpak_read_entry(id, &blob)) parse_light_payload(blob, d.get());
  if (native_type_log())
    std::fprintf(stderr,
                 "[native-type] light type 0x%08X '%s': type %d%s, diffuse "
                 "(%g, %g, %g) ambient (%g, %g, %g) specular (%g, %g, %g), "
                 "position (%g, %g, %g) direction (%g, %g, %g) range %g "
                 "attenuation (%g, %g, %g)\n",
                 static_cast<unsigned>(id), e->name.c_str(), d->type,
                 d->is_static ? " static" : " dynamic", d->diffuse[0],
                 d->diffuse[1], d->diffuse[2], d->ambient[0], d->ambient[1],
                 d->ambient[2], d->specular[0], d->specular[1], d->specular[2],
                 d->position[0], d->position[1], d->position[2],
                 d->direction[0], d->direction[1], d->direction[2], d->range,
                 d->attenuation[0], d->attenuation[1], d->attenuation[2]);
  std::lock_guard<std::mutex> lock(g_nt_mu);
  auto& slot = g_light_types[id];
  if (!slot) slot = std::move(d);
  return slot.get();
}

}  // namespace

const RenderLightDesc* light_type_payload(int32_t id) {
  return light_type_mutable(id);
}

// PE 940 FUN_004ebef0: each colour byte x 1 / 256 (DAT_005a6930), into the
// payload of the handle's own node: a light type made with
// ResourceRef.duplicate is a node of its own (light_type_duplicate), so the
// record it was copied from keeps its values.
bool light_type_set_colours(int32_t id, int32_t diffuse, int32_t ambient,
                            int32_t specular) {
  RenderLightDesc* d = light_type_mutable(id);
  if (!d) return false;
  if (native_type_log()) {
    const int32_t record = light_type_source(id);
    std::fprintf(stderr,
                 "[native-type] light type 0x%08X%s setLight diffuse "
                 "0x%06X ambient 0x%06X specular 0x%06X\n",
                 static_cast<unsigned>(id), record ? " (copy)" : "",
                 static_cast<unsigned>(diffuse & 0xffffff),
                 static_cast<unsigned>(ambient & 0xffffff),
                 static_cast<unsigned>(specular & 0xffffff));
  }
  constexpr float k = 0.00390625f;
  auto put = [](float* rgb, int32_t packed, float scale) {
    rgb[0] = static_cast<float>((packed >> 16) & 0xff) * scale;
    rgb[1] = static_cast<float>((packed >> 8) & 0xff) * scale;
    rgb[2] = static_cast<float>(packed & 0xff) * scale;
  };
  put(d->diffuse, diffuse, k);
  put(d->ambient, ambient, k);
  put(d->specular, specular, k);
  render_d3d9_scene_light_update_type(id, d);
  return true;
}

// PE 940 ResourceRef.duplicate 0x4E32E0 -> FUN_004e9470: a light type
// (typeof 13) is copied by FUN_0051ee30 into a NEW node and the handle is
// bound to it (X 3.4, q18 / q19). Host: the copy is a payload of its own
// under the new runtime id; RenderRef.setLight / setFlare on it leave the
// record alone, and the lights made from it (`new RenderRef(map, suntype,
// "sunny")`, Scene.java:309) read the copy.
bool light_type_duplicate(int32_t src_id, int32_t copy_id) {
  if (!src_id || !copy_id || src_id == copy_id) return false;
  RenderLightDesc* src = light_type_mutable(src_id);
  if (!src) return false;
  int32_t record = src_id;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    auto c = g_light_copies.find(src_id);
    if (c != g_light_copies.end()) record = c->second;
    g_light_types[copy_id] = std::make_unique<RenderLightDesc>(*src);
    g_light_copies[copy_id] = record;
  }
  if (native_type_log()) {
    const RpakEntry* e = rpak_find_entry(record);
    std::fprintf(stderr,
                 "[native-type] duplicate light type 0x%08X '%s' -> copy "
                 "0x%08X (record 0x%08X): diffuse (%g, %g, %g) ambient (%g, "
                 "%g, %g) specular (%g, %g, %g)\n",
                 static_cast<unsigned>(src_id), e ? e->name.c_str() : "",
                 static_cast<unsigned>(copy_id), static_cast<unsigned>(record),
                 src->diffuse[0], src->diffuse[1], src->diffuse[2],
                 src->ambient[0], src->ambient[1], src->ambient[2],
                 src->specular[0], src->specular[1], src->specular[2]);
  }
  return true;
}

int32_t light_type_source(int32_t id) {
  if (!id) return 0;
  std::lock_guard<std::mutex> lock(g_nt_mu);
  auto it = g_light_copies.find(id);
  return it == g_light_copies.end() ? 0 : it->second;
}

// PE 940 RenderRef.setFlare 0x4E2950 -> FUN_004ebe00 on the light type's
// payload: +0xD4 = glow min size, +0xD8 = glow max size, +0xE0 = colour,
// +0x100 = flare count, +0xE4 = ray count (asm 0x4e2989..0x4e29dc), and the
// texture handle relinked into the payload. The cLights re-read their
// type's payload every frame (FUN_004592d0).
// Fork (Q10): stored only; nothing draws a lens flare from it yet.
bool light_type_set_flare(int32_t id, int32_t texture_id, int32_t colour,
                          float min_size, float max_size, int32_t flare_count,
                          int32_t ray_count) {
  RenderLightDesc* d = light_type_mutable(id);
  if (!d) return false;
  d->flare_set = true;
  d->flare_texture = texture_id;
  d->flare_colour = static_cast<uint32_t>(colour);
  d->flare_min = min_size;
  d->flare_max = max_size;
  d->flare_count = flare_count;
  d->flare_rays = ray_count;
  render_d3d9_scene_light_update_type(id, d);
  if (native_type_log())
    std::fprintf(stderr,
                 "[native-type] light type 0x%08X%s setFlare texture 0x%08X "
                 "colour 0x%08X glow %g .. %g flares %d rays %d (stored, not "
                 "drawn)\n",
                 static_cast<unsigned>(id),
                 light_type_source(id) ? " (copy)" : "",
                 static_cast<unsigned>(texture_id),
                 static_cast<unsigned>(colour), min_size, max_size,
                 flare_count, ray_count);
  return true;
}

// Fork (diagnostic): the payloads as they stand, records first.
void native_light_type_debug_dump(FILE* out) {
  if (!out || !native_script_boot()) return;
  struct Row {
    int32_t id;
    int32_t record;
    RenderLightDesc d;
  };
  std::vector<Row> rows;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    for (const auto& kv : g_light_types) {
      if (!kv.second) continue;
      auto c = g_light_copies.find(kv.first);
      rows.push_back({kv.first, c == g_light_copies.end() ? 0 : c->second,
                      *kv.second});
    }
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    const bool ac = a.record != 0, bc = b.record != 0;
    if (ac != bc) return !ac;
    return static_cast<uint32_t>(a.id) < static_cast<uint32_t>(b.id);
  });
  for (const Row& r : rows) {
    const RpakEntry* e = rpak_find_entry(r.record ? r.record : r.id);
    char from[48] = "";
    if (r.record)
      std::snprintf(from, sizeof from, " copy of 0x%08X",
                    static_cast<unsigned>(r.record));
    std::fprintf(out,
                 "[render-dbg] light type 0x%08X '%s'%s: type %d diffuse "
                 "(%g, %g, %g) ambient (%g, %g, %g) specular (%g, %g, %g) "
                 "flare %s",
                 static_cast<unsigned>(r.id), e ? e->name.c_str() : "", from,
                 r.d.type, r.d.diffuse[0], r.d.diffuse[1], r.d.diffuse[2],
                 r.d.ambient[0], r.d.ambient[1], r.d.ambient[2],
                 r.d.specular[0], r.d.specular[1], r.d.specular[2],
                 r.d.flare_set ? "set" : "none");
    if (r.d.flare_set)
      std::fprintf(out, " (texture 0x%08X colour 0x%08X %g .. %g, %d, %d)",
                   static_cast<unsigned>(r.d.flare_texture),
                   static_cast<unsigned>(r.d.flare_colour), r.d.flare_min,
                   r.d.flare_max, r.d.flare_count, r.d.flare_rays);
    std::fprintf(out, "\n");
  }
}

// native_part_debug_dump, the `[render-dbg]` lines of the parts and cars,
// is in NativeTypes_part6.inc.
