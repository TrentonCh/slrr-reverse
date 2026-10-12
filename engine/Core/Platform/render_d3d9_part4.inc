// Fragment 4 of render_d3d9.cpp - included, not compiled on its own (after
// fragments 1 to 3, which keep their 1/3 .. 3/3 headers).
//
// M2: the ground mesh and material mode 3 of Build 940.
//   default_texture            C3DLow+0x340 (bound by FUN_0043d990)
//   scx_v3_header_read         FUN_004583f0 (+ FUN_00458390 defaults)
//   parse_scx_material_table   FUN_00451130 (ground mesh loader)
//   ..._mesh_create_ground_*   RT_ground init FUN_0046df30 (mesh+0x60/+0x64)
//   ..._mesh_ground_add_section  RI_ground ctor FUN_004658b0 (one section)
//   ..._mesh_bind_slots        FUN_00441c40 (textures by slot)
//   draw_mode3_section         FUN_0043f3a0 mode 3 + FUN_00436230 (Tex) /
//                              FUN_00435f60 (MixVCA)
//   draw_ground_group          rg_ground: FUN_0045e620, FUN_0045f6d0,
//                              FUN_00468b70
//   draw_object_prelit_group   rg_object_prelit: FUN_0045e720, FUN_0045f7a0,
//                              FUN_00468cd0
//   draw_object_prelit_transparent  one entry of rg_prelittransparent:
//                              FUN_0045e870, FUN_00468200
//
// The first part of this fragment is the tail of fragment 3 (the bone
// parent hooks, the mesh parent / bone / transform accessors, the
// uploaded vertex and index buffers and the legacy mesh queue), moved here
// unchanged to keep fragment 3 under the cap. It is still inside the
// `namespace inv` that fragment 1 opened and ends with its closing brace.

// PE ResHandle_Link @ 0x004290F0 (this = node+0x44, a2 = hook+0x0C).
static void res_handle_link_node44(void* node, BoneParentHook* hook) {
  if (!node || !hook) return;
  auto* head = reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 0x44);
  void* cur = head[1];  // node+0x48
  if (cur) *reinterpret_cast<void**>(cur) = &hook->link0;
  hook->link0 = nullptr;
  hook->link1 = cur;
  head[1] = &hook->link0;
  hook->owner_node = node;
}

// PE ResHandle_Rebind @ 0x00429060 (this = bone+0x44|light+0xC, a2 = owner_node|0).
// Exported for voidEvent add_light owner path (RenderNatives / Resources.h).
void res_handle_rebind(void* rh /*bone+0x44*/, void* owner_node) {
  if (!rh) return;
  auto* t = reinterpret_cast<void**>(rh);
  void* old_owner = t[3];  // rh+0x0C
  if (old_owner == owner_node) return;
  if (old_owner) {
    // Unlink from old owner+0x48 list (same shape as hook dtor slice).
    if (t[0]) {
      *reinterpret_cast<void**>(reinterpret_cast<char*>(t[0]) + 4) = t[1];
    } else {
      *reinterpret_cast<void**>(reinterpret_cast<char*>(old_owner) + 0x48) =
          t[1];
    }
    if (t[1]) *reinterpret_cast<void**>(t[1]) = t[0];
    t[3] = nullptr;
    t[2] = nullptr;
    t[0] = nullptr;
    t[1] = nullptr;
  }
  if (owner_node) {
    auto* head =
        reinterpret_cast<void**>(reinterpret_cast<char*>(owner_node) + 0x44);
    void* cur = head[1];  // owner+0x48
    if (cur) *reinterpret_cast<void**>(cur) = rh;
    t[0] = nullptr;
    t[1] = cur;
    head[1] = rh;
    t[3] = owner_node;
    t[2] = *reinterpret_cast<void**>(reinterpret_cast<char*>(owner_node) +
                                     0x50);  // rh+8 ← *(owner+0x50)
  } else {
    t[3] = nullptr;
    t[2] = nullptr;
    t[0] = nullptr;
    t[1] = nullptr;
  }
}

// PE ResHandle_unlinkIndexedSlot @ 0x004293B0 (idx=1 → bone+0x0C/+0x10).
static void res_handle_unlink_indexed_slot1(PeBoneNode* bone) {
  if (!bone) return;
  auto* b = bone->raw;
  void* next = *reinterpret_cast<void**>(b + 0x0C);
  void* prev = *reinterpret_cast<void**>(b + 0x10);
  if (next && prev) {
    *reinterpret_cast<void**>(reinterpret_cast<char*>(next) + 0x10) = prev;
    *reinterpret_cast<void**>(reinterpret_cast<char*>(prev) + 0x0C) = next;
  }
  *reinterpret_cast<void**>(b + 0x0C) = nullptr;
  *reinterpret_cast<void**>(b + 0x10) = nullptr;
}

// PE ResHandle_insertIndexedSlot @ 0x0048F470 with this=payload+0x150, idx=1
// ≡ HEAD insert at payload+0x168 sentinel / +0x178 head (a4==0 manual path).
static void pe_bone_own_list_insert(PeBoneOwnList& own, PeBoneNode* bone) {
  if (!bone) return;
  auto* base = own.raw;
  void* sentinel = base + 0x168;
  void** head_slot = reinterpret_cast<void**>(base + 0x178);
  // Ensure empty-list init (map default-ctor already did; defend OOB reuse).
  if (*head_slot == nullptr) {
    *head_slot = sentinel;
    *reinterpret_cast<void**>(reinterpret_cast<char*>(sentinel) + 0x0C) =
        sentinel;
  }
  void* old_head = *head_slot;
  *reinterpret_cast<void**>(reinterpret_cast<char*>(old_head) + 0x0C) = bone;
  *reinterpret_cast<void**>(bone->raw + 0x0C) = sentinel;
  *reinterpret_cast<void**>(bone->raw + 0x10) = old_head;
  *head_slot = bone;
}

static PeBoneNode* pe_bone_ensure(MeshXform& xf) {
  if (!xf.pe_bone) xf.pe_bone = new PeBoneNode{};
  return xf.pe_bone;
}

static int32_t pe_bone_type_field(PeBoneNode* bone) {
  if (!bone) return 0;
  // bone[+0x4C] = ResHandle at +0x44 field+8 (a3[19] gate / type-match).
  return *reinterpret_cast<int32_t*>(bone->raw + 0x4C);
}

static void pe_bone_store_hook(PeBoneNode* bone, BoneParentHook* hook) {
  if (!bone) return;
  *reinterpret_cast<BoneParentHook**>(bone->raw + 0xAC) = hook;
}

// PE ResHandle_maybeRelinkLod @ 0x00425150 — gate then RelinkLodSlot
// @ 0x00537790 (Resources: tryRelinkLod + freelist skip). Called from
// both a4!=0 arms @ 0x48BEA9 / 0x48BEC0.
static void res_handle_maybe_relink_lod(void* parent_key) {
  if (!parent_key) return;
  void* inner = native_ptr_node(reinterpret_cast<InvObject*>(parent_key));
  res_handle_maybe_relink_lod_node(inner);
}

// PE BoneParentHook_dtor @ 0x0045F8B0 (a2&1 → free): unlink ResHandle slice
// then payload+0x78 dllist (same splice as ResListNode_dtor @ 0x00429150).
static void bone_parent_hook_destroy(BoneParentHook* hook, int32_t* node_flags) {
  constexpr int32_t kHookFlag = 0x4000;
  if (!hook) return;
  // Unlink from node+0x44 list (hook+0x0C slice).
  if (hook->owner_node) {
    void* link = &hook->link0;
    auto* l0 = reinterpret_cast<void**>(link);
    void* owner = hook->owner_node;
    if (l0[0]) {
      *reinterpret_cast<void**>(reinterpret_cast<char*>(l0[0]) + 4) = l0[1];
    } else {
      *reinterpret_cast<void**>(reinterpret_cast<char*>(owner) + 0x48) = l0[1];
    }
    if (l0[1]) *reinterpret_cast<void**>(l0[1]) = l0[0];
    hook->owner_node = nullptr;
    hook->node_field50 = 0;
    hook->link0 = nullptr;
    hook->link1 = nullptr;
  }
  // Unlink from payload+0x78 dllist (+4/+8).
  if (hook->list_back && hook->list_fwd) {
    *reinterpret_cast<void**>(reinterpret_cast<char*>(hook->list_fwd) + 8) =
        hook->list_back;
    *reinterpret_cast<void**>(reinterpret_cast<char*>(hook->list_back) + 4) =
        hook->list_fwd;
  }
  hook->list_fwd = nullptr;
  hook->list_back = nullptr;
  if (node_flags) *node_flags &= ~kHookFlag;
  delete hook;
}

// PE ResPayload_createBoneParentHook @ 0x0053F7E0.
// this=getPayload(parent); arg=render handle → child_node=*(handle+0xC).
static BoneParentHook* res_payload_create_bone_parent_hook(void* payload,
                                                          void* child_node) {
  if (!payload || !child_node) return nullptr;
  auto* flags =
      reinterpret_cast<int32_t*>(reinterpret_cast<char*>(child_node) + 0x54);
  // PE: flags!=0 && !(flags&0x4000). HostResNode has no PE flag word at
  // +0x54 (mid starts there); stock live nodes carry bits (e.g. |=0x200 @
  // 0x539988). Seed 0x200 when zero so the gate can pass on host nodes.
  if (*flags == 0) *flags = 0x200;
  if ((*flags & 0x4000) != 0) return nullptr;

  auto* hook = new BoneParentHook{};
  // ResListNode_ctor zeros +4/+8 then create zeros +0x0C..+0x18; vtbl set.
  // Host: vtbl unused (no PE virtual call); fields match asm stores.
  if (hook->owner_node != child_node) {
    res_handle_link_node44(child_node, hook);
    hook->node_field50 =
        *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(child_node) + 0x50);
  }
  *flags |= 0x4000;

  // payload+0x78 sentinel / +0x80 tail (asm 0x53f88b..0x53f89d).
  auto* p = reinterpret_cast<char*>(payload);
  void* sentinel = p + 0x78;
  auto* tail_slot = reinterpret_cast<void**>(p + 0x80);
  // PE ctor leaves tail → sentinel; host mid pad is zero — init empty list.
  if (*tail_slot == nullptr) {
    *tail_slot = sentinel;
    *reinterpret_cast<void**>(p + 0x7C) = sentinel;
  }
  void* old_tail = *tail_slot;
  *reinterpret_cast<void**>(reinterpret_cast<char*>(old_tail) + 4) = hook;
  hook->list_back = old_tail;
  hook->list_fwd = sentinel;
  *tail_slot = hook;
  return hook;
}

void render_d3d9_mesh_set_parent(void* key, void* parent) {
  if (!key) return;
  // Ignore self-parent; break trivial cycles.
  if (parent == key) parent = nullptr;
  MeshXform& xf = g_mesh_xforms[key];

  // PE RenderRef_LinkOrUnlinkBone @ 0x0048BE10 (always from
  // SetBoneMatrixParentLink @ 0x0048BF50 after HEAD|+0x1800).
  // a1=render handle, a2=THIS payload, a3=bone*, a4=parent_ref|0.
  //   a4==0 && bone[+0x4C]!=0: teardown hook; ~0x4000; unlink +0xC/+0x10;
  //     ResHandle_Rebind(bone+0x44, 0); own-list insert +0x168/+0x178.
  //   a4!=0 && bone[+0x4C]==*(a4+8): ResHandle_maybeRelinkLod(a4) only.
  //   a4!=0 mismatch: unlinkIndexedSlot(1); Rebind(bone+0x44, *(a4+0xC));
  //     insertIndexedSlot(payload+0x150,1,bone); getPayload(A0)+hook→+0xAC;
  //     then maybeRelinkLod(a4) @ 0x48BEA9.
  // Host: PeBoneNode + PeBoneOwnList side map; RelinkLod gate @ 0x425150.
  constexpr int32_t kTagA0 = static_cast<int32_t>(0xA0000000u);

  auto node_flags_ptr = [](InvObject* o) -> int32_t* {
    void* node = native_ptr_node(o);
    if (!node) return nullptr;
    return reinterpret_cast<int32_t*>(reinterpret_cast<char*>(node) + 0x54);
  };
  auto teardown_hook = [&](InvObject* child) {
    if (!xf.bone_parent_hook) return;
    BoneParentHook* hook = xf.bone_parent_hook;
    xf.bone_parent_hook = nullptr;
    if (xf.pe_bone) pe_bone_store_hook(xf.pe_bone, nullptr);
    // PE: (**hook)(hook, 1) → BoneParentHook_dtor; then &= ~0x4000 on
    // *(handle+0xC)+0x54 (also cleared inside dtor path after vtbl call).
    bone_parent_hook_destroy(hook, node_flags_ptr(child));
  };

  auto* child = reinterpret_cast<InvObject*>(key);
  PeBoneNode* bone = pe_bone_ensure(xf);
  PeBoneOwnList& own = g_bone_own_lists[key];

  if (!parent) {
    // a4==0: only when bone[+0x4C]!=0 (PE gate @ 0x48BED0).
    teardown_hook(child);
    if (pe_bone_type_field(bone) != 0) {
      res_handle_unlink_indexed_slot1(bone);
      res_handle_rebind(bone->raw + 0x44, nullptr);
      pe_bone_own_list_insert(own, bone);
    }
    xf.parent = nullptr;
    xf.attach_bone = 0;
    return;
  }

  // Type-match stand-in: bone[+0x4C]==*(a4+8) → RelinkLod only @ 0x48BEC0.
  // Host handle+8 is alive (not PE type); InvObject* equality is the stand-in.
  if (xf.parent == parent) {
    res_handle_maybe_relink_lod(parent);
    return;
  }

  // mismatch path @ 0x48BE2F.. then maybeRelinkLod @ 0x48BEA9.
  teardown_hook(child);
  res_handle_unlink_indexed_slot1(bone);
  native_ptr_ensure(child);
  native_ptr_ensure(reinterpret_cast<InvObject*>(parent));
  void* child_inner = native_ptr_node(child);
  void* parent_inner = native_ptr_node(reinterpret_cast<InvObject*>(parent));
  // Seed owner+0x50 so Rebind writes non-zero bone[+0x4C] (a4==0 gate later).
  if (parent_inner) {
    auto* p50 =
        reinterpret_cast<int32_t*>(reinterpret_cast<char*>(parent_inner) + 0x50);
    if (*p50 == 0) *p50 = 1;
  }
  res_handle_rebind(bone->raw + 0x44, parent_inner);
  pe_bone_own_list_insert(own, bone);
  void* payload =
      parent_inner ? res_handle_get_payload(parent_inner, kTagA0) : nullptr;
  if (payload && child_inner) {
    xf.bone_parent_hook =
        res_payload_create_bone_parent_hook(payload, child_inner);
    pe_bone_store_hook(bone, xf.bone_parent_hook);
  } else {
    pe_bone_store_hook(bone, nullptr);
  }
  xf.parent = parent;
  res_handle_maybe_relink_lod(parent);
}

// Fork: creation parent from RenderRef.create (see MeshXform::tree_parent).
void render_d3d9_mesh_set_tree_parent(void* key, void* parent) {
  if (!key) return;
  g_mesh_xforms[key].tree_parent = (parent == key) ? nullptr : parent;
}

void render_d3d9_mesh_set_attach_bone(void* key, int32_t bone_id) {
  if (!key) return;
  g_mesh_xforms[key].attach_bone = bone_id < 0 ? 0 : bone_id;
}

void* render_d3d9_mesh_get_parent(void* key) {
  if (!key) return nullptr;
  auto it = g_mesh_xforms.find(key);
  return it == g_mesh_xforms.end() ? nullptr : it->second.parent;
}

int32_t render_d3d9_mesh_get_attach_bone(void* key) {
  if (!key) return 0;
  auto it = g_mesh_xforms.find(key);
  return it == g_mesh_xforms.end() ? 0 : it->second.attach_bone;
}

int32_t render_d3d9_mesh_get_bone_id(void* key, const char* alias) {
  // PE getBoneId @ 0x00481020: handle 0 → Mighty ERROR, return 0 (never -1).
  if (!key) return 0;
  std::string name = alias ? alias : "";
  // Stock Camera path attaches under parent + "bone00".
  if (name.empty() || name == "bone00" || name == "root" || name == "Root")
    return 0;
  auto& aliases = g_mesh_bone_alias[key];
  auto it = aliases.find(name);
  if (it != aliases.end()) return it->second;
  int32_t& next = g_mesh_bone_next[key];
  if (next < 1) next = 1;
  const int32_t id = next++;
  aliases[name] = id;
  return id;
}

void render_d3d9_mesh_set_bone_local(void* key, int32_t bone_id, float px,
                                     float py, float pz, float yaw, float pitch,
                                     float roll) {
  if (!key || bone_id < 0) return;
  MeshXform& b = g_mesh_bones[key][bone_id];
  b.px = px;
  b.py = py;
  b.pz = pz;
  b.oy = yaw;
  b.op = pitch;
  b.or_ = roll;
  b.sx = b.sy = b.sz = 1.f;
}

void render_d3d9_mesh_get_transform(void* key, float* px, float* py, float* pz,
                                    float* yaw, float* pitch, float* roll,
                                    float* sx, float* sy, float* sz) {
  MeshXform xf;
  auto it = g_mesh_xforms.find(key);
  if (it != g_mesh_xforms.end()) xf = it->second;
  if (px) *px = xf.px;
  if (py) *py = xf.py;
  if (pz) *pz = xf.pz;
  if (yaw) *yaw = xf.oy;
  if (pitch) *pitch = xf.op;
  if (roll) *roll = xf.or_;
  if (sx) *sx = xf.sx;
  if (sy) *sy = xf.sy;
  if (sz) *sz = xf.sz;
}

void render_d3d9_mesh_world_origin(void* key, float* wx, float* wy, float* wz) {
  float x = 0, y = 0, z = 0;
#ifdef _WIN32
  D3DMATRIX world{};
  resolve_world(key, &world, 0);
  x = world.m[3][0];
  y = world.m[3][1];
  z = world.m[3][2];
#else
  (void)key;
  auto it = g_mesh_xforms.find(key);
  if (it != g_mesh_xforms.end()) {
    x = it->second.px;
    y = it->second.py;
    z = it->second.pz;
  }
#endif
  if (wx) *wx = x;
  if (wy) *wy = y;
  if (wz) *wz = z;
}

int32_t render_d3d9_mesh_vb_stride_from_fvf(uint32_t pe_fvf) {
  // Soft PE GfxVbLayout_FromFvf @ 0x4FE980 — running offset at layout[17]
  // (= wrapper+0x60). Field offset table OOS; stride only.
  int32_t off = 0;
  if ((pe_fvf & 1u) != 0) {
    const int32_t blends = static_cast<int32_t>((pe_fvf >> 8) & 7u);
    off = 12;
    for (int32_t i = 0; i < blends; ++i) off += 4;
  }
  if ((pe_fvf & 0x800u) != 0) off += 4;
  if ((pe_fvf & 2u) != 0) off += 12;
  if ((pe_fvf & 4u) != 0) off += 4;
  if ((pe_fvf & 8u) != 0) off += 4;
  const int32_t tex_n = static_cast<int32_t>((pe_fvf >> 4) & 0xFu);
  int32_t bit = 16;
  for (int32_t t = 0; t < tex_n; ++t) {
    switch ((pe_fvf >> bit) & 3u) {
      case 0:
        off += 4;
        break;
      case 1:
        off += 8;
        break;
      case 2:
        off += 12;
        break;
      case 3:
        off += 16;
        break;
      default:
        break;
    }
    bit += 2;
  }
  return off;
}

uint32_t render_d3d9_mesh_invo_flags_to_fvf(uint32_t invo_flags,
                                            int32_t tex_count) {
  // Soft PE UploadVbFvf @ 0x503475..0x5034EE (before GfxVbLayout_FromFvf).
  uint32_t fvf = (invo_flags & 1u) != 0 ? 1u : 0u;
  if ((invo_flags & 0x40u) != 0) fvf |= 2u;
  if ((invo_flags & 0x80u) != 0) fvf |= 4u;
  if ((invo_flags & 0x100u) != 0) fvf |= 8u;
  if ((invo_flags & 4u) != 0 || (invo_flags & 0x18u) != 0) fvf |= 0x300u;
  if ((invo_flags & 0x20u) != 0) fvf |= 0x800u;
  if (tex_count < 0) tex_count = 0;
  if (tex_count > 8) tex_count = 8;
  fvf |= static_cast<uint32_t>(tex_count) << 4;
  // PE: for each UV slot, OR (1 << (16 + 2*i)) — texcoord format case0.
  for (int32_t i = 0, bit = 16; i < tex_count; ++i, bit += 2)
    fvf |= 1u << bit;
  return fvf;
}

bool render_d3d9_mesh_vb_create(void* key, int32_t vert_count, uint32_t pe_fvf,
                                int32_t stride, const void* bytes,
                                size_t bytes_len) {
  // Soft PE AsyncLoad_Mesh_UploadVbFvf @ 0x503400 — alloc wrapper + blob;
  // CreateVertexBuffer with FVF=pe_fvf (Present SetFVF bind). Attach / pack /
  // AABB OOS (ParseInvo later).
  if (!key || vert_count <= 0) return false;
  int32_t stride_use = stride;
  if (stride_use <= 0)
    stride_use = render_d3d9_mesh_vb_stride_from_fvf(pe_fvf);
  if (stride_use <= 0) return false;
  const size_t need =
      static_cast<size_t>(vert_count) * static_cast<size_t>(stride_use);

  MeshVbState& st = g_mesh_vbs[key];
#ifdef _WIN32
  if (st.vb) {
    st.vb->Release();
    st.vb = nullptr;
  }
#endif
  st.vert_count = vert_count;
  st.stride = stride_use;
  st.pe_fvf = pe_fvf;
  st.cpu.assign(need, 0);
  if (bytes && bytes_len > 0) {
    const size_t n = bytes_len < need ? bytes_len : need;
    std::memcpy(st.cpu.data(), bytes, n);
  }
  st.ready = true;

#ifdef _WIN32
  if (!g_dev) return true;  // bookkeeping without GPU (headless), like dims
  // PE wrapper+0x0C pe_fvf → CreateVertexBuffer FVF (host soft Present bind).
  if (FAILED(g_dev->CreateVertexBuffer(static_cast<UINT>(need), 0, pe_fvf,
                                       D3DPOOL_MANAGED, &st.vb, nullptr)) ||
      !st.vb) {
    st.vb = nullptr;
    return false;
  }
  if (bytes && bytes_len > 0) {
    void* locked = nullptr;
    if (SUCCEEDED(st.vb->Lock(0, static_cast<UINT>(need), &locked, 0)) &&
        locked) {
      const size_t n = bytes_len < need ? bytes_len : need;
      std::memcpy(locked, bytes, n);
      st.vb->Unlock();
    }
  }
  return true;
#else
  (void)bytes_len;
  return st.ready;
#endif
}

void render_d3d9_mesh_vb_destroy(void* key) {
  if (!key) return;
  auto it = g_mesh_vbs.find(key);
  if (it == g_mesh_vbs.end()) return;
#ifdef _WIN32
  // W35-02: soft PE VBSlots freelist (device+0x320) — enqueue; DrainVBSlots.
  if (it->second.vb) {
    g_vb_drain_list.push_back(it->second.vb);
    it->second.vb = nullptr;
  }
#endif
  g_mesh_vbs.erase(it);
}

bool render_d3d9_mesh_vb_ready(void* key) {
  if (!key) return false;
  auto it = g_mesh_vbs.find(key);
  return it != g_mesh_vbs.end() && it->second.ready;
}

int32_t render_d3d9_mesh_vb_vert_count(void* key) {
  if (!key) return 0;
  auto it = g_mesh_vbs.find(key);
  return it == g_mesh_vbs.end() ? 0 : it->second.vert_count;
}

int32_t render_d3d9_mesh_vb_stride(void* key) {
  if (!key) return 0;
  auto it = g_mesh_vbs.find(key);
  return it == g_mesh_vbs.end() ? 0 : it->second.stride;
}

uint32_t render_d3d9_mesh_vb_fvf(void* key) {
  if (!key) return 0;
  auto it = g_mesh_vbs.find(key);
  return it == g_mesh_vbs.end() ? 0u : it->second.pe_fvf;
}

void* render_d3d9_mesh_vb_d3d(void* key) {
  if (!key) return nullptr;
  auto it = g_mesh_vbs.find(key);
  if (it == g_mesh_vbs.end()) return nullptr;
#ifdef _WIN32
  return it->second.vb;
#else
  return nullptr;
#endif
}

bool render_d3d9_mesh_ib_create(void* key, int32_t index_count,
                                const uint16_t* indices) {
  // Soft PE AsyncLoad_Mesh_UploadIbTris @ 0x5038E0 — malloc 20B wrapper
  // (count / blob / type=2 / stride=2) + 2*count bytes; vtbl+20 attach OOS.
  if (!key || index_count <= 0) return false;
  MeshIbState& st = g_mesh_ibs[key];
#ifdef _WIN32
  if (st.ib) {
    st.ib->Release();
    st.ib = nullptr;
  }
#endif
  st.index_count = index_count;
  st.cpu.assign(static_cast<size_t>(index_count), 0);
  if (indices) {
    std::memcpy(st.cpu.data(), indices,
                static_cast<size_t>(index_count) * sizeof(uint16_t));
  }
  st.ready = true;

#ifdef _WIN32
  if (!g_dev) return true;
  const UINT bytes =
      static_cast<UINT>(index_count) * static_cast<UINT>(sizeof(uint16_t));
  if (FAILED(g_dev->CreateIndexBuffer(bytes, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED,
                                      &st.ib, nullptr)) ||
      !st.ib) {
    st.ib = nullptr;
    return false;
  }
  if (indices) {
    void* locked = nullptr;
    if (SUCCEEDED(st.ib->Lock(0, bytes, &locked, 0)) && locked) {
      std::memcpy(locked, indices, bytes);
      st.ib->Unlock();
    }
  }
  return true;
#else
  return st.ready;
#endif
}

void render_d3d9_mesh_ib_destroy(void* key) {
  if (!key) return;
  auto it = g_mesh_ibs.find(key);
  if (it == g_mesh_ibs.end()) return;
#ifdef _WIN32
  // W35-02: soft PE IBSlots freelist (device+0x324) — enqueue; DrainIBSlots.
  if (it->second.ib) {
    g_ib_drain_list.push_back(it->second.ib);
    it->second.ib = nullptr;
  }
#endif
  g_mesh_ibs.erase(it);
}

bool render_d3d9_mesh_ib_ready(void* key) {
  if (!key) return false;
  auto it = g_mesh_ibs.find(key);
  return it != g_mesh_ibs.end() && it->second.ready;
}

int32_t render_d3d9_mesh_ib_index_count(void* key) {
  if (!key) return 0;
  auto it = g_mesh_ibs.find(key);
  return it == g_mesh_ibs.end() ? 0 : it->second.index_count;
}

void* render_d3d9_mesh_ib_d3d(void* key) {
  if (!key) return nullptr;
  auto it = g_mesh_ibs.find(key);
  if (it == g_mesh_ibs.end()) return nullptr;
#ifdef _WIN32
  return it->second.ib;
#else
  return nullptr;
#endif
}

void render_d3d9_mesh_queue_clear() { g_mesh_queue.clear(); }

void render_d3d9_mesh_queue_add(void* key) {
  if (!key || !render_d3d9_mesh_ready(key)) return;
  g_mesh_queue.push_back(key);
}

int32_t render_d3d9_mesh_queue_count() {
  return static_cast<int32_t>(g_mesh_queue.size());
}

}  // namespace inv

namespace inv {
namespace {

// ---------------------------------------------------------------------------
// The engine default texture
// ---------------------------------------------------------------------------
// PE 940 FUN_0043d990(stage, entry): an entry without a texture, or with one
// that is not ready, binds the texture at C3DLow+0x340. The device creation
// FUN_00425a30 makes that texture right after the frame defaults: C3DLow
// vt+0xec(4, 4, 1, 3) = 4x4, one level, engine format 3, filled from four
// rows of the four dwords at 0x593EB0, every one 0x80808080 (file bytes):
// mid grey with alpha 0x80. (M2 spec Q5.)
// Two entries of misc.garage name source files that do not exist
// (g1\png\g1Oszl.png, shell slot 7; g1\png\g1Auto.png, props slot 6): the
// texture loader makes no texture for them (FUN_00455e80 returns null when
// the file cannot be read) and the stock frame shows the `auto` section
// grey, mean (108,106,108) under its lightmap.
// Fork: engine format 3 is taken as A8R8G8B8, as in the host's format table.
#ifdef _WIN32
IDirect3DTexture9* g_default_tex = nullptr;
IDirect3DDevice9* g_default_tex_dev = nullptr;

void default_texture_release() {
  if (g_default_tex) g_default_tex->Release();
  g_default_tex = nullptr;
  g_default_tex_dev = nullptr;
}

IDirect3DBaseTexture9* default_texture() {
  if (!g_dev) return nullptr;
  if (g_default_tex && g_default_tex_dev == g_dev) return g_default_tex;
  default_texture_release();  // a texture of an earlier device
  IDirect3DTexture9* tex = nullptr;
  if (FAILED(g_dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8,
                                  D3DPOOL_MANAGED, &tex, nullptr)) ||
      !tex)
    return nullptr;
  D3DLOCKED_RECT lr{};
  if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) {
    tex->Release();
    return nullptr;
  }
  const uint32_t grey[4] = {0x80808080u, 0x80808080u, 0x80808080u,
                            0x80808080u};
  for (int row = 0; row < 4; ++row)
    std::memcpy(static_cast<uint8_t*>(lr.pBits) + row * lr.Pitch, grey,
                sizeof(grey));
  tex->UnlockRect(0);
  g_default_tex = tex;
  g_default_tex_dev = g_dev;
  return g_default_tex;
}
#endif

// ---------------------------------------------------------------------------
// The material table of a ground render object
// ---------------------------------------------------------------------------
constexpr size_t kScxV3Header = 0x88;  // the exe's material header struct

// PE 940 FUN_00458390: the defaults of the header struct before a header of
// any version is read into it.
void scx_header_defaults(uint8_t* h) {
  std::memset(h, 0, kScxV3Header);
  const int16_t none = -1;
  const uint32_t stride = 0x2c;
  const float one = 1.f;
  std::memcpy(h + 0x20, &one, 4);     // specular level
  std::memcpy(h + 0x36, &none, 2);
  std::memcpy(h + 0x38, &none, 2);    // lightmap slot
  std::memcpy(h + 0x3c, &stride, 4);
  std::memcpy(h + 0x40, &none, 2);
  std::memcpy(h + 0x42, &none, 2);
  std::memcpy(h + 0x44, &none, 2);
  std::memcpy(h + 0x4e, &none, 2);
  // +0x28 flags, +0x46..+0x4d, +0x50..+0x58, +0x5c..+0x64 are zero.
}

// PE 940 FUN_004583f0(header struct, file header): a version 3 header is
// copied over the defaults, at most 0x88 bytes; with flag 8 the short at
// +0x42 of the file header is the copy length when it is below 0x88. Then
// the uv channel of the primary map (+0x46) falls back to 0 when it is not
// 0, 1 or 2, and three zero shorts at +0x40 / +0x42 / +0x44 become -1.
// Returns the header size the file states (the walk advances by it), 0
// when the header does not fit the data.
uint32_t scx_v3_header_read(const uint8_t* data, size_t size, size_t base,
                            uint8_t* h) {
  if (base + 0x2c > size) return 0;
  uint32_t hsz = 0, flags = 0;
  std::memcpy(&hsz, data + base, 4);
  std::memcpy(&flags, data + base + 0x28, 4);
  const int16_t none = -1;
  const uint32_t stride = 0x2c;
  std::memcpy(h + 0x38, &none, 2);
  std::memcpy(h + 0x3c, &stride, 4);
  std::memset(h + 0x5c, 0, 12);
  std::memcpy(h + 0x40, &none, 2);
  std::memcpy(h + 0x42, &none, 2);
  std::memcpy(h + 0x44, &none, 2);
  std::memset(h + 0x46, 0, 8);
  std::memset(h + 0x58, 0, 2);
  std::memcpy(h + 0x4e, &none, 2);
  std::memset(h + 0x50, 0, 8);
  std::memcpy(h, &hsz, 4);
  std::memcpy(h + 0x28, &flags, 4);
  size_t n = hsz;
  if ((flags & 8u) != 0 && base + 0x44 <= size) {
    int16_t alt = 0;
    std::memcpy(&alt, data + base + 0x42, 2);
    std::memcpy(h + 0x42, &alt, 2);
    if (alt < 0x88) n = static_cast<size_t>(alt < 0 ? 0 : alt);
  }
  if (n > kScxV3Header) n = kScxV3Header;
  if (base + n > size) return 0;  // Fork: the exe reads on
  std::memcpy(h, data + base, n);
  uint16_t chan = 0;
  std::memcpy(&chan, h + 0x46, 2);
  if (chan > 2) {
    chan = 0;
    std::memcpy(h + 0x46, &chan, 2);
  }
  int16_t a = 0, b = 0, c = 0;
  std::memcpy(&a, h + 0x40, 2);
  std::memcpy(&b, h + 0x42, 2);
  std::memcpy(&c, h + 0x44, 2);
  if (a == 0 && b == 0 && c == 0) {
    std::memcpy(h + 0x40, &none, 2);
    std::memcpy(h + 0x42, &none, 2);
    std::memcpy(h + 0x44, &none, 2);
  }
  return hsz;
}

// PE 940 ground mesh loader FUN_00451130: the `mesh` of a render object
// with flags 0x800 is read as a list of material headers and nothing else.
// After "INVO" and the version the walk is {header, 8 bytes} until the end
// of the data; in a version 3 file a zero header size ends it. The 8 bytes
// are the vertex and triangle counts of an ordinary mesh block, which the
// loader steps over WITHOUT reading: it expects them to be zero (they are
// in all 35 .scY files of the install, every one version 3 with 0x68-byte
// headers). Every material is created with flag 0x80000000 (prelit)
// whatever its header says.
// Versions 1 and 2 carry 0x38- and 0x34-byte headers that the exe rewrites
// into the version 3 struct; ported from the decompile, no shipped file
// uses them.
bool parse_scx_material_table(const uint8_t* data, size_t size,
                              std::vector<MeshSubmesh>* out) {
  out->clear();
  if (!data || size < 8 || std::memcmp(data, "INVO", 4) != 0) return false;
  uint32_t ver = 0;
  std::memcpy(&ver, data + 4, 4);
  if (ver < 1 || ver > 3) return false;
  auto chan = [](float c) -> uint32_t {
    if (!(c == c)) c = 0.5f;
    if (c < 0.f) c = 0.f;
    if (c > 1.f) c = 1.f;
    return static_cast<uint32_t>(c * 255.f + 0.5f);
  };
  size_t p = 8;
  while (p + 4 <= size && out->size() < 4096) {
    uint8_t h[kScxV3Header];
    scx_header_defaults(h);
    size_t advance = 0;
    uint32_t hsz = 0;
    if (ver == 3) {
      std::memcpy(&hsz, data + p, 4);
      if (hsz == 0) break;
      if (scx_v3_header_read(data, size, p, h) == 0) break;
      advance = hsz;
    } else {
      // Version 1: five map slots as dwords; version 2: as shorts, padded
      // to 12 bytes. Then diffuse RGBA, specular RGB, shininess, (version
      // 2: one unread dword,) flags.
      const size_t words = ver == 1 ? 14 : 13;
      if (p + words * 4 > size) break;
      auto rd = [&](size_t i) {
        uint32_t v = 0;
        std::memcpy(&v, data + p + i * 4, 4);
        return v;
      };
      size_t f = 0;  // index of the first colour dword
      for (int i = 0; i < 5; ++i) {
        int16_t slot = 0;
        if (ver == 1) {
          slot = static_cast<int16_t>(rd(static_cast<size_t>(i)));
        } else {
          std::memcpy(&slot, data + p + static_cast<size_t>(i) * 2, 2);
        }
        std::memcpy(h + 0x2c + i * 2, &slot, 2);
      }
      f = ver == 1 ? 5 : 3;
      for (size_t i = 0; i < 7; ++i) {
        const uint32_t v = rd(f + i);
        std::memcpy(h + 0x04 + i * 4, &v, 4);
      }
      const uint32_t shin = rd(f + 7);
      const uint32_t flags = rd(ver == 1 ? f + 8 : f + 9);
      std::memcpy(h + 0x24, &shin, 4);
      std::memcpy(h + 0x28, &flags, 4);
      const uint32_t own = ver == 1 ? 0x38u : 0x34u;
      const uint32_t stride = 0x34;
      std::memcpy(h, &own, 4);
      std::memcpy(h + 0x3c, &stride, 4);
      hsz = own;
      advance = words * 4;
    }
    {
      int16_t a = 0, b = 0, c = 0;
      std::memcpy(&a, h + 0x40, 2);
      std::memcpy(&b, h + 0x42, 2);
      std::memcpy(&c, h + 0x44, 2);
      if (a == -1 && b == -1 && c == -1) {
        a = 0;
        std::memcpy(h + 0x40, &a, 2);
      }
    }
    if (p + advance + 8 > size) break;  // Fork: a cut block ends the table
    MeshSubmesh sm;
    if (ver == 3 && hsz > 0x68u) {
      const size_t nlim = hsz < kScxV3Header ? hsz : kScxV3Header;
      size_t nend = 0x68;
      while (nend < nlim && h[nend] != 0) ++nend;
      sm.name.assign(reinterpret_cast<const char*>(h + 0x68), nend - 0x68);
    }
    {
      float r = 0, g = 0, b = 0;
      std::memcpy(&r, h + 0x04, 4);
      std::memcpy(&g, h + 0x08, 4);
      std::memcpy(&b, h + 0x0c, 4);
      sm.diffuse = 0xFF000000u | (chan(r) << 16) | (chan(g) << 8) | chan(b);
    }
    scx_v3_material(h, kScxV3Header, 0, static_cast<uint32_t>(kScxV3Header),
                    0u, &sm);
    sm.mat_flags |= 0x80000000u;  // C3DLow vt+0xd0(header, 0x80000000)
    sm.mat_index = static_cast<int32_t>(out->size());
    out->push_back(std::move(sm));
    p += advance + 8;
  }
  return !out->empty();
}

void grow_bounds(MeshState& st, bool* init, float x, float y, float z) {
  if (!*init) {
    st.bmin[0] = st.bmax[0] = x;
    st.bmin[1] = st.bmax[1] = y;
    st.bmin[2] = st.bmax[2] = z;
    *init = true;
    return;
  }
  if (x < st.bmin[0]) st.bmin[0] = x;
  if (y < st.bmin[1]) st.bmin[1] = y;
  if (z < st.bmin[2]) st.bmin[2] = z;
  if (x > st.bmax[0]) st.bmax[0] = x;
  if (y > st.bmax[1]) st.bmax[1] = y;
  if (z > st.bmax[2]) st.bmax[2] = z;
}

}  // namespace

// PE 940 RT_ground init FUN_0046df30: the template takes the mesh data of
// its render object (RT+0xC4) and makes one group entry per material
// (RT+0xD4 / +0xD8). Host: the mesh `key` holds the material table; its
// sections come from render_d3d9_mesh_ground_add_section.
bool render_d3d9_mesh_create_ground_from_memory(void* key, const uint8_t* data,
                                                size_t size,
                                                const char* label) {
  if (!key) return false;
  std::vector<MeshSubmesh> mats;
  if (!parse_scx_material_table(data, size, &mats)) return false;
  {
    auto it = g_meshes.find(key);
    if (it != g_meshes.end()) release_mesh_textures(key, it->second);
  }
  g_mesh_geom_src.erase(key);
  MeshState st;
  st.label = label ? label : "";
  st.unit = 1.f;  // POLY positions are metres
  st.group = kMeshGroupGround;
  st.materials = std::move(mats);
  st.ready = false;  // nothing to draw before the first section
  {
    auto old = g_meshes.find(key);
    st.seq = (old != g_meshes.end() && old->second.seq) ? old->second.seq
                                                         : ++g_mesh_seq;
  }
  g_meshes[key] = std::move(st);
  return true;
}

bool render_d3d9_mesh_create_ground_from_file(void* key, const char* path) {
  if (!key || !path || !path[0]) return false;
  std::string resolved = rpak_resolve_path(path);
  if (resolved.empty()) resolved = path;
  std::vector<uint8_t> buf;
  if (!load_file_bytes(resolved, &buf) || buf.empty()) return false;
  return render_d3d9_mesh_create_ground_from_memory(key, buf.data(),
                                                    buf.size(),
                                                    resolved.c_str());
}

int32_t render_d3d9_mesh_material_count(void* key) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end()) return 0;
  return static_cast<int32_t>(it->second.materials.size());
}

// PE 940 RI_ground ctor FUN_004658b0, one POLY section: a section whose
// material index is not below the mesh's material count is skipped; the
// others fill a section of that material (vertex stream 0x15003f, 16-bit
// indices through FUN_004553e0).
// Fork: a section above 65,535 vertices is cut into several host sections
// (the exe's index stream is 16-bit as well and would wrap).
bool render_d3d9_mesh_ground_add_section(void* key, uint32_t material,
                                         const RenderWideVertex* verts,
                                         uint32_t vert_count,
                                         const uint32_t* indices,
                                         uint32_t index_count) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end() || it->second.group != kMeshGroupGround)
    return false;
  MeshState& st = it->second;
  if (material >= st.materials.size()) return false;
  index_count -= index_count % 3;
  if (!verts || !indices || vert_count == 0 || index_count == 0) return false;
  for (uint32_t i = 0; i < index_count; ++i)
    if (indices[i] >= vert_count) return false;
  static_assert(sizeof(RenderWideVertex) == sizeof(MeshVertexWide),
                "the public wide vertex is the renderer's");
  bool bounds_init = false;
  for (const MeshSubmesh& s : st.subs)
    if (!s.verts.empty()) bounds_init = true;
  auto put_vertex = [&](MeshSubmesh& sm, const RenderWideVertex& src) {
    MeshVertexWide w;
    std::memcpy(&w, &src, sizeof(w));
    MeshVertex v;
    v.x = w.x;
    v.y = w.y;
    v.z = w.z;
    v.nx = w.nx;
    v.ny = w.ny;
    v.nz = w.nz;
    v.u = w.uv[0][0];
    v.v = w.uv[0][1];
    sm.wide.push_back(w);
    sm.verts.push_back(v);
    grow_bounds(st, &bounds_init, w.x, w.y, w.z);
  };
  auto new_section = [&]() {
    MeshSubmesh sm = st.materials[material];  // the material and its bindings
    sm.mat_index = static_cast<int32_t>(material);
    return sm;
  };
  if (vert_count <= 65535u) {
    MeshSubmesh sm = new_section();
    sm.wide.reserve(vert_count);
    sm.verts.reserve(vert_count);
    for (uint32_t i = 0; i < vert_count; ++i) put_vertex(sm, verts[i]);
    sm.indices.resize(index_count);
    for (uint32_t i = 0; i < index_count; ++i)
      sm.indices[i] = static_cast<uint16_t>(indices[i]);
    st.subs.push_back(std::move(sm));
  } else {
    std::vector<int32_t> local(vert_count, -1);
    std::vector<uint32_t> used;
    MeshSubmesh sm = new_section();
    auto flush = [&]() {
      if (!sm.indices.empty()) st.subs.push_back(std::move(sm));
      sm = new_section();
      for (uint32_t u : used) local[u] = -1;
      used.clear();
    };
    for (uint32_t t = 0; t + 2 < index_count; t += 3) {
      uint32_t fresh = 0;
      for (int k = 0; k < 3; ++k)
        if (local[indices[t + k]] < 0) ++fresh;
      if (sm.wide.size() + fresh > 65535u) flush();
      for (int k = 0; k < 3; ++k) {
        const uint32_t src = indices[t + k];
        if (local[src] < 0) {
          local[src] = static_cast<int32_t>(sm.wide.size());
          used.push_back(src);
          put_vertex(sm, verts[src]);
        }
        sm.indices.push_back(static_cast<uint16_t>(local[src]));
      }
    }
    if (!sm.indices.empty()) st.subs.push_back(std::move(sm));
  }
  st.ready = !st.subs.empty();
  return true;
}

// PE 940 FUN_00441c40(material, textures, count): every texture entry of
// the material whose slot is inside the render object's texture list takes
// textures[slot]; an entry whose slot is past the list keeps what it had
// (nothing). The exe runs it per ground material at draw until the
// template's textures are in (FUN_00468b70) and once per RT_object template
// (FUN_0046f4b0).
// Host: `textures[i]` is the g_textures key of the i-th `texture` line, or
// null for an empty line or a texture that did not load: such an entry is
// unbound and draws with the engine default texture.
void render_d3d9_mesh_bind_slots(void* key, void* const* textures,
                                 int32_t count) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end()) return;
  MeshState& st = it->second;
  auto pick = [&](int16_t slot) -> void* {
    return (textures && slot >= 0 && slot < count) ? textures[slot] : nullptr;
  };
  auto bind = [&](MeshSubmesh& sm) {
    sm.texture_key = pick(sm.slot_primary);
    sm.texture_second = pick(sm.slot_second);
    sm.texture_env = pick(sm.slot_env);
    sm.texture_lightmap = pick(sm.slot_lightmap);
    sm.texture_path.clear();
    sm.slot_bound = true;
  };
  for (size_t i = 0; i < st.subs.size(); ++i) {
    MeshSubmesh& sm = st.subs[i];
    if (!sm.has_mat) continue;  // no parsed material: no entries to bind
    // A texture the file-name heuristic made for this section is the
    // mesh's own; it goes when the slot takes over.
    if (sm.texture_key && sm.texture_key == mesh_sub_tex_key(key, i))
      render_d3d9_texture_destroy(sm.texture_key);
    bind(sm);
  }
  for (MeshSubmesh& sm : st.materials)
    if (sm.has_mat) bind(sm);
}

int32_t render_d3d9_mesh_texture_uses(void* key, void* texture_key) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end() || !texture_key) return 0;
  int32_t n = 0;
  for (const MeshSubmesh& sm : it->second.subs) {
    if (sm.texture_key == texture_key) ++n;
    if (sm.texture_second == texture_key) ++n;
    if (sm.texture_env == texture_key) ++n;
    if (sm.texture_lightmap == texture_key) ++n;
  }
  return n;
}

int32_t render_d3d9_mesh_slot_summary(void* key, char* buf, size_t cap) {
  if (buf && cap) buf[0] = 0;
  auto it = g_meshes.find(key);
  if (it == g_meshes.end()) return 0;
  int32_t parsed = 0;
  size_t used = 0;
  auto ready = [](void* tkey) {
    if (!tkey) return false;
    auto tit = g_textures.find(tkey);
    return tit != g_textures.end() && texture_d3d_base(tit->second) != nullptr;
  };
  for (const MeshSubmesh& sm : it->second.subs) {
    if (sm.has_mat) ++parsed;
    if (!buf || used + 1 >= cap) continue;
    char item[160];
    auto slot = [&](int16_t s, void* tkey, char* out, size_t n) {
      if (s == -1)
        std::snprintf(out, n, "-");
      else
        std::snprintf(out, n, "%d%s", static_cast<int>(s),
                      ready(tkey) ? "" : "!");
    };
    char p[12], s2[12], lm[12];
    slot(sm.slot_primary, sm.texture_key, p, sizeof p);
    slot(sm.slot_second, sm.texture_second, s2, sizeof s2);
    slot(sm.slot_lightmap, sm.texture_lightmap, lm, sizeof lm);
    std::snprintf(item, sizeof item, " %s(%s/%s/%s %08X w%zu)",
                  sm.name.empty() ? "?" : sm.name.c_str(), p, s2, lm,
                  sm.has_mat ? sm.mat_flags : 0u, sm.wide.size());
    const size_t n = std::strlen(item);
    if (used + n + 1 > cap) {
      if (used + 4 < cap) {
        std::memcpy(buf + used, " ...", 5);
        used = cap;  // nothing more fits
      }
      continue;
    }
    std::memcpy(buf + used, item, n + 1);
    used += n;
  }
  return parsed;
}

// ---------------------------------------------------------------------------
// Material mode 3
// ---------------------------------------------------------------------------
namespace {

// What the stage setups below need from the device. The exe picks a variant
// of each material class at start-up with IDirect3DDevice9::ValidateDevice
// (FUN_00442500 -> FUN_0043ef70, device vt+0x118) and counts down from the
// variant index in the image when a setup does not validate:
//   CMaterial_Tex with a lightmap, DAT_005da05c: starts at 1 (file bytes) =
//   one pass, two stages;
//   CMaterial_MixVCA with a lightmap, DAT_005eecf0: in .bss, so it starts
//   at 0 and can only stay there = two passes of two stages. (M2 spec Q4.)
// Fork: the host does not validate; it asks the caps for MULTIPLYADD and
// two stages and otherwise takes a LERP setup of the same result.
struct Mode3Caps {
  IDirect3DDevice9* dev = nullptr;
  bool multiply_add = false;
  bool lerp = false;
};
Mode3Caps g_mode3_caps;

const Mode3Caps& mode3_caps() {
  if (g_mode3_caps.dev == g_dev) return g_mode3_caps;
  g_mode3_caps = Mode3Caps{};
  g_mode3_caps.dev = g_dev;
  D3DCAPS9 caps{};
  if (g_dev && SUCCEEDED(g_dev->GetDeviceCaps(&caps))) {
    const bool two = caps.MaxTextureBlendStages >= 2 &&
                     caps.MaxSimultaneousTextures >= 2;
    g_mode3_caps.multiply_add =
        two && (caps.TextureOpCaps & D3DTEXOPCAPS_MULTIPLYADD) != 0;
    g_mode3_caps.lerp = two && (caps.TextureOpCaps & D3DTEXOPCAPS_LERP) != 0;
  }
  return g_mode3_caps;
}

// PE 940 FUN_0043d990(stage, entry): the entry's texture when it has one
// that is ready, else the engine default texture. (An unbound env entry
// 0x6000003 takes the global envmap C3DLow+0x39C instead; mode 3 binds no
// env entry.) An entry that has a texture sets CLAMP on the axes whose
// address bit is clear; FUN_0043d930 puts WRAP back when the pass ends.
// Host: `addr` = the entry's address bits (MeshSubmesh::addr_*: 3 for a v3
// entry, M2 spec Q14); the axes are set either way, so nothing is left to
// put back between passes.
void mode3_bind_entry(DWORD stage, void* key, uint8_t addr) {
  IDirect3DBaseTexture9* tex = nullptr;
  if (key) {
    auto tit = g_textures.find(key);
    if (tit != g_textures.end()) tex = texture_d3d_base(tit->second);
  }
  g_dev->SetSamplerState(stage, D3DSAMP_ADDRESSU,
                         (key && (addr & 1u) == 0) ? D3DTADDRESS_CLAMP
                                                   : D3DTADDRESS_WRAP);
  g_dev->SetSamplerState(stage, D3DSAMP_ADDRESSV,
                         (key && (addr & 2u) == 0) ? D3DTADDRESS_CLAMP
                                                   : D3DTADDRESS_WRAP);
  if (!tex) {
    tex = default_texture();
    ++g_cam_pass_stats.default_tex_binds;
  }
  g_dev->SetTexture(stage, tex);
}

// CMaterial_MixVCA and the ids FUN_0042bff0 maps to it: 2, 6, 0xA, 0xE,
// 0x12, 0x16, 0x1A, 0x1E, 0x22, 0x26.
bool shader_is_mix_vca(int32_t shader) {
  return shader >= 2 && shader <= 0x26 && (shader & 3) == 2;
}

// CMaterial_Tex: id 0 and every id the factory switch FUN_0042bff0 does
// not know (its default case). The ids it knows: 1 .. 0x27, 0x30 .. 0x35,
// 0x37 and 0xFFFFFB .. 0xFFFFFF.
bool shader_is_tex(int32_t shader) {
  if (shader == 0) return true;
  if (shader >= 1 && shader <= 0x27) return false;
  if (shader >= 0x30 && shader <= 0x35) return false;
  if (shader == 0x37) return false;
  if (shader >= 0xFFFFFB && shader <= 0xFFFFFF) return false;
  return shader > 0;  // -1 = no parsed material
}

// The exe copies uv[channel] into the texcoord set of a stage for channel
// 0, 1 or 2 and writes (0, 0) for any other (FUN_004658b0). Host: a set the
// wide FVF does not have reads as (0, 0) as well.
DWORD mode3_texcoord_index(int16_t channel) {
  return (channel >= 0 && channel <= 2) ? static_cast<DWORD>(channel) : 3u;
}

void mode3_draw(const MeshSubmesh& sm) {
  g_dev->DrawIndexedPrimitiveUP(
      D3DPT_TRIANGLELIST, 0, static_cast<UINT>(sm.wide.size()),
      static_cast<UINT>(sm.indices.size() / 3), sm.indices.data(),
      D3DFMT_INDEX16, sm.wide.data(), sizeof(MeshVertexWide));
}

// One section in material mode 3, the mode of rg_ground and of the prelit
// object group: C3DLow vt+0xe0(material, 3) = FUN_0043f3a0, then per pass
// vt+0xe4, the draw, and vt+0xe8.
// The caller has set the wide FVF, the world matrix, lighting off and the
// samplers of stages 0 and 1; this function leaves stage 0 on MODULATE and
// stage 1 disabled, as the exe's passes do when they end.
// `group_alpha_test`: the group has set an alpha test of its own
// (rg_prelittransparent: FUN_00422c60(2,1), GREATER 4), which a material
// without an alpha reference leaves alone (FUN_0043f3a0 only touches
// ALPHAREF and ALPHATESTENABLE when material+0xC is not zero).
void draw_mode3_section(const MeshSubmesh& sm, bool group_alpha_test = false) {
  if (sm.wide.empty() || sm.indices.size() < 3) return;
  // FUN_0043f3a0, mode 3: the vertex colours are the material sources.
  // (With lighting off, as in rg_ground, D3D passes both through as they
  // are; D3DTA_SPECULAR below reads the second one.)
  g_dev->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
  g_dev->SetRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2);
  // Cull mode. The frame defaults FUN_00426d10 send SetRenderState(
  // CULLMODE, C3DLow[6]) and the C3DLow ctor FUN_004223a0 sets C3DLow[6] =
  // 2 = D3DCULL_CW: under the exe's right-handed projection that removes
  // the faces that look away from the eye (every triangle of the POLY data
  // and of the props has cross(v1 - v0, v2 - v0) . normal > 0).
  // FUN_0043f3a0 switches to D3DCULL_NONE for a material whose create
  // flags have 0x02000000. The host builds the same view and projection
  // and keeps the file's vertex order, so the same mode culls the same
  // faces.
  // Thin sheets of the props are two coplanar faces with opposite normals
  // and different lightmap coordinates (370 such pairs in `texturak`, the
  // jack stands among them): without culling both are drawn at one depth.
  // Fork: only material mode 3 culls; every other draw of the host keeps
  // D3DCULL_NONE (M2 spec, kept states), put back by Mode3Scope::end().
  g_dev->SetRenderState(D3DRS_CULLMODE,
                        (sm.mat_flags & 0x02000000u) != 0 ? D3DCULL_NONE
                                                           : D3DCULL_CW);
  // material+0xC != 0: ALPHAREF and ALPHATESTENABLE. Host: the alpha-test
  // create flag with the material's reference, as in the object groups.
  const bool atest = (sm.mat_flags & 0x20000000u) != 0;
  if (atest) {
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    g_dev->SetRenderState(D3DRS_ALPHAREF, sm.mat_alpha_ref & 0xffu);
  } else if (!group_alpha_test) {
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  }
  // SetRenderState(TEXTUREFACTOR, (int)(diffuse alpha * fade * 255) << 24);
  // alpha of every pass = stage 0 texture alpha * TFACTOR (frame defaults
  // FUN_00426d10: stage 0 ALPHAOP MODULATE, ALPHAARG2 TFACTOR).
  g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, sm.mat_diffuse & 0xFF000000u);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
  // Fork: the exe leaves the alpha op of stage 1 at its default, DISABLE,
  // while the stage's colour op runs; the host passes the alpha on.
  g_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  g_dev->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);

  const bool mix = shader_is_mix_vca(sm.mat_shader);
  const bool lightmap = sm.slot_lightmap != -1;  // entry [0] slot != -1
  const Mode3Caps& caps = mode3_caps();

  if (mix) {
    // CMaterial_MixVCA (FUN_0042ac10; entries [3] primary 0x6000000, [4]
    // second 0x6000001, [0] lightmap 0x6000007), with a lightmap
    // FUN_00435f60, variant 0. Pass 0:
    //   stage 0 = primary, COLORARG2 = SPECULAR | COMPLEMENT (0x14), the op
    //   stays MODULATE: primary * (1 - s);
    //   stage 1 = second, COLOROP = MULTIPLYADD (0x19), COLORARG2 =
    //   SPECULAR: current + second * s;
    // s = the D3D specular vertex colour = c0.rgb of the POLY vertex.
    mode3_bind_entry(0, sm.texture_key, sm.addr_primary);
    mode3_bind_entry(1, sm.texture_second, sm.addr_second);
    g_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX,
                                mode3_texcoord_index(sm.chan_primary));
    g_dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX,
                                mode3_texcoord_index(sm.chan_second));
    if (caps.multiply_add) {
      g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      g_dev->SetTextureStageState(0, D3DTSS_COLORARG2,
                                  D3DTA_SPECULAR | D3DTA_COMPLEMENT);
      g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MULTIPLYADD);
      g_dev->SetTextureStageState(1, D3DTSS_COLORARG0, D3DTA_CURRENT);
      g_dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      g_dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_SPECULAR);
    } else {
      // Fork: no MULTIPLYADD on this device (the exe's variant would not
      // validate and the class is switched off, DAT_005eecf8). The same
      // mix with LERP: second * s + primary * (1 - s).
      g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      g_dev->SetTextureStageState(1, D3DTSS_COLOROP,
                                  caps.lerp ? D3DTOP_LERP : D3DTOP_DISABLE);
      g_dev->SetTextureStageState(1, D3DTSS_COLORARG0, D3DTA_SPECULAR);
      g_dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      g_dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
    }
    mode3_draw(sm);
    if (lightmap) {
      // Pass 1 (FUN_00440f50, then the stage states): the frame is
      // multiplied by the lightmap. ALPHABLENDENABLE 1, SRCBLEND ZERO,
      // DESTBLEND SRCCOLOR, BLENDOP ADD, alpha test off; stage 0 = lightmap
      // on texcoord set 2 (the exe's third set = the lightmap channel),
      // COLOROP SELECTARG1; stage 1 off.
      g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
      g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
      g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
      g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
      g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
      g_dev->SetTexture(1, nullptr);
      mode3_bind_entry(0, sm.texture_lightmap, sm.addr_lightmap);
      g_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX,
                                  mode3_texcoord_index(sm.chan_lightmap));
      g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
      g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
      g_dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
      mode3_draw(sm);
      // The pass after the last (FUN_004406d0): the blend state back.
      g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
      g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
      g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }
    // else, Fork (M2 spec Q13): without a lightmap slot the exe's setup is
    // FUN_0043c420, whose third stage multiplies the mix by D3DTA_DIFFUSE.
    // A ground vertex carries no diffuse RGB (c0 & 0xFF000000), so what
    // that setup shows was not settled; the host draws the mix alone.
    // No material of garage1 comes here.
    g_dev->SetTexture(0, nullptr);
    g_dev->SetTexture(1, nullptr);
    g_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
    g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
    return;
  }

  // CMaterial_Tex (FUN_0042af10; entries [3] primary, [0] lightmap) and,
  // Fork, every class whose mode 3 setup was not read (MixTA, ColorMixTex,
  // the specular and sphere-map variants: none in garage1).
  // With a lightmap FUN_00436230, variant 1, one pass:
  //   stage 0 = primary, COLOROP SELECTARG1 (the texture alone);
  //   stage 1 = lightmap, COLOROP MODULATE.
  // No vertex colour and no light take part.
  mode3_bind_entry(0, sm.texture_key, sm.addr_primary);
  g_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX,
                              mode3_texcoord_index(sm.chan_primary));
  g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  if (lightmap) {
    mode3_bind_entry(1, sm.texture_lightmap, sm.addr_lightmap);
    g_dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX,
                                mode3_texcoord_index(sm.chan_lightmap));
    g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
  } else {
    // Fork (M2 spec Q13): FUN_0043c9a0 binds the primary alone and leaves
    // stage 0 on its default MODULATE with the vertex diffuse; see the
    // MixVCA note above. The host draws the texture alone. No material of
    // garage1 comes here; 1,456 of the 1,542 ground materials of the other
    // maps do.
    g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  }
  mode3_draw(sm);
  // The pass after the last: both stages unbound (FUN_0043d930), stage 0
  // COLOROP back to MODULATE, stage 1 to DISABLE.
  g_dev->SetTexture(0, nullptr);
  g_dev->SetTexture(1, nullptr);
  g_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
  g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
}

constexpr DWORD kFvfWide =
    D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX3;

// Fork: the device state around the draws of material mode 3. The states
// the passes change and no other draw of the host sets for itself go back
// to what they were when the scope ends (the exe resets its whole state
// block once a frame, FUN_00426d10). Stage 1 serves the envmap stand-in of
// the object groups as well.
// begin(): lighting off (the groups of mode 3 all start with C3DLow
// vt+0x10c(0,0)), blending off, z write on, the wide FVF, and the frame
// defaults of the samplers: LINEAR min, mag and mip (C3DLow[0xcc..0xce] =
// 2), WRAP on both axes.
// end(): what the object groups (draw_section) expect to find.
struct Mode3Scope {
  static constexpr size_t kNumRs = 8;
  static constexpr size_t kNumTss0 = 6;
  DWORD saved_rs[kNumRs] = {};
  DWORD saved_tss0[kNumTss0] = {};
  DWORD saved_mip1 = D3DTEXF_NONE;

  static const D3DRENDERSTATETYPE* rs_list() {
    static const D3DRENDERSTATETYPE k[kNumRs] = {
        D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE,
        D3DRS_SRCBLEND,              D3DRS_DESTBLEND,
        D3DRS_BLENDOP,               D3DRS_ALPHAFUNC,
        D3DRS_ALPHAREF,              D3DRS_TEXTUREFACTOR};
    return k;
  }
  static const D3DTEXTURESTAGESTATETYPE* tss0_list() {
    static const D3DTEXTURESTAGESTATETYPE k[kNumTss0] = {
        D3DTSS_COLOROP,  D3DTSS_COLORARG1, D3DTSS_COLORARG2,
        D3DTSS_ALPHAOP,  D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2};
    return k;
  }

  void begin() {
    for (size_t i = 0; i < kNumRs; ++i)
      g_dev->GetRenderState(rs_list()[i], &saved_rs[i]);
    for (size_t i = 0; i < kNumTss0; ++i)
      g_dev->GetTextureStageState(0, tss0_list()[i], &saved_tss0[i]);
    g_dev->GetSamplerState(1, D3DSAMP_MIPFILTER, &saved_mip1);
    g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_dev->SetFVF(kFvfWide);
    for (DWORD stage = 0; stage < 2; ++stage) {
      g_dev->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
      g_dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
      g_dev->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
      g_dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
      g_dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    }
  }

  void end() {
    g_dev->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1);
    g_dev->SetTexture(0, nullptr);
    g_dev->SetTexture(1, nullptr);
    for (DWORD stage = 0; stage < 2; ++stage) {
      g_dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
      g_dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    }
    g_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    g_dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0);
    g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_dev->SetTextureStageState(1, D3DTSS_COLORARG0, D3DTA_CURRENT);
    g_dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
    g_dev->SetSamplerState(1, D3DSAMP_MIPFILTER, saved_mip1);
    for (size_t i = 0; i < kNumTss0; ++i)
      g_dev->SetTextureStageState(0, tss0_list()[i], saved_tss0[i]);
    for (size_t i = 0; i < kNumRs; ++i)
      g_dev->SetRenderState(rs_list()[i], saved_rs[i]);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  }
};

// The world matrix of an object section: the instance matrix, its 3x3
// times MeshState::unit (INVO centimetres -> metres; the exe scaled the
// vertices at load).
void set_object_world(void* key, const MeshState& ms) {
  D3DMATRIX world{};
  resolve_world(key, &world, 0);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) world.m[r][c] *= ms.unit;
  g_dev->SetTransform(D3DTS_WORLD, &world);
}

}  // namespace

// PE 940 rg_ground (cam+0x528, vftable 0x599170), the group after
// rg_horizon in the camera pass FUN_00460ba0.
//   begin FUN_0045e620: C3DLow vt+0x10c(0,0) = D3DRS_LIGHTING off, world =
//   the identity (the ground has no instance pose);
//   loop FUN_0045f6d0: the group's entries in the order they were added,
//   which is material by material for each RT_ground template
//   (FUN_0046df30); each entry FUN_00468b70 binds the template's textures
//   into the material (FUN_00441c40), enters material mode 3 and draws
//   every filled section of that material.
// Host: `secs` holds the sections mesh by mesh in POLY walk order; they
// are drawn per mesh in material order.
// Fork: the world matrix is the identity times MeshState::unit (1.0 for
// POLY metres; the exe multiplied the vertices by 10.0 into its engine
// units). The cull mode is the exe's (draw_mode3_section).
static void draw_ground_group(const std::vector<GroundSec>& secs) {
  if (secs.empty() || !g_dev) return;
  std::vector<GroundSec> order(secs);
  for (size_t a = 0; a < order.size();) {
    size_t b = a;
    while (b < order.size() && order[b].ms == order[a].ms) ++b;
    std::stable_sort(order.begin() + static_cast<std::ptrdiff_t>(a),
                     order.begin() + static_cast<std::ptrdiff_t>(b),
                     [](const GroundSec& x, const GroundSec& y) {
                       return x.ms->subs[static_cast<size_t>(x.sub)].mat_index <
                              y.ms->subs[static_cast<size_t>(y.sub)].mat_index;
                     });
    a = b;
  }
  Mode3Scope scope;
  scope.begin();
  const int32_t default_binds_before = g_cam_pass_stats.default_tex_binds;
  const MeshState* world_of = nullptr;
  for (const GroundSec& gs : order) {
    if (gs.ms != world_of) {
      D3DMATRIX world{};
      mat_identity(&world);
      world.m[0][0] = world.m[1][1] = world.m[2][2] = gs.ms->unit;
      g_dev->SetTransform(D3DTS_WORLD, &world);
      world_of = gs.ms;
    }
    draw_mode3_section(gs.ms->subs[static_cast<size_t>(gs.sub)]);
    ++g_cam_pass_stats.ground_sections;
  }
  g_cam_pass_stats.ground_default_tex_binds +=
      g_cam_pass_stats.default_tex_binds - default_binds_before;
  scope.end();
}

static bool mode3_object_class(int32_t shader) {
  return shader_is_tex(shader) || shader_is_mix_vca(shader);
}

// PE 940 rg_object_prelit (cam+0x69c): the opaque prelit sections of the
// object instances, one group entry per material (rg_object_prelit_material).
//   group begin FUN_0045e780 (vftable 0x599260 +4): C3DLow vt+0x10c(0,0)
//   = lighting off; the group has no end of its own;
//   material begin FUN_0045e720: C3DLow vt+0xe0(material, 3), mode 3;
//   loop FUN_0045f7a0: per pass vt+0xe4, then every entry's draw
//   FUN_00468cd0: the lights of the entry's sector (FUN_0045c8f0), the
//   world matrix = the instance matrix (RI+0x30; the identity for a slot
//   copy, RI+0x9c), the section (vt+0xb8);
//   end FUN_00460100: vt+0xe8.
// Host: the sections in the order the pass collected them, each through
// draw_mode3_section. For the props of the first garage that is
// CMaterial_Tex with a lightmap, FUN_00436230: primary(uv of its entry) *
// lightmap(uv of its entry), no vertex colour, no light.
// Fork: the instance colour is not consulted (FUN_0045f7a0 and FUN_00468cd0
// do not read it either); skinned sections (RI+0xd0 > 1) are not ported.
static void draw_object_prelit_group(const std::vector<Mode3Sec>& secs) {
  if (secs.empty() || !g_dev) return;
  Mode3Scope scope;
  scope.begin();
  for (const Mode3Sec& s : secs) {
    set_object_world(s.key, *s.ms);
    draw_mode3_section(s.ms->subs[static_cast<size_t>(s.sub)]);
    ++g_cam_pass_stats.prelit_mode3_sections;
  }
  scope.end();
}

// PE 940 rg_prelittransparent (cam+0x80c), one entry.
//   begin FUN_0045e870: C3DLow vt+0x10c(0,0) = lighting off, vt+0x34(2,1) =
//   FUN_00422c60: ZWRITEENABLE 0, ALPHABLENDENABLE 1, SRCBLEND SRCALPHA,
//   DESTBLEND INVSRCALPHA, BLENDOP ADD, ALPHATESTENABLE 1, ALPHAFUNC
//   GREATER, ALPHAREF 4 (the state of rg_transparent);
//   loop FUN_00468200: the entries back to front through the heap of
//   rg_transparent (FUN_0046b5e0 / FUN_0040abe0, key RI+0xb8), each with
//   vt+0xe0(material, 3), per pass vt+0xe4 and the entry draw, then vt+0xe8.
//   (M2 spec Q6: the mode is 3.)
// So a transparent prelit section is its mode 3 colour, texture * lightmap
// for CMaterial_Tex, blended by the alpha of stage 0 = texture alpha *
// TFACTOR alpha (the `ablak` window panes: DXT3 alpha).
// Host: the caller walks the heap order; the state is set per entry
// because entries of the legacy path can sit between two of these.
static void draw_object_prelit_transparent(const Mode3Sec& sec) {
  if (!g_dev) return;
  Mode3Scope scope;
  scope.begin();
  g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
  g_dev->SetRenderState(D3DRS_ALPHAREF, 4);
  set_object_world(sec.key, *sec.ms);
  draw_mode3_section(sec.ms->subs[static_cast<size_t>(sec.sub)], true);
  ++g_cam_pass_stats.prelit_transparent_mode3_sections;
  scope.end();
}

// Fork: one `[render-dbg]` line per ground mesh (render_d3d9_debug_dump).
void render_d3d9_ground_debug_dump(FILE* out) {
  if (!out) return;
  for (const auto& kv : g_meshes) {
    const MeshState& ms = kv.second;
    if (ms.group != kMeshGroupGround) continue;
    size_t verts = 0, tris = 0;
    int unbound = 0;
    auto ready = [](void* key) {
      if (!key) return false;
      auto tit = g_textures.find(key);
      return tit != g_textures.end() && texture_d3d_base(tit->second);
    };
    for (const MeshSubmesh& sm : ms.subs) {
      verts += sm.wide.size();
      tris += sm.indices.size() / 3;
    }
    for (const MeshSubmesh& sm : ms.materials) {
      if (sm.slot_primary != -1 && !ready(sm.texture_key)) ++unbound;
      if (shader_is_mix_vca(sm.mat_shader) && sm.slot_second != -1 &&
          !ready(sm.texture_second))
        ++unbound;
      if (sm.slot_lightmap != -1 && !ready(sm.texture_lightmap)) ++unbound;
    }
    std::fprintf(out,
                 "[render-dbg] ground mesh %p '%s' ready=%d unit=%g "
                 "materials=%zu sections=%zu vertices=%zu triangles=%zu "
                 "bounds=(%.3f,%.3f,%.3f)..(%.3f,%.3f,%.3f) "
                 "unbound_entries=%d drawn_sections=%d default_tex_binds=%d "
                 "caps: multiply_add=%d lerp=%d\n",
                 kv.first, ms.label.c_str(), ms.ready ? 1 : 0, ms.unit,
                 ms.materials.size(), ms.subs.size(), verts, tris, ms.bmin[0],
                 ms.bmin[1], ms.bmin[2], ms.bmax[0], ms.bmax[1], ms.bmax[2],
                 unbound, g_cam_pass_stats.ground_sections,
                 g_cam_pass_stats.ground_default_tex_binds,
                 mode3_caps().multiply_add ? 1 : 0, mode3_caps().lerp ? 1 : 0);
  }
}

// ---------------------------------------------------------------------------
// Scene lights (PE 940 cLight)
// ---------------------------------------------------------------------------
// The exe, per camera and frame:
//   FUN_0045f880 walks the camera's light list (cam+0x6BFC): FUN_004592d0
//   re-reads each light from its type's payload and its instance's world
//   matrix; a light that is neither off (cLight+0x38) nor fake (+0x3C)
//   is copied into the device table (C3DLow vt+0x110) and gets its index;
//   FUN_0045a2c0 selects, per sector of lit instances, the first
//   directional light and up to four spot and four point lights whose
//   range reaches the sector (FUN_0045a120) - and nothing at all when the
//   camera has no directional light;
//   FUN_0045c8f0 -> FUN_00427110 at draw: SetLight / LightEnable for the
//   selected lights in the order directional, spots, points (eight at
//   most; cLight types 4 and 5 are sent as point and spot), the other
//   slots off.
// Host: the scene lights a camera sees are its list (camera_sees, the
// order they were made); one selection for the whole pass.
// Fork: no sector, so no range test: every point and spot light the camera
// sees counts (four of each). A light whose instance is hidden (node flag
// 0x400000 -> cLight+0x38) and a spot whose node has flag 0x2000000 (fake)
// are not told apart from the others; no host instance carries those flags.
namespace {

std::vector<D3DLIGHT9> g_last_cam_lights;  // of the last pass that saw one
int g_last_cam_lights_seen = 0;

// PE 940 FUN_004592d0 for one light: the D3DLIGHT9 at cLight+0x48.
// Position = payload position through the instance's bone00 world matrix,
// direction = payload direction through its rotation, normalised.
// Fork: units. The exe hands `position` and `range` to D3D as they are and
// multiplies them by 0.1 for the node bounds (FUN_00458740), so they are
// its engine units (metres x 10) [inferred: M2 spec step 5 item 4, not
// checked against a frame; no point or spot light is in the garage]. The
// host world is metres: position and range x 0.1, and the attenuation
// factors x 10 and x 100 so that the falloff over a distance stays the
// same. A directional light has neither.
bool scene_light_record(void* key, const RenderLightDesc& d, D3DLIGHT9* out) {
  if (d.type != 1 && d.type != 2 && d.type != 3) return false;
  D3DMATRIX w{};
  resolve_world(key, &w, 0);
  constexpr float kUnit = 0.1f;  // DAT_005935d4
  const float px = d.position[0] * kUnit, py = d.position[1] * kUnit,
              pz = d.position[2] * kUnit;
  D3DLIGHT9 l{};
  l.Type = d.type == 3 ? D3DLIGHT_DIRECTIONAL
                       : (d.type == 1 ? D3DLIGHT_POINT : D3DLIGHT_SPOT);
  l.Diffuse.r = d.diffuse[0];
  l.Diffuse.g = d.diffuse[1];
  l.Diffuse.b = d.diffuse[2];
  l.Diffuse.a = d.diffuse[3];
  l.Specular.r = d.specular[0];
  l.Specular.g = d.specular[1];
  l.Specular.b = d.specular[2];
  l.Specular.a = d.specular[3];
  l.Ambient.r = d.ambient[0];
  l.Ambient.g = d.ambient[1];
  l.Ambient.b = d.ambient[2];
  l.Ambient.a = d.ambient[3];
  l.Position.x = px * w.m[0][0] + py * w.m[1][0] + pz * w.m[2][0] + w.m[3][0];
  l.Position.y = px * w.m[0][1] + py * w.m[1][1] + pz * w.m[2][1] + w.m[3][1];
  l.Position.z = px * w.m[0][2] + py * w.m[1][2] + pz * w.m[2][2] + w.m[3][2];
  float dx = d.direction[0] * w.m[0][0] + d.direction[1] * w.m[1][0] +
             d.direction[2] * w.m[2][0];
  float dy = d.direction[0] * w.m[0][1] + d.direction[1] * w.m[1][1] +
             d.direction[2] * w.m[2][1];
  float dz = d.direction[0] * w.m[0][2] + d.direction[1] * w.m[1][2] +
             d.direction[2] * w.m[2][2];
  const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (len != 0.f) {
    dx /= len;
    dy /= len;
    dz /= len;
  }
  l.Direction.x = dx;
  l.Direction.y = dy;
  l.Direction.z = dz;
  l.Range = d.range * kUnit;
  l.Falloff = d.falloff;
  l.Attenuation0 = d.attenuation[0];
  l.Attenuation1 = d.attenuation[1] / kUnit;
  l.Attenuation2 = d.attenuation[2] / (kUnit * kUnit);
  l.Theta = d.theta;
  l.Phi = d.phi;
  *out = l;
  return true;
}

// The states the lit groups get when the camera has lights: the frame
// defaults of the exe (FUN_00426d10).
constexpr size_t kNumLightRs = 8;
const D3DRENDERSTATETYPE kLightRs[kNumLightRs] = {
    D3DRS_AMBIENT,               D3DRS_COLORVERTEX,
    D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE,
    D3DRS_AMBIENTMATERIALSOURCE, D3DRS_EMISSIVEMATERIALSOURCE,
    D3DRS_LOCALVIEWER,           D3DRS_SPECULARENABLE};
DWORD g_light_saved_rs[kNumLightRs] = {};

}  // namespace

void render_d3d9_scene_light_set(void* instance_key, int32_t type_id,
                                 const RenderLightDesc* desc) {
  if (!instance_key) return;
  if (!desc) {
    g_scene_lights.erase(instance_key);
    return;
  }
  auto it = g_scene_lights.find(instance_key);
  if (it == g_scene_lights.end()) {
    SceneLight sl;
    sl.type_id = type_id;
    sl.desc = *desc;
    sl.seq = ++g_scene_light_seq;
    g_scene_lights.emplace(instance_key, sl);
  } else {
    it->second.type_id = type_id;
    it->second.desc = *desc;
  }
}

void render_d3d9_scene_light_update_type(int32_t type_id,
                                         const RenderLightDesc* desc) {
  if (!type_id || !desc) return;
  for (auto& kv : g_scene_lights)
    if (kv.second.type_id == type_id) kv.second.desc = *desc;
}

// PE 940 FUN_0045f880 (refresh) + FUN_0045a2c0 (selection) for one camera.
static void collect_camera_lights(void* root, CamLights* out) {
  out->any = false;
  out->device.clear();
  if (g_scene_lights.empty() || !root) return;
  std::vector<std::pair<uint32_t, void*>> seen;
  for (const auto& kv : g_scene_lights)
    if (camera_sees(root, kv.first)) seen.emplace_back(kv.second.seq, kv.first);
  if (seen.empty()) return;
  std::sort(seen.begin(), seen.end());
  out->any = true;
  bool have_directional = false;
  D3DLIGHT9 directional{};
  // FUN_0045a2c0 keeps the point and spot lights in two lists: the
  // dynamic ones (cLight types 4 / 5) are walked before the static ones
  // (1 / 2, `lit_dynamic 0` or no such line).
  std::vector<D3DLIGHT9> dynamic_list, static_list;
  const bool saved_fold = g_fold_own_bone0;
  g_fold_own_bone0 = true;  // the instance's own bone00 pose counts
  for (const auto& s : seen) {
    const RenderLightDesc& d = g_scene_lights[s.second].desc;
    D3DLIGHT9 l{};
    // Types 0, 4 (fakepoint) and 5 (fakespot) never reach the device.
    if (!scene_light_record(s.second, d, &l)) continue;
    if (d.type == 3) {
      if (!have_directional) {  // the first one only
        directional = l;
        have_directional = true;
      }
    } else {
      (d.is_static ? static_list : dynamic_list).push_back(l);
    }
  }
  g_fold_own_bone0 = saved_fold;
  // Without a directional light the exe's list stays empty: the lit groups
  // then get no device light at all.
  if (have_directional) {
    std::vector<D3DLIGHT9> spots, points;
    for (const std::vector<D3DLIGHT9>* list : {&dynamic_list, &static_list})
      for (const D3DLIGHT9& l : *list) {
        if (l.Type == D3DLIGHT_POINT) {
          if (points.size() < 4) points.push_back(l);
        } else if (spots.size() < 4) {
          spots.push_back(l);
        }
      }
    out->device.push_back(directional);
    out->device.insert(out->device.end(), spots.begin(), spots.end());
    out->device.insert(out->device.end(), points.begin(), points.end());
    if (out->device.size() > 8) out->device.resize(8);  // FUN_00427110
  }
  g_last_cam_lights = out->device;
  g_last_cam_lights_seen = static_cast<int>(seen.size());
}

// PE 940 FUN_00427110 (C3DLow vt+0x114) with the frame defaults of
// FUN_00426d10: D3DRS_AMBIENT = C3DLow+0x30, which is 0 from the
// constructor and only ever re-sent (the scene's ambient light is the
// Ambient term of the lights), COLORVERTEX 1, the four material sources
// MATERIAL, LOCALVIEWER 1, SPECULARENABLE 0.
// Fork: NORMALIZENORMALS stays on (draw_camera_instances): the host scales
// in the world matrix where the exe scaled the vertices at load. The
// states are put back by end_camera_lights (the exe resets its state block
// once a frame).
static void apply_camera_lights(const CamLights& lights) {
  for (size_t i = 0; i < kNumLightRs; ++i)
    g_dev->GetRenderState(kLightRs[i], &g_light_saved_rs[i]);
  const DWORD n = static_cast<DWORD>(lights.device.size());
  for (DWORD i = 0; i < 8; ++i) {
    if (i < n) {
      g_dev->SetLight(i, &lights.device[i]);
      g_dev->LightEnable(i, TRUE);
    } else {
      g_dev->LightEnable(i, FALSE);
    }
  }
  g_dev->SetRenderState(D3DRS_AMBIENT, 0);
  g_dev->SetRenderState(D3DRS_COLORVERTEX, TRUE);
  g_dev->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
  g_dev->SetRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
  g_dev->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
  g_dev->SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
  g_dev->SetRenderState(D3DRS_LOCALVIEWER, TRUE);
  g_dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
}

static void end_camera_lights() {
  for (DWORD i = 0; i < 8; ++i) g_dev->LightEnable(i, FALSE);
  for (size_t i = 0; i < kNumLightRs; ++i)
    g_dev->SetRenderState(kLightRs[i], g_light_saved_rs[i]);
}

void render_d3d9_scene_light_debug_dump(FILE* out) {
  if (!out) return;
  if (g_scene_lights.empty() && g_cam_pass_stats.light_passes == 0) return;
  std::fprintf(out,
               "[render-dbg] scene lights=%zu light_passes=%d; last pass "
               "with lights: seen=%d device=%zu\n",
               g_scene_lights.size(), g_cam_pass_stats.light_passes,
               g_last_cam_lights_seen, g_last_cam_lights.size());
  for (size_t i = 0; i < g_last_cam_lights.size(); ++i) {
    const D3DLIGHT9& l = g_last_cam_lights[i];
    std::fprintf(out,
                 "[render-dbg]   device light %zu: %s diffuse=(%.3f, %.3f, "
                 "%.3f) ambient=(%.3f, %.3f, %.3f) specular=(%.3f, %.3f, "
                 "%.3f) direction=(%.3f, %.3f, %.3f) position=(%.3f, %.3f, "
                 "%.3f) range=%g attenuation=(%g, %g, %g)\n",
                 i,
                 l.Type == D3DLIGHT_DIRECTIONAL
                     ? "directional"
                     : (l.Type == D3DLIGHT_POINT ? "point" : "spot"),
                 l.Diffuse.r, l.Diffuse.g, l.Diffuse.b, l.Ambient.r,
                 l.Ambient.g, l.Ambient.b, l.Specular.r, l.Specular.g,
                 l.Specular.b, l.Direction.x, l.Direction.y, l.Direction.z,
                 l.Position.x, l.Position.y, l.Position.z, l.Range,
                 l.Attenuation0, l.Attenuation1, l.Attenuation2);
  }
}

}  // namespace inv
