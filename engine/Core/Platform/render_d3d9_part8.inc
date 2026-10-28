// Fragment 8 of render_d3d9.cpp - included, not compiled on its own (after
// fragments 1 to 7).
//
// M7: the paint booth's drawing (../slrr-ghidra/notes/m7/exe_paint.md).
//   render_d3d9_texture_make_paintable   FUN_0044d590 with FUN_0044e490: the
//                               render target copy a stroke draws into
//   render_d3d9_paint_fill      the PaintTexture job (FUN_00420290), run by
//                               FUN_00421820 -> C3DLow+0x190 FUN_004289b0
//   render_d3d9_paint_decal     the PaintDecal job (FUN_00420100) with the
//                               projector of FUN_004c8c60; a permanent one
//                               runs FUN_00421690 -> FUN_004693c0 ->
//                               C3DLow+0x1F8 FUN_00449370 -> +0x1F4
//                               FUN_00449400 (FUN_00449170, FUN_00448c70),
//                               a preview FUN_00421760 -> +0x1FC FUN_00449b40
//   render_d3d9_paint_draw_previews / render_d3d9_paint_run_jobs   where the
//                               frame runs them (FUN_00460ba0, FUN_00421920)
//   render_d3d9_paint_debug_dump  host: `[render-dbg] paint` lines

namespace inv {
namespace {

// A stroke: the render instance whose sections are painted, the target
// texture (the paintable copy), the brush, and the projector as three rows
// over world positions in metres: (s, t, n) = row . (x, y, z, 1).
struct PaintDecalJob {
  void* mesh = nullptr;
  void* target = nullptr;
  void* brush = nullptr;
  float rows[3][4] = {};
  bool flip = false;
  uint32_t colour = 0xFFFFFFFFu;
};
std::vector<PaintDecalJob> g_paint_decals;    // flag bit 0 clear
std::vector<PaintDecalJob> g_paint_previews;  // flag bit 0 set (temp)
struct PaintFillJob {
  void* target = nullptr;
  uint32_t colour = 0;
};
std::vector<PaintFillJob> g_paint_fills;

struct PaintRenderStats {
  int copies = 0, copy_fail = 0;
  int fills = 0, fill_fail = 0;
  int decals = 0, decal_fail = 0, sections = 0, triangles = 0, accepted = 0;
  int previews = 0, preview_sections = 0;
  uint32_t frames_with_jobs = 0;
} g_paint_rstats;

// Fork: the depth surfaces of the strokes, by size. PE 940 keeps four
// (C3DLow+0x598, count +0x5A8) and drops the oldest for a fifth size.
struct PaintDepth {
  UINT w = 0, h = 0;
  IDirect3DSurface9* surf = nullptr;
};
std::vector<PaintDepth> g_paint_depths;

IDirect3DSurface9* paint_depth_surface(UINT w, UINT h) {
  for (PaintDepth& d : g_paint_depths)
    if (d.w == w && d.h == h && d.surf) return d.surf;
  if (g_paint_depths.size() == 4) {
    if (g_paint_depths.front().surf) g_paint_depths.front().surf->Release();
    g_paint_depths.erase(g_paint_depths.begin());
  }
  PaintDepth d;
  d.w = w;
  d.h = h;
  if (FAILED(g_dev->CreateDepthStencilSurface(w, h, g_depth_format,
                                              D3DMULTISAMPLE_NONE, 0, TRUE,
                                              &d.surf, nullptr)))
    d.surf = nullptr;
  g_paint_depths.push_back(d);
  return d.surf;
}

// What a stroke changes of the device, captured before and applied after
// (the exe sets each state back by hand, FUN_00449400 and FUN_004289b0).
struct PaintDeviceSave {
  IDirect3DStateBlock9* block = nullptr;
  IDirect3DSurface9* rt = nullptr;
  IDirect3DSurface9* depth = nullptr;
  D3DVIEWPORT9 vp{};
  bool save() {
    if (FAILED(g_dev->CreateStateBlock(D3DSBT_ALL, &block))) block = nullptr;
    g_dev->GetRenderTarget(0, &rt);
    g_dev->GetDepthStencilSurface(&depth);
    g_dev->GetViewport(&vp);
    return block != nullptr;
  }
  void restore() {
    if (rt) {
      g_dev->SetRenderTarget(0, rt);
      rt->Release();
      rt = nullptr;
    }
    g_dev->SetDepthStencilSurface(depth);
    if (depth) {
      depth->Release();
      depth = nullptr;
    }
    if (block) {
      block->Apply();
      block->Release();
      block = nullptr;
    }
    g_dev->SetViewport(&vp);
  }
};

// The texture a key names, as a render target texture (the paintable copy).
IDirect3DTexture9* paint_target_texture(void* key) {
  auto it = g_textures.find(key);
  if (it == g_textures.end() || !it->second.tex) return nullptr;
  D3DSURFACE_DESC desc{};
  if (FAILED(it->second.tex->GetLevelDesc(0, &desc))) return nullptr;
  if ((desc.Usage & D3DUSAGE_RENDERTARGET) == 0) return nullptr;
  return it->second.tex;
}

// The mesh whose sections an instance draws (a copy shows its source's).
const MeshState* paint_mesh_of(void* key) {
  void* geo = key;
  auto gs = g_mesh_geom_src.find(key);
  if (gs != g_mesh_geom_src.end() && gs->second) geo = gs->second;
  auto it = g_meshes.find(geo);
  if (it == g_meshes.end() || !it->second.ready) return nullptr;
  return &it->second;
}

// The world matrix the camera pass draws the instance with (draw_section):
// resolve_world with the instance's own bone00 folded, the file units
// multiplied into the 3x3.
void paint_world_of(void* key, const MeshState& ms, D3DMATRIX* w) {
  const bool fold = g_fold_own_bone0;
  g_fold_own_bone0 = true;
  resolve_world(key, w, 0);
  g_fold_own_bone0 = fold;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) w->m[r][c] *= ms.unit;
}

void paint_xform(const D3DMATRIX& w, float x, float y, float z, float out[3]) {
  out[0] = x * w.m[0][0] + y * w.m[1][0] + z * w.m[2][0] + w.m[3][0];
  out[1] = x * w.m[0][1] + y * w.m[1][1] + z * w.m[2][1] + w.m[3][1];
  out[2] = x * w.m[0][2] + y * w.m[1][2] + z * w.m[2][2] + w.m[3][2];
}

// A section's vertex: its position (file units) and the uv of the texture
// coordinate set 0 the exe's vertex data holds, the material's primary
// channel (FUN_00454940).
void paint_vertex(const MeshSubmesh& sm, size_t i, float p[3], float uv[2]) {
  if (!sm.wide.empty() && i < sm.wide.size()) {
    const MeshVertexWide& v = sm.wide[i];
    p[0] = v.x;
    p[1] = v.y;
    p[2] = v.z;
    const int c = (sm.chan_primary >= 0 && sm.chan_primary < 3) ? sm.chan_primary : 0;
    uv[0] = v.uv[c][0];
    uv[1] = v.uv[c][1];
    return;
  }
  const MeshVertex& v = sm.verts[i];
  p[0] = v.x;
  p[1] = v.y;
  p[2] = v.z;
  uv[0] = v.u;
  uv[1] = v.v;
}
size_t paint_vertex_count(const MeshSubmesh& sm) {
  return sm.wide.empty() ? sm.verts.size() : sm.wide.size();
}

// The 7-dword vertex of FUN_00449400: XYZRHW | DIFFUSE | TEX1.
struct PaintVert {
  float x, y, z, rhw;
  uint32_t diffuse;
  float s, t;
};
constexpr DWORD kPaintFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// FUN_00575a60 (fmod by 1) and the + 1 of a negative result.
float paint_frac(float v) {
  float f = std::fmod(v, 1.f);
  if (f < 0.f) f += 1.f;
  return f;
}

// FUN_00449170: a triangle is painted when its brush coordinates are not
// all below 0 nor all above 1 on either axis and its signed area in (s, t)
// times k is positive (k = 1 with flip, -1 without).
bool paint_accept(const PaintVert& a, const PaintVert& b, const PaintVert& c,
                  float k) {
  if (a.s < 0.f && b.s < 0.f && c.s < 0.f) return false;
  if (a.t < 0.f && b.t < 0.f && c.t < 0.f) return false;
  if (a.s > 1.f && b.s > 1.f && c.s > 1.f) return false;
  if (a.t > 1.f && b.t > 1.f && c.t > 1.f) return false;
  const float area = (a.t - c.t) * (a.s - b.s) - (a.t - b.t) * (a.s - c.s);
  return 0.f < area * k;
}

// FUN_00448c70 with mask 0: per edge a band 1.5 px to either side, 1 px past
// each end (DAT_005da260 = 1.0, DAT_005da264 = 1.5), at z 0.7, two
// triangles with the uv of the edge's ends.
void paint_bleed(const PaintVert* v, std::vector<PaintVert>* out) {
  for (int k = 0; k < 3; ++k) {
    const PaintVert& a = v[k];
    const PaintVert& b = v[(k + 1) % 3];
    float ex = a.x - b.x, ey = a.y - b.y;
    const float len = std::sqrt(ex * ex + ey * ey);
    if (!(len > 0.f)) continue;
    ex /= len;
    ey /= len;
    const float px = -ey * 1.5f, py = ex * 1.5f;  // the side offset
    const float lx = ex * 1.f, ly = ey * 1.f;     // along the edge
    PaintVert A = a, D = a, B = b, C = b;
    A.x = a.x + lx + px; A.y = a.y + ly + py;
    D.x = a.x + lx - px; D.y = a.y + ly - py;
    B.x = b.x - lx + px; B.y = b.y - ly + py;
    C.x = b.x - lx - px; C.y = b.y - ly - py;
    A.z = B.z = C.z = D.z = 0.7f;
    out->push_back(A);
    out->push_back(B);
    out->push_back(C);
    out->push_back(A);
    out->push_back(C);
    out->push_back(D);
  }
}

bool paint_run_decal(const PaintDecalJob& j) {
  IDirect3DTexture9* target = paint_target_texture(j.target);
  const MeshState* ms = paint_mesh_of(j.mesh);
  auto bit = g_textures.find(j.brush);
  IDirect3DBaseTexture9* brush =
      bit != g_textures.end() ? texture_d3d_base(bit->second) : nullptr;
  // FUN_00449400: nothing without a brush texture (xPaint's stroke).
  if (!target || !ms || !brush) return false;
  IDirect3DSurface9* surf = nullptr;
  if (FAILED(target->GetSurfaceLevel(0, &surf)) || !surf) return false;
  D3DSURFACE_DESC desc{};
  surf->GetDesc(&desc);
  IDirect3DSurface9* depth = paint_depth_surface(desc.Width, desc.Height);
  if (!depth) {
    surf->Release();
    return false;
  }
  D3DMATRIX w{};
  paint_world_of(j.mesh, *ms, &w);
  PaintDeviceSave save;
  save.save();
  g_dev->SetRenderTarget(0, surf);
  g_dev->SetDepthStencilSurface(depth);
  g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, j.colour);
  g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  g_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  g_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  // The device's stage defaults (FUN_00426d10) with COLORARG2 = TFACTOR:
  // colour = brush.rgb x colour.rgb, alpha = brush.a x colour.a.
  g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
  g_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
  g_dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS,
                              D3DTTFF_DISABLE);
  g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  g_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  g_dev->SetTexture(0, brush);
  g_dev->SetTexture(1, nullptr);
  g_dev->SetVertexShader(nullptr);
  g_dev->SetPixelShader(nullptr);
  g_dev->SetFVF(kPaintFvf);
  const DWORD clear =
      D3DCLEAR_ZBUFFER |
      ((g_depth_format == D3DFMT_D24S8 || g_depth_format == D3DFMT_D24X4S4 ||
        g_depth_format == D3DFMT_D15S1)
           ? D3DCLEAR_STENCIL
           : 0);
  const float k = j.flip ? 1.f : -1.f;  // DAT_00593598 / DAT_005935ac
  const float tw = static_cast<float>(desc.Width);
  const float th = static_cast<float>(desc.Height);
  std::vector<PaintVert> verts, tris, bleed;
  // FUN_004693c0: every section of the instance's mesh, each through
  // FUN_00449370 (one depth clear per section).
  for (const MeshSubmesh& sm : ms->subs) {
    const size_t nv = paint_vertex_count(sm);
    if (nv == 0 || sm.indices.size() < 3) continue;
    ++g_paint_rstats.sections;
    verts.resize(nv);
    for (size_t i = 0; i < nv; ++i) {
      float p[3], uv[2], wp[3];
      paint_vertex(sm, i, p, uv);
      paint_xform(w, p[0], p[1], p[2], wp);
      PaintVert& o = verts[i];
      o.x = paint_frac(uv[0]) * tw - 0.5f;  // DAT_00593590
      o.y = paint_frac(uv[1]) * th - 0.5f;
      o.z = 0.5f;
      o.rhw = 2.f;
      o.diffuse = 0xFFFFFFFFu;  // Fork: the exe leaves it unset; unused
      o.s = j.rows[0][0] * wp[0] + j.rows[0][1] * wp[1] + j.rows[0][2] * wp[2] +
            j.rows[0][3];
      o.t = j.rows[1][0] * wp[0] + j.rows[1][1] * wp[1] + j.rows[1][2] * wp[2] +
            j.rows[1][3];
    }
    tris.clear();
    bleed.clear();
    for (size_t t = 0; t + 2 < sm.indices.size(); t += 3) {
      const uint16_t i0 = sm.indices[t], i1 = sm.indices[t + 1],
                     i2 = sm.indices[t + 2];
      if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
      ++g_paint_rstats.triangles;
      if (!paint_accept(verts[i0], verts[i1], verts[i2], k)) continue;
      ++g_paint_rstats.accepted;
      const PaintVert tri[3] = {verts[i0], verts[i1], verts[i2]};
      tris.insert(tris.end(), tri, tri + 3);
      paint_bleed(tri, &bleed);
    }
    if (tris.empty()) continue;
    g_dev->Clear(0, nullptr, clear, 0, 1.f, 0);
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST,
                           static_cast<UINT>(tris.size() / 3), tris.data(),
                           sizeof(PaintVert));
    if (!bleed.empty())
      g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST,
                             static_cast<UINT>(bleed.size() / 3), bleed.data(),
                             sizeof(PaintVert));
  }
  save.restore();
  surf->Release();
  return true;
}

// FUN_004289b0: the level-0 surface as render target and a quad over it
// with the colour: SELECTARG1 TFACTOR for colour and alpha, blended
// SRCALPHA / INVSRCALPHA, no depth. The quad's corners are the pixel
// centres 0 and W - 1 (and 0, H - 1), so the last column and row keep what
// they had, as in the exe.
bool paint_run_fill(const PaintFillJob& j) {
  IDirect3DTexture9* target = paint_target_texture(j.target);
  if (!target) return false;
  IDirect3DSurface9* surf = nullptr;
  if (FAILED(target->GetSurfaceLevel(0, &surf)) || !surf) return false;
  D3DSURFACE_DESC desc{};
  surf->GetDesc(&desc);
  PaintDeviceSave save;
  save.save();
  g_dev->SetRenderTarget(0, surf);
  g_dev->SetDepthStencilSurface(nullptr);
  g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, j.colour);
  g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
  g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  g_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  g_dev->SetTexture(0, nullptr);
  g_dev->SetVertexShader(nullptr);
  g_dev->SetPixelShader(nullptr);
  g_dev->SetFVF(D3DFVF_XYZRHW);
  const float w1 = static_cast<float>(desc.Width) - 1.f;
  const float h1 = static_cast<float>(desc.Height) - 1.f;
  const float quad[4][4] = {
      {0.f, h1, 0.5f, 2.f}, {w1, h1, 0.5f, 2.f}, {0.f, 0.f, 0.5f, 2.f},
      {w1, 0.f, 0.5f, 2.f}};
  g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
  save.restore();
  surf->Release();
  return true;
}

}  // namespace

bool render_d3d9_texture_make_paintable(void* dst_key, void* src_key) {
  if (!dst_key || !src_key || !g_dev) return false;
  auto sit = g_textures.find(src_key);
  if (sit == g_textures.end() || !sit->second.tex) {
    ++g_paint_rstats.copy_fail;
    return false;
  }
  IDirect3DTexture9* src = sit->second.tex;
  D3DSURFACE_DESC desc{};
  if (FAILED(src->GetLevelDesc(0, &desc))) {
    ++g_paint_rstats.copy_fail;
    return false;
  }
  const DWORD levels = src->GetLevelCount();
  const std::string label = sit->second.label;
  PFN_D3DXLoadSurfaceFromSurface load = d3dx_load_surface_from_surface();
  IDirect3DTexture9* copy = nullptr;
  if (!load ||
      FAILED(g_dev->CreateTexture(desc.Width, desc.Height, levels,
                                  D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                  D3DPOOL_DEFAULT, &copy, nullptr)) ||
      !copy) {
    ++g_paint_rstats.copy_fail;
    return false;
  }
  // FUN_0044e490: every level from the old texture, filter NONE.
  for (DWORD l = 0; l < levels; ++l) {
    IDirect3DSurface9* d = nullptr;
    IDirect3DSurface9* s = nullptr;
    if (SUCCEEDED(copy->GetSurfaceLevel(l, &d)) &&
        SUCCEEDED(src->GetSurfaceLevel(l, &s)))
      load(d, nullptr, nullptr, s, nullptr, nullptr, 1 /* D3DX_FILTER_NONE */,
           0);
    if (s) s->Release();
    if (d) d->Release();
  }
  TextureState& st = g_textures[dst_key];
  texture_release_gpu(st);
  st.label = label + " (paint)";
  st.w = static_cast<int32_t>(desc.Width);
  st.h = static_cast<int32_t>(desc.Height);
  st.mips = static_cast<int32_t>(levels);
  st.engine_fmt = 9;  // DAT_005da29c: the render target format A8R8G8B8
  st.luma_alpha = false;
  st.tex = copy;
  ++g_paint_rstats.copies;
  return true;
}

void render_d3d9_texture_take(void* dst_key, void* src_key) {
  if (!dst_key || !src_key || dst_key == src_key) return;
  auto sit = g_textures.find(src_key);
  if (sit == g_textures.end()) return;
  TextureState moved = sit->second;
  sit->second.tex = nullptr;  // the D3D objects go with the state
  sit->second.cube = nullptr;
  sit->second.mip_up_surf = nullptr;
  g_textures.erase(sit);
  TextureState& dst = g_textures[dst_key];
  texture_release_gpu(dst);
  dst = moved;
}

void render_d3d9_paint_fill(void* target_key, uint32_t argb) {
  if (!target_key) return;
  g_paint_fills.push_back({target_key, argb});
}

bool render_d3d9_paint_decal(void* mesh_key, void* target_key, void* brush_key,
                             const float a[3], const float b[3], float size,
                             float rot, int32_t flip, uint32_t argb,
                             int32_t temp) {
  if (!mesh_key || !target_key) return false;
  // FUN_004c8c60: n = -(B - A) / |B - A|, the up vector (0, 1, 0) turned by
  // `rot` about n (FUN_00566400), r = up x n, v = r x n; without flip r is
  // negated; s = r.(x - A) / size + 0.5, t = v.(x - A) / size + 0.5 (the
  // engine-unit factors 10 and 0.1 / size cancel to metres).
  const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
  const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (!(len > 0.f) || size == 0.f) return false;
  const float inv = 1.f / len;
  const float n[3] = {-dx * inv, -dy * inv, -dz * inv};
  const float c = std::cos(rot), s = std::sin(rot), t = 1.f - c;
  const float up[3] = {t * n[0] * n[1] - s * n[2], t * n[1] * n[1] + c,
                       t * n[1] * n[2] + s * n[0]};
  float r[3] = {up[1] * n[2] - up[2] * n[1], up[2] * n[0] - up[0] * n[2],
                up[0] * n[1] - up[1] * n[0]};
  const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  if (rl != 0.f)
    for (float& x : r) x /= rl;
  const float v[3] = {r[1] * n[2] - r[2] * n[1], r[2] * n[0] - r[0] * n[2],
                      r[0] * n[1] - r[1] * n[0]};
  if (!flip)
    for (float& x : r) x = -x;
  PaintDecalJob j;
  j.mesh = mesh_key;
  j.target = target_key;
  j.brush = brush_key;
  j.flip = flip != 0;
  j.colour = argb;
  const float* basis[3] = {r, v, n};
  for (int row = 0; row < 3; ++row) {
    const float* e = basis[row];
    for (int k = 0; k < 3; ++k) j.rows[row][k] = e[k] / size;
    j.rows[row][3] = -(e[0] * a[0] + e[1] * a[1] + e[2] * a[2]) / size +
                     (row < 2 ? 0.5f : 0.f);
  }
  if (temp) g_paint_previews.push_back(j);
  else g_paint_decals.push_back(j);
  return true;
}

// FUN_00421760 -> FUN_00449b40, after the instances of a camera pass: each
// preview whose instance this camera drew, its sections again with the
// brush projected (the exe: texture coordinates from the camera-space
// position through inverse(world x view) x P; host: the same (s, t) per
// vertex on the CPU), no depth write, colour = brush.rgb x colour.rgb,
// alpha = brush.a.
void render_d3d9_paint_draw_previews(const std::vector<void*>& members) {
  if (g_paint_previews.empty() || !g_dev) return;
  std::unordered_set<void*> drawn(members.begin(), members.end());
  bool any = false;
  for (const PaintDecalJob& j : g_paint_previews)
    if (drawn.count(j.mesh)) any = true;
  if (!any) return;
  PaintDeviceSave save;
  save.save();
  g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  g_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  g_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  g_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
  g_dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS,
                              D3DTTFF_DISABLE);
  g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  g_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  g_dev->SetTexture(1, nullptr);
  g_dev->SetVertexShader(nullptr);
  g_dev->SetPixelShader(nullptr);
  g_dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
  struct PreVert {
    float x, y, z, s, t;
  };
  std::vector<PreVert> pv;
  for (const PaintDecalJob& j : g_paint_previews) {
    if (!drawn.count(j.mesh)) continue;
    const MeshState* ms = paint_mesh_of(j.mesh);
    auto bit = g_textures.find(j.brush);
    IDirect3DBaseTexture9* brush =
        bit != g_textures.end() ? texture_d3d_base(bit->second) : nullptr;
    if (!ms || !brush) continue;
    ++g_paint_rstats.previews;
    D3DMATRIX w{};
    paint_world_of(j.mesh, *ms, &w);
    g_dev->SetTransform(D3DTS_WORLD, &w);
    g_dev->SetTexture(0, brush);
    g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, j.colour);
    for (const MeshSubmesh& sm : ms->subs) {
      const size_t nv = paint_vertex_count(sm);
      if (nv == 0 || sm.indices.size() < 3) continue;
      // The cull of the lit sections (draw_lit_section): CW unless the
      // material is two-sided; the exe sets CULLMODE 2 here.
      g_dev->SetRenderState(D3DRS_CULLMODE, (sm.mat_flags & 0x02000000u)
                                                ? D3DCULL_NONE
                                                : D3DCULL_CW);
      pv.resize(nv);
      for (size_t i = 0; i < nv; ++i) {
        float p[3], uv[2], wp[3];
        paint_vertex(sm, i, p, uv);
        paint_xform(w, p[0], p[1], p[2], wp);
        pv[i] = {p[0], p[1], p[2],
                 j.rows[0][0] * wp[0] + j.rows[0][1] * wp[1] +
                     j.rows[0][2] * wp[2] + j.rows[0][3],
                 j.rows[1][0] * wp[0] + j.rows[1][1] * wp[1] +
                     j.rows[1][2] * wp[2] + j.rows[1][3]};
      }
      g_dev->DrawIndexedPrimitiveUP(
          D3DPT_TRIANGLELIST, 0, static_cast<UINT>(nv),
          static_cast<UINT>(sm.indices.size() / 3), sm.indices.data(),
          D3DFMT_INDEX16, pv.data(), sizeof(PreVert));
      ++g_paint_rstats.preview_sections;
    }
  }
  save.restore();
}

// After the camera passes of a frame: the strokes (FUN_00421690, at the end
// of the pass), then the fills (FUN_00421820, FUN_00421920); the previews
// of the frame are dropped (FUN_00421760 deletes each after drawing it).
void render_d3d9_paint_run_jobs() {
  g_paint_previews.clear();
  if (!g_dev || (g_paint_decals.empty() && g_paint_fills.empty())) return;
  ++g_paint_rstats.frames_with_jobs;
  const std::vector<PaintDecalJob> decals = std::move(g_paint_decals);
  g_paint_decals.clear();
  for (const PaintDecalJob& j : decals) {
    if (paint_run_decal(j)) ++g_paint_rstats.decals;
    else ++g_paint_rstats.decal_fail;
  }
  const std::vector<PaintFillJob> fills = std::move(g_paint_fills);
  g_paint_fills.clear();
  for (const PaintFillJob& j : fills) {
    if (paint_run_fill(j)) ++g_paint_rstats.fills;
    else ++g_paint_rstats.fill_fail;
  }
}

// PE 940 FUN_004ebb80 -> FUN_00457cb0(path): level 0 of a texture as a
// baseline JPEG, each texel's R, G, B (its alpha dropped), quality 95
// (DAT_005da334). A render target is read back through system memory.
// Fork: the encoder is Windows' (WIC, ImageQuality 0.95), not the exe's
// bundled one; the bytes differ, the picture is the same within the
// encoders' rounding.
bool render_d3d9_texture_save_jpeg(void* key, const char* path, int quality) {
  if (!key || !path || !path[0] || !g_dev) return false;
  auto it = g_textures.find(key);
  if (it == g_textures.end() || !it->second.tex) return false;
  IDirect3DTexture9* tex = it->second.tex;
  D3DSURFACE_DESC desc{};
  if (FAILED(tex->GetLevelDesc(0, &desc))) return false;
  IDirect3DSurface9* level = nullptr;
  if (FAILED(tex->GetSurfaceLevel(0, &level)) || !level) return false;
  IDirect3DSurface9* sys = nullptr;
  std::vector<uint8_t> bgr(static_cast<size_t>(desc.Width) * desc.Height * 3);
  bool ok = false;
  // A surface in another format (DXT) goes through D3DX into A8R8G8B8.
  if (SUCCEEDED(g_dev->CreateOffscreenPlainSurface(
          desc.Width, desc.Height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys,
          nullptr)) &&
      sys) {
    HRESULT hr = E_FAIL;
    if ((desc.Usage & D3DUSAGE_RENDERTARGET) != 0 &&
        desc.Format == D3DFMT_A8R8G8B8)
      hr = g_dev->GetRenderTargetData(level, sys);
    else if (PFN_D3DXLoadSurfaceFromSurface load = d3dx_load_surface_from_surface())
      hr = load(sys, nullptr, nullptr, level, nullptr, nullptr, 1, 0);
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(hr) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
      for (UINT y = 0; y < desc.Height; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch;
        uint8_t* dst = bgr.data() + static_cast<size_t>(y) * desc.Width * 3;
        for (UINT x = 0; x < desc.Width; ++x) {
          dst[x * 3 + 0] = row[x * 4 + 0];
          dst[x * 3 + 1] = row[x * 4 + 1];
          dst[x * 3 + 2] = row[x * 4 + 2];
        }
      }
      sys->UnlockRect();
      ok = true;
    }
    sys->Release();
  }
  level->Release();
  if (!ok) return false;
  static bool com_inited = false;  // as upload_jpeg_bgra's decoder
  if (!com_inited) {
    const HRESULT chr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(chr) || chr == RPC_E_CHANGED_MODE) com_inited = true;
  }
  IWICImagingFactory* factory = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
      !factory)
    return false;
  std::wstring wpath;
  for (const char* p = path; *p; ++p)
    wpath.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* enc = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  IPropertyBag2* props = nullptr;
  bool written = false;
  if (SUCCEEDED(factory->CreateStream(&stream)) &&
      SUCCEEDED(stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE)) &&
      SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) &&
      SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
      SUCCEEDED(enc->CreateNewFrame(&frame, &props))) {
    PROPBAG2 option{};
    wchar_t name[] = L"ImageQuality";
    option.pstrName = name;
    VARIANT value;
    std::memset(&value, 0, sizeof(value));  // VT_EMPTY, as VariantInit
    value.vt = VT_R4;
    value.fltVal = static_cast<float>(quality) / 100.f;
    props->Write(1, &option, &value);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(frame->Initialize(props)) &&
        SUCCEEDED(frame->SetSize(desc.Width, desc.Height)) &&
        SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
        IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR) &&
        SUCCEEDED(frame->WritePixels(desc.Height, desc.Width * 3,
                                     static_cast<UINT>(bgr.size()), bgr.data())) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit()))
      written = true;
  }
  if (props) props->Release();
  if (frame) frame->Release();
  if (enc) enc->Release();
  if (stream) stream->Release();
  factory->Release();
  return written;
}

void render_d3d9_paint_debug_dump(std::FILE* out) {
  if (!out) return;
  const PaintRenderStats& s = g_paint_rstats;
  std::fprintf(out,
               "[render-dbg] paint: copies=%d (failed %d) fills=%d (failed "
               "%d) strokes=%d (failed %d) sections=%d triangles=%d "
               "accepted=%d previews=%d (sections %d) frames=%u\n",
               s.copies, s.copy_fail, s.fills, s.fill_fail, s.decals,
               s.decal_fail, s.sections, s.triangles, s.accepted, s.previews,
               s.preview_sections, s.frames_with_jobs);
}

}  // namespace inv
