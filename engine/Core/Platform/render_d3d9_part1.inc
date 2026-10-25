// Fragment 1/3 of render_d3d9.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


namespace inv {
namespace {

#ifdef _WIN32
HWND g_hwnd = nullptr;
IDirect3D9* g_d3d = nullptr;
IDirect3DDevice9* g_dev = nullptr;
WNDCLASSEXA g_wc{};
bool g_class_reg = false;
HICON g_stock_icon = nullptr;
HCURSOR g_stock_cursors[16] = {};
int32_t g_stock_cursor_id = 2;
bool g_assets_ok = false;
bool g_quit_requested = false;

std::string module_dir() {
  char buf[MAX_PATH];
  const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (!n || n >= MAX_PATH) return {};
  std::string p(buf, n);
  const auto slash = p.find_last_of("\\/");
  if (slash == std::string::npos) return {};
  return p.substr(0, slash);
}

bool file_exists_a(const std::string& path) {
  const DWORD a = GetFileAttributesA(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string find_stock_assets_root() {
  // Prefer assets copied next to the exe (CMake POST_BUILD).
  const std::string exe = module_dir();
  auto try_root = [](const std::string& cand) -> std::string {
    if (cand.empty()) return {};
    if (file_exists_a(cand + "\\Icon\\0000\\1.ico") ||
        file_exists_a(cand + "/Icon/0000/1.ico"))
      return cand;
    return {};
  };
  if (!exe.empty()) {
    if (std::string r = try_root(exe + "\\assets\\StreetLegal_Redline_exe");
        !r.empty())
      return r;
    if (std::string r = try_root(exe + "/assets/StreetLegal_Redline_exe");
        !r.empty())
      return r;
    if (std::string r =
            try_root(exe + "\\..\\..\\assets\\StreetLegal_Redline_exe");
        !r.empty())
      return r;
  }
  const char* cwd_tails[] = {
      "native/engine/assets/StreetLegal_Redline_exe",
      "../native/engine/assets/StreetLegal_Redline_exe",
      "../../native/engine/assets/StreetLegal_Redline_exe",
      "assets/StreetLegal_Redline_exe",
  };
  for (const char* t : cwd_tails) {
    if (std::string r = try_root(t); !r.empty()) return r;
  }
  return {};
}

bool load_stock_window_assets() {
  if (g_assets_ok) return true;
  const std::string root = find_stock_assets_root();
  if (root.empty()) return false;

  std::string ico = root + "\\Icon\\0000\\1.ico";
  if (!file_exists_a(ico)) ico = root + "/Icon/0000/1.ico";
  if (file_exists_a(ico)) {
    g_stock_icon = static_cast<HICON>(LoadImageA(
        nullptr, ico.c_str(), IMAGE_ICON, 0, 0,
        LR_LOADFROMFILE | LR_DEFAULTSIZE));
  }

  // RT_CURSOR names extracted from stock exe: 2.cur … 11.cur
  for (int id = 2; id <= 11; ++id) {
    char name[32];
    std::snprintf(name, sizeof(name), "%d.cur", id);
    std::string cur = root + "\\Cursor\\0409\\" + name;
    if (!file_exists_a(cur)) cur = root + "/Cursor/0409/" + name;
    if (!file_exists_a(cur)) continue;
    g_stock_cursors[id] = LoadCursorFromFileA(cur.c_str());
  }
  g_assets_ok = g_stock_icon != nullptr || g_stock_cursors[2] != nullptr;
  return g_assets_ok;
}

void apply_stock_cursor_to_window() {
  HCURSOR cur = nullptr;
  if (g_stock_cursor_id >= 0 &&
      g_stock_cursor_id < static_cast<int32_t>(sizeof(g_stock_cursors) /
                                               sizeof(g_stock_cursors[0])))
    cur = g_stock_cursors[g_stock_cursor_id];
  if (!cur) cur = g_stock_cursors[2];
  if (!cur) cur = LoadCursor(nullptr, IDC_ARROW);
  if (g_hwnd) SetClassLongPtrA(g_hwnd, GCLP_HCURSOR, reinterpret_cast<LONG_PTR>(cur));
  SetCursor(cur);
}
#endif

int32_t g_w = 0;
int32_t g_h = 0;
int32_t g_mode = 0;
// Soft PE GfxDevice_presentCount @ 0x006495FC — ++ in GfxDevice_Present
// @ 0x004BB423 after D3D Present(0,0,0,0). FMV_DirectShow_Open @ 0x4F92AD
// and RunUpdateQuad @ 0x4F94E9 read it (parity / odd BeginScene); Soft
// count here matches Present path; FMV %2 BeginScene consumer OOS until
// video_fmv reads this (header export not in this ticket).
int32_t g_present_count = 0;

struct Mode {
  int32_t w;
  int32_t h;
  int32_t depth;
};
std::vector<Mode> g_modes;

struct ViewportState {
  int32_t pri = 0;
  // Host stand-in for PE viewport-rect (vtable+0xC after sub_5447D0
  // 0x80000001): +0x10 left, +0x14 top, +0x18 width, +0x1C height.
  // resize @ 0x00481700 FSTPs unboxed Java (FFFF) here with no video_* scale.
  float x = 0.f;
  float y = 0.f;
  float w = 1.f;
  float h = 1.f;
  int32_t pending_clear = 0;
  // Bound: on the exe's bound viewport list (PE 940 FUN_00421e40 bind /
  // FUN_00421f50 unbind). Every bound viewport is drawn each frame.
  bool active = false;
  // Fork: bind order stamp. FUN_00421e40 inserts a viewport in front of the
  // first bound one whose priority is >= its own, so among equal priorities
  // the most recently bound one is drawn first (underneath).
  uint32_t bind_seq = 0;
};
std::unordered_map<void*, ViewportState> g_viewports;
// Fork: the most recently activated viewport that is still bound; only the
// legacy single-viewport readers use it (apply_active_viewport, the world
// clear in render_d3d9_flush, render_d3d9_viewport_active).
void* g_active_vp = nullptr;
uint32_t g_vp_bind_seq = 0;

struct CameraState {
  void* parent = nullptr;
  void* viewport = nullptr;
  int32_t pri = 0;
  // PE 940 camera params+0x10 (FUN_00524dc0): the Camera.create aov, stored
  // unchanged. FUN_00461dd0 uses it as the FULL vertical angle in degrees.
  // Fork: legacy host cameras (apply_active_camera, project / unproject)
  // still read it as a half angle.
  float aov_deg = 45.f;
  // PE 940 params+0xc: aspect captured at Camera.create from the viewport's
  // pixel rect (FUN_004d8de0); 0 = use the viewport aspect of the frame.
  float aspect = 0.f;
  // Fork: registration stamp on the viewport's camera list (FUN_00464f60:
  // ascending camera priority, the newest first among equals).
  uint32_t seq = 0;
  // Fork: g_cam_pass_frame of the last frame in which draw_viewport_cameras
  // ran a pass for this camera (0 = never). render_d3d9_project /
  // render_d3d9_viewport_unproject use the pass's own view and projection
  // for such a camera and the legacy look-at model for the others.
  uint32_t pass_frame = 0;
  float dmin = 0.1f;
  float dmax = 100.f;
  // PE 940 params+0x24 / +0x1C / +0x20 / +0x28: on, near and far x 10, the
  // colour (render_d3d9_camera_set_fog; camera_pass_fog draws with them).
  bool fog_on = false;
  float fog_near = 0.f, fog_far = 0.f;
  uint32_t fog_colour = 0;
  float lod_bias = 1.f;
  float lod_amp = 1.f;
  int32_t oc = 1;
  int32_t pt = 0;
  bool active = false;
  // Phase 2.35 — look-at view (identity if unset).
  bool lookat = false;
  float eye_x = 0, eye_y = 2.f, eye_z = -8.f;
  float at_x = 0, at_y = 1.f, at_z = 0;
};
std::unordered_map<void*, CameraState> g_cameras;
void* g_active_cam = nullptr;
uint32_t g_cam_seq = 0;
// Fork: counts draw_viewport_cameras calls (one per flushed frame).
uint32_t g_cam_pass_frame = 0;

// Fork: what draw_viewport_cameras did in the last flushed frame
// (render_d3d9_debug_dump prints it; the log only under SLRR_RENDER_TRACE).
struct CamPassStats {
  int32_t bound_vps = 0;
  int32_t passes = 0;
  int32_t instances = 0;
  int32_t texts = 0;
  // Below a camera root but under a WORLDTREELEAF container (not drawn).
  int32_t hidden_instances = 0;
  int32_t hidden_texts = 0;
  // rg_ground: sections drawn, and texture entries that drew with the
  // engine default texture (unbound slot or a texture that did not load).
  int32_t ground_sections = 0;
  int32_t default_tex_binds = 0;          // every group
  int32_t ground_default_tex_binds = 0;   // rg_ground alone
  // rg_horizon sections, and the object sections drawn in material mode 3
  // (rg_object_prelit, rg_prelittransparent).
  int32_t horizon_sections = 0;
  int32_t prelit_mode3_sections = 0;
  int32_t prelit_transparent_mode3_sections = 0;
  // Passes whose camera saw a scene light (they do not use the stand-in).
  int32_t light_passes = 0;
  // Sections drawn with two bone matrices (draw_two_bone_section).
  int32_t two_bone_sections = 0;
};
CamPassStats g_cam_pass_stats;
std::vector<std::string> g_cam_pass_log;

struct FogState {
  bool enabled = false;
  int32_t color = 0;
  float near_z = 0.f;
  float far_z = 0.f;
};
FogState g_fog;

struct LightState {
  bool enabled = false;
  int32_t diffuse = 0x00ffffff;
  int32_t ambient = 0x00282c34;
  int32_t specular = 0x00ffffff;
};
LightState g_light;

struct FlareState {
  void* key = nullptr;
  void* tex = nullptr;
  int32_t color = 0xe4e4e4;
  float min_size = 1.f;
  float max_size = 10.f;
  int32_t count = 0;
  int32_t rays = 0;
  bool has_world = false;
  float wx = 0, wy = 0, wz = 0;
  bool has_screen = false;
  float sx = 0.55f, sy = 0.42f;
  bool behind = false;
};
std::vector<FlareState> g_flares;
int32_t g_flare_sprites_last = 0;
bool g_flares_enabled = true;

// PE RenderRef_applyLight @ 0x0048C9D0: byte * Light_byteToUnit (flt_5F13E0 = 1/256).
constexpr float kLightByteToUnit = 0.00390625f;

void rgb_bytes(int32_t rgb, float* r, float* g, float* b) {
  *r = static_cast<float>((rgb >> 16) & 0xff) * kLightByteToUnit;
  *g = static_cast<float>((rgb >> 8) & 0xff) * kLightByteToUnit;
  *b = static_cast<float>(rgb & 0xff) * kLightByteToUnit;
}

struct TextureState {
  std::string label;
  int32_t w = 0;
  int32_t h = 0;
  int32_t mips = 1;
  // PE GfxTexture+16 / CreateEmpty a5 — engine fmt index (Config.texture_format
  // or FormatFromFourcc). Used by soft SumMipBppWeight@4F6230.
  int32_t engine_fmt = 3;
  bool luma_alpha = false;
  // W27B/W28B — soft PE GfxTexture async mip (+0x1C level, +0x20 rows_done,
  // +0x24 surface, +0x28 flag, +0x2C src, +0x30 rows_step, +0x34 ddspf).
  // W28B adds Type2 rows_budget / payload_offset / level_index / finish_lod.
  int32_t mip_up_level = 0;
  int32_t mip_up_rows_done = 0;
  int32_t mip_up_rows_step = 0;
  int32_t mip_up_flag = 0;
  const uint8_t* mip_up_src = nullptr;
  uint32_t mip_up_fourcc = 0;
  bool mip_up_active = false;
  bool mip_level_ready = false;  // soft PE GfxTexture+13 byte after Poll done
#ifdef _WIN32
  IDirect3DTexture9* tex = nullptr;
  // W31B — soft PE GfxCubeTexture (+4) from CreateEmpty@4F6ED0.
  IDirect3DCubeTexture9* cube = nullptr;
  IDirect3DSurface9* mip_up_surf = nullptr;
#endif
};

#ifdef _WIN32
void texture_release_gpu(TextureState& st) {
  if (st.tex) {
    st.tex->Release();
    st.tex = nullptr;
  }
  if (st.cube) {
    st.cube->Release();
    st.cube = nullptr;
  }
}

// Soft PE: cube preferred over 2D for SetTexture (IDirect3DBaseTexture9*).
IDirect3DBaseTexture9* texture_d3d_base(const TextureState& st) {
  if (st.cube) return st.cube;
  return st.tex;
}

// The engine default texture (PE 940 C3DLow+0x340), bound for a texture
// entry that is unbound or not ready (FUN_0043d990). Defined in
// render_d3d9_part4.inc; released with the device.
IDirect3DBaseTexture9* default_texture();
void default_texture_release();
// The shadow target and ramp (render_d3d9_part7.inc), released with the
// device.
void shadow_release();

#ifdef _WIN32
// Soft PE envmap stage1 bind (setGlobalEnvmap@47C220 = store only).
// Present: COLOROP ADD @ PE material path 4CE53A (TSS stage1 op=7);
// TEXCOORDINDEX = D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR (0x30000) @
// PE 4E7718 / 4D9126 (SetTextureStageState vt+0x10C).
void apply_envmap_stage1(IDirect3DBaseTexture9* env_tex) {
  if (!g_dev) return;
  if (env_tex) {
    g_dev->SetTexture(1, env_tex);
    g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD);
    g_dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
    g_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX,
                                D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
  } else {
    g_dev->SetTexture(1, nullptr);
    g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0);
  }
}
#endif

// W35-02 — soft freelists for GfxDevice_DrainGpuCaches @ 0x4B7B90.
// PE managers live at g_GfxDevice+0x33C (tex) / +0x320 (VB) / +0x324 (IB);
// destroy enqueues COM ptrs; Drain*(force=1) Release+clear. Live-key
// replace paths keep immediate texture_release_gpu / VB|IB->Release.
std::vector<IDirect3DBaseTexture9*> g_tex_drain_list;
std::vector<IDirect3DVertexBuffer9*> g_vb_drain_list;
std::vector<IDirect3DIndexBuffer9*> g_ib_drain_list;

void texture_enqueue_drain(TextureState& st) {
  if (st.tex) {
    g_tex_drain_list.push_back(st.tex);
    st.tex = nullptr;
  }
  if (st.cube) {
    g_tex_drain_list.push_back(st.cube);
    st.cube = nullptr;
  }
}

void drain_texture_cache_force() {
  // Soft PE GfxDevice_DrainTextureCache @ 0x4F8240 with a2=1 (no budget).
  for (IDirect3DBaseTexture9* p : g_tex_drain_list) {
    if (p) p->Release();
  }
  g_tex_drain_list.clear();
}

void drain_vb_slots_force() {
  // Soft PE GfxDevice_DrainVertexBufferSlots @ 0x4ECDB0 with a2=1.
  for (IDirect3DVertexBuffer9* p : g_vb_drain_list) {
    if (p) p->Release();
  }
  g_vb_drain_list.clear();
}

void drain_ib_slots_force() {
  // Soft PE GfxDevice_DrainIndexBufferSlots @ 0x4EBDF0 with a2=1.
  for (IDirect3DIndexBuffer9* p : g_ib_drain_list) {
    if (p) p->Release();
  }
  g_ib_drain_list.clear();
}
#endif
std::unordered_map<void*, TextureState> g_textures;
// Host analogue of g_GfxEngine/off_6187B0+0x64 (PE @ 0x0047C220).
// setGlobalEnvmap does not SetTexture; store only — do not bind here.
void* g_envmap = nullptr;

// W27B — release GetSurfaceLevel hold (PE Begin@4F6880 Release prior +0x24).
void texture_clear_async_mip(TextureState& st) {
#ifdef _WIN32
  if (st.mip_up_surf) {
    st.mip_up_surf->Release();
    st.mip_up_surf = nullptr;
  }
#endif
  st.mip_up_level = 0;
  st.mip_up_rows_done = 0;
  st.mip_up_rows_step = 0;
  st.mip_up_flag = 0;
  st.mip_up_src = nullptr;
  st.mip_up_fourcc = 0;
  st.mip_up_active = false;
}

// PE Poll@4F691C: DXT1 fourcc → 2 else 4 bytes/texel for src pitch.
int32_t async_mip_src_bpp(uint32_t fourcc, int32_t engine_fmt) {
  if (fourcc == 0x31545844u) return 2;  // 'DXT1'
  if (fourcc != 0) return 4;
  if (engine_fmt == 6) return 2;
  return 4;
}

struct OsdRect {
  void* key = nullptr;
  float x = 0.f;
  float y = 0.f;
  float w = 2.f;
  float h = 2.f;
  void* texture = nullptr;
  int32_t pri = 0;
  uint32_t color = 0xFFFFFFFFu;
  bool visible = true;
};
std::vector<OsdRect> g_osd;

struct FontGlyph {
  // Legacy screen pass (draw_osd_texts): UV box and its width in atlas
  // pixels as the advance.
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
  float adv_px = 0;
  // PE 940: glyph c = vertices c*4 .. c*4+3 of the charset mesh, kept as
  // stored (FUN_0044bdc0 copies them in file order; fade.SCX glyph 'a' has
  // vertex 2 == vertex 0). Positions are the SCX values * 0.1 (INVO v3
  // loader FUN_00451b00, DAT_005935d4) = pixels; uv from vertex+24.
  float px[4] = {0, 0, 0, 0};
  float py[4] = {0, 0, 0, 0};
  float tu[4] = {0, 0, 0, 0};
  float tv[4] = {0, 0, 0, 0};
  // Pen advance = vertex[c*4+1].x, signed (FUN_0044bdc0 last line, measure
  // FUN_0044bfb0).
  float advance = 0;
};

struct FontState {
  std::string name;
  void* atlas_key = nullptr;
  int32_t atlas_w = 0;
  int32_t atlas_h = 0;
  int32_t px_height = 20;
  std::string charset;
  std::vector<FontGlyph> glyphs;
  bool ready = false;
  // PE 940 text flush FUN_0044cb60: the texture is entry 0x6000000 of the
  // charset section's material, none when the material has no such entry.
  // The INVO v3 builder FUN_0042ba10 files the map under 0x6000001 when the
  // header flags & 0x101 == 0x100 (fade.SCX), and the material made for it
  // (shader 3, CMaterial_ColorMixTex, factory FUN_004297e0) declares the
  // texture entries 0x6000001 / 4 / 5 / 7 only.
  bool no_texture = false;
};
std::unordered_map<void*, FontState> g_fonts;

struct TextState {
  void* font = nullptr;
  float x = 0.f;
  float y = 0.f;
  uint32_t color = 0xFFFFFFFFu;
  int32_t align = 2;  // LEFT
  std::string text;
  bool alive = false;
  bool visible = true;
  // Tree text: created under a parent node (PE 940 Text.create FUN_004e5b30
  // -> r_text render instance FUN_004e8f40). It is drawn only by the pass
  // of a camera whose root is above it (rg_text_section) and never by the
  // legacy draw_osd_texts.
  bool tree = false;
  // PE 940 payload+0x178 -> RI_text+0xd8: multiplies the pen advance only
  // (FUN_0044bdc0 param_7); 1.0 for r_text.
  float adv_scale = 1.f;
  // Fork: Text.setScale(sx, sy), see java_render_Text_setScale.
  float mesh_sx = 1.f;
  float mesh_sy = 1.f;
  // Fork: creation stamp (stable order inside a text section).
  uint32_t seq = 0;
};
std::unordered_map<void*, TextState> g_texts;
uint32_t g_text_seq = 0;

struct OsdText {
  void* text_key = nullptr;
  void* font = nullptr;
  float x = 0.f;
  float y = 0.f;
  uint32_t color = 0xFFFFFFFFu;
  int32_t align = 2;
  std::string text;
  int32_t pri = 0;
  bool visible = true;
};
std::vector<OsdText> g_osd_text;

struct MeshVertex {
  float x, y, z;
  float nx, ny, nz;
  float u, v;
};

// PE 940 vertex stream 0x15003f (FUN_004547a0, filled by FUN_00455450 from
// the staging block DAT_005ef100..): position, normal, D3D diffuse, D3D
// specular and the uv sets. The exe writes texcoord set i = uv[channel of
// material stage i] while it builds the vertices (RI_ground FUN_004658b0,
// v4 block FUN_00454940).
// Fork: the host keeps the three uv sets in file order and selects the
// channel per stage with D3DTSS_TEXCOORDINDEX (same picture).
// FVF: XYZ | NORMAL | DIFFUSE | SPECULAR | TEX3, 56 bytes.
struct MeshVertexWide {
  float x, y, z;
  float nx, ny, nz;
  uint32_t diffuse;
  uint32_t specular;
  float uv[3][2];
};
static_assert(sizeof(MeshVertexWide) == 56, "wide vertex is 14 dwords");

struct MeshSubmesh {
  std::string name;
  std::vector<MeshVertex> verts;
  std::vector<uint16_t> indices;
  // Optional: the same vertices with both colours and every uv set (empty
  // when the source has neither a colour nor a second uv set). Material
  // mode 3 (draw_mode3_section, render_d3d9_part4.inc) draws from it.
  std::vector<MeshVertexWide> wide;
  void* texture_key = nullptr;  // entry in g_textures (owned via mesh)
  std::string texture_path;
  uint32_t diffuse = 0xFF808080u;
  // Section material as the exe reads it (camera pass only; the legacy
  // draw_meshes keeps using `diffuse`). INVO v4 material chunk (PE 940
  // FUN_0042c340): +0x08 shader id, +0x0C create flags, +0x10 entry count,
  // entries from +0x14; INVO v3 header through FUN_0042ba10.
  // Create flags: 0x40000000 transparent (material+8 bit0 -> rg_transparent),
  // 0x80000000 prelit (bit1, lighting off), 0x20000000 alpha test,
  // 0x02000000 two-sided.
  bool has_mat = false;
  uint32_t mat_flags = 0;
  // Colour id 0 (ARGB) as the material ends up with it: Diffuse = Ambient,
  // alpha -> TEXTUREFACTOR. Opaque white for the Tex family whatever the
  // file says (scx_shader_white_diffuse).
  uint32_t mat_diffuse = 0xFFFFFFFFu;
  uint32_t mat_emissive = 0xFF000000u;  // colour id 2: Emissive
  uint32_t mat_alpha_ref = 0x38;        // entry 0x5000000 (FUN_004412c0)
  // Shader id & 0xffffff as the factory FUN_0042bff0 takes it (0 =
  // CMaterial_Tex): the v4 chunk's own, or the one the v3 builder
  // FUN_0042ba10 picks. -1 without a parsed material.
  int32_t mat_shader = -1;
  // Texture entries of the material (PE 940: 0x24-byte entries, id
  // 0x06000000 | kind; entry+0x0C = slot, +0x10 = uv channel). The slot is
  // the zero-based index into the `texture` lines of the render object that
  // uses the mesh (FUN_00441c40); -1 = the material has no such entry.
  // v3 header (FUN_0042ba10): +0x2C primary (kind 0), +0x34 second (kind
  // 1), +0x32 sphere / env map (kind 3), +0x38 lightmap (kind 7, always on
  // uv channel 1); uv channels +0x46 (primary) and +0x48 (second).
  int16_t slot_primary = -1;
  int16_t slot_second = -1;
  int16_t slot_env = -1;
  int16_t slot_lightmap = -1;
  int16_t chan_primary = 0;
  int16_t chan_second = 0;
  int16_t chan_lightmap = 1;
  // Address bits of each entry (in-memory entry dword [2], v4 file entry
  // +0x0C): 1 = wrap U, 2 = wrap V. FUN_0043d990 sets D3DTADDRESS_CLAMP on
  // an axis whose bit is clear when it binds the entry; the frame default
  // is WRAP. 3 for a v3 material: FUN_00441330, which makes the entries of
  // FUN_0042ba10, writes 3.
  uint8_t addr_primary = 3;
  uint8_t addr_second = 3;
  uint8_t addr_env = 3;
  uint8_t addr_lightmap = 3;
  // uv transform of each entry: scaleU, scaleV, offsetU, offsetV (in-memory
  // entry dwords [5..8], v4 file entry +0x10..+0x1C; 1, 1, 0, 0 for a v3
  // material). The vertex loaders apply it while they write the texcoord
  // set of the entry's stage (v4 FUN_00454940, ground FUN_004658b0):
  //   u' = ((u - 0.5) - offsetU) * scaleU + 0.5
  //   v' = 0.5 - ((0.5 - v) - offsetV) * scaleV
  // parse_scx_v4 does the same at load, so the draw never reads these.
  float uvx_primary[4] = {1.f, 1.f, 0.f, 0.f};
  float uvx_second[4] = {1.f, 1.f, 0.f, 0.f};
  float uvx_lightmap[4] = {1.f, 1.f, 0.f, 0.f};
  // Keys in g_textures for the second, env and lightmap entries, beside
  // `texture_key` (the primary). Null = unbound: the draw then uses the
  // engine default texture (C3DLow+0x340, FUN_0043d990).
  void* texture_second = nullptr;
  void* texture_env = nullptr;
  void* texture_lightmap = nullptr;
  // True once render_d3d9_mesh_bind_slots bound the entries from a render
  // object's texture list. The file-name heuristic bind_mesh_textures
  // leaves such a section alone.
  bool slot_bound = false;
  // Ground mesh: index of the section's material in the material table
  // (the draw order of rg_ground). -1 for every other mesh.
  int32_t mat_index = -1;
  // INVO v4 section chunk (PE 940 FUN_00452960; section+0x14 / +8 / +0xC):
  // type 1 = rigid (mode 0), type 2 {dword, boneA, boneB} = two bones (mode
  // 1), type 3 {dword, n, bone[n]} = a bone list (mode -1); and the first
  // blend weight of every vertex (vertex format bit 0x4), empty without
  // one. Read on the Build 940 boot only.
  int32_t bone_mode = 0;
  std::vector<int32_t> bone_idx;
  std::vector<float> weights;
  // The file's colour and float entries as the class programs of the lit
  // groups read them (render_d3d9_part6.inc): colour id 0 (r, g, b, a) and
  // its packed dword, id 1 (specular), id 2, float 0x1000000 (power), float
  // 0x1000001 (env factor). Defaults = the class constructors'.
  float col0[4] = {1.f, 1.f, 1.f, 1.f};
  uint32_t col0_packed = 0xFFFFFFFFu;
  float col1[3] = {1.f, 1.f, 1.f};
  float col2[3] = {0.f, 0.f, 0.f};
  float power = 16.f;
  float env_factor = 0.5f;
};

// PE 940 mesh bone (0xA4 bytes at mesh+0x120, count mesh+0x11C), read from
// `<mesh>.bon` (render_d3d9_mesh_load_bones): +0x00 parent, +0x04 name,
// +0x24 the 4x4 bind matrix with the position in its fourth row. Host: the
// position is in file units, like the vertices.
struct MeshBone {
  std::string name;
  int32_t parent = -1;
  float bind[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
                    0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
};

// PE 940 render group a mesh's sections are drawn in when the group does not
// come from the section material: rg_ground (cam+0x528, RT_ground: render
// object flags 0x800) and rg_horizon (cam+0x890, render object flags 0x1).
enum MeshGroup : int32_t {
  kMeshGroupObject = 0,  // rg_object / rg_object_prelit / the blended groups
  kMeshGroupGround = 1,
  kMeshGroupHorizon = 2,
};

struct MeshState {
  std::string label;
  std::vector<MeshSubmesh> subs;
  float bmin[3] = {0, 0, 0};
  float bmax[3] = {0, 0, 0};
  bool ready = false;
  // Fork: creation stamp of the instance (stable order in the camera pass).
  uint32_t seq = 0;
  // Fork: file units -> script metres, multiplied into the 3x3 of every
  // section's world matrix. 0.01 for an SCX mesh: INVO positions are
  // centimetres (the exe multiplies them by 0.1 into its decimetres at
  // load, FUN_00454940 / FUN_00451b00). 1.0 for a mesh built from POLY
  // data, which is metres (the exe multiplies by 10.0, FUN_004658b0).
  float unit = 0.01f;
  int32_t group = kMeshGroupObject;
  // Ground mesh only: the material table (PE 940 mesh+0x60 / +0x64, loader
  // FUN_00451130), one entry per header of the render object's `mesh`
  // file, without geometry. A section copies its material from here.
  std::vector<MeshSubmesh> materials;
  // The skeleton of a mesh whose type record has `flags 8`; empty for
  // every other mesh, which is then drawn rigid whatever its section
  // chunks say (FUN_00465e40 / FUN_00469990: RI+0xD0 == 0).
  std::vector<MeshBone> bones;
  // Fork: stamp of the section geometry, new when a mesh file is parsed or
  // its vertices are scaled, copied by a clone; 0 = none (shadow_geom).
  uint32_t geom_rev = 0;
};
std::unordered_map<void*, MeshState> g_meshes;
uint32_t g_mesh_seq = 0;
uint32_t g_mesh_geom_rev = 0;
// Fork: copy -> the mesh it shows (render_d3d9_mesh_set_geometry_source).
std::unordered_map<void*, void*> g_mesh_geom_src;
// PE 940 render type `flags` (typeof-14 rsd line -> payload+0x34 ->
// template+0x18, FUN_00522d90 / FUN_0046e1a0), per render type and per
// instance made from it (render_d3d9_mesh_set_type_flags). Absent = 0.
std::unordered_map<void*, uint32_t> g_mesh_type_flags;
std::vector<void*> g_mesh_queue;
// Fork: render instances whose render type was not loaded when
// RenderRef.create ran (script boot only). PE 940: the type-3 builder
// FUN_00462b40 requests the type when a camera walk first meets the
// instance (see render_d3d9_mesh_set_pending_type).
struct MeshPending {
  int32_t type_id = 0;
  // 0 waiting for a camera; 1 the type is not a mesh recipe (a typeof-13
  // light: kept for the light pass); 2 the type could not be loaded.
  // 1 and 2 are not asked again.
  int32_t state = 0;
  uint32_t seq = 0;  // creation order
};
std::unordered_map<void*, MeshPending> g_mesh_pending;
uint32_t g_mesh_pending_seq = 0;
// PE 940 cLight (0x108 bytes, FUN_00458740): a render instance whose
// resource is a light type (render_d3d9_scene_light_set,
// render_d3d9_part4.inc). Keyed by the instance.
struct SceneLight {
  int32_t type_id = 0;
  RenderLightDesc desc;
  uint32_t seq = 0;  // the order the lights were met
};
std::unordered_map<void*, SceneLight> g_scene_lights;
uint32_t g_scene_light_seq = 0;

// W26B — soft PE AsyncLoad_Mesh_UploadVbFvf @ 0x503400 wrapper (malloc 0x68):
//   +0x04 vert_count, +0x08 CPU blob, +0x0C pe_fvf, +0x1C layout
//   (GfxVbLayout_FromFvf@4FE980), stride @ +0x60.
struct MeshVbState {
  int32_t vert_count = 0;
  int32_t stride = 0;
  uint32_t pe_fvf = 0;
  std::vector<uint8_t> cpu;
  bool ready = false;
#ifdef _WIN32
  IDirect3DVertexBuffer9* vb = nullptr;
#endif
};
std::unordered_map<void*, MeshVbState> g_mesh_vbs;

// W26B — soft PE AsyncLoad_Mesh_UploadIbTris @ 0x5038E0 wrapper (malloc 20):
//   [0]=count [1]=blob [2]=type=2 [3]=stride=2; u16 triangle list.
// Present DrawIndexed @ 4ED080: PrimCount = count/3 (soft); BaseVertex/
// StartIndex = 0 (PE args a5/a6 when non-zero — host default 0).
struct MeshIbState {
  int32_t index_count = 0;
  std::vector<uint16_t> cpu;
  bool ready = false;
#ifdef _WIN32
  IDirect3DIndexBuffer9* ib = nullptr;
#endif
};
std::unordered_map<void*, MeshIbState> g_mesh_ibs;

// PE ResPayload_createBoneParentHook @ 0x0053F7E0 — Engine_malloc(0x1C).
// Layout (asm 0x53f81a..0x53f8a3):
//   +0x00 vtbl (off_5F355C; dtor BoneParentHook_dtor @ 0x45F8B0)
//   +0x04/+0x08 payload+0x78 dllist (fwd→sentinel, back→old tail)
//   +0x0C..+0x18 ResHandle_Link slice (a2 = hook+0x0C → node+0x44)
struct BoneParentHook {
  void* vtbl = nullptr;
  void* list_fwd = nullptr;   // +0x04
  void* list_back = nullptr;  // +0x08
  void* link0 = nullptr;      // +0x0C
  void* link1 = nullptr;      // +0x10
  int32_t node_field50 = 0;   // +0x14 ← *(node+0x50)
  void* owner_node = nullptr; // +0x18
};
static_assert(sizeof(BoneParentHook) == 0x1C, "PE hook size 0x1C");

// PE bone node (LinkOrUnlinkBone a3 / RenderPayload_findBoneById). Raw blob so
// +0x0C/+0x10 dllist, +0x44 ResHandle, +0x4C (=RH+8), +0xAC hook match PE.
struct PeBoneNode {
  uint8_t raw[0xB0]{};
};
static_assert(sizeof(PeBoneNode) == 0xB0, "bone through +0xAC");

// PE payload own-list arena: sentinel @ +0x168, head @ +0x178 (head ==
// sentinel+0x10). HostMidPayload stops ~0x88 — side map, same offsets.
struct PeBoneOwnList {
  uint8_t raw[0x17C]{};
  PeBoneOwnList() {
    void* sent = raw + 0x168;
    *reinterpret_cast<void**>(raw + 0x178) = sent;
    *reinterpret_cast<void**>(reinterpret_cast<char*>(sent) + 0x0C) = sent;
  }
};

struct MeshXform {
  float px = 0, py = 0, pz = 0;
  float oy = 0, op = 0, or_ = 0;
  float sx = 1, sy = 1, sz = 1;
  void* parent = nullptr;
  // When parent is set: Local * BoneLocal(parent, attach_bone) * ParentWorld.
  int32_t attach_bone = 0;
  // Fork: creation parent (RenderRef.create(parent,type,alias)). The PE keeps
  // the render instance in its parent's tree independently of the bone
  // link; setMatrix(pos,ori) (bone_ref=0) unlinks the bone parent only.
  void* tree_parent = nullptr;
  // PE LinkOrUnlinkBone bone[+0xAC] = ResPayload_createBoneParentHook result.
  BoneParentHook* bone_parent_hook = nullptr;
  // Host PeBone for a4==0 / Rebind / indexed-slot splice (not InvObject*).
  PeBoneNode* pe_bone = nullptr;
  // PE setColor @ 0x00480310 → slot+0xCC DWORD as-is (no 1/256).
  int32_t color = 0;
  int32_t color_set = 0;
};
std::unordered_map<void*, PeBoneOwnList> g_bone_own_lists;
std::unordered_map<void*, MeshXform> g_mesh_xforms;
// Per-mesh bone local poses (id 0 = root / identity unless written).
std::unordered_map<void*, std::unordered_map<int32_t, MeshXform>> g_mesh_bones;
// Per-mesh alias → bone id (0 reserved for bone00/root).
std::unordered_map<void*, std::unordered_map<std::string, int32_t>> g_mesh_bone_alias;
std::unordered_map<void*, int32_t> g_mesh_bone_next;  // next id to allocate (>=1)

#ifdef _WIN32
void mat_identity(D3DMATRIX* m) {
  std::memset(m, 0, sizeof(*m));
  m->m[0][0] = m->m[1][1] = m->m[2][2] = m->m[3][3] = 1.f;
}

void mat_mul(const D3DMATRIX& a, const D3DMATRIX& b, D3DMATRIX* out) {
  D3DMATRIX r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] +
                  a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
  *out = r;
}

void build_local_world(const MeshXform& xf, D3DMATRIX* out) {
  const float cy = std::cos(xf.oy), sy = std::sin(xf.oy);
  const float cp = std::cos(xf.op), sp = std::sin(xf.op);
  const float cr = std::cos(xf.or_), sr = std::sin(xf.or_);
  // R = Ry(yaw) * Rx(pitch) * Rz(roll)
  const float r00 = cy * cr + sy * sp * sr;
  const float r01 = cp * sr;
  const float r02 = -sy * cr + cy * sp * sr;
  const float r10 = -cy * sr + sy * sp * cr;
  const float r11 = cp * cr;
  const float r12 = sy * sr + cy * sp * cr;
  const float r20 = sy * cp;
  const float r21 = -sp;
  const float r22 = cy * cp;
  // Local = Scale * R * Translation  (row-vector: v*S*R*T)
  D3DMATRIX& m = *out;
  std::memset(&m, 0, sizeof(m));
  m.m[0][0] = r00 * xf.sx;
  m.m[0][1] = r01 * xf.sx;
  m.m[0][2] = r02 * xf.sx;
  m.m[1][0] = r10 * xf.sy;
  m.m[1][1] = r11 * xf.sy;
  m.m[1][2] = r12 * xf.sy;
  m.m[2][0] = r20 * xf.sz;
  m.m[2][1] = r21 * xf.sz;
  m.m[2][2] = r22 * xf.sz;
  m.m[3][0] = xf.px;
  m.m[3][1] = xf.py;
  m.m[3][2] = xf.pz;
  m.m[3][3] = 1.f;
}

// World = Local * ParentWorld (row-vector hierarchy).
// If attach_bone != 0 (or bone 0 has an explicit pose): insert bone local of parent.
// Fork: during the per-viewport camera pass an instance's own bone00 local
// (where RenderRef.setMatrix(pos, ypr) stores a script instance's placement)
// is folded into its world transform.
static bool g_fold_own_bone0 = false;

bool resolve_world(void* key, D3DMATRIX* out, int depth) {
  if (!out) return false;
  if (!key || depth > 32) {
    mat_identity(out);
    return false;
  }
  MeshXform xf;
  auto it = g_mesh_xforms.find(key);
  if (it != g_mesh_xforms.end()) xf = it->second;
  D3DMATRIX local{};
  build_local_world(xf, &local);
  if (g_fold_own_bone0) {
    auto bit = g_mesh_bones.find(key);
    if (bit != g_mesh_bones.end()) {
      auto b0 = bit->second.find(0);
      if (b0 != bit->second.end()) {
        D3DMATRIX own{};
        build_local_world(b0->second, &own);
        D3DMATRIX tmp{};
        mat_mul(own, local, &tmp);
        local = tmp;
      }
    }
  }
  void* const xparent = xf.parent ? xf.parent : xf.tree_parent;
  if (!xparent) {
    *out = local;
    return true;
  }
  D3DMATRIX parent_world{};
  resolve_world(xparent, &parent_world, depth + 1);
  D3DMATRIX bone_local{};
  mat_identity(&bone_local);
  auto bit = g_mesh_bones.find(xparent);
  if (bit != g_mesh_bones.end()) {
    auto b2 = bit->second.find(xf.attach_bone);
    if (b2 != bit->second.end()) build_local_world(b2->second, &bone_local);
  }
  D3DMATRIX bone_world{};
  mat_mul(bone_local, parent_world, &bone_world);
  mat_mul(local, bone_world, out);
  return true;
}
#endif

void ensure_modes() {
  if (!g_modes.empty()) return;
  g_modes.push_back({800, 600, 32});
  g_modes.push_back({1024, 768, 32});
  g_modes.push_back({1280, 720, 32});
  g_modes.push_back({1920, 1080, 32});
}

#ifdef _WIN32
LRESULT CALLBACK render_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
      // PE Engine_WndProc @ 0x004B8000: 0x200-0x202 and 0x204-0x205 only
      // (not L/R DBLCLK). Store NDC then DefWindowProc.
      input_wndproc_mouse(hwnd, msg, wp, lp);
      break;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        apply_stock_cursor_to_window();
        return TRUE;
      }
      break;
    case WM_DESTROY:
      g_quit_requested = true;
      PostQuitMessage(0);
      return 0;
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    default:
      break;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

// The depth format the device was made with (render_d3d9_debug_dump).
D3DFORMAT g_depth_format = D3DFMT_D16;
// The behaviour flags the device was made with (render_d3d9_debug_dump).
DWORD g_create_flags = 0;

// PE 940 FUN_00426b10(back buffer format), called by the device creation
// FUN_00425a30 for D3DPRESENT_PARAMETERS.AutoDepthStencilFormat (and by the
// mode change FUN_00422ec0): the first entry of a seven-format table that
// passes CheckDeviceFormat(D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE) and
// CheckDepthStencilMatch. In a 32-bit mode (C3DLow+0x1C0 == 32, table
// 0x593E60): D32, D24X8, D24S8, D24X4S4, D16_LOCKABLE, D16, D15S1.
// Otherwise (table 0x593E50): D16_LOCKABLE, D16, D15S1, D32, D24X8, D24S8,
// D24X4S4. The adapter format of both checks is X8R8G8B8 for an A8R8G8B8
// back buffer, else the back buffer format. (M2 spec Q3.)
// Fork: the host's back buffer takes the desktop format (windowed,
// D3DFMT_UNKNOWN), so the desktop format decides the table and is the
// render target format of the match; when nothing passes the host takes
// D16 (the exe reads past its table).
// Why it matters: D16 with the garage camera's near plane (0.05 m) resolves
// 3 cm at 10 m and 11 cm at 20 m, where lockers, poles and adverts stand
// centimetres off the back wall.
D3DFORMAT pick_depth_format(D3DDEVTYPE dev_type) {
  D3DDISPLAYMODE mode{};
  if (!g_d3d ||
      FAILED(g_d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode)))
    return D3DFMT_D16;
  static const D3DFORMAT kMode32[7] = {
      D3DFMT_D32,          D3DFMT_D24X8, D3DFMT_D24S8, D3DFMT_D24X4S4,
      D3DFMT_D16_LOCKABLE, D3DFMT_D16,   D3DFMT_D15S1};
  static const D3DFORMAT kMode16[7] = {
      D3DFMT_D16_LOCKABLE, D3DFMT_D16,   D3DFMT_D15S1, D3DFMT_D32,
      D3DFMT_D24X8,        D3DFMT_D24S8, D3DFMT_D24X4S4};
  const bool mode32 =
      mode.Format == D3DFMT_X8R8G8B8 || mode.Format == D3DFMT_A8R8G8B8;
  const D3DFORMAT* table = mode32 ? kMode32 : kMode16;
  const D3DFORMAT adapter_fmt =
      mode.Format == D3DFMT_A8R8G8B8 ? D3DFMT_X8R8G8B8 : mode.Format;
  for (int i = 0; i < 7; ++i) {
    const D3DFORMAT f = table[i];
    if (FAILED(g_d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, dev_type,
                                        adapter_fmt, D3DUSAGE_DEPTHSTENCIL,
                                        D3DRTYPE_SURFACE, f)))
      continue;
    if (FAILED(g_d3d->CheckDepthStencilMatch(D3DADAPTER_DEFAULT, dev_type,
                                             adapter_fmt, mode.Format, f)))
      continue;
    return f;
  }
  return D3DFMT_D16;
}

const char* depth_format_name(D3DFORMAT f) {
  switch (f) {
    case D3DFMT_D32: return "D32";
    case D3DFMT_D24X8: return "D24X8";
    case D3DFMT_D24S8: return "D24S8";
    case D3DFMT_D24X4S4: return "D24X4S4";
    case D3DFMT_D16_LOCKABLE: return "D16_LOCKABLE";
    case D3DFMT_D16: return "D16";
    case D3DFMT_D15S1: return "D15S1";
    default: return "?";
  }
}

bool create_device(HWND hwnd, int32_t w, int32_t h) {
  if (g_dev) {
    g_dev->Release();
    g_dev = nullptr;
  }
  if (!g_d3d) {
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) {
      std::fprintf(stderr, "[render] Direct3DCreate9 failed\n");
      return false;
    }
  }

  D3DPRESENT_PARAMETERS pp{};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_UNKNOWN;
  pp.BackBufferWidth = static_cast<UINT>(w);
  pp.BackBufferHeight = static_cast<UINT>(h);
  pp.hDeviceWindow = hwnd;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = pick_depth_format(D3DDEVTYPE_HAL);
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  // PE 940 FUN_00425a30: IDirect3D9::CreateDevice(adapter, D3DDEVTYPE_HAL,
  // window, flags, ..) with the behaviour flags 0x44, then 0x84, then 0x24:
  // hardware, mixed, software vertex processing (0x40 / 0x80 / 0x20), each
  // with D3DCREATE_MULTITHREADED (0x04); "Create hw 3d device FAILED" when
  // all three fail. (The host asked for software vertex processing only,
  // which transforms and lights every vertex on the CPU: 94 ms a frame in
  // the garage once the lift arm is drawn.)
  // Literal values: the thin SDK headers some builds use lack the names.
  constexpr DWORD kCreateFlags[3] = {0x00000044L, 0x00000084L, 0x00000024L};
  g_create_flags = 0;
  auto create_hal = [&]() {
    HRESULT r = E_FAIL;
    for (const DWORD flags : kCreateFlags) {
      r = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, flags,
                              &pp, &g_dev);
      if (SUCCEEDED(r)) {
        g_create_flags = flags;
        break;
      }
    }
    return r;
  };
  HRESULT hr = create_hal();
  if (FAILED(hr) && pp.AutoDepthStencilFormat != D3DFMT_D16) {
    // The format the device was made with before the depth request.
    pp.AutoDepthStencilFormat = D3DFMT_D16;
    hr = create_hal();
  }
  if (FAILED(hr)) {
    // Fork: the reference rasteriser as a last resort (the exe stops).
    pp.AutoDepthStencilFormat = D3DFMT_D16;
    hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd,
                             0x00000020L, &pp, &g_dev);
    if (SUCCEEDED(hr)) g_create_flags = 0x00000020L;
  }
  if (SUCCEEDED(hr) && g_dev) g_depth_format = pp.AutoDepthStencilFormat;
  if (FAILED(hr) || !g_dev) {
    std::fprintf(stderr, "[render] CreateDevice failed hr=0x%08lX\n",
                 static_cast<unsigned long>(hr));
    return false;
  }
  g_w = w;
  g_h = h;
  // Soft: new IDirect3DDevice9 → presentCount restart (PE GfxDevice ctor
  // path zeros GfxDevice_presentCount@6495FC with device rebuild).
  g_present_count = 0;
  return true;
}
#endif

}  // namespace

bool render_d3d9_ready() {
#ifdef _WIN32
  return g_dev != nullptr;
#else
  return false;
#endif
}

int32_t render_d3d9_width() { return g_w; }
int32_t render_d3d9_height() { return g_h; }
int32_t render_d3d9_present_count() { return g_present_count; }

bool render_d3d9_open(int32_t width, int32_t height, const char* title) {
#ifdef _WIN32
  ensure_modes();
  if (width <= 0) width = 800;
  if (height <= 0) height = 600;
  if (!title || !title[0]) title = "SLRR Engine";

  load_stock_window_assets();

  HINSTANCE hi = GetModuleHandleA(nullptr);
  if (!g_class_reg) {
    g_wc = {};
    g_wc.cbSize = sizeof(g_wc);
    g_wc.style = CS_OWNDC;
    g_wc.lpfnWndProc = render_wnd_proc;
    g_wc.hInstance = hi;
    g_wc.hIcon = g_stock_icon ? g_stock_icon : LoadIcon(nullptr, IDI_APPLICATION);
    g_wc.hIconSm = g_stock_icon;
    g_wc.hCursor = g_stock_cursors[2] ? g_stock_cursors[2]
                                      : LoadCursor(nullptr, IDC_ARROW);
    g_wc.lpszClassName = "SLRREngineWnd";
    if (!RegisterClassExA(&g_wc)) {
      std::fprintf(stderr, "[render] RegisterClassEx failed\n");
      return false;
    }
    g_class_reg = true;
  }

  if (g_hwnd) {
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
  }
  g_quit_requested = false;

  // Fixed client size matching the backbuffer — resizable chrome stretches
  // 4:3 content into widescreen and makes OSD text look crushed.
  // Fork: SLRR_WINDOW_NOACTIVATE=1 keeps automated runs from stealing
  // focus (window shown without activation).
  const char* noact_env = std::getenv("SLRR_WINDOW_NOACTIVATE");
  const bool no_activate = noact_env && noact_env[0] == '1';
  const DWORD kWndStyle =
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
      (no_activate ? 0 : WS_VISIBLE);
  RECT rc{0, 0, width, height};
  AdjustWindowRect(&rc, kWndStyle, FALSE);
  g_hwnd = CreateWindowExA(
      no_activate ? WS_EX_NOACTIVATE : 0, g_wc.lpszClassName, title, kWndStyle,
      CW_USEDEFAULT, CW_USEDEFAULT,
      rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hi, nullptr);
  if (!g_hwnd) {
    std::fprintf(stderr, "[render] CreateWindowEx failed\n");
    return false;
  }
  if (g_stock_icon) {
    SendMessageA(g_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_stock_icon));
    SendMessageA(g_hwnd, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(g_stock_icon));
  }
  apply_stock_cursor_to_window();
  ShowWindow(g_hwnd, no_activate ? SW_SHOWNOACTIVATE : SW_SHOW);
  UpdateWindow(g_hwnd);

  if (!create_device(g_hwnd, width, height)) {
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    return false;
  }

  // Pick matching mode index if listed.
  for (size_t i = 0; i < g_modes.size(); ++i) {
    if (g_modes[i].w == width && g_modes[i].h == height) {
      g_mode = static_cast<int32_t>(i);
      break;
    }
  }

  std::printf("[render] D3D9 ready %dx%d hwnd=%p\n", g_w, g_h,
              static_cast<void*>(g_hwnd));
  return true;
#else
  (void)width;
  (void)height;
  (void)title;
  return false;
#endif
}

bool render_d3d9_assets_ready() {
#ifdef _WIN32
  if (!g_assets_ok) load_stock_window_assets();
  return g_assets_ok;
#else
  return false;
#endif
}

bool render_d3d9_set_stock_cursor(int32_t cursor_id) {
#ifdef _WIN32
  if (!g_assets_ok) load_stock_window_assets();
  if (cursor_id == 0) cursor_id = 2;
  if (cursor_id < 2 || cursor_id > 11) return false;
  if (!g_stock_cursors[cursor_id]) return false;
  g_stock_cursor_id = cursor_id;
  apply_stock_cursor_to_window();
  return true;
#else
  (void)cursor_id;
  return false;
#endif
}

int32_t render_d3d9_stock_cursor() {
#ifdef _WIN32
  return g_stock_cursor_id;
#else
  return 0;
#endif
}

bool render_d3d9_stock_icon_loaded() {
#ifdef _WIN32
  if (!g_assets_ok) load_stock_window_assets();
  return g_stock_icon != nullptr;
#else
  return false;
#endif
}

void render_d3d9_set_cursor_visible(int32_t visible) {
#ifdef _WIN32
  if (visible) {
    render_d3d9_set_stock_cursor(g_stock_cursor_id);
    while (ShowCursor(TRUE) < 0) {
    }
  } else {
    while (ShowCursor(FALSE) >= 0) {
    }
  }
#else
  (void)visible;
#endif
}

void render_d3d9_close() {
#ifdef _WIN32
  // W35-02: soft +0x33C/+0x320/+0x324 freelists while device still alive.
  drain_texture_cache_force();
  drain_vb_slots_force();
  drain_ib_slots_force();
  default_texture_release();
  shadow_release();
  if (g_dev) {
    g_dev->Release();
    g_dev = nullptr;
  }
  if (g_d3d) {
    g_d3d->Release();
    g_d3d = nullptr;
  }
  if (g_hwnd) {
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
  }
  g_w = g_h = 0;
#endif
  g_viewports.clear();
  g_active_vp = nullptr;
  g_cameras.clear();
  g_active_cam = nullptr;
  g_fog = FogState{};
  g_light = LightState{};
  g_flares.clear();
  g_flare_sprites_last = 0;
  for (auto& kv : g_textures) {
    texture_clear_async_mip(kv.second);
#ifdef _WIN32
    texture_release_gpu(kv.second);
#endif
  }
  g_textures.clear();
  g_envmap = nullptr;
  g_osd.clear();
  g_osd_text.clear();
  g_fonts.clear();
  g_texts.clear();
  g_meshes.clear();
  g_mesh_queue.clear();
  g_mesh_xforms.clear();
  for (auto& kv : g_mesh_vbs) {
#ifdef _WIN32
    if (kv.second.vb) kv.second.vb->Release();
#endif
  }
  g_mesh_vbs.clear();
  for (auto& kv : g_mesh_ibs) {
#ifdef _WIN32
    if (kv.second.ib) kv.second.ib->Release();
#endif
  }
  g_mesh_ibs.clear();
  input_live_shutdown();
}

void* render_d3d9_hwnd() {
#ifdef _WIN32
  return g_hwnd;
#else
  return nullptr;
#endif
}

void* render_d3d9_device() {
#ifdef _WIN32
  return g_dev;
#else
  return nullptr;
#endif
}

void set_fullscreen_viewport() {
#ifdef _WIN32
  if (!g_dev || g_w <= 0 || g_h <= 0) return;
  // Soft full-RT rect; PE GfxDevice_SetViewportRect @ 0x4BD500 packs
  // D3DVIEWPORT9 MinZ=0 / MaxZ=0x3F800000 (1.0f) then device vt+0xBC.
  D3DVIEWPORT9 dvp{};
  dvp.X = 0;
  dvp.Y = 0;
  dvp.Width = static_cast<DWORD>(g_w);
  dvp.Height = static_cast<DWORD>(g_h);
  dvp.MinZ = 0.f;
  {
    const uint32_t maxz_bits = 0x3F800000u;
    std::memcpy(&dvp.MaxZ, &maxz_bits, sizeof(dvp.MaxZ));
  }
  g_dev->SetViewport(&dvp);
#endif
}

void blit_video_quad(IDirect3DTexture9* tex, float left, float top, float right,
                     float bottom) {
#ifdef _WIN32
  struct Vtx {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
  };
  const DWORD fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
  Vtx v[4] = {
      {left, top, 0.f, 1.f, 0xFFFFFFFFu, 0.f, 0.f},
      {right, top, 0.f, 1.f, 0xFFFFFFFFu, 1.f, 0.f},
      {left, bottom, 0.f, 1.f, 0xFFFFFFFFu, 0.f, 1.f},
      {right, bottom, 0.f, 1.f, 0xFFFFFFFFu, 1.f, 1.f},
  };
  set_fullscreen_viewport();
  g_dev->SetFVF(fvf);
  g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  g_dev->SetTexture(0, tex);
  g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
  g_dev->SetTexture(0, nullptr);
#else
  (void)tex;
  (void)left;
  (void)top;
  (void)right;
  (void)bottom;
#endif
}

void render_d3d9_draw_fullscreen_texture(void* d3d_texture) {
#ifdef _WIN32
  if (!g_dev || !d3d_texture || g_w <= 0 || g_h <= 0) return;
  auto* tex = static_cast<IDirect3DTexture9*>(d3d_texture);
  blit_video_quad(tex, 0.f, 0.f, static_cast<float>(g_w),
                  static_cast<float>(g_h));
#else
  (void)d3d_texture;
#endif
}

void render_d3d9_draw_video_texture(void* d3d_texture, int32_t src_w,
                                    int32_t src_h) {
#ifdef _WIN32
  if (!g_dev || !d3d_texture || g_w <= 0 || g_h <= 0) return;
  if (src_w <= 0 || src_h <= 0) {
    render_d3d9_draw_fullscreen_texture(d3d_texture);
    return;
  }
  auto* tex = static_cast<IDirect3DTexture9*>(d3d_texture);
  const float dw = static_cast<float>(g_w);
  const float dh = static_cast<float>(g_h);
  const float sa = static_cast<float>(src_w) / static_cast<float>(src_h);
  const float da = dw / dh;
  float left = 0.f, top = 0.f, right = dw, bottom = dh;
  // Stock TextureRenderer: fit video aspect inside the client rect.
  if (sa > da) {
    const float h = dw / sa;
    top = (dh - h) * 0.5f;
    bottom = top + h;
  } else {
    const float w = dh * sa;
    left = (dw - w) * 0.5f;
    right = left + w;
  }
  blit_video_quad(tex, left, top, right, bottom);
#else
  (void)d3d_texture;
  (void)src_w;
  (void)src_h;
#endif
}

void set_lookat_view(float eye_x, float eye_y, float eye_z, float at_x,
                     float at_y, float at_z);

void apply_active_viewport() {
#ifdef _WIN32
  // Soft PE GfxCamera_ApplyViewport @ 0x516660 → GfxEngine_SetViewport @
  // 0x4FD560 → GfxDevice_SetViewportRect @ 0x4BD500 (D3D SetViewport vt+0xBC).
  // PE: display_w/h from GfxDevice_GetDisplaySize @ 0x4BD5B0 (device+0x1DC/1E0);
  // pixel = norm_rect × display; MinZ=0 MaxZ=0x3F800000. Host clamps to RT.
  if (!g_dev || !g_active_vp) return;
  auto it = g_viewports.find(g_active_vp);
  if (it == g_viewports.end()) return;
  const ViewportState& vp = it->second;
  D3DVIEWPORT9 dvp{};
  const float fw = static_cast<float>(g_w > 0 ? g_w : 800);
  const float fh = static_cast<float>(g_h > 0 ? g_h : 600);
  dvp.X = static_cast<DWORD>((vp.x > 0.f ? vp.x : 0.f) * fw);
  dvp.Y = static_cast<DWORD>((vp.y > 0.f ? vp.y : 0.f) * fh);
  dvp.Width = static_cast<DWORD>((vp.w > 1.f / fw ? vp.w : 1.f / fw) * fw);
  if (dvp.Width < 1) dvp.Width = 1;
  dvp.Height = static_cast<DWORD>((vp.h > 1.f / fh ? vp.h : 1.f / fh) * fh);
  if (dvp.Height < 1) dvp.Height = 1;
  if (dvp.X + dvp.Width > static_cast<DWORD>(fw))
    dvp.Width = static_cast<DWORD>(fw) - dvp.X;
  if (dvp.Y + dvp.Height > static_cast<DWORD>(fh))
    dvp.Height = static_cast<DWORD>(fh) - dvp.Y;
  dvp.MinZ = 0.f;
  {
    const uint32_t maxz_bits = 0x3F800000u;
    std::memcpy(&dvp.MaxZ, &maxz_bits, sizeof(dvp.MaxZ));
  }
  g_dev->SetViewport(&dvp);
#endif
}

void apply_active_camera() {
#ifdef _WIN32
  if (!g_dev || !g_active_cam) return;
  auto it = g_cameras.find(g_active_cam);
  if (it == g_cameras.end()) return;
  const CameraState& cam = it->second;

  float aspect = 4.f / 3.f;
  void* vp_key = cam.viewport ? cam.viewport : g_active_vp;
  if (vp_key) {
    auto vit = g_viewports.find(vp_key);
    if (vit != g_viewports.end()) {
      const float px_w =
          vit->second.w * static_cast<float>(g_w > 0 ? g_w : 1);
      const float px_h =
          vit->second.h * static_cast<float>(g_h > 0 ? g_h : 1);
      if (px_h > 0.f) aspect = px_w / px_h;
    }
  } else if (g_h > 0) {
    aspect = static_cast<float>(g_w) / static_cast<float>(g_h);
  }

  // Fork: legacy single-camera path. It reads the stored angle as a HALF
  // angle (host cameras pass 45); the exe's reading (full angle, PE 940
  // FUN_00461dd0) is applied by the per-viewport camera pass only.
  float half_rad = cam.aov_deg * 0.01745329252f;
  if (half_rad < 0.01f) half_rad = 0.01f;
  if (half_rad > 1.55f) half_rad = 1.55f;
  const float ys = 1.f / std::tan(half_rad);
  const float xs = ys / aspect;
  const float zn = cam.dmin > 1e-4f ? cam.dmin : 0.1f;
  const float zf = cam.dmax > zn + 1e-3f ? cam.dmax : zn + 100.f;
  const float q = zf / (zf - zn);

  D3DMATRIX proj{};
  proj.m[0][0] = xs;
  proj.m[1][1] = ys;
  proj.m[2][2] = q;
  proj.m[2][3] = 1.f;
  proj.m[3][2] = -q * zn;
  g_dev->SetTransform(D3DTS_PROJECTION, &proj);

  if (cam.lookat) {
    set_lookat_view(cam.eye_x, cam.eye_y, cam.eye_z, cam.at_x, cam.at_y,
                    cam.at_z);
  } else {
    D3DMATRIX view{};
    view.m[0][0] = view.m[1][1] = view.m[2][2] = view.m[3][3] = 1.f;
    g_dev->SetTransform(D3DTS_VIEW, &view);
  }

  if (g_fog.enabled) {
    const DWORD fog_col = D3DCOLOR_XRGB((g_fog.color >> 16) & 0xff,
                                        (g_fog.color >> 8) & 0xff,
                                        g_fog.color & 0xff);
    g_dev->SetRenderState(D3DRS_FOGENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_FOGCOLOR, fog_col);
    g_dev->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
    float n = g_fog.near_z;
    float f = g_fog.far_z;
    g_dev->SetRenderState(D3DRS_FOGSTART, *reinterpret_cast<DWORD*>(&n));
    g_dev->SetRenderState(D3DRS_FOGEND, *reinterpret_cast<DWORD*>(&f));
  } else {
    g_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
  }
#else
  (void)0;
#endif
}

void set_lookat_view(float eye_x, float eye_y, float eye_z, float at_x,
                     float at_y, float at_z) {
#ifdef _WIN32
  if (!g_dev) return;
  // D3DXMatrixLookAtLH-compatible view.
  float zx = at_x - eye_x;
  float zy = at_y - eye_y;
  float zz = at_z - eye_z;
  float zl = std::sqrt(zx * zx + zy * zy + zz * zz);
  if (zl < 1e-6f) zl = 1.f;
  zx /= zl;
  zy /= zl;
  zz /= zl;
  // xaxis = normalize(cross(up=(0,1,0), zaxis))
  float xx = zz;
  float xy = 0.f;
  float xz = -zx;
  float xl = std::sqrt(xx * xx + xy * xy + xz * xz);
  if (xl < 1e-6f) {
    xx = 1.f;
    xy = 0.f;
    xz = 0.f;
    xl = 1.f;
  }
  xx /= xl;
  xy /= xl;
  xz /= xl;
  // yaxis = cross(zaxis, xaxis)
  float yx = zy * xz - zz * xy;
  float yy = zz * xx - zx * xz;
  float yz = zx * xy - zy * xx;
  D3DMATRIX view{};
  view.m[0][0] = xx;
  view.m[0][1] = yx;
  view.m[0][2] = zx;
  view.m[1][0] = xy;
  view.m[1][1] = yy;
  view.m[1][2] = zy;
  view.m[2][0] = xz;
  view.m[2][1] = yz;
  view.m[2][2] = zz;
  view.m[3][0] = -(xx * eye_x + xy * eye_y + xz * eye_z);
  view.m[3][1] = -(yx * eye_x + yy * eye_y + yz * eye_z);
  view.m[3][2] = -(zx * eye_x + zy * eye_y + zz * eye_z);
  view.m[3][3] = 1.f;
  g_dev->SetTransform(D3DTS_VIEW, &view);
#else
  (void)eye_x;
  (void)eye_y;
  (void)eye_z;
  (void)at_x;
  (void)at_y;
  (void)at_z;
#endif
}

// Fork: file units -> script metres is MeshState::unit (0.01 for SCX
// centimetres, 1.0 for POLY metres); texts are screen-space glyph quads in
// pixels, see draw_camera_text_section.
static int g_vp_pass_trace_budget = 40;  // SLRR_RENDER_TRACE=1 lines

// One step up an instance's parent chain: bone-link parent, else creation
// parent (RenderRef.create), else GameRef parent (Group / Dummy -> Osd).
static void* chain_parent_of(void* cur) {
  void* next = nullptr;
  auto it = g_mesh_xforms.find(cur);
  if (it != g_mesh_xforms.end()) {
    next = it->second.parent;
    if (!next) next = it->second.tree_parent;
  }
  if (!next) next = gameref_get_parent(static_cast<InvObject*>(cur));  // Group -> Osd
  return next == cur ? nullptr : next;
}

// Root of a render instance's parent chain (g_mesh_xforms), cycle-guarded.
static void* mesh_root_of(void* key) {
  void* cur = key;
  for (int guard = 0; cur && guard < 64; ++guard) {
    void* next = chain_parent_of(cur);
    if (!next) break;
    cur = next;
  }
  return cur;
}

constexpr int32_t kGameRefWorldTreeRoot = 0x10;  // GameRef.WORLDTREEROOT
constexpr int32_t kGameRefWorldTreeLeaf = 0x40;  // GameRef.WORLDTREELEAF

// PE 940 world-tree root of a node (node+0xfc; FUN_0051f440 on create /
// reparent, FUN_0051f7f0 from GameRef.setFlags): the nearest
// ancestor-or-self whose flags carry WORLDTREEROOT, else the topmost node.
// A camera walks the tree below the root of its own node (FUN_00461dd0
// 0x462251, cam+0xacc).
static void* camera_root_of(void* start) {
  void* cur = start;
  for (int guard = 0; cur && guard < 64; ++guard) {
    if (gameref_get_flags(static_cast<InvObject*>(cur)) & kGameRefWorldTreeRoot)
      return cur;
    void* next = chain_parent_of(cur);
    if (!next) break;
    cur = next;
  }
  return cur;
}

// PE 940 camera walk FUN_0045eb10 (started on the camera root by
// FUN_0045e910): a container whose node+0x84 has WORLDTREELEAF is not
// descended (FUN_0045f030 only marks its subtree hidden), the root
// included; every other child container is descended whatever its own
// WORLDTREEROOT bit; a render instance is queued without testing its own
// flag. Host: the same test walking UP from the instance.
static bool camera_sees(void* root, void* key, bool* hidden = nullptr) {
  if (hidden) *hidden = false;
  if (!root || !key) return false;
  if (key == root) return true;  // the root itself is an instance
  bool leaf = false;
  void* cur = chain_parent_of(key);
  for (int guard = 0; cur && guard < 64; ++guard) {
    if (gameref_get_flags(static_cast<InvObject*>(cur)) & kGameRefWorldTreeLeaf)
      leaf = true;
    if (cur == root) {
      if (hidden) *hidden = leaf;  // below the root, but not descended into
      return !leaf;
    }
    cur = chain_parent_of(cur);
  }
  return false;
}

// Rigid inverse of a world matrix (rotation transpose, translated).
static void invert_rigid(const D3DMATRIX& m, D3DMATRIX* out) {
  D3DMATRIX r{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r.m[i][j] = m.m[j][i];
  const float tx = m.m[3][0], ty = m.m[3][1], tz = m.m[3][2];
  r.m[3][0] = -(tx * r.m[0][0] + ty * r.m[1][0] + tz * r.m[2][0]);
  r.m[3][1] = -(tx * r.m[0][1] + ty * r.m[1][1] + tz * r.m[2][1]);
  r.m[3][2] = -(tx * r.m[0][2] + ty * r.m[1][2] + tz * r.m[2][2]);
  r.m[3][3] = 1.f;
  *out = r;
}

// General 4x4 inverse (PE 940 FUN_00567630: cofactors FUN_005677d0 times the
// reciprocal determinant). False when the matrix is singular.
static bool invert_general(const D3DMATRIX& src, D3DMATRIX* out) {
  const float* m = &src.m[0][0];
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
           m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
           m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
           m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
            m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
           m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
           m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
           m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
            m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
           m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
           m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
            m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
            m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
           m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
           m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
            m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
            m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (!(std::fabs(det) > 1e-20f)) return false;
  const float rdet = 1.f / det;
  float* o = &out->m[0][0];
  for (int i = 0; i < 16; ++i) o[i] = inv[i] * rdet;
  return true;
}

// PE 940 FUN_00461dd0 -> C3DLow vt+0x80 FUN_00422e80 =
// D3DXMatrixPerspectiveFovRH(fovy, aspect, zn, zf): fovy = aov * pi/180
// (DAT_00593a50; the Camera.create aov is the FULL vertical angle),
// zn = dmin, zf = dmax * cot(aov/2)^(1/4) (0x461f3f-0x461f80, DAT_005993cc =
// pi/360), aspect = the value stored at Camera.create unless it is 0
// (0x461fe8). Host units are metres; the exe's planes are metres * 10.
// The OLD exe is the same (camera update @ 0x005100B0, callers 0x005105C0 /
// 0x0050E9E0): params+0x10 -> cam+0xA4; zf = dmax * sqrt(sqrt(cos / sin))
// of aov * flt_5F2DB4 (pi/360); aspect params+0xC unless 0.0; then device
// vt+0x80 (vtable 0x5F1550 -> 0x004BA4B0 -> static D3DX 0x0058EADB, which
// writes m[2][3] = -1: PerspectiveFovRH) with fovy = aov * flt_5F13BC
// (pi/180). Its scripts halve the Java aov as Build 940's do (Camera.<init>
// tree 10: * 0.5) and Osd carries the same CAM_AOV 60 / CAM_OFFSET 5.48 /
// SCALE_3D 1.469, so the old build's navigator minimap (Camera(.., 90 ->
// native 45, ..)) is drawn with fovy 45, right-handed, by this pass as well.
static void build_camera_projection(const CameraState& cam, float vp_aspect,
                                    D3DMATRIX* out, float* out_xs,
                                    float* out_ys) {
  float aov = cam.aov_deg;
  if (!(aov > 0.5f)) aov = 0.5f;  // Fork: keep the cotangent finite
  if (aov > 179.f) aov = 179.f;
  const float half = aov * 0.008726389f;  // DAT_005993cc
  const float cot = std::cos(half) / std::sin(half);
  float aspect = cam.aspect != 0.f ? cam.aspect : vp_aspect;
  if (!(aspect > 1e-6f)) aspect = 1.f;
  const float ys = 1.f / std::tan(aov * 0.017453292f * 0.5f);  // DAT_00593a50
  const float xs = ys / aspect;
  const float zn = cam.dmin > 1e-4f ? cam.dmin : 0.1f;  // Fork: guard
  float zf = cam.dmax * std::sqrt(std::sqrt(cot));
  if (!(zf > zn + 1e-3f)) zf = zn + 100.f;  // Fork: guard
  D3DMATRIX p{};
  p.m[0][0] = xs;
  p.m[1][1] = ys;
  p.m[2][2] = zf / (zn - zf);
  p.m[2][3] = -1.f;
  p.m[3][2] = zn * zf / (zn - zf);
  *out = p;
  if (out_xs) *out_xs = xs;
  if (out_ys) *out_ys = ys;
}

// View and projection of a camera exactly as its pass builds them (PE 940
// FUN_00461dd0): projection from the stored angle / aspect / planes, view =
// general inverse of the camera's world matrix with its own bone00 pose
// folded (bone00 world copied to cam+0x39c, FUN_00567630). The exe uses the
// same matrices to draw and to project / unproject (C3DLow vt+0x94 / +0x9c),
// so the host helpers share this function with draw_viewport_cameras.
static void camera_pass_matrices(void* cam_key, const CameraState& cam,
                                 D3DMATRIX* cam_world, D3DMATRIX* view,
                                 D3DMATRIX* proj, float* xs, float* ys,
                                 float* vp_aspect_out) {
  float vp_aspect = 4.f / 3.f;
  auto vit = g_viewports.find(cam.viewport);
  if (vit != g_viewports.end()) {
    const float px_w = vit->second.w * static_cast<float>(g_w > 0 ? g_w : 1);
    const float px_h = vit->second.h * static_cast<float>(g_h > 0 ? g_h : 1);
    if (px_h > 0.f) vp_aspect = px_w / px_h;
  }
  build_camera_projection(cam, vp_aspect, proj, xs, ys);
  const bool saved_fold = g_fold_own_bone0;
  g_fold_own_bone0 = true;
  resolve_world(cam_key, cam_world, 0);
  g_fold_own_bone0 = saved_fold;
  if (!invert_general(*cam_world, view)) invert_rigid(*cam_world, view);
  if (vp_aspect_out) *vp_aspect_out = vp_aspect;
}

// Fork: true when the camera pass drew with this camera in the last flushed
// frame (see CameraState::pass_frame).
static bool camera_drawn_by_pass(const CameraState& cam) {
  return cam.pass_frame != 0 && cam.pass_frame == g_cam_pass_frame;
}

void draw_meshes();

// Fork: true on the Build 940 script boot (SLRR_PE_BOOT_INIT=1, the gate of
// NativeTypes.cpp). Renderer behaviour read from Build 940 that the legacy
// path, which mirrors the old build, must not pick up asks this.
static bool render_boot940() {
  static const bool on = [] {
    const char* e = std::getenv("SLRR_PE_BOOT_INIT");
    return e && e[0] == '1';
  }();
  return on;
}

// Fragment 5: the detail a camera gives the nodes of its walk, and a
// section with two bone matrices.
static void camera_offer_detail(void* cam_key);
// Fragment 5: the D3D fog of a camera pass (FUN_00427260); g_pass_fog = on.
static void camera_pass_fog(const CameraState& cam);
static bool g_pass_fog = false;
static bool draw_two_bone_section(void* key, const MeshState& ms,
                                  const MeshSubmesh& sm,
                                  const D3DMATRIX& world,
                                  bool two_sets = false);
// Fragment 6: the class programs of the lit groups (Build 940 boot).
static void lit_frame_begin();
static bool draw_lit_section(void* key, const MeshState& ms,
                             const MeshSubmesh& sm, const D3DMATRIX& world,
                             bool transparent, bool no_zwrite);

// rg_ground of a camera pass (render_d3d9_part4.inc): the sections of the
// ground meshes the camera sees, drawn in material mode 3.
struct GroundSec {
  const MeshState* ms;
  int sub;
};
static void draw_ground_group(const std::vector<GroundSec>& secs);

// An object section drawn in material mode 3 (render_d3d9_part4.inc):
// rg_object_prelit (cam+0x69c) and rg_prelittransparent (cam+0x80c).
struct Mode3Sec {
  void* key;
  const MeshState* ms;
  int sub;
};
static void draw_object_prelit_group(const std::vector<Mode3Sec>& secs);
static void draw_object_prelit_transparent(const Mode3Sec& sec);
// True for the material classes whose mode 3 stage setup the host has
// (render_d3d9_part4.inc).
static bool mode3_object_class(int32_t shader);

// The device lights of a camera pass (render_d3d9_part4.inc): `any` when
// the camera sees at least one scene light, `device` what FUN_0045a2c0
// selects of them. A pass that sees none keeps the stand-in below.
struct CamLights {
  bool any = false;
#ifdef _WIN32
  std::vector<D3DLIGHT9> device;
#endif
};
static void collect_camera_lights(void* root, CamLights* out);
static void apply_camera_lights(const CamLights& lights);
static void end_camera_lights();

// Fragment 7: the projected shadows of a camera pass (FUN_00460ba0, after
// the opaque groups). Build 940 boot only.
static void* g_pass_cam = nullptr;  // the camera whose pass is drawn
static void shadow_frame_begin();
static void draw_camera_shadows(const std::vector<void*>& members,
                                const std::vector<GroundSec>& ground,
                                const D3DMATRIX& cam_world,
                                const CamLights& lights);

// Host stand-in scene light (one directional light + ambient), shared by the
// legacy queue draw and the lit groups of the camera pass.
static void apply_scene_light() {
  D3DLIGHT9 light{};
  light.Type = D3DLIGHT_DIRECTIONAL;
  float dr = 1.f, dg = 1.f, db = 1.f;
  float ar = 40.f / 255.f, ag = 44.f / 255.f, ab = 52.f / 255.f;
  float sr = 1.f, sg = 1.f, sb = 1.f;
  if (g_light.enabled) {
    rgb_bytes(g_light.diffuse, &dr, &dg, &db);
    rgb_bytes(g_light.ambient, &ar, &ag, &ab);
    rgb_bytes(g_light.specular, &sr, &sg, &sb);
  }
  light.Diffuse.r = dr;
  light.Diffuse.g = dg;
  light.Diffuse.b = db;
  light.Specular.r = sr;
  light.Specular.g = sg;
  light.Specular.b = sb;
  light.Direction.x = -0.4f;
  light.Direction.y = -0.7f;
  light.Direction.z = 0.5f;
  g_dev->SetLight(0, &light);
  g_dev->LightEnable(0, TRUE);
  // Packed ambient bytes (setLight a5 dir=0; D3DRS_AMBIENT is host bind).
  if (g_light.enabled) {
    const int32_t amb = g_light.ambient;
    g_dev->SetRenderState(
        D3DRS_AMBIENT,
        D3DCOLOR_XRGB((amb >> 16) & 0xff, (amb >> 8) & 0xff, amb & 0xff));
  } else {
    g_dev->SetRenderState(
        D3DRS_AMBIENT,
        D3DCOLOR_XRGB(static_cast<int>(ar * 255.f + 0.5f),
                      static_cast<int>(ag * 255.f + 0.5f),
                      static_cast<int>(ab * 255.f + 0.5f)));
  }
}

// Camera-pass instance draw. PE 940 camera render FUN_00460ba0, the groups
// an instance tree uses, in the exe's order: rg_object (cam+0x5b4, opaque,
// lit), rg_objectprelit (cam+0x628, opaque, lighting off),
// rg_prelittransparent (cam+0x80c), rg_transparent (cam+0x784). The group
// of a section comes from its material create flags (RI_object ctor
// FUN_00465e40: 0x40000000 -> material+8 bit0 transparent, 0x80000000 ->
// bit1 prelit), NOT from the texture format.
// rg_transparent (begin FUN_0045e8d0 = C3DLow vt+0x10c(1,0) lighting on +
// vt+0x34(2,1) FUN_00422c60): ZWRITE off, Z test stays on (LESSEQUAL),
// SRCALPHA / INVSRCALPHA, BLENDOP ADD, alpha test GREATER ref 4; its
// entries are drawn back to front, key = dot(instance origin - eye, camera
// world Z row) (RI+0xb8), in the order the exe's binary min-heap yields:
// push FUN_0046b5e0 (sift up while the parent key is greater), draw the
// root, pop FUN_0040abe0 (the last entry sinks while it is greater than the
// smaller child). Equal keys therefore come out first-queued first and the
// rest in reverse (FUN_00468490; rg_prelittransparent FUN_00468200 the
// same).
// Material (FUN_00434170 / FUN_00440b50): Diffuse = Ambient = the
// material's colour id 0 entry, Emissive = colour id 2, D3DRS_TEXTUREFACTOR
// alpha = that entry's alpha, stage 0 alpha = texture alpha * TFACTOR
// (defaults FUN_00426d10). For the Tex family the colour id 0 entry is
// opaque white whatever the file says; the other shaders keep the file
// value (scx_shader_white_diffuse, render_d3d9_part3.inc).
// The Rectangle mesh etalon_negyzet_alpha is transparent with white Diffuse
// and white Emissive: lit, yet the texture shows unshaded.
// Instance colour (RenderRef.setColor FUN_004e2830 -> payload+0x17c ->
// RI_object+0xcc): consumed only when the render type's `flags` have 0x80
// with 0x200 (mode 1) or 0x400 (mode 2) (RI_object ctor FUN_00465e40). For
// the draw of that instance FUN_00440a60(0, r, g, b, a) (mode 1) replaces
// the material's colour id 0 - also the white of the Tex family - and
// FUN_00440a60(2, ...) (mode 2) its colour id 2; both are restored after
// the draw (FUN_00468490 / FUN_00467c70). FUN_00440a60 does nothing for a
// material whose create flags have 0x04000000. In frontend.rdb only
// paintcan and finishObject (flags 640) qualify, Rectangles (etalon, flags
// 4096) do not.
// Fork: an instance whose colour was never set keeps the material colour
// (the exe would use the payload's initial value, which was not read).
// Prelit groups (rg_objectprelit, rg_prelittransparent begin FUN_0045e870):
// lighting off (C3DLow vt+0x10c(0,0) FUN_00426190). A prelit vertex has two
// colours: the first (D3DFVF_DIFFUSE) is 0xFFFFFFFF in every INVO v4 chunk
// of the install (0xFF808080 from the v3 loader), the second
// (D3DFVF_SPECULAR) carries the baked lighting, which material pass 3
// combines per material (FUN_0043f3a0 mode 3, D3DTA_SPECULAR stage setups,
// lightmap stages FUN_00436230 / FUN_0043c9a0 / FUN_00439350).
// PE 940 group order of the pass (FUN_00460ba0): rg_horizon (cam+0x890),
// rg_ground (cam+0x528), rg_object_prelit (cam+0x69c), rg_object
// (cam+0x5b4), rg_objectprelit (cam+0x628), rg_prelittransparent
// (cam+0x80c), rg_transparent (cam+0x784). The RI_object ctor FUN_00465e40
// puts every section of an instance whose template has the horizon flag
// (RT+0xFC, render object flags & 1) into rg_horizon and nothing else;
// otherwise an opaque prelit section goes into rg_objectprelit AND
// rg_object_prelit and a transparent prelit one into rg_prelittransparent.
// rg_object_prelit (group begin FUN_0045e780 = lighting off; per material
// begin FUN_0045e720, loop FUN_0045f7a0, entry FUN_00468cd0) and
// rg_prelittransparent (begin FUN_0045e870 = lighting off +
// FUN_00422c60(2,1), loop FUN_00468200, back to front through the same
// heap) draw in material mode 3, C3DLow vt+0xe0(material, 3):
// texture * lightmap for CMaterial_Tex with a lightmap entry
// (draw_mode3_section, render_d3d9_part4.inc). rg_objectprelit (loop
// FUN_00468090) only adds the passes of the sector's static point and spot
// lights.
// Host, Build 940 boot: a prelit section that is bound by slot, has a
// lightmap entry and wide vertices takes the mode 3 path; the groups are
// drawn horizon, ground, prelit, lit, prelit transparent, transparent.
// Fork: every other prelit section (no lightmap entry: FUN_0043c9a0,
// texture * vertex diffuse, M2 spec Q13; or not bound by slot: the legacy
// path) is drawn as texture * material Diffuse with lighting off; the
// static-light passes of rg_objectprelit are not ported; the legacy path
// keeps its order (lit before prelit).
// Fork: render object flag 0x8000 (the props, city buildings): the exe
// draws a per-instance copy of the mesh, transformed by the instance
// matrix when the instance is made and lit in software from the sector's
// static lights (FUN_00465e40 -> FUN_004688f0 / FUN_00465670, C3DLow
// vt+0x11c), under an identity world matrix (FUN_00468cd0, RI+0x9c). The
// host draws the type's mesh under the instance matrix: the same picture
// without static lights (M2 spec Q8), and mode 3 of CMaterial_Tex reads no
// vertex colour.
// Lit groups (rg_object, rg_transparent): the lights of the camera when it
// sees a light instance (apply_camera_lights), else the host stand-in light
// (Fork: the legacy path, the navigator pass and every Osd camera).
// Fork: the transparent queue is filled in creation order (the exe
// fills the group's entry array in tree-walk order); a section without a
// parsed material keeps the legacy rule (heuristic diffuse, blend only with
// a DXT3/DXT5 texture); CULL_NONE for every section drawn here (the exe
// culls: the class programs of render_d3d9_part6.inc and mode 3 do too).
static void draw_camera_instances(const std::vector<void*>& members,
                                  const D3DMATRIX& cam_world,
                                  const CamLights& lights) {
  if (members.empty()) return;
  struct Sec {
    void* key;
    const MeshState* ms;
    int sub;        // -1: uploaded VB/IB, the whole mesh with subs[0]'s material
    float depth;
    bool legacy_blend;
    bool mode3;     // prelit, drawn in material mode 3
  };
  std::vector<Sec> horizon, opaque_lit, opaque_prelit, trans_prelit, trans_lit;
  const bool boot940 = render_boot940();
  // rg_ground (cam+0x528): the entries of every RT_ground template the
  // camera's walk met, drawn before the object groups (FUN_00460ba0).
  std::vector<GroundSec> ground;
  const float ex = cam_world.m[3][0], ey = cam_world.m[3][1], ez = cam_world.m[3][2];
  const float zx = cam_world.m[2][0], zy = cam_world.m[2][1], zz = cam_world.m[2][2];
  auto tex_fmt_of = [](const MeshSubmesh* sm) -> int32_t {
    if (!sm || !sm->texture_key) return 3;
    auto tit = g_textures.find(sm->texture_key);
    return tit == g_textures.end() ? 3 : tit->second.engine_fmt;
  };
  for (void* key : members) {
    auto it = g_meshes.find(key);
    if (it == g_meshes.end() || !it->second.ready) continue;
    const MeshState& ms = it->second;
    if (ms.group == kMeshGroupGround) {
      // A ground mesh has no instance pose and no object group: every
      // section goes to rg_ground whatever its material flags say.
      for (size_t s = 0; s < ms.subs.size(); ++s)
        if (!ms.subs[s].wide.empty() && ms.subs[s].indices.size() >= 3)
          ground.push_back({&ms, static_cast<int>(s)});
      continue;
    }
    D3DMATRIX w{};
    resolve_world(key, &w, 0);
    const float depth = (w.m[3][0] - ex) * zx + (w.m[3][1] - ey) * zy + (w.m[3][2] - ez) * zz;
    // RT+0xFC, set from render object flags & 1 (FUN_0046e1a0): all 16 sky
    // dome render objects of the install. Fork: Build 940 boot only.
    bool horizon_inst = false;
    if (boot940) {
      auto fit = g_mesh_type_flags.find(key);
      horizon_inst = fit != g_mesh_type_flags.end() && (fit->second & 1u) != 0;
    }
    bool uploaded = false;
    {
      auto vit = g_mesh_vbs.find(key);
      auto iit = g_mesh_ibs.find(key);
      uploaded = vit != g_mesh_vbs.end() && vit->second.ready && vit->second.vb &&
                 iit != g_mesh_ibs.end() && iit->second.ready && iit->second.ib &&
                 iit->second.index_count >= 3;
    }
    const int nsub = uploaded ? 1 : static_cast<int>(ms.subs.size());
    for (int s = 0; s < nsub; ++s) {
      const MeshSubmesh* sm = ms.subs.empty() ? nullptr : &ms.subs[static_cast<size_t>(s)];
      if (!uploaded && (!sm || sm->verts.empty() || sm->indices.size() < 3)) continue;
      Sec sec{key, &ms, uploaded ? -1 : s, depth, false, false};
      if (horizon_inst) {
        horizon.push_back(sec);
        continue;
      }
      if (sm && sm->has_mat) {
        const bool transparent = (sm->mat_flags & 0x40000000u) != 0;
        const bool prelit = (sm->mat_flags & 0x80000000u) != 0;
        // Mode 3 with a lightmap: CMaterial_Tex takes FUN_00436230 when its
        // lightmap entry has a slot (FUN_00434170 case 3: entry [0] slot !=
        // -1). slot_bound is only ever set on the Build 940 boot.
        // Fork: a class whose mode 3 setup was not read (TexSpheremap,
        // Color*, the bump classes; mode3_object_class) stays on the
        // object path below.
        sec.mode3 = prelit && !uploaded && sm->slot_bound &&
                    sm->slot_lightmap != -1 && !sm->wide.empty() &&
                    mode3_object_class(sm->mat_shader);
        (transparent ? (prelit ? trans_prelit : trans_lit)
                     : (prelit ? opaque_prelit : opaque_lit)).push_back(sec);
      } else {
        const int32_t fmt = tex_fmt_of(sm);
        sec.legacy_blend = (fmt == 7 || fmt == 13);  // legacy: DXT5 / DXT3
        (sec.legacy_blend ? trans_lit : opaque_lit).push_back(sec);
      }
    }
  }
  // PE 940 FUN_00468490 / FUN_00468200: every queued entry goes through the
  // heap, then the root is drawn and popped until the heap is empty.
  auto heap_order = [](std::vector<Sec>& v) {
    std::vector<Sec> h;
    h.reserve(v.size());
    for (const Sec& s : v) {  // FUN_0046b5e0
      size_t i = h.size();
      h.push_back(s);
      while (i != 0) {
        const size_t p = (i - 1) / 2;
        if (h[p].depth <= s.depth) break;
        h[i] = h[p];
        i = p;
      }
      h[i] = s;
    }
    std::vector<Sec> out;
    out.reserve(h.size());
    while (!h.empty()) {
      out.push_back(h[0]);
      const size_t n = h.size() - 1;  // FUN_0040abe0: h[n] is the last entry
      if (n != 0) {
        const Sec last = h[n];
        size_t i = 0, l = 1;
        while (l < n) {
          const size_t r = i * 2 + 2;
          size_t c = l;
          if (r < n && !(h[l].depth <= h[r].depth)) c = r;
          if (last.depth <= h[c].depth) break;
          h[i] = h[c];
          i = c;
          l = i * 2 + 1;
        }
        h[i] = last;
      }
      h.pop_back();
    }
    v.swap(out);
  };
  heap_order(trans_prelit);
  heap_order(trans_lit);

  const DWORD fvf_default = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;
  g_dev->SetFVF(fvf_default);
  g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
  g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  if (lights.any) {
    apply_camera_lights(lights);
    ++g_cam_pass_stats.light_passes;
  } else {
    apply_scene_light();
  }

  IDirect3DBaseTexture9* env_tex = nullptr;
  if (g_envmap) {
    auto eit = g_textures.find(g_envmap);
    if (eit != g_textures.end()) env_tex = texture_d3d_base(eit->second);
  }

  auto draw_section = [&](const Sec& sec, bool transparent, bool prelit,
                          bool no_zwrite = false) {
    const MeshState& ms = *sec.ms;
    const MeshSubmesh* sm =
        ms.subs.empty() ? nullptr : &ms.subs[static_cast<size_t>(sec.sub < 0 ? 0 : sec.sub)];
    const bool has_mat = sm && sm->has_mat;
    D3DMATRIX world{};
    resolve_world(sec.key, &world, 0);
    // Fork: SCX vertices are centimetres (see GameRef_core extent "cm");
    // script poses / camera planes are metres (MeshState::unit).
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) world.m[r][c] *= ms.unit;
    g_dev->SetTransform(D3DTS_WORLD, &world);
    // Build 940 boot: a section of a lit group goes through the program
    // of its material class (render_d3d9_part6.inc).
    if (boot940 && has_mat && !prelit && sec.sub >= 0 &&
        draw_lit_section(sec.key, ms, *sm, world, transparent, no_zwrite))
      return;

    IDirect3DBaseTexture9* tex = nullptr;
    if (sm && sm->texture_key) {
      auto tit = g_textures.find(sm->texture_key);
      if (tit != g_textures.end()) tex = texture_d3d_base(tit->second);
    }
    g_dev->SetTexture(0, tex);
    // PE 940 FUN_0043d990 (the bind of every material class, object modes
    // included: FUN_00434170): an entry that has a texture clamps the axes
    // whose address bit is clear; FUN_0043d930 puts WRAP back when the
    // pass ends. The Rectangle mesh etalon_negyzet_alpha has both bits
    // clear: with WRAP the last pixel row of a rectangle blends in the
    // opposite edge of its texture (the row under the Osd top bar).
    // Fork: on the Build 940 boot only; the legacy path keeps WRAP.
    const bool clamp_u =
        render_boot940() && has_mat && tex && (sm->addr_primary & 1u) == 0;
    const bool clamp_v =
        render_boot940() && has_mat && tex && (sm->addr_primary & 2u) == 0;
    if (clamp_u) g_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    if (clamp_v) g_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    // FUN_0043d990 again: an entry without a texture, or with one that is
    // not ready, binds the engine default texture (C3DLow+0x340, 4x4
    // 0x80808080). Host: a section bound by slot whose primary entry names
    // a slot that is past the render object's list, empty, or a texture
    // that did not load (misc.garage: g1\png\g1Auto.png).
    // Fork: a material without a primary entry in its file stays
    // untextured (what slot the exe's class default gives that entry was
    // not read).
    if (!tex && has_mat && sm->slot_bound && sm->slot_primary != -1) {
      tex = default_texture();
      g_dev->SetTexture(0, tex);
      ++g_cam_pass_stats.default_tex_binds;
    }

    // Instance colour mode (RI_object ctor FUN_00465e40, from the render
    // type flags); FUN_00440a60 leaves a 0x04000000 material alone.
    int colour_mode = 0;
    uint32_t inst_colour = 0xFFFFFFFFu;
    if (has_mat && (sm->mat_flags & 0x04000000u) == 0) {
      auto fit = g_mesh_type_flags.find(sec.key);
      if (fit != g_mesh_type_flags.end() && (fit->second & 0x80u) != 0) {
        if (fit->second & 0x200u) colour_mode = 1;
        if (fit->second & 0x400u) colour_mode = 2;
      }
      if (colour_mode != 0) {
        auto xit = g_mesh_xforms.find(sec.key);
        if (xit != g_mesh_xforms.end() && xit->second.color_set)
          inst_colour = static_cast<uint32_t>(xit->second.color);
        else
          colour_mode = 0;  // Fork: never set, the material colour stays
      }
    }

    D3DMATERIAL9 mat{};
    if (has_mat) {
      // Mode 1: the instance ARGB is colour id 0 for this draw (Diffuse =
      // Ambient, alpha -> TEXTUREFACTOR); mode 2: its RGB is colour id 2.
      const uint32_t d = colour_mode == 1 ? inst_colour : sm->mat_diffuse;
      const uint32_t e = colour_mode == 2 ? inst_colour : sm->mat_emissive;
      mat.Diffuse.r = mat.Ambient.r = static_cast<float>((d >> 16) & 0xff) / 255.f;
      mat.Diffuse.g = mat.Ambient.g = static_cast<float>((d >> 8) & 0xff) / 255.f;
      mat.Diffuse.b = mat.Ambient.b = static_cast<float>(d & 0xff) / 255.f;
      mat.Diffuse.a = mat.Ambient.a = static_cast<float>(d >> 24) / 255.f;
      mat.Emissive.r = static_cast<float>((e >> 16) & 0xff) / 255.f;
      mat.Emissive.g = static_cast<float>((e >> 8) & 0xff) / 255.f;
      mat.Emissive.b = static_cast<float>(e & 0xff) / 255.f;
      // FUN_00434170: SetRenderState(TEXTUREFACTOR, (fade * diffuse.a * 255) << 24)
      g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, (d & 0xFF000000u) | 0x00FFFFFFu);
      if (tex) {
        g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
      } else {
        g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
      }
    } else {
      // Legacy rule (no parsed material).
      const uint32_t d = sm ? sm->diffuse : 0xFF808080u;
      const float r = static_cast<float>((d >> 16) & 0xff) / 255.f;
      const float g = static_cast<float>((d >> 8) & 0xff) / 255.f;
      const float b = static_cast<float>(d & 0xff) / 255.f;
      mat.Diffuse.r = mat.Ambient.r = r > 0.05f ? r : 0.75f;
      mat.Diffuse.g = mat.Ambient.g = g > 0.05f ? g : 0.78f;
      mat.Diffuse.b = mat.Ambient.b = b > 0.05f ? b : 0.82f;
      mat.Diffuse.a = mat.Ambient.a = 1.f;
      if (tex) {
        g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      } else {
        g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
      }
    }
    g_dev->SetMaterial(&mat);
    g_dev->SetRenderState(D3DRS_LIGHTING, prelit ? FALSE : TRUE);

    if (transparent && !sec.legacy_blend) {
      // FUN_00422c60(2,1)
      g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
      g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
      g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
      g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
      g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
      g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
      g_dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
      g_dev->SetRenderState(D3DRS_ALPHAREF, 4);
    } else if (sec.legacy_blend) {
      g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
      g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
      g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
      g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
      g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    } else {
      g_dev->SetRenderState(D3DRS_ZWRITEENABLE, no_zwrite ? FALSE : TRUE);
      g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
      // 0x20000000: alpha test, reference from material entry 0x5000000
      // (v3 builder FUN_0042ba10 -> FUN_004412c0(0x5000000, 0x38)).
      const bool atest = has_mat && (sm->mat_flags & 0x20000000u) != 0;
      g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, atest ? TRUE : FALSE);
      if (atest) {
        g_dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        g_dev->SetRenderState(D3DRS_ALPHAREF, sm->mat_alpha_ref & 0xffu);
      }
    }
    // Fork: the global envmap stand-in (stage 1) stays for every section
    // except shader ids 0 and 4 (CMaterial_Tex FUN_0042af10 and its specular
    // twin FUN_0042b400 declare no reflection map entry 0x6000003 / 1).
    const bool env =
        env_tex && !(has_mat && (sm->mat_shader == 0 || sm->mat_shader == 4));
    if (env) apply_envmap_stage1(env_tex);

    if (sec.sub < 0) {
      const MeshVbState& vb = g_mesh_vbs[sec.key];
      const MeshIbState& ib = g_mesh_ibs[sec.key];
      g_dev->SetFVF(vb.pe_fvf != 0 ? vb.pe_fvf : fvf_default);
      g_dev->SetStreamSource(0, vb.vb, 0, static_cast<UINT>(vb.stride));
      g_dev->SetIndices(ib.ib);
      const UINT nvert = static_cast<UINT>(vb.vert_count);
      const UINT ntri = static_cast<UINT>(ib.index_count / 3);
      if (nvert > 0 && ntri > 0)
        g_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, nvert, 0, ntri);
      g_dev->SetStreamSource(0, nullptr, 0, 0);
      g_dev->SetIndices(nullptr);
      g_dev->SetFVF(fvf_default);
    } else if (sm->bone_mode == 1 && !ms.bones.empty() &&
               draw_two_bone_section(sec.key, ms, *sm, world)) {
      // FUN_00468cd0 / FUN_00444180: drawn with the matrices of its two
      // bones (render_d3d9_part5.inc).
    } else {
      const UINT ntri = static_cast<UINT>(sm->indices.size() / 3);
      g_dev->DrawIndexedPrimitiveUP(
          D3DPT_TRIANGLELIST, 0, static_cast<UINT>(sm->verts.size()), ntri,
          sm->indices.data(), D3DFMT_INDEX16, sm->verts.data(),
          sizeof(MeshVertex));
    }
    if (env) apply_envmap_stage1(nullptr);
    if (clamp_u) g_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    if (clamp_v) g_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
  };

  // rg_horizon (cam+0x890), the first group after the depth clear.
  //   begin FUN_0045e680: C3DLow vt+0x10c(0,0) = lighting off, then vt+0x2c
  //   = FUN_00422b80: the view matrix becomes the inverse of the camera's
  //   world matrix with its translation zeroed (the group is drawn around
  //   the eye), ZFUNC = ALWAYS, ZWRITEENABLE = 0, and FOGENABLE = 0 when
  //   fog is on (the host pass has turned fog off already);
  //   material begin FUN_0045e6d0: C3DLow vt+0xe0(material, 0), the object
  //   mode, per pass vt+0xe4 (FUN_0045f700) and the entry draw with the
  //   instance world matrix;
  //   end FUN_00424a10: ZFUNC = LESSEQUAL, the view matrix back,
  //   ZWRITEENABLE = 1 (and fog back on).
  // Mode 0 of CMaterial_Tex (FUN_00434170 case 0 -> 1) sets the material,
  // binds the primary entry and leaves stage 0 on MODULATE: with lighting
  // off the colour is texture * vertex diffuse, and the v3 loader stores
  // the file's vertex colour OR 0xFF000000 as the diffuse of a material
  // that is not prelit (FUN_00451b00). (M2 spec Q7.)
  // Host: draw_section with lighting off; a vertex without a diffuse reads
  // white, and so does every vertex of skydome.SCX, the one mesh the 16
  // horizon render objects use (376 colours, all 0xFFFFFFFF).
  // Fork: a prelit section here is drawn the same way (the exe hands mode
  // 0 of a prelit material to its static-light material, FUN_00434170).
  if (!horizon.empty()) {
    D3DMATRIX eye_rot = cam_world;
    eye_rot.m[3][0] = eye_rot.m[3][1] = eye_rot.m[3][2] = 0.f;
    D3DMATRIX view_rot{};
    if (!invert_general(eye_rot, &view_rot)) invert_rigid(eye_rot, &view_rot);
    D3DMATRIX saved_view{};
    g_dev->GetTransform(D3DTS_VIEW, &saved_view);
    g_dev->SetTransform(D3DTS_VIEW, &view_rot);
    g_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    if (g_pass_fog) g_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    for (const Sec& s : horizon) {
      draw_section(s, false, true, true);
      ++g_cam_pass_stats.horizon_sections;
    }
    g_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    g_dev->SetTransform(D3DTS_VIEW, &saved_view);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    if (g_pass_fog) g_dev->SetRenderState(D3DRS_FOGENABLE, TRUE);
  }
  // rg_ground, then the object groups. The ground group leaves the state
  // the object groups start from.
  draw_ground_group(ground);
  if (boot940) {
    // rg_object_prelit: material mode 3 for the sections that have a
    // lightmap; the others as the host drew them before.
    std::vector<Mode3Sec> prelit_mode3;
    for (const Sec& s : opaque_prelit)
      if (s.mode3) prelit_mode3.push_back({s.key, s.ms, s.sub});
    draw_object_prelit_group(prelit_mode3);
    for (const Sec& s : opaque_prelit)
      if (!s.mode3) draw_section(s, false, true);
    for (const Sec& s : opaque_lit) draw_section(s, false, false);
    draw_camera_shadows(members, ground, cam_world, lights);
    for (const Sec& s : trans_prelit) {
      if (s.mode3)
        draw_object_prelit_transparent({s.key, s.ms, s.sub});
      else
        draw_section(s, true, true);
    }
  } else {
    for (const Sec& s : opaque_lit) draw_section(s, false, false);
    for (const Sec& s : opaque_prelit) draw_section(s, false, true);
    for (const Sec& s : trans_prelit) draw_section(s, true, true);
  }
  for (const Sec& s : trans_lit) draw_section(s, true, false);

  // Leave the state the other passes expect (FUN_00424a70 group end:
  // blend off, alpha test off, ZWRITE on).
  g_dev->SetTexture(0, nullptr);
  apply_envmap_stage1(nullptr);
  if (lights.any) end_camera_lights();
  g_dev->LightEnable(0, FALSE);
  g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  g_dev->SetRenderState(D3DRS_NORMALIZENORMALS, FALSE);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFFu);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
}

// PE 940 text section of a camera pass (rg_text_section, cam+0x4b4): defined
// in render_d3d9_part3.inc.
static void draw_camera_text_section(const std::vector<void*>& keys);

// PE 940 FUN_0045f880: the nodes a camera's walk met for the first time
// are built before its groups are drawn (FUN_00462750 / FUN_00462b40; a
// render instance requests its render type there). The build budgets are
// cam+0x73A4 / +0x73A8: 0x400 while the camera is fresh or the game loads,
// then 16 and 8 a frame (DAT_005da34c, DAT_005da350).
// Fork: the host loads the type at once (the exe files an asynchronous
// request unless the type has flag 0x1000), at most 8 instances per camera
// and frame, in creation order.
// Fork: the parent chain of a waiting instance is walked once per frame,
// not once per camera: every native part leaves its render instances
// waiting, and most of them hang under a car that no camera shows. `s_up`
// holds the chains above the instances one after the other with, per node,
// whether a WORLDTREELEAF container was met on the way there: the test of
// camera_sees for any root. The arrays keep their storage between frames.
static void materialise_pending_instances(void* root) {
  if (g_mesh_pending.empty()) return;
  struct Waiting {
    uint32_t seq;
    void* key;
    uint32_t first, count;  // its chain in s_up
  };
  static uint32_t s_frame = 0;
  static std::vector<Waiting> s_waiting;
  static std::vector<std::pair<void*, bool>> s_up;
  if (s_frame != g_cam_pass_frame) {
    s_frame = g_cam_pass_frame;
    s_waiting.clear();
    s_up.clear();
    for (const auto& pk : g_mesh_pending) {
      if (pk.second.state != 0) continue;
      Waiting w{pk.second.seq, pk.first, static_cast<uint32_t>(s_up.size()), 0};
      bool leaf = false;
      void* cur = chain_parent_of(pk.first);
      for (int guard = 0; cur && guard < 64; ++guard) {
        if (gameref_get_flags(static_cast<InvObject*>(cur)) & kGameRefWorldTreeLeaf)
          leaf = true;
        s_up.emplace_back(cur, leaf);
        ++w.count;
        cur = chain_parent_of(cur);
      }
      s_waiting.push_back(w);
    }
  }
  std::vector<std::pair<uint32_t, void*>> todo;
  for (const Waiting& w : s_waiting) {
    bool sees = (w.key == root);
    for (uint32_t i = 0; !sees && i < w.count; ++i) {
      const std::pair<void*, bool>& up = s_up[w.first + i];
      if (up.first != root) continue;
      sees = !up.second;
      break;
    }
    if (sees) todo.emplace_back(w.seq, w.key);
  }
  if (todo.empty()) return;
  std::sort(todo.begin(), todo.end());
  constexpr size_t kPendingPerPass = 8;
  if (todo.size() > kPendingPerPass) todo.resize(kPendingPerPass);
  for (const auto& t : todo) {
    auto it = g_mesh_pending.find(t.second);
    if (it == g_mesh_pending.end() || it->second.state != 0) continue;
    const int32_t type_id = it->second.type_id;
    // The callback creates meshes and textures: no iterator into the
    // renderer's maps is held across it.
    const int32_t made = renderref_materialise_pending(
        static_cast<InvObject*>(t.second), type_id);
    it = g_mesh_pending.find(t.second);
    if (it == g_mesh_pending.end()) continue;
    if (made > 0)
      g_mesh_pending.erase(it);
    else
      it->second.state = (made == 0) ? 1 : 2;
  }
}

// PE 940 present walk (GfxEngine PresentFrame FUN_00421920): every BOUND
// viewport in list order - FUN_00421e40 keeps the list ascending by
// priority, so the lowest priority is drawn first (bottom) and the highest
// last (top); among equals the most recently bound one comes first. Per
// viewport FUN_004653c0 sets the D3D viewport from the normalised rect and
// walks its cameras (FUN_00464f60: ascending camera priority, newest first
// among equals). Per camera FUN_00461dd0 builds view = inverse(camera bone00
// world) and the right-handed projection, FUN_0045e910 collects the
// instances below the camera root and FUN_00460ba0 clears DEPTH ONLY
// (cam+0x1a4 & vp+0x24 never carry the target bit; Z 0x3F7FFF58, inside the
// viewport) and draws the render groups, the text section last.
// Fork: instances already in g_mesh_queue (legacy world path) are left to
// draw_meshes, and a camera that sees neither an instance nor a text is
// skipped (its depth clear would have no effect).
// The legacy --game path runs this pass too: the old build binds the
// navigator viewport (priority 12) with a camera over the 64 map tiles. In
// the --no-wait smoke run the finished frame does not show it: the legacy
// OSD rectangles drawn afterwards (draw_osd_rects) cover that part of the
// screen (SLRR_CAM_PASS_SHOT_STAGE=pass dumps the frame before them).
void draw_viewport_cameras() {
#ifdef _WIN32
  g_cam_pass_stats = CamPassStats{};
  g_cam_pass_log.clear();
  if (!g_dev) return;
  ++g_cam_pass_frame;
  if (g_cam_pass_frame == 0) g_cam_pass_frame = 1;  // 0 = never drawn
  for (const auto& kv : g_viewports)
    if (kv.second.active) ++g_cam_pass_stats.bound_vps;
  if (g_cameras.empty()) return;
  static const bool trace_on = std::getenv("SLRR_RENDER_TRACE") != nullptr;
  struct Pass {
    uint32_t vp_pri, vp_seq, cam_pri, cam_seq;
    void* cam;
  };
  std::vector<Pass> passes;
  for (const auto& kv : g_cameras) {
    if (!kv.second.viewport) continue;
    auto vit = g_viewports.find(kv.second.viewport);
    if (vit == g_viewports.end() || !vit->second.active) continue;
    passes.push_back({static_cast<uint32_t>(vit->second.pri), vit->second.bind_seq,
                      static_cast<uint32_t>(kv.second.pri), kv.second.seq, kv.first});
  }
  if (passes.empty()) return;
  // Unsigned compares as in the exe (0x421ef6 CMP / JNC).
  std::sort(passes.begin(), passes.end(), [](const Pass& a, const Pass& b) {
    if (a.vp_pri != b.vp_pri) return a.vp_pri < b.vp_pri;
    if (a.vp_seq != b.vp_seq) return a.vp_seq > b.vp_seq;
    if (a.cam_pri != b.cam_pri) return a.cam_pri < b.cam_pri;
    if (a.cam_seq != b.cam_seq) return a.cam_seq > b.cam_seq;
    return a.cam < b.cam;
  });
  float kPeClearZ = 1.f;
  {
    const uint32_t zbits = 0x3F7FFF58u;  // PE 940 FUN_004231a0 Clear Z
    std::memcpy(&kPeClearZ, &zbits, sizeof(kPeClearZ));
  }
  const std::vector<void*> saved_queue = g_mesh_queue;
  void* const saved_vp = g_active_vp;
  // PE 940 FUN_0045e910 -> FUN_00461980: every camera gives the nodes of
  // its walk a detail value, and the level of detail of a part or car
  // follows the largest of the frame (FUN_0051a5f0).
  // Host: all cameras first, then the levels, then the passes, so that an
  // instance of the wrong level is neither loaded nor drawn. Build 940 boot
  // only.
  if (render_boot940()) {
    for (const Pass& pc : passes) camera_offer_detail(pc.cam);
    native_camera_detail_end(g_cam_pass_frame);
    lit_frame_begin();  // the global envmap of the frame
    shadow_frame_begin();
  }
  for (const Pass& pc : passes) {
    void* cam_key = pc.cam;
    const CameraState& cam = g_cameras[cam_key];
    void* const start = cam.parent ? cam.parent : cam_key;
    void* root = camera_root_of(start);
    // Instances this camera sees whose render type is not loaded yet.
    materialise_pending_instances(root);
    // The light instances it sees (FUN_0045f880 refreshes the camera's
    // light list every frame).
    CamLights lights;
    collect_camera_lights(root, &lights);
    // Instances below the camera root, in creation order (Fork: the exe
    // queues them in tree-walk order).
    std::vector<void*> members;
    for (const auto& mk : g_meshes) {
      if (!mk.second.ready || mk.first == cam_key) continue;
      if (std::find(saved_queue.begin(), saved_queue.end(), mk.first) != saved_queue.end()) continue;
      bool hidden = false;
      if (!camera_sees(root, mk.first, &hidden)) {
        if (hidden) ++g_cam_pass_stats.hidden_instances;
        continue;
      }
      if (native_filter_hidden(chain_parent_of(mk.first))) continue;  // m6 step 5
      members.push_back(mk.first);
    }
    std::sort(members.begin(), members.end(), [](void* a, void* b) {
      const uint32_t sa = g_meshes[a].seq, sb = g_meshes[b].seq;
      return sa != sb ? sa < sb : a < b;
    });
    // PE 940: an r_text instance is part of the same walk (rg_text_section).
    std::vector<void*> texts;
    for (const auto& tk : g_texts) {
      const TextState& t = tk.second;
      if (!t.tree || !t.alive || !t.visible || !t.font || t.text.empty()) continue;
      bool hidden = false;
      if (!camera_sees(root, tk.first, &hidden)) {
        if (hidden) ++g_cam_pass_stats.hidden_texts;
        continue;
      }
      texts.push_back(tk.first);
    }
    const bool trace = trace_on && g_vp_pass_trace_budget > 0;
    if (trace && members.empty() && g_vp_pass_trace_budget == 40) {
      // One-time chain dump: how the ready meshes hang together.
      int shown = 0;
      for (const auto& mk : g_meshes) {
        if (!mk.second.ready || shown++ >= 12) continue;
        void* cur = mk.first;
        std::string chain;
        for (int g = 0; cur && g < 8; ++g) {
          char b[96];
          const char* cn = tree_host_class(static_cast<InvObject*>(cur));
          auto xit = g_mesh_xforms.find(cur);
          void* xp = (xit != g_mesh_xforms.end()) ? xit->second.parent : nullptr;
          void* tp = (xit != g_mesh_xforms.end()) ? xit->second.tree_parent : nullptr;
          void* gp = gameref_get_parent(static_cast<InvObject*>(cur));
          std::snprintf(b, sizeof b, " %p(%s;xf=%p;tp=%p;gr=%p)", cur, cn ? cn : "", xp, tp, gp);
          chain += b;
          void* next = xp ? xp : (tp ? tp : gp);
          if (!next || next == cur) break;
          cur = next;
        }
        std::fprintf(stderr, "[render] chain%s\n", chain.c_str());
      }
      std::fprintf(stderr, "[render] cam %p parent %p(%s) gr=%p\n", cam_key, cam.parent,
                   cam.parent && tree_host_class(static_cast<InvObject*>(cam.parent)) ? tree_host_class(static_cast<InvObject*>(cam.parent)) : "",
                   gameref_get_parent(static_cast<InvObject*>(cam.parent)));
    }
    if (members.empty() && texts.empty()) continue;  // Fork: nothing to draw
    ++g_cam_pass_stats.passes;
    {
      auto cit = g_cameras.find(cam_key);
      if (cit != g_cameras.end()) cit->second.pass_frame = g_cam_pass_frame;
    }
    g_cam_pass_stats.instances += static_cast<int32_t>(members.size());
    // Viewport rect (FUN_00464d30 pixel rect + SetViewport).
    g_active_vp = cam.viewport;
    apply_active_viewport();
    const ViewportState& vps = g_viewports[cam.viewport];
    // Projection and view = inverse of the camera's world matrix, own
    // bone00 pose folded (camera_pass_matrices). With the RH projection the
    // camera looks down its -Z, +Y up, +X right.
    float vp_aspect = 4.f / 3.f, xs = 1.f, ys = 1.f;
    D3DMATRIX proj{}, cam_world{}, view{};
    camera_pass_matrices(cam_key, cam, &cam_world, &view, &proj, &xs, &ys,
                         &vp_aspect);
    g_dev->SetTransform(D3DTS_PROJECTION, &proj);
    g_dev->SetTransform(D3DTS_VIEW, &view);
    g_fold_own_bone0 = true;  // instances and texts: own bone00 pose folded
    camera_pass_fog(cam);  // FUN_00461dd0 -> FUN_00427260
    // FUN_00460ba0 -> FUN_004231a0: depth only, never the target.
    g_dev->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, kPeClearZ, 0);
    void* const top = mesh_root_of(start);
    auto ndc_of = [&](void* key, float* vx, float* vy, float* vz, float* nx, float* ny) {
      D3DMATRIX w{};
      resolve_world(key, &w, 0);
      const float wx = w.m[3][0], wy = w.m[3][1], wz = w.m[3][2];
      *vx = wx * view.m[0][0] + wy * view.m[1][0] + wz * view.m[2][0] + view.m[3][0];
      *vy = wx * view.m[0][1] + wy * view.m[1][1] + wz * view.m[2][1] + view.m[3][1];
      *vz = wx * view.m[0][2] + wy * view.m[1][2] + wz * view.m[2][2] + view.m[3][2];
      // RH clip w = -view z.
      *nx = *vz != 0.f ? *vx * xs / -*vz : 0.f;
      *ny = *vz != 0.f ? *vy * ys / -*vz : 0.f;
    };
    auto tex_label_of = [&](void* key) -> const char* {
      const MeshState& ms = g_meshes[key];
      if (!ms.subs.empty() && ms.subs[0].texture_key) {
        auto tit = g_textures.find(ms.subs[0].texture_key);
        if (tit != g_textures.end()) return tit->second.label.c_str();
      }
      return "";
    };
    if (trace_on) {
      // Last-frame log for render_d3d9_debug_dump.
      char b[512];
      std::snprintf(b, sizeof b,
                    "pass %d: vp=%p pri=%u seq=%u rect=(%g,%g,%g,%g) cam=%p cam_pri=%u aov=%g aspect=%g root=%p top=%p%s instances=%zu texts=%zu eye=(%g,%g,%g)",
                    g_cam_pass_stats.passes - 1, cam.viewport, pc.vp_pri, pc.vp_seq, vps.x, vps.y, vps.w, vps.h,
                    cam_key, pc.cam_pri, cam.aov_deg, cam.aspect != 0.f ? cam.aspect : vp_aspect, root, top,
                    root != top ? " (root below the topmost node)" : "", members.size(), texts.size(),
                    cam_world.m[3][0], cam_world.m[3][1], cam_world.m[3][2]);
      g_cam_pass_log.emplace_back(b);
      for (size_t i = 0; i < members.size() && i < 48; ++i) {
        float vx, vy, vz, nx, ny;
        ndc_of(members[i], &vx, &vy, &vz, &nx, &ny);
        const MeshState& ms = g_meshes[members[i]];
        // Half extents in NDC of the local AABB (cm) at the instance depth.
        const float hw = vz != 0.f ? 0.5f * (ms.bmax[0] - ms.bmin[0]) * ms.unit * xs / -vz : 0.f;
        const float hh = vz != 0.f ? 0.5f * (ms.bmax[1] - ms.bmin[1]) * ms.unit * ys / -vz : 0.f;
        std::snprintf(b, sizeof b, "  inst %p ndc=(%.4f,%.4f) half=(%.4f,%.4f) view_z=%.4f matflags=%08X shader=%d diffuse=%08X typeflags=%u tex=%s",
                      members[i], nx, ny, hw, hh, vz,
                      ms.subs.empty() ? 0u : ms.subs[0].mat_flags,
                      ms.subs.empty() ? -1 : ms.subs[0].mat_shader,
                      ms.subs.empty() ? 0u : ms.subs[0].mat_diffuse,
                      render_d3d9_mesh_type_flags(members[i]), tex_label_of(members[i]));
        g_cam_pass_log.emplace_back(b);
      }
    }
    if (trace) {
      --g_vp_pass_trace_budget;
      std::fprintf(stderr, "[render] vp-pass cam=%p vp=%p vp_pri=%u vp_seq=%u cam_pri=%u parent=%p root=%p top=%p members=%zu texts=%zu cam_pos=(%g,%g,%g) aov=%g aspect=%g\n",
                   cam_key, cam.viewport, pc.vp_pri, pc.vp_seq, pc.cam_pri, cam.parent, root, top,
                   members.size(), texts.size(), cam_world.m[3][0], cam_world.m[3][1], cam_world.m[3][2],
                   cam.aov_deg, cam.aspect != 0.f ? cam.aspect : vp_aspect);
      if (root != top)
        std::fprintf(stderr, "[render]   camera root %p is below the topmost node %p\n", root, top);
      for (size_t i = 0; i < members.size() && i < 6; ++i) {
        D3DMATRIX w{}; resolve_world(members[i], &w, 0);
        const MeshState& ms = g_meshes[members[i]];
        {
          auto xit = g_mesh_xforms.find(members[i]);
          auto bit = g_mesh_bones.find(members[i]);
          const MeshXform* b0 = nullptr;
          if (bit != g_mesh_bones.end()) { auto b = bit->second.find(0); if (b != bit->second.end()) b0 = &b->second; }
          std::fprintf(stderr, "[render]   member-xf %p xf.p=(%g,%g,%g) xf.parent=%p tree=%p bone0=%s(%g,%g,%g) nbones=%zu\n",
                       members[i], xit != g_mesh_xforms.end() ? xit->second.px : -1.f, xit != g_mesh_xforms.end() ? xit->second.py : -1.f, xit != g_mesh_xforms.end() ? xit->second.pz : -1.f,
                       xit != g_mesh_xforms.end() ? xit->second.parent : nullptr, xit != g_mesh_xforms.end() ? xit->second.tree_parent : nullptr,
                       b0 ? "y" : "n", b0 ? b0->px : 0.f, b0 ? b0->py : 0.f, b0 ? b0->pz : 0.f, bit != g_mesh_bones.end() ? bit->second.size() : (size_t)0);
        }
        std::fprintf(stderr, "[render]   member %p pos=(%g,%g,%g) bmin=(%g,%g,%g) bmax=(%g,%g,%g) subs=%zu tex=%s\n",
                     members[i], w.m[3][0], w.m[3][1], w.m[3][2], ms.bmin[0], ms.bmin[1], ms.bmin[2],
                     ms.bmax[0], ms.bmax[1], ms.bmax[2], ms.subs.size(), tex_label_of(members[i]));
      }
      for (size_t i = 0; i < members.size() && i < 12; ++i) {
        float vx, vy, vz, nx, ny;
        ndc_of(members[i], &vx, &vy, &vz, &nx, &ny);
        std::fprintf(stderr, "[render]   ndc %p view=(%g,%g,%g) ndc=(%g,%g)\n", members[i], vx, vy, vz, nx, ny);
      }
    }
    g_pass_cam = cam_key;
    draw_camera_instances(members, cam_world, lights);
    draw_camera_text_section(texts);  // rg_text_section: after every mesh group
    g_fold_own_bone0 = false;
  }
  // Fork: the host's 2D layers after the passes draw without fog.
  if (g_pass_fog) g_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
  g_pass_fog = false;
  g_mesh_queue = saved_queue;
  g_active_vp = saved_vp;
  if (g_active_vp) apply_active_viewport();
#endif
}

void draw_meshes() {
#ifdef _WIN32
  if (!g_dev || g_mesh_queue.empty()) return;

  auto xform_point = [](const D3DMATRIX& m, float x, float y, float z,
                        float* ox, float* oy, float* oz) {
    *ox = x * m.m[0][0] + y * m.m[1][0] + z * m.m[2][0] + m.m[3][0];
    *oy = x * m.m[0][1] + y * m.m[1][1] + z * m.m[2][1] + m.m[3][1];
    *oz = x * m.m[0][2] + y * m.m[1][2] + z * m.m[2][2] + m.m[3][2];
  };

  // Look-at around union of transformed AABBs.
  bool have_bounds = false;
  float bmin[3] = {0, 0, 0};
  float bmax[3] = {0, 0, 0};
  for (void* key : g_mesh_queue) {
    auto it = g_meshes.find(key);
    if (it == g_meshes.end() || !it->second.ready) continue;
    const MeshState& ms = it->second;
    D3DMATRIX world{};
    resolve_world(key, &world, 0);
    const float xs[2] = {ms.bmin[0], ms.bmax[0]};
    const float ys[2] = {ms.bmin[1], ms.bmax[1]};
    const float zs[2] = {ms.bmin[2], ms.bmax[2]};
    for (int i = 0; i < 2; ++i)
      for (int j = 0; j < 2; ++j)
        for (int k = 0; k < 2; ++k) {
          float wx, wy, wz;
          xform_point(world, xs[i], ys[j], zs[k], &wx, &wy, &wz);
          if (!have_bounds) {
            bmin[0] = bmax[0] = wx;
            bmin[1] = bmax[1] = wy;
            bmin[2] = bmax[2] = wz;
            have_bounds = true;
          } else {
            if (wx < bmin[0]) bmin[0] = wx;
            if (wy < bmin[1]) bmin[1] = wy;
            if (wz < bmin[2]) bmin[2] = wz;
            if (wx > bmax[0]) bmax[0] = wx;
            if (wy > bmax[1]) bmax[1] = wy;
            if (wz > bmax[2]) bmax[2] = wz;
          }
        }
  }
  if (have_bounds) {
    const float cx = 0.5f * (bmin[0] + bmax[0]);
    const float cy = 0.5f * (bmin[1] + bmax[1]);
    const float cz = 0.5f * (bmin[2] + bmax[2]);
    float ext = bmax[0] - bmin[0];
    if (bmax[1] - bmin[1] > ext) ext = bmax[1] - bmin[1];
    if (bmax[2] - bmin[2] > ext) ext = bmax[2] - bmin[2];
    if (ext < 1.f) ext = 1.f;
    set_lookat_view(cx, cy + ext * 0.35f, cz - ext * 1.6f, cx, cy, cz);
  }

  // Soft PE PresentFrame draw path stand-in (GfxEngine_PresentFrame@4FCA30
  // → device vt+40 Present after draw). Default FVF = XYZ|NORMAL|TEX1
  // (0x112) — stock UploadVbFvf@503475..5034EE builds pe_fvf per mesh;
  // GfxDevice_DrawIndexedTris@4ED080 does not SetFVF (assumes prior bind).
  const DWORD fvf_default = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;
  g_dev->SetFVF(fvf_default);
  g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_LIGHTING, TRUE);
  g_dev->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
  g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

  apply_scene_light();

  // Soft material: global envmap → stage1 (setGlobalEnvmap@47C220 store;
  // D3D bind via apply_envmap_stage1 — ADD + TCI reflection).
  IDirect3DBaseTexture9* env_tex = nullptr;
  if (g_envmap) {
    auto eit = g_textures.find(g_envmap);
    if (eit != g_textures.end()) env_tex = texture_d3d_base(eit->second);
  }

  for (void* key : g_mesh_queue) {
    auto it = g_meshes.find(key);
    if (it == g_meshes.end() || !it->second.ready) continue;
    D3DMATRIX world{};
    resolve_world(key, &world, 0);
    g_dev->SetTransform(D3DTS_WORLD, &world);

    const MeshXform* xf = nullptr;
    {
      auto xit = g_mesh_xforms.find(key);
      if (xit != g_mesh_xforms.end()) xf = &xit->second;
    }

    // Soft PE mesh upload Present bind: UploadVbFvf@503400 + UploadIbTris
    // @5038E0 → GfxDevice_DrawIndexedTris@4ED080 (SetStreamSource vt+400 /
    // SetIndices vt+416 / DrawIndexedPrimitive vt+328 TRIANGLELIST=4).
    const MeshVbState* uploaded_vb = nullptr;
    const MeshIbState* uploaded_ib = nullptr;
    {
      auto vit = g_mesh_vbs.find(key);
      auto iit = g_mesh_ibs.find(key);
      if (vit != g_mesh_vbs.end() && vit->second.ready && vit->second.vb &&
          iit != g_mesh_ibs.end() && iit->second.ready && iit->second.ib &&
          iit->second.index_count >= 3) {
        uploaded_vb = &vit->second;
        uploaded_ib = &iit->second;
      }
    }

    if (uploaded_vb && uploaded_ib) {
      // Soft materials (CMaterial_Tex stand-in): stage0 modulate tex×diffuse.
      IDirect3DBaseTexture9* tex0 = nullptr;
      int32_t tex_fmt = 3;
      if (!it->second.subs.empty() && it->second.subs[0].texture_key) {
        auto tit = g_textures.find(it->second.subs[0].texture_key);
        if (tit != g_textures.end()) {
          tex0 = texture_d3d_base(tit->second);
          tex_fmt = tit->second.engine_fmt;
        }
      }
      g_dev->SetTexture(0, tex0);
      if (tex0) {
        g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      } else {
        g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
      }
      // DXT3/DXT5 (engine_fmt 13/7 via FormatFromFourcc@4F8990) → alpha blend.
      const bool alpha_tex = (tex_fmt == 7 || tex_fmt == 13);
      g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, alpha_tex ? TRUE : FALSE);
      if (alpha_tex) {
        g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
      }
      apply_envmap_stage1(env_tex);

      D3DMATERIAL9 mat{};
      const uint32_t dcol =
          (xf && xf->color_set) ? static_cast<uint32_t>(xf->color)
                                : (!it->second.subs.empty()
                                       ? it->second.subs[0].diffuse
                                       : 0xFF808080u);
      const float r = static_cast<float>((dcol >> 16) & 0xff) / 255.f;
      const float g = static_cast<float>((dcol >> 8) & 0xff) / 255.f;
      const float b = static_cast<float>(dcol & 0xff) / 255.f;
      mat.Diffuse.r = mat.Ambient.r = r;
      mat.Diffuse.g = mat.Ambient.g = g;
      mat.Diffuse.b = mat.Ambient.b = b;
      mat.Diffuse.a = mat.Ambient.a = 1.f;
      g_dev->SetMaterial(&mat);

      // Soft SetFVF(pe_fvf) — CreateVertexBuffer FVF; PE@4ED080 omits SetFVF.
      const DWORD pe_fvf =
          uploaded_vb->pe_fvf != 0 ? uploaded_vb->pe_fvf : fvf_default;
      g_dev->SetFVF(pe_fvf);
      // PE@4ED080: SetStreamSource(0, *(vb+0x18), 0, *(vb+0x0C)=stride);
      // SetIndices(*ib); DrawIndexed(4, baseVert, 0, nVert, startIdx, nPrim).
      // Soft hosts baseVert=0 / startIdx=0; nPrim = index_count/3 (IB tris).
      g_dev->SetStreamSource(0, uploaded_vb->vb, 0,
                             static_cast<UINT>(uploaded_vb->stride));
      g_dev->SetIndices(uploaded_ib->ib);
      const UINT nvert = static_cast<UINT>(uploaded_vb->vert_count);
      const UINT ntri =
          static_cast<UINT>(uploaded_ib->index_count / 3);
      if (nvert > 0 && ntri > 0)
        g_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, nvert, 0, ntri);
      g_dev->SetStreamSource(0, nullptr, 0, 0);
      g_dev->SetIndices(nullptr);
      g_dev->SetFVF(fvf_default);
      apply_envmap_stage1(nullptr);
      g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
      continue;
    }

    for (const MeshSubmesh& sm : it->second.subs) {
      if (sm.verts.empty() || sm.indices.size() < 3) continue;

      IDirect3DBaseTexture9* tex = nullptr;
      int32_t tex_fmt = 3;
      if (sm.texture_key) {
        auto tit = g_textures.find(sm.texture_key);
        if (tit != g_textures.end()) {
          tex = texture_d3d_base(tit->second);
          tex_fmt = tit->second.engine_fmt;
        }
      }
      g_dev->SetTexture(0, tex);
      if (tex) {
        g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      } else {
        g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
      }
      // Soft materials: DXT3/5 alpha (FormatFromFourcc → fmt 13/7).
      const bool alpha_tex = (tex_fmt == 7 || tex_fmt == 13);
      g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, alpha_tex ? TRUE : FALSE);
      if (alpha_tex) {
        g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
      }
      apply_envmap_stage1(env_tex);

      D3DMATERIAL9 mat{};
      const uint32_t d =
          (xf && xf->color_set) ? static_cast<uint32_t>(xf->color)
                                : sm.diffuse;
      const float r = static_cast<float>((d >> 16) & 0xff) / 255.f;
      const float g = static_cast<float>((d >> 8) & 0xff) / 255.f;
      const float b = static_cast<float>(d & 0xff) / 255.f;
      if (xf && xf->color_set) {
        mat.Diffuse.r = mat.Ambient.r = r;
        mat.Diffuse.g = mat.Ambient.g = g;
        mat.Diffuse.b = mat.Ambient.b = b;
      } else {
        mat.Diffuse.r = mat.Ambient.r = r > 0.05f ? r : 0.75f;
        mat.Diffuse.g = mat.Ambient.g = g > 0.05f ? g : 0.78f;
        mat.Diffuse.b = mat.Ambient.b = b > 0.05f ? b : 0.82f;
      }
      mat.Diffuse.a = mat.Ambient.a = 1.f;
      g_dev->SetMaterial(&mat);

      const UINT ntri = static_cast<UINT>(sm.indices.size() / 3);
      g_dev->DrawIndexedPrimitiveUP(
          D3DPT_TRIANGLELIST, 0, static_cast<UINT>(sm.verts.size()), ntri,
          sm.indices.data(), D3DFMT_INDEX16, sm.verts.data(),
          sizeof(MeshVertex));
    }
  }

  g_dev->SetTexture(0, nullptr);
  apply_envmap_stage1(nullptr);
  g_dev->LightEnable(0, FALSE);
  g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  g_dev->SetRenderState(D3DRS_NORMALIZENORMALS, FALSE);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
#endif
}

void queue_flare_sprites() {
  if (!g_flares_enabled) {
    // Still strip prior OSD keys so disable takes effect immediately.
    for (const FlareState& f : g_flares) {
      if (!f.key) continue;
      for (int i = 0; i < 32; ++i) {
        void* k = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(f.key) ^
            (0xF1000000u + static_cast<uint32_t>(i)));
        render_d3d9_osd_remove_rect(k);
      }
      for (int i = 0; i < 16; ++i) {
        void* k = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(f.key) ^
            (0xF2000000u + static_cast<uint32_t>(i)));
        render_d3d9_osd_remove_rect(k);
      }
    }
    g_flare_sprites_last = 0;
    return;
  }
  // Remove prior flare OSD keys, then lay sprites along sun→center streak.
  for (const FlareState& f : g_flares) {
    if (!f.key) continue;
    for (int i = 0; i < 32; ++i) {
      void* k = reinterpret_cast<void*>(
          reinterpret_cast<uintptr_t>(f.key) ^
          (0xF1000000u + static_cast<uint32_t>(i)));
      render_d3d9_osd_remove_rect(k);
    }
    for (int i = 0; i < 16; ++i) {
      void* k = reinterpret_cast<void*>(
          reinterpret_cast<uintptr_t>(f.key) ^
          (0xF2000000u + static_cast<uint32_t>(i)));
      render_d3d9_osd_remove_rect(k);
    }
  }
  g_flare_sprites_last = 0;
  for (FlareState& f : g_flares) {
    if (!f.key || f.count <= 0) continue;
    float sun_x = 0.55f;
    float sun_y = 0.42f;
    f.behind = false;
    f.has_screen = false;
    if (f.has_world) {
      float px = 0, py = 0;
      if (!render_d3d9_project(f.wx, f.wy, f.wz, &px, &py)) {
        f.behind = true;
        continue;  // behind / invalid — hide
      }
      sun_x = px;
      sun_y = py;
      f.sx = px;
      f.sy = py;
      f.has_screen = true;
    }
    const int n = f.count > 24 ? 24 : f.count;
    uint32_t argb = static_cast<uint32_t>(f.color);
    if ((argb & 0xFF000000u) == 0) argb |= 0xC0000000u;
    for (int i = 0; i < n; ++i) {
      const float t =
          n <= 1 ? 0.f : static_cast<float>(i) / static_cast<float>(n - 1);
      const float x = sun_x + (0.f - sun_x) * (t * 1.6f);
      const float y = sun_y + (0.f - sun_y) * (t * 1.6f);
      float sz = f.min_size + (f.max_size - f.min_size) * t;
      sz *= 0.035f;
      if (sz < 0.02f) sz = 0.02f;
      if (sz > 0.55f) sz = 0.55f;
      void* k = reinterpret_cast<void*>(
          reinterpret_cast<uintptr_t>(f.key) ^
          (0xF1000000u + static_cast<uint32_t>(i)));
      render_d3d9_osd_set_rect_color(k, x, y, sz, sz, f.tex, argb, 60 + i);
      ++g_flare_sprites_last;
    }
    const int rays = f.rays > 12 ? 12 : f.rays;
    for (int r = 0; r < rays; ++r) {
      const float ang =
          (6.2831853f * static_cast<float>(r)) /
          static_cast<float>(rays > 0 ? rays : 1);
      const float len = f.min_size * 0.04f;
      const float x = sun_x + std::cos(ang) * len * 0.5f;
      const float y = sun_y + std::sin(ang) * len * 0.5f;
      void* k = reinterpret_cast<void*>(
          reinterpret_cast<uintptr_t>(f.key) ^
          (0xF2000000u + static_cast<uint32_t>(r)));
      render_d3d9_osd_set_rect_color(k, x, y, len * 0.15f, len, f.tex, argb,
                                     50 + r);
      ++g_flare_sprites_last;
    }
  }
}
