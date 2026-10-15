// Fragment 3/3 of render_d3d9.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


bool render_d3d9_texture_apply_luma_alpha(void* key) {
#ifdef _WIN32
  if (!g_dev || !key) return false;
  auto it = g_textures.find(key);
  if (it == g_textures.end() || !it->second.tex) return false;
  TextureState& st = it->second;
  if (st.luma_alpha) return true;

  D3DSURFACE_DESC desc{};
  if (FAILED(st.tex->GetLevelDesc(0, &desc)) || desc.Width == 0 ||
      desc.Height == 0)
    return false;

  IDirect3DTexture9* out = nullptr;
  if (FAILED(g_dev->CreateTexture(desc.Width, desc.Height, 1, 0,
                                  D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &out,
                                  nullptr)) ||
      !out)
    return false;

  D3DLOCKED_RECT src_lr{};
  D3DLOCKED_RECT dst_lr{};
  if (FAILED(st.tex->LockRect(0, &src_lr, nullptr, D3DLOCK_READONLY)) ||
      FAILED(out->LockRect(0, &dst_lr, nullptr, 0))) {
    if (src_lr.pBits) st.tex->UnlockRect(0);
    out->Release();
    return false;
  }

  auto* dst_base = static_cast<uint8_t*>(dst_lr.pBits);
  const int w = static_cast<int>(desc.Width);
  const int h = static_cast<int>(desc.Height);

  if (desc.Format == D3DFMT_DXT1) {
    const auto* src = static_cast<const uint8_t*>(src_lr.pBits);
    const int bw = (w + 3) / 4;
    const int bh = (h + 3) / 4;
    for (int by = 0; by < bh; ++by) {
      for (int bx = 0; bx < bw; ++bx) {
        uint8_t px[16][4];
        decode_dxt1_block(src + (by * bw + bx) * 8, px);
        for (int k = 0; k < 16; ++k) {
          const int x = bx * 4 + (k % 4);
          const int y = by * 4 + (k / 4);
          if (x >= w || y >= h) continue;
          const uint8_t bb = px[k][0], gg = px[k][1], rr = px[k][2];
          uint8_t* d = dst_base + y * dst_lr.Pitch + x * 4;
          d[0] = bb;
          d[1] = gg;
          d[2] = rr;
          d[3] = luma_to_osd_bg_alpha(rr, gg, bb);
        }
      }
    }
  } else {
    // Already uncompressed — rewrite alpha from luma, keep RGB.
    for (int y = 0; y < h; ++y) {
      const auto* s =
          static_cast<const uint8_t*>(src_lr.pBits) + y * src_lr.Pitch;
      auto* d = dst_base + y * dst_lr.Pitch;
      for (int x = 0; x < w; ++x) {
        uint8_t bb = 0, gg = 0, rr = 0;
        if (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8) {
          bb = s[x * 4 + 0];
          gg = s[x * 4 + 1];
          rr = s[x * 4 + 2];
        } else if (desc.Format == D3DFMT_R5G6B5) {
          const uint16_t c = static_cast<uint16_t>(s[x * 2] | (s[x * 2 + 1] << 8));
          rgb565_to_rgb(c, &rr, &gg, &bb);
        } else {
          out->UnlockRect(0);
          st.tex->UnlockRect(0);
          out->Release();
          return false;
        }
        d[x * 4 + 0] = bb;
        d[x * 4 + 1] = gg;
        d[x * 4 + 2] = rr;
        d[x * 4 + 3] = luma_to_osd_bg_alpha(rr, gg, bb);
      }
    }
  }

  out->UnlockRect(0);
  st.tex->UnlockRect(0);
  st.tex->Release();
  st.tex = out;
  st.mips = 1;
  st.luma_alpha = true;
  return true;
#else
  (void)key;
  return false;
#endif
}

int32_t render_d3d9_texture_width(void* key) {
  auto it = g_textures.find(key);
  return it == g_textures.end() ? 0 : it->second.w;
}

int32_t render_d3d9_texture_height(void* key) {
  auto it = g_textures.find(key);
  return it == g_textures.end() ? 0 : it->second.h;
}

int32_t render_d3d9_texture_mips(void* key) {
  auto it = g_textures.find(key);
  if (it == g_textures.end()) return 0;
  return it->second.mips > 0 ? it->second.mips : 1;
}

const char* render_d3d9_texture_label(void* key) {
  auto it = g_textures.find(key);
  return it == g_textures.end() ? "" : it->second.label.c_str();
}

void render_d3d9_set_global_envmap(void* key) {
  // PE @ 0x0047C220: cmp current(+0x64) vs handle @ 0x47C24A → jz no-op.
  // handle 0 @ loc_47C2B8 zeros node (+0x58..+0x64). Else unlink/relink
  // list; host has no resource+0x48 list — assign g_envmap only.
  // Present binds via apply_envmap_stage1 (ADD + TCI_CAMERASPACEREFLECTION).
  if (g_envmap == key) return;
  g_envmap = key;
}

void* render_d3d9_global_envmap() { return g_envmap; }

void render_d3d9_osd_clear() {
  g_osd.clear();
  g_osd_text.clear();
}

void render_d3d9_osd_add_rect(float x, float y, float w, float h, void* texture,
                              int32_t pri) {
  OsdRect r;
  r.x = x;
  r.y = y;
  r.w = w;
  r.h = h;
  r.texture = texture;
  r.pri = pri;
  g_osd.push_back(r);
}

void render_d3d9_osd_set_rect(void* key, float x, float y, float w, float h,
                              void* texture, int32_t pri) {
  render_d3d9_osd_set_rect_color(key, x, y, w, h, texture, 0xFFFFFFFFu, pri);
}

void render_d3d9_osd_set_rect_color(void* key, float x, float y, float w,
                                    float h, void* texture, uint32_t argb,
                                    int32_t pri) {
  if (key) {
    for (auto& r : g_osd) {
      if (r.key == key) {
        r.x = x;
        r.y = y;
        r.w = w;
        r.h = h;
        r.texture = texture;
        r.pri = pri;
        r.color = argb ? argb : 0xFFFFFFFFu;
        return;
      }
    }
  }
  OsdRect r;
  r.key = key;
  r.x = x;
  r.y = y;
  r.w = w;
  r.h = h;
  r.texture = texture;
  r.pri = pri;
  r.color = argb ? argb : 0xFFFFFFFFu;
  g_osd.push_back(r);
}

void render_d3d9_osd_set_rect_visible(void* key, int32_t visible) {
  if (!key) return;
  for (auto& r : g_osd) {
    if (r.key == key) {
      r.visible = visible != 0;
      return;
    }
  }
}

void render_d3d9_osd_remove_rect(void* key) {
  if (!key) return;
  for (size_t i = 0; i < g_osd.size();) {
    if (g_osd[i].key == key)
      g_osd.erase(g_osd.begin() + static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
}

void render_d3d9_debug_dump(FILE* out) {
  if (!out) return;
  size_t ready = 0, queued_ready = 0;
  for (const auto& kv : g_meshes) if (kv.second.ready) ++ready;
  for (void* k : g_mesh_queue) { auto it = g_meshes.find(k); if (it != g_meshes.end() && it->second.ready) ++queued_ready; }
  std::fprintf(out, "[render-dbg] meshes=%zu ready=%zu queue=%zu queued_ready=%zu textures=%zu viewports=%zu active_vp=%p cameras=%zu active_cam=%p osd_rects=%zu osd_texts=%zu\n",
               g_meshes.size(), ready, g_mesh_queue.size(), queued_ready, g_textures.size(),
               g_viewports.size(), g_active_vp, g_cameras.size(), g_active_cam, g_osd.size(), g_osd_text.size());
  // Fork: camera passes of the last flushed frame (draw_viewport_cameras).
  std::fprintf(out, "[render-dbg] bound_vps=%d cam_passes=%d cam_instances=%d cam_texts=%d leaf_hidden_instances=%d leaf_hidden_texts=%d\n",
               g_cam_pass_stats.bound_vps, g_cam_pass_stats.passes,
               g_cam_pass_stats.instances, g_cam_pass_stats.texts,
               g_cam_pass_stats.hidden_instances, g_cam_pass_stats.hidden_texts);
  {
    // Bound viewports in the exe's draw order (FUN_00421e40: ascending
    // priority, the most recently bound first among equals).
    struct Row { uint32_t pri, seq; void* key; };
    std::vector<Row> rows;
    for (const auto& kv : g_viewports)
      if (kv.second.active)
        rows.push_back({static_cast<uint32_t>(kv.second.pri), kv.second.bind_seq, kv.first});
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
      return a.pri != b.pri ? a.pri < b.pri : a.seq > b.seq;
    });
    for (const Row& r : rows) {
      const ViewportState& vp = g_viewports[r.key];
      int cams = 0;
      for (const auto& ck : g_cameras)
        if (ck.second.viewport == r.key) ++cams;
      std::fprintf(out, "[render-dbg] bound vp=%p pri=%u seq=%u rect=(%g,%g,%g,%g) cameras=%d\n",
                   r.key, r.pri, r.seq, vp.x, vp.y, vp.w, vp.h, cams);
    }
  }
  for (const std::string& line : g_cam_pass_log)  // only under SLRR_RENDER_TRACE
    std::fprintf(out, "[render-dbg] %s\n", line.c_str());
  {
    // Fork: the depth format of the device and the instances that wait for
    // (or were refused) their render type.
    int waiting = 0, not_mesh = 0, failed = 0;
    for (const auto& pk : g_mesh_pending) {
      if (pk.second.state == 0) ++waiting;
      else if (pk.second.state == 1) ++not_mesh;
      else ++failed;
    }
#ifdef _WIN32
    const char* depth_name = depth_format_name(g_depth_format);
    const unsigned create_flags = static_cast<unsigned>(g_create_flags);
#else
    const char* depth_name = "none";
    const unsigned create_flags = 0;
#endif
    std::fprintf(out, "[render-dbg] depth=%s device_flags=0x%X pending_instances=%zu waiting=%d not_mesh=%d failed=%d\n",
                 depth_name, create_flags, g_mesh_pending.size(), waiting, not_mesh, failed);
  }
  native_camera_debug_dump(out);  // one line per native camera
  native_cursor_debug_dump(out);  // `[cursor] state` (SLRR_PE_NATIVE_PROF)
  native_part_debug_dump(out);    // the native parts that are drawn
  render_d3d9_bone_debug_dump(out);
  render_d3d9_lit_debug_dump(out);  // the class programs of the lit groups
  render_d3d9_scene_light_debug_dump(out);
  render_d3d9_shadow_debug_dump(out);  // the projected shadows
  render_d3d9_ground_debug_dump(out);  // one line per ground mesh
  // Fork: the object sections of the last flushed frame that went through
  // rg_horizon and material mode 3, and every bind of the engine default
  // texture (unbound or failed texture entries, all groups).
  std::fprintf(out, "[render-dbg] object groups: horizon_sections=%d prelit_mode3_sections=%d prelit_transparent_mode3_sections=%d default_tex_binds=%d\n",
               g_cam_pass_stats.horizon_sections,
               g_cam_pass_stats.prelit_mode3_sections,
               g_cam_pass_stats.prelit_transparent_mode3_sections,
               g_cam_pass_stats.default_tex_binds);
  {
    // Fork: render type flags known to the renderer (types and instances),
    // by value; 640 = 0x280 is the only colour-mode value in frontend.rdb.
    std::vector<std::pair<uint32_t, int>> by_value;
    for (const auto& kv : g_mesh_type_flags) {
      auto it = std::find_if(by_value.begin(), by_value.end(),
                             [&](const std::pair<uint32_t, int>& e) { return e.first == kv.second; });
      if (it == by_value.end())
        by_value.emplace_back(kv.second, 1);
      else
        ++it->second;
    }
    std::sort(by_value.begin(), by_value.end());
    std::string s;
    for (const auto& kv : by_value) {
      char b[48];
      std::snprintf(b, sizeof b, " %u:%d", kv.first, kv.second);
      s += b;
    }
    std::fprintf(out, "[render-dbg] render_type_flags keys=%zu by value (flags:count):%s\n",
                 g_mesh_type_flags.size(), s.c_str());
  }
  {
    // Fork: texts by kind; fonts with their glyph source.
    int tree = 0, tree_shown = 0;
    for (const auto& tk : g_texts) {
      if (!tk.second.tree) continue;
      ++tree;
      if (tk.second.alive && tk.second.visible && !tk.second.text.empty()) ++tree_shown;
    }
    std::fprintf(out, "[render-dbg] texts=%zu tree_texts=%d tree_texts_with_string=%d fonts=%zu\n",
                 g_texts.size(), tree, tree_shown, g_fonts.size());
    if (std::getenv("SLRR_RENDER_TRACE")) {
      for (const auto& fk : g_fonts) {
        const FontState& f = fk.second;
        const FontGlyph* a = f.glyphs.size() > 'A' ? &f.glyphs['A'] : nullptr;
        std::fprintf(out, "[render-dbg] font %p '%s' glyphs=%zu atlas=%dx%d no_texture=%d 'A' adv=%.1f box=(%.1f,%.1f)-(%.1f,%.1f)\n",
                     fk.first, f.name.c_str(), f.glyphs.size(), f.atlas_w, f.atlas_h,
                     f.no_texture ? 1 : 0, a ? a->advance : 0.f, a ? a->px[0] : 0.f,
                     a ? a->py[0] : 0.f, a ? a->px[3] : 0.f, a ? a->py[3] : 0.f);
      }
    }
  }
}

int32_t render_d3d9_osd_count() {
  return static_cast<int32_t>(g_osd.size());
}

int32_t render_d3d9_osd_text_count() {
  return static_cast<int32_t>(g_osd_text.size());
}

namespace {

int32_t font_px_height_from_name(const std::string& name) {
  std::string n = name;
  for (char& c : n) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  // Mirror Text.getFontSize RID table.
  if (n.find("simple40") != std::string::npos) return 40;
  if (n.find("slii24") != std::string::npos) return 24;
  if (n.find("simple20") != std::string::npos) return 20;
  if (n.find("slii17") != std::string::npos) return 15;
  if (n.find("slii11") != std::string::npos) return 10;
  if (n.find("console10") != std::string::npos) return 10;
  if (n.find("console5") != std::string::npos) return 7;
  if (n.find("sl28") != std::string::npos) return 28;
  if (n.find("sl14") != std::string::npos) return 14;
  return 20;
}

bool load_file_bytes(const std::string& path, std::vector<uint8_t>* out) {
  if (!out) return false;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    std::string resolved = rpak_resolve_path(path.c_str());
    if (!resolved.empty()) f = std::fopen(resolved.c_str(), "rb");
  }
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 32 * 1024 * 1024) {
    std::fclose(f);
    return false;
  }
  out->resize(static_cast<size_t>(sz));
  const bool ok = std::fread(out->data(), 1, out->size(), f) == out->size();
  std::fclose(f);
  return ok;
}

// Decode PNG/JPEG/etc via WIC into a font atlas.
// SLII*.png are LA: white fill + black outline with coverage alpha — preserve
// BGRA. Greyscale coverage atlases (no dark-with-alpha outline) still use
// white RGB + luma→alpha so Text.changeColor can tint.
// straight: upload the decoded BGRA as it is. PE 940 uses the charset
// texture like any other (text flush FUN_0044cb60: colour = texture x
// diffuse, alpha = texture alpha x diffuse alpha); every font PNG of the
// install carries its coverage in the alpha channel.
bool upload_wic_font_atlas(void* key, const uint8_t* data, size_t size,
                           const char* label, bool straight = false) {
#ifdef _WIN32
  if (!g_dev || !key || !data || size < 8) return false;

  static bool com_inited = false;
  if (!com_inited) {
    const HRESULT chr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(chr) || chr == RPC_E_CHANGED_MODE) com_inited = true;
  }

  IWICImagingFactory* factory = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr) || !factory) return false;

  IWICStream* stream = nullptr;
  hr = factory->CreateStream(&stream);
  if (FAILED(hr) || !stream) {
    factory->Release();
    return false;
  }
  hr = stream->InitializeFromMemory(const_cast<BYTE*>(data),
                                    static_cast<DWORD>(size));
  if (FAILED(hr)) {
    stream->Release();
    factory->Release();
    return false;
  }

  IWICBitmapDecoder* decoder = nullptr;
  hr = factory->CreateDecoderFromStream(stream, nullptr,
                                        WICDecodeMetadataCacheOnLoad, &decoder);
  stream->Release();
  if (FAILED(hr) || !decoder) {
    factory->Release();
    return false;
  }

  IWICBitmapFrameDecode* frame = nullptr;
  hr = decoder->GetFrame(0, &frame);
  decoder->Release();
  if (FAILED(hr) || !frame) {
    factory->Release();
    return false;
  }

  IWICFormatConverter* conv = nullptr;
  hr = factory->CreateFormatConverter(&conv);
  if (FAILED(hr) || !conv) {
    frame->Release();
    factory->Release();
    return false;
  }
  hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                        nullptr, 0.0, WICBitmapPaletteTypeCustom);
  frame->Release();
  if (FAILED(hr)) {
    conv->Release();
    factory->Release();
    return false;
  }

  UINT w = 0, h = 0;
  conv->GetSize(&w, &h);
  if (w == 0 || h == 0 || w > 8192 || h > 8192) {
    conv->Release();
    factory->Release();
    return false;
  }
  const UINT stride = w * 4;
  const UINT bytes = stride * h;
  std::vector<uint8_t> pixels(bytes);
  hr = conv->CopyPixels(nullptr, stride, bytes, pixels.data());
  conv->Release();
  factory->Release();
  if (FAILED(hr)) return false;

  // Detect baked outline (black RGB + non-zero alpha), e.g. SLII LA PNGs.
  bool outline_atlas = straight;
  for (UINT i = 0; i < w * h && !outline_atlas; ++i) {
    const uint8_t* p = pixels.data() + i * 4;
    const int luma =
        (static_cast<int>(p[0]) + static_cast<int>(p[1]) + static_cast<int>(p[2])) /
        3;
    if (luma < 40 && p[3] > 16) {
      outline_atlas = true;
      break;
    }
  }
  if (!outline_atlas) {
    // Coverage mask: opaque RGB → alpha; RGB white for diffuse tint.
    for (UINT i = 0; i < w * h; ++i) {
      uint8_t* p = pixels.data() + i * 4;
      const int a = (static_cast<int>(p[0]) + p[1] + p[2]) / 3;
      p[0] = 0xff;
      p[1] = 0xff;
      p[2] = 0xff;
      p[3] = static_cast<uint8_t>(a);
    }
  }

  TextureState& st = g_textures[key];
  st.label = label ? label : "";
  if (st.tex) {
    st.tex->Release();
    st.tex = nullptr;
  }
  if (FAILED(g_dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                  &st.tex, nullptr)) ||
      !st.tex)
    return false;
  D3DLOCKED_RECT lr{};
  if (FAILED(st.tex->LockRect(0, &lr, nullptr, 0))) {
    st.tex->Release();
    st.tex = nullptr;
    return false;
  }
  if (lr.Pitch == static_cast<INT>(stride)) {
    std::memcpy(lr.pBits, pixels.data(), bytes);
  } else {
    const uint8_t* src = pixels.data();
    uint8_t* dst = static_cast<uint8_t*>(lr.pBits);
    for (UINT y = 0; y < h; ++y) {
      std::memcpy(dst, src, stride);
      src += stride;
      dst += lr.Pitch;
    }
  }
  st.tex->UnlockRect(0);
  st.w = static_cast<int32_t>(w);
  st.h = static_cast<int32_t>(h);
  st.mips = 1;
  return true;
#else
  (void)key;
  (void)data;
  (void)size;
  (void)label;
  (void)straight;
  return false;
#endif
}

bool upload_tga_font_atlas(void* key, const uint8_t* data, size_t size,
                           const char* label) {
#ifdef _WIN32
  if (!g_dev || !key || !data || size < 18) return false;
  const uint8_t id_len = data[0];
  const uint8_t cmap_type = data[1];
  const uint8_t img_type = data[2];
  const uint16_t cmap_first =
      static_cast<uint16_t>(data[3] | (data[4] << 8));
  const uint16_t cmap_len =
      static_cast<uint16_t>(data[5] | (data[6] << 8));
  const uint8_t cmap_entry_bits = data[7];
  const uint16_t w = static_cast<uint16_t>(data[12] | (data[13] << 8));
  const uint16_t h = static_cast<uint16_t>(data[14] | (data[15] << 8));
  const uint8_t bpp = data[16];
  const uint8_t desc = data[17];
  if (w == 0 || h == 0 || bpp != 8) return false;
  // Type 3 = greyscale; type 1 = indexed (SLII*_detect.tga).
  if (img_type != 3 && img_type != 1) return false;
  if (img_type == 1 && (cmap_type != 1 || cmap_len == 0 || cmap_entry_bits < 24))
    return false;

  const size_t cmap_bytes =
      img_type == 1
          ? static_cast<size_t>(cmap_len) * (cmap_entry_bits / 8)
          : 0;
  const size_t header = 18u + id_len + cmap_bytes;
  const size_t need = header + static_cast<size_t>(w) * static_cast<size_t>(h);
  if (size < need) return false;

  // Build 256 greyscale alpha samples from palette (or identity for type 3).
  uint8_t alpha_lut[256];
  for (int i = 0; i < 256; ++i) alpha_lut[i] = static_cast<uint8_t>(i);
  if (img_type == 1) {
    std::memset(alpha_lut, 0, sizeof(alpha_lut));
    const uint8_t* pal = data + 18u + id_len;
    const int epb = cmap_entry_bits / 8;
    for (uint16_t i = 0; i < cmap_len && i < 256; ++i) {
      const uint8_t* e = pal + static_cast<size_t>(i) * epb;
      // TGA palette is BGR(A).
      const uint8_t b = e[0], g = e[1], r = e[2];
      const int a = (static_cast<int>(r) + g + b) / 3;
      const int idx = static_cast<int>(cmap_first) + i;
      if (idx >= 0 && idx < 256) alpha_lut[idx] = static_cast<uint8_t>(a);
    }
  }

  TextureState& st = g_textures[key];
  st.label = label ? label : "";
  if (st.tex) {
    st.tex->Release();
    st.tex = nullptr;
  }
  if (FAILED(g_dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                  &st.tex, nullptr)) ||
      !st.tex)
    return false;

  D3DLOCKED_RECT lr{};
  if (FAILED(st.tex->LockRect(0, &lr, nullptr, 0))) {
    st.tex->Release();
    st.tex = nullptr;
    return false;
  }
  const bool top_left = (desc & 0x20) != 0;
  for (uint16_t y = 0; y < h; ++y) {
    const uint16_t src_y = top_left ? y : static_cast<uint16_t>(h - 1 - y);
    const uint8_t* src = data + header + static_cast<size_t>(src_y) * w;
    uint8_t* dst = static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch;
    for (uint16_t x = 0; x < w; ++x) {
      const uint8_t a = alpha_lut[src[x]];
      dst[x * 4 + 0] = 0xff;
      dst[x * 4 + 1] = 0xff;
      dst[x * 4 + 2] = 0xff;
      dst[x * 4 + 3] = a;
    }
  }
  st.tex->UnlockRect(0);
  st.w = w;
  st.h = h;
  st.mips = 1;
  return true;
#else
  (void)key;
  (void)data;
  (void)size;
  (void)label;
  return false;
#endif
}

// Charset mesh -> glyph table. PE 940 INVO version < 4 loader (FUN_00451b00,
// header rule FUN_004583f0): H = dword at file+8 (header size); vertex stride
// = dword at file+8+0x3c when H > 0x3c, else 44; vertex count at file+8+H,
// the vertices right after it; position at +0, multiplied by 0.1
// (DAT_005935d4) while the vertex is staged (0x4522f7-0x452319), uv at +24.
// On disk the font SCX files have H = 0x38 (stride 44, 1024 vertices) and
// fade.SCX H = 0x88 (stride 64, 1056 vertices). Glyph c = vertices c*4 ..
// c*4+3 in file order, advance = vertex[c*4+1].x (FUN_0044bdc0); nothing in
// the mesh marks a font (RT_text FUN_0046e920 only tests the generic "mesh
// data ready" bit), so any INVO v3 mesh is a charset.
// *uv_glyphs = glyphs with a real UV span (validity test of the by-name
// loader), *hdr_flags = header +0x28 (material flags, FUN_0042ba10).
bool parse_font_glyphs(const uint8_t* scx, size_t scx_size,
                       std::vector<FontGlyph>* out, int* uv_glyphs = nullptr,
                       uint32_t* hdr_flags = nullptr) {
  if (uv_glyphs) *uv_glyphs = 0;
  if (hdr_flags) *hdr_flags = 0;
  if (!scx || !out || scx_size < 0x48) return false;
  out->clear();
  if (std::memcmp(scx, "INVO", 4) != 0) return false;
  uint32_t ver = 0, hdr = 0, vcount = 0;
  std::memcpy(&ver, scx + 4, 4);
  std::memcpy(&hdr, scx + 8, 4);
  constexpr size_t kBase = 8;
  if (ver != 3 || hdr < 0x2c || hdr > 0x1000) return false;
  if (kBase + static_cast<size_t>(hdr) + 4 > scx_size) return false;
  size_t stride = 44;
  if (hdr > 0x3c) {
    uint32_t s = 0;
    std::memcpy(&s, scx + kBase + 0x3c, 4);
    stride = s;
  }
  if (stride < 32 || stride > 256) return false;
  std::memcpy(&vcount, scx + kBase + hdr, 4);
  if (vcount < 4 || vcount > 65536) return false;
  const size_t vstart = kBase + hdr + 4;
  const uint64_t need = static_cast<uint64_t>(vstart) +
                        static_cast<uint64_t>(vcount) * static_cast<uint64_t>(stride);
  if (need > scx_size) return false;
  if (hdr_flags) std::memcpy(hdr_flags, scx + kBase + 0x28, 4);

  const size_t nglyphs = static_cast<size_t>(vcount / 4);
  out->reserve(nglyphs);
  int ok = 0;
  auto finite = [](float f) { return f == f && f > -1e9f && f < 1e9f; };
  for (size_t g = 0; g < nglyphs; ++g) {
    FontGlyph glyph;
    for (int i = 0; i < 4; ++i) {
      const size_t off = vstart + (g * 4 + static_cast<size_t>(i)) * stride;
      float x = 0, y = 0, u = 0, v = 0;
      std::memcpy(&x, scx + off, 4);
      std::memcpy(&y, scx + off + 4, 4);
      std::memcpy(&u, scx + off + 24, 4);
      std::memcpy(&v, scx + off + 28, 4);
      glyph.px[i] = finite(x) ? x * 0.1f : 0.f;  // DAT_005935d4
      glyph.py[i] = finite(y) ? y * 0.1f : 0.f;
      glyph.tu[i] = finite(u) ? u : 0.f;
      glyph.tv[i] = finite(v) ? v : 0.f;
    }
    glyph.advance = glyph.px[1];
    // Legacy UV box (draw_osd_texts).
    float umin = 2.f, umax = -1.f, vmin = 2.f, vmax = -1.f;
    for (int i = 0; i < 4; ++i) {
      const float u = glyph.tu[i], v = glyph.tv[i];
      if (!(u >= 0.f && u <= 1.01f && v >= 0.f && v <= 1.01f)) {
        umin = umax = vmin = vmax = 0.f;
        break;
      }
      if (u < umin) umin = u;
      if (u > umax) umax = u;
      if (v < vmin) vmin = v;
      if (v > vmax) vmax = v;
    }
    glyph.u0 = umin;
    glyph.v0 = vmin;
    glyph.u1 = umax;
    glyph.v1 = vmax;
    out->push_back(glyph);
    if ((umax - umin) > 0.002f && (vmax - vmin) > 0.015f) ++ok;
  }
  if (uv_glyphs) *uv_glyphs = ok;
  return !out->empty();
}

// PE 940 measure, C3DLow vt+0x214 FUN_0044bfb0: sum of vertex[c*4+1].x *
// scale; ESC (0x1b) 'C' skips 10 bytes, ESC + any other byte skips 2.
// Fork: a code beyond the charset's vertex count adds nothing (the exe reads
// past the vertex buffer) and an escape never runs past the terminator.
float font_measure_run(const FontState& font, const char* text, float scale) {
  float w = 0.f;
  if (!text) return w;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    if (*p == 0x1b) {
      const int skip = p[1] == 'C' ? 10 : 2;
      for (int i = 0; i < skip && *p; ++i) ++p;
      continue;
    }
    const size_t gi = *p++;
    if (gi < font.glyphs.size()) w += font.glyphs[gi].advance * scale;
  }
  return w;
}

// "mesh 0x0000004A" / "texture 0x00000026" lines of a render type (typeof 14)
// payload; frontend.rpk writes them in either order.
bool render_type_ids(const std::vector<uint8_t>& blob, uint32_t* mesh,
                     uint32_t* tex) {
  *mesh = 0;
  *tex = 0;
  const char* p = reinterpret_cast<const char*>(blob.data());
  const char* const end = p + blob.size();
  while (p < end) {
    const char* le = p;
    while (le < end && *le != '\n' && *le != '\r') ++le;
    const std::string line(p, static_cast<size_t>(le - p));
    auto value_of = [&line](const char* kw, uint32_t* out) {
      const size_t n = std::strlen(kw);
      size_t i = 0;
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
      if (line.compare(i, n, kw) != 0) return;
      i += n;
      if (i >= line.size() || (line[i] != ' ' && line[i] != '\t' && line[i] != '=')) return;
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '=')) ++i;
      if (i < line.size())
        *out = static_cast<uint32_t>(std::strtoul(line.c_str() + i, nullptr, 0));
    };
    value_of("mesh", mesh);
    value_of("texture", tex);
    p = le;
    while (p < end && (*p == '\n' || *p == '\r')) ++p;
  }
  return *mesh != 0;
}

}  // namespace

bool render_d3d9_font_load(void* key, const char* name) {
  if (!key || !name || !name[0]) return false;
  std::string n = name;
  // Strip path / extension.
  {
    const size_t slash = n.find_last_of("/\\");
    if (slash != std::string::npos) n = n.substr(slash + 1);
    const size_t dot = n.find_last_of('.');
    if (dot != std::string::npos) n = n.substr(0, dot);
  }

  std::string up = n;
  for (char& c : up) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }

  std::string scx = "frontend/meshes/Fonts/" + n + ".scx";
  std::string charset_path = "frontend/font.dat";

  std::vector<uint8_t> scx_bytes, atlas_bytes, cs_bytes;
  if (!load_file_bytes(scx, &scx_bytes)) {
    scx = "frontend/meshes/Fonts/" + n + ".scx";
    if (!load_file_bytes(scx, &scx_bytes)) return false;
  }
  if (!load_file_bytes(charset_path, &cs_bytes) || cs_bytes.empty()) return false;

  // Prefer real atlases (TGA then PNG). Never prefer *_detect.tga — those are
  // debug maps and produce garbage glyphs (SLII* ships as SLII24.png).
  enum class AtlasKind { Tga, Wic };
  AtlasKind kind = AtlasKind::Tga;
  std::string atlas_path;
  auto try_atlas = [&](const std::string& path, AtlasKind k) -> bool {
    if (!load_file_bytes(path, &atlas_bytes) || atlas_bytes.empty()) return false;
    atlas_path = path;
    kind = k;
    return true;
  };
  if (!try_atlas("frontend/textures/Fonts/" + n + ".tga", AtlasKind::Tga) &&
      !try_atlas("frontend/textures/Fonts/" + up + ".tga", AtlasKind::Tga) &&
      !try_atlas("frontend/textures/Fonts/" + n + ".png", AtlasKind::Wic) &&
      !try_atlas("frontend/textures/Fonts/" + up + ".png", AtlasKind::Wic) &&
      !try_atlas("frontend/textures/Fonts/" + up + "_detect.tga", AtlasKind::Tga) &&
      !try_atlas("frontend/textures/Fonts/" + n + "_detect.tga", AtlasKind::Tga)) {
    return false;
  }

  void* atlas_key =
      reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(key) + 0x20000u);
  const bool uploaded =
      (kind == AtlasKind::Tga)
          ? upload_tga_font_atlas(atlas_key, atlas_bytes.data(),
                                  atlas_bytes.size(), atlas_path.c_str())
          : upload_wic_font_atlas(atlas_key, atlas_bytes.data(),
                                  atlas_bytes.size(), atlas_path.c_str());
  if (!uploaded) {
    // No device / WIC fail: keep dims so font_ready + Text metrics still work
    // (Valocity HUD smoke without --window; mirrors PTX skydome path).
    int32_t pw = 0, ph = 0;
    bool dims_ok = false;
    if (kind == AtlasKind::Tga && atlas_bytes.size() >= 18) {
      pw = static_cast<int32_t>(
          atlas_bytes[12] | (static_cast<uint16_t>(atlas_bytes[13]) << 8));
      ph = static_cast<int32_t>(
          atlas_bytes[14] | (static_cast<uint16_t>(atlas_bytes[15]) << 8));
      dims_ok = pw > 0 && ph > 0;
    } else if (atlas_bytes.size() >= 24 && atlas_bytes[0] == 0x89 &&
               atlas_bytes[1] == 'P' && atlas_bytes[2] == 'N' &&
               atlas_bytes[3] == 'G') {
      const uint32_t w = (static_cast<uint32_t>(atlas_bytes[16]) << 24) |
                         (static_cast<uint32_t>(atlas_bytes[17]) << 16) |
                         (static_cast<uint32_t>(atlas_bytes[18]) << 8) |
                         static_cast<uint32_t>(atlas_bytes[19]);
      const uint32_t h = (static_cast<uint32_t>(atlas_bytes[20]) << 24) |
                         (static_cast<uint32_t>(atlas_bytes[21]) << 16) |
                         (static_cast<uint32_t>(atlas_bytes[22]) << 8) |
                         static_cast<uint32_t>(atlas_bytes[23]);
      if (w > 0 && h > 0 && w <= 8192 && h <= 8192) {
        pw = static_cast<int32_t>(w);
        ph = static_cast<int32_t>(h);
        dims_ok = true;
      }
    }
    if (!dims_ok) {
      render_d3d9_texture_destroy(atlas_key);
      return false;
    }
    TextureState& tst = g_textures[atlas_key];
    tst.label = atlas_path;
    tst.w = pw;
    tst.h = ph;
    tst.mips = 1;
  }
  std::printf("[font] %s atlas=%s\n", n.c_str(), atlas_path.c_str());

  FontState st;
  st.name = n;
  st.atlas_key = atlas_key;
  st.atlas_w = render_d3d9_texture_width(atlas_key);
  st.atlas_h = render_d3d9_texture_height(atlas_key);
  st.px_height = font_px_height_from_name(n);
  st.charset.assign(reinterpret_cast<const char*>(cs_bytes.data()),
                    cs_bytes.size());
  int uv_glyphs = 0;
  if (!parse_font_glyphs(scx_bytes.data(), scx_bytes.size(), &st.glyphs,
                         &uv_glyphs) ||
      uv_glyphs < 32) {
    render_d3d9_texture_destroy(atlas_key);
    return false;
  }
  for (FontGlyph& g : st.glyphs) {
    g.adv_px = (g.u1 - g.u0) * static_cast<float>(st.atlas_w);
    if (g.adv_px < 1.f) g.adv_px = static_cast<float>(st.px_height) * 0.35f;
  }
  st.ready = !st.glyphs.empty();
  {
    auto it = g_fonts.find(key);
    if (it != g_fonts.end() && it->second.atlas_key &&
        it->second.atlas_key != atlas_key)
      render_d3d9_texture_destroy(it->second.atlas_key);
  }
  g_fonts[key] = std::move(st);
  return g_fonts[key].ready;
}

bool render_d3d9_font_load_from_rid(void* key, int32_t res_id) {
  if (!key || res_id == 0) return false;
  const RpakEntry* ent = rpak_find_entry(res_id);
  if (!ent || ent->name.empty()) return false;
  return render_d3d9_font_load(key, ent->name.c_str());
}

// PE 940: a Text charset is a typeof-14 render object; its payload names the
// mesh and the texture ("mesh 0x4A / texture 0x26"; fade_render 0x1A25 ->
// mesh 0x1A4A fade.SCX, texture 0xA8). RT_text takes the glyph vertices from
// that mesh and the texture from its section material (FUN_0046dc10), so the
// font is whatever the pack says, not a file found by name.
// Fork: the atlas is decoded here from the texture's sourcefile; a texture
// that cannot be decoded leaves the font without one and the glyphs are
// drawn with the plain diffuse colour.
bool render_d3d9_font_load_charset(void* key, int32_t res_id) {
  if (!key || res_id == 0) return false;
  std::string name;
  std::vector<uint8_t> blob;
  {
    const RpakEntry* ent = rpak_find_entry(res_id);
    if (!ent) return false;
    name = ent->name;
  }
  if (!rpak_read_entry(res_id, &blob)) return false;
  uint32_t mesh_local = 0, tex_local = 0;
  if (!render_type_ids(blob, &mesh_local, &tex_local)) return false;
  const int32_t pack = rpak_id_pack(res_id);
  const int32_t mesh_id = static_cast<int32_t>(rpak_remap_local_id(pack, mesh_local));
  const int32_t tex_id =
      tex_local ? static_cast<int32_t>(rpak_remap_local_id(pack, tex_local)) : 0;

  std::vector<uint8_t> scx;
  std::string scx_path;
  {
    const RpakEntry* me = mesh_id ? rpak_find_entry(mesh_id) : nullptr;
    if (!me) return false;
    scx_path = me->path;
  }
  {
    std::vector<uint8_t> mblob;
    if (rpak_read_entry(mesh_id, &mblob) && mblob.size() >= 4 &&
        std::memcmp(mblob.data(), "INVO", 4) == 0)
      scx = std::move(mblob);
    else if (!load_file_bytes(scx_path, &scx))
      return false;
  }

  FontState st;
  uint32_t hdr_flags = 0;
  if (!parse_font_glyphs(scx.data(), scx.size(), &st.glyphs, nullptr, &hdr_flags))
    return false;
  st.name = name;
  st.px_height = font_px_height_from_name(name);
  {
    std::vector<uint8_t> cs;
    if (load_file_bytes("frontend/font.dat", &cs))
      st.charset.assign(reinterpret_cast<const char*>(cs.data()), cs.size());
  }
  // FUN_0042ba10: header flags & 0x101 == 0x100 files the diffuse map under
  // entry 0x6000001 of a shader-3 material (CMaterial_ColorMixTex, factory
  // FUN_004297e0: texture entries 0x6000001 / 4 / 5 / 7, no 0x6000000). The
  // text flush asks for 0x6000000 and binds no texture when that lookup
  // returns 0 (FUN_0044cb60, FUN_0043ee70): the fade curtain is the plain
  // text colour.
  st.no_texture = (hdr_flags & 0x101u) == 0x100u;
  void* const atlas_key =
      reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(key) + 0x20000u);
  std::string atlas_path;
  if (!st.no_texture && tex_id) {
    if (const RpakEntry* te = rpak_find_entry(tex_id)) atlas_path = te->path;
  }
  bool uploaded = false;
  if (!atlas_path.empty()) {
    std::vector<uint8_t> ab;
    if (load_file_bytes(atlas_path, &ab) && !ab.empty()) {
      uploaded = path_ends_with_ci(atlas_path, ".tga")
                     ? upload_tga_font_atlas(atlas_key, ab.data(), ab.size(),
                                             atlas_path.c_str())
                     : upload_wic_font_atlas(atlas_key, ab.data(), ab.size(),
                                             atlas_path.c_str(), true);
    }
    if (!uploaded) render_d3d9_texture_destroy(atlas_key);
  }
  if (uploaded) {
    st.atlas_key = atlas_key;
    st.atlas_w = render_d3d9_texture_width(atlas_key);
    st.atlas_h = render_d3d9_texture_height(atlas_key);
  }
  for (FontGlyph& g : st.glyphs) {  // legacy screen pass advance
    g.adv_px = (g.u1 - g.u0) * static_cast<float>(st.atlas_w);
    if (g.adv_px < 1.f) g.adv_px = static_cast<float>(st.px_height) * 0.35f;
  }
  st.ready = true;
  std::printf("[font] %s atlas=%s (charset mesh %s, %zu glyphs)\n", name.c_str(),
              uploaded ? atlas_path.c_str() : "none", scx_path.c_str(),
              st.glyphs.size());
  {
    auto it = g_fonts.find(key);
    if (it != g_fonts.end() && it->second.atlas_key &&
        it->second.atlas_key != st.atlas_key)
      render_d3d9_texture_destroy(it->second.atlas_key);
  }
  g_fonts[key] = std::move(st);
  return true;
}

bool render_d3d9_font_ready(void* key) {
  auto it = g_fonts.find(key);
  return it != g_fonts.end() && it->second.ready;
}

const char* render_d3d9_font_name(void* key) {
  auto it = g_fonts.find(key);
  if (it == g_fonts.end() || !it->second.ready) return "";
  return it->second.name.c_str();
}

int32_t render_d3d9_font_glyph_count(void* key) {
  auto it = g_fonts.find(key);
  if (it == g_fonts.end()) return 0;
  return static_cast<int32_t>(it->second.glyphs.size());
}

int32_t render_d3d9_font_px_height(void* key) {
  auto it = g_fonts.find(key);
  if (it == g_fonts.end() || !it->second.ready) return 0;
  return it->second.px_height;
}

float render_d3d9_font_measure_px(void* key, const char* text) {
  auto it = g_fonts.find(key);
  if (it == g_fonts.end() || !it->second.ready || !text) return 0.f;
  // PE 940 Text.getWidthPixels FUN_004e5ae0 -> FUN_00421100(charset, cstr,
  // 1.0, 0) -> C3DLow vt+0x214: the charset mesh advances, space included.
  return font_measure_run(it->second, text, 1.f);
}

void render_d3d9_font_destroy(void* key) {
  auto it = g_fonts.find(key);
  if (it == g_fonts.end()) return;
  if (it->second.atlas_key) render_d3d9_texture_destroy(it->second.atlas_key);
  g_fonts.erase(it);
}

void render_d3d9_text_create(void* key, void* font, float x, float y) {
  if (!key) return;
  TextState& t = g_texts[key];
  t.font = font;
  t.x = x;
  t.y = y;
  t.color = 0xFFFFFFFFu;
  t.align = 2;
  t.text.clear();
  t.alive = true;
  t.tree = false;
  t.adv_scale = 1.f;
  t.mesh_sx = t.mesh_sy = 1.f;
  t.seq = ++g_text_seq;
}

void render_d3d9_text_set_tree(void* key, int32_t tree) {
  auto it = g_texts.find(key);
  if (it != g_texts.end()) it->second.tree = tree != 0;
}

void render_d3d9_text_set_advance_scale(void* key, float scale) {
  auto it = g_texts.find(key);
  if (it != g_texts.end()) it->second.adv_scale = scale;
}

void render_d3d9_text_set_scale(void* key, float sx, float sy) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.mesh_sx = sx;
  it->second.mesh_sy = sy;
}

void render_d3d9_text_destroy(void* key) {
  if (!key) return;
  for (size_t i = 0; i < g_osd_text.size();) {
    if (g_osd_text[i].text_key == key)
      g_osd_text.erase(g_osd_text.begin() + static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
  g_texts.erase(key);
}

void render_d3d9_text_set_color(void* key, uint32_t argb) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.color = argb;
  for (auto& ot : g_osd_text) {
    if (ot.text_key == key) ot.color = argb;
  }
}

void render_d3d9_text_set_align(void* key, int32_t align) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.align = align;
}

void render_d3d9_text_set_pos(void* key, float x, float y) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.x = x;
  it->second.y = y;
}

void render_d3d9_text_set_string(void* key, const char* utf8) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.text = utf8 ? utf8 : "";
}

void render_d3d9_text_set_visible(void* key, int32_t visible) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.visible = visible != 0;
  for (auto& ot : g_osd_text) {
    if (ot.text_key == key) ot.visible = it->second.visible;
  }
}

const char* render_d3d9_text_get_string(void* key) {
  auto it = g_texts.find(key);
  if (it == g_texts.end() || !it->second.alive) return "";
  return it->second.text.c_str();
}

void render_d3d9_text_update(void* key) {
  auto it = g_texts.find(key);
  if (it == g_texts.end() || !it->second.alive) return;
  const TextState& t = it->second;
  // Replace existing OSD entry for this text instance.
  for (size_t i = 0; i < g_osd_text.size();) {
    if (g_osd_text[i].text_key == key)
      g_osd_text.erase(g_osd_text.begin() + static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
  if (t.text.empty() || !t.font) return;
  OsdText ot;
  ot.text_key = key;
  ot.font = t.font;
  ot.x = t.x;
  ot.y = t.y;
  ot.color = t.color;
  ot.align = t.align;
  ot.text = t.text;
  ot.pri = 0;
  ot.visible = t.visible;
  g_osd_text.push_back(std::move(ot));
}

// PE 940 text section of a camera pass: rg_text_section (cam+0x4b4), the
// last render group of FUN_00460ba0, after every mesh group.
// An r_text instance is not 3D geometry. RI_text sync (FUN_0046a110 /
// FUN_0051db70) takes only the bone00 WORLD translation; (m41, m42) in
// script units are the NDC anchor with y DOWN, and C3DLow vt+0x200
// (FUN_0044c400) maps it into the pixel rect of the current viewport:
//   px = vp_w * (ndc_x + 1) * 0.5 + vp_left
//   py = vp_h * (ndc_y + 1) * 0.5 + vp_top
// FUN_0044c4a0 (font mode): pen.x -= width for align 0 (RIGHT), width / 2
// for align 1 (CENTER), nothing for align 2 (LEFT), width = FUN_0044bfb0.
// Per glyph FUN_0044bdc0 writes the four charset vertices c*4 .. c*4+3 as
// XYZRHW|DIFFUSE|TEX1: x = pen.x + v.x - 0.25, y = py + v.y - 0.25, z = 0,
// rhw = 100, diffuse = run colour, uv copied; then pen.x += vertex[1].x *
// scale (the advance keeps its sign: fade.SCX 'a' moves the pen by -4341.8
// px and is one large triangle that covers the viewport only that way).
// Two triangles per glyph, (0,1,2) and (2,1,3). ESC (0x1b) 'C' + 8 hex
// digits = run colour, ESC 'c' = back to the call colour, ESC + any other
// byte is skipped.
// State: section begin FUN_0045e8b0 = C3DLow vt+0x34(2,1) (FUN_00422c60):
// ZWRITE off, SRCALPHA / INVSRCALPHA, BLENDOP ADD, alpha test GREATER ref 4,
// Z test left on (z = 0 always passes). Flush FUN_0044cb60: CULL NONE, POINT
// min / mag, no mip, fog off, stage 0 ALPHAARG2 = CURRENT with the engine
// defaults ALPHAOP MODULATE / ALPHAARG1 TEXTURE (FUN_00426d10), colour =
// texture x diffuse (D3D default; the text path never sets the colour op),
// FVF 0x144, texture = material entry 0x6000000 or none.
// Order: the section keeps one rg_font group per charset; FUN_004633f0
// appends a new group the first time a text of that charset is queued and
// FUN_00462400 appends the text to its group, so texts are drawn font by
// font (rg_font begin FUN_0045e5f0 = SetFont, loop FUN_0045f7f0).
// Fork: the groups are rebuilt every frame from creation stamps (font order
// = the oldest text of each font this camera sees, creation order inside a
// font); the exe keeps them per camera from the first frame a text was seen
// and swaps the last entry into a removed slot (FUN_00463ce0). Text.setScale
// is applied per text (see java_render_Text_setScale). Without a texture the
// stage selects the diffuse colour: Direct3D 9 documents an unbound
// D3DTA_TEXTURE as white, and for the black curtain the result is the same
// whatever the driver returns.
static void draw_camera_text_section(const std::vector<void*>& keys) {
  if (keys.empty() || !g_dev) return;
  static const bool trace_on = std::getenv("SLRR_RENDER_TRACE") != nullptr;
  D3DVIEWPORT9 dvp{};
  if (FAILED(g_dev->GetViewport(&dvp))) return;
  const float vp_left = static_cast<float>(dvp.X);
  const float vp_top = static_cast<float>(dvp.Y);
  const float vp_w = static_cast<float>(dvp.Width);
  const float vp_h = static_cast<float>(dvp.Height);

  struct Item {
    void* key;
    const TextState* t;
    const FontState* font;
  };
  std::vector<Item> items;
  items.reserve(keys.size());
  for (void* key : keys) {
    auto kit = g_texts.find(key);
    if (kit == g_texts.end()) continue;
    auto fit = g_fonts.find(kit->second.font);
    if (fit == g_fonts.end() || !fit->second.ready) continue;
    items.push_back({key, &kit->second, &fit->second});
  }
  if (items.empty()) return;
  {
    // Font groups in first-use order, texts in creation order inside one.
    std::unordered_map<std::string, uint32_t> group_seq;
    for (const Item& it : items) {
      auto r = group_seq.emplace(it.font->name, it.t->seq);
      if (!r.second && it.t->seq < r.first->second) r.first->second = it.t->seq;
    }
    std::sort(items.begin(), items.end(), [&group_seq](const Item& a, const Item& b) {
      const uint32_t ga = group_seq[a.font->name], gb = group_seq[b.font->name];
      if (ga != gb) return ga < gb;
      return a.t->seq != b.t->seq ? a.t->seq < b.t->seq : a.key < b.key;
    });
  }

  static const D3DRENDERSTATETYPE kRs[] = {
      D3DRS_ZENABLE,          D3DRS_ZFUNC,     D3DRS_ZWRITEENABLE,
      D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,  D3DRS_DESTBLEND,
      D3DRS_BLENDOP,          D3DRS_ALPHATESTENABLE, D3DRS_ALPHAFUNC,
      D3DRS_ALPHAREF,         D3DRS_CULLMODE,  D3DRS_FOGENABLE,
      D3DRS_LIGHTING};
  static const D3DSAMPLERSTATETYPE kSs[] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER,
                                           D3DSAMP_MIPFILTER};
  static const D3DTEXTURESTAGESTATETYPE kTs[] = {
      D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2,
      D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2};
  DWORD rs_saved[sizeof(kRs) / sizeof(kRs[0])] = {};
  DWORD ss_saved[sizeof(kSs) / sizeof(kSs[0])] = {};
  DWORD ts_saved[sizeof(kTs) / sizeof(kTs[0])] = {};
  for (size_t i = 0; i < sizeof(kRs) / sizeof(kRs[0]); ++i)
    g_dev->GetRenderState(kRs[i], &rs_saved[i]);
  for (size_t i = 0; i < sizeof(kSs) / sizeof(kSs[0]); ++i)
    g_dev->GetSamplerState(0, kSs[i], &ss_saved[i]);
  for (size_t i = 0; i < sizeof(kTs) / sizeof(kTs[0]); ++i)
    g_dev->GetTextureStageState(0, kTs[i], &ts_saved[i]);

  struct Vtx {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
  };
  g_dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);  // 0x144
  g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
  g_dev->SetRenderState(D3DRS_ALPHAREF, 4);
  g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  g_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  g_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_CURRENT);

  std::vector<Vtx> verts;
  std::vector<uint16_t> idx;
  auto flush = [&]() {
    if (!idx.empty())
      g_dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0,
                                    static_cast<UINT>(verts.size()),
                                    static_cast<UINT>(idx.size() / 3), idx.data(),
                                    D3DFMT_INDEX16, verts.data(), sizeof(Vtx));
    verts.clear();
    idx.clear();
  };
  int textured = -1;  // stage setup of the previous text
  for (const Item& item : items) {
    const TextState& t = *item.t;
    const FontState& font = *item.font;
    IDirect3DTexture9* tex = nullptr;
    if (!font.no_texture && font.atlas_key) {
      auto tit = g_textures.find(font.atlas_key);
      if (tit != g_textures.end()) tex = tit->second.tex;
    }
    g_dev->SetTexture(0, tex);
    if (textured != (tex ? 1 : 0)) {
      textured = tex ? 1 : 0;
      g_dev->SetTextureStageState(0, D3DTSS_COLOROP,
                                  tex ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
      g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP,
                                  tex ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
    }
    // Anchor: bone00 world translation = NDC, y down (g_fold_own_bone0 is
    // set by the pass, so the text's own (x, z, 0) pose is in the world).
    D3DMATRIX world{};
    resolve_world(item.key, &world, 0);
    const float ndc_x = world.m[3][0], ndc_y = world.m[3][1];
    float pen = vp_w * (ndc_x + 1.f) * 0.5f + vp_left;
    const float py = vp_h * (ndc_y + 1.f) * 0.5f + vp_top;
    const float adv = t.adv_scale * t.mesh_sx;
    float width = 0.f;
    if (t.align != 2) width = font_measure_run(font, t.text.c_str(), adv);
    if (t.align == 0)
      pen -= width;
    else if (t.align == 1)
      pen -= width * 0.5f;
    const float pen0 = pen;
    DWORD colour = t.color;
    int glyphs = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(t.text.c_str());
    while (*p) {
      if (*p == 0x1b) {
        const unsigned char e = p[1];
        if (!e) break;  // Fork: a trailing ESC ends the run
        p += 2;
        if (e == 'C') {
          DWORD c = 0;
          int n = 0;
          for (; n < 8 && *p; ++n, ++p) {
            const unsigned char h = *p;
            c <<= 4;
            if (h >= '0' && h <= '9') c += static_cast<DWORD>(h - '0');
            else if (h >= 'A' && h <= 'F') c += static_cast<DWORD>(h - 'A' + 10);
            else if (h >= 'a' && h <= 'f') c += static_cast<DWORD>(h - 'a' + 10);
          }
          if (n < 8) break;  // the string ended inside the colour
          colour = c;
        } else if (e == 'c') {
          colour = t.color;
        }
        continue;
      }
      const size_t gi = *p++;
      if (gi >= font.glyphs.size()) continue;
      const FontGlyph& g = font.glyphs[gi];
      if (verts.size() + 4 > 0xFFF0u) flush();
      const uint16_t base = static_cast<uint16_t>(verts.size());
      for (int i = 0; i < 4; ++i)
        verts.push_back({pen + g.px[i] * t.mesh_sx - 0.25f,
                         py + g.py[i] * t.mesh_sy - 0.25f, 0.f, 100.f, colour,
                         g.tu[i], g.tv[i]});
      const uint16_t tri[6] = {base, static_cast<uint16_t>(base + 1),
                               static_cast<uint16_t>(base + 2),
                               static_cast<uint16_t>(base + 2),
                               static_cast<uint16_t>(base + 1),
                               static_cast<uint16_t>(base + 3)};
      idx.insert(idx.end(), tri, tri + 6);
      pen += g.advance * adv;
      ++glyphs;
    }
    flush();
    if (trace_on) {
      char b[512];
      std::snprintf(b, sizeof b,
                    "  text %p '%.48s' font=%s align=%d color=%08X ndc=(%.4f,%.4f) px=(%.1f,%.1f) pen0=%.1f end=%.1f glyphs=%d tex=%d",
                    item.key, t.text.c_str(), font.name.c_str(), t.align,
                    static_cast<unsigned>(t.color), ndc_x, ndc_y,
                    vp_w * (ndc_x + 1.f) * 0.5f + vp_left, py, pen0, pen, glyphs,
                    tex ? 1 : 0);
      g_cam_pass_log.emplace_back(b);
    }
  }
  g_cam_pass_stats.texts += static_cast<int32_t>(items.size());

  g_dev->SetTexture(0, nullptr);
  for (size_t i = 0; i < sizeof(kRs) / sizeof(kRs[0]); ++i)
    g_dev->SetRenderState(kRs[i], rs_saved[i]);
  for (size_t i = 0; i < sizeof(kSs) / sizeof(kSs[0]); ++i)
    g_dev->SetSamplerState(0, kSs[i], ss_saved[i]);
  for (size_t i = 0; i < sizeof(kTs) / sizeof(kTs[0]); ++i)
    g_dev->SetTextureStageState(0, kTs[i], ts_saved[i]);
}

namespace {

bool read_u32(const uint8_t* data, size_t size, size_t off, uint32_t* out) {
  if (off + 4 > size || !out) return false;
  std::memcpy(out, data + off, 4);
  return true;
}

std::string scx_mat_name(const uint8_t* data, size_t size, uint32_t off,
                         uint32_t end) {
  if (off + 24 > end || end > size) return {};
  // Observed at +0x14: big-endian length (or 3 zero pad + len byte), then ASCII.
  // e.g. 00 00 00 08 "F grill\0" / 00 00 00 08 "hood\0..."
  const uint32_t p = off + 0x14;
  if (p + 4 >= end) return {};
  uint32_t n = (static_cast<uint32_t>(data[p]) << 24) |
               (static_cast<uint32_t>(data[p + 1]) << 16) |
               (static_cast<uint32_t>(data[p + 2]) << 8) |
               static_cast<uint32_t>(data[p + 3]);
  if (n == 0 || n > 64 || p + 4 + n > end) return {};
  uint32_t len = n;
  while (len > 0 && data[p + 4 + len - 1] == 0) --len;
  if (len == 0) return {};
  for (uint32_t i = 0; i < len; ++i) {
    const uint8_t c = data[p + 4 + i];
    if (c < 32 || c > 126) return {};
  }
  return std::string(reinterpret_cast<const char*>(data + p + 4), len);
}

uint32_t scx_mat_diffuse(const uint8_t* data, size_t size, uint32_t off,
                         uint32_t end) {
  if (off + 64 > end || end > size) return 0xFF808080u;
  // First 0xAARRGGBB-ish dword with alpha 0xFF after the name field.
  for (uint32_t p = off + 0x30; p + 4 <= end; p += 4) {
    uint32_t c = 0;
    if (!read_u32(data, size, p, &c)) break;
    if ((c >> 24) == 0xffu) return c;
  }
  return 0xFF808080u;
}

// PE 940 INVO v4 material chunk (reader FUN_0042c340): +0x08 shader id,
// +0x0C create flags, +0x10 entry count, entries from +0x14. Entry = dword
// (type in the top byte, id in the low 24 bits) + payload; entry sizes by
// type: 0/1/5/7 = 8 bytes, 2 = 12, 3 = 16, 4 = 20, 6 = 32, 8 = 36.
// Type 0 = colour (packed ARGB): id 0 diffuse, id 1 specular, id 2
// illumination. Type 5 = integer parameter (id 0: alpha reference; on disk
// only in alpha-test chunks, values 20 / 25). Disk census of the Build 940
// install: the walk ends exactly on the chunk end for all 4196 material
// chunks; 115 physics meshes (phys_*.scx) keep something else in their
// type-1 chunk (count 0x2000x) and are left on the legacy material.
// Fork: an alpha-test chunk without a type 5 entry uses 0x38, the value the
// v3 builder sets (FUN_0042ba10); the exe's v4 default was not read.
// Type 6 = texture entry, 32 bytes (FUN_00440fe0): +0 0x06000000 | kind, +4
// slot (index into the texture lines of the render object, FUN_00441c40),
// +8 uv channel, +0xC address bits (1 wrap U, 2 wrap V; a clear bit
// clamps), +0x10 scaleU, +0x14 scaleV, +0x18 offsetU, +0x1C offsetV. Kinds:
// 0 primary, 1 second, 3 sphere / env map, 4 lightmap (the reader renames
// it 7). The host keeps slot, channel, address bits and the uv scale and
// offset per kind (kind 5 is not kept: no class the host draws reads it).
// Disk census of the Build 940 install (2,487 v4 files, 4,215 material
// chunks): 173 primary entries carry a scale / offset that is not 1, 1, 0,
// 0 (atlas cells of the city facades and signs); every second, env and
// lightmap entry is 1, 1, 0, 0.
//
// PE 940 material factory FUN_0042bff0 (switch on shader id & 0xffffff).
// The Tex family - FUN_0042af10 (id 0 and every id the switch does not
// know), FUN_0042b400 (4), FUN_0042b670 (8 / 0x10 / 0x20), FUN_0042b520
// (0xc / 0x14 / 0x24) - writes its colour id 0 entry with FUN_00441270(0)
// = opaque white AFTER merging the file entries (FUN_0043f2e0; 0x42aff2 ->
// 0x42b002, 0x42b4f9 -> 0x42b506, 0x42b7aa -> 0x42b7ba, 0x42b647 ->
// 0x42b654), so the file's diffuse never reaches Diffuse / Ambient or the
// TEXTUREFACTOR alpha (FUN_00434170 reads that entry). Every other factory
// declares colour 0 before the merge (the file value stays) or has no
// colour 0 entry at all (0x1d, 0x37, 0xfffffb; left on the file value).
bool scx_shader_white_diffuse(uint32_t shader) {
  switch (shader & 0xffffffu) {
    case 0x00: case 0x04: case 0x08: case 0x10: case 0x20:
    case 0x0c: case 0x14: case 0x24:
      return true;
    case 0x01: case 0x19:                                    // FUN_0042a380
    case 0x02: case 0x06: case 0x0a: case 0x0e: case 0x12:   // FUN_0042ac10
    case 0x16: case 0x1a: case 0x1e: case 0x22: case 0x26:
    case 0x03: case 0x1b:                                    // FUN_004297e0
    case 0x05:                                               // FUN_0042a660
    case 0x07: case 0x1f:                                    // FUN_00429920
    case 0x09: case 0x11: case 0x21:                         // FUN_0042aab0
    case 0x0b: case 0x13: case 0x23:                         // FUN_00429bc0
    case 0x0d: case 0x25:                                    // FUN_0042a7d0
    case 0x0f: case 0x17: case 0x27:                         // FUN_00429a40
    case 0x15: case 0x18: case 0x1c: case 0x1d:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
    case 0x37:
    case 0xfffffb: case 0xfffffc: case 0xfffffd: case 0xfffffe:
    case 0xffffff:
      return false;
    default:
      return true;  // FUN_0042bff0 default: FUN_0042af10
  }
}

// PE 940 material vt+8 (the stream description): the classes that list a
// second texture entry as texcoord set 1 beside the primary as set 0 are
// the MixTA classes: FUN_0042c560 (CMaterial_MixTA), FUN_0042c6a0
// (MixTASpecular), FUN_0042c710 (MixTASpheremap, MixTASpecularSpheremap,
// ..Masked). By the shader id the factory FUN_0042bff0 takes.
bool scx_shader_two_sets(uint32_t shader) {
  switch (shader & 0xffffffu) {
    case 0x01: case 0x19: case 0x05: case 0x09: case 0x11: case 0x21:
    case 0x0d: case 0x25: case 0x15:
      return true;
    default:
      return false;
  }
}

void scx_v4_material(const uint8_t* data, size_t size, uint32_t off,
                     uint32_t end, MeshSubmesh* sm) {
  if (!sm || end > size || off + 0x14 > end) return;
  uint32_t shader = 0, flags = 0, count = 0;
  if (!read_u32(data, size, off + 0x08, &shader) ||
      !read_u32(data, size, off + 0x0C, &flags) ||
      !read_u32(data, size, off + 0x10, &count) || count > 64)
    return;
  uint32_t diffuse = 0xFFFFFFFFu, emissive = 0xFF000000u, alpha_ref = 0x38;
  struct TexEntry {
    bool seen = false;
    uint32_t slot = 0, chan = 0, addr = 0;
    float xf[4] = {1.f, 1.f, 0.f, 0.f};  // scaleU, scaleV, offsetU, offsetV
  };
  TexEntry tex_primary, tex_second, tex_env, tex_lightmap;
  // For the class programs of the lit groups (render_d3d9_part6.inc): the
  // colour entries 0 / 1 / 2 and the float entries 0x1000000 (specular
  // power) and 0x1000001 (env factor). FUN_0043f2e0 gives a class entry the
  // FIRST chunk entry with its id.
  uint32_t lit_col[3] = {0, 0, 0}, lit_col_seen = 0, lit_f_seen = 0;
  float lit_f[2] = {0.f, 0.f};
  uint32_t p = off + 0x14;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t head = 0;
    if (p + 4 > end || !read_u32(data, size, p, &head)) return;
    const uint32_t type = head >> 24, id = head & 0x00FFFFFFu;
    uint32_t len = 0;
    switch (type) {
      case 0: case 1: case 5: case 7: len = 8; break;
      case 2: len = 12; break;
      case 3: len = 16; break;
      case 4: len = 20; break;
      case 6: len = 32; break;
      case 8: len = 36; break;
      default: return;  // unknown entry: leave the legacy material
    }
    if (p + len > end) return;
    uint32_t v = 0;
    read_u32(data, size, p + 4, &v);
    if (type == 0 && id <= 2 && !(lit_col_seen & (1u << id))) {
      lit_col_seen |= 1u << id;
      lit_col[id] = v;
    } else if (type == 1 && id <= 1 && !(lit_f_seen & (1u << id))) {
      lit_f_seen |= 1u << id;
      std::memcpy(&lit_f[id], &v, 4);
    }
    if (type == 0 && id == 0) diffuse = v;
    else if (type == 0 && id == 2) emissive = v;
    else if (type == 5 && id == 0) alpha_ref = v;
    else if (type == 6) {
      TexEntry* e = id == 0 ? &tex_primary
                    : id == 1 ? &tex_second
                    : id == 3 ? &tex_env
                    : (id == 4 || id == 7) ? &tex_lightmap : nullptr;
      if (e) {
        e->seen = true;
        e->slot = v;
        read_u32(data, size, p + 8, &e->chan);
        read_u32(data, size, p + 0xC, &e->addr);
        std::memcpy(e->xf, data + p + 0x10, sizeof(e->xf));
      }
    }
    p += len;
  }
  sm->has_mat = true;
  sm->mat_shader = static_cast<int32_t>(shader & 0xffffffu);
  sm->mat_flags = flags;
  sm->mat_diffuse = scx_shader_white_diffuse(shader) ? 0xFFFFFFFFu : diffuse;
  sm->mat_emissive = emissive;
  sm->mat_alpha_ref = alpha_ref;
  // FUN_00441140: a colour entry keeps the packed dword and four floats,
  // byte x DAT_00593e24 (1 / 255). An entry the chunk does not have keeps
  // the default of the class constructor (MeshSubmesh).
  // Fork: a float entry that is not a finite number keeps the default.
  {
    auto unit = [](uint32_t c, int shift) {
      return static_cast<float>((c >> shift) & 0xffu) * 0.003921568859368563f;
    };
    if (lit_col_seen & 1u) {
      sm->col0[0] = unit(lit_col[0], 16);
      sm->col0[1] = unit(lit_col[0], 8);
      sm->col0[2] = unit(lit_col[0], 0);
      sm->col0[3] = unit(lit_col[0], 24);
      sm->col0_packed = lit_col[0];
    }
    if (lit_col_seen & 2u) {
      sm->col1[0] = unit(lit_col[1], 16);
      sm->col1[1] = unit(lit_col[1], 8);
      sm->col1[2] = unit(lit_col[1], 0);
    }
    if (lit_col_seen & 4u) {
      sm->col2[0] = unit(lit_col[2], 16);
      sm->col2[1] = unit(lit_col[2], 8);
      sm->col2[2] = unit(lit_col[2], 0);
    }
    auto finite = [](float f) { return f == f && std::fabs(f) < 1.0e6f; };
    if ((lit_f_seen & 1u) && finite(lit_f[0])) sm->power = lit_f[0];
    if ((lit_f_seen & 2u) && finite(lit_f[1])) sm->env_factor = lit_f[1];
  }
  auto keep = [](const TexEntry& e, int16_t* slot, int16_t* chan,
                 uint8_t* addr, float* xf) {
    if (!e.seen) return;
    *slot = static_cast<int16_t>(e.slot);
    if (chan) *chan = static_cast<int16_t>(e.chan);
    *addr = static_cast<uint8_t>(e.addr & 3u);
    if (xf) {
      // Fork: a scale or offset that is not a finite number reads as the
      // identity (none in the install).
      bool ok = true;
      for (int k = 0; k < 4; ++k)
        if (!(e.xf[k] == e.xf[k]) || std::fabs(e.xf[k]) > 1.0e6f) ok = false;
      if (ok) std::memcpy(xf, e.xf, sizeof(e.xf));
    }
  };
  keep(tex_primary, &sm->slot_primary, &sm->chan_primary, &sm->addr_primary,
       sm->uvx_primary);
  keep(tex_second, &sm->slot_second, &sm->chan_second, &sm->addr_second,
       sm->uvx_second);
  keep(tex_env, &sm->slot_env, nullptr, &sm->addr_env, nullptr);
  keep(tex_lightmap, &sm->slot_lightmap, &sm->chan_lightmap,
       &sm->addr_lightmap, sm->uvx_lightmap);
}

// PE 940 INVO v3 material header -> material (FUN_0042ba10), header at the
// block start: +0x04..+0x10 diffuse RGBA floats (colour id 0, alpha =
// opacity), +0x14..+0x1C specular RGB, +0x20 specular level, +0x24
// shininess, +0x28 flags, +0x2C / +0x30 / +0x32 / +0x34 / +0x38 map indices
// (int16, -1 = none), +0x5C..+0x64 illumination RGB (colour id 2).
// Create flags: the caller's (`caller_flags`: FUN_0042ba10(header, flags);
// the v1..v3 loader FUN_00451b00 passes 0x80000000 for a mesh whose path
// contains "objects\meshes", the ground loader FUN_00451130 always), and
// prelit (0x80000000) when flags & 2 or the +0x38 map exists;
// transparent when flags & 1 or opacity < 0.99 (DAT_005947ac): 0x40000000,
// or 0x20000000 (alpha test, reference 0x38) when flags & 0x800.
// Shader id handed to the factory FUN_0042bff0:
//   textured = the +0x2C or the +0x34 map exists; without the +0x34 map the
//   flags lose 0x1000 and 0x40 first (& 0xffffefbf);
//   specular = no +0x30 map, level * specular > 0.1 (DAT_005935d4) on a
//   channel, and shininess > 8 (DAT_005947b0);
//   untextured: 0x30, +1 when specular, +2 when the +0x32 map exists;
//   textured: k = 1 (flags & 0x40), else 2 (flags & 0x1000), else 3
//   (flags & 0x101 == 0x100, which also drops the transparent / alpha-test
//   bits, & 0x9fffffff), else 0; id = k, +4 when specular, +8 when the
//   +0x32 map exists.
// So a plain textured v3 material is shader 0 / 4 / 8 / 0xc = the Tex family
// (opaque white Diffuse, see scx_shader_white_diffuse); fade.SCX (flags
// 0x100) is shader 3, CMaterial_ColorMixTex.
void scx_v3_material(const uint8_t* data, size_t size, size_t base,
                     uint32_t hsz, uint32_t caller_flags, MeshSubmesh* sm) {
  if (!sm || hsz < 0x68u || base + 0x68 > size) return;
  float d[4] = {1, 1, 1, 1}, sp[3] = {0, 0, 0}, e[3] = {0, 0, 0};
  float spec_level = 0.f, shininess = 0.f;
  uint32_t f28 = 0;
  int16_t map2c = -1, map30 = -1, map32 = -1, map34 = -1, map38 = -1;
  std::memcpy(d, data + base + 0x04, 16);
  std::memcpy(sp, data + base + 0x14, 12);
  std::memcpy(&spec_level, data + base + 0x20, 4);
  std::memcpy(&shininess, data + base + 0x24, 4);
  std::memcpy(&f28, data + base + 0x28, 4);
  std::memcpy(&map2c, data + base + 0x2C, 2);
  std::memcpy(&map30, data + base + 0x30, 2);
  std::memcpy(&map32, data + base + 0x32, 2);
  std::memcpy(&map34, data + base + 0x34, 2);
  std::memcpy(&map38, data + base + 0x38, 2);
  std::memcpy(e, data + base + 0x5C, 12);
  auto chan = [](float c) -> uint32_t {
    if (!(c == c)) c = 0.f;
    if (c < 0.f) c = 0.f;
    if (c > 1.f) c = 1.f;
    return static_cast<uint32_t>(c * 255.f + 0.5f);
  };
  const bool textured = map2c != -1 || map34 != -1;
  if (map34 == -1) f28 &= 0xffffefbfu;
  uint32_t mf = caller_flags;
  if ((f28 & 2u) != 0 || map38 != -1) mf |= 0x80000000u;
  if ((f28 & 1u) != 0 || d[3] < 0.99f)
    mf |= (f28 & 0x800u) ? 0x20000000u : 0x40000000u;
  const bool specular =
      map30 == -1 && shininess > 8.f &&
      (spec_level * sp[0] > 0.1f || spec_level * sp[1] > 0.1f ||
       spec_level * sp[2] > 0.1f);
  uint32_t shader = 0;
  if (!textured) {
    shader = 0x30u + (specular ? 1u : 0u) + (map32 != -1 ? 2u : 0u);
  } else {
    const uint32_t k = (f28 & 0x40u) ? 1u
                       : (f28 & 0x1000u) ? 2u
                       : ((f28 & 0x101u) == 0x100u) ? 3u : 0u;
    if (k == 3u) mf &= 0x9fffffffu;
    shader = k + (specular ? 4u : 0u) + (map32 != -1 ? 8u : 0u);
  }
  sm->has_mat = true;
  sm->mat_shader = static_cast<int32_t>(shader);
  sm->mat_flags = mf;
  sm->mat_diffuse =
      scx_shader_white_diffuse(shader)
          ? 0xFFFFFFFFu
          : (chan(d[3]) << 24) | (chan(d[0]) << 16) | (chan(d[1]) << 8) | chan(d[2]);
  sm->mat_emissive = 0xFF000000u | (chan(e[0]) << 16) | (chan(e[1]) << 8) | chan(e[2]);
  sm->mat_alpha_ref = 0x38;
  // The entries as the class programs of the lit groups read them
  // (render_d3d9_part6.inc), FUN_0042ba10: colour 0 = the diffuse rgba,
  // colour 1 = level x specular colour, colour 2 = the illumination, float
  // 0x1000000 = the shininess, and with an env map (+0x32) float 0x1000001
  // = (spec.g + spec.r + spec.b) x DAT_00593700 (1 / 6, without the level).
  // FUN_004411c0 keeps the floats and packs (int)(x x 255) per channel.
  // Fork: a value that is not a finite number reads 0.
  {
    auto fin = [](float c) {
      return (c == c && std::fabs(c) < 1.0e6f) ? c : 0.f;
    };
    auto pack = [](float c) {
      return static_cast<uint32_t>(static_cast<int32_t>(c * 255.f));
    };
    for (int k = 0; k < 4; ++k) sm->col0[k] = fin(d[k]);
    sm->col0_packed = (((((pack(sm->col0[3]) << 8) | pack(sm->col0[0])) << 8) |
                        pack(sm->col0[1]))
                       << 8) |
                      pack(sm->col0[2]);
    for (int k = 0; k < 3; ++k) {
      sm->col1[k] = fin(spec_level * sp[k]);
      sm->col2[k] = fin(e[k]);
    }
    sm->power = fin(shininess);
    if (map32 != -1) sm->env_factor = fin((sp[1] + sp[0] + sp[2]) * 0.16666667f);
  }
  // The texture entries FUN_0042ba10 makes from the header, by slot into
  // the render object's texture list: +0x2C primary (uv channel = the short
  // at +0x46), +0x34 second (uv channel +0x48), +0x32 sphere / env map,
  // +0x38 lightmap (always uv channel 1). The header reader FUN_004583f0
  // turns a primary channel that is not 0, 1 or 2 into 0.
  // Not kept apart: for shader 3 (flags & 0x101 == 0x100) the primary slot
  // goes to entry 0x6000001 instead of 0x6000000.
  uint16_t ch46 = 0;
  int16_t ch48 = 0;
  std::memcpy(&ch46, data + base + 0x46, 2);
  std::memcpy(&ch48, data + base + 0x48, 2);
  sm->slot_primary = map2c;
  sm->slot_second = map34;
  sm->slot_env = map32;
  sm->slot_lightmap = map38;
  sm->chan_primary = ch46 > 2 ? int16_t{0} : static_cast<int16_t>(ch46);
  sm->chan_second = ch48;
  sm->chan_lightmap = 1;
}

std::string ascii_lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

std::string mat_name_to_token(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    if (c == ' ' || c == '-' || c == '/')
      out.push_back('_');
    else
      out.push_back(c);
  }
  return out;
}

std::string guess_car_prefix(const std::filesystem::path& scx_path) {
  // .../Baiern_data/meshes/hood.scx → Baiern
  std::filesystem::path cur = scx_path;
  for (int i = 0; i < 4 && !cur.empty(); ++i) {
    std::string stem = cur.stem().string();
    if (stem.size() > 5) {
      const std::string low = ascii_lower(stem);
      if (low.size() >= 5 && low.compare(low.size() - 5, 5, "_data") == 0)
        return stem.substr(0, stem.size() - 5);
    }
    if (ascii_lower(cur.filename().string()) == "meshes") {
      std::string parent = cur.parent_path().stem().string();
      const std::string low = ascii_lower(parent);
      if (low.size() >= 5 && low.compare(low.size() - 5, 5, "_data") == 0)
        return parent.substr(0, parent.size() - 5);
      if (!parent.empty()) return parent;
    }
    cur = cur.parent_path();
  }
  return {};
}

std::filesystem::path guess_textures_dir(const std::filesystem::path& scx_path) {
  namespace fs = std::filesystem;
  const fs::path parent = scx_path.parent_path();
  fs::path cand = parent / "textures";
  if (fs::is_directory(cand)) return cand;
  cand = parent.parent_path() / "textures";
  if (fs::is_directory(cand)) return cand;
  return parent.parent_path() / "textures";
}

std::string resolve_mat_texture_path(const std::filesystem::path& scx_path,
                                     const std::string& mat_name) {
  namespace fs = std::filesystem;
  if (mat_name.empty()) return {};
  const fs::path texdir = guess_textures_dir(scx_path);
  if (!fs::is_directory(texdir)) return {};

  const std::string prefix = guess_car_prefix(scx_path);
  const std::string token = mat_name_to_token(mat_name);
  const std::string token_l = ascii_lower(token);
  const std::string prefix_l = ascii_lower(prefix);

  // Prefer exact / short matches: Prefix_token.dds then Prefix_token_*.dds
  std::string best;
  int best_score = -1;
  std::error_code ec;
  for (fs::directory_iterator it(texdir, ec); !ec && it != fs::directory_iterator();
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string fname = it->path().filename().string();
    const std::string fl = ascii_lower(fname);
    if (fl.size() < 5 || fl.compare(fl.size() - 4, 4, ".dds") != 0) continue;

    int score = -1;
    if (!prefix_l.empty()) {
      const std::string exact = prefix_l + "_" + token_l + ".dds";
      if (fl == exact) score = 1000;
      else {
        const std::string stem = prefix_l + "_" + token_l + "_";
        if (fl.compare(0, stem.size(), stem) == 0) {
          // Prefer shorter suffix (base variants over CITY etc. not in name).
          score = 800 - static_cast<int>(fl.size());
        }
      }
    }
    if (score < 0) {
      // Fallback: filename contains token (e.g. rearview_mirror).
      if (fl.find(token_l) != std::string::npos)
        score = 400 - static_cast<int>(fl.size());
    }
    if (score > best_score) {
      best_score = score;
      best = it->path().string();
    }
  }
  return best;
}

void* mesh_sub_tex_key(void* mesh_key, size_t index) {
  return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mesh_key) +
                                 0x10000u + index + 1u);
}

void bind_mesh_textures(void* mesh_key, MeshState& st) {
  if (!mesh_key) return;
  std::filesystem::path scx;
  if (!st.label.empty()) scx = st.label;
  for (size_t i = 0; i < st.subs.size(); ++i) {
    MeshSubmesh& sm = st.subs[i];
    if (sm.name.empty()) continue;
    // Bound by slot from a render object's texture list (PE 940
    // FUN_00441c40): the record decides, not a file name. This function
    // runs again on every clone.
    if (sm.slot_bound) continue;
    const std::string path = resolve_mat_texture_path(scx, sm.name);
    if (path.empty()) continue;
    void* tkey = mesh_sub_tex_key(mesh_key, i);
    if (render_d3d9_texture_create_from_file(tkey, path.c_str()) &&
        render_d3d9_texture_ready(tkey)) {
      sm.texture_key = tkey;
      sm.texture_path = path;
    } else {
      render_d3d9_texture_destroy(tkey);
      sm.texture_key = nullptr;
      sm.texture_path.clear();
    }
  }
}

void release_mesh_textures(void* mesh_key, MeshState& st) {
  for (size_t i = 0; i < st.subs.size(); ++i) {
    MeshSubmesh& sm = st.subs[i];
    if (sm.texture_key && mesh_key &&
        sm.texture_key == mesh_sub_tex_key(mesh_key, i))
      render_d3d9_texture_destroy(sm.texture_key);
    sm.texture_key = nullptr;
    sm.texture_path.clear();
  }
}

bool parse_scx_v4(const uint8_t* data, size_t size, MeshState& out) {
  if (!data || size < 24) return false;
  if (std::memcmp(data, "INVO", 4) != 0) return false;
  uint32_t ver = 0, nchunk = 0, rsv = 0;
  if (!read_u32(data, size, 4, &ver) || !read_u32(data, size, 8, &nchunk) ||
      !read_u32(data, size, 12, &rsv))
    return false;
  if (ver != 4 || nchunk == 0 || nchunk > 4096) return false;
  if (16ull + static_cast<uint64_t>(nchunk) * 8ull > size) return false;

  // PE 940 FUN_00452960: the file is the count at +8, that many {chunk
  // type, offset} pairs from +12, and then the chunks {type, size, ..} one
  // after the other: 0 a material, which opens a group; 1 / 2 / 3 a
  // section of that group (rigid / two bones {dword, boneA, boneB} / a
  // bone list {dword, n, bone[n]}); 4 the vertices and 5 the indices of
  // the section before them.
  // Fork: the legacy path keeps its old reading of the directory: the
  // offset of chunk i with the type of chunk i + 1 (the dword at 20 + 8i).
  // With it a material is "the chunk before a rigid section", the vertex
  // chunk "the one before type 5" and the index chunk "the one before type
  // 0", which finds every rigid mesh and refuses a mesh with a type-2 or
  // type-3 section (spring.scx, shock_absorber.scx, FR_strut.scx). The
  // Build 940 boot reads the pairs as they are.
  const bool boot940 = render_boot940();
  struct Chunk {
    uint32_t off;
    uint32_t typ;
  };
  std::vector<Chunk> chunks(nchunk);
  for (uint32_t i = 0; i < nchunk; ++i) {
    if (!read_u32(data, size, 16 + i * 8, &chunks[i].off) ||
        !read_u32(data, size, boot940 ? 12 + i * 8 : 16 + i * 8 + 4,
                  &chunks[i].typ))
      return false;
    if (chunks[i].off >= size) return false;
  }

  auto chunk_end = [&](uint32_t i) -> uint32_t {
    if (i + 1 < nchunk) return chunks[i + 1].off;
    return static_cast<uint32_t>(size);
  };

  // The sections: the material chunk, the section chunk (-1 on the legacy
  // path), the vertex and the index chunk of each.
  struct Sect {
    uint32_t mat;
    int sec;
    int v;
    int x;
  };
  std::vector<Sect> sects;
  if (boot940) {
    int cur_mat = -1;
    Sect pend{0, -1, -1, -1};
    bool open = false;
    for (uint32_t j = 0; j < nchunk; ++j) {
      const uint32_t ty = chunks[j].typ;
      if (ty == 0) {
        cur_mat = static_cast<int>(j);
        open = false;
      } else if (ty >= 1 && ty <= 3) {
        open = cur_mat >= 0;
        pend = Sect{static_cast<uint32_t>(cur_mat < 0 ? 0 : cur_mat),
                    static_cast<int>(j), -1, -1};
      } else if (open && ty == 4 && pend.v < 0) {
        pend.v = static_cast<int>(j);
      } else if (open && ty == 5 && pend.x < 0) {
        pend.x = static_cast<int>(j);
      }
      if (open && pend.v >= 0 && pend.x >= 0) {
        sects.push_back(pend);
        open = false;
      }
    }
  } else {
    for (uint32_t i = 0; i < nchunk; ++i) {
      if (chunks[i].typ != 1) continue;
      // Find verts(5) and indices(0) after this material.
      int v_i = -1;
      int x_i = -1;
      for (uint32_t j = i + 1; j < nchunk; ++j) {
        if (chunks[j].typ == 1) break;
        if (chunks[j].typ == 5 && v_i < 0) v_i = static_cast<int>(j);
        if (chunks[j].typ == 0 && x_i < 0) x_i = static_cast<int>(j);
      }
      if (v_i < 0 || x_i < 0) continue;
      sects.push_back(Sect{i, -1, v_i, x_i});
    }
  }

  out.subs.clear();
  bool bounds_init = false;

  for (const Sect& sc : sects) {
    const uint32_t i = sc.mat;
    const int v_i = sc.v;
    const int x_i = sc.x;

    const uint32_t voff = chunks[v_i].off;
    const uint32_t vend = chunk_end(static_cast<uint32_t>(v_i));
    const uint32_t ioff = chunks[x_i].off;
    const uint32_t iend = chunk_end(static_cast<uint32_t>(x_i));
    if (voff + 16 > vend || ioff + 12 > iend) continue;

    uint32_t vtag = 0, vsize = 0, vcount = 0, vflags = 0;
    if (!read_u32(data, size, voff, &vtag) ||
        !read_u32(data, size, voff + 4, &vsize) ||
        !read_u32(data, size, voff + 8, &vcount) ||
        !read_u32(data, size, voff + 12, &vflags))
      continue;
    (void)vtag;
    if (vcount == 0 || vcount > 500000) continue;
    // Phase 2.44: stride from chunk payload — 0x0241→32 (pos+n+uv),
    // 0x0041→24 (pos+n, glass). Fall back to 32.
    uint32_t stride = 32;
    uint32_t file_stride = 0;  // (chunk size - 16) / count, FUN_00454940
    if (vsize >= 16) {
      const uint32_t payload = vsize - 16;
      if (payload % vcount == 0) {
        const uint32_t s = payload / vcount;
        file_stride = s;
        if (s == 24 || s == 32 || s == 36 || s == 40) stride = s;
      }
    }
    // PE 940 vertex chunk reader FUN_00454940: the components follow each
    // other in this order, each present when its flag bit is set:
    //   0x001 position (3 floats); blend weights 0x004 (1 float), else
    //   0x008 (2), else 0x010 (3); 0x020 blend indices (dword); 0x040
    //   normal (3 floats); 0x080 first colour (dword, D3DFVF_DIFFUSE
    //   through FUN_00455210, alpha forced to 0xFF); 0x100 second colour
    //   (dword, D3DFVF_SPECULAR); then one uv pair per bit 0x200 .. 0x8000.
    // Census of the Build 940 install: the sum of these sizes equals the
    // chunk stride for all 4365 vertex chunks (19 flag combinations).
    // MeshVertex keeps position, normal and one uv pair (the first set of
    // the file; on the Build 940 boot the set of the primary texture entry,
    // see below). Before the flag walk the uv was read at +24 whatever the
    // flags (the colour pair for 0x3c1 / 0x7c1, the prelit world meshes)
    // and strides 44 / 48 / 56 were read with stride 32.
    // The colours and the other uv sets go into the wide array
    // (MeshVertexWide), filled further down. The first colour is 0xFFFFFFFF
    // in every chunk of the install that has it; the second carries the
    // baked lighting of prelit sections and reaches the exe's stage setups
    // as D3DTA_SPECULAR (material mode 3).
    // Fork: a layout without normals (physics hulls, flags 0x001, stride
    // 12) stays unloaded as before.
    int pos_off = 0;
    int nrm_off = stride >= 24 ? 12 : -1;  // legacy layout guess
    int uv_off = stride >= 32 ? 24 : -1;
    int weight_off = -1;  // the first blend weight (flags 0x4 / 0x8 / 0x10)
    // Only when the flag walk matches the stride: the two colours and the
    // start of the uv area. FUN_00454940 addresses a uv set by channel:
    // local_60[k] = the summed size of the sets below k that the block has
    // (bit 0x200 << j), so a channel whose own set is absent reads the next
    // one that is there.
    bool walk_ok = false;
    int cola_off = -1, colb_off = -1, uvbase_off = -1;
    int uv_sets = 0;        // number of uv pairs in the block
    int chan_off[8] = {};   // offset of channel k inside the uv area
    {
      uint32_t o = 0;
      int f_pos = -1, f_nrm = -1, f_uv = -1, f_cola = -1, f_colb = -1;
      if (vflags & 0x001u) { f_pos = static_cast<int>(o); o += 12; }
      const int f_weight = (vflags & 0x01cu) ? static_cast<int>(o) : -1;
      if (vflags & 0x004u) o += 4;
      else if (vflags & 0x008u) o += 8;
      else if (vflags & 0x010u) o += 12;
      if (vflags & 0x020u) o += 4;
      if (vflags & 0x040u) { f_nrm = static_cast<int>(o); o += 12; }
      if (vflags & 0x080u) { f_cola = static_cast<int>(o); o += 4; }
      if (vflags & 0x100u) { f_colb = static_cast<int>(o); o += 4; }
      const int f_uvbase = static_cast<int>(o);
      int nsets = 0, k = 0;
      for (uint32_t bit = 0x200u; bit <= 0x8000u; bit <<= 1, ++k) {
        chan_off[k] = nsets * 8;
        if (!(vflags & bit)) continue;
        if (f_uv < 0) f_uv = static_cast<int>(o);
        o += 8;
        ++nsets;
      }
      chan_off[7] = nsets * 8;
      if (f_pos == 0 && f_nrm > 0 && file_stride != 0 && o == file_stride) {
        stride = file_stride;
        pos_off = f_pos;
        nrm_off = f_nrm;
        uv_off = f_uv;
        walk_ok = true;
        cola_off = f_cola;
        colb_off = f_colb;
        uvbase_off = f_uvbase;
        uv_sets = nsets;
        weight_off = f_weight;
      }
    }
    const uint64_t need =
        static_cast<uint64_t>(voff) + 16ull +
        static_cast<uint64_t>(vcount) * static_cast<uint64_t>(stride);
    if (need > vend || need > size) continue;

    uint32_t itag = 0, isize = 0, icount = 0;
    if (!read_u32(data, size, ioff, &itag) ||
        !read_u32(data, size, ioff + 4, &isize) ||
        !read_u32(data, size, ioff + 8, &icount))
      continue;
    (void)itag;
    (void)isize;
    if (icount < 3 || icount > 1500000 || (icount % 3) != 0) continue;
    if (static_cast<uint64_t>(ioff) + 12ull +
            static_cast<uint64_t>(icount) * 2ull >
        iend)
      continue;

    MeshSubmesh sm;
    sm.name = scx_mat_name(data, size, chunks[i].off, chunk_end(i));
    sm.diffuse = scx_mat_diffuse(data, size, chunks[i].off, chunk_end(i));
    scx_v4_material(data, size, chunks[i].off, chunk_end(i), &sm);
    sm.verts.resize(vcount);
    const uint8_t* vsrc = data + voff + 16;
    if (sc.sec >= 0) {
      // The section chunk (FUN_00452960 cases 1 to 3): section vt+0x3C adds
      // a bone index, vt+0x40 sets the bone mode.
      const uint32_t soff = chunks[static_cast<size_t>(sc.sec)].off;
      const uint32_t send = chunk_end(static_cast<uint32_t>(sc.sec));
      const uint32_t sty = chunks[static_cast<size_t>(sc.sec)].typ;
      uint32_t a = 0, b = 0;
      if (sty == 2 && soff + 20 <= send && read_u32(data, size, soff + 12, &a) &&
          read_u32(data, size, soff + 16, &b)) {
        sm.bone_mode = 1;
        sm.bone_idx.push_back(static_cast<int32_t>(a));
        sm.bone_idx.push_back(static_cast<int32_t>(b));
      } else if (sty == 3 && soff + 16 <= send &&
                 read_u32(data, size, soff + 12, &a)) {
        sm.bone_mode = -1;
        for (uint32_t k = 0; k < a && k < 256; ++k) {
          if (soff + 20 + k * 4 > send ||
              !read_u32(data, size, soff + 16 + k * 4, &b))
            break;
          sm.bone_idx.push_back(static_cast<int32_t>(b));
        }
      }
      if (sm.bone_mode != 0 && walk_ok && weight_off >= 0) {
        sm.weights.resize(vcount);
        for (uint32_t vi = 0; vi < vcount; ++vi)
          std::memcpy(&sm.weights[vi],
                      vsrc + static_cast<size_t>(vi) * stride + weight_off, 4);
      }
    }
    for (uint32_t vi = 0; vi < vcount; ++vi) {
      MeshVertex& v = sm.verts[vi];
      const uint8_t* p = vsrc + static_cast<size_t>(vi) * stride;
      std::memcpy(&v.x, p + pos_off, 12);
      if (nrm_off >= 0)
        std::memcpy(&v.nx, p + nrm_off, 12);
      else
        v.nx = v.ny = v.nz = 0.f;
      if (uv_off >= 0)
        std::memcpy(&v.u, p + uv_off, 8);
      else
        v.u = v.v = 0.f;
      // Drop insane/NaN positions (rare bad verts in light meshes).
      if (!(v.x == v.x) || !(v.y == v.y) || !(v.z == v.z) ||
          std::fabs(v.x) > 1.0e6f || std::fabs(v.y) > 1.0e6f ||
          std::fabs(v.z) > 1.0e6f) {
        v.x = v.y = v.z = 0.f;
        v.nx = 0.f;
        v.ny = 1.f;
        v.nz = 0.f;
      }
    }
    // PE 940 FUN_00454940, the texcoord sets: the material gives its
    // stages (material vt+8: per stage the uv channel, scaleU, scaleV,
    // offsetU, offsetV of the stage's texture entry) and the loader writes
    //   set i = (((u - 0.5) - offsetU) * scaleU + 0.5,
    //            0.5 - ((0.5 - v) - offsetV) * scaleV)
    // from uv[channel of stage i]; the D3D diffuse is the first colour OR
    // 0xFF000000 and the D3D specular the second colour (each 0xFFFFFFFF
    // when the block has none).
    // Host: the wide array holds one set per texture entry in a fixed
    // order - 0 primary, 1 second, 2 lightmap - and the entry's channel is
    // rewritten to that index (draw_mode3_section selects the set of a
    // stage with D3DTSS_TEXCOORDINDEX = the channel). That is the exe's
    // layout up to the order of the sets, and it is what lets two entries
    // put different transforms on one file channel.
    // Fork: a channel outside the block reads (0, 0) (the exe reads past
    // the vertex); channel -1, the planar map from x and -z, is not ported
    // (no primary, second or lightmap entry of the install has it).
    // The array is made when the block has a colour or more than one uv
    // set, or the material has a lightmap entry.
    // Build 940 boot: also when the second entry is on another channel or
    // has another uv transform than the primary; the MixTA classes sample
    // it through texcoord set 1 (render_d3d9_part6.inc).
    const bool second_own_set =
        boot940 && sm.has_mat && sm.slot_second != -1 &&
        (sm.chan_second != sm.chan_primary ||
         std::memcmp(sm.uvx_second, sm.uvx_primary, sizeof(sm.uvx_second)) != 0);
    const bool want_wide =
        walk_ok && ((vflags & 0x180u) != 0 || uv_sets > 1 ||
                    (sm.has_mat && sm.slot_lightmap != -1) || second_own_set);
    struct StageUv {
      bool on = false;
      int off = -1;  // offset of the pair in the vertex, -1 = (0, 0)
      float xf[4] = {1.f, 1.f, 0.f, 0.f};
    };
    auto stage_of = [&](bool on, int16_t chan, const float* xf) {
      StageUv s;
      s.on = on;
      if (!on) return s;
      if (chan >= 0 && chan <= 7 && uvbase_off >= 0 &&
          static_cast<uint32_t>(uvbase_off + chan_off[chan] + 8) <= stride)
        s.off = uvbase_off + chan_off[chan];
      if (xf) std::memcpy(s.xf, xf, sizeof(s.xf));
      return s;
    };
    auto stage_uv = [](const StageUv& s, const uint8_t* p, float* out) {
      float uv[2] = {0.f, 0.f};
      if (s.off >= 0) std::memcpy(uv, p + s.off, 8);
      out[0] = ((uv[0] - 0.5f) - s.xf[2]) * s.xf[0] + 0.5f;
      out[1] = 0.5f - ((0.5f - uv[1]) - s.xf[3]) * s.xf[1];
      if (!(out[0] == out[0]) || !(out[1] == out[1])) out[0] = out[1] = 0.f;
    };
    auto uvx_identity = [](const float* xf) {
      return xf[0] == 1.f && xf[1] == 1.f && xf[2] == 0.f && xf[3] == 0.f;
    };
    // A material that was not parsed has no entries: its one set is the
    // first of the file, unchanged.
    const StageUv st_primary =
        sm.has_mat ? stage_of(true, sm.chan_primary, sm.uvx_primary)
                   : stage_of(uv_sets > 0, 0, nullptr);
    const StageUv st_second =
        stage_of(sm.has_mat && sm.slot_second != -1, sm.chan_second,
                 sm.uvx_second);
    const StageUv st_lightmap =
        stage_of(sm.has_mat && sm.slot_lightmap != -1, sm.chan_lightmap,
                 sm.uvx_lightmap);
    if (want_wide) {
      sm.wide.resize(vcount);
      for (uint32_t vi = 0; vi < vcount; ++vi) {
        const uint8_t* p = vsrc + static_cast<size_t>(vi) * stride;
        const MeshVertex& v = sm.verts[vi];  // position already checked
        MeshVertexWide& w = sm.wide[vi];
        w.x = v.x;
        w.y = v.y;
        w.z = v.z;
        w.nx = v.nx;
        w.ny = v.ny;
        w.nz = v.nz;
        uint32_t ca = 0xFFFFFFFFu, cb = 0xFFFFFFFFu;
        if (cola_off >= 0) std::memcpy(&ca, p + cola_off, 4);
        if (colb_off >= 0) std::memcpy(&cb, p + colb_off, 4);
        w.diffuse = ca | 0xFF000000u;
        w.specular = cb;
        w.uv[0][0] = w.uv[0][1] = 0.f;
        w.uv[1][0] = w.uv[1][1] = 0.f;
        w.uv[2][0] = w.uv[2][1] = 0.f;
        if (st_primary.on) stage_uv(st_primary, p, w.uv[0]);
        if (st_second.on) stage_uv(st_second, p, w.uv[1]);
        if (st_lightmap.on) stage_uv(st_lightmap, p, w.uv[2]);
      }
    }
    // Stage 0 of CMaterial_Tex and CMaterial_MixVCA is the primary entry
    // (material vt+8: FUN_0042c840 / FUN_0042c750), and the object modes
    // sample it with texcoord set 0: the one uv pair of MeshVertex is that
    // entry's set. With channel 0 and no scale or offset it is the first
    // set of the file, which is what the host read before.
    // Fork: the stage lists of the other classes were not read; the host
    // draws their primary texture the same way. Any other channel or a
    // transform is applied on the Build 940 boot only (the legacy path
    // mirrors the old build's loader, whose handling of the entry was not
    // read).
    if (walk_ok && sm.has_mat && render_boot940() &&
        (sm.chan_primary != 0 || !uvx_identity(sm.uvx_primary))) {
      for (uint32_t vi = 0; vi < vcount; ++vi) {
        float uv[2] = {0.f, 0.f};
        stage_uv(st_primary, vsrc + static_cast<size_t>(vi) * stride, uv);
        sm.verts[vi].u = uv[0];
        sm.verts[vi].v = uv[1];
      }
    }
    if (want_wide && sm.has_mat) {
      // The entries now name the sets of the wide array.
      sm.chan_primary = 0;
      if (sm.slot_second != -1) sm.chan_second = 1;
      if (sm.slot_lightmap != -1) sm.chan_lightmap = 2;
    }

    sm.indices.resize(icount);
    std::memcpy(sm.indices.data(), data + ioff + 12, icount * 2);

    uint16_t max_i = 0;
    for (uint16_t idx : sm.indices) {
      if (idx > max_i) max_i = idx;
    }
    if (max_i >= vcount) continue;

    for (const MeshVertex& v : sm.verts) {
      if (!bounds_init) {
        out.bmin[0] = out.bmax[0] = v.x;
        out.bmin[1] = out.bmax[1] = v.y;
        out.bmin[2] = out.bmax[2] = v.z;
        bounds_init = true;
      } else {
        if (v.x < out.bmin[0]) out.bmin[0] = v.x;
        if (v.y < out.bmin[1]) out.bmin[1] = v.y;
        if (v.z < out.bmin[2]) out.bmin[2] = v.z;
        if (v.x > out.bmax[0]) out.bmax[0] = v.x;
        if (v.y > out.bmax[1]) out.bmax[1] = v.y;
        if (v.z > out.bmax[2]) out.bmax[2] = v.z;
      }
    }
    out.subs.push_back(std::move(sm));
  }

  out.ready = !out.subs.empty();
  return out.ready;
}

// Phase 2.51/2.54 — INVO v3 visual mesh (skydome / older parts).
// File: INVO + ver=3 + mat blocks. Each mat block (no magic after the first):
//   +0x00 hdr_size (≥0x80), +0x04 floats, +0x68 name, +0x88 vcount,
//   +0x8C verts stride 64 (pos+n+uv in first 32), then u32 tri_count +
//   tri_count×3 uint32 indices. Phase 2.54: loop subsequent mat blocks.
// Fonts use hdr=0x38 — skip.
// `caller_flags`: the create flags FUN_00451b00 gives every material of the
// mesh (0x80000000 when the mesh path contains "objects\meshes", else 0).
bool parse_scx_v3(const uint8_t* data, size_t size, MeshState& out,
                  uint32_t caller_flags) {
  if (!data || size < 0xA0) return false;
  if (std::memcmp(data, "INVO", 4) != 0) return false;
  uint32_t ver = 0, hdr0 = 0;
  if (!read_u32(data, size, 4, &ver) || !read_u32(data, size, 8, &hdr0))
    return false;
  if (ver != 3 || hdr0 < 0x80u || hdr0 > 0x200u) return false;

  constexpr uint32_t kStride = 64;
  out.subs.clear();
  bool bounds_init = false;
  size_t mat_base = 0x08;  // first block shares file header after ver

  auto chan = [](float c) -> uint32_t {
    if (!(c == c)) c = 0.5f;
    if (c < 0.f) c = 0.f;
    if (c > 1.f) c = 1.f;
    return static_cast<uint32_t>(c * 255.f + 0.5f);
  };

  while (mat_base + 0x8C < size) {
    uint32_t hsz = 0;
    if (!read_u32(data, size, mat_base, &hsz)) break;
    if (hsz < 0x80u || hsz > 0x200u) break;

    uint32_t vcount = 0;
    if (!read_u32(data, size, mat_base + 0x88, &vcount)) break;
    if (vcount == 0 || vcount > 200000 || vcount > 65535u) break;
    const size_t vstart = mat_base + 0x8C;
    const uint64_t vend =
        static_cast<uint64_t>(vstart) +
        static_cast<uint64_t>(vcount) * static_cast<uint64_t>(kStride);
    if (vend + 4ull > size) break;

    uint32_t tri_count = 0;
    if (!read_u32(data, size, static_cast<size_t>(vend), &tri_count)) break;
    if (tri_count == 0 || tri_count > 500000) break;
    const uint64_t icount = static_cast<uint64_t>(tri_count) * 3ull;
    const uint64_t istart = vend + 4ull;
    if (istart + icount * 4ull > size) break;

    // Sanity: a few verts must be finite before committing.
    bool verts_ok = true;
    for (uint32_t vi = 0; vi < vcount && vi < 8u; ++vi) {
      float x = 0, y = 0, z = 0;
      std::memcpy(&x, data + vstart + vi * kStride, 4);
      std::memcpy(&y, data + vstart + vi * kStride + 4, 4);
      std::memcpy(&z, data + vstart + vi * kStride + 8, 4);
      if (!(x == x) || !(y == y) || !(z == z) || std::fabs(x) > 1.0e6f ||
          std::fabs(y) > 1.0e6f || std::fabs(z) > 1.0e6f) {
        verts_ok = false;
        break;
      }
    }
    if (!verts_ok) break;

    MeshSubmesh sm;
    const size_t nstart = mat_base + 0x68;
    const size_t nlim = mat_base + hsz < size ? mat_base + hsz : size;
    size_t nend = nstart;
    while (nend < nlim && data[nend] != 0) ++nend;
    if (nend > nstart)
      sm.name.assign(reinterpret_cast<const char*>(data + nstart), nend - nstart);

    if (mat_base + 0x10 <= size) {
      float r = 0, g = 0, b = 0;
      std::memcpy(&r, data + mat_base + 0x04, 4);
      std::memcpy(&g, data + mat_base + 0x08, 4);
      std::memcpy(&b, data + mat_base + 0x0C, 4);
      sm.diffuse = 0xFF000000u | (chan(r) << 16) | (chan(g) << 8) | chan(b);
    }
    // The slots and uv channels of the header's texture entries are kept
    // by scx_v3_material (primary +0x2C on the channel at +0x46, second
    // +0x34 on +0x48, env +0x32, lightmap +0x38 on channel 1), so a render
    // object binds a v3 mesh by slot like a v4 one.
    scx_v3_material(data, size, mat_base, hsz, caller_flags, &sm);

    // PE 940 v1..v3 loader FUN_00451b00: every block becomes a stream of
    // format 0x15003f. From the 64-byte version 3 vertex: position +0,
    // normal +12, uv0 +24, uv1 +32, colour +40 (two dwords +44 / +48), uv2
    // +52, a second colour +60 that header flag 4 selects. A material that
    // is not prelit gets D3D diffuse = colour | 0xFF000000 and specular 0;
    // a prelit one gets diffuse 0xFF808080 and specular = colour |
    // 0xFF000000 (or the dword at +60 with header flag 4). Texcoord set i
    // = uv[channel of stage i], no scale or offset.
    // Fork: the host keeps position, normal and uv0 only. The wide array
    // (colours, uv1, uv2) is not filled for a v3 mesh: no v3 mesh of the
    // first garage has a lightmap entry or a primary entry off channel 0
    // (the lift is lit and its five textured sections use uv0; the sky dome
    // skydome.SCX carries 0xFFFFFFFF in all 376 colours, so its unlit
    // texture * diffuse is the texture). A prelit v3 object with a lightmap
    // therefore stays on the object path until this is filled.
    sm.verts.resize(vcount);
    for (uint32_t vi = 0; vi < vcount; ++vi) {
      MeshVertex& v = sm.verts[vi];
      const uint8_t* p = data + vstart + static_cast<size_t>(vi) * kStride;
      std::memcpy(&v.x, p, 12);
      std::memcpy(&v.nx, p + 12, 12);
      std::memcpy(&v.u, p + 24, 8);
      if (!(v.x == v.x) || !(v.y == v.y) || !(v.z == v.z) ||
          std::fabs(v.x) > 1.0e6f || std::fabs(v.y) > 1.0e6f ||
          std::fabs(v.z) > 1.0e6f) {
        v.x = v.y = v.z = 0.f;
        v.nx = 0.f;
        v.ny = 1.f;
        v.nz = 0.f;
      }
      if (!bounds_init) {
        out.bmin[0] = out.bmax[0] = v.x;
        out.bmin[1] = out.bmax[1] = v.y;
        out.bmin[2] = out.bmax[2] = v.z;
        bounds_init = true;
      } else {
        if (v.x < out.bmin[0]) out.bmin[0] = v.x;
        if (v.y < out.bmin[1]) out.bmin[1] = v.y;
        if (v.z < out.bmin[2]) out.bmin[2] = v.z;
        if (v.x > out.bmax[0]) out.bmax[0] = v.x;
        if (v.y > out.bmax[1]) out.bmax[1] = v.y;
        if (v.z > out.bmax[2]) out.bmax[2] = v.z;
      }
    }

    // PE 940 FUN_00451b00, the texcoord sets (Build 940 boot only): set i =
    // uv[channel of the entry the class lists as stage i], with uv0 at +24,
    // uv1 at +32 and uv2 at +52 of the 64-byte vertex. The one set of
    // MeshVertex is the primary entry's (channel = the short at +0x46 of
    // the header). A class that lists the second entry too
    // (scx_shader_two_sets) with that entry on another channel gets the
    // wide array: set 0 primary, set 1 second, set 2 uv1 (the lightmap
    // channel); the channels of the entries then name those sets, as for a
    // v4 block. Colours as the comment above has them.
    // Fork: a channel that is not 0, 1 or 2 reads (0, 0).
    if (render_boot940() && sm.has_mat) {
      auto chan_off = [](int ch) {
        return ch == 0 ? 24 : (ch == 1 ? 32 : (ch == 2 ? 52 : -1));
      };
      const int off_p = chan_off(sm.chan_primary);
      if (sm.chan_primary != 0 && off_p >= 0) {
        for (uint32_t vi = 0; vi < vcount; ++vi)
          std::memcpy(&sm.verts[vi].u,
                      data + vstart + static_cast<size_t>(vi) * kStride + off_p,
                      8);
      }
      if (scx_shader_two_sets(static_cast<uint32_t>(sm.mat_shader)) &&
          sm.slot_second != -1 && sm.chan_second != sm.chan_primary) {
        const int off_s = chan_off(sm.chan_second);
        const bool prelit = (sm.mat_flags & 0x80000000u) != 0;
        uint32_t f28 = 0;
        std::memcpy(&f28, data + mat_base + 0x28, 4);
        sm.wide.resize(vcount);
        for (uint32_t vi = 0; vi < vcount; ++vi) {
          const uint8_t* p = data + vstart + static_cast<size_t>(vi) * kStride;
          const MeshVertex& v = sm.verts[vi];
          MeshVertexWide& w = sm.wide[vi];
          w.x = v.x;
          w.y = v.y;
          w.z = v.z;
          w.nx = v.nx;
          w.ny = v.ny;
          w.nz = v.nz;
          uint32_t c = 0, c60 = 0;
          std::memcpy(&c, p + 40, 4);
          std::memcpy(&c60, p + 60, 4);
          if (!prelit) {
            w.diffuse = c | 0xFF000000u;
            w.specular = 0;
          } else {
            w.diffuse = 0xFF808080u;
            w.specular = (f28 & 4u) ? c60 : (c | 0xFF000000u);
          }
          w.uv[0][0] = v.u;
          w.uv[0][1] = v.v;
          w.uv[1][0] = w.uv[1][1] = 0.f;
          if (off_s >= 0) std::memcpy(w.uv[1], p + off_s, 8);
          std::memcpy(w.uv[2], p + 32, 8);
        }
        sm.chan_primary = 0;
        sm.chan_second = 1;
        if (sm.slot_lightmap != -1) sm.chan_lightmap = 2;
      }
    }

    sm.indices.resize(static_cast<size_t>(icount));
    bool idx_ok = true;
    for (uint64_t i = 0; i < icount; ++i) {
      uint32_t idx = 0;
      if (!read_u32(data, size, static_cast<size_t>(istart + i * 4ull), &idx) ||
          idx >= vcount) {
        idx_ok = false;
        break;
      }
      sm.indices[static_cast<size_t>(i)] = static_cast<uint16_t>(idx);
    }
    if (!idx_ok) break;

    out.subs.push_back(std::move(sm));
    mat_base = static_cast<size_t>(istart + icount * 4ull);
    while (mat_base + 4 <= size) {
      uint32_t z = 0;
      if (!read_u32(data, size, mat_base, &z) || z != 0) break;
      mat_base += 4;
    }
  }

  out.ready = !out.subs.empty();
  return out.ready;
}

}  // namespace

bool render_d3d9_mesh_create_from_memory(void* key, const uint8_t* data,
                                         size_t size, const char* label,
                                         uint32_t v3_create_flags) {
  if (!key || !data || size < 16) return false;
  {
    auto it = g_meshes.find(key);
    if (it != g_meshes.end()) release_mesh_textures(key, it->second);
  }
  g_mesh_geom_src.erase(key);  // Fork: own geometry from now on
  MeshState st;
  st.label = label ? label : "";
  if (!parse_scx_v4(data, size, st) &&
      !parse_scx_v3(data, size, st, v3_create_flags)) {
    g_meshes.erase(key);
    return false;
  }
  st.geom_rev = ++g_mesh_geom_rev;
  bind_mesh_textures(key, st);
  {
    auto old = g_meshes.find(key);
    st.seq = (old != g_meshes.end() && old->second.seq) ? old->second.seq : ++g_mesh_seq;
  }
  g_meshes[key] = std::move(st);
  return true;
}

bool render_d3d9_mesh_create_from_file(void* key, const char* path,
                                       uint32_t v3_create_flags) {
  if (!key || !path || !path[0]) return false;
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::string resolved = rpak_resolve_path(path);
    if (!resolved.empty()) f = std::fopen(resolved.c_str(), "rb");
  }
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 64 * 1024 * 1024) {
    std::fclose(f);
    return false;
  }
  std::vector<uint8_t> buf(static_cast<size_t>(sz));
  if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
    std::fclose(f);
    return false;
  }
  std::fclose(f);
  // Prefer the path we actually opened so texture-neighbour resolution works.
  std::string label = path;
  std::string resolved = rpak_resolve_path(path);
  if (!resolved.empty()) label = resolved;
  return render_d3d9_mesh_create_from_memory(key, buf.data(), buf.size(),
                                             label.c_str(), v3_create_flags);
}

bool render_d3d9_mesh_create_skydome(void* key, float radius) {
  if (!key || radius <= 0.f) return false;
  {
    auto it = g_meshes.find(key);
    if (it != g_meshes.end()) release_mesh_textures(key, it->second);
  }
  g_mesh_geom_src.erase(key);  // Fork: own geometry from now on
  constexpr int kStacks = 16;
  constexpr int kSlices = 32;
  constexpr float kPi = 3.14159265f;
  MeshSubmesh sm;
  sm.name = "skydome";
  sm.diffuse = 0xFF6EC1FFu;  // daytime sky tint until PTX bind
  sm.verts.reserve(static_cast<size_t>((kStacks + 1) * (kSlices + 1)));
  for (int lat = 0; lat <= kStacks; ++lat) {
    const float v = static_cast<float>(lat) / static_cast<float>(kStacks);
    const float phi = v * kPi;  // 0..pi
    const float y = std::cos(phi);
    const float r = std::sin(phi);
    for (int lon = 0; lon <= kSlices; ++lon) {
      const float u = static_cast<float>(lon) / static_cast<float>(kSlices);
      const float theta = u * 2.f * kPi;
      const float x = r * std::cos(theta);
      const float z = r * std::sin(theta);
      MeshVertex vtx;
      vtx.x = x * radius;
      vtx.y = y * radius;
      vtx.z = z * radius;
      // Inward normals (viewed from inside the dome).
      vtx.nx = -x;
      vtx.ny = -y;
      vtx.nz = -z;
      vtx.u = u;
      vtx.v = v;
      sm.verts.push_back(vtx);
    }
  }
  for (int lat = 0; lat < kStacks; ++lat) {
    for (int lon = 0; lon < kSlices; ++lon) {
      const uint16_t i0 =
          static_cast<uint16_t>(lat * (kSlices + 1) + lon);
      const uint16_t i1 = static_cast<uint16_t>(i0 + 1);
      const uint16_t i2 =
          static_cast<uint16_t>((lat + 1) * (kSlices + 1) + lon);
      const uint16_t i3 = static_cast<uint16_t>(i2 + 1);
      sm.indices.push_back(i0);
      sm.indices.push_back(i2);
      sm.indices.push_back(i1);
      sm.indices.push_back(i1);
      sm.indices.push_back(i2);
      sm.indices.push_back(i3);
    }
  }
  MeshState st;
  st.label = "skydome_proc";
  st.bmin[0] = st.bmin[1] = st.bmin[2] = -radius;
  st.bmax[0] = st.bmax[1] = st.bmax[2] = radius;
  st.subs.push_back(std::move(sm));
  st.ready = true;
  g_meshes[key] = std::move(st);
  return true;
}

void render_d3d9_mesh_set_geometry_source(void* dst, void* src) {
  if (!dst) return;
  if (!src || src == dst)
    g_mesh_geom_src.erase(dst);
  else
    g_mesh_geom_src[dst] = src;
}

void render_d3d9_mesh_set_type_flags(void* key, uint32_t flags) {
  if (!key) return;
  if (flags)
    g_mesh_type_flags[key] = flags;
  else
    g_mesh_type_flags.erase(key);
}

uint32_t render_d3d9_mesh_type_flags(void* key) {
  auto it = g_mesh_type_flags.find(key);
  return it == g_mesh_type_flags.end() ? 0u : it->second;
}

void render_d3d9_mesh_set_pending_type(void* key, int32_t type_id) {
  if (!key) return;
  if (!type_id) {
    g_mesh_pending.erase(key);
    return;
  }
  MeshPending& p = g_mesh_pending[key];
  p.type_id = type_id;
  p.state = 0;
  p.seq = ++g_mesh_pending_seq;
}

int32_t render_d3d9_mesh_pending_type(void* key) {
  auto it = g_mesh_pending.find(key);
  return it == g_mesh_pending.end() ? 0 : it->second.type_id;
}

bool render_d3d9_mesh_pending_waiting(void* key) {
  auto it = g_mesh_pending.find(key);
  return it != g_mesh_pending.end() && it->second.state == 0;
}

namespace {
void scale_mesh_state(MeshState& st, float sx, float sy, float sz);
}  // namespace

bool render_d3d9_mesh_scale_vertices(void* key, float sx, float sy, float sz) {
  if (!key) return false;
  auto it = g_meshes.find(key);
  if (it == g_meshes.end() || !it->second.ready) return false;
  scale_mesh_state(it->second, sx, sy, sz);
  // Fork: the exe scales the mesh sections in place (PE 940 FUN_004d8710:
  // section vt+0x4c per section) and every instance of every render type
  // using that mesh shows it. Host: scale the copies that lead back to it.
  for (const auto& kv : g_mesh_geom_src) {
    bool hit = false;
    void* cur = kv.second;
    for (int guard = 0; cur && guard < 8; ++guard) {
      if (cur == key) {
        hit = true;
        break;
      }
      auto up = g_mesh_geom_src.find(cur);
      cur = up == g_mesh_geom_src.end() ? nullptr : up->second;
    }
    if (!hit) continue;
    auto mit = g_meshes.find(kv.first);
    if (mit != g_meshes.end() && mit->second.ready)
      scale_mesh_state(mit->second, sx, sy, sz);
  }
  return true;
}

namespace {
void scale_mesh_state(MeshState& st, float sx, float sy, float sz) {
  bool any = false;
  float bmin[3] = {1e30f, 1e30f, 1e30f};
  float bmax[3] = {-1e30f, -1e30f, -1e30f};
  for (MeshSubmesh& sm : st.subs) {
    for (MeshVertex& v : sm.verts) {
      v.x *= sx;
      v.y *= sy;
      v.z *= sz;
      if (v.x < bmin[0]) bmin[0] = v.x;
      if (v.y < bmin[1]) bmin[1] = v.y;
      if (v.z < bmin[2]) bmin[2] = v.z;
      if (v.x > bmax[0]) bmax[0] = v.x;
      if (v.y > bmax[1]) bmax[1] = v.y;
      if (v.z > bmax[2]) bmax[2] = v.z;
      any = true;
    }
    for (MeshVertexWide& w : sm.wide) {  // the same vertices, all streams
      w.x *= sx;
      w.y *= sy;
      w.z *= sz;
    }
  }
  if (any) {
    for (int i = 0; i < 3; ++i) {
      st.bmin[i] = bmin[i];
      st.bmax[i] = bmax[i];
    }
  }
  // Other vertices from now on: the static copies made for the old ones
  // (shadow_geom, render_d3d9_part7.inc) are not this mesh's any more.
  st.geom_rev = ++g_mesh_geom_rev;
}
}  // namespace

bool render_d3d9_mesh_clone(void* dst, void* src) {
  if (!dst || !src || dst == src) return false;
  auto it = g_meshes.find(src);
  if (it == g_meshes.end() || !it->second.ready) return false;
  {
    auto dit = g_meshes.find(dst);
    if (dit != g_meshes.end()) release_mesh_textures(dst, dit->second);
  }
  // Fork: an independent copy unless the caller links it again
  // (render_d3d9_mesh_set_geometry_source).
  g_mesh_geom_src.erase(dst);
  MeshState st = it->second;
  for (size_t i = 0; i < st.subs.size(); ++i) {
    if (st.subs[i].texture_key == mesh_sub_tex_key(src, i))
      st.subs[i].texture_key = nullptr;
  }
  bind_mesh_textures(dst, st);
  {
    // The instance keeps its stamp across changeResource re-clones.
    auto old = g_meshes.find(dst);
    st.seq = (old != g_meshes.end() && old->second.seq) ? old->second.seq : ++g_mesh_seq;
  }
  g_meshes[dst] = std::move(st);
  return true;
}

void render_d3d9_mesh_set_texture_at(void* mesh_key, int32_t submesh,
                                     void* texture_key) {
  if (!mesh_key) return;
  auto it = g_meshes.find(mesh_key);
  if (it == g_meshes.end() || submesh < 0 ||
      static_cast<size_t>(submesh) >= it->second.subs.size())
    return;
  it->second.subs[static_cast<size_t>(submesh)].texture_key = texture_key;
}

void render_d3d9_mesh_set_texture(void* mesh_key, void* texture_key) {
  render_d3d9_mesh_set_texture_at(mesh_key, 0, texture_key);
}

void render_d3d9_mesh_set_color(void* key, int32_t argb) {
  if (!key) return;
  MeshXform& xf = g_mesh_xforms[key];
  xf.color = argb;
  xf.color_set = 1;
}

int32_t render_d3d9_mesh_get_color(void* key) {
  if (!key) return 0;
  auto it = g_mesh_xforms.find(key);
  if (it == g_mesh_xforms.end() || !it->second.color_set) return 0;
  return it->second.color;
}

void render_d3d9_mesh_destroy(void* key) {
  if (!key) return;
  auto it = g_meshes.find(key);
  if (it != g_meshes.end()) {
    release_mesh_textures(key, it->second);
    g_meshes.erase(it);
  }
  g_mesh_type_flags.erase(key);
  g_mesh_pending.erase(key);
  g_scene_lights.erase(key);  // a light instance: its cLight goes with it
  // Fork: geometry links from and to this key.
  g_mesh_geom_src.erase(key);
  for (auto git = g_mesh_geom_src.begin(); git != g_mesh_geom_src.end();) {
    if (git->second == key)
      git = g_mesh_geom_src.erase(git);
    else
      ++git;
  }
  g_mesh_xforms.erase(key);
  g_mesh_bones.erase(key);
  g_mesh_bone_alias.erase(key);
  g_mesh_bone_next.erase(key);
  for (size_t i = 0; i < g_mesh_queue.size();) {
    if (g_mesh_queue[i] == key)
      g_mesh_queue.erase(g_mesh_queue.begin() + static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
}

bool render_d3d9_mesh_ready(void* key) {
  auto it = g_meshes.find(key);
  return it != g_meshes.end() && it->second.ready;
}

int32_t render_d3d9_mesh_submesh_count(void* key) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end()) return 0;
  return static_cast<int32_t>(it->second.subs.size());
}

int32_t render_d3d9_mesh_vertex_count(void* key) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end()) return 0;
  int32_t n = 0;
  for (const auto& sm : it->second.subs)
    n += static_cast<int32_t>(sm.verts.size());
  return n;
}

int32_t render_d3d9_mesh_index_count(void* key) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end()) return 0;
  int32_t n = 0;
  for (const auto& sm : it->second.subs)
    n += static_cast<int32_t>(sm.indices.size());
  return n;
}

int32_t render_d3d9_mesh_textured_count(void* key) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end()) return 0;
  int32_t n = 0;
  for (const auto& sm : it->second.subs) {
    if (sm.texture_key && render_d3d9_texture_ready(sm.texture_key)) ++n;
  }
  return n;
}

void* render_d3d9_mesh_get_texture(void* key, int32_t submesh) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end() || submesh < 0 ||
      static_cast<size_t>(submesh) >= it->second.subs.size())
    return nullptr;
  return it->second.subs[static_cast<size_t>(submesh)].texture_key;
}

bool render_d3d9_mesh_local_bounds(void* key, float bmin[3], float bmax[3]) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end() || !it->second.ready || !bmin || !bmax) return false;
  for (int i = 0; i < 3; ++i) {
    bmin[i] = it->second.bmin[i];
    bmax[i] = it->second.bmax[i];
  }
  return true;
}

int32_t render_d3d9_mesh_copy_positions(void* key, float* xyz_interleaved,
                                        int32_t max_count) {
  if (!key || !xyz_interleaved || max_count <= 0) return 0;
  auto it = g_meshes.find(key);
  if (it == g_meshes.end() || !it->second.ready) return 0;
  int32_t n = 0;
  // Stride sample for huge meshes.
  int32_t total = 0;
  for (const auto& sm : it->second.subs)
    total += static_cast<int32_t>(sm.verts.size());
  const int32_t step = total > max_count * 4 ? (total / max_count) : 1;
  int32_t seen = 0;
  for (const auto& sm : it->second.subs) {
    for (const MeshVertex& v : sm.verts) {
      if ((seen++ % step) != 0) continue;
      if (n >= max_count) return n;
      xyz_interleaved[n * 3 + 0] = v.x;
      xyz_interleaved[n * 3 + 1] = v.y;
      xyz_interleaved[n * 3 + 2] = v.z;
      ++n;
    }
  }
  return n;
}

void render_d3d9_mesh_set_transform(void* key, float px, float py, float pz,
                                    float yaw, float pitch, float roll,
                                    float sx, float sy, float sz) {
  if (!key) return;
  MeshXform& xf = g_mesh_xforms[key];
  xf.px = px;
  xf.py = py;
  xf.pz = pz;
  xf.oy = yaw;
  xf.op = pitch;
  xf.or_ = roll;
  xf.sx = sx != 0.f ? sx : 1.f;
  xf.sy = sy != 0.f ? sy : 1.f;
  xf.sz = sz != 0.f ? sz : 1.f;
}
