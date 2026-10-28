// Fragment 5 of Resources.cpp - included, not compiled on its own (after
// fragments 1 to 4).
//
// M7, the paint booth (../slrr-ghidra/notes/m7/exe_paint.md): the runtime
// textures a stroke draws into.
//   paint_texture_copy       FUN_004e9470 -> FUN_0051ee30 for a texture:
//                            a new runtime resource (id 0xFFFF0000 | n from
//                            the node allocator FUN_00518a40) with the
//                            payload of the old one, made a render target
//                            the first time a stroke needs it
//                            (FUN_0044d590)
//   paint_texture_from_file  the texture of an unsaved row of a car file
//                            (`sourcefile <file>.<k>`, FUN_004d5d60)
//   paint_texture_handle     the host handle (g_textures key) of such an id
// Main thread, like the natives of the script boot.

namespace inv {

namespace {

struct PaintTex {
  InvObject* ref = nullptr;  // the handle; its g_textures entry is the texture
  int32_t source_id = 0;     // the texture it was copied from (0: a file)
  std::string file;          // the file it was read from ("" : a copy)
  // node+0x88: the payload text the node was made from (`sourcefile <file>
  // \r\nflags <n>\r\n`); "" for a paintable copy (FUN_0051ee30 copies the
  // texture, not a text). What a car file's unsaved row keeps inline.
  std::string payload;
  bool target = false;       // a render target (a stroke can draw into it)
};
std::unordered_map<int32_t, PaintTex>& g_paint_tex =
    *new std::unordered_map<int32_t, PaintTex>();

InvObject* paint_texture_new_ref(int32_t* id_out) {
  InvObject* ref = resref_new();
  if (!ref) return nullptr;
  const int32_t id = static_cast<int32_t>(host_alloc_local_rid());
  {
    std::lock_guard<std::mutex> lock(g_mu);
    ResState& r = R(ref);
    r.id = id;
    r.restype = 7;  // a texture (node+0x7C), for File.write(ResourceRef)
    r.loaded = true;
  }
  *id_out = id;
  return ref;
}

}  // namespace

int32_t paint_texture_copy(int32_t source_id) {
  if (!native_script_boot() || !source_id) return 0;
  InvObject* const src = line_texture(source_id);
  if (!src) return 0;
  int32_t id = 0;
  InvObject* const ref = paint_texture_new_ref(&id);
  if (!ref) return 0;
  if (!render_d3d9_texture_make_paintable(ref, src)) return 0;
  PaintTex& t = g_paint_tex[id];
  t.ref = ref;
  t.source_id = source_id;
  t.target = true;
  if (native_type_log()) {
    const RpakEntry* e = rpak_find_entry(source_id);
    std::fprintf(stderr,
                 "[paint] texture 0x%08X '%s' -> paintable copy 0x%08X "
                 "(%dx%d, %d level(s))\n",
                 static_cast<unsigned>(source_id), e ? e->name.c_str() : "",
                 static_cast<unsigned>(id), render_d3d9_texture_width(ref),
                 render_d3d9_texture_height(ref),
                 render_d3d9_texture_mips(ref));
  }
  return id;
}

int32_t paint_texture_from_file(const char* path, const char* payload) {
  if (!native_script_boot() || !path || !path[0]) return 0;
  int32_t id = 0;
  InvObject* const ref = paint_texture_new_ref(&id);
  if (!ref) return 0;
  if (!render_d3d9_texture_create_from_file(ref, path)) return 0;
  PaintTex& t = g_paint_tex[id];
  t.ref = ref;
  t.file = path;
  // FUN_004d5d60: an inline row's text, else `sourcefile <file>.<k>`,
  // `flags 1`.
  t.payload = payload ? std::string(payload)
                      : "sourcefile " + std::string(path) + "\r\nflags 1\r\n";
  if (native_type_log())
    std::fprintf(stderr, "[paint] side file '%s' -> texture 0x%08X (%dx%d)\n",
                 path, static_cast<unsigned>(id),
                 render_d3d9_texture_width(ref),
                 render_d3d9_texture_height(ref));
  return id;
}

InvObject* paint_texture_handle(int32_t id) {
  auto it = g_paint_tex.find(id);
  if (it == g_paint_tex.end()) return nullptr;
  PaintTex& t = it->second;
  // A texture made from a file whose handle was unloaded since (the
  // catalog unloads its decal buttons' textures): read again. In the exe
  // the resource stays while a handle holds it (`new Decal(id)`).
  if (t.ref && !t.file.empty() && !t.target && !render_d3d9_texture_ready(t.ref))
    render_d3d9_texture_create_from_file(t.ref, t.file.c_str());
  return t.ref;
}

std::string paint_texture_payload(int32_t id) {
  auto it = g_paint_tex.find(id);
  return it == g_paint_tex.end() ? std::string() : it->second.payload;
}

void paint_texture_register_file(int32_t id, InvObject* ref, const char* path,
                                 const char* payload) {
  if (!native_script_boot() || !id || !ref || !path || !path[0]) return;
  PaintTex& t = g_paint_tex[id];
  t.ref = ref;
  t.file = path;
  t.payload = payload ? payload : "";
  t.target = false;
}

// FUN_0044d590 for a runtime texture that is no render target yet (one read
// from a side file): the texture is made one in place, its levels kept.
bool paint_texture_target(int32_t id) {
  auto it = g_paint_tex.find(id);
  if (it == g_paint_tex.end() || !it->second.ref) return false;
  if (it->second.target) return true;
  InvObject* const tmp = resref_new();
  if (!tmp || !render_d3d9_texture_make_paintable(tmp, it->second.ref))
    return false;
  render_d3d9_texture_take(it->second.ref, tmp);
  it->second.target = true;
  return true;
}

}  // namespace inv
