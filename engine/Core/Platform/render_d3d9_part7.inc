// Fragment 7 of render_d3d9.cpp - included, not compiled on its own (after
// fragments 1 to 6).
//
// M3 step 7: the projected shadows of a camera pass. Build 940 boot only.
//   render_d3d9_set_shadow_options  Config.shadows / shadow_size /
//                              shadow_detail as the engine holds them
//                              (DAT_005da018, DAT_005da01c, DAT_005e8ca4)
//   shadow_target, shadow_ramp FUN_00421280 (the render target), FUN_0042ce00
//                              (the 256x1 height ramp)
//   shadow_view                FUN_00423df0
//   shadow_light_direction     FUN_00459e00 with FUN_0045ae50
//   draw_camera_shadows        FUN_00460ba0 (place in the pass),
//                              FUN_0045cb10 (bounds of an owner),
//                              FUN_0045c7c0 (detail), FUN_0045c6a0 and
//                              FUN_0044b9b0 (target, view, texture matrix),
//                              FUN_0044a850 / FUN_00468310 / FUN_0044b2f0 /
//                              FUN_0044b0d0 (the casters), FUN_0045b150 with
//                              FUN_004335f0 and FUN_00440ec0 (the receivers)
//   shadow_geom                host: static buffers for the casters
//   render_d3d9_shadow_debug_dump   host: `[render-dbg]` lines
//
// The exe, per camera pass (FUN_00460ba0), after the opaque groups and
// before the transparent ones, when Config.shadows is not 0: for every
// "sector" that has casters,
//   - a sector is the set of render instances with one owner id (RI+0x98 =
//     payload+0x1B8): the node id of a part or car, and the car's id for
//     every part installed on it (FUN_004ca390);
//   - its bounds are rebuilt every frame (FUN_0045cb10) as the box around
//     the cubes (centre +- radius) of its instances that are not hidden,
//     where an instance's bounds are a sphere at its position whose radius
//     is the longer of |min| and |max| of the mesh (FUN_00469990; the mesh
//     loaders FUN_00451b00 / FUN_00452960 store that sphere);
//   - every section of an instance whose render type has flag 8 is a
//     caster (RI_object ctor FUN_00465e40);
//   - the casters are drawn into a square render target from the light
//     direction, the ground is drawn again with that texture projected
//     onto it, a height ramp in stage 1 and dest = dest x (1 - src).
// Host: the instances a camera pass draws (its `members`) are grouped by
// the owner their part or car object has (native_render_owner).
// Fork: units. The exe works in its engine units here (metres x 10: centre
// and radius x 10.0 in FUN_00459e00, the projection planes -100 / 100); the
// host world is metres, so the planes are -10 / 10 and the ratios are the
// same.
// Fork: the ground. The exe draws a copy of the ground mesh made for this
// (mesh+0x128, positions and normals of all its triangles, one material
// CMaterial_ProjectedShadowTexture); the host draws the POLY sections of
// rg_ground again.
// Fork: lights. The exe selects lights per sector (FUN_0045a2c0); the host
// has one selection per camera pass (collect_camera_lights).
// Not ported: the other two shadow kinds of a sector, FUN_0045c390 (a
// render object with `shd_texture`) and FUN_0045b530 (the textured quad of
// `shd_center` / `shd_diru` / `shd_dirv`); no render object of the install
// has an active `shd_texture` line.

namespace inv {

namespace {

// PE 940 DAT_005da018 / DAT_005da01c / DAT_005e8ca4. The values here are
// the field initialisers of Config.class.
int32_t g_shadow_on = 2;
int32_t g_shadow_size = 256;
float g_shadow_detail = 0.25f;
std::string g_shadow_source = "class defaults";

// One record per owner of a camera pass of the frame being built
// (render_d3d9_shadow_debug_dump prints the last flushed frame).
struct ShadowRec {
  int16_t pass = 0;
  int32_t owner = 0;
  int32_t instances = 0;  // of the owner, with bounds
  int32_t casters = 0;    // instances with render type flag 8
  int32_t sections = 0;   // caster sections drawn
  int32_t triangles = 0;
  int32_t receivers = 0;  // ground sections drawn with the projection
  float centre[3] = {0, 0, 0};
  float radius = 0.f;
  float dir[3] = {0, 0, 0};
  float detail = 0.f;
  bool drawn = false;
  std::string first;  // mesh of the first caster
};
struct ShadowPassRec {
  int16_t pass = 0;
  int32_t owners = 0;  // with at least one caster
  int32_t drawn = 0;
  int32_t below_detail = 0;
  int32_t no_owner = 0;  // flag 8 instances that have no owner
  int32_t ground_sections = 0;
  int32_t lights = 0;
};
std::vector<ShadowRec> g_shadow_recs;
std::vector<ShadowPassRec> g_shadow_passes;
int32_t g_shadow_skip_ground = 0;  // passes without a ground section
int32_t g_shadow_skip_light = 0;   // passes without a device light

#ifdef _WIN32
IDirect3DTexture9* g_shadow_target = nullptr;
IDirect3DTexture9* g_shadow_ramp = nullptr;
IDirect3DDevice9* g_shadow_dev = nullptr;  // the device both were made on
int32_t g_shadow_target_size = 0;
D3DFORMAT g_shadow_target_fmt = D3DFMT_UNKNOWN;
bool g_shadow_target_failed = false;

// Fork: static buffers for the casters. The exe draws a caster section
// from the vertex and index buffers the mesh section owns (FUN_0044b2f0);
// the host keeps the sections of a mesh in system memory and the camera
// groups send them with DrawIndexedPrimitiveUP. The caster pass reads
// positions alone, so the positions and the indices of a mesh are put
// into one vertex and one index buffer the first time the mesh casts, and
// every instance with the same geometry (MeshState::geom_rev: the
// instances cloned from one render type) is drawn from them: the same
// triangles in the same order, one DrawIndexedPrimitive per section. A
// mesh without a stamp, or one whose buffers the device refuses, is sent
// from system memory as before. Buffers no caster used for kShadowGeomKeep
// frames are released, and all of them with the device.
struct ShadowGeom {
  struct Range {
    UINT base_vertex, verts, first_index, tris;
  };
  IDirect3DVertexBuffer9* vb = nullptr;  // D3DFVF_XYZ
  IDirect3DIndexBuffer9* ib = nullptr;   // 16-bit, each section from 0
  std::vector<Range> ranges;             // the sections with geometry
  uint32_t used = 0;                     // g_cam_pass_frame of the last use
  bool failed = false;
};
std::unordered_map<uint32_t, ShadowGeom> g_shadow_geom;  // by geom_rev
constexpr uint32_t kShadowGeomKeep = 1024;

void shadow_geom_free(ShadowGeom& g) {
  if (g.vb) g.vb->Release();
  if (g.ib) g.ib->Release();
  g.vb = nullptr;
  g.ib = nullptr;
}
#endif

void shadow_release() {
#ifdef _WIN32
  for (auto& kv : g_shadow_geom) shadow_geom_free(kv.second);
  g_shadow_geom.clear();
  if (g_shadow_target) g_shadow_target->Release();
  if (g_shadow_ramp) g_shadow_ramp->Release();
  g_shadow_target = nullptr;
  g_shadow_ramp = nullptr;
  g_shadow_dev = nullptr;
  g_shadow_target_size = 0;
  g_shadow_target_fmt = D3DFMT_UNKNOWN;
  g_shadow_target_failed = false;
#endif
}

#ifdef _WIN32
// PE 940 FUN_00421280: when Config.shadows is not 0, two render-target
// textures of shadow_size x shadow_size, C3DLow vt+0x134(size, 0) =
// vt+0xEC(size, size, 1, 9): one level, D3DUSAGE_RENDERTARGET,
// D3DPOOL_DEFAULT (FUN_0044cf50), engine format 9, which the format table
// at 0x5DA278 starts as D3DFMT_A8R8G8B8.
// Fork: one target (the exe hands shadowTarget[DAT_005e8c90] to every
// sector of a pass; the sectors are drawn one after the other). Made when
// the first pass needs it. X8R8G8B8 when the device refuses A8R8G8B8 (the
// exe's FUN_00424e20 rewrites the table entry from what the device
// supports; not read).
IDirect3DTexture9* shadow_target() {
  if (!g_dev || g_shadow_on == 0 || g_shadow_size <= 0) return nullptr;
  if (g_shadow_dev != g_dev) shadow_release();
  if (g_shadow_target && g_shadow_target_size == g_shadow_size)
    return g_shadow_target;
  if (g_shadow_target_failed && g_shadow_target_size == g_shadow_size)
    return nullptr;
  if (g_shadow_target) g_shadow_target->Release();
  g_shadow_target = nullptr;
  g_shadow_dev = g_dev;
  g_shadow_target_size = g_shadow_size;
  g_shadow_target_failed = false;
  const UINT n = static_cast<UINT>(g_shadow_size);
  for (D3DFORMAT fmt : {D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8}) {
    IDirect3DTexture9* tex = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture(n, n, 1, D3DUSAGE_RENDERTARGET, fmt,
                                       D3DPOOL_DEFAULT, &tex, nullptr)) &&
        tex) {
      g_shadow_target = tex;
      g_shadow_target_fmt = fmt;
      return tex;
    }
  }
  g_shadow_target_failed = true;
  std::fprintf(stderr,
               "[render] shadows: no %dx%d render target; the pass is off\n",
               g_shadow_size, g_shadow_size);
  return nullptr;
}

// The static buffers of a caster mesh (see ShadowGeom); null when the mesh
// is to be sent from system memory.
ShadowGeom* shadow_geom(const MeshState& ms) {
  if (ms.geom_rev == 0 || !g_dev || g_shadow_dev != g_dev) return nullptr;
  auto it = g_shadow_geom.find(ms.geom_rev);
  if (it != g_shadow_geom.end()) {
    it->second.used = g_cam_pass_frame;
    return it->second.failed ? nullptr : &it->second;
  }
  ShadowGeom g;
  g.used = g_cam_pass_frame;
  size_t nv = 0, ni = 0;
  for (const MeshSubmesh& sm : ms.subs) {
    if (sm.verts.empty() || sm.indices.size() < 3) continue;
    const size_t tris = sm.indices.size() / 3;
    g.ranges.push_back({static_cast<UINT>(nv), static_cast<UINT>(sm.verts.size()),
                        static_cast<UINT>(ni), static_cast<UINT>(tris)});
    nv += sm.verts.size();
    ni += tris * 3;
  }
  bool ok = nv != 0 && ni != 0;
  if (ok)
    ok = SUCCEEDED(g_dev->CreateVertexBuffer(
             static_cast<UINT>(nv * 3 * sizeof(float)), D3DUSAGE_WRITEONLY,
             D3DFVF_XYZ, D3DPOOL_MANAGED, &g.vb, nullptr)) &&
         g.vb;
  if (ok)
    ok = SUCCEEDED(g_dev->CreateIndexBuffer(
             static_cast<UINT>(ni * sizeof(uint16_t)), D3DUSAGE_WRITEONLY,
             D3DFMT_INDEX16, D3DPOOL_MANAGED, &g.ib, nullptr)) &&
         g.ib;
  if (ok) {
    void* vp = nullptr;
    void* ip = nullptr;
    ok = SUCCEEDED(g.vb->Lock(0, 0, &vp, 0)) && vp;
    if (ok) {
      float* dst = static_cast<float*>(vp);
      for (const MeshSubmesh& sm : ms.subs) {
        if (sm.verts.empty() || sm.indices.size() < 3) continue;
        for (const MeshVertex& v : sm.verts) {
          *dst++ = v.x;
          *dst++ = v.y;
          *dst++ = v.z;
        }
      }
      g.vb->Unlock();
      ok = SUCCEEDED(g.ib->Lock(0, 0, &ip, 0)) && ip;
    }
    if (ok) {
      uint16_t* dst = static_cast<uint16_t*>(ip);
      for (const MeshSubmesh& sm : ms.subs) {
        if (sm.verts.empty() || sm.indices.size() < 3) continue;
        const size_t n = sm.indices.size() / 3 * 3;
        std::memcpy(dst, sm.indices.data(), n * sizeof(uint16_t));
        dst += n;
      }
      g.ib->Unlock();
    }
  }
  if (!ok) {
    shadow_geom_free(g);
    g.ranges.clear();
    g.failed = true;  // not tried again for this geometry
  }
  ShadowGeom& kept = g_shadow_geom[ms.geom_rev];
  kept = std::move(g);
  return kept.failed ? nullptr : &kept;
}

// PE 940 FUN_0042ce00: the texture at DAT_005eeccc, C3DLow vt+0xEC(0x100,
// 1, 1, 3) = 256x1, one level, engine format 3. Texel i: every byte 4i
// below 64, 0xFFFFFFFF up to 127, every byte 16 x (143 - i) up to 143,
// then 0.
// Fork: engine format 3 is taken as A8R8G8B8, as for the default texture.
IDirect3DTexture9* shadow_ramp() {
  if (!g_dev) return nullptr;
  if (g_shadow_dev != g_dev) shadow_release();
  if (g_shadow_ramp) return g_shadow_ramp;
  g_shadow_dev = g_dev;
  IDirect3DTexture9* tex = nullptr;
  if (FAILED(g_dev->CreateTexture(256, 1, 1, 0, D3DFMT_A8R8G8B8,
                                  D3DPOOL_MANAGED, &tex, nullptr)) ||
      !tex)
    return nullptr;
  D3DLOCKED_RECT lr{};
  if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) {
    tex->Release();
    return nullptr;
  }
  uint32_t row[256];
  for (uint32_t i = 0; i < 256; ++i) {
    if (i < 0x40) {
      row[i] = ((((i << 8 | i) << 8 | i) << 8) | i) << 2;
    } else if (i < 0x80) {
      row[i] = 0xFFFFFFFFu;
    } else if (i < 0x90) {
      const uint32_t k = 0x8F - i;
      row[i] = (~i << 28) | (k * 0x1000u) | (k * 0x10u) | (k * 0x100000u);
    } else {
      row[i] = 0;
    }
  }
  std::memcpy(lr.pBits, row, sizeof(row));
  tex->UnlockRect(0);
  g_shadow_ramp = tex;
  return tex;
}

// PE 940 FUN_00423df0(matrix, x, y, z): the rotation that looks along a
// direction. right = (-z, 0, x) normalised ((1, 0, 0) when x and z are
// both below 1e-5), n = the direction normalised, up = right x n
// normalised; a row vector p comes out as (p.right, p.up, p.n).
void shadow_view(const float d[3], D3DMATRIX* out) {
  float rx = -d[2];
  if (std::fabs(static_cast<double>(rx)) < 1e-5 &&  // DAT_00593e30
      std::fabs(static_cast<double>(d[0])) < 1e-5)
    rx = 1.f;
  float len = std::sqrt(rx * rx + d[0] * d[0]);
  float inv = len != 0.f ? 1.f / len : 0.f;
  const float r0 = inv * rx, r1 = inv * 0.f, r2 = inv * d[0];
  len = std::sqrt(d[1] * d[1] + d[0] * d[0] + d[2] * d[2]);
  inv = len != 0.f ? 1.f / len : 0.f;
  const float n0 = inv * d[0], n1 = inv * d[1], n2 = inv * d[2];
  float u0 = n2 * r1 - n1 * r2;
  float u1 = n0 * r2 - n2 * r0;
  float u2 = n1 * r0 - n0 * r1;
  len = std::sqrt(u0 * u0 + u1 * u1 + u2 * u2);  // D3DXVec3Normalize
  if (len != 0.f) {
    u0 /= len;
    u1 /= len;
    u2 /= len;
  }
  D3DMATRIX m{};
  m.m[0][0] = r0;
  m.m[1][0] = r1;
  m.m[2][0] = r2;
  m.m[0][1] = u0;
  m.m[1][1] = u1;
  m.m[2][1] = u2;
  m.m[0][2] = n0;
  m.m[1][2] = n1;
  m.m[2][2] = n2;
  m.m[3][3] = 1.f;
  *out = m;
}

// PE 940 FUN_00459e00: the direction a sector's shadow is cast along, the
// normalised sum over its selected lights of FUN_0045ae50's direction
// times weight: a directional light gives its direction and |Diffuse rgb|;
// a point light the direction from the light to the sector centre and
// |Diffuse rgb| / (Attenuation0 + Attenuation1 x d + Attenuation2 x d x
// d); a spot light the same, and no direction when the centre is behind it.
// Host: the device lights of the camera pass (their positions and
// attenuation factors are in metres, scene_light_record).
// Fork: a point or spot light whose attenuation sum is not positive adds
// nothing (the exe divides by it).
void shadow_light_direction(const CamLights& lights, const float centre[3],
                            float out[3]) {
  float sum[3] = {0.f, 0.f, 0.f};
  for (const D3DLIGHT9& l : lights.device) {
    float d[3] = {0.f, 0.f, 0.f};
    float att = 1.f;
    if (l.Type == D3DLIGHT_DIRECTIONAL) {
      d[0] = l.Direction.x;
      d[1] = l.Direction.y;
      d[2] = l.Direction.z;
      const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      if (len != 0.f)
        for (float& c : d) c /= len;
    } else {
      d[0] = centre[0] - l.Position.x;
      d[1] = centre[1] - l.Position.y;
      d[2] = centre[2] - l.Position.z;
      const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      if (dist != 0.f)
        for (float& c : d) c /= dist;
      if (l.Type == D3DLIGHT_SPOT &&
          d[1] * l.Direction.y + d[0] * l.Direction.x + d[2] * l.Direction.z <=
              0.f)
        d[0] = d[1] = d[2] = 0.f;
      const float den = l.Attenuation1 * dist + l.Attenuation0 +
                        dist * dist * l.Attenuation2;
      att = den > 0.f ? 1.f / den : 0.f;
    }
    const float w = std::sqrt(l.Diffuse.g * l.Diffuse.g +
                              l.Diffuse.r * l.Diffuse.r +
                              l.Diffuse.b * l.Diffuse.b) *
                    att;
    for (int i = 0; i < 3; ++i) sum[i] += d[i] * w;
  }
  const float len =
      std::sqrt(sum[1] * sum[1] + sum[0] * sum[0] + sum[2] * sum[2]);
  if (len != 0.f)
    for (float& c : sum) c /= len;
  for (int i = 0; i < 3; ++i) out[i] = sum[i];
}

// What the pass changes on the device and puts back when it ends. (The exe
// restores its own list in FUN_0044b0d0 and in the restore branch of
// FUN_004335f0, and resets its whole state block once a frame.)
struct ShadowScope {
  static constexpr size_t kNumRs = 17;
  static constexpr size_t kNumTss = 8;
  static constexpr size_t kNumSamp = 6;
  DWORD rs[kNumRs] = {};
  DWORD tss[2][kNumTss] = {};
  DWORD tss2_colorop = D3DTOP_DISABLE;
  DWORD samp[2][kNumSamp] = {};
  D3DMATRIX world{}, view{}, proj{}, tex0{}, tex1{};
  D3DMATERIAL9 material{};
  D3DVIEWPORT9 viewport{};
  DWORD fvf = 0;
  IDirect3DSurface9* rt = nullptr;
  IDirect3DSurface9* ds = nullptr;

  static const D3DRENDERSTATETYPE* rs_list() {
    static const D3DRENDERSTATETYPE k[kNumRs] = {
        D3DRS_ZENABLE,                D3DRS_ZFUNC,
        D3DRS_ZWRITEENABLE,           D3DRS_LIGHTING,
        D3DRS_ALPHABLENDENABLE,       D3DRS_SRCBLEND,
        D3DRS_DESTBLEND,              D3DRS_BLENDOP,
        D3DRS_ALPHATESTENABLE,        D3DRS_TEXTUREFACTOR,
        D3DRS_CULLMODE,               D3DRS_SCISSORTESTENABLE,
        D3DRS_SPECULARENABLE,         D3DRS_DIFFUSEMATERIALSOURCE,
        D3DRS_SPECULARMATERIALSOURCE, D3DRS_AMBIENTMATERIALSOURCE,
        D3DRS_EMISSIVEMATERIALSOURCE};
    return k;
  }
  static const D3DTEXTURESTAGESTATETYPE* tss_list() {
    static const D3DTEXTURESTAGESTATETYPE k[kNumTss] = {
        D3DTSS_COLOROP,       D3DTSS_COLORARG1, D3DTSS_COLORARG2,
        D3DTSS_ALPHAOP,       D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
        D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS};
    return k;
  }
  static const D3DSAMPLERSTATETYPE* samp_list() {
    static const D3DSAMPLERSTATETYPE k[kNumSamp] = {
        D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,  D3DSAMP_ADDRESSW,
        D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER};
    return k;
  }

  void begin() {
    for (size_t i = 0; i < kNumRs; ++i)
      g_dev->GetRenderState(rs_list()[i], &rs[i]);
    for (DWORD s = 0; s < 2; ++s) {
      for (size_t i = 0; i < kNumTss; ++i)
        g_dev->GetTextureStageState(s, tss_list()[i], &tss[s][i]);
      for (size_t i = 0; i < kNumSamp; ++i)
        g_dev->GetSamplerState(s, samp_list()[i], &samp[s][i]);
    }
    g_dev->GetTextureStageState(2, D3DTSS_COLOROP, &tss2_colorop);
    g_dev->GetTransform(D3DTS_WORLD, &world);
    g_dev->GetTransform(D3DTS_VIEW, &view);
    g_dev->GetTransform(D3DTS_PROJECTION, &proj);
    g_dev->GetTransform(D3DTS_TEXTURE0, &tex0);
    g_dev->GetTransform(D3DTS_TEXTURE1, &tex1);
    g_dev->GetMaterial(&material);
    g_dev->GetViewport(&viewport);
    g_dev->GetFVF(&fvf);
    g_dev->GetRenderTarget(0, &rt);
    g_dev->GetDepthStencilSurface(&ds);
  }

  // The camera's target, depth buffer, viewport, projection and view
  // (FUN_0044b0d0).
  void camera_target() {
    if (rt) g_dev->SetRenderTarget(0, rt);
    g_dev->SetDepthStencilSurface(ds);
    g_dev->SetViewport(&viewport);
    g_dev->SetTransform(D3DTS_PROJECTION, &proj);
    g_dev->SetTransform(D3DTS_VIEW, &view);
  }

  void end() {
    camera_target();
    if (rt) rt->Release();
    if (ds) ds->Release();
    rt = ds = nullptr;
    g_dev->SetTexture(0, nullptr);
    g_dev->SetTexture(1, nullptr);
    g_dev->SetTransform(D3DTS_WORLD, &world);
    g_dev->SetTransform(D3DTS_TEXTURE0, &tex0);
    g_dev->SetTransform(D3DTS_TEXTURE1, &tex1);
    g_dev->SetMaterial(&material);
    g_dev->SetFVF(fvf);
    g_dev->SetTextureStageState(2, D3DTSS_COLOROP, tss2_colorop);
    for (DWORD s = 0; s < 2; ++s) {
      for (size_t i = 0; i < kNumTss; ++i)
        g_dev->SetTextureStageState(s, tss_list()[i], tss[s][i]);
      for (size_t i = 0; i < kNumSamp; ++i)
        g_dev->SetSamplerState(s, samp_list()[i], samp[s][i]);
    }
    for (size_t i = 0; i < kNumRs; ++i)
      g_dev->SetRenderState(rs_list()[i], rs[i]);
  }
};

// One owner of a camera pass: the exe's sector.
struct ShadowOwner {
  int32_t id = 0;
  bool bounded = false;
  float centre[3] = {0, 0, 0};  // sector+0x5B0
  float half[3] = {0, 0, 0};    // sector+0x5BC
  int32_t instances = 0;
  std::vector<void*> casters;  // rg_shadow (sector+0x6DC), by instance
};

// PE 940 FUN_0045cb10 / FUN_00515fc0: the sector box takes the first
// instance's sphere as a cube and grows by the cube of every other one.
void shadow_owner_grow(ShadowOwner& o, const float p[3], float r) {
  if (!o.bounded) {
    for (int i = 0; i < 3; ++i) {
      o.centre[i] = p[i];
      o.half[i] = r;
    }
    o.bounded = true;
    return;
  }
  for (int i = 0; i < 3; ++i) {
    float lo = p[i] - r, hi = p[i] + r;
    if (o.centre[i] - o.half[i] < lo) lo = o.centre[i] - o.half[i];
    if (hi < o.half[i] + o.centre[i]) hi = o.half[i] + o.centre[i];
    o.centre[i] = (hi + lo) * 0.5f;
    o.half[i] = hi - o.centre[i];
  }
}
#endif  // _WIN32

}  // namespace

// PE 940 FUN_0047c240 (the engine reads Config) with the test of
// FUN_00421280: a shadow size of 0 turns the shadows off.
// `source` says where the values come from (the log).
void render_d3d9_set_shadow_options(int32_t shadows, int32_t size,
                                    float detail, const char* source) {
  if (size <= 0) {
    size = 0;
    shadows = 0;
  }
  g_shadow_on = shadows;
  g_shadow_size = size;
  g_shadow_detail = detail;
  g_shadow_source = source ? source : "";
}

static void shadow_frame_begin() {
  g_shadow_recs.clear();
  g_shadow_passes.clear();
  g_shadow_skip_ground = 0;
  g_shadow_skip_light = 0;
#ifdef _WIN32
  // The static caster buffers nothing has cast from for a while.
  if ((g_cam_pass_frame & 0xffu) == 0 && !g_shadow_geom.empty()) {
    for (auto it = g_shadow_geom.begin(); it != g_shadow_geom.end();) {
      if (g_cam_pass_frame - it->second.used > kShadowGeomKeep) {
        shadow_geom_free(it->second);
        it = g_shadow_geom.erase(it);
      } else {
        ++it;
      }
    }
  }
#endif
}

#ifdef _WIN32
// The shadows of one camera pass. `members` are the instances the pass
// draws, `ground` the sections of its rg_ground, `cam_world` the camera's
// world matrix (C3DLow vt+0x98 FUN_00425960: the inverse of the view
// matrix), `lights` the lights of the pass.
// Host: a pass without a ground section or without a device light draws
// nothing here. In the exe the casters of such a pass would still be drawn
// into the target, and the receivers only when the camera has a light
// (FUN_0045c6a0); with no light their lit term is 0. Nothing shows either
// way.
static void draw_camera_shadows(const std::vector<void*>& members,
                                const std::vector<GroundSec>& ground,
                                const D3DMATRIX& cam_world,
                                const CamLights& lights) {
  // FUN_00460ba0: DAT_005da39c (1) && DAT_005da018.
  if (g_shadow_on == 0 || g_shadow_size <= 0 || members.empty()) return;
  if (ground.empty()) {
    ++g_shadow_skip_ground;
    return;
  }
  if (!lights.any || lights.device.empty()) {
    ++g_shadow_skip_light;
    return;
  }

  // Host: a pass in which no instance has render type flag 8 has no
  // caster, whatever its owners are (rg_shadow stays empty, FUN_00465e40):
  // nothing is grouped for it.
  {
    bool flag8 = false;
    for (void* key : members) {
      auto fit = g_mesh_type_flags.find(key);
      if (fit != g_mesh_type_flags.end() && (fit->second & 9u) == 8u) {
        flag8 = true;
        break;
      }
    }
    if (!flag8) return;
  }

  // The sectors of the pass. Host: the owner of a node and the place of
  // an owner in `owners` are looked up through two maps of this pass,
  // which keep their storage between passes.
  std::vector<ShadowOwner> owners;
  static std::unordered_map<void*, int32_t> s_asked;
  static std::unordered_map<int32_t, size_t> s_owner_at;
  s_asked.clear();
  s_owner_at.clear();
  ShadowPassRec prec;
  prec.pass = static_cast<int16_t>(g_cam_pass_stats.passes - 1);
  prec.ground_sections = static_cast<int32_t>(ground.size());
  prec.lights = static_cast<int32_t>(lights.device.size());
  for (void* key : members) {
    auto it = g_meshes.find(key);
    if (it == g_meshes.end() || !it->second.ready) continue;
    const MeshState& ms = it->second;
    if (ms.group == kMeshGroupGround) continue;
    uint32_t flags = 0;
    {
      auto fit = g_mesh_type_flags.find(key);
      if (fit != g_mesh_type_flags.end()) flags = fit->second;
    }
    // A horizon instance (RT+0xFC) is in rg_horizon alone and its bounds
    // are not kept up (FUN_00469990).
    if (flags & 1u) continue;
    // Fork: the owner id of an instance no part or car made was not read
    // (payload+0x1B8 of a RenderRef a script creates); on the host it has
    // none, and an instance without owner is in no sector (FUN_00469990
    // looks a sector up only for a non-zero id).
    // (One question per node the instances hang under.)
    void* const up = chain_parent_of(key);
    int32_t id = 0;
    {
      auto ait = s_asked.find(up);
      if (ait != s_asked.end()) {
        id = ait->second;
      } else {
        id = native_render_owner(up);
        s_asked.emplace(up, id);
      }
    }
    if (id == 0) {
      if (flags & 8u) ++prec.no_owner;
      continue;
    }
    bool has_vertices = false;
    for (const MeshSubmesh& sm : ms.subs)
      if (!sm.verts.empty()) has_vertices = true;
    {
      auto vit = g_mesh_vbs.find(key);
      if (vit != g_mesh_vbs.end() && vit->second.ready &&
          vit->second.vert_count > 0)
        has_vertices = true;
    }
    // A mesh without a vertex has no bounds (mesh+0x104, kind 2) and no
    // section to cast.
    if (!has_vertices) continue;
    ShadowOwner* o = nullptr;
    {
      auto oit = s_owner_at.find(id);
      if (oit != s_owner_at.end()) {
        o = &owners[oit->second];
      } else {
        s_owner_at.emplace(id, owners.size());
        owners.emplace_back();
        o = &owners.back();
        o->id = id;
      }
    }
    // FUN_00469990: the sphere of the mesh (centre the mesh origin, radius
    // sqrt(max(|min|^2, |max|^2)), FUN_00451b00 / FUN_00452960) at the
    // instance's position.
    const float a = ms.bmin[0] * ms.bmin[0] + ms.bmin[1] * ms.bmin[1] +
                    ms.bmin[2] * ms.bmin[2];
    const float b = ms.bmax[0] * ms.bmax[0] + ms.bmax[1] * ms.bmax[1] +
                    ms.bmax[2] * ms.bmax[2];
    const float radius = std::sqrt(a < b ? b : a) * ms.unit;
    D3DMATRIX w{};
    resolve_world(key, &w, 0);
    const float pos[3] = {w.m[3][0], w.m[3][1], w.m[3][2]};
    shadow_owner_grow(*o, pos, radius);
    ++o->instances;
    if (flags & 8u) o->casters.push_back(key);  // FUN_00465e40
  }
  bool any = false;
  for (const ShadowOwner& o : owners)
    if (!o.casters.empty()) any = true;
  if (!any) {
    if (prec.no_owner != 0) g_shadow_passes.push_back(prec);
    return;
  }

  // The camera's detail terms (FUN_0045c7c0 gets cam+0x4A0, +0x4AC,
  // +0x4B0: cot(aov / 2), lodBias, lodAmp).
  float cot = 1.f, lod_amp = 1.f, lod_bias = 1.f;
  {
    auto cit = g_cameras.find(g_pass_cam);
    if (cit != g_cameras.end()) {
      float aov = cit->second.aov_deg;
      if (!(aov > 0.5f)) aov = 0.5f;  // Fork: as build_camera_projection
      if (aov > 179.f) aov = 179.f;
      const float half = aov * 0.008726389f;
      cot = std::cos(half) / std::sin(half);
      lod_amp = cit->second.lod_amp;
      lod_bias = cit->second.lod_bias;
    }
  }
  const float eye[3] = {cam_world.m[3][0], cam_world.m[3][1],
                        cam_world.m[3][2]};

  ShadowScope scope;
  bool scope_open = false;
  IDirect3DTexture9* target = nullptr;
  IDirect3DTexture9* ramp = nullptr;

  for (ShadowOwner& o : owners) {
    if (o.casters.empty()) continue;  // sector+0x6D0 == 0: not listed
    ++prec.owners;
    ShadowRec rec;
    rec.pass = prec.pass;
    rec.owner = o.id;
    rec.instances = o.instances;
    rec.casters = static_cast<int32_t>(o.casters.size());
    for (int i = 0; i < 3; ++i) rec.centre[i] = o.centre[i];
    // The radius of a box is the length of its half extents.
    const float radius =
        std::sqrt(o.half[0] * o.half[0] + o.half[1] * o.half[1] +
                  o.half[2] * o.half[2]);
    rec.radius = radius;
    {
      auto mit = g_meshes.find(o.casters[0]);
      if (mit != g_meshes.end()) rec.first = mit->second.label;
    }
    // FUN_0045c7c0: min(1, radius / distance x cot x lodAmp + lodBias),
    // 1 at the eye; nothing below Config.shadow_detail.
    float detail = 1.f;
    {
      const float dx = eye[0] - o.centre[0], dy = eye[1] - o.centre[1];
      const float dz = eye[2] - o.centre[2];
      const float d2 = dx * dx + dy * dy + dz * dz;
      if (!(d2 < 9.99999975e-05f)) {  // DAT_005938c4
        detail = std::sqrt((radius * radius) / d2) * cot * lod_amp + lod_bias;
        if (!(0.f <= detail)) detail = 0.f;
      }
      if (1.f <= detail) detail = 1.f;
    }
    rec.detail = detail;
    shadow_light_direction(lights, o.centre, rec.dir);
    // FUN_0044b9b0 does nothing for a radius below 1e-4.
    // Fork: the exe then draws the sector with what the one before left
    // in C3DLow; the host leaves it out.
    if (!(g_shadow_detail <= detail) || radius < 9.99999975e-05f) {
      ++prec.below_detail;
      g_shadow_recs.push_back(rec);
      continue;
    }
    if (!target) {
      target = shadow_target();
      ramp = shadow_ramp();
    }
    if (!target || !ramp) {
      g_shadow_recs.push_back(rec);
      continue;
    }
    if (!scope_open) {
      scope.begin();
      scope_open = true;
    }

    // FUN_0044b9b0: the rotation that looks along the direction, and the
    // receiver's texture matrix inverse(camera view) x translate(-centre)
    // x rotation x {scale (0.5 / r, -0.5 / r, 0.5 / r), offset 0.5}.
    D3DMATRIX rot{};
    shadow_view(rec.dir, &rot);
    D3DMATRIX light_view = rot;  // translate(-centre) x rot
    for (int j = 0; j < 3; ++j)
      light_view.m[3][j] = -(o.centre[0] * rot.m[0][j] +
                             o.centre[1] * rot.m[1][j] +
                             o.centre[2] * rot.m[2][j]);
    const float half_inv = 0.5f / radius;  // DAT_00593590 / radius
    D3DMATRIX scale{};
    scale.m[0][0] = half_inv;
    scale.m[1][1] = -half_inv;
    scale.m[2][2] = half_inv;
    scale.m[3][0] = scale.m[3][1] = scale.m[3][2] = 0.5f;
    scale.m[3][3] = 1.f;
    D3DMATRIX tmp{}, tex_matrix{};
    mat_mul(cam_world, light_view, &tmp);
    mat_mul(tmp, scale, &tex_matrix);

    // The casters. Begin FUN_0044a850: level 0 of the target, no depth
    // buffer, VIEW = the rotation, PROJECTION = D3DXMatrixOrthoRH(2r, 2r,
    // -100, 100) (engine units), ZENABLE 0, lighting off, ZFUNC ALWAYS,
    // ALPHABLENDENABLE 0, TEXTUREFACTOR 0xFFFFFFFF.
    {
      IDirect3DSurface9* surf = nullptr;
      if (FAILED(target->GetSurfaceLevel(0, &surf)) || !surf) {
        g_shadow_recs.push_back(rec);
        continue;
      }
      g_dev->SetRenderTarget(0, surf);
      g_dev->SetDepthStencilSurface(nullptr);
      surf->Release();
    }
    g_dev->SetTransform(D3DTS_VIEW, &rot);
    {
      const float w = radius + radius;
      constexpr float zn = -10.f, zf = 10.f;  // Fork: metres
      D3DMATRIX ortho{};
      ortho.m[0][0] = 2.f / w;
      ortho.m[1][1] = 2.f / w;
      ortho.m[2][2] = 1.f / (zn - zf);
      ortho.m[3][2] = zn / (zn - zf);
      ortho.m[3][3] = 1.f;
      g_dev->SetTransform(D3DTS_PROJECTION, &ortho);
    }
    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFFu);
    // The first caster (FUN_0044b2f0): Clear(target, 0), and with
    // DAT_005da268 (1) on a device that can scissor a scissor rectangle one
    // texel inside the target, so that its border stays black for the
    // CLAMP addressing of the receiver.
    g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 0.f, 0);
    {
      D3DVIEWPORT9 vp{};
      g_dev->GetViewport(&vp);
      RECT sc;
      sc.left = 1;
      sc.top = 1;
      sc.right = static_cast<LONG>(vp.Width + vp.X) - 1;
      sc.bottom = static_cast<LONG>(vp.Height + vp.Y) - 1;
      g_dev->SetScissorRect(&sc);
      g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    }
    // Per caster (FUN_00468310, FUN_0044b2f0): WORLD = the rotation of the
    // instance with (instance position - centre) as translation, texture 0
    // = null, CULLMODE NONE, the section's geometry.
    // Fork: the exe sets no colour state here: lighting is off, no texture
    // is bound and its object streams have no vertex colour, so the frame
    // default MODULATE (texture, diffuse) gives white (M3 spec step 7,
    // inferred). The host says it: stage 0 selects TFACTOR, which the
    // begin has set to white, and the alpha test is off (it is on in the
    // exe, GREATER 4 from C3DLow vt+0x34(2, 1), and the alpha is 1).
    // Fork: a section with two bone matrices is drawn rigid with the
    // instance matrix: the exe sets WORLD alone here, not the matrices of
    // the bones.
    g_dev->SetTexture(0, nullptr);
    g_dev->SetTexture(1, nullptr);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    g_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    g_dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS,
                                D3DTTFF_DISABLE);
    g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    for (void* key : o.casters) {
      auto mit = g_meshes.find(key);
      if (mit == g_meshes.end()) continue;
      const MeshState& ms = mit->second;
      D3DMATRIX world{};
      resolve_world(key, &world, 0);
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) world.m[r][c] *= ms.unit;
      for (int j = 0; j < 3; ++j) world.m[3][j] -= o.centre[j];
      g_dev->SetTransform(D3DTS_WORLD, &world);
      auto vit = g_mesh_vbs.find(key);
      auto iit = g_mesh_ibs.find(key);
      if (vit != g_mesh_vbs.end() && vit->second.ready && vit->second.vb &&
          iit != g_mesh_ibs.end() && iit->second.ready && iit->second.ib &&
          iit->second.index_count >= 3) {
        const MeshVbState& vb = vit->second;
        const MeshIbState& ib = iit->second;
        g_dev->SetFVF(vb.pe_fvf != 0
                          ? vb.pe_fvf
                          : (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1));
        g_dev->SetStreamSource(0, vb.vb, 0, static_cast<UINT>(vb.stride));
        g_dev->SetIndices(ib.ib);
        const UINT nvert = static_cast<UINT>(vb.vert_count);
        const UINT ntri = static_cast<UINT>(ib.index_count / 3);
        if (nvert > 0 && ntri > 0) {
          g_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, nvert, 0, ntri);
          ++rec.sections;
          rec.triangles += static_cast<int32_t>(ntri);
        }
        g_dev->SetStreamSource(0, nullptr, 0, 0);
        g_dev->SetIndices(nullptr);
        continue;
      }
      // Fork: from the static copy of the mesh's positions (ShadowGeom).
      if (const ShadowGeom* sg = shadow_geom(ms)) {
        g_dev->SetFVF(D3DFVF_XYZ);
        g_dev->SetStreamSource(0, sg->vb, 0, 3 * sizeof(float));
        g_dev->SetIndices(sg->ib);
        for (const ShadowGeom::Range& r : sg->ranges) {
          g_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,
                                      static_cast<INT>(r.base_vertex), 0,
                                      r.verts, r.first_index, r.tris);
          ++rec.sections;
          rec.triangles += static_cast<int32_t>(r.tris);
        }
        g_dev->SetStreamSource(0, nullptr, 0, 0);
        g_dev->SetIndices(nullptr);
        continue;
      }
      g_dev->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1);
      for (const MeshSubmesh& sm : ms.subs) {
        if (sm.verts.empty() || sm.indices.size() < 3) continue;
        const UINT ntri = static_cast<UINT>(sm.indices.size() / 3);
        g_dev->DrawIndexedPrimitiveUP(
            D3DPT_TRIANGLELIST, 0, static_cast<UINT>(sm.verts.size()), ntri,
            sm.indices.data(), D3DFMT_INDEX16, sm.verts.data(),
            sizeof(MeshVertex));
        ++rec.sections;
        rec.triangles += static_cast<int32_t>(ntri);
      }
    }
    // End FUN_0044b0d0: the scissor test off, the camera's target, depth
    // buffer, viewport, projection and view, ZENABLE 1, ZFUNC LESSEQUAL.
    g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    scope.camera_target();
    g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);

    // The receivers (FUN_0045c6a0: when a caster was drawn and the camera
    // has lights): the lights on, WORLD = the identity, and every ground
    // instance the camera sees through FUN_004335f0.
    if (rec.sections != 0) {
      for (DWORD i = 0; i < 8; ++i) {
        if (i < lights.device.size()) {
          g_dev->SetLight(i, &lights.device[i]);
          g_dev->LightEnable(i, TRUE);
        } else {
          g_dev->LightEnable(i, FALSE);
        }
      }
      g_dev->SetRenderState(D3DRS_LIGHTING, TRUE);
      g_dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
      // Fork: the wide vertices carry two colours the exe's shadow copy
      // does not have; the material is the source of every term.
      g_dev->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
      g_dev->SetRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
      g_dev->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
      g_dev->SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
      // FUN_00440ec0: dest = dest x (1 - src), no alpha test; then no z
      // write.
      g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
      g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
      g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCCOLOR);
      g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
      g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
      g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
      // The frame default cull mode (C3DLow[6] = D3DCULL_CW): the
      // material of the shadow copy has no create flag.
      g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
      g_dev->SetTexture(0, target);
      g_dev->SetTexture(1, ramp);
      g_dev->SetTransform(D3DTS_TEXTURE0, &tex_matrix);
      g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFFu);
      for (DWORD s = 0; s < 2; ++s) {
        // CLAMP on the three axes (stage 0: when DAT_005da114, which is
        // 1), no mip filter, LINEAR min and mag.
        g_dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        g_dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        g_dev->SetSamplerState(s, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
        g_dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        g_dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        g_dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
      }
      // Diffuse = (fade, fade, fade, 1) with the global fade C3DLow+0x38
      // (1 in a normal frame), every other term 0.
      D3DMATERIAL9 mat{};
      mat.Diffuse.r = mat.Diffuse.g = mat.Diffuse.b = mat.Diffuse.a = 1.f;
      g_dev->SetMaterial(&mat);
      // Stage 0: the target through the camera-space position. The colour
      // op is the frame default, MODULATE with the lit diffuse.
      g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
      g_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX,
                                  D3DTSS_TCI_CAMERASPACEPOSITION);
      g_dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS,
                                  D3DTTFF_COUNT2);
      // Stage 1: the ramp, u = (world y - centre.y) x 0.25 / radius + 0.5,
      // v = 0.5, from the y column of inverse(view).
      {
        const float k = 0.25f / radius;  // DAT_005947a0 / C3DLow+0x43C
        D3DMATRIX rm{};
        rm.m[0][0] = k * cam_world.m[0][1];
        rm.m[1][0] = k * cam_world.m[1][1];
        rm.m[2][0] = k * cam_world.m[2][1];
        rm.m[3][0] = (cam_world.m[3][1] - o.centre[1]) * k + 0.5f;
        rm.m[3][1] = 0.5f;
        rm.m[2][2] = 1.f;
        rm.m[3][3] = 1.f;
        g_dev->SetTransform(D3DTS_TEXTURE1, &rm);
      }
      g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
      g_dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      g_dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
      // Fork: the exe leaves the alpha op of stage 1 at DISABLE; the host
      // passes the alpha on (it is not read: no alpha test, colour blend
      // factors).
      g_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      g_dev->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
      g_dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX,
                                  D3DTSS_TCI_CAMERASPACEPOSITION);
      g_dev->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS,
                                  D3DTTFF_COUNT2);
      g_dev->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
      g_dev->SetFVF(kFvfWide);
      const MeshState* world_of = nullptr;
      for (const GroundSec& gs : ground) {
        const MeshSubmesh& sm = gs.ms->subs[static_cast<size_t>(gs.sub)];
        if (sm.wide.empty() || sm.indices.size() < 3) continue;
        if (gs.ms != world_of) {
          D3DMATRIX world{};
          mat_identity(&world);
          world.m[0][0] = world.m[1][1] = world.m[2][2] = gs.ms->unit;
          g_dev->SetTransform(D3DTS_WORLD, &world);
          world_of = gs.ms;
        }
        mode3_draw(sm);
        ++rec.receivers;
      }
      // FUN_004335f0, the restore branch: both stages unbound, z write on.
      // (The scope puts the rest back when the pass ends.)
      g_dev->SetTexture(0, nullptr);
      g_dev->SetTexture(1, nullptr);
      g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
      rec.drawn = true;
      ++prec.drawn;
    }
    g_shadow_recs.push_back(rec);
  }
  if (scope_open) scope.end();
  g_shadow_passes.push_back(prec);
}
#else
static void draw_camera_shadows(const std::vector<void*>&,
                                const std::vector<GroundSec>&,
                                const D3DMATRIX&, const CamLights&) {}
#endif

// `[render-dbg]`: the shadow options, and for the last flushed frame one
// line per camera pass that had casters and one per owner.
void render_d3d9_shadow_debug_dump(FILE* out) {
  if (!out || !render_boot940()) return;
  char target[64] = "none yet";
#ifdef _WIN32
  if (g_shadow_target)
    std::snprintf(target, sizeof target, "%dx%d %s", g_shadow_target_size,
                  g_shadow_target_size,
                  g_shadow_target_fmt == D3DFMT_A8R8G8B8 ? "A8R8G8B8"
                                                         : "X8R8G8B8");
  else if (g_shadow_target_failed)
    std::snprintf(target, sizeof target, "FAILED %dx%d", g_shadow_target_size,
                  g_shadow_target_size);
#endif
  std::fprintf(out,
               "[render-dbg] shadow options: shadows=%d shadow_size=%d "
               "shadow_detail=%g (%s); target=%s; last frame: passes with "
               "casters=%zu, passes without ground=%d, without a device "
               "light=%d\n",
               g_shadow_on, g_shadow_size, g_shadow_detail,
               g_shadow_source.c_str(), target, g_shadow_passes.size(),
               g_shadow_skip_ground, g_shadow_skip_light);
  for (const ShadowPassRec& p : g_shadow_passes) {
    std::fprintf(out,
                 "[render-dbg]   shadow pass %d: owners=%d drawn=%d "
                 "below_detail=%d casters_without_owner=%d ground_sections=%d "
                 "lights=%d\n",
                 p.pass, p.owners, p.drawn, p.below_detail, p.no_owner,
                 p.ground_sections, p.lights);
    for (const ShadowRec& r : g_shadow_recs) {
      if (r.pass != p.pass) continue;
      std::fprintf(out,
                   "[render-dbg]     shadow owner 0x%08X: instances=%d "
                   "casters=%d sections=%d triangles=%d centre=(%.4f, %.4f, "
                   "%.4f) radius=%.4f dir=(%.4f, %.4f, %.4f) detail=%.4f "
                   "target=%d receivers=%d drawn=%d first='%s'\n",
                   static_cast<unsigned>(r.owner), r.instances, r.casters,
                   r.sections, r.triangles, r.centre[0], r.centre[1],
                   r.centre[2], r.radius, r.dir[0], r.dir[1], r.dir[2],
                   r.detail, g_shadow_size, r.receivers, r.drawn ? 1 : 0,
                   r.first.c_str());
    }
  }
}

}  // namespace inv
