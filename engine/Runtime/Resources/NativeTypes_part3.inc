// Fragment 3 of NativeTypes.cpp - included, not compiled on its own.
//
// Render objects (typeof 14) and the ground mesh. Mirrors:
//   render_object_desc        FUN_00522d90 (payload of a typeof-14 record)
//   render_template_class     FUN_0046d890 (template class from the flags)
//   render_object_textures    the texture handles of a template (RT_ground
//                             RT+0xC8 / +0xCC / +0xD0, FUN_0046df30)
//   ground_mesh_build         RT_ground init FUN_0046df30 (the mesh is a
//                             material table) + RI_ground ctor FUN_004658b0
//                             (POLY sections -> sections of the mesh) +
//                             FUN_00441c40 (textures by slot)

namespace {

// Guarded by g_nt_mu.
std::unordered_map<int32_t, std::unique_ptr<RenderObjectDesc>> g_render_descs;
// The texture handles of a render object, one per `texture` line; the lines
// that name the same record share one handle. Guarded by g_nt_mu.
std::unordered_map<int32_t, std::vector<InvObject*>> g_render_textures;

// PE 940 FUN_00522d90: the payload is text; FUN_00569350 cuts it into
// lines, the first word of a line selects the handler (`_stricmp`).
void parse_render_object_payload(int32_t id, const std::vector<uint8_t>& blob,
                                 RenderObjectDesc* d) {
  // The exe copies the payload and puts a NUL after it.
  std::vector<uint8_t> buf(blob);
  buf.push_back(0);
  const uint8_t* p = buf.data();
  const uint8_t* const end = buf.data() + buf.size();
  const int32_t pack = rpak_id_pack(id);
  char word[1025];
  std::string line;
  // The exe scans `texture` into one local for every line, so a line whose
  // number does not parse repeats the id of the line before it.
  int32_t tex_local = 0;
  while (p < end) {
    p += read_line(p, end, &line);
    if (line.empty()) continue;
    word[0] = 0;
    engine_sscanf(line.c_str(), "%1024s", word);
    if (word_is(word, "mesh")) {
      // "%s %d" (the scanner takes 0x), FUN_0051e860 (pack-relative id),
      // FUN_0051fa40(id, 5, 0) -> payload+0x0C.
      int32_t local = 0;
      engine_sscanf(line.c_str(), "%1024s %d", word, &local);
      d->mesh_id = local ? static_cast<int32_t>(rpak_remap_local_id(
                               pack, static_cast<uint32_t>(local)))
                         : 0;
    } else if (word_is(word, "flags")) {
      // "%s %d" into payload+0x34: `flags 32768.000` reads 32768, and a
      // second `flags` line replaces the first.
      int32_t v = static_cast<int32_t>(d->flags);
      engine_sscanf(line.c_str(), "%1024s %d", word, &v);
      d->flags = static_cast<uint32_t>(v);
    } else if (word_is(word, "texture")) {
      // "%s %d" into a stack array, in file order; the handles are made
      // after the last line (FUN_0051e860, FUN_0051fa40(id, 7, 0)).
      // `texture 0x00000000` is a slot without a texture.
      engine_sscanf(line.c_str(), "%1024s %d", word, &tex_local);
      // Fork: bounded (the exe's array holds 1024 ids and is not checked).
      if (d->textures.size() < 1024)
        d->textures.push_back(
            tex_local ? static_cast<int32_t>(rpak_remap_local_id(
                            pack, static_cast<uint32_t>(tex_local)))
                      : 0);
    }
    // The class lines are tested for every line, after the chain above.
    // Each sets its class bit in the flags and clears the other two.
    if (word_is(word, "particle"))
      d->flags = (d->flags & 0xffefdfffu) | 0x100u;
    if (word_is(word, "skidmark"))
      d->flags = (d->flags & 0xffeffeffu) | 0x2000u;
    if (word_is(word, "welding"))
      d->flags = (d->flags & 0xffffdeffu) | 0x100000u;
    // shd_texture |= 0x20000, shd_vbase / shd_vup |= 0x40000, shd_center /
    // shd_diru / shd_dirv |= 0x30000 (the projected shadow of the object).
    if (word_is(word, "shd_texture"))
      d->flags |= 0x20000u;
    else if (word_is(word, "shd_vbase") || word_is(word, "shd_vup"))
      d->flags |= 0x40000u;
    else if (word_is(word, "shd_center") || word_is(word, "shd_diru") ||
             word_is(word, "shd_dirv"))
      d->flags |= 0x30000u;
    // Not kept (Fork): numbones, lod_bias, lod_amp, skeleton, the names of
    // the particle / skidmark / welding lines and the shd_* vectors.
  }
}

// The file a mesh record (typeof 5) names: the word after `sourcefile`,
// whatever its extension. "" when the record has none.
std::string mesh_record_source(int32_t mesh_id) {
  std::vector<uint8_t> blob;
  if (!mesh_id || !rpak_read_entry(mesh_id, &blob)) return {};
  blob.push_back(0);
  const uint8_t* p = blob.data();
  const uint8_t* const end = blob.data() + blob.size();
  char word[1025];
  char path[1025];
  std::string line;
  while (p < end) {
    p += read_line(p, end, &line);
    if (line.empty()) continue;
    word[0] = path[0] = 0;
    engine_sscanf(line.c_str(), "%1024s %1024s", word, path);
    if (word_is(word, "sourcefile") && path[0]) return path;
  }
  return {};
}

}  // namespace

const RenderObjectDesc* render_object_desc(int32_t id) {
  static const RenderObjectDesc kNone;
  if (id == 0) return &kNone;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    auto it = g_render_descs.find(id);
    if (it != g_render_descs.end()) return it->second.get();
  }
  const RpakEntry* e = rpak_find_entry(id);
  if (!e || e->restype != 14) return &kNone;
  auto d = std::make_unique<RenderObjectDesc>();
  d->found = true;
  d->id = id;
  std::vector<uint8_t> blob;
  if (rpak_read_entry(id, &blob)) parse_render_object_payload(id, blob, d.get());
  std::lock_guard<std::mutex> lock(g_nt_mu);
  auto& slot = g_render_descs[id];
  if (!slot) slot = std::move(d);
  return slot.get();
}

// PE 940 FUN_0046d890: the first test that hits decides.
int render_template_class(uint32_t flags) {
  if (flags & 0x4u) return kRenderTemplateText;
  if (flags & 0x100u) return kRenderTemplateParticle;
  if (flags & 0x2000u) return kRenderTemplateSkidmark;
  if (flags & 0x100000u) return kRenderTemplateWelding;
  if (flags & 0x800u) return kRenderTemplateGround;
  return kRenderTemplateObject;
}

// The texture handles of a render object, by slot. The exe's template
// requests each texture node and re-binds until all are in (FUN_00468b70 ->
// FUN_0046ddd0, asynchronous).
// Fork: one host handle per distinct record of the render object, bound and
// loaded here at once (ResourceRef.set + ResourceRef.load); they stay
// loaded for the life of the process (the exe drops them with the
// template). Null for an empty slot.
std::vector<InvObject*> render_object_textures(int32_t id) {
  const RenderObjectDesc* d = render_object_desc(id);
  if (!d->found) return {};
  std::vector<InvObject*> slots;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    auto it = g_render_textures.find(id);
    if (it != g_render_textures.end()) slots = it->second;
  }
  if (slots.size() != d->textures.size()) {
    slots.assign(d->textures.size(), nullptr);
    std::unordered_map<int32_t, InvObject*> by_id;
    for (size_t i = 0; i < d->textures.size(); ++i) {
      const int32_t tid = d->textures[i];
      if (!tid) continue;
      InvObject*& o = by_id[tid];
      if (!o) {
        o = resref_new();
        if (o) java_util_resource_ResourceRef_set(o, tid);
      }
      slots[i] = o;
    }
    std::lock_guard<std::mutex> lock(g_nt_mu);
    auto& kept = g_render_textures[id];
    if (kept.size() == slots.size())
      slots = kept;  // made meanwhile: the first set stays
    else
      kept = slots;
  }
  for (size_t i = 0; i < slots.size(); ++i) {
    InvObject* o = slots[i];
    if (!o || render_d3d9_texture_ready(o)) continue;
    bool seen = false;  // one load per handle
    for (size_t k = 0; k < i && !seen; ++k) seen = slots[k] == o;
    if (!seen) java_util_resource_ResourceRef_load(o);
  }
  return slots;
}

std::vector<void*> render_object_texture_keys(int32_t id, int* missing,
                                              std::string* not_loaded) {
  if (missing) *missing = 0;
  if (not_loaded) not_loaded->clear();
  const RenderObjectDesc* d = render_object_desc(id);
  const std::vector<InvObject*> textures = render_object_textures(id);
  std::vector<void*> keys(textures.size(), nullptr);
  for (size_t i = 0; i < textures.size(); ++i) {
    if (textures[i] && render_d3d9_texture_ready(textures[i])) {
      keys[i] = textures[i];
    } else if (i < d->textures.size() && d->textures[i] != 0) {
      if (missing) ++*missing;
      if (not_loaded) {
        char b[32];
        std::snprintf(b, sizeof b, " %zu:0x%08X", i,
                      static_cast<unsigned>(d->textures[i]));
        *not_loaded += b;
      }
    }
  }
  return keys;
}

int32_t ground_node_of_render_instance(InvObject* render_instance) {
  if (!render_instance) return 0;
  std::lock_guard<std::mutex> lock(g_nt_mu);
  for (const auto& kv : g_nodes) {
    const InstanceNodeState* st = kv.second.get();
    if (!st || !st->object || st->object->type_index != kNativeGround)
      continue;
    const auto* g = static_cast<const GroundObject*>(st->object.get());
    if (g->r_ground == render_instance) return kv.first;
  }
  return 0;
}

// PE 940: when a camera's walk meets a node with POLY sections
// (FUN_0045eb10 queues it, FUN_0045f880 -> FUN_00462750 builds it) the map's
// ground object is asked for its render instance (getinfo 0x17), the
// template of that instance's render object is taken by its flags
// (FUN_0046d890 -> RT_ground) and an RI_ground is made for the node
// (FUN_004658b0): one section per POLY section whose material index is
// below the material count of the template's mesh.
// Fork: the host builds every POLY section of the tree in one mesh, keyed
// by the ground's render instance, the first time a camera sees it (the exe
// builds node by node on a per-frame budget). The projected-shadow copy of
// the geometry (mesh+0x128) is not made.
int ground_mesh_build(InvObject* render_instance, int32_t render_object_id) {
  if (!render_instance || !native_script_boot()) return 0;
  const int32_t node_id = ground_node_of_render_instance(render_instance);
  if (!node_id) return 0;
  const InstanceTree* tree = instance_node_tree(node_id);
  const RenderObjectDesc* d = render_object_desc(render_object_id);
  if (!tree || !d->found ||
      render_template_class(d->flags) != kRenderTemplateGround)
    return 0;
  // RT_ground init FUN_0046df30: RT+0xC4 = the mesh data of the render
  // object, read by the ground mesh loader FUN_00451130 as a material
  // table. The file is reached through the mesh record's `sourcefile`
  // whatever its extension (`.scY` in every shipped map).
  const std::string source = mesh_record_source(d->mesh_id);
  if (source.empty() ||
      !render_d3d9_mesh_create_ground_from_file(render_instance,
                                                source.c_str())) {
    if (native_type_log())
      std::fprintf(stderr,
                   "[native-type] ground mesh node=0x%08X render object "
                   "0x%08X: material table '%s' (mesh 0x%08X) NOT LOADED\n",
                   static_cast<unsigned>(node_id),
                   static_cast<unsigned>(render_object_id), source.c_str(),
                   static_cast<unsigned>(d->mesh_id));
    return -1;
  }
  const uint32_t material_count = static_cast<uint32_t>(
      render_d3d9_mesh_material_count(render_instance));
  // FUN_00441c40: textures[slot] of the render object for every texture
  // entry of every material. A slot whose texture did not load is unbound.
  int missing = 0;
  std::string missing_slots;
  const std::vector<void*> keys =
      render_object_texture_keys(render_object_id, &missing, &missing_slots);
  render_d3d9_mesh_bind_slots(render_instance,
                              keys.empty() ? nullptr : keys.data(),
                              static_cast<int32_t>(keys.size()));
  // RI_ground ctor FUN_004658b0, per vertex: position (the exe multiplies
  // by 10.0 into its engine units, 0x465b5a..0x465b89; the host mesh keeps
  // metres and is drawn with unit 1.0), normal, D3D diffuse = c0 &
  // 0xFF000000, D3D specular = (c0 & 0x00FFFFFF) | (c1 << 24), and the
  // three uv pairs (the exe copies uv[channel of stage i] into texcoord set
  // i; the host selects the channel per stage when it draws).
  uint32_t added = 0, skipped = 0, verts = 0, tris = 0;
  std::vector<RenderWideVertex> wide;
  for (const InstancePolySection& sec : tree->poly) {
    if (sec.material >= material_count) {
      ++skipped;
      continue;
    }
    wide.resize(sec.verts.size());
    for (size_t i = 0; i < sec.verts.size(); ++i) {
      const InstancePolyVertex& s = sec.verts[i];
      RenderWideVertex& w = wide[i];
      std::memcpy(w.pos, s.pos, sizeof(w.pos));
      std::memcpy(w.normal, s.normal, sizeof(w.normal));
      w.diffuse = s.c0 & 0xFF000000u;
      w.specular = (s.c0 & 0x00FFFFFFu) | (s.c1 << 24);
      std::memcpy(w.uv[0], s.uv0, sizeof(w.uv[0]));
      std::memcpy(w.uv[1], s.uv1, sizeof(w.uv[1]));
      std::memcpy(w.uv[2], s.uv2, sizeof(w.uv[2]));
    }
    if (render_d3d9_mesh_ground_add_section(
            render_instance, sec.material, wide.data(),
            static_cast<uint32_t>(wide.size()), sec.indices.data(),
            static_cast<uint32_t>(sec.indices.size()))) {
      ++added;
      verts += static_cast<uint32_t>(sec.verts.size());
      tris += static_cast<uint32_t>(sec.indices.size() / 3);
    } else {
      ++skipped;
    }
  }
  render_d3d9_mesh_set_type_flags(render_instance, d->flags);
  if (native_type_log()) {
    const RpakEntry* e = rpak_find_entry(render_object_id);
    std::fprintf(stderr,
                 "[native-type] ground mesh node=0x%08X render object "
                 "0x%08X '%s' flags %u: material table '%s' %u materials; "
                 "%u of %zu POLY sections (%u skipped), %u vertices, %u "
                 "triangles; %zu texture slots, %d not loaded%s%s\n",
                 static_cast<unsigned>(node_id),
                 static_cast<unsigned>(render_object_id),
                 e ? e->name.c_str() : "", d->flags, source.c_str(),
                 material_count, added, tree->poly.size(), skipped, verts,
                 tris, keys.size(), missing, missing ? ":" : "",
                 missing_slots.c_str());
    for (size_t i = 0; i < keys.size(); ++i) {
      const RpakEntry* te =
          d->textures[i] ? rpak_find_entry(d->textures[i]) : nullptr;
      std::fprintf(stderr,
                   "[native-type]   slot %zu: 0x%08X '%s' %s %dx%d, %d mip "
                   "level(s)\n",
                   i, static_cast<unsigned>(d->textures[i]),
                   te ? te->name.c_str() : "",
                   keys[i] ? "loaded" : (d->textures[i] ? "NOT LOADED" : "empty"),
                   keys[i] ? render_d3d9_texture_width(keys[i]) : 0,
                   keys[i] ? render_d3d9_texture_height(keys[i]) : 0,
                   keys[i] ? render_d3d9_texture_mips(keys[i]) : 0);
    }
  }
  return 1;
}
