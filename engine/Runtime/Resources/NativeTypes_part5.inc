// Fragment 5 of NativeTypes.cpp - included, not compiled on its own.
//
// The part type's slots and its `install` command (M2 step 6; spec Q11,
// read in Ghidra). `_type_part` 0x59DC54 vt+0x38 = FUN_004c4400, event 0x10
// (GameRef.command), first word `install`:
//
//   install <cursor> <root id> <my slot> <target id> <target slot> [x y z]
//   scanned with "%*s %d %d %d %d %d %f %f %f"
//
//   - <root id>: the node the part is moved under. The handle part+0x60 is
//     bound to it as a restype-1 node (FUN_0051fa40(id, 1, 0)); FUN_004cb1d0
//     relinks the part's node there (FUN_004ec210) before anything else,
//     which is what Part.addPart tests (`xa.getParentID() != rootID`).
//   - <target id>, <target slot>: the part to install on and its slot;
//     slot 0 = whichever free slot of the target takes one of mine.
//   - <my slot>: scanned and never read. The slot of the installing part
//     is the one FUN_004c96c0 finds (the variable is overwritten with its
//     result, asm 0x4c459b).
//   - <cursor>: scanned and never read here.
//   - x y z: a place in the world, used only when all three numbers were
//     scanned (the count is at least 8): among the slots that fit, the one
//     nearest to it.
//
// Which slots fit (FUN_004c96c0 -> FUN_004c9af0 -> FUN_004c89f0 ->
// FUN_004c9d90): two slots can be joined when the `attach` list of one
// names the other's type and slot id, or when an `attach` entry of one is
// also on the `compatible` list of the other. A slot is free when its
// handle is unbound and it is not disabled. The target is tried first;
// when it gives nothing, the root (a part, or a car and then every part on
// that car).
//
// Where the part ends up (FUN_004cb1d0 -> FUN_004c1a10 -> FUN_004eac50):
//   pose(my reference body) =
//       pose(target's reference body) * target slot * inverse(my slot)
// with both slots in the frame of their part's reference body
// (part_type_rebase_slots). Its render instances follow that body as
// before (FUN_004c2b10), so a part needs no offset of its own for being
// installed. For the garage lift: both slots are written (0, 0, 0), which
// puts them at minus the mass centre of each part's hull, and the arm's
// mesh origin lands on the support's mesh origin.
//
// When the two slots have no bolts the install is finished at once
// (FUN_004c7d20): the slot entries of both parts are marked (1 on the
// installed part, 2 on the target), the installed part's compound becomes
// a child body of the target's compound, whose origin moves to the new
// mass centre (FUN_005032d0), the part takes the target's car (getinfo
// 0x2B), announces itself to it with `add_part <id>` (FUN_004ca390 ->
// FUN_004c17b0, which calls the script's addpart(GameRef)), and the script
// instance's install() is called (FUN_0047dd50).
//
// Fork, in short (each is marked again where it applies):
//   - no bodies: the slot entries are the link. A part installed on
//     another is put on its slot again whenever that part's pose changes;
//   - an install whose slots have bolts is finished at once as well (the
//     exe leaves it waiting, slot flag 4, until its bolts are tightened;
//     that path was not read);
//   - the `car` type has no host object. An install whose target or root is
//     a node of a native type without host object cannot be matched; the
//     host assumes the exe finds a slot there and only moves the part's
//     node under the root.
// Not ported: `remove`, `cancel`, `repair`, `flap_toggle`, `texture`,
// `bigdecal`, `decal`, `paint`, `colorscheme`, `rem_part`, `filter` (the
// host's generic chain sees them as before), bone01 (type+0x11F8), the
// state a slot inherits from the target (part+0xB0, part+0x120), the car's
// own lists (`add_light`, `add_wing`, `add_rollbar`, `add_mslot`,
// `add_linked`, `updatevariables`).

namespace {

constexpr float kPartFar = 100000000.f;  // _DAT_0059f98c

struct PartInstallStats {
  int matched = 0;   // installed on a slot of a part
  int assumed = 0;   // Fork: target or root is a type without host object
  int failed = 0;    // no slot fits
  int destroyed_children = 0;
};
PartInstallStats g_part_install_stats;

// The part object of a node object; null for every other node.
std::shared_ptr<PartObject> part_of(InvObject* node) {
  if (!node) return nullptr;
  std::shared_ptr<NativeObject> o = native_object_of(node);
  if (!o || o->type_index != kNativePart) return nullptr;
  return std::static_pointer_cast<PartObject>(o);
}

// PE 940 FUN_0051fa40(id, 1, 0): a handle on the restype-1 node with that
// id. Host: the node object of a node GameRef.create made, else the handle
// that built a pack instance. Fork: a pack instance nothing requested yet
// is not found.
InvObject* part_node_by_id(int32_t id) {
  if (!id) return nullptr;
  if (InvObject* o = script_node_object(id)) return o;
  return instance_node_handle(id);
}

int32_t part_node_id(InvObject* node) {
  if (!node) return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_res.find(node);
  return it == g_res.end() ? 0 : it->second.id;
}

// True when the two host objects are the same node (a node has two).
bool part_same_node(InvObject* a, InvObject* b) {
  if (a == b) return true;
  if (!a || !b) return false;
  const int32_t ia = part_node_id(a);
  return ia != 0 && ia == part_node_id(b);
}

// True when the node is an instance of a native type the host has no object
// for (`car`, `object`, ...): the exe would ask that type's game object.
bool part_node_unported(InvObject* node) {
  if (!node) return false;
  int32_t type_id = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_res.find(node);
    if (it == g_res.end() || it->second.restype != 1) return false;
    type_id = it->second.type_id;
  }
  if (!type_id) return false;
  const GameTypeDesc* d = gametype_desc(type_id);
  return d->found && d->native_index >= 0 &&
         kNativeTypes[d->native_index].create == nullptr;
}

const PartSlot* part_type_slot(const PartTypeDesc& d, int32_t id) {
  for (const PartSlot& s : d.slots)  // FUN_004c6f90
    if (s.id == id) return &s;
  return nullptr;
}

// PE 940 FUN_004c9d90(type A, slot of A, type B, slot of B): can the two
// slots be joined?
bool part_slots_fit(const PartTypeDesc& A, int32_t sa, const PartTypeDesc& B,
                    int32_t sb) {
  if (A.type_id == B.type_id && sa == sb) return false;
  // Fork: the exe reads the lists of a slot it did not find (a null
  // pointer); the host takes a missing slot as one with empty lists.
  static const PartSlot kNone;
  const PartSlot* a = part_type_slot(A, sa);
  const PartSlot* b = part_type_slot(B, sb);
  if (!a) a = &kNone;
  if (!b) b = &kNone;
  for (const PartSlot::Attach& x : a->attach)
    if (x.type_id == B.type_id && x.slot == sb) return true;
  for (const PartSlot::Attach& x : b->attach)
    if (x.type_id == A.type_id && x.slot == sa) return true;
  for (const PartSlot::Attach& x : a->attach)
    for (const PartSlot::Attach& c : b->compatible)
      if (x.type_id == c.type_id && x.slot == c.slot) return true;
  for (const PartSlot::Attach& x : b->attach)
    for (const PartSlot::Attach& c : a->compatible)
      if (x.type_id == c.type_id && x.slot == c.slot) return true;
  return false;
}

// PE 940 FUN_004cb0b0: true when the slot the part is installed through is
// locked (flags 0x10 and 0x40), or the part it sits on says so.
bool part_chain_locked(PartObject* p, int depth) {
  for (const PartObject::Slot& s : p->slots_) {
    if (!(s.flags & 1u)) continue;
    if ((s.flags & 0x50u) == 0x50u) return true;
    if (!s.other) continue;
    std::shared_ptr<PartObject> up = part_of(s.other);
    return (up && depth < 64) ? part_chain_locked(up.get(), depth + 1) : false;
  }
  return false;
}

// PE 940 FUN_004c6e50: where a slot is in the world: the position of the
// body it is on plus that body's rotation times the slot's position.
bool part_slot_world(PartObject* p, int32_t slot_id, CamV3* out) {
  const PartObject::Slot* s = p->slot(slot_id);
  if (!s) return false;  // Fork: the exe reads through a null entry
  *out = p->pos_ + cam_mul(p->rot_, s->pos);
  return true;
}

inline float part_dist2(CamV3 p, const float* h) {
  const float dx = p.x - h[0], dy = p.y - h[1], dz = p.z - h[2];
  return dx * dx + dy * dy + dz * dz;
}

// PE 940 FUN_004c89f0: the free slot of `target` that takes slot `my_slot`
// of a part of type `my`; with a hint the nearest one. 0 = none.
int32_t part_free_slot_for(PartObject* target, const PartTypeDesc& my,
                           int32_t my_slot, const float* hint) {
  if (part_chain_locked(target, 0)) return 0;
  int32_t best = 0;
  float best_d = kPartFar;
  for (const PartObject::Slot& t : target->slots_) {
    if (t.other || (t.flags & 0x10u)) continue;
    if (!part_slots_fit(*target->type_, t.id, my, my_slot)) continue;
    if (!hint) return t.id;
    CamV3 at;
    if (!part_slot_world(target, t.id, &at)) continue;
    const float d = part_dist2(at, hint);
    if (d < best_d) {
      best = t.id;
      best_d = d;
    }
  }
  return best;
}

// PE 940 FUN_004c9af0: which of my slots goes on `target`? `*tslot` 0 = any
// free slot of the target (the one found is written back). Returns the id
// of my slot, 0 when nothing fits.
int32_t part_match_target(PartObject* me, PartObject* target, int32_t* tslot,
                          const float* hint) {
  // getinfo 0x27 of the target: slots of its type minus slots in use.
  if (static_cast<int>(target->slots_.size()) - target->installed_ < 1)
    return 0;
  if (target == me) return 0;  // the same node
  const int32_t want = *tslot;
  if (want == 0) {
    int32_t found = 0;
    float best_d = kPartFar;
    for (const PartObject::Slot& s : me->slots_) {
      // My slot is free, or it is the one I am installed through already.
      if (!(s.other == nullptr || (s.flags & 1u)) || (s.flags & 0x10u))
        continue;
      const int32_t t = part_free_slot_for(target, *me->type_, s.id, hint);
      if (!t) continue;
      if (!hint) {
        *tslot = t;
        return s.id;
      }
      CamV3 at;
      if (!part_slot_world(target, t, &at)) continue;
      const float d = part_dist2(at, hint);
      if (d < best_d) {
        *tslot = t;
        found = s.id;
        best_d = d;
      }
    }
    return found;
  }
  // FUN_004c9fe0: the slot asked for exists on the target, is free and not
  // disabled, and the target's chain is not locked.
  bool free_slot = false;
  if (!part_chain_locked(target, 0))
    for (const PartObject::Slot& t : target->slots_)
      if (t.id == want && !t.other && !(t.flags & 0x10u)) {
        free_slot = true;
        break;
      }
  if (!free_slot) return 0;
  for (const PartObject::Slot& s : me->slots_)
    if (!s.other && !(s.flags & 0x10u) &&
        part_slots_fit(*me->type_, s.id, *target->type_, want))
      return s.id;
  return 0;
}

// PE 940 FUN_004c96c0: the pair of slots for an install. `*target` and
// `*tslot` come from the command and are replaced by what was found.
// Returns the id of my slot, 0 when nothing fits. `*assume` is set when the
// answer depends on a type the host has no object for.
int32_t part_find_install(PartObject* me, InvObject** target, int32_t* tslot,
                          const float* hint, bool* assume) {
  int32_t want = *tslot;
  int32_t mine = 0;
  std::shared_ptr<PartObject> tp;
  if (*target) {
    tp = part_of(*target);
    if (tp) {
      // The target itself, without the hint.
      mine = part_match_target(me, tp.get(), &want, nullptr);
      if (mine) {
        *tslot = want;
        return mine;
      }
    } else if (part_node_unported(*target)) {
      *assume = true;
      return 0;
    }
  }
  // The root: FUN_0047c640(root, 7) is 9 for a part, 5 for a car.
  InvObject* const root = me->root_;
  if (!root) return 0;
  std::shared_ptr<PartObject> rp = part_of(root);
  if (!rp) {
    // A car: its own slots, then every part on it (getinfo 0x31).
    // Fork: not ported with the car type.
    if (part_node_unported(root)) *assume = true;
    return 0;
  }
  float best_d = kPartFar;
  const int32_t r = part_match_target(me, rp.get(), &want, hint);
  if (r) {
    if (!hint) {
      *target = root;
      *tslot = want;
      return r;
    }
    CamV3 at;
    if (part_slot_world(rp.get(), want, &at)) {
      const float d = part_dist2(at, hint);
      if (d < best_d) {
        best_d = d;
        *target = root;
        *tslot = want;
        mine = r;
      }
    }
  }
  if (!mine) {
    // Fork: Part.getCar still answers from the host's legacy tables, where a
    // part that sits on a car names itself, so the scripts pass that part
    // as root where the exe gets the car and would go on to search the
    // whole car. When the part tried here sits on a car the host has no
    // object for, the answer is the car's.
    for (PartObject* p : {tp.get(), rp.get()})
      if (p && p->car_ && p->car_ != p->key_ && part_node_unported(p->car_))
        *assume = true;
  }
  return mine;
}

// PE 940 FUN_004c1a10: the pose of my reference body for an install of my
// slot on the target's slot: the pose of the body the target's slot is on
// (getinfo 0x2F: its reference body), times the target slot, times the
// inverse of my slot. Returns the number of bolts of the two slots.
int part_install_pose(PartObject* me, int32_t my_slot, PartObject* target,
                      int32_t target_slot, CamV3* pos, CamM3* rot) {
  CamV3 p = target->pos_;
  CamM3 R = target->rot_;
  int bolts = 0;
  // Fork: the exe reads a slot it does not find through a null pointer; the
  // host leaves that side out.
  if (const PartObject::Slot* t = target->slot(target_slot)) {
    p = p + cam_mul(R, t->pos);
    R = cam_mm(R, t->rot);  // FUN_00567020
  }
  if (const PartSlot* tt = part_type_slot(*target->type_, target_slot))
    bolts += tt->bolts;
  if (const PartObject::Slot* m = me->slot(my_slot)) {
    R = cam_mm(R, part_transpose(m->rot));  // FUN_00567380, FUN_00567020
    p = p - cam_mul(R, m->pos);
  }
  if (const PartSlot* mt = part_type_slot(*me->type_, my_slot))
    bolts += mt->bolts;
  *pos = p;
  *rot = R;
  return bolts;
}

// PE 940 FUN_0047dd50: a method of the node's script instance by name (the
// exe makes a THRD-RUNVMI thread for it: at once for `install`, queued for
// `addpart`).
// Fork: called like the script constructor in GameRef.create, at once in
// both cases, and only when the override that would run has a body (an
// empty one, like Part.install, changes nothing).
void part_script_call(InvObject* obj, const char* name, const char* sig,
                      const std::vector<JvmValue>& extra) {
  Jvm* j = jvm_active();
  const char* cn = obj ? tree_host_class(obj) : nullptr;
  if (!j || !cn || !cn[0]) return;
  const JvmClass* c = j->find_class(cn);
  for (int guard = 0; c && guard < 32; ++guard) {
    if (const JvmMethod* m = j->find_method(*c, name, sig)) {
      const bool body =
          m->tree_index >= 0 &&
          static_cast<size_t>(m->tree_index) < c->trees.size() &&
          !c->trees[static_cast<size_t>(m->tree_index)].nodes.empty();
      if (!body) return;
      std::vector<JvmValue> args;
      args.push_back(JvmValue::make_obj(obj));
      for (const JvmValue& a : extra) args.push_back(a);
      j->invoke(c->name.c_str(), name, sig, args, false);
      return;
    }
    if (c->super_name.empty()) break;
    c = j->find_class(c->super_name.c_str());
  }
}

// PE 940 FUN_004c17b0, the `add_part <id>` command a part sends to its car
// when it is installed: the car keeps a handle on the part (part+0xDC) and
// its script instance gets addpart(GameRef) with a new handle on the part.
void part_add_part(PartObject* car, int32_t part_id) {
  InvObject* node = part_node_by_id(part_id);
  if (std::shared_ptr<PartObject> p = part_of(node)) node = p->key_;
  if (node) {
    bool known = false;
    for (InvObject* a : car->attached_)
      if (a == node) known = true;
    // Fork: one entry per part (the exe appends a node per command).
    if (!known) car->attached_.push_back(node);
  }
  bool scripted = false;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto it = g_refs.find(car->key_);
    scripted = it != g_refs.end() && !it->second.script_class.empty();
  }
  if (!scripted) return;  // payload+0x50 == 0
  InvObject* ref = gameref_new();
  java_util_resource_ResourceRef_set(ref, part_id);
  part_script_call(car->key_, "addpart", "(Ljava.util.resource.GameRef;)V",
                   {JvmValue::make_obj(ref)});
}

// PE 940 FUN_004ca390: after an install the part announces itself to the
// car it now sits on, and so does every part installed on it.
void part_announce(PartObject* me, InvObject* car, int depth) {
  me->car_ = car;
  if (car && !part_same_node(car, me->key_)) {
    // FUN_0047d550(part+0xC0, 0, 0x10, "add_part <id>", 0).
    // Fork: only a car that is a part gets the command. The host's generic
    // `add_part` branch (the stand-in for the car type's) makes a new part
    // from the number, which is not what this command asks for.
    if (std::shared_ptr<PartObject> cp = part_of(car))
      part_add_part(cp.get(), part_node_id(me->key_));
  }
  if (depth > 64) return;
  for (const PartObject::Slot& s : me->slots_) {
    if (!s.other || !(s.flags & 2u)) continue;
    if (std::shared_ptr<PartObject> c = part_of(s.other))
      part_announce(c.get(), car, depth + 1);
  }
}

// PE 940 FUN_004cb6c0: every part installed on `me` goes under the node
// `me` hangs under, is put on its slot again (FUN_004cb8d0) and takes the
// state of `me`; and so on down the tree.
void part_adopt_subtree(PartObject* me, int depth) {
  if (depth > 64) return;
  InvObject* const parent = gameref_get_parent(me->key_);
  for (const PartObject::Slot& s : me->slots_) {
    if (!s.other || !(s.flags & 2u)) continue;
    std::shared_ptr<PartObject> c = part_of(s.other);
    if (!c) continue;
    c->pose_on_slot(depth + 1);
    if (parent && !part_same_node(parent, c->key_))
      gameref_node_set_parent(c->key_, parent);
    c->state_ = me->state_;
    part_adopt_subtree(c.get(), depth + 1);
  }
}

// PE 940 FUN_004c7d20: the install is finished.
void part_install_finish(PartObject* me) {
  PartObject* first = nullptr;
  std::shared_ptr<PartObject> first_keep;
  for (PartObject::Slot& s : me->slots_) {
    if (!s.other || !(s.flags & 4u)) continue;
    s.flags = (s.flags & ~4u) | 1u;
    ++me->installed_;
    std::shared_ptr<PartObject> tp = part_of(s.other);
    if (tp) {
      if (PartObject::Slot* t = tp->slot(s.other_slot))
        t->flags = (t->flags & ~4u) | 2u;
      ++tp->installed_;
      // Not ported: FUN_004c7060(target, 1) (part+0x120) and the word the
      // part takes over from the target (part+0xB0, FUN_004c9530).
      if (!first) {
        first = tp.get();
        first_keep = tp;
      }
    }
  }
  // The bodies: my compound becomes a child body of the compound of the
  // body the target's slot is on (FUN_00501d00, FUN_004f7b20), whose origin
  // then moves to the mass centre of all its children (FUN_005032d0).
  // Fork: no bodies. The slot entries say what sits on what: the target
  // puts me on its slot again whenever its pose changes
  // (PartObject::set_ref_pose) and answers getinfo 2 with the mass centre
  // of the whole tree (PartObject::compound_pos).
  part_adopt_subtree(me, 0);
  // getinfo 0x2B of the target: the car it sits on becomes mine, and the
  // part and everything on it announce themselves there (FUN_004ca390).
  if (first) part_announce(me, first->car_, 0);
  // FUN_0047dd50(node, "install"): Part.install(), "called after
  // installing".
  part_script_call(me->key_, "install", "()V", {});
  me->root_ = nullptr;  // part+0x60 is unbound again
}

// PE 940 FUN_004cb1d0: the part goes on the slot.
void part_install(PartObject* me, int32_t my_slot, InvObject* target_node,
                  PartObject* target, int32_t target_slot) {
  // The part's node goes under the root, with its render instances
  // (FUN_004ec210; they hang under the part's node already here).
  if (me->root_ && !part_same_node(me->root_, me->key_))
    gameref_node_set_parent(me->key_, me->root_);
  me->state_ = 1;
  CamV3 pos;
  CamM3 rot = cam_identity();
  const int bolts =
      part_install_pose(me, my_slot, target, target_slot, &pos, &rot);
  // FUN_004eac50 on part+0x2C (the compound's handle where the type has no
  // reference body): the pose of that body, by moving the root of its tree.
  me->set_ref_pose(pos, rot, nullptr, 0);
  // The handles are on the nodes; the host keeps each part's own key (the
  // object its render instances hang under), whichever of a node's two
  // host objects the command named.
  (void)target_node;
  if (PartObject::Slot* m = me->slot(my_slot)) {
    m->flags |= 4u;
    m->other = target->key_;
    m->other_slot = target_slot;
  }
  if (PartObject::Slot* t = target->slot(target_slot)) {
    t->flags |= 4u;
    t->other = me->key_;
    t->other_slot = my_slot;
  }
  // Not ported: bone01 of a type with type+0x11F8.
  // The exe finishes the install here only when the two slots have no
  // bolts; otherwise both entries keep flag 4 until the bolts are in
  // (not read). Fork: the host has no bolts and finishes at once.
  (void)bolts;
  part_install_finish(me);
}

// vt+0x38 FUN_004c4400, event 0x10, `install ...`.
bool part_cmd_install(PartObject* me, const char* text) {
  int32_t cursor = 0, root_id = 0, my_slot_arg = 0, target_id = 0;
  int32_t target_slot = 0;
  float hint[3] = {0.f, 0.f, 0.f};
  const int n = engine_sscanf(text, "%*s %d %d %d %d %d %f %f %f", &cursor,
                              &root_id, &my_slot_arg, &target_id,
                              &target_slot, &hint[0], &hint[1], &hint[2]);
  (void)cursor;
  (void)my_slot_arg;  // never read by the exe either
  me->root_ = part_node_by_id(root_id);
  InvObject* target = part_node_by_id(target_id);
  bool assume = false;
  const int32_t mine = part_find_install(me, &target, &target_slot,
                                         n < 8 ? nullptr : hint, &assume);
  std::shared_ptr<PartObject> tp = mine ? part_of(target) : nullptr;
  const bool log = native_type_log();
  if (mine && tp) {
    // A tree that does not end in a car the host cannot place is logged
    // in full (the garage lift); the parts of a car only at first.
    const bool on_car = tp->car_ && tp->car_ != tp->key_ &&
                        part_node_unported(tp->car_);
    part_install(me, mine, target, tp.get(), target_slot);
    ++g_part_install_stats.matched;
    if (log && (!on_car || g_part_install_stats.matched <= 4)) {
      const RpakEntry* a = rpak_find_entry(me->type_->type_id);
      const RpakEntry* b = rpak_find_entry(tp->type_->type_id);
      std::fprintf(stderr,
                   "[native-type] install '%s' (node 0x%08X) slot %d on '%s' "
                   "(node 0x%08X) slot %d: reference body (%.4f, %.4f, %.4f) "
                   "ypr (%.4f, %.4f, %.4f), root 0x%08X\n",
                   a ? a->name.c_str() : "",
                   static_cast<unsigned>(part_node_id(me->key_)), mine,
                   b ? b->name.c_str() : "",
                   static_cast<unsigned>(part_node_id(tp->key_)), target_slot,
                   me->pos_.x, me->pos_.y, me->pos_.z, me->ypr_[0],
                   me->ypr_[1], me->ypr_[2], static_cast<unsigned>(root_id));
    }
    return true;
  }
  if (assume) {
    // Fork: the target or the root is an instance of a native type without
    // host object (`car`). The exe matches the part against that object's
    // slots and, for a car, against every part on it; the host assumes it
    // finds one and does the one thing of FUN_004cb1d0 that needs no slot:
    // the part's node goes under the root. No pose, no slot entry.
    InvObject* car = nullptr;
    if (target && part_node_unported(target)) car = target;
    if (!car && me->root_ && part_node_unported(me->root_)) car = me->root_;
    if (!car)
      for (InvObject* via : {target, me->root_})
        if (std::shared_ptr<PartObject> p = part_of(via))
          if (p->car_ && p->car_ != p->key_) car = p->car_;
    if (me->root_ && !part_same_node(me->root_, me->key_))
      gameref_node_set_parent(me->key_, me->root_);
    me->state_ = 1;
    if (car) part_announce(me, car, 0);
    me->root_ = nullptr;
    ++g_part_install_stats.assumed;
    if (log && g_part_install_stats.assumed <= 2) {
      const RpakEntry* a = rpak_find_entry(me->type_->type_id);
      std::fprintf(stderr,
                   "[native-type] install '%s' (node 0x%08X): target 0x%08X "
                   "/ root 0x%08X is, or sits on, an instance of a type "
                   "without host object; node moved under the root, no "
                   "slot\n",
                   a ? a->name.c_str() : "",
                   static_cast<unsigned>(part_node_id(me->key_)),
                   static_cast<unsigned>(target_id),
                   static_cast<unsigned>(root_id));
    }
    return true;
  }
  // Nothing fits: the exe leaves the part where it is (FUN_004c4400
  // returns 0) and the handle part+0x60 stays bound.
  ++g_part_install_stats.failed;
  if (log && g_part_install_stats.failed <= 8) {
    const RpakEntry* a = rpak_find_entry(me->type_->type_id);
    std::fprintf(stderr,
                 "[native-type] install '%s' (node 0x%08X): no slot fits "
                 "(target 0x%08X slot %d, root 0x%08X)\n",
                 a ? a->name.c_str() : "",
                 static_cast<unsigned>(part_node_id(me->key_)),
                 static_cast<unsigned>(target_id), target_slot,
                 static_cast<unsigned>(root_id));
  }
  return true;
}

// The mass of a part's own bodies and of every part installed on it, and
// the sum of mass times position (world).
void part_tree_mass(PartObject* p, float* mass, CamV3* sum, int depth) {
  const PartTypeDesc& d = *p->type_;
  if (d.total_mass > 0.f) {
    const CamM3 rc = cam_mm(p->rot_, part_transpose(d.ref_rot));
    const CamV3 c = p->pos_ + cam_mul(rc, d.com_raw - d.ref_at);
    *mass += d.total_mass;
    *sum = *sum + c * d.total_mass;
  }
  if (depth > 64) return;
  for (const PartObject::Slot& s : p->slots_) {
    if (!s.other || !(s.flags & 2u)) continue;
    if (std::shared_ptr<PartObject> c = part_of(s.other))
      part_tree_mass(c.get(), mass, sum, depth + 1);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// PartObject, the methods that need the slots
// ---------------------------------------------------------------------------
namespace {

void PartObject::set_ref_pose(const CamV3& pos, const CamM3& rot,
                              const float* ypr, int depth) {
  pos_ = pos;
  rot_ = rot;
  if (ypr) {
    ypr_[0] = ypr[0];
    ypr_[1] = ypr[1];
    ypr_[2] = ypr[2];
  } else {
    cam_matrix_to_ypr(rot_, &ypr_[0], &ypr_[1], &ypr_[2]);
  }
  push_pose();
  if (depth > 64) return;
  // Fork: in the exe the parts installed on this one are bodies of the same
  // compound and move with it; here each is put on its slot again.
  for (const Slot& s : slots_) {
    if (!s.other || !(s.flags & 2u)) continue;
    if (std::shared_ptr<PartObject> c = part_of(s.other))
      c->pose_on_slot(depth + 1);
  }
}

// PE 940 FUN_004cb8d0.
bool PartObject::pose_on_slot(int depth) {
  for (const Slot& s : slots_) {
    if (!s.other || !(s.flags & 1u)) continue;
    std::shared_ptr<PartObject> t = part_of(s.other);
    if (!t) continue;
    CamV3 p;
    CamM3 r = cam_identity();
    part_install_pose(this, s.id, t.get(), s.other_slot, &p, &r);
    set_ref_pose(p, r, nullptr, depth);
    return true;
  }
  return false;
}

// PE 940 FUN_005032d0, as far as the position goes: the compound's origin
// is the mass centre of its child bodies when that is more than 3.2 mm off
// (|C|^2 > 1e-5), and the compound of an installed part is one of the
// target's children. Fork: the 3.2 mm test is applied to a part's own
// bodies only (PartTypeDesc::com_shift); a tree with installed parts
// answers with the mass centre of all its bodies.
CamV3 PartObject::compound_pos() {
  const PartTypeDesc& d = *type_;
  const CamM3 rc = cam_mm(rot_, part_transpose(d.ref_rot));
  bool tree = false;
  for (const Slot& s : slots_)
    if (s.other && (s.flags & 2u)) tree = true;
  if (tree) {
    float mass = 0.f;
    CamV3 sum;
    part_tree_mass(this, &mass, &sum, 0);
    if (mass > 0.f) return sum * (1.f / mass);
  }
  const CamV3 origin = d.com_shift ? d.com_raw : CamV3();
  return pos_ + cam_mul(rc, origin - d.ref_at);
}

// vt+0x10 FUN_004c3a50: the parts installed on this one are destroyed with
// it (FUN_004e9450 on every slot handle with flag 2), then its bodies and
// render instances, then the parts on its `add_part` list.
void PartObject::destroy() {
  std::vector<InvObject*> doomed;
  for (const Slot& s : slots_)
    if (s.other && (s.flags & 2u)) doomed.push_back(s.other);
  for (InvObject* a : attached_) doomed.push_back(a);
  attached_.clear();
  // The handles other parts hold on this node go unbound with the node:
  // the slot of the part this one is installed on (its flag and the count
  // of slots in use stay as they are, as in the exe; `remove` is what
  // clears them), and the car's list.
  for (const Slot& s : slots_) {
    if (!s.other) continue;
    if (std::shared_ptr<PartObject> o = part_of(s.other))
      for (Slot& os : o->slots_)
        if (os.other == key_) os.other = nullptr;
  }
  if (car_ && car_ != key_)
    if (std::shared_ptr<PartObject> c = part_of(car_))
      for (size_t i = c->attached_.size(); i-- > 0;)
        if (c->attached_[i] == key_)
          c->attached_.erase(c->attached_.begin() +
                             static_cast<std::ptrdiff_t>(i));
  for (Slot& s : slots_) s.other = nullptr;
  for (Inst& i : insts_) {
    if (!i.r) continue;
    java_util_resource_ResourceRef_destroy(i.r);
    java_util_resource_ResourceRef_deleteNative(i.r);
    i.r = nullptr;
  }
  insts_.clear();
  render_d3d9_mesh_destroy(key_);  // the pose entry of the key
  for (InvObject* d : doomed) {
    if (!part_of(d)) continue;  // gone already
    ++g_part_install_stats.destroyed_children;
    java_util_resource_ResourceRef_destroy(d);
  }
}

// vt+0x1C FUN_004c6370 -> FUN_004eac50 on part+0x2C: the reference body
// gets that pose as it is (the position is the body's, not the mesh
// origin's), by moving the root of the body tree it is in (FUN_004e9730);
// the velocities are set and a body that is awake is put to sleep.
bool PartObject::set_matrix(const float pos[3], const float ypr[3]) {
  const CamV3 p = cam_v(pos[0], pos[1], pos[2]);
  const CamM3 r = cam_ypr_to_matrix(ypr[0], ypr[1], ypr[2]);
  // The topmost part of the tree this one is installed in.
  PartObject* top = this;
  std::shared_ptr<PartObject> keep;
  for (int depth = 0; depth < 64; ++depth) {
    std::shared_ptr<PartObject> up;
    for (const Slot& s : top->slots_)
      if (s.other && (s.flags & 1u)) {
        up = part_of(s.other);
        break;
      }
    if (!up) break;
    keep = up;
    top = up.get();
  }
  if (top == this) {
    set_ref_pose(p, r, ypr, 0);
    return true;
  }
  // The whole tree moves so that this part's body lands on the pose.
  const CamM3 d = cam_mm(r, part_transpose(rot_));
  const CamV3 tp = p + cam_mul(d, top->pos_ - pos_);
  top->set_ref_pose(tp, cam_mm(d, top->rot_), nullptr, 0);
  return true;
}

// vt+0x3C FUN_004c3da0: 1 the physics instance, 6 the type, 7 the kind of
// object (9), 0x17 the first render instance, 0x27 the free slots, 0x2B
// the car, 0x2F the body a slot is on, and more part queries (wear, mesh,
// texture). GameRef.getPos asks channel 2, which the engine answers from
// the physics instance of query 1: the compound `part_comp`.
bool PartObject::getinfo(int32_t query, NativeInfo* out) {
  switch (query) {
    case 2: {
      const CamV3 c = compound_pos();
      out->v[0] = c.x;
      out->v[1] = c.y;
      out->v[2] = c.z;
      return true;
    }
    case 7:
      out->i = 9;
      return true;
    case 0x17:
      out->obj = insts_.empty() ? nullptr : insts_[0].r;
      return out->obj != nullptr;
    case 0x27:
      out->i = static_cast<int32_t>(slots_.size()) - installed_;
      return true;
    case 0x2b:
      out->obj = car_;
      return car_ != nullptr;
    default:
      return false;  // 1: Fork, no physics instance; the rest: later
  }
}

// The rotation of the compound (GameRef.getOri reads the physics instance
// of getinfo 1).
bool PartObject::get_ori(float ypr[3]) {
  if (part_is_identity(type_->ref_rot)) {
    ypr[0] = ypr_[0];
    ypr[1] = ypr_[1];
    ypr[2] = ypr_[2];
    return true;
  }
  const CamM3 rc = cam_mm(rot_, part_transpose(type_->ref_rot));
  cam_matrix_to_ypr(rc, &ypr[0], &ypr[1], &ypr[2]);
  return true;
}

// vt+0x38 FUN_004c4400. Events 1 and 4 (damage), 0x80 and 0x10000 are not
// ported; of the commands (event 0x10) `install` and `add_part` are.
bool PartObject::event(int32_t type, const char* text) {
  if (type != 0x10) return false;
  char word[1025];
  word[0] = 0;
  engine_sscanf(text ? text : "", "%1024s", word);
  if (word_is(word, "install")) return part_cmd_install(this, text);
  if (word_is(word, "add_part")) {
    // "%*s %d" -> FUN_004c17b0.
    int32_t id = 0;
    engine_sscanf(text, "%*s %d", &id);
    part_add_part(this, id);
    return true;
  }
  return false;
}

}  // namespace

// One `[render-dbg]` line: what the `install` commands of the run came to.
void native_part_install_dump(FILE* out) {
  if (!out || !native_script_boot()) return;
  std::fprintf(out,
               "[render-dbg] part installs: %d on a slot, %d assumed (target "
               "or root is, or sits on, a type without host object), %d "
               "without a fitting slot; %d installed part(s) destroyed with "
               "their target\n",
               g_part_install_stats.matched, g_part_install_stats.assumed,
               g_part_install_stats.failed,
               g_part_install_stats.destroyed_children);
}
