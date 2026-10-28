// Fragment 12 of NativeTypes.cpp - included, not compiled on its own.
//
// M7, the paint booth (../slrr-ghidra/notes/m7/exe_paint.md): the `paint`,
// `decal` and `bigdecal` commands of a part and of a car (PE 940
// FUN_004c4400 / FUN_004a2e60), the private paintable texture of a part
// (FUN_004ca120, also GII_PAINTABLE) and the pick of the Painter natives
// (doPaint 0x4E0560, paintPart 0x4E0800). The drawing itself is the
// renderer's (render_d3d9_part8.inc).

namespace {

struct PaintCmdStats {
  int paintable_asked = 0, copies = 0, copy_fail = 0;
  int paints = 0, decals = 0, previews = 0, bigdecals = 0;
  int no_texture = 0, no_instance = 0, no_brush = 0;
  int sends = 0, send_miss = 0;
} g_paint_cmd;

// PE 940 FUN_004ca120(type, part): true at once when the part's texture
// (part+0x84) is a runtime resource (id 0xFFFFxxxx); else the texture is
// duplicated (FUN_004e9470 -> FUN_0051ee30, a texture record only) and
// swapped into the part (FUN_004c75a0, part_set_texture); false when it
// could not be.
// Fork: a runtime texture that is no render target yet (read from a side
// file) is made one here; the exe does it when the first stroke draws
// (FUN_0044d590 from the texture's vt+0x4C).
bool part_make_paintable(PartObject* p) {
  ++g_paint_cmd.paintable_asked;
  if (!p->texture_) return false;
  if ((static_cast<uint32_t>(p->texture_) >> 16) == 0xFFFFu) {
    paint_texture_target(p->texture_);
    return true;
  }
  const int32_t was = p->texture_;
  const int32_t copy = paint_texture_copy(was);
  if (!copy) {
    ++g_paint_cmd.copy_fail;
    return false;
  }
  part_set_texture(p, copy);
  if (p->texture_ != copy) {
    ++g_paint_cmd.copy_fail;
    return false;
  }
  ++g_paint_cmd.copies;
  if (native_type_log()) {
    const RpakEntry* a = rpak_find_entry(p->type_->type_id);
    std::fprintf(stderr,
                 "[paint] '%s' (node 0x%08X): texture 0x%08X -> its own "
                 "copy 0x%08X\n",
                 a ? a->name.c_str() : "",
                 static_cast<unsigned>(part_node_id(p->key_)),
                 static_cast<unsigned>(was), static_cast<unsigned>(copy));
  }
  return true;
}

// part+0x4C: the render instance a stroke goes to, the one of the main
// render entry (the instance the private copy is bound to, part_lines_to).
InvObject* part_paint_instance(PartObject* p) {
  if (PartObject::Inst* i = part_main_instance(p)) return i->r;
  return nullptr;
}

// `decal`: "%*s %d,%d %f,%f,%f %f,%f,%f %f %f %d %d" = colour (-1), brush
// texture id, A (far), B (near), size (1.0), rot, flip, temp; then
// FUN_004c8c60. Nothing for a part without a texture; a stroke that is not
// a preview needs the private copy first.
// Fork: the bounding sphere test of the mesh (FUN_005007c0: the segment
// must enter the sphere grown by size / 2) is not made; the stroke's own
// test per triangle (FUN_00449170) gives the same picture.
bool part_cmd_decal(PartObject* me, const char* text) {
  int32_t colour = -1, brush = 0, flip = 0, temp = 0;
  float a[3] = {0.f, 0.f, 0.f}, b[3] = {0.f, 0.f, 0.f};
  float size = 1.f, rot = 0.f;
  engine_sscanf(text, "%*s %d,%d %f,%f,%f %f,%f,%f %f %f %d %d", &colour,
                &brush, &a[0], &a[1], &a[2], &b[0], &b[1], &b[2], &size, &rot,
                &flip, &temp);
  if (!me->texture_) {
    ++g_paint_cmd.no_texture;
    return true;
  }
  if (!temp && !part_make_paintable(me)) return true;
  InvObject* const inst = part_paint_instance(me);
  if (!inst) {
    ++g_paint_cmd.no_instance;
    return true;
  }
  void* const target = temp ? renderref_line_texture_key(me->texture_)
                            : paint_texture_handle(me->texture_);
  void* const brush_key = brush ? renderref_line_texture_key(brush) : nullptr;
  if (brush && !brush_key) ++g_paint_cmd.no_brush;
  if (brush_key && native_type_log()) {
    static std::unordered_set<int32_t> s_logged;
    if (s_logged.insert(brush).second)
      std::fprintf(stderr, "[paint] brush 0x%08X: '%s' %dx%d, %d level(s)\n",
                   static_cast<unsigned>(brush),
                   render_d3d9_texture_label(brush_key),
                   render_d3d9_texture_width(brush_key),
                   render_d3d9_texture_height(brush_key),
                   render_d3d9_texture_mips(brush_key));
  }
  if (render_d3d9_paint_decal(inst, target, brush_key, a, b, size, rot, flip,
                              static_cast<uint32_t>(colour), temp)) {
    if (temp) ++g_paint_cmd.previews;
    else ++g_paint_cmd.decals;
  }
  return true;
}

// `paint`: "%*s %f %f %d %d %f" = u, v, colour (-1), mode, size (0.02);
// needs a texture and the private copy, then FUN_004eaf60 queues the fill
// with the colour (u, v, mode and size are not read by the fill).
bool part_cmd_paint(PartObject* me, const char* text) {
  float u = 0.f, v = 0.f, size = 0.02f;
  int32_t colour = -1, mode = 0;
  engine_sscanf(text, "%*s %f %f %d %d %f", &u, &v, &colour, &mode, &size);
  if (!me->texture_) {
    ++g_paint_cmd.no_texture;
    return true;
  }
  if (!part_make_paintable(me)) return true;
  if (void* const target = paint_texture_handle(me->texture_)) {
    render_d3d9_paint_fill(target, static_cast<uint32_t>(colour));
    ++g_paint_cmd.paints;
  }
  return true;
}

// `bigdecal <rest>`: `decal <rest>` (the line from its fourth character)
// to the part or car itself and to every part on its list +0xDC.
// Fork: the part's test part+200 != part+8, which sends the line to
// part+0xC0 instead (exe_paint.md 8), is not ported.
bool part_cmd_bigdecal(PartObject* me, const char* text) {
  ++g_paint_cmd.bigdecals;
  const char* p = text;
  while (*p == ' ' || *p == '\t') ++p;
  const char* const sub = std::strlen(p) >= 3 ? p + 3 : p;
  me->event(0x10, sub);
  const std::vector<InvObject*> listed = me->attached_;
  for (InvObject* node : listed)
    if (std::shared_ptr<PartObject> part = part_of(node))
      if (part.get() != me) part->event(0x10, sub);
  return true;
}

// The three for both handlers; false for another word.
bool part_cmd_painter(PartObject* me, const char* word, const char* text) {
  if (word_is(word, "bigdecal")) return part_cmd_bigdecal(me, text);
  if (word_is(word, "decal")) return part_cmd_decal(me, text);
  if (word_is(word, "paint")) return part_cmd_paint(me, text);
  return false;
}

// The exe's '%f' of one value with `fmt` appended to `out`.
void painter_put_f(std::string* out, const char* fmt, float v) {
  std::string s;
  if (!exe940_format_f(fmt, static_cast<double>(v), &s)) {
    char buf[64];
    std::snprintf(buf, sizeof buf, fmt, static_cast<double>(v));
    s = buf;
  }
  *out += s;
}

// GII_PAINTABLE (0x4D): a part answers 1 when it has a texture and
// FUN_004ca120 succeeds, a car when FUN_004ca120 succeeds (FUN_004c3da0,
// FUN_004a15c0); the private copy is made by the question.
int32_t native_part_paintable(PartObject* p, bool car) {
  if (!car && !p->texture_) return 0;
  return part_make_paintable(p) ? 1 : 0;
}

}  // namespace

bool native_painter_send(InvObject* cursor, int kind, int32_t color,
                         int32_t brush, int32_t temp, float rot, float size,
                         int32_t flip) {
  if (!native_script_boot() || !cursor) return false;
  std::shared_ptr<NativeObject> o = native_object_of(cursor);
  if (!o || o->type_index != kNativeCursor) return false;
  const CursorObject* c = static_cast<const CursorObject*>(o.get());
  // FUN_0047c640(cursor, 2): the cursor's position; FUN_004eb000 there.
  const bool hit = cursor_pick(c->x_, c->y_);
  InvObject* const node = hit ? cursor_node(g_pick.obj_id) : nullptr;
  std::shared_ptr<PartObject> p = node ? part_of(node) : nullptr;
  if (!p) {
    ++g_paint_cmd.send_miss;
    return false;
  }
  std::string line;
  char buf[96];
  if (kind == 1) {
    std::snprintf(buf, sizeof buf, "paint 0 0 %d 0 -1", color);
    line = buf;
  } else {
    // "bigdecal %d,%d %.3f,%.3f,%.3f %.3f,%.3f,%.3f %.2f %.2f %d %d":
    // colour, brush, A (far), B (near), size, rot, flip, temp.
    std::snprintf(buf, sizeof buf, "bigdecal %d,%d ", color, brush);
    line = buf;
    for (int i = 0; i < 3; ++i)
      painter_put_f(&line, i < 2 ? "%.3f," : "%.3f ", g_pick.far_pt[i]);
    for (int i = 0; i < 3; ++i)
      painter_put_f(&line, i < 2 ? "%.3f," : "%.3f ", g_pick.near_pt[i]);
    painter_put_f(&line, "%.2f ", size);
    painter_put_f(&line, "%.2f ", rot);
    std::snprintf(buf, sizeof buf, "%d %d", flip, temp);
    line += buf;
  }
  ++g_paint_cmd.sends;
  if (native_type_log() && (kind == 1 || !temp))
    std::fprintf(stderr, "[paint] send to node 0x%08X: %s\n",
                 static_cast<unsigned>(g_pick.obj_id), line.c_str());
  p->event(0x10, line.c_str());
  return true;
}

// `[paint]` summary at loop end (SLRR_PE_NATIVE_PROF).
void native_paint_debug_dump(FILE* out) {
  if (!out || !native_script_boot() || !native_type_log()) return;
  const PaintCmdStats& s = g_paint_cmd;
  if (s.sends == 0 && s.bigdecals == 0 && s.paintable_asked == 0) return;
  std::fprintf(out,
               "[paint] commands: sends=%d (no part under the cursor %d) "
               "bigdecal=%d decal=%d preview=%d paint=%d; paintable asked "
               "%d, copies %d (failed %d); no texture %d, no instance %d, "
               "brush not loaded %d\n",
               s.sends, s.send_miss, s.bigdecals, s.decals, s.previews,
               s.paints, s.paintable_asked, s.copies, s.copy_fail,
               s.no_texture, s.no_instance, s.no_brush);
}
