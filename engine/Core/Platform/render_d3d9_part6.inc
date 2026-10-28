// Fragment 6 of render_d3d9.cpp - included, not compiled on its own (after
// fragments 1 to 5).
//
// M3 step 5: the lit material classes of Build 940 in object mode 0, as the
// lit groups of a camera pass draw them (rg_object cam+0x5b4, rg_transparent
// cam+0x784). Build 940 boot only.
//   lit_class_of               FUN_0042bff0 (the factory) and what the class
//                              constructors turn a class into by its flags
//   lit_bind / lit_unbind      FUN_0043d990 / FUN_0043d930
//   lit_texgen / lit_texgen_off  FUN_00441ad0 / FUN_00440970
//   lit_material*              FUN_00440b50 / FUN_00440d50 / FUN_00440e40
//   lit_apply                  apply(pass) of the 17 classes, each at the
//                              start value of its selector
//   draw_lit_section           FUN_0043f3a0 (begin), the loops of the two
//                              groups (FUN_00467c70, FUN_00468490),
//                              FUN_0043f460 (end: apply(-1), cull mode back)
//   lit_frame_begin, render_d3d9_set_global_envmap_id
//                              FUN_004dc910 / FUN_00421920 (global envmap)
//   render_d3d9_lit_debug_dump   host: `[render-dbg]` lines

namespace inv {

// PE 940 GfxEngine.setGlobalEnvmap (FUN_004dc910): the engine keeps a handle
// on the texture node (engine+0x98) and its frame function FUN_00421920
// hands the node's texture to C3DLow (+0x39C) every frame: null (the stage
// then samples the default texture) until the texture is in.
// Host, script boot: the resource id of the handle the script passes;
// lit_frame_begin asks for its texture once per flushed frame.
// Fork: loaded at once on first use (renderref_envmap_texture).
static int32_t g_envmap_id = 0;
static void* g_envmap_tex = nullptr;  // g_textures key, null = not loaded

void render_d3d9_set_global_envmap_id(int32_t res_id) { g_envmap_id = res_id; }

// One record per section a class program drew in the frame being built
// (render_d3d9_lit_debug_dump prints the last flushed frame).
struct LitRec {
  void* key;
  int16_t sub;
  int16_t cls;
  int16_t pass;     // index of the camera pass
  uint8_t passes;   // material passes drawn
  uint8_t env_src;  // 0 no env stage, 1 the entry's texture, 2 global, 3 default
  uint8_t wide;     // drawn with two uv sets
  uint8_t two_bone;
};
static std::vector<LitRec> g_lit_recs;
static int g_lit_no_program = 0;   // parsed material, class without a program
static int g_lit_not_drawn = 0;    // begin() returned zero passes
static int g_lit_cap_missing = 0;  // drawn without its env stage (caps)
static bool g_lit_cap_logged = false;

static void lit_frame_begin() {
  g_lit_recs.clear();
  g_lit_no_program = 0;
  g_lit_not_drawn = 0;
  g_lit_cap_missing = 0;
  g_envmap_tex = g_envmap_id ? renderref_envmap_texture(g_envmap_id) : nullptr;
}

namespace {

// The classes of FUN_0042bff0 that have a program here (the class id the
// constructor stores at material+4).
enum LitClass : int {
  kLitNone = -1,
  kLitTex = 0x00,
  kLitMixTA = 0x01,
  kLitColorMixTex = 0x03,
  kLitTexSpecular = 0x04,
  kLitMixTASpecular = 0x05,
  kLitColorMixTexSpecular = 0x07,
  kLitTexSpheremap = 0x08,
  kLitMixTASpheremap = 0x09,
  kLitColorMixTexSpheremap = 0x0B,
  kLitTexSpecularSpheremap = 0x0C,
  kLitMixTASpecularSpheremap = 0x0D,
  kLitColorMixTexSpecularSpheremap = 0x0F,
  kLitMixTASpecularSpheremapMasked = 0x15,
  kLitColor = 0x30,
  kLitColorSpecular = 0x31,
  kLitColorSpheremap = 0x32,
  kLitColorSpecularSpheremap = 0x33,
};

const char* lit_class_name(int cls) {
  switch (cls) {
    case kLitTex: return "Tex";
    case kLitMixTA: return "MixTA";
    case kLitColorMixTex: return "ColorMixTex";
    case kLitTexSpecular: return "TexSpecular";
    case kLitMixTASpecular: return "MixTASpecular";
    case kLitColorMixTexSpecular: return "ColorMixTexSpecular";
    case kLitTexSpheremap: return "TexSpheremap";
    case kLitMixTASpheremap: return "MixTASpheremap";
    case kLitColorMixTexSpheremap: return "ColorMixTexSpheremap";
    case kLitTexSpecularSpheremap: return "TexSpecularSpheremap";
    case kLitMixTASpecularSpheremap: return "MixTASpecularSpheremap";
    case kLitColorMixTexSpecularSpheremap: return "ColorMixTexSpecularSpheremap";
    case kLitMixTASpecularSpheremapMasked: return "MixTASpecularSpheremapMasked";
    case kLitColor: return "Color";
    case kLitColorSpecular: return "ColorSpecular";
    case kLitColorSpheremap: return "ColorSpheremap";
    case kLitColorSpecularSpheremap: return "ColorSpecularSpheremap";
    default: return "?";
  }
}

// PE 940 FUN_0042bff0(shader id & 0xffffff, entries, count, flags): the
// constructor the factory calls, then what that constructor does with the
// create flags (asm of the 17 constructors, m3\mat\a07 and a15): several
// hand the material to a simpler class when a flag is set (0x80000000
// prelit; 0x08000000 is on 16 lit sections of the install, 10 of them
// TexSpecular):
//   0x33 -> 0x32 (0x88000000)        0x32 -> 0x30 (0x08000000)
//   0x31 -> 0x30 (0x88000000)        4 -> 0 (0x88000000)
//   0x0C -> 8 (0x88000000)           8 -> 0 (0x68000000)
//   0x0D, 0x15 -> 9 (0x88000000)     9 -> 1 (0x88000000)
//   5 -> 1 (0x88000000)              1 -> 0 (0x08000000)
//   7, 0x0B, 0x0F -> 3 (0x88000000)  3 -> 0x30 (0x08000000)
// The constructors also step down for a transparent or alpha-test material
// (0x60000000) when the selector of the class is 0; the selectors start at
// 1, 2 or 3 (file bytes) and the host takes the start values (Fork, Q11).
// Returns kLitNone for a class without a program here: CMaterial_MixVCA (2,
// 6, 0xA, ..), the bump classes 0x18 / 0x1C / 0x1D, 0x34, 0x35, 0x37 and
// 0xFFFFFB .. 0xFFFFFF. Every other id is CMaterial_Tex (the factory's
// default case).
int lit_class_of(int32_t shader, uint32_t flags) {
  int c = kLitNone;
  switch (static_cast<uint32_t>(shader) & 0xffffffu) {
    case 0x00: c = kLitTex; break;
    case 0x01: case 0x19: c = kLitMixTA; break;
    case 0x03: case 0x1b: c = kLitColorMixTex; break;
    case 0x04: c = kLitTexSpecular; break;
    case 0x05: c = kLitMixTASpecular; break;
    case 0x07: case 0x1f: c = kLitColorMixTexSpecular; break;
    case 0x08: case 0x10: case 0x20: c = kLitTexSpheremap; break;
    case 0x09: case 0x11: case 0x21: c = kLitMixTASpheremap; break;
    case 0x0b: case 0x13: case 0x23: c = kLitColorMixTexSpheremap; break;
    case 0x0c: case 0x14: case 0x24: c = kLitTexSpecularSpheremap; break;
    case 0x0d: case 0x25: c = kLitMixTASpecularSpheremap; break;
    case 0x0f: case 0x17: case 0x27: c = kLitColorMixTexSpecularSpheremap; break;
    case 0x15: c = kLitMixTASpecularSpheremapMasked; break;
    case 0x30: c = kLitColor; break;
    case 0x31: c = kLitColorSpecular; break;
    case 0x32: c = kLitColorSpheremap; break;
    case 0x33: c = kLitColorSpecularSpheremap; break;
    case 0x02: case 0x06: case 0x0a: case 0x0e: case 0x12: case 0x16:
    case 0x1a: case 0x1e: case 0x22: case 0x26:
    case 0x18: case 0x1c: case 0x1d: case 0x34: case 0x35: case 0x37:
    case 0xfffffb: case 0xfffffc: case 0xfffffd: case 0xfffffe:
    case 0xffffff:
      return kLitNone;
    default: c = kLitTex; break;
  }
  for (int guard = 0; guard < 4; ++guard) {
    int to = c;
    switch (c) {
      case kLitColorSpecularSpheremap:
        if (flags & 0x88000000u) to = kLitColorSpheremap;
        break;
      case kLitColorSpheremap:
        if (flags & 0x08000000u) to = kLitColor;
        break;
      case kLitColorSpecular:
        if (flags & 0x88000000u) to = kLitColor;
        break;
      case kLitTexSpecular:
        if (flags & 0x88000000u) to = kLitTex;
        break;
      case kLitTexSpecularSpheremap:
        if (flags & 0x88000000u) to = kLitTexSpheremap;
        break;
      case kLitTexSpheremap:
        if (flags & 0x68000000u) to = kLitTex;
        break;
      case kLitMixTASpecularSpheremap:
      case kLitMixTASpecularSpheremapMasked:
        if (flags & 0x88000000u) to = kLitMixTASpheremap;
        break;
      case kLitMixTASpheremap:
      case kLitMixTASpecular:
        if (flags & 0x88000000u) to = kLitMixTA;
        break;
      case kLitMixTA:
        if (flags & 0x08000000u) to = kLitTex;
        break;
      case kLitColorMixTexSpecular:
      case kLitColorMixTexSpheremap:
      case kLitColorMixTexSpecularSpheremap:
        if (flags & 0x88000000u) to = kLitColorMixTex;
        break;
      case kLitColorMixTex:
        if (flags & 0x08000000u) to = kLitColor;
        break;
      default:
        break;
    }
    if (to == c) break;
    c = to;
  }
  return c;
}

#ifdef _WIN32
// (int)(x * 255.0) as the apply functions pack a float into a byte of
// D3DRS_TEXTUREFACTOR (DAT_00593e40 = 255.0, truncation).
DWORD lit_byte(float x) {
  return static_cast<DWORD>(static_cast<int>(x * 255.f));
}

// C3DLow+0x38, the global fade the apply functions multiply the alpha (and
// in the glass passes the env factor and the specular colour) by: 1.0 in a
// normal frame. Fork: the host has no fade.
constexpr float kLitFade = 1.f;

// What the device can do of what the start-value programs need. The exe
// steps a class down to a multi-pass variant when ValidateDevice refuses a
// pass (FUN_00442500); those variants are not ported.
// Fork (Q11): when a cap is missing the class is drawn without its env
// stage and one line is logged.
struct LitCaps {
  IDirect3DDevice9* dev = nullptr;
  bool multiply_add = false;
  bool temp = false;
  DWORD stages = 0;
  DWORD textures = 0;
};
LitCaps g_lit_caps;

const LitCaps& lit_caps() {
  if (g_lit_caps.dev == g_dev) return g_lit_caps;
  g_lit_caps = LitCaps{};
  g_lit_caps.dev = g_dev;
  D3DCAPS9 caps{};
  if (g_dev && SUCCEEDED(g_dev->GetDeviceCaps(&caps))) {
    g_lit_caps.multiply_add =
        (caps.TextureOpCaps & D3DTEXOPCAPS_MULTIPLYADD) != 0;
    g_lit_caps.temp = (caps.PrimitiveMiscCaps & D3DPMISCCAPS_TSSARGTEMP) != 0;
    g_lit_caps.stages = caps.MaxTextureBlendStages;
    g_lit_caps.textures = caps.MaxSimultaneousTextures;
  }
  return g_lit_caps;
}

// PE 940 FUN_0043d990(stage, entry). An entry without a texture: the global
// envmap (C3DLow+0x39C) for an env entry (id 0x6000003) when one is set,
// else the default texture (C3DLow+0x340). An entry with a texture: CLAMP
// on the axes whose address bit is clear, then the texture when it is
// ready, else the default texture.
// Host: the sampler of the stage first gets the frame defaults of
// FUN_00426d10 (LINEAR min / mag / mip, WRAP), which the exe sets for all
// eight stages once a frame. Returns 1 the entry's texture, 2 the global
// envmap, 3 the default texture.
int lit_bind(DWORD stage, void* key, uint8_t addr, bool env_entry) {
  g_dev->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
  g_dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
  IDirect3DBaseTexture9* tex = nullptr;
  int src = 3;
  if (!key) {
    if (env_entry && g_envmap_tex) {
      auto tit = g_textures.find(g_envmap_tex);
      if (tit != g_textures.end()) tex = texture_d3d_base(tit->second);
      if (tex) src = 2;
    }
  } else {
    if ((addr & 1u) == 0)
      g_dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    if ((addr & 2u) == 0)
      g_dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    auto tit = g_textures.find(key);
    if (tit != g_textures.end()) tex = texture_d3d_base(tit->second);
    if (tex) src = 1;
  }
  if (!tex) {
    tex = default_texture();
    ++g_cam_pass_stats.default_tex_binds;
  }
  g_dev->SetTexture(stage, tex);
  return src;
}

// PE 940 FUN_0043d930(stage, entry): WRAP back on the axes the bind
// clamped, and no texture on the stage.
void lit_unbind(DWORD stage) {
  g_dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
  g_dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
  g_dev->SetTexture(stage, nullptr);
}

// PE 940 FUN_00441ad0(stage), the "spheremap" of every class with an env
// entry (asm 0x441ad0..0x441be0): with m = the D3D view matrix (C3DLow
// vt+0x94 = C3DLow+0x23C),
//   x = m._13, z = m._33, both divided by sqrt(x x + z z) when that is not 0
//   a = atan2(x, z)                       (FLD x, FLD z, FPATAN)
//   T = 0 but _11 = 0.5, _22 = -0.5, _33 = 0.5, _41 = -a / pi, _42 = 0.5,
//       _44 = 0.5
//   SetTransform(D3DTS_TEXTURE0 + stage, T)
//   TEXTURETRANSFORMFLAGS = COUNT2
//   TEXCOORDINDEX = D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR
// so u = 0.5 Rx - a / pi and v = 0.5 - 0.5 Ry for the reflection vector R
// in camera space: the picture slides sideways as the camera turns. The uv
// channel stored in the env entry is not read.
void lit_texgen(DWORD stage) {
  D3DMATRIX view{};
  g_dev->GetTransform(D3DTS_VIEW, &view);
  float x = view.m[0][2], z = view.m[2][2];
  const float len = std::sqrt(z * z + x * x);
  if (len != 0.f) {
    const float inv = 1.f / len;
    z = inv * z;
    x = inv * x;
  }
  const float a = std::atan2(x, z);
  D3DMATRIX t{};
  t.m[0][0] = 0.5f;
  t.m[1][1] = -0.5f;
  t.m[2][2] = 0.5f;
  t.m[3][0] = -a / 3.1415927f;  // DAT_00593a68
  t.m[3][1] = 0.5f;
  t.m[3][3] = 0.5f;
  g_dev->SetTransform(
      static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + stage), &t);
  g_dev->SetTextureStageState(stage, D3DTSS_TEXTURETRANSFORMFLAGS,
                              D3DTTFF_COUNT2);
  g_dev->SetTextureStageState(stage, D3DTSS_TEXCOORDINDEX,
                              D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
}

// PE 940 FUN_00440970(stage): TEXTURETRANSFORMFLAGS = DISABLE,
// TEXCOORDINDEX = stage.
// Fork: stage 1 goes back to set 0, where the other draws of the host
// leave it (apply_envmap_stage1, Mode3Scope::end).
void lit_texgen_off(DWORD stage) {
  g_dev->SetTextureStageState(stage, D3DTSS_TEXTURETRANSFORMFLAGS,
                              D3DTTFF_DISABLE);
  g_dev->SetTextureStageState(stage, D3DTSS_TEXCOORDINDEX,
                              stage == 1 ? 0 : stage);
}

// The D3DMATERIAL9 the exe keeps at 0x5EED28 and sends with every
// SetMaterial helper. One helper may leave a member the last one wrote
// (FUN_00440e40 does not write Ambient).
D3DMATERIAL9 g_lit_d3d_material{};

// PE 940 FUN_00440b50(E, r, g, b, a) / FUN_00440b30(E, D): Diffuse = Ambient
// = (r, g, b, a), Specular = 0 (the four zero floats at 0x5EED6C), Emissive
// = (E.rgb, 0), Power = 0.
void lit_material(const float* e, const float* d) {
  D3DMATERIAL9& m = g_lit_d3d_material;
  m.Diffuse.r = m.Ambient.r = d[0];
  m.Diffuse.g = m.Ambient.g = d[1];
  m.Diffuse.b = m.Ambient.b = d[2];
  m.Diffuse.a = m.Ambient.a = d[3];
  m.Specular.r = m.Specular.g = m.Specular.b = m.Specular.a = 0.f;
  m.Emissive.r = e[0];
  m.Emissive.g = e[1];
  m.Emissive.b = e[2];
  m.Emissive.a = 0.f;
  m.Power = 0.f;
  g_dev->SetMaterial(&m);
}

// PE 940 FUN_00440d50(E, r, g, b, a, S, power) / FUN_00440ca0(E, D, S,
// power): Diffuse = Ambient = (r, g, b, a), Specular = (S.rgb, 0), Emissive
// = (E.rgb, 0), Power = power.
void lit_material_specular(const float* e, const float* d, const float* s,
                           float power) {
  D3DMATERIAL9& m = g_lit_d3d_material;
  m.Diffuse.r = m.Ambient.r = d[0];
  m.Diffuse.g = m.Ambient.g = d[1];
  m.Diffuse.b = m.Ambient.b = d[2];
  m.Diffuse.a = m.Ambient.a = d[3];
  m.Specular.r = s[0];
  m.Specular.g = s[1];
  m.Specular.b = s[2];
  m.Specular.a = 0.f;
  m.Emissive.r = e[0];
  m.Emissive.g = e[1];
  m.Emissive.b = e[2];
  m.Emissive.a = 0.f;
  m.Power = power;
  g_dev->SetMaterial(&m);
}

// PE 940 FUN_00440e40(r, g, b, power), the additive pass: Diffuse = 0,
// Emissive = 0, Specular = (r, g, b, 0), Power = power; Ambient stays what
// the pass before it sent.
void lit_material_additive(float r, float g, float b, float power) {
  D3DMATERIAL9& m = g_lit_d3d_material;
  m.Diffuse.r = m.Diffuse.g = m.Diffuse.b = m.Diffuse.a = 0.f;
  m.Specular.r = r;
  m.Specular.g = g;
  m.Specular.b = b;
  m.Specular.a = 0.f;
  m.Emissive.r = m.Emissive.g = m.Emissive.b = m.Emissive.a = 0.f;
  m.Power = power;
  g_dev->SetMaterial(&m);
}

const float kLitWhite[4] = {1.f, 1.f, 1.f, 1.f};

// One section as its class program reads it.
struct LitMat {
  int cls = kLitNone;
  const MeshSubmesh* sm = nullptr;
  // Colour id 0 and id 2 as the material has them for this draw: the file
  // values, opaque white colour 0 for the Tex family (its constructors
  // write it after the file entries are merged), and the instance colour
  // in colour mode 1 / 2 (FUN_00440a60).
  float col0[4] = {1.f, 1.f, 1.f, 1.f};
  uint32_t col0_packed = 0xFFFFFFFFu;
  float col2[3] = {0.f, 0.f, 0.f};
  bool glass = false;        // ColorSpecularSpheremap, create flag 0x40000000
  bool group_blend = false;  // drawn by rg_transparent
  bool env = true;           // false: the device cannot do the env stage
  // Texcoord set of the primary and of the second entry in the vertices
  // that are drawn.
  DWORD set_primary = 0;
  DWORD set_second = 0;
  // The one texture entry of a ColorMixTex class (id 0x6000001): its
  // texture, address bits and texcoord set.
  void* mix_key = nullptr;
  uint8_t mix_addr = 3;
  DWORD mix_set = 0;
  int env_src = 0;
};

void lit_tss(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
  g_dev->SetTextureStageState(stage, type, value);
}

// A stage after stage 0 that a program enables. The exe leaves its ALPHAOP
// at the D3D default DISABLE while the colour op is enabled, which D3D
// leaves undefined (Q12).
// Fork: the alpha of the stage before it passes through.
void lit_alpha_through(DWORD stage) {
  lit_tss(stage, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  lit_tss(stage, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
}

// Stages 0, 1 and 2 of the MixTA classes (FUN_004304d0 selector 2,
// FUN_00430f20 selector 2, FUN_004329e0 / FUN_00431430 / FUN_00432040
// selector 3): the primary, the second laid over it by its own alpha, the
// lit vertex colour.
//   stage 0 COLOROP = SELECTARG1
//   stage 1 COLOROP = BLENDTEXTUREALPHA      (texcoord set 1)
//   stage 2 COLOROP = MODULATE, COLORARG1 = DIFFUSE, no texture
void lit_mixta_stages(const LitMat& m) {
  const MeshSubmesh& sm = *m.sm;
  lit_bind(0, sm.texture_key, sm.addr_primary, false);
  lit_bind(1, sm.texture_second, sm.addr_second, false);
  lit_tss(0, D3DTSS_TEXCOORDINDEX, m.set_primary);
  lit_tss(1, D3DTSS_TEXCOORDINDEX, m.set_second);
  lit_tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  lit_tss(1, D3DTSS_COLOROP, D3DTOP_BLENDTEXTUREALPHA);
  lit_tss(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);  // the D3D defaults
  lit_tss(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
  lit_tss(2, D3DTSS_COLOROP, D3DTOP_MODULATE);
  lit_tss(2, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  lit_tss(2, D3DTSS_COLORARG2, D3DTA_CURRENT);  // the D3D default
  lit_alpha_through(1);
  lit_alpha_through(2);
}

// Stages 0 and 1 of the ColorMixTex classes (FUN_0042e660, FUN_0042e880,
// FUN_0042ee80 / FUN_0042ea50 selector 1): colour 0 shows where the texture
// is transparent.
//   stage 0 COLOROP = BLENDTEXTUREALPHA, COLORARG2 = TFACTOR (rgb = colour 0)
//   stage 1 COLOROP = MODULATE, COLORARG1 = DIFFUSE, no texture
void lit_colormix_stages(const LitMat& m) {
  lit_bind(0, m.mix_key, m.mix_addr, false);
  lit_tss(0, D3DTSS_TEXCOORDINDEX, m.mix_set);
  lit_tss(0, D3DTSS_COLOROP, D3DTOP_BLENDTEXTUREALPHA);
  lit_tss(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
  lit_tss(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
  lit_tss(1, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  lit_tss(1, D3DTSS_COLORARG2, D3DTA_CURRENT);  // the D3D default
  lit_alpha_through(1);
}

// The env stage of the one-pass programs: the env entry on `stage`, the
// reflection-vector texgen, and `current + env x TFACTOR`
// (D3DTOP_MULTIPLYADD = COLORARG0 + COLORARG1 x COLORARG2 with the D3D
// defaults COLORARG0 = CURRENT, COLORARG1 = TEXTURE). `arg2` is TFACTOR, or
// TFACTOR | ALPHAREPLICATE for the ColorMixTex classes, whose factor is in
// the alpha byte.
void lit_env_stage(LitMat& m, DWORD stage, DWORD arg2) {
  if (!m.env) return;
  const MeshSubmesh& sm = *m.sm;
  m.env_src = lit_bind(stage, sm.texture_env, sm.addr_env, true);
  lit_texgen(stage);
  lit_tss(stage, D3DTSS_COLOROP, D3DTOP_MULTIPLYADD);
  lit_tss(stage, D3DTSS_COLORARG0, D3DTA_CURRENT);
  lit_tss(stage, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  lit_tss(stage, D3DTSS_COLORARG2, arg2);
  lit_alpha_through(stage);
}

// The highest stage a class program touches.
DWORD lit_top_stage(int cls) {
  switch (cls) {
    case kLitColorSpheremap:
    case kLitColorSpecularSpheremap:
    case kLitTexSpheremap:
    case kLitTexSpecularSpheremap:
    case kLitColorMixTex:
    case kLitColorMixTexSpecular:
      return 1;
    case kLitMixTA:
    case kLitMixTASpecular:
    case kLitColorMixTexSpheremap:
    case kLitColorMixTexSpecularSpheremap:
      return 2;
    case kLitMixTASpheremap:
    case kLitMixTASpecularSpheremap:
      return 3;
    case kLitMixTASpecularSpheremapMasked:
      return 4;
    default:
      return 0;
  }
}

// PE 940 apply(pass) of a class in object mode 0 (C3DLow vt+0xE4), at the
// start value of the class selector (DAT_005da0d0, 0fc, 0b8, 064 = 1;
// 09c, 06c = 2; 0f8, 068, 084 = 3; 04c, 0b4 = 1: file bytes).
// State that comes from the frame defaults (FUN_00426d10) and that the
// caller has set: stage 0 COLOROP = ALPHAOP = MODULATE, COLORARG1 =
// ALPHAARG1 = TEXTURE, COLORARG2 = CURRENT, ALPHAARG2 = TFACTOR; no texture
// on any stage; SPECULARENABLE 0.
// D3DRS_TEXTUREFACTOR: the alpha of colour 0 in the top byte and, for a
// class with an env stage, the env factor as a grey in the other three:
//   ((int)(A x fade x 255) << 24) | grey((int)(F x 255) & 0xff)
// After the last stage D3D adds the specular colour when SPECULARENABLE is
// 1.
void lit_apply(LitMat& m, int pass) {
  const MeshSubmesh& sm = *m.sm;
  const DWORD a = lit_byte(m.col0[3] * kLitFade);
  const DWORD f = lit_byte(sm.env_factor) & 0xffu;
  const DWORD tf_alpha = a << 24;
  const DWORD tf_env = (((((a << 8) | f) << 8) | f) << 8) | f;
  switch (m.cls) {
    case kLitColor:
      // FUN_0042e4e0: lit(colour 0).
      lit_material(m.col2, m.col0);
      lit_tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
      lit_tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
      g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, tf_alpha);
      break;
    case kLitColorSpecular:
      // FUN_0042f240: lit(colour 0) + specular.
      g_dev->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
      lit_material_specular(m.col2, m.col0, sm.col1, sm.power);
      lit_tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
      lit_tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
      g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, tf_alpha);
      break;
    case kLitColorSpheremap:
      // FUN_0042f780, selector 1: lit(colour 0) + env x F.
      lit_material(m.col2, m.col0);
      lit_tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
      lit_tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
      g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, tf_env);
      lit_env_stage(m, 1, D3DTA_TFACTOR);
      break;
    case kLitColorSpecularSpheremap:
      // FUN_0042f330.
      if (!m.glass) {
        // Selector 1: lit(colour 0) + env x F + specular.
        g_dev->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
        g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, tf_env);
        lit_material_specular(m.col2, m.col0, sm.col1, sm.power);
        lit_tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        lit_tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        lit_env_stage(m, 1, D3DTA_TFACTOR);
      } else if (pass == 0) {
        // Create flag 0x40000000 (begin FUN_004290d0 returns 2), pass 0:
        // the lit tint, blended by the group with the alpha of colour 0.
        // Stage 0 keeps COLOROP = MODULATE without a texture: the lit
        // colour.
        lit_material(m.col2, m.col0);
        const DWORD ff = lit_byte(sm.env_factor * kLitFade) & 0xffu;
        g_dev->SetRenderState(D3DRS_TEXTUREFACTOR,
                              (((((a << 8) | ff) << 8) | ff) << 8) | ff);
        lit_tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
      } else {
        // Pass 1: env x F + specular, added. FUN_004409a0(0): blend ONE /
        // ONE, BLENDOP ADD, and the alpha test of the group off.
        // Fork: its FOGCOLOR = 0 is not sent; the camera pass of the host
        // has fog off (M U6).
        g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        if (m.group_blend) g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
        lit_material_additive(sm.col1[0] * kLitFade, sm.col1[1] * kLitFade,
                              sm.col1[2] * kLitFade, sm.power);
        m.env_src = lit_bind(0, sm.texture_env, sm.addr_env, true);
        lit_tss(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        lit_tss(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        lit_texgen(0);
      }
      break;
    case kLitTex:
      // FUN_00434170: primary x lit(colour 0).
      lit_material(m.col2, m.col0);
      lit_bind(0, sm.texture_key, sm.addr_primary, false);
      g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, tf_alpha);
      break;
    case kLitTexSpecular:
      // FUN_00434bd0: primary x lit(colour 0) + specular.
      g_dev->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
      lit_material_specular(m.col2, m.col0, sm.col1, sm.power);
      lit_bind(0, sm.texture_key, sm.addr_primary, false);
      g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, tf_alpha);
      break;
    case kLitTexSpheremap:
      // FUN_00434ff0, selector 1: primary x lit(colour 0) + env x F.
      lit_material(m.col2, m.col0);
      lit_bind(0, sm.texture_key, sm.addr_primary, false);
      g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, tf_env);
      lit_env_stage(m, 1, D3DTA_TFACTOR);
      break;
    case kLitTexSpecularSpheremap:
      // FUN_00434cd0, selector 1: primary x lit(colour 0) + env x F +
      // specular.
      g_dev->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
      lit_material_specular(m.col2, m.col0, sm.col1, sm.power);
      lit_bind(0, sm.texture_key, sm.addr_primary, false);
      g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, tf_env);
      lit_env_stage(m, 1, D3DTA_TFACTOR);
      break;
    case kLitMixTA:
      // FUN_004304d0, selector 2: (primary lerp second by second.a) x lit,
      // white Diffuse and Ambient.
      lit_material(m.col2, kLitWhite);
      lit_mixta_stages(m);
      g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, tf_alpha);
      break;
    case kLitMixTASpecular:
      // FUN_00430f20, selector 2: the same + specular.
      g_dev->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
      lit_material_specular(m.col2, kLitWhite, sm.col1, sm.power);
      lit_mixta_stages(m);
      g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, tf_alpha);
      break;
    case kLitMixTASpheremap:
      // FUN_004329e0, selector 3: mix x lit + env x F, the env on stage 3.
      lit_material(m.col2, kLitWhite);
      g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, tf_env);
      lit_mixta_stages(m);
      lit_env_stage(m, 3, D3DTA_TFACTOR);
      break;
    case kLitMixTASpecularSpheremap:
      // FUN_00431430, selector 3 (DAT_005eed1c has no writer: 0): mix x
      // lit + env x F + specular.
      g_dev->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
      lit_material_specular(m.col2, kLitWhite, sm.col1, sm.power);
      g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, tf_env);
      lit_mixta_stages(m);
      lit_env_stage(m, 3, D3DTA_TFACTOR);
      break;
    case kLitMixTASpecularSpheremapMasked:
      // FUN_00432040, selector 3: mix x lit + (1 - second.a) x env x F +
      // specular: no reflection where the second texture is opaque.
      //   stage 3 = env: COLOROP = MODULATE, COLORARG2 = TFACTOR,
      //             RESULTARG = TEMP
      //   stage 4 = the second texture again, TEXCOORDINDEX = 1: COLOROP =
      //             MULTIPLYADD, COLORARG1 = TEXTURE | COMPLEMENT |
      //             ALPHAREPLICATE (0x32), COLORARG2 = TEMP
      g_dev->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
      lit_material_specular(m.col2, kLitWhite, sm.col1, sm.power);
      g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, tf_env);
      lit_mixta_stages(m);
      if (m.env) {
        m.env_src = lit_bind(3, sm.texture_env, sm.addr_env, true);
        lit_bind(4, sm.texture_second, sm.addr_second, false);
        lit_tss(4, D3DTSS_TEXCOORDINDEX, m.set_second);
        lit_texgen(3);
        lit_tss(3, D3DTSS_COLOROP, D3DTOP_MODULATE);
        lit_tss(3, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        lit_tss(3, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        lit_tss(3, D3DTSS_RESULTARG, D3DTA_TEMP);
        lit_tss(4, D3DTSS_COLOROP, D3DTOP_MULTIPLYADD);
        lit_tss(4, D3DTSS_COLORARG0, D3DTA_CURRENT);
        lit_tss(4, D3DTSS_COLORARG1,
                D3DTA_TEXTURE | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE);
        lit_tss(4, D3DTSS_COLORARG2, D3DTA_TEMP);
        lit_alpha_through(3);
        lit_alpha_through(4);
      }
      break;
    case kLitColorMixTex:
      // FUN_0042e660: (texture lerp colour 0 by texture.a) x lit.
      lit_material(m.col2, kLitWhite);
      lit_colormix_stages(m);
      g_dev->SetRenderState(D3DRS_TEXTUREFACTOR,
                            tf_alpha | (m.col0_packed & 0x00ffffffu));
      break;
    case kLitColorMixTexSpecular:
      // FUN_0042e880: the same + specular.
      g_dev->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
      lit_material_specular(m.col2, kLitWhite, sm.col1, sm.power);
      lit_colormix_stages(m);
      g_dev->SetRenderState(D3DRS_TEXTUREFACTOR,
                            tf_alpha | (m.col0_packed & 0x00ffffffu));
      break;
    case kLitColorMixTexSpheremap:
    case kLitColorMixTexSpecularSpheremap:
      // FUN_0042ee80 / FUN_0042ea50, selector 1: ... + env x F (+
      // specular). The alpha byte of TEXTUREFACTOR carries the env factor
      // ((int)(F x 255) << 24, no fade, not the alpha of colour 0) and the
      // env stage reads it with COLORARG2 = TFACTOR | ALPHAREPLICATE
      // (0x23): the alpha such a section puts out is texture.a x F.
      if (m.cls == kLitColorMixTexSpecularSpheremap) {
        lit_material_specular(m.col2, kLitWhite, sm.col1, sm.power);
        g_dev->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
      } else {
        lit_material(m.col2, kLitWhite);
      }
      lit_colormix_stages(m);
      g_dev->SetRenderState(D3DRS_TEXTUREFACTOR,
                            (lit_byte(sm.env_factor) << 24) |
                                (m.col0_packed & 0x00ffffffu));
      lit_env_stage(m, 2, D3DTA_TFACTOR | D3DTA_ALPHAREPLICATE);
      break;
    default:
      break;
  }
}

// PE 940 apply(-1) (C3DLow vt+0xE8): every class puts back what its pass
// changed. Host: the same for every class, over the stages the class can
// touch: SPECULARENABLE 0, the stages unbound (FUN_0043d930), stage 0 on
// MODULATE with the default arguments, the later stages DISABLE with the
// default arguments and RESULTARG, the texgen off (FUN_00440970); for the
// glass the blend of the group back (FUN_00440620(0)).
void lit_restore(const LitMat& m) {
  if (m.glass) {
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, m.group_blend ? TRUE : FALSE);
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    if (m.group_blend) g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
  }
  g_dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
  const DWORD top = lit_top_stage(m.cls);
  for (DWORD s = 0; s <= top; ++s) {
    lit_unbind(s);
    lit_texgen_off(s);
    lit_tss(s, D3DTSS_COLOROP, s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE);
    lit_tss(s, D3DTSS_COLORARG0, D3DTA_CURRENT);
    lit_tss(s, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    lit_tss(s, D3DTSS_COLORARG2, D3DTA_CURRENT);
    lit_tss(s, D3DTSS_ALPHAOP, s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE);
    lit_tss(s, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    if (s == 3) lit_tss(s, D3DTSS_RESULTARG, D3DTA_CURRENT);
  }
  lit_tss(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
}

bool lit_class_tex_family(int cls) {
  return cls == kLitTex || cls == kLitTexSpecular || cls == kLitTexSpheremap ||
         cls == kLitTexSpecularSpheremap;
}
bool lit_class_mixta(int cls) {
  return cls == kLitMixTA || cls == kLitMixTASpecular ||
         cls == kLitMixTASpheremap || cls == kLitMixTASpecularSpheremap ||
         cls == kLitMixTASpecularSpheremapMasked;
}
bool lit_class_colormix(int cls) {
  return cls == kLitColorMixTex || cls == kLitColorMixTexSpecular ||
         cls == kLitColorMixTexSpheremap ||
         cls == kLitColorMixTexSpecularSpheremap;
}
bool lit_class_env(int cls) {
  return cls == kLitColorSpheremap || cls == kLitColorSpecularSpheremap ||
         cls == kLitTexSpheremap || cls == kLitTexSpecularSpheremap ||
         cls == kLitMixTASpheremap || cls == kLitMixTASpecularSpheremap ||
         cls == kLitMixTASpecularSpheremapMasked ||
         cls == kLitColorMixTexSpheremap ||
         cls == kLitColorMixTexSpecularSpheremap;
}
#endif  // _WIN32

}  // namespace

#ifdef _WIN32
// One section of a lit group through the program of its material class.
// PE 940: C3DLow vt+0xE0(material, 0) = FUN_0043f3a0 (begin: the pass
// count, and for a material with an alpha reference ALPHAREF /
// ALPHATESTENABLE), per pass vt+0xE4 (apply) and the draw of the entry,
// then vt+0xE8 = FUN_0043f460 (apply(-1), alpha test off, the cull mode
// back). rg_object draws a material's entries pass by pass (FUN_00467c70),
// rg_transparent entry by entry, back to front, every pass of an entry
// before the next entry (FUN_00468490).
// The caller (draw_section of draw_camera_instances) has set the world
// matrix. `transparent`: the section is in rg_transparent (begin
// FUN_0045e8d0 = lighting on + FUN_00422c60(2, 1)).
// Returns false when the section has to take the generic path of
// draw_section: no parsed material, a class without a program, or
// Fork: a section that was not bound by slot. That is a mesh a script
// swapped in with RenderRef.changeResource (the Rectangle meshes of the
// Osd, class Tex): its texture is not an entry of a render object, and the
// generic path sends for it what CMaterial_Tex sends, without culling.
// Instance colour (FUN_00467c70 / FUN_00468490, RI+0x128): mode 1 writes
// the instance ARGB over colour id 0 for the draw, mode 2 over colour id 2
// (FUN_00440a60: each byte / 255, the packed dword from (int)(x x 255)).
static bool draw_lit_section(void* key, const MeshState& ms,
                             const MeshSubmesh& sm, const D3DMATRIX& world,
                             bool transparent, bool no_zwrite) {
  if (!sm.has_mat || !sm.slot_bound) return false;
  LitMat m;
  m.cls = lit_class_of(sm.mat_shader, sm.mat_flags);
  if (m.cls == kLitNone) {
    ++g_lit_no_program;
    return false;
  }
  m.sm = &sm;
  if (!lit_class_tex_family(m.cls)) {
    std::memcpy(m.col0, sm.col0, sizeof(m.col0));
    m.col0_packed = sm.col0_packed;
  }
  std::memcpy(m.col2, sm.col2, sizeof(m.col2));
  if ((sm.mat_flags & 0x04000000u) == 0) {
    int colour_mode = 0;
    auto fit = g_mesh_type_flags.find(key);
    if (fit != g_mesh_type_flags.end() && (fit->second & 0x80u) != 0) {
      if (fit->second & 0x200u) colour_mode = 1;
      if (fit->second & 0x400u) colour_mode = 2;
    }
    if (colour_mode != 0) {
      auto xit = g_mesh_xforms.find(key);
      // Fork: an instance whose colour was never set keeps the material
      // colour (draw_camera_instances).
      if (xit != g_mesh_xforms.end() && xit->second.color_set) {
        const uint32_t c = static_cast<uint32_t>(xit->second.color);
        constexpr float k = 0.003921569f;  // DAT_00593e24
        const float r = static_cast<float>((c >> 16) & 0xff) * k;
        const float g = static_cast<float>((c >> 8) & 0xff) * k;
        const float b = static_cast<float>(c & 0xff) * k;
        const float a = static_cast<float>(c >> 24) * k;
        if (colour_mode == 1) {
          m.col0[0] = r;
          m.col0[1] = g;
          m.col0[2] = b;
          m.col0[3] = a;
          m.col0_packed = (((((lit_byte(a) << 8) | lit_byte(r)) << 8) |
                            lit_byte(g))
                           << 8) |
                          lit_byte(b);
        } else {
          m.col2[0] = r;
          m.col2[1] = g;
          m.col2[2] = b;
        }
      }
    }
  }
  m.glass = m.cls == kLitColorSpecularSpheremap &&
            (sm.mat_flags & 0x40000000u) != 0;
  m.group_blend = transparent;
  // FUN_004295e0: CMaterial_TexSpecular has no pass while its primary
  // entry has no texture; such a section is not drawn. The entry has no
  // texture for a slot past the list, for an empty line, and for a line
  // whose texture failed to load: the template's prepare FUN_0046e1a0
  // writes a texture into the array FUN_00441c40 binds from (RT+0xF0)
  // only for a line whose data object is loaded (state word +0x38 not
  // negative, bit 8 set); a line without a data object, or with one that
  // failed, counts as done and leaves 0 there. So the null key of
  // render_d3d9_mesh_bind_slots is the exe's test in all three cases.
  // (A loaded texture that is not ready on the device is another thing:
  // FUN_0043d990 binds the default texture for it.)
  if (m.cls == kLitTexSpecular && !sm.texture_key) {
    ++g_lit_not_drawn;
    return true;
  }
  const int passes = m.glass ? 2 : 1;  // FUN_004290d0

  // Caps of the env stage (5 blend stages and 4 textures at most, for the
  // Masked class).
  if (lit_class_env(m.cls) && !m.glass) {
    const LitCaps& caps = lit_caps();
    const DWORD top = lit_top_stage(m.cls);
    const DWORD textures =
        m.cls == kLitMixTASpecularSpheremapMasked
            ? 4u
            : (lit_class_mixta(m.cls) ? 3u : (m.cls == kLitColorSpheremap ||
                                              m.cls == kLitColorSpecularSpheremap
                                                  ? 1u
                                                  : 2u));
    m.env = caps.multiply_add && caps.stages > top &&
            caps.textures >= textures &&
            (m.cls != kLitMixTASpecularSpheremapMasked || caps.temp);
    if (!m.env) {
      ++g_lit_cap_missing;
      if (!g_lit_cap_logged) {
        g_lit_cap_logged = true;
        std::fprintf(stderr,
                     "[render] lit class programs: the device lacks a cap of "
                     "the env stage (MULTIPLYADD=%d TEMP=%d blend stages=%lu "
                     "textures=%lu); such classes are drawn without it\n",
                     caps.multiply_add ? 1 : 0, caps.temp ? 1 : 0,
                     static_cast<unsigned long>(caps.stages),
                     static_cast<unsigned long>(caps.textures));
      }
    }
  }

  // Texcoord sets. The exe's stream of a class has one set per texture
  // entry the class lists (material vt+8: FUN_0042c840 / 4a0 / 4d0 / 930
  // one, the MixTA classes FUN_0042c560 / 6a0 / 710 primary and second; an
  // env entry is never listed), set i = uv[channel of entry i]. Host: the
  // one set of MeshVertex is the primary entry's; the wide vertices carry
  // the second entry's as their set 1 when it differs.
  const bool has_second = sm.slot_second != -1;
  const bool wide = has_second && !sm.wide.empty() &&
                    sm.wide.size() == sm.verts.size() &&
                    (lit_class_mixta(m.cls) ||
                     (lit_class_colormix(m.cls) && sm.slot_primary == -1));
  if (wide) {
    m.set_primary = mode3_texcoord_index(sm.chan_primary);
    m.set_second = mode3_texcoord_index(sm.chan_second);
  }
  if (lit_class_colormix(m.cls)) {
    // The class has one texture entry, id 0x6000001. A v4 chunk stores it
    // as such (the host's second entry); the v1..v3 builder FUN_0042ba10
    // makes it from the header's first map (+0x2C, the host's primary
    // entry), and FUN_0043f2e0 takes the first entry with the id.
    if (sm.slot_primary != -1) {
      m.mix_key = sm.texture_key;
      m.mix_addr = sm.addr_primary;
      m.mix_set = 0;
    } else {
      m.mix_key = sm.texture_second;
      m.mix_addr = sm.addr_second;
      m.mix_set = wide ? m.set_second : 0;
    }
  }

  // The group.
  g_dev->SetRenderState(D3DRS_LIGHTING, TRUE);
  if (transparent) {
    // FUN_00422c60(2, 1)
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    g_dev->SetRenderState(D3DRS_ALPHAREF, 4);
  } else {
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, no_zwrite ? FALSE : TRUE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  }
  // FUN_0043f3a0: a material with an alpha reference (create flag
  // 0x20000000, FUN_0043f2e0: 0x38 or its 0x5000000 entry).
  if (sm.mat_flags & 0x20000000u) {
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    g_dev->SetRenderState(D3DRS_ALPHAREF, sm.mat_alpha_ref & 0xffu);
  }
  // Cull mode. The frame defaults FUN_00426d10 send CULLMODE = C3DLow[6],
  // which the constructor FUN_004223a0 sets to 2 = D3DCULL_CW, for every
  // group; FUN_0043f3a0 switches to D3DCULL_NONE for a material with
  // create flag 0x02000000 and FUN_0043f510 puts the mode back. Under the
  // right-handed projection CW removes the triangles that look away from
  // the eye (draw_mode3_section has the same rule; every triangle of the
  // car and lift meshes has cross(v1 - v0, v2 - v0) along its vertex
  // normals). Glass is modelled with two faces of opposite winding: with
  // culling a pane is one layer, the face toward the eye. No lit section
  // of the install has the flag.
  // Fork: the draws of the host outside the class programs and material
  // mode 3 keep D3DCULL_NONE; it is put back below.
  g_dev->SetRenderState(D3DRS_CULLMODE, (sm.mat_flags & 0x02000000u)
                                            ? D3DCULL_NONE
                                            : D3DCULL_CW);

  // The frame defaults the programs start from.
  g_dev->SetTexture(0, nullptr);
  lit_tss(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  lit_tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  lit_tss(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
  lit_tss(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
  lit_tss(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  lit_tss(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
  lit_tss(0, D3DTSS_TEXCOORDINDEX, 0);

  // Fork: the wide vertices carry two colours the lit groups do not read
  // (the exe's lit stream has none: format 0x41); the four material
  // sources are MATERIAL for the draw, as the frame defaults have them.
  static const D3DRENDERSTATETYPE kSources[4] = {
      D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE,
      D3DRS_AMBIENTMATERIALSOURCE, D3DRS_EMISSIVEMATERIALSOURCE};
  DWORD saved_sources[4] = {};
  if (wide) {
    for (int i = 0; i < 4; ++i) {
      g_dev->GetRenderState(kSources[i], &saved_sources[i]);
      g_dev->SetRenderState(kSources[i], D3DMCS_MATERIAL);
    }
  }

  LitRec rec{key, 0, static_cast<int16_t>(m.cls),
             static_cast<int16_t>(g_cam_pass_stats.passes - 1),
             static_cast<uint8_t>(passes), 0, static_cast<uint8_t>(wide ? 1 : 0),
             0};
  rec.sub = static_cast<int16_t>(&sm - ms.subs.data());
  const UINT nvert = static_cast<UINT>(sm.verts.size());
  const UINT ntri = static_cast<UINT>(sm.indices.size() / 3);
  for (int pass = 0; pass < passes; ++pass) {
    lit_apply(m, pass);
    if (sm.bone_mode == 1 && !ms.bones.empty() &&
        draw_two_bone_section(key, ms, sm, world, wide)) {
      // FUN_0045e7a0: material mode 1 = mode 0 with the matrices of the
      // section's two bones (render_d3d9_part5.inc).
      rec.two_bone = 1;
    } else if (wide) {
      g_dev->SetFVF(kFvfWide);
      g_dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, nvert, ntri,
                                    sm.indices.data(), D3DFMT_INDEX16,
                                    sm.wide.data(), sizeof(MeshVertexWide));
    } else {
      g_dev->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1);
      g_dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, nvert, ntri,
                                    sm.indices.data(), D3DFMT_INDEX16,
                                    sm.verts.data(), sizeof(MeshVertex));
    }
  }
  lit_restore(m);
  g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  g_dev->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1);
  if (wide)
    for (int i = 0; i < 4; ++i)
      g_dev->SetRenderState(kSources[i], saved_sources[i]);
  rec.env_src = static_cast<uint8_t>(m.env_src);
  g_lit_recs.push_back(rec);
  return true;
}
#else
static bool draw_lit_section(void*, const MeshState&, const MeshSubmesh&,
                             const D3DMATRIX&, bool, bool) {
  return false;
}
#endif

// `[render-dbg]`: the global envmap, the sections the class programs drew
// in the last flushed frame per camera pass and class, and one line per
// distinct section (render type mesh and section index) with the values
// its program read.
void render_d3d9_lit_debug_dump(FILE* out) {
  if (!out || !render_boot940()) return;
  auto base_name = [](const std::string& s) -> std::string {
    const size_t p = s.find_last_of("\\/");
    return p == std::string::npos ? s : s.substr(p + 1);
  };
  auto tex_name = [&](void* key) -> std::string {
    if (!key) return "(default texture)";
    auto tit = g_textures.find(key);
    if (tit == g_textures.end()) return "(gone)";
    return base_name(tit->second.label);
  };
  {
    int w = 0, h = 0;
    std::string label = "-";
    auto tit = g_envmap_tex ? g_textures.find(g_envmap_tex) : g_textures.end();
    if (tit != g_textures.end()) {
      w = tit->second.w;
      h = tit->second.h;
      label = tit->second.label;
    }
    int by_src[4] = {0, 0, 0, 0};
    for (const LitRec& r : g_lit_recs) ++by_src[r.env_src & 3];
    std::fprintf(out,
                 "[render-dbg] global envmap: id=0x%08X %s '%s' %dx%d; env "
                 "stages of the last frame: entry texture=%d global envmap=%d "
                 "default texture=%d\n",
                 static_cast<unsigned>(g_envmap_id),
                 g_envmap_id == 0 ? "none set"
                                  : (g_envmap_tex ? "loaded" : "NOT LOADED"),
                 label.c_str(), w, h, by_src[1], by_src[2], by_src[3]);
  }
  // Per camera pass and class.
  int max_pass = -1;
  for (const LitRec& r : g_lit_recs)
    if (r.pass > max_pass) max_pass = r.pass;
  std::fprintf(out,
               "[render-dbg] lit sections of the last frame: %zu drawn by a "
               "class program; class without a program=%d; no pass "
               "(TexSpecular without a texture)=%d; env stage dropped "
               "(caps)=%d\n",
               g_lit_recs.size(), g_lit_no_program, g_lit_not_drawn,
               g_lit_cap_missing);
  static const int kOrder[17] = {0x00, 0x04, 0x08, 0x0C, 0x01, 0x05, 0x09,
                                 0x0D, 0x15, 0x03, 0x07, 0x0B, 0x0F, 0x30,
                                 0x31, 0x32, 0x33};
  for (int p = 0; p <= max_pass; ++p) {
    int n = 0, glass = 0, two_bone = 0, wide = 0;
    int by_cls[0x34] = {};
    for (const LitRec& r : g_lit_recs) {
      if (r.pass != p) continue;
      ++n;
      if (r.cls >= 0 && r.cls < 0x34) ++by_cls[r.cls];
      if (r.passes > 1) ++glass;
      if (r.two_bone) ++two_bone;
      if (r.wide) ++wide;
    }
    if (n == 0) continue;
    std::string s;
    for (int c : kOrder) {
      if (!by_cls[c]) continue;
      char b[64];
      std::snprintf(b, sizeof b, " %s=%d", lit_class_name(c), by_cls[c]);
      s += b;
    }
    std::fprintf(out,
                 "[render-dbg]   lit pass %d: %d section(s):%s; two-pass "
                 "glass=%d two uv sets=%d two-bone=%d\n",
                 p, n, s.c_str(), glass, wide, two_bone);
  }
  // Per distinct section.
  struct Row {
    void* src;
    int sub;
    const LitRec* rec;
    void* key;
    int count;
  };
  std::vector<Row> rows;
  for (const LitRec& r : g_lit_recs) {
    void* src = r.key;
    auto git = g_mesh_geom_src.find(r.key);
    if (git != g_mesh_geom_src.end() && git->second) src = git->second;
    bool found = false;
    for (Row& row : rows)
      if (row.src == src && row.sub == r.sub) {
        ++row.count;
        found = true;
        break;
      }
    if (!found) rows.push_back({src, r.sub, &r, r.key, 1});
  }
  for (const Row& row : rows) {
    auto mit = g_meshes.find(row.key);
    if (mit == g_meshes.end() || row.sub < 0 ||
        static_cast<size_t>(row.sub) >= mit->second.subs.size())
      continue;
    const MeshSubmesh& sm = mit->second.subs[static_cast<size_t>(row.sub)];
    const LitRec& r = *row.rec;
    const char* env = r.env_src == 1 ? "entry"
                      : r.env_src == 2 ? "global"
                      : r.env_src == 3 ? "default" : "-";
    std::fprintf(out,
                 "[render-dbg]   lit '%s' section %d '%s' class 0x%02X %s "
                 "(file 0x%02X flags 0x%08X) x%d passes=%d colour0=(%.3f, "
                 "%.3f, %.3f, %.3f) spec=(%.3f, %.3f, %.3f) power=%.2f F=%.3f "
                 "emissive=%06X primary=%s second=%s env=%s uv_sets=%d%s\n",
                 base_name(mit->second.label).c_str(), row.sub,
                 sm.name.c_str(), static_cast<unsigned>(r.cls),
                 lit_class_name(r.cls), static_cast<unsigned>(sm.mat_shader),
                 static_cast<unsigned>(sm.mat_flags), row.count, r.passes,
                 sm.col0[0], sm.col0[1], sm.col0[2], sm.col0[3], sm.col1[0],
                 sm.col1[1], sm.col1[2], sm.power, sm.env_factor,
                 static_cast<unsigned>(sm.mat_emissive & 0xFFFFFFu),
                 sm.slot_primary == -1 ? "-" : tex_name(sm.texture_key).c_str(),
                 sm.slot_second == -1 ? "-" : tex_name(sm.texture_second).c_str(),
                 env, r.wide ? 2 : 1, r.two_bone ? " two-bone" : "");
  }
}

}  // namespace inv
