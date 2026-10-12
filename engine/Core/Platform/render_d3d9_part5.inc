// Fragment 5 of render_d3d9.cpp - included, not compiled on its own (after
// fragments 1 to 4).
//
// M3 step 4: what a camera gives the nodes it walks, and meshes with bones.
//   camera_offer_detail          FUN_0045e910 -> FUN_00461980 (the camera
//                                side; the nodes are in NativeTypes_part8.inc)
//   ..._camera_walk_parent / ..._camera_sees_below
//                                the walk FUN_0045eb10, asked from the node
//   ..._mesh_load_bones          FUN_004515e0 (`<mesh>.bon`)
//   draw_two_bone_section        FUN_0051c990 (skin matrices), FUN_0046a2d0
//                                (the matrices of an instance), FUN_00468cd0
//                                / FUN_00444180 (two world matrices and one
//                                blend weight)
//   ..._mesh_counts, ..._mesh_erase_bone   host helpers

namespace inv {

// PE 940 FUN_0045e910 / FUN_00461980: a camera gives every node of its walk
// a detail value from its eye (cam+0x350), cot(aov / 2) (cam+0x4A0, written
// by FUN_00461b20 as cos / sin of aov x pi / 360), lodAmp (cam+0x4B0) and
// lodBias (cam+0x4AC). The nodes are the game objects of the walk
// (native_camera_detail, NativeTypes_part8.inc); the root is the one the
// pass draws from. Build 940 boot only.
static void camera_offer_detail(void* cam_key) {
#ifdef _WIN32
  auto it = g_cameras.find(cam_key);
  if (it == g_cameras.end()) return;
  const CameraState& cam = it->second;
  void* const start = cam.parent ? cam.parent : cam_key;
  void* const root = camera_root_of(start);
  if (!root) return;
  D3DMATRIX proj{}, cam_world{}, view{};
  float xs = 1.f, ys = 1.f, vp_aspect = 1.f;
  camera_pass_matrices(cam_key, cam, &cam_world, &view, &proj, &xs, &ys,
                       &vp_aspect);
  float aov = cam.aov_deg;
  if (!(aov > 0.5f)) aov = 0.5f;  // Fork: as build_camera_projection
  if (aov > 179.f) aov = 179.f;
  const float half = aov * 0.008726389f;  // DAT_00593a50 x DAT_00593590
  const float cot = std::cos(half) / std::sin(half);
  const float eye[3] = {cam_world.m[3][0], cam_world.m[3][1],
                        cam_world.m[3][2]};
  native_camera_detail(root, eye, cot, cam.lod_amp, cam.lod_bias,
                       g_cam_pass_frame);
#else
  (void)cam_key;
#endif
}

// One step up the chain a camera's walk comes down (chain_parent_of).
void* render_d3d9_camera_walk_parent(void* key) {
  return key ? chain_parent_of(key) : nullptr;
}

// True when the walk of a camera with that root reaches the children of
// `parent`: the root is `parent` or above it and no node from `parent` up
// to the root is a WORLDTREELEAF container (camera_sees, started one step
// up).
bool render_d3d9_camera_sees_below(void* root, void* parent) {
  if (!root || !parent) return false;
  bool leaf = false;
  void* cur = parent;
  for (int guard = 0; cur && guard < 64; ++guard) {
    if (gameref_get_flags(static_cast<InvObject*>(cur)) & kGameRefWorldTreeLeaf)
      leaf = true;
    if (cur == root) return !leaf;
    cur = chain_parent_of(cur);
  }
  return false;
}

void render_d3d9_mesh_counts(void* key, int32_t* verts, int32_t* tris) {
  int32_t v = 0, t = 0;
  auto it = g_meshes.find(key);
  if (it != g_meshes.end())
    for (const MeshSubmesh& sm : it->second.subs) {
      v += static_cast<int32_t>(sm.verts.size());
      t += static_cast<int32_t>(sm.indices.size() / 3);
    }
  if (verts) *verts = v;
  if (tris) *tris = t;
}

// The instance no longer poses that bone (the exe frees the controller's
// target, FUN_004eb4e0; here the bone goes back to "follows bone00").
void render_d3d9_mesh_erase_bone(void* key, int32_t bone_id) {
  if (!key || bone_id <= 0) return;
  auto it = g_mesh_bones.find(key);
  if (it != g_mesh_bones.end()) it->second.erase(bone_id);
}

// PE 940 FUN_004515e0: the bones of a mesh whose type record has `flags 8`
// (mesh+0x3C bit 0) come from the mesh path with its last four characters
// replaced by ".bon". Per bone: a name line, a parent index line ("%d"),
// four lines "%f,%f,%f" (three matrix rows and the position), three lines
// that are read past (limits). The record is 0xA4 bytes at mesh+0x120:
// +0x00 parent, +0x04 name[32], +0x24 the 4x4 bind matrix, position in
// the fourth row.
// Host: the position stays in file units (centimetres, like the vertices;
// the exe multiplies both by 0.1). Returns the number of bones; 0 when the
// file is missing (the mesh is then drawn rigid, as in the exe).
int32_t render_d3d9_mesh_load_bones(void* key) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end()) return 0;
  MeshState& ms = it->second;
  ms.bones.clear();
  if (ms.label.size() < 4) return 0;
  std::string path = ms.label;
  path.replace(path.size() - 4, 4, ".bon");
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    const std::string resolved = rpak_resolve_path(path.c_str());
    if (!resolved.empty()) f = std::fopen(resolved.c_str(), "rb");
  }
  if (!f) return 0;
  std::string text;
  char buf[4096];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
  std::fclose(f);
  std::vector<std::string> lines;
  for (size_t pos = 0; pos < text.size();) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    std::string line = text.substr(pos, eol - pos);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                             line.back() == '\t'))
      line.pop_back();
    lines.push_back(line);
    pos = eol + 1;
  }
  for (size_t i = 0; i + 5 < lines.size(); i += 9) {
    if (lines[i].empty()) break;
    MeshBone b;
    b.name = lines[i].substr(0, 31);
    std::sscanf(lines[i + 1].c_str(), "%d", &b.parent);
    std::memset(b.bind, 0, sizeof b.bind);
    for (int r = 0; r < 4; ++r) {
      float x = 0.f, y = 0.f, z = 0.f;
      std::sscanf(lines[i + 2 + static_cast<size_t>(r)].c_str(), "%f,%f,%f",
                  &x, &y, &z);
      b.bind[r * 4 + 0] = x;
      b.bind[r * 4 + 1] = y;
      b.bind[r * 4 + 2] = z;
    }
    b.bind[15] = 1.f;
    ms.bones.push_back(b);
  }
  return static_cast<int32_t>(ms.bones.size());
}

int32_t render_d3d9_mesh_bone_count(void* key) {
  auto it = g_meshes.find(key);
  return it == g_meshes.end() ? 0
                              : static_cast<int32_t>(it->second.bones.size());
}

// D3DCAPS9::MaxVertexBlendMatrices of the device, -1 until asked.
static int g_blend_matrices = -1;

#ifdef _WIN32
// A vertex with one blend weight: the 36-byte layout of the v4 files with
// format 0x245 (position, weight, normal, uv). FVF XYZB1 | NORMAL | TEX1.
struct MeshVertexBlend {
  float x, y, z;
  float w;
  float nx, ny, nz;
  float u, v;
};
static_assert(sizeof(MeshVertexBlend) == 36, "blend vertex is 9 dwords");
// The same with a second uv pair (the second texture entry of a MixTA
// class, render_d3d9_part6.inc). FVF XYZB1 | NORMAL | TEX2.
struct MeshVertexBlend2 {
  float x, y, z;
  float w;
  float nx, ny, nz;
  float uv[2][2];
};
static_assert(sizeof(MeshVertexBlend2) == 44, "blend vertex is 11 dwords");

// PE 940 FUN_0051c990 / FUN_0046a2d0 / FUN_00468cd0 / FUN_00444180: a
// section with two bones (INVO v4 section chunk type 2) of an instance
// whose mesh has bones.
//   - The instance update replaces the world matrix of the controller named
//     like mesh bone i by inverse(bind_i) x world (asm 0x51cef0..0x51cf5e:
//     FUN_00567630 inverts the bind matrix, FUN_0046b020 multiplies, row
//     vectors).
//   - The camera-side instance hands the group one matrix per mesh bone;
//     when the instance has fewer controllers than the mesh has bones
//     every matrix is bone00's.
//   - The material (mode 1) sets D3DTS_WORLDMATRIX(0) = matrix[boneA],
//     D3DTS_WORLDMATRIX(1) = matrix[boneB] and D3DRS_VERTEXBLEND =
//     D3DVBF_1WEIGHTS; the vertex is {position, weight, normal, uv}.
// Host: `world` is the matrix of bone00 as the caller drew it (the
// instance's world with the file unit in its 3x3). Another bone's world is
// its local pose times the instance's world (g_mesh_bones, see
// MeshXform). Returns false when the section has to be drawn rigid: the
// mesh has no bones, or the instance poses none but bone00.
// Fork: with fewer than two blend matrices the exe switches the device to
// software vertex processing; the host blends on the CPU.
// The matrices of the section's two bones (see draw_two_bone_section).
static bool two_bone_matrices(void* key, const MeshState& ms,
                              const MeshSubmesh& sm, const D3DMATRIX& world,
                              D3DMATRIX* out_a, D3DMATRIX* out_b) {
  if (sm.bone_mode != 1 || sm.bone_idx.size() < 2 || ms.bones.size() < 2 ||
      sm.weights.size() != sm.verts.size() || sm.indices.size() < 3)
    return false;
  auto ait = g_mesh_bone_alias.find(key);
  auto bit = g_mesh_bones.find(key);
  // One matrix per mesh bone.
  std::vector<D3DMATRIX> skin(ms.bones.size());
  bool posed = false;
  D3DMATRIX inst_world{};
  resolve_world(key, &inst_world, 0);
  for (size_t i = 0; i < ms.bones.size(); ++i) {
    const MeshBone& b = ms.bones[i];
    const MeshXform* local = nullptr;
    const bool is_root = b.name == "bone00";
    if (!is_root) {
      if (ait == g_mesh_bone_alias.end() || bit == g_mesh_bones.end())
        return false;
      auto id = ait->second.find(b.name);
      if (id == ait->second.end()) return false;
      auto lb = bit->second.find(id->second);
      if (lb == bit->second.end()) return false;
      local = &lb->second;
      posed = true;
    }
    D3DMATRIX bind{}, inv{};
    std::memcpy(&bind, b.bind, sizeof bind);
    if (!invert_general(bind, &inv)) mat_identity(&inv);
    D3DMATRIX bone_world = world;
    if (local) {
      // inverse(bind) x unit x local x instance world.
      D3DMATRIX l{}, lw{};
      build_local_world(*local, &l);
      mat_mul(l, inst_world, &lw);
      D3DMATRIX s{};
      mat_identity(&s);
      s.m[0][0] = s.m[1][1] = s.m[2][2] = ms.unit;
      mat_mul(s, lw, &bone_world);
    }
    mat_mul(inv, bone_world, &skin[i]);
  }
  if (!posed) return false;
  const size_t ia = static_cast<size_t>(sm.bone_idx[0]);
  const size_t ib = static_cast<size_t>(sm.bone_idx[1]);
  if (ia >= skin.size() || ib >= skin.size()) return false;
  *out_a = skin[ia];
  *out_b = skin[ib];
  return true;
}

// `two_sets`: the vertices carry the first two uv sets of the wide array
// (the caller has set the material sources for the CPU path, whose vertex
// then has the two colours of the wide format).
static bool draw_two_bone_section(void* key, const MeshState& ms,
                                  const MeshSubmesh& sm,
                                  const D3DMATRIX& world, bool two_sets) {
  D3DMATRIX skin_a{}, skin_b{};
  if (!two_bone_matrices(key, ms, sm, world, &skin_a, &skin_b)) return false;
  const UINT nvert = static_cast<UINT>(sm.verts.size());
  const UINT ntri = static_cast<UINT>(sm.indices.size() / 3);
  if (sm.wide.size() != sm.verts.size()) two_sets = false;
  if (g_blend_matrices < 0) {
    D3DCAPS9 caps{};
    g_blend_matrices = SUCCEEDED(g_dev->GetDeviceCaps(&caps))
                           ? static_cast<int>(caps.MaxVertexBlendMatrices)
                           : 0;
  }
  ++g_cam_pass_stats.two_bone_sections;
  if (g_blend_matrices >= 2 && two_sets) {
    static std::vector<MeshVertexBlend2> s_bv2;
    s_bv2.resize(nvert);
    for (UINT i = 0; i < nvert; ++i) {
      const MeshVertexWide& v = sm.wide[i];
      MeshVertexBlend2& o = s_bv2[i];
      o.x = v.x;
      o.y = v.y;
      o.z = v.z;
      o.w = sm.weights[i];
      o.nx = v.nx;
      o.ny = v.ny;
      o.nz = v.nz;
      std::memcpy(o.uv, v.uv, sizeof(o.uv));
    }
    g_dev->SetTransform(D3DTS_WORLDMATRIX(0), &skin_a);
    g_dev->SetTransform(D3DTS_WORLDMATRIX(1), &skin_b);
    g_dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_1WEIGHTS);
    g_dev->SetFVF(D3DFVF_XYZB1 | D3DFVF_NORMAL | D3DFVF_TEX2);
    g_dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, nvert, ntri,
                                  sm.indices.data(), D3DFMT_INDEX16,
                                  s_bv2.data(), sizeof(MeshVertexBlend2));
    g_dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
    g_dev->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1);
    g_dev->SetTransform(D3DTS_WORLD, &world);
    return true;
  }
  if (g_blend_matrices >= 2) {
    static std::vector<MeshVertexBlend> s_bv;
    s_bv.resize(nvert);
    for (UINT i = 0; i < nvert; ++i) {
      const MeshVertex& v = sm.verts[i];
      MeshVertexBlend& o = s_bv[i];
      o.x = v.x;
      o.y = v.y;
      o.z = v.z;
      o.w = sm.weights[i];
      o.nx = v.nx;
      o.ny = v.ny;
      o.nz = v.nz;
      o.u = v.u;
      o.v = v.v;
    }
    g_dev->SetTransform(D3DTS_WORLDMATRIX(0), &skin_a);
    g_dev->SetTransform(D3DTS_WORLDMATRIX(1), &skin_b);
    g_dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_1WEIGHTS);
    g_dev->SetFVF(D3DFVF_XYZB1 | D3DFVF_NORMAL | D3DFVF_TEX1);
    g_dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, nvert, ntri,
                                  sm.indices.data(), D3DFMT_INDEX16,
                                  s_bv.data(), sizeof(MeshVertexBlend));
    g_dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
    g_dev->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1);
    g_dev->SetTransform(D3DTS_WORLD, &world);
    return true;
  }
  // The same sum on the CPU, drawn under an identity world matrix.
  static std::vector<MeshVertex> s_cv;
  s_cv.resize(nvert);
  const D3DMATRIX& a = skin_a;
  const D3DMATRIX& b = skin_b;
  for (UINT i = 0; i < nvert; ++i) {
    const MeshVertex& v = sm.verts[i];
    const float w = sm.weights[i], u = 1.f - w;
    MeshVertex& o = s_cv[i];
    o = v;
    o.x = w * (v.x * a.m[0][0] + v.y * a.m[1][0] + v.z * a.m[2][0] + a.m[3][0]) +
          u * (v.x * b.m[0][0] + v.y * b.m[1][0] + v.z * b.m[2][0] + b.m[3][0]);
    o.y = w * (v.x * a.m[0][1] + v.y * a.m[1][1] + v.z * a.m[2][1] + a.m[3][1]) +
          u * (v.x * b.m[0][1] + v.y * b.m[1][1] + v.z * b.m[2][1] + b.m[3][1]);
    o.z = w * (v.x * a.m[0][2] + v.y * a.m[1][2] + v.z * a.m[2][2] + a.m[3][2]) +
          u * (v.x * b.m[0][2] + v.y * b.m[1][2] + v.z * b.m[2][2] + b.m[3][2]);
    // The bone matrices are a rotation and one scale: the normal goes
    // through the same 3x3 (the draw normalises it).
    o.nx = w * (v.nx * a.m[0][0] + v.ny * a.m[1][0] + v.nz * a.m[2][0]) +
           u * (v.nx * b.m[0][0] + v.ny * b.m[1][0] + v.nz * b.m[2][0]);
    o.ny = w * (v.nx * a.m[0][1] + v.ny * a.m[1][1] + v.nz * a.m[2][1]) +
           u * (v.nx * b.m[0][1] + v.ny * b.m[1][1] + v.nz * b.m[2][1]);
    o.nz = w * (v.nx * a.m[0][2] + v.ny * a.m[1][2] + v.nz * a.m[2][2]) +
           u * (v.nx * b.m[0][2] + v.ny * b.m[1][2] + v.nz * b.m[2][2]);
  }
  D3DMATRIX ident{};
  mat_identity(&ident);
  g_dev->SetTransform(D3DTS_WORLD, &ident);
  if (two_sets) {
    // The blended position and normal in the wide vertex.
    static std::vector<MeshVertexWide> s_cw;
    s_cw.resize(nvert);
    for (UINT i = 0; i < nvert; ++i) {
      MeshVertexWide& o = s_cw[i];
      o = sm.wide[i];
      o.x = s_cv[i].x;
      o.y = s_cv[i].y;
      o.z = s_cv[i].z;
      o.nx = s_cv[i].nx;
      o.ny = s_cv[i].ny;
      o.nz = s_cv[i].nz;
    }
    g_dev->SetFVF(kFvfWide);
    g_dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, nvert, ntri,
                                  sm.indices.data(), D3DFMT_INDEX16,
                                  s_cw.data(), sizeof(MeshVertexWide));
    g_dev->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1);
  } else {
    g_dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, nvert, ntri,
                                  sm.indices.data(), D3DFMT_INDEX16,
                                  s_cv.data(), sizeof(MeshVertex));
  }
  g_dev->SetTransform(D3DTS_WORLD, &world);
  return true;
}
#endif

// `[render-dbg]`: the meshes with bones and the two-bone sections of the
// last flushed frame; then one line per instance that is drawn with two
// bone matrices: the world position of its two bones, the box of its
// blended vertices, and how far they reach along the line from bone00 to
// bone01 (`along`, metres from bone00) and away from it (`radius`).
void render_d3d9_bone_debug_dump(FILE* out) {
  if (!out) return;
  size_t with_bones = 0, sections = 0;
  for (const auto& kv : g_meshes) {
    if (!kv.second.bones.empty()) ++with_bones;
    for (const MeshSubmesh& sm : kv.second.subs)
      if (sm.bone_mode != 0) ++sections;
  }
  if (!with_bones && !sections) return;
  std::fprintf(out,
               "[render-dbg] bones: %zu mesh(es) with a .bon skeleton, %zu "
               "section(s) with a bone mode, two_bone_sections drawn in the "
               "last frame=%d, device blend matrices=%d (%s)\n",
               with_bones, sections, g_cam_pass_stats.two_bone_sections,
               g_blend_matrices,
               g_blend_matrices < 0
                   ? "not asked"
                   : g_blend_matrices >= 2 ? "D3DRS_VERTEXBLEND" : "CPU blend");
#ifdef _WIN32
  std::vector<std::pair<uint32_t, void*>> keys;
  for (const auto& kv : g_meshes)
    if (kv.second.ready && !kv.second.bones.empty())
      keys.emplace_back(kv.second.seq, kv.first);
  std::sort(keys.begin(), keys.end());
  const bool saved_fold = g_fold_own_bone0;
  g_fold_own_bone0 = true;  // as the camera pass draws
  for (const auto& k : keys) {
    const MeshState& ms = g_meshes[k.second];
    D3DMATRIX inst{};
    resolve_world(k.second, &inst, 0);
    D3DMATRIX world = inst;
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) world.m[r][c] *= ms.unit;
    for (const MeshSubmesh& sm : ms.subs) {
      D3DMATRIX a{}, b{};
      if (!two_bone_matrices(k.second, ms, sm, world, &a, &b)) continue;
      // bone00 and bone01 in the world: bone00 is the instance, the other
      // one its local pose under it.
      float p0[3] = {inst.m[3][0], inst.m[3][1], inst.m[3][2]};
      float p1[3] = {p0[0], p0[1], p0[2]};
      auto ait = g_mesh_bone_alias.find(k.second);
      auto bit = g_mesh_bones.find(k.second);
      if (ait != g_mesh_bone_alias.end() && bit != g_mesh_bones.end()) {
        auto id = ait->second.find("bone01");
        if (id != ait->second.end()) {
          auto lb = bit->second.find(id->second);
          if (lb != bit->second.end()) {
            D3DMATRIX l{}, lw{};
            build_local_world(lb->second, &l);
            mat_mul(l, inst, &lw);
            p1[0] = lw.m[3][0];
            p1[1] = lw.m[3][1];
            p1[2] = lw.m[3][2];
          }
        }
      }
      float ax[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
      const float len = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
      if (len > 0.f)
        for (float& v : ax) v /= len;
      float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
      float along_lo = 0.f, along_hi = 0.f, radius = 0.f;
      for (size_t i = 0; i < sm.verts.size(); ++i) {
        const MeshVertex& v = sm.verts[i];
        const float w = sm.weights[i], u = 1.f - w;
        float p[3];
        for (int c = 0; c < 3; ++c)
          p[c] = w * (v.x * a.m[0][c] + v.y * a.m[1][c] + v.z * a.m[2][c] + a.m[3][c]) +
                 u * (v.x * b.m[0][c] + v.y * b.m[1][c] + v.z * b.m[2][c] + b.m[3][c]);
        const float d[3] = {p[0] - p0[0], p[1] - p0[1], p[2] - p0[2]};
        const float t = d[0] * ax[0] + d[1] * ax[1] + d[2] * ax[2];
        const float r2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - t * t;
        const float r = r2 > 0.f ? std::sqrt(r2) : 0.f;
        for (int c = 0; c < 3; ++c) {
          if (i == 0 || p[c] < lo[c]) lo[c] = p[c];
          if (i == 0 || p[c] > hi[c]) hi[c] = p[c];
        }
        if (i == 0 || t < along_lo) along_lo = t;
        if (i == 0 || t > along_hi) along_hi = t;
        if (r > radius) radius = r;
      }
      std::fprintf(out,
                   "[render-dbg]   two-bone instance %p '%s' bone00=(%.4f, "
                   "%.4f, %.4f) bone01=(%.4f, %.4f, %.4f) apart=%.4f "
                   "box=(%.4f, %.4f, %.4f)..(%.4f, %.4f, %.4f) along=%.4f..%.4f "
                   "radius=%.4f verts=%zu\n",
                   k.second, sm.name.c_str(), p0[0], p0[1], p0[2], p1[0], p1[1],
                   p1[2], len, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2],
                   along_lo, along_hi, radius, sm.verts.size());
    }
  }
  g_fold_own_bone0 = saved_fold;
#endif
}

}  // namespace inv
