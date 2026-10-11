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
// bone, FUN_004c2950 -> FUN_004c1f90 create, FUN_004c2b10 render instances,
// FUN_004c6370 setMatrix, FUN_004c3da0 getinfo, FUN_004c3a50 destroy.
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
// Fork: bounded by the file size (the exe trusts the file); the exe's
// run-time switch DAT_007a75e4 that reads versions 5 / 6 as 3 / 4 is not
// ported (no shipped shape file has those versions).
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
      if (p + 16 > size) return false;
      const uint32_t type = rd32(base + p);
      const uint32_t csize = rd32(base + p + 4);
      if (csize < 16 || static_cast<uint64_t>(p) + csize > size) return false;
      if (type == 4) {
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
  // 0x100 nocollision, 0x200 noclick, 0x400 bone.
  uint32_t flags = 0;
  // kind 3
  int32_t res_id = 0;          // +0x44: a render object or a light type
  bool is_light = false;       // the record is typeof 13
  std::vector<int32_t> colors; // +0x5C / +0x60
  // kind 1
  float mass = 0.f;            // shape+0x2C is 1 / mass, 0 for mass 0
  int shape_kind = 0;          // shape+8: 1 ball, 5 box, 2 3 4 rods, 6 mesh
  std::string shape_path;
  CamV3 com;                   // shape+0x40
  double volume = 0.0;         // shape+0x4C (mesh shapes with a mass)
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
  // +0x6C: one per `bolt` line behind the slot (FUN_004bfd00). An install
  // whose two slots have no bolts is finished at once (FUN_004cb1d0).
  int bolts = 0;
  struct Attach {
    int32_t type_id = 0;  // resolved through the cfg's pack (FUN_0051e860)
    int32_t slot = 0;
  };
  std::vector<Attach> attach;      // +0xC4: `attach <type> <slot>`
  std::vector<Attach> compatible;  // +0x110: `compatible <type> <slot>`
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
  std::vector<PartSlot> slots;     // type+0x116C
  std::vector<float> lods;         // type+0xFAC / +0xFB0
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
  if (e->shape_kind != 6) return;
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
}

void parse_part_cfg(const std::string& text, int32_t pack, PartTypeDesc* d) {
  // FUN_00569b40 / FUN_00569b20 zero the locals the scans fill; a field the
  // line lacks keeps that zero.
  part_cfg_lines(text, [&](const PartCfgLine& l) {
    PartEntry* last = d->entries.empty() ? nullptr : &d->entries.back();
    if (word_is(l.word, "render") || word_is(l.word, "light") ||
        word_is(l.word, "deformable")) {
      // FUN_004bf8e0, "%d %f %f %f %f %f %f".
      if (d->entries.size() > 31 || d->render_count > 31) return;
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
      d->entries.push_back(e);
      ++d->render_count;
    } else if (word_is(l.word, "body")) {
      // FUN_004bf4d0, "%f %f %f %f %f %f %f %s %f %f %f": offset, angles,
      // mass, shape, three sizes.
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
      part_body_shape(shape, mass, &e, d);
      d->entries.push_back(e);
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
    }
    // The other keywords of the table at 0x5DAB88 are read past: buoy,
    // shadow, trigger, collision_ps, category,
    // slotdmgmode, slotdeform, slottype, slotflags, slotmove, slotturn,
    // deform, mesh, texture, logo, damage, intdamage, material, wing,
    // rollbar, skinned, linked, stockpart, sfx_loop.
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
  part_type_derive(d.get());
  part_type_rebase_slots(d.get());
  if (native_type_log()) {
    // One line per type that is not the plain case, and for the first of
    // the plain ones: a new career makes about ninety part types.
    static bool s_first = true;
    const bool plain = d->cfg_found && d->unported_shapes == 0 &&
                       d->missing_shapes == 0;
    bool log = !plain;
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
                   "[native-type] part type 0x%08X '%s' cfg '%s'%s: %zu "
                   "entries (%d render, %d body, bone %d), click %d, %zu "
                   "slot(s), mass %g, mass centre (%.4f, %.4f, %.4f)%s%s%s\n",
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
class PartObject final : public NativeObject {
 public:
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
  };
  std::vector<Inst> insts_;
  int hidden_entries_ = 0;   // light-state entries (node flag 0x400000)
  int other_lod_entries_ = 0;
  bool unlinked_ = false;    // neither a body nor a click: nothing to follow

  // part+0x78: one 0x68-byte entry per slot of the type, in the type's
  // order (FUN_004c1f90 copies position and rotation from the type's list;
  // Part.setSlotPos rewrites them, which is not ported).
  struct Slot {
    int32_t id = 0;
    CamV3 pos;                   // +0x2C
    CamM3 rot = cam_identity();  // +0x38
    // +0x14: 1 this part is installed through the slot, 2 a part is
    // installed on the slot, 4 an install waits for its bolts, 0x10 the
    // slot is disabled (Part.disableSlot, not ported), 0x40 with 0x10 on
    // an installed slot locks the chain above it (FUN_004cb0b0).
    uint32_t flags = 0;
    // +0x00: a handle on the node of the part on the other side. Fork: its
    // node object. A handle goes unbound when its node is destroyed.
    InvObject* other = nullptr;
    int32_t other_slot = 0;      // +0x10
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

  void push_pose() {
    render_d3d9_mesh_set_transform(key_, pos_.x, pos_.y, pos_.z, ypr_[0],
                                   ypr_[1], ypr_[2], 1.f, 1.f, 1.f);
  }

  Slot* slot(int32_t id) {  // FUN_004c6fd0
    for (Slot& s : slots_)
      if (s.id == id) return &s;
    return nullptr;
  }

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

// PE 940 FUN_004c2b10 for level of detail 0: one render instance per
// render entry whose `lod` range holds 0.
void part_make_render_instances(PartObject* part) {
  const PartTypeDesc& d = *part->type_;
  for (size_t i = 0; i < d.entries.size(); ++i) {
    const PartEntry& e = d.entries[i];
    if (e.kind != 3) continue;
    // Fork: the level of detail of the node (payload+0xA0 against the
    // `lods` distances, FUN_004c2950) is not ported; the host always makes
    // level 0. Entry flag 0x1000 keeps an entry out of every level.
    const uint32_t first_lod = (e.flags >> 16) & 0xfu;
    if (first_lod != 0 || (e.flags & 0x1000u) != 0) {
      ++part->other_lod_entries_;
      continue;
    }
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
    // FUN_00524840(part node, entry+0x44, "part_r_o", 0): a render instance
    // under the part's node. Fork: a host RenderRef object with its own id
    // (Q12) under the part's key in the renderer's tree; it waits on the
    // pending list like every instance whose type is not loaded, so a part
    // no camera sees loads nothing.
    InvObject* r = tree_host_new("java.util.resource.RenderRef");
    if (!r) continue;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      auto& rs = R(r);
      rs.id = resref_next_runtime_id();
      rs.type = 3;  // INSTANCE_RENDER
      rs.restype = 3;
      rs.type_id = e.res_id;
      rs.parent = part->key_;
      rs.alias = "part_r_o";
      rs.loaded = true;
    }
    tree_field_set_obj(r, "alias", string_new("part_r_o"));
    float yaw = e.ypr[0], pitch = e.ypr[1], roll = e.ypr[2];
    bool same = true;
    for (int a = 0; a < 3 && same; ++a)
      for (int b = 0; b < 3 && same; ++b)
        same = rot.m[a][b] == e.rot.m[a][b];
    if (!same) cam_matrix_to_ypr(rot, &yaw, &pitch, &roll);
    render_d3d9_mesh_set_tree_parent(r, part->key_);
    render_d3d9_mesh_set_transform(r, at.x, at.y, at.z, yaw, pitch, roll, 1.f,
                                   1.f, 1.f);
    render_d3d9_mesh_set_pending_type(r, e.res_id);
    // FUN_004ebd20(colors[index % count]): the instance colour of the
    // create string's seventh field (render instance payload+0x17C).
    if (!e.colors.empty()) {
      const int32_t n = static_cast<int32_t>(e.colors.size());
      const int32_t pick = part->colour_index_ % n;
      render_d3d9_mesh_set_color(
          r, e.colors[static_cast<size_t>(pick < 0 ? pick + n : pick)]);
    }
    PartObject::Inst inst;
    inst.r = r;
    inst.entry = static_cast<int>(i);
    part->insts_.push_back(inst);
  }
}

// PE 940 FUN_004c2950 -> FUN_004c1f90 (type vt+0x0C).
std::shared_ptr<NativeObject> part_create(const NativeCreate& c) {
  auto part = std::make_shared<PartObject>();
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
  part->owns_create_pose = true;
  part->push_pose();
  part_make_render_instances(part.get());
  // Not made here (Fork): the bodies and the click shape themselves, the
  // callback lists
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
  return part;
}

}  // namespace

// ---------------------------------------------------------------------------
// Light types (typeof 13, `ResourceType_Render_Light`)
// ---------------------------------------------------------------------------
namespace {

std::unordered_map<int32_t, std::unique_ptr<RenderLightDesc>> g_light_types;

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

// PE 940 FUN_004ebef0: each colour byte x 1 / 256 (DAT_005a6930).
// Fork: the exe writes the payload of the handle's own node, and a light
// type made with ResourceRef.duplicate has a node of its own; the host's
// duplicate shares the record id (Resources_part2.inc), so the copies of
// one record share these values.
bool light_type_set_colours(int32_t id, int32_t diffuse, int32_t ambient,
                            int32_t specular) {
  RenderLightDesc* d = light_type_mutable(id);
  if (!d) return false;
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

void native_part_install_dump(FILE* out);  // NativeTypes_part5.inc

// `[render-dbg]` lines for the parts: a count, and one line per part whose
// render instance has its mesh (a camera saw it).
void native_part_debug_dump(FILE* out) {
  if (!out || !native_script_boot()) return;
  std::vector<std::shared_ptr<NativeObject>> parts;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    for (const auto& kv : g_objects) {
      if (!kv.second || kv.second->type_index != kNativePart) continue;
      bool seen = false;
      for (const auto& p : parts)
        if (p == kv.second) seen = true;
      if (!seen) parts.push_back(kv.second);
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
               "with_mesh=%zu\n",
               parts.size(), instances, with_mesh);
  for (const auto& o : parts) {
    const PartObject* part = static_cast<const PartObject*>(o.get());
    for (const PartObject::Inst& i : part->insts_) {
      if (!render_d3d9_mesh_ready(i.r)) continue;
      const PartTypeDesc& d = *part->type_;
      const PartEntry& e = d.entries[static_cast<size_t>(i.entry)];
      const RpakEntry* te = rpak_find_entry(d.type_id);
      const RpakEntry* re = rpak_find_entry(e.res_id);
      float px = 0.f, py = 0.f, pz = 0.f;
      render_d3d9_mesh_get_transform(i.r, &px, &py, &pz, nullptr, nullptr,
                                     nullptr, nullptr, nullptr, nullptr);
      const CamV3 world = part->pos_ + cam_mul(part->rot_, cam_v(px, py, pz));
      // The slot this part is installed through, and on what.
      char on[96];
      on[0] = 0;
      for (const PartObject::Slot& s : part->slots_)
        if (s.other && (s.flags & 1u)) {
          std::snprintf(on, sizeof(on), " installed: slot %d on %p slot %d",
                        s.id, static_cast<void*>(s.other), s.other_slot);
          break;
        }
      std::fprintf(out,
                   "[render-dbg] native part %p '%s' body=(%.4f, %.4f, %.4f) "
                   "ypr=(%.4f, %.4f, %.4f) mass_centre=(%.4f, %.4f, %.4f) "
                   "instance %p render 0x%08X '%s' local=(%.4f, %.4f, %.4f) "
                   "mesh_origin=(%.4f, %.4f, %.4f) sections=%d%s\n",
                   static_cast<void*>(part->key_), te ? te->name.c_str() : "",
                   part->pos_.x, part->pos_.y, part->pos_.z, part->ypr_[0],
                   part->ypr_[1], part->ypr_[2], d.com.x, d.com.y, d.com.z,
                   static_cast<void*>(i.r), static_cast<unsigned>(e.res_id),
                   re ? re->name.c_str() : "", px, py, pz, world.x, world.y,
                   world.z, render_d3d9_mesh_submesh_count(i.r), on);
    }
  }
  native_part_install_dump(out);
}
