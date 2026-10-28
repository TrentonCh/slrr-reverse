// Fragment 13 of NativeTypes.cpp - included, not compiled on its own.
//
// M7: the `button` type, row 7 of the native type table (`_type_button`
// vftable 0x59ADB0, gh/button_type.c in ../slrr-ghidra/notes/m7): the
// GameRef Osd.createIcon makes (`new GameRef(group, typeRid, pos +
// " 0.0, 0.0, 0.0", null)`), as the Painter's five decal buttons
// (misc.garage:0x8C `decals_button.cfg`: `render 0x83`, `body 0x90`).
//   cfg     FUN_00488d90 (vt+0x04) with the keyword table 0x5DA4D0:
//           `body` FUN_004884c0 ("%s %f %f %f": a body record by "%d", else
//           a mesh file, FUN_004f7d60(0, 7, 1, 1, 1, path); the angles of
//           the body), `render` FUN_00488740 ("%d %f %f %f %f %f %f": the
//           render type, the instance's offset type+0x19C and angles
//           type+0x1A8 on the body); `name` and every other word read past
//   create  FUN_00488bb0 (vt+0x0C): "%f,%f,%f,%f,%f,%f" position and
//           angles; the body there (FUN_004e8a50), an `r_object` render
//           instance of the `render` line with bone00 on the body at the
//           line's offset (FUN_00524840, FUN_004ec470); event mask 0x10010
//   event   FUN_00488f30 (vt+0x38): `render %d` binds the instance handle
//           to that render instance (the old one is released,
//           FUN_004e9450) and hangs its bone00 on the body at the same
//           offset; `status %d` and the cursor codes 1 / 2 move the body
//           down and up; every event answers 1, so the button's script is
//           not called on the direct route (its notification listeners
//           are, m4 exe_mouse_q.md Q8)
//   getinfo FUN_00488e80 (vt+0x3C): 6 the type, 7 the kind 10
//   destroy FUN_00488e30 / FUN_00488df0
// Fork: no physics: the body is a pose that the pick tests
// (button_click_bodies); the press motion of the body (`status`, LDOWN /
// LUP) is not ported; FUN_00488930 (the drop on a button) is not read.

namespace {

struct ButtonTypeDesc {
  int32_t type_id = 0;
  bool cfg_found = false;
  std::string body_word;            // the `body` line's shape (record / file)
  CamM3 body_rot = cam_identity();  // its angles
  int32_t render_id = 0;            // type+0x1EC (0: no `render` line)
  CamV3 render_off;                 // type+0x19C
  CamM3 render_rot = cam_identity();  // type+0x1A8
  std::shared_ptr<const ClickShape> click;  // the body's shape
};
std::unordered_map<int32_t, std::unique_ptr<ButtonTypeDesc>>& g_button_types =
    *new std::unordered_map<int32_t, std::unique_ptr<ButtonTypeDesc>>();

const ButtonTypeDesc* button_type_desc(const GameTypeDesc* type) {
  static const ButtonTypeDesc kNone;
  if (!type || !type->found) return &kNone;
  {
    std::lock_guard<std::mutex> lock(g_nt_mu);
    auto it = g_button_types.find(type->type_id);
    if (it != g_button_types.end()) return it->second.get();
  }
  auto d = std::make_unique<ButtonTypeDesc>();
  d->type_id = type->type_id;
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
  const int32_t pack = rpak_id_pack(type->type_id);
  if (d->cfg_found) {
    part_cfg_lines(text, [&](const PartCfgLine& l) {
      if (word_is(l.word, "body")) {
        char w[260] = {0};
        float a[3] = {0.f, 0.f, 0.f};
        engine_sscanf(l.rest, "%259s %f %f %f", w, &a[0], &a[1], &a[2]);
        d->body_word = w;
        d->body_rot = cam_ypr_to_matrix(a[0], a[1], a[2]);
      } else if (word_is(l.word, "render")) {
        int32_t local = 0;
        float o[3] = {0.f, 0.f, 0.f}, a[3] = {0.f, 0.f, 0.f};
        engine_sscanf(l.rest, "%d %f %f %f %f %f %f", &local, &o[0], &o[1],
                      &o[2], &a[0], &a[1], &a[2]);
        d->render_id = local ? static_cast<int32_t>(rpak_remap_local_id(
                                   pack, static_cast<uint32_t>(local)))
                             : 0;
        d->render_off = cam_v(o[0], o[1], o[2]);
        d->render_rot = cam_ypr_to_matrix(a[0], a[1], a[2]);
      }
    });
  }
  if (!d->body_word.empty()) d->click = part_click_shape(d->body_word.c_str(), pack);
  if (native_type_log()) {
    const RpakEntry* e = rpak_find_entry(type->type_id);
    const ClickShape* s = d->click.get();
    std::fprintf(stderr,
                 "[native-type] button type 0x%08X '%s' cfg '%s'%s: render "
                 "0x%08X, body '%s' (shape kind %d, %zu triangle(s))\n",
                 static_cast<unsigned>(type->type_id), e ? e->name.c_str() : "",
                 type->cfg_path.c_str(), d->cfg_found ? "" : " NOT FOUND",
                 static_cast<unsigned>(d->render_id), d->body_word.c_str(),
                 s ? s->kind : -1, s ? s->tri.size() : static_cast<size_t>(0));
  }
  std::lock_guard<std::mutex> lock(g_nt_mu);
  auto& slot = g_button_types[type->type_id];
  if (!slot) slot = std::move(d);
  return slot.get();
}

class ButtonObject;
std::vector<ButtonObject*>& g_buttons_live = *new std::vector<ButtonObject*>();

class ButtonObject final : public NativeObject {
 public:
  ButtonObject() { g_buttons_live.push_back(this); }
  ~ButtonObject() override {
    if (phy_id_) g_click_phy.erase(phy_id_);
    for (size_t i = g_buttons_live.size(); i-- > 0;)
      if (g_buttons_live[i] == this) {
        g_buttons_live[i] = g_buttons_live.back();
        g_buttons_live.pop_back();
        break;
      }
  }
  ButtonObject(const ButtonObject&) = delete;
  ButtonObject& operator=(const ButtonObject&) = delete;

  const ButtonTypeDesc* type_ = nullptr;
  InvObject* key_ = nullptr;  // the node; the render instance hangs under it
  float pos_[3] = {0.f, 0.f, 0.f};
  float ypr_[3] = {0.f, 0.f, 0.f};
  CamM3 rot_ = cam_identity();
  InvObject* inst_ = nullptr;  // the render instance the button shows
  bool inst_own_ = false;      // made by the create (else the script's)
  int32_t phy_id_ = 0;         // the host id of the body's "click" node
  bool gone_ = false;

  void push_pose() {
    render_d3d9_mesh_set_transform(key_, pos_[0], pos_[1], pos_[2], ypr_[0],
                                   ypr_[1], ypr_[2], 1.f, 1.f, 1.f);
  }
  // FUN_004ec470(bone00, body, type+0x19C, type+0x1A8): the instance hangs
  // under the node at the line's offset (the body stands at the node's
  // pose turned by the body's angles).
  void hang(InvObject* r) {
    const CamM3 rr = cam_mm(type_->body_rot, type_->render_rot);
    const CamV3 at = cam_mul(type_->body_rot, type_->render_off);
    float y = 0.f, p = 0.f, ro = 0.f;
    if (!part_is_identity(rr)) cam_matrix_to_ypr(rr, &y, &p, &ro);
    render_d3d9_mesh_set_tree_parent(r, key_);
    render_d3d9_mesh_set_parent(r, key_);  // the pose follows the node
    render_d3d9_mesh_set_transform(r, at.x, at.y, at.z, y, p, ro, 1.f, 1.f,
                                   1.f);
  }
  void drop_own_instance() {
    if (inst_ && inst_own_) {
      java_util_resource_ResourceRef_destroy(inst_);
      java_util_resource_ResourceRef_deleteNative(inst_);
    }
    inst_ = nullptr;
    inst_own_ = false;
  }

  void destroy() override {
    if (gone_) return;
    gone_ = true;
    drop_own_instance();
  }

  bool event(int32_t type, const char* text) override {
    if (gone_) return true;
    if (type != 0x10) return true;  // the cursor: answered (Fork: no motion)
    char word[64] = {0};
    engine_sscanf(text ? text : "", "%63s", word);
    if (word_is(word, "render")) {
      int32_t id = 0;
      engine_sscanf(text, "%*s %d", &id);
      if (!type_->render_id) return true;  // type+0x1EC == 0: nothing
      // The instance the script made under this button (Fork: the host's
      // instance carries its render type's id, which other handles share).
      InvObject* const parents[3] = {key_, handle, instance};
      InvObject* r = id ? resref_find_render_instance(id, parents, 3) : nullptr;
      drop_own_instance();  // FUN_004e9450 on the handle
      if (r) {
        inst_ = r;
        hang(r);
      }
      if (native_type_log())
        std::fprintf(stderr,
                     "[native-type] button %p: render 0x%08X%s\n",
                     static_cast<void*>(key_), static_cast<unsigned>(id),
                     r ? "" : " (no such instance)");
      return true;
    }
    return true;  // `status` and every other word: answered
  }

  bool getinfo(int32_t query, NativeInfo* out) override {
    switch (query) {
      case 6:
        out->i = type_->type_id;
        return true;
      case 7:
        out->i = 10;
        return true;
      default:
        return false;
    }
  }
};

// PE 940 FUN_00524840(inst, type+0x1E4, "r_object", 0) under the node.
InvObject* button_new_render_instance(ButtonObject* b, int32_t res_id) {
  InvObject* r = tree_host_new("java.util.resource.RenderRef");
  if (!r) return nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& rs = R(r);
    rs.id = resref_next_runtime_id();
    rs.type = 3;  // INSTANCE_RENDER
    rs.restype = 3;
    rs.type_id = res_id;
    rs.parent = b->key_;
    rs.alias = "r_object";
    rs.loaded = true;
  }
  tree_field_set_obj(r, "alias", string_new("r_object"));
  b->hang(r);
  render_d3d9_mesh_set_pending_type(r, res_id);
  return r;
}

std::shared_ptr<NativeObject> button_create(const NativeCreate& c) {
  auto b = std::make_shared<ButtonObject>();
  b->type_index = kNativeButton;
  b->type_ = button_type_desc(c.type);
  b->key_ = c.instance ? c.instance : c.handle;
  engine_sscanf(c.params ? c.params : "", "%f,%f,%f,%f,%f,%f", &b->pos_[0],
                &b->pos_[1], &b->pos_[2], &b->ypr_[0], &b->ypr_[1],
                &b->ypr_[2]);
  b->rot_ = cam_ypr_to_matrix(b->ypr_[0], b->ypr_[1], b->ypr_[2]);
  if (b->type_->render_id) {
    b->inst_ = button_new_render_instance(b.get(), b->type_->render_id);
    b->inst_own_ = b->inst_ != nullptr;
  }
  if (b->type_->click) {
    b->phy_id_ = resref_next_runtime_id();
    g_click_phy[b->phy_id_] = b->key_;
  }
  b->owns_create_pose = true;
  b->push_pose();
  if (native_type_log())
    std::fprintf(stderr,
                 "[native-type] button %p type 0x%08X: create (%g, %g, %g) "
                 "ypr (%g, %g, %g), render 0x%08X, body %s\n",
                 static_cast<void*>(b->key_),
                 static_cast<unsigned>(b->type_->type_id), b->pos_[0],
                 b->pos_[1], b->pos_[2], b->ypr_[0], b->ypr_[1], b->ypr_[2],
                 static_cast<unsigned>(b->type_->render_id),
                 b->type_->click ? "yes" : "no");
  return b;
}

}  // namespace

// The click bodies of the buttons (the pick, cursor_pick): the body at the
// node's world origin, turned by the node's and the body's angles.
// Fork: a parent node that is turned is not looked at (the Osd's groups
// are not).
void button_click_bodies(std::vector<ClickPickBody>* out) {
  if (!out || g_buttons_live.empty()) return;
  for (ButtonObject* b : g_buttons_live) {
    if (b->gone_ || !b->key_ || !b->phy_id_ || !b->type_->click) continue;
    const ClickShape* s = b->type_->click.get();
    if (s->kind != 6 && s->kind != 7) continue;
    if (s->tri.empty()) continue;
    float w[3] = {0.f, 0.f, 0.f};
    render_d3d9_mesh_world_origin(b->key_, &w[0], &w[1], &w[2]);
    const CamM3 r = cam_mm(b->rot_, b->type_->body_rot);
    ClickPickBody cb;
    cb.owner = b->key_;
    cb.phy = b->phy_id_;
    for (int i = 0; i < 3; ++i) cb.pos[i] = w[i];
    for (int i = 0; i < 3; ++i)
      for (int k = 0; k < 3; ++k) cb.rot[i][k] = r.m[i][k];
    cb.radius = s->radius;
    cb.shape = s;
    out->push_back(cb);
  }
}
