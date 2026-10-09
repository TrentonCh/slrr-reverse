// Fragment 1/2 of jvm_vmthread.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


namespace inv {

// Fork diagnostic switch (SLRR_PE_STREAM_TRACE=1).
static bool vm_trace_on() {
  static const bool on = [] {
    const char* v = std::getenv("SLRR_PE_STREAM_TRACE");
    return v && v[0] == '1';
  }();
  return on;
}

namespace {
Jvm* g_active_jvm = nullptr;
}

void jvm_set_active(Jvm* j) { g_active_jvm = j; }
Jvm* jvm_active() { return g_active_jvm; }

namespace {

// PE g_CallFramePool @ 0x62DDF0 / CallFramePool_alloc @ 0x408D90:
// freelist at pool+0x10, else carve 64B from 0x4000 chunks (256 slots).
// Host: freelist of heap VmCallFrame (no PE chunk table).
struct CallFramePoolHost {
  std::vector<VmCallFrame*> freelist;
  std::vector<std::unique_ptr<VmCallFrame>> owned;
};
CallFramePoolHost g_callframe_pool;

VmCallFrame* callframe_pool_alloc() {
  if (!g_callframe_pool.freelist.empty()) {
    VmCallFrame* f = g_callframe_pool.freelist.back();
    g_callframe_pool.freelist.pop_back();
    return f;
  }
  auto up = std::make_unique<VmCallFrame>();
  VmCallFrame* p = up.get();
  g_callframe_pool.owned.push_back(std::move(up));
  return p;
}

void callframe_pool_release(VmCallFrame* f) {
  if (!f) return;
  // Fork diagnostic: a frame released twice would be handed out while
  // still live (caller frame aliasing seen in rpkScan #2).
  for (VmCallFrame* q : g_callframe_pool.freelist) {
    if (q == f) {
      std::fprintf(stderr, "[pe-stream] DOUBLE RELEASE of frame %p\n", static_cast<void*>(f));
      return;
    }
  }
  f->locals.clear();
  f->operand.clear();
  f->dllist_prev = nullptr;
  f->dllist_next = nullptr;
  f->pe = {};
  g_callframe_pool.freelist.push_back(f);
}

// PE VMThread_CallFrame_ctor @ 0x407B10 (thiscall):
// vtbl off_5E7414→off_5E7410; local vec capacity=a6; op vec empty;
// +0x18=&locals, +0x28=&operand; +0x2C=a4 +0x30=a5 +0x34=a3 +0x3C=a2.
VmCallFrame* callframe_ctor(VmCallFrame* frame, VmThread* thr, uint32_t clazz,
                            uint32_t code, uint32_t instance, int local_slots) {
  if (!frame) return nullptr;
  frame->pe = {};
  frame->locals.clear();
  frame->operand.clear();
  frame->dllist_prev = nullptr;
  frame->dllist_next = nullptr;
  if (local_slots > 0) {
    frame->locals.resize(static_cast<std::size_t>(local_slots));
    frame->pe.local_cap = static_cast<uint32_t>(local_slots);
  }
  frame->pe.local_hdr = 1;  // PE this+0x0C header live
  frame->pe.op_hdr = 1;     // PE this+0x1C header live (pushCallFrame)
  frame->pe.code = code;
  frame->pe.clazz = clazz;
  frame->pe.instance = instance;
  frame->pe.vmthread =
      static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(thr));
  return frame;
}

void frame_list_link(VmFrameList* list, VmCallFrame* frame) {
  if (!list || !frame) return;
  // PE: insert before cursor in circular dllist (frame_list+8).
  frame->dllist_next = list->cursor;
  frame->dllist_prev = list->cursor ? list->cursor->dllist_prev : nullptr;
  if (list->cursor) list->cursor->dllist_prev = frame;
  list->cursor = frame;
  list->frames.push_back(frame);
}

// PE Class_isInheritedFrom_desc("java.lang.Native") @ invokeMethod Native.ptr.
bool class_inherits_native(Jvm* j, const char* fqn) {
  if (!j || !fqn || !fqn[0]) return false;
  const JvmClass* c = j->find_class(fqn);
  if (!c) {
    j->load_class(fqn);
    c = j->find_class(fqn);
  }
  for (int guard = 0; c && guard < 64; ++guard) {
    if (c->name == "java.lang.Native") return true;
    if (c->super_name.empty() || c->super_name == "java.lang.Object") break;
    if (!j->find_class(c->super_name.c_str())) j->load_class(c->super_name.c_str());
    c = j->find_class(c->super_name.c_str());
  }
  return false;
}

// PE Object_getField(obj,"ptr")+8 → Native.ptr (aPtr @ 0x60C338).
void unbox_native_ptr_args(Jvm* j, std::vector<JvmValue>* args,
                           bool skip_self = false) {
  if (!j || !args) return;
  size_t ai = 0;
  for (JvmValue& a : *args) {
    // Fork: host natives take the receiver as InvObject*, never as the
    // PE Native.ptr int (FindFile.first(self, ...) received Int 0).
    if (ai++ == 0 && skip_self) continue;
    if (a.tag != JvmTag::Obj || !a.v.o) continue;
    const char* cn = tree_host_class(a.v.o);
    if (!class_inherits_native(j, cn)) continue;
    // PE: if obj+8 (payload) != 0 → box Int(ptr); else leave/zero.
    const int32_t ptr = tree_field_get_int(a.v.o, "ptr");
    a = JvmValue::make_int(ptr);
  }
}

}  // namespace

// PE green-thread dllist: VMThread_init links into *(JVM+0x18)+0x18.
// Host: circular-ish doubly-linked list headed by g_vm_sched_head.
static VmThread* g_vm_sched_head = nullptr;
static VmThread* g_vm_sched_cursor = nullptr;  // PE JVM+0x78 run cursor
static int g_jvm_pump_gen = 0;                // PE Jvm+0x40 bump
// PE Jvm+0x3C GC slice budget (Jvm_GcSlice clamp [4,64]). Host mirror
// only — never recalculated (no +0x44/+0x30 dllist lens).
static int g_jvm_gc_slice_budget = 4;
// PE Jvm+0x24 — EMA of mark-sweep wall time (µs-ish). PumpFrame @
// 0x418E62..0x418E90: ftol(delta_ms*1000*0.8 + old*0.2). System.info(3).
static int32_t g_jvm_gc_ema = 0;

// PE Jvm_GcSlice @ 0x418C20 size 0xE8 — host SKIP (W34-17: no soft path).
// PE: if (Jvm+0x40 & 0xFF)==0: count finalizeQ(+0x44) + grey(+0x30)/budget
//   → write +0x3C = max(4, min(64, (nFin + nGrey/budget)>>6));
//   then pop ≤+0x3C heads via Object_FinalizeFree @ 0x408560;
//   if budget remain & Q empty: splice +0x30 dllist into +0x2C.
// Skip: host has no PE object dllists at +0x44/+0x30/+0x2C;
// Object_FinalizeFree @ 0x408560 → Thread_callMethod("finalize") →
// VMThread_run opcode switch @ 0x4210D4 (do not invent interpreter).
static void jvm_gc_slice() {
  (void)g_jvm_gc_slice_budget;  // PE +0x3C; live only with dllists
}

// W35-17: mark-sweep body of Jvm_PumpFrame @ 0x418D37..0x418E5F / JVM_GC
// @ 0x417F80 — SKIP (no soft shell without bytecode).
// Gate PE: *(Jvm+0x34) > *(Jvm+0x38). Then:
//   grey@+0x20 set → Jvm_MarkSweepDrainGrey @ 0x418970 → walk live
//     dllist(+0x10) unmarked → Object_EnqueueFinalize @ 0x408500 →
//     *(+0x34) -= *(+0x38);
//   else alloc grey PtrVec → Jvm_MarkSweepSeedSceneRoots @ 0x418110 →
//     Jvm_MarkSweepSeedThreadLocals @ 0x418290 → Object_MarkGrey @
//     0x4198B0 over class slots (ret -1 busy).
// Host: no PE object dllists / grey stack / Class shells; GcSlice
// FinalizeFree still → opcode@4210D4. Soft path = EMA only (below).
static void jvm_mark_sweep_shell() {}

static void vm_sched_link(VmThread* thr) {
  if (!thr) return;
  thr->sched_prev = nullptr;
  thr->sched_next = g_vm_sched_head;
  if (g_vm_sched_head) g_vm_sched_head->sched_prev = thr;
  g_vm_sched_head = thr;
}

static void vm_sched_unlink(VmThread* thr) {
  if (!thr) return;
  if (g_vm_sched_cursor == thr) g_vm_sched_cursor = thr->sched_next;
  if (thr->sched_prev)
    thr->sched_prev->sched_next = thr->sched_next;
  else if (g_vm_sched_head == thr)
    g_vm_sched_head = thr->sched_next;
  if (thr->sched_next) thr->sched_next->sched_prev = thr->sched_prev;
  thr->sched_prev = thr->sched_next = nullptr;
}

static float vm_now_ms() {
#ifdef _WIN32
  return static_cast<float>(GetTickCount());
#else
  return 0.f;
#endif
}

// PE VMThread_init @ 0x41F340 — fills 56B blob; FrameList malloc(28);
// CallFrame from g_CallFramePool via CallFrame_ctor; links frame dllist;
// sets curr_frame; links green-thread list. CallNamedMethod_va @ 0x4256CC.
VmThread* vmthread_init(void* jvm, int32_t priority, int32_t sync_flags,
                        const char* name) {
  auto* thr = new VmThread();
  // PE &VMThread_vtbl @ 0x5F098C — host keeps 0 (no PE vtbl image).
  thr->pe.vtbl = 0;
  thr->pe.link_prev = 0;
  thr->pe.link_next = 0;
  // Truncate host pointer into PE dword slot (stock VM is 32-bit).
  thr->pe.jvm = static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(jvm));
  thr->pe.jvm_aux = 0;  // PE *(JVM+4) when JVM live
  thr->pe.java_backref = 0;
  thr->pe.unk_24 = 0;
  thr->pe.priority = priority;  // +0x28
  thr->pe.flags = sync_flags;   // +0x2C (CallNamedMethod sync 0|1)
  thr->pe.sleep_deadline = 0.f; // +0x30
  thr->pe.notify_34 = 0;        // +0x34
  if (name && name[0])
    thr->name_storage = name;
  else
    thr->name_storage.clear();
  // PE +0x1C = strdup name; host keeps string in name_storage (no PE heap).
  thr->pe.name = 0;

  // PE malloc(28) FrameList + VMThread_FrameList_vtbl @ 0x5F0988.
  thr->frame_list = new VmFrameList();
  thr->pe.frame_list = 1;  // host: non-zero = live (no 32-bit ptr store)

  // PE CallFramePool_alloc(g_CallFramePool) + CallFrame_ctor(thr,0,0,0,0).
  VmCallFrame* fr = callframe_pool_alloc();
  callframe_ctor(fr, thr, /*clazz=*/0, /*code=*/0, /*instance=*/0,
                 /*local_slots=*/0);
  frame_list_link(thr->frame_list, fr);
  thr->curr_frame = fr;
  thr->pe.curr_frame = 1;

  // PE @ 0x41F4AA..0x41F4C4: insert into JVM green-thread dllist.
  vm_sched_link(thr);
  return thr;
}

// PE Engine_CallNamedMethod_packArgs tag3 @ 0x425380: box float (dword_62DEEC
// class) from va qword; append to arg vec. Host: JvmValue float only.
void vmthread_pack_arg_float(VmThread* thr, float f) {
  if (!thr) return;
  thr->pack_vec.push_back(JvmValue::make_float(f));
}

// PE VMThread_pushCallFrame @ 0x41F9F0 (thiscall): for i in [0, a2.count)
// push a2.data[i] onto *(this+8)+0x28 operand vector; alloc boxed Int
// (dword_62E00C) with value=count; push box; return count.
int vmthread_push_call_frame(VmThread* thr) {
  if (!thr) return 0;
  if (!thr->curr_frame) {
    VmCallFrame* fr = callframe_pool_alloc();
    callframe_ctor(fr, thr, 0, 0, 0, 0);
    if (thr->frame_list) frame_list_link(thr->frame_list, fr);
    thr->curr_frame = fr;
    thr->pe.curr_frame = 1;
  }
  VmCallFrame* fr = thr->curr_frame;
  fr->operand.clear();
  thr->operand.clear();
  fr->operand.reserve(thr->pack_vec.size() + 1);
  thr->operand.reserve(thr->pack_vec.size() + 1);
  for (const JvmValue& v : thr->pack_vec) {
    fr->operand.push_back(v);
    thr->operand.push_back(v);
  }
  const int count = static_cast<int>(thr->pack_vec.size());
  const JvmValue box = JvmValue::make_int(count);  // PE boxed count
  fr->operand.push_back(box);
  thr->operand.push_back(box);
  fr->pe.op_count = static_cast<uint32_t>(fr->operand.size());
  fr->pe.op_cap = fr->pe.op_count;
  return count;
}

// PE VMThread_invokeMethod @ 0x41FBC0 — host slice (no bytecode interp).
// ACC_NATIVE (method flags & 0x40) @ 0x41FC2D:
//   pe_native_hash(class,method) → soft NativeHash_find/next (128×0xC);
//   strcmp NativeRec +0xC/+0x10; NativeSigDesc_sameIntern @ 0x41DED0;
//   build CallInfo → nativeImpl(CallInfo*) → JVM_UnboxArg @ 0x45D910.
// Host soft: resolve_native (bucket dig) + soft_unbox_bind + jvm->invoke;
//   Native.ptr unbox via Object_getField("ptr")+8 → Int before call.
// Java path: PE CallFramePool_alloc+ctor+dllist queue; ret 0. Host: stash
// pending TREE invoke for VMThread_run / Jvm_RunThreadsBudgeted; ret 0.
// Do NOT invent opcode switch @ 0x4210D4 here.
int vmthread_invoke_method(VmThread* thr, InvObject* self, const char* class_fqn,
                           const char* method, const char* signature) {
  if (!thr || !class_fqn || !method || !signature) return -1;
  Jvm* j = jvm_active();
  if (!j) return -1;

  // PE @ 0x420237: writePcToFrame — parent+0x2C = thr+0x24 before the new
  // CallFrame. Soft stream_call / try_stream nest through here; without the
  // save, popFrameRestorePc rewinds PC and re-executes the call opcode forever.
  if (thr->curr_frame) thr->curr_frame->pe.code = thr->pe.unk_24;

  // PE Java path @ 0x420247: CallFramePool_alloc + CallFrame_ctor(this, clazz,
  // code, instance, local_slots) + dllist link; curr_frame = new frame.
  VmCallFrame* fr = callframe_pool_alloc();
  const uint32_t inst_u =
      static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(self));
  callframe_ctor(fr, thr, /*clazz=*/0, /*code=*/0, /*instance=*/inst_u,
                 /*local_slots=*/0);
  if (thr->frame_list) frame_list_link(thr->frame_list, fr);
  thr->curr_frame = fr;
  thr->pe.curr_frame = 1;

  std::vector<JvmValue> args;
  args.reserve(thr->pack_vec.size() + 1);
  if (self) args.push_back(JvmValue::make_obj(self));
  for (const JvmValue& v : thr->pack_vec) args.push_back(v);
  thr->pack_vec.clear();  // Fork: consumed; never reuse stale args

  // Copy packed args onto new frame locals (PE pops operand → locals).
  fr->locals = args;
  fr->pe.local_count = static_cast<uint32_t>(fr->locals.size());
  fr->pe.local_cap = fr->pe.local_count;

  const NativeEntry* ne = resolve_native(class_fqn, method, signature);
  bool is_native = ne && ne->fn;
  if (!is_native) {
    if (!j->find_class(class_fqn)) j->load_class(class_fqn);
    if (const JvmClass* cls = j->find_class(class_fqn)) {
      if (const JvmMethod* m = j->find_method(*cls, method, signature))
        is_native = m->is_native;
    }
  }

  if (is_native) {
    // PE ACC_NATIVE @ 0x41FC2D..0x4201D8 — RegisterNative lookup + UnboxArg
    // descriptor; host: Native.ptr arg unbox then invoke (registered sig).
    unbox_native_ptr_args(j, &args, /*skip_self=*/self != nullptr);
    JvmValue result = j->invoke(class_fqn, method, signature, args, false);
    fr->operand.push_back(result);
    fr->pe.op_count = static_cast<uint32_t>(fr->operand.size());
    // Fork: hand the result to whoever pops this leaf frame.
    thr->native_ret = result;
    thr->has_native_ret = (result.tag != JvmTag::Void);
    thr->pending.live = false;
    return 1;
  }

  // PE bytecode queue returns 0; CallNamedMethod_va @ 0x42570F → VMThread_run.
  thr->pending.live = true;
  thr->pending.class_fqn = class_fqn;
  thr->pending.method = method;
  thr->pending.signature = signature;
  thr->pending.args = std::move(args);
  return 0;
}

namespace {

// Soft ≡ NativeSigDesc_ctorFromOperands @ 0x41DCB0 + getJni @ 0x41DEF0:
// PE pops argc box, concatenates Value type strings, Intern "("+types+")",
// then getJni adds ret (default "V"). Soft: pack_vec tags → "(… )V".
std::string soft_jni_from_pack_vec(const std::vector<JvmValue>& pack) {
  std::string s = "(";
  for (const JvmValue& v : pack) {
    switch (v.tag) {
      case JvmTag::Int:
        s += 'I';
        break;
      case JvmTag::Float:
        s += 'F';
        break;
      case JvmTag::Obj:
        s += "Ljava/lang/Object;";
        break;
      default:
        s += 'I';
        break;
    }
  }
  s += ")V";  // PE getJni default ret 'V' when ret slot empty
  return s;
}

// ---------------------------------------------------------------------------
// Fork: faithful method resolution, ported from the Build 940 exe
// (Class_lookupMethod @ 0x5494C0, Class_findMethodSlot @ 0x54A150,
// NativeSigDesc_compatScore @ 0x5607D0, type score @ 0x55BD80).
// ---------------------------------------------------------------------------
// NativeSigDesc_ctorFromOperands @ 0x560930: one type string per operand.
static std::string pe_value_type(const JvmValue& v) {
  switch (v.tag) {
    case JvmTag::Int: return "I";
    case JvmTag::Float: return "F";
    case JvmTag::Obj: {
      if (!v.v.o) return "L;";
      if (tree_vector_is(v.v.o)) return "[";
      const char* hc = tree_host_class(v.v.o);
      if (hc && hc[0]) return std::string("L") + hc + ";";
      if (string_cstr(v.v.o)) return "Ljava.lang.String;";
      return "L;";
    }
    default: return "L;";
  }
}

static std::vector<std::string> pe_split_args(const std::string& sig) {
  std::vector<std::string> out;
  size_t i = sig.find('(');
  if (i == std::string::npos) return out;
  ++i;
  while (i < sig.size() && sig[i] != ')') {
    const size_t start = i;
    while (i < sig.size() && sig[i] == '[') ++i;
    if (i < sig.size() && sig[i] == 'L') {
      const size_t e = sig.find(';', i);
      i = (e == std::string::npos) ? sig.size() : e + 1;
    } else {
      ++i;
    }
    out.push_back(sig.substr(start, i - start));
  }
  return out;
}

// Inheritance distance from `from` up to `to`; -1 when unrelated.
static int pe_class_distance(Jvm* j, const std::string& from, const std::string& to) {
  if (from == to) return 0;
  if (to == "java.lang.Object") return 1;
  if (!j->find_class(from.c_str())) j->load_class(from.c_str());
  const JvmClass* c = j->find_class(from.c_str());
  int d = 0;
  for (int guard = 0; c && guard < 64; ++guard) {
    if (c->name == to) return d;
    if (c->super_name.empty()) break;
    if (!j->find_class(c->super_name.c_str())) j->load_class(c->super_name.c_str());
    c = j->find_class(c->super_name.c_str());
    ++d;
  }
  return -1;
}

static std::string pe_strip_L(const std::string& t) {
  if (t.size() >= 2 && t[0] == 'L' && t.back() == ';') return t.substr(1, t.size() - 2);
  return t;
}

// Type score @ 0x55BD80: method param type `mt` vs call arg type `ct`.
static int pe_type_score(Jvm* j, const std::string& mt, const std::string& ct) {
  if (mt == ct) return 0;
  const char c = ct.empty() ? 0 : ct[0];
  const char m = mt.empty() ? 0 : mt[0];
  if (c == 'F') return (m == 'I') ? 101 : -1;
  if (c == 'I') {
    if (m == 'F') return 100;
    // Host representation: a resource id literal is an Int here, while the
    // PE types it as a ResourceRef value. Accept it for ResourceRef-derived
    // parameters (new Style(w, h, Frontend.mediumFont, ...)).
    if (m == 'L' && pe_class_distance(j, pe_strip_L(mt), "java.util.resource.ResourceRef") >= 0)
      return 100;
    return -1;
  }
  if (c == 'L' || c == '[') {
    if (m == 'F' || m == 'I') return (ct == "Ljava.lang.String;") ? 100 : -1;
    if (m == 'L' || m == '[') {
      if (mt == "L;" || ct == "L;") return 100;  // untyped object / null
      if (m == '[' || c == '[') return (m == c) ? 0 : -1;
      return pe_class_distance(j, pe_strip_L(ct), pe_strip_L(mt));
    }
  }
  return -1;
}

// Class_findMethodSlot @ 0x54A150: walk the super chain, scan each method
// table from the end, score same-name candidates; 0 = exact (stop), else
// the lowest score wins (a tie logs "more matching methods found").
static const JvmMethod* pe_find_method(Jvm* j, const char* class_fqn,
                                       const char* name,
                                       const std::string& callsig,
                                       std::string* owner_out) {
  const std::vector<std::string> cargs = pe_split_args(callsig);
  const JvmMethod* best = nullptr;
  int best_score = 1000000;
  std::string best_owner;
  bool tie = false;
  if (!j->find_class(class_fqn)) j->load_class(class_fqn);
  const JvmClass* cls = j->find_class(class_fqn);
  for (int guard = 0; cls && guard < 64; ++guard) {
    for (size_t k = cls->methods.size(); k-- > 0;) {
      const JvmMethod& cand = cls->methods[k];
      if (cand.name != name) continue;
      const std::vector<std::string> margs = pe_split_args(cand.signature);
      if (margs.size() != cargs.size()) continue;
      int score = 0;
      for (size_t a = 0; a < margs.size(); ++a) {
        const int sc = pe_type_score(j, margs[a], cargs[a]);
        if (sc < 0) { score = -1; break; }
        score += sc;
      }
      if (score < 0) continue;
      if (score == 0) {
        if (owner_out) *owner_out = cls->name;
        return &cand;
      }
      if (score < best_score) {
        best = &cand; best_score = score; best_owner = cls->name; tie = false;
      } else if (score == best_score && best) {
        tie = true;
      }
    }
    if (cls->super_name.empty() || cls->super_name == "java.lang.Object") break;
    if (!j->find_class(cls->super_name.c_str())) j->load_class(cls->super_name.c_str());
    cls = j->find_class(cls->super_name.c_str());
  }
  if (best && owner_out) *owner_out = best_owner;
  if (tie && vm_trace_on())
    std::fprintf(stderr, "[pe-stream]   Class::getMethod: more matching methods found %s.%s%s\n",
                 class_fqn, name, callsig.c_str());
  return best;
}

// Soft Class_lookupMethod_nameSig @ 0x404910 → Class_findMethodSlot @ 0x405920:
// exact name+JNI first; on miss, PE scores NativeSigDesc_compatScore over
// same-name methods (getJni often ends ")V" while TREE has ")I"). Soft: first
// same-name method on the class / supers.
const JvmMethod* soft_lookup_method_name(Jvm* j, const char* class_fqn,
                                         const char* method,
                                         const char* soft_jni,
                                         std::string* owner_out = nullptr) {
  if (!j || !class_fqn || !method) return nullptr;
  if (owner_out) owner_out->clear();
  if (!j->find_class(class_fqn)) j->load_class(class_fqn);
  const JvmClass* cls = j->find_class(class_fqn);
  for (int guard = 0; cls && guard < 64; ++guard) {
    if (soft_jni && soft_jni[0]) {
      if (const JvmMethod* m = j->find_method(*cls, method, soft_jni)) {
        if (owner_out) *owner_out = cls->name;
        return m;
      }
    }
    // PE compatScore path: name match when operand-built JNI ret is default V.
    for (const JvmMethod& cand : cls->methods) {
      if (cand.name == method) {
        if (owner_out) *owner_out = cls->name;
        return &cand;
      }
    }
    if (cls->super_name.empty() || cls->super_name == "java.lang.Object") break;
    if (!j->find_class(cls->super_name.c_str()))
      j->load_class(cls->super_name.c_str());
    cls = j->find_class(cls->super_name.c_str());
  }
  return nullptr;
}

}  // namespace

// PE Thread_callMethod @ 0x4207C0 — name-only resolve + invokeMethod.
// Object_callMethod @ 0x408A30 / Object_callMethod_init @ 0x408A70 /
// Object_callInitIf @ 0x408A90 thin wrappers.
int vmthread_thread_call_method(VmThread* thr, InvObject* self,
                                const char* class_fqn, const char* method) {
  if (!thr || !class_fqn || !method || !method[0]) return -1;
  // PE null clazz → "callMethod: null clazz %s"; ret -1.
  Jvm* j = jvm_active();
  if (!j) return -1;

  // Ensure pack_vec mirrors pushCallFrame operand args (drop trailing count
  // box if present — PE ctorFromOperands pops argc Value first).
  if (thr->pack_vec.empty() && thr->curr_frame &&
      !thr->curr_frame->operand.empty()) {
    const auto& op = thr->curr_frame->operand;
    if (op.back().tag == JvmTag::Int) {
      const int argc = op.back().v.i;
      if (argc >= 0 && static_cast<std::size_t>(argc) < op.size()) {
        thr->pack_vec.assign(op.end() - 1 - argc, op.end() - 1);
        // Fork: PE NativeSigDesc_ctorFromOperands @ 0x41DCB0 pops the argc
        // box and CallFrame_ctor moves the args into the callee locals, so
        // the caller operand stack loses argc+1 entries here. Without this
        // the box is left behind and the next assign stores it (rpkScan
        // `ff = new FindFile()` ended with ff == 0).
        auto& mop = thr->curr_frame->operand;
        mop.resize(mop.size() - 1 - static_cast<std::size_t>(argc));
        auto& mref = thr->curr_frame->operand_ref;
        if (mref.size() > mop.size()) mref.resize(mop.size());
        thr->curr_frame->pe.op_count = static_cast<uint32_t>(mop.size());
      }
    }
  }

  // PE Object_callMethod @ 0x408A30 uses obj+0xC clazz; Soft 4025 may fall
  // back to the caller class when host_class is empty. Vector bags always
  // resolve as java.util.Vector (no classpath .class — invoke leaves).
  const char* resolve_fqn = class_fqn;
  if (self && tree_vector_is(self)) resolve_fqn = "java.util.Vector";

  const std::string soft_jni = soft_jni_from_pack_vec(thr->pack_vec);
  // Fork: PE resolution. A 0x101A methodref supplies the descriptor from
  // the constant pool (Thread_evalName @ 0x562540); a utf8 call builds it
  // from the operand types (NativeSigDesc_ctorFromOperands @ 0x560930).
  std::string callsig;
  if (!thr->call_sig_hint.empty()) {
    callsig = thr->call_sig_hint;
  } else {
    callsig = "(";
    for (const JvmValue& v : thr->pack_vec) callsig += pe_value_type(v);
    callsig += ")V";
  }
  thr->call_sig_hint.clear();
  std::string owner_fqn;  // class that declares the resolved method
  const JvmMethod* m = pe_find_method(j, resolve_fqn, method, callsig, &owner_fqn);
  if (!m) {
    // PE: ScriptError "not found". Soft fallback keeps the name-only match
    // (traced) until every host signature is PE-shaped.
    m = soft_lookup_method_name(j, resolve_fqn, method, soft_jni.c_str(),
                                &owner_fqn);
    // Never run a script body with the wrong arity (GameRef.<init>() picked
    // <init>(GameRef) and read a missing local); the PE reports "not found"
    // and makes no call.
    if (m && !m->is_native &&
        pe_split_args(m->signature).size() != pe_split_args(callsig).size())
      m = nullptr;
    if (vm_trace_on())
      std::fprintf(stderr, "[pe-stream]   lookup fallback %s.%s%s -> %s\n",
                   resolve_fqn, method, callsig.c_str(),
                   m ? m->signature.c_str() : "(none)");
  }
  // PE miss → ScriptError "not found"; Soft: still attempt soft_jni invoke
  // (resolve_native / TREE may bind). Exact PE: return name on miss.
  if (!m) {
    if (vm_trace_on())
      std::fprintf(stderr, "[pe-stream]   script error: Thread::callMethod: %s.%s%s not found\n",
                   resolve_fqn, method, callsig.c_str());
    // PE Thread_callMethod: ScriptError, return -1 (no call). Only natives
    // may still bind by name through invoke_method below.
    if (!resolve_native(resolve_fqn, method, soft_jni.c_str())) return -1;
  }
  const char* sig = m ? m->signature.c_str() : soft_jni.c_str();
  // Fork: an inherited NATIVE (Zed_VT.newNative -> GameRef.newNative) is
  // registered under the declaring class; invoke it there. Script methods
  // keep the leaf class so virtual dispatch stays on the receiver.
  const char* call_fqn = (m && m->is_native && !owner_fqn.empty())
                             ? owner_fqn.c_str()
                             : resolve_fqn;
  return vmthread_invoke_method(thr, self, call_fqn, method, sig);
}

// PE op33 @ 0x421254: peek recv from operand; Object_callMethod(recv, thr,
// "<init>"). Soft: caller supplies name (PE case hardcodes "<init>").
int vmthread_op33_call_method(VmThread* thr, InvObject* self,
                              const char* class_fqn, const char* method) {
  // PE null instance on instance method → "instance missing" + continue.
  if (!self) return -1;
  return vmthread_thread_call_method(thr, self, class_fqn, method);
}

// PE op34 @ 0x4212C7 → Object_callMethod_init @ 0x408A70:
//   Thread_callMethod(thr, clazz, this, "<init>"); frame+0x30 / +0x34.
int vmthread_op34_call_method_init(VmThread* thr, InvObject* self,
                                   const char* class_fqn) {
  return vmthread_thread_call_method(thr, self, class_fqn, "<init>");
}

// PE op35 @ 0x4212A4 → Object_callInitIf @ 0x408A90:
//   a3!=0 → Thread_callMethod(thr, clazz, this, "<init>"); else ret 1
//   (continue / default advance). Soft: null/empty class_fqn ≡ a3==0.
int vmthread_op35_call_init_if(VmThread* thr, InvObject* self,
                               const char* class_fqn) {
  if (!class_fqn || !class_fqn[0]) return 1;
  return vmthread_thread_call_method(thr, self, class_fqn, "<init>");
}

// PE op36 @ 0x4212E4: PC+=8; Thread_evalName → Thread_callMethod (script
// marker method+0xC==-2) or VMThread_invokeMethod; ret0→yield else advance.
int vmthread_op36_invoke(VmThread* thr, InvObject* self, const char* class_fqn,
                         const char* method, const char* signature) {
  if (!thr) return -1;
  // Soft PC stride-8 (PE thr+0x24 write before evalName). No CP evalName.
  thr->pe.unk_24 += 8;
  return vmthread_invoke_method(thr, self, class_fqn, method, signature);
}

// PE op 0x21 @ 0x421254: peek recv from CallFrame+0x28 operand via
//   slots[depth-1] = argc box, recv = slots[depth - argc - 2];
// then Object_callMethod(recv, thr, "<init>") → class from obj+0xC.
InvObject* vmthread_op21_peek_recv(VmThread* thr) {
  if (!thr || !thr->curr_frame) return nullptr;
  const auto& op = thr->curr_frame->operand;
  if (op.empty()) return nullptr;
  int argc = 0;
  if (op.back().tag == JvmTag::Int) argc = op.back().v.i;
  if (argc < 0) argc = 0;
  const size_t depth = op.size();
  if (static_cast<size_t>(argc) + 2 > depth) return nullptr;
  const JvmValue& recv = op[depth - static_cast<size_t>(argc) - 2];
  return (recv.tag == JvmTag::Obj) ? recv.v.o : nullptr;
}

// ---------------------------------------------------------------------------
// PE VMThread_run @ 0x420FF0 size 0x2979 — opcode switch @ 0x4210D4.
// Soft: no PE insn stream (thr+0x24 / pe.unk_24). TREE invoke stands in for
// the bytecode body; hosted cases below cover return/popFrame/stack/literal/
// local-load/getfield/invoke slices behind vmthread_run / RunThreadsBudgeted.
//
// Low switch 0..0x48 via VMThread_opcodeCaseMap @ 0x4239B0 → jpt_4210E5
// @ 0x42396C (insn stride 8):
//   16 @ 0x423932 DONE; 42 @ 0x421697 ret-void; 43 @ 0x421662 ret-val;
//   24 @ 0x4216BB pop; 40 @ 0x42164E pop+push; 29 @ 0x4214BC field-get;
//   33 @ 0x421254 Object_callMethod DONE soft; 34 @ 0x4212C7 init DONE soft;
//   35 @ 0x4212A4 callInitIf DONE soft; 36 @ 0x4212E4 invoke DONE soft;
//   4 @ 0x42116C / 5 @ 0x4211E0 cond-skip DONE soft;
//   32 @ 0x421398 JT_ARRAYACCESS DONE soft; 41 @ 0x421583 pool DONE soft;
//   45 @ 0x4210EC assign-store DONE soft; 0/19/66/72 fatal.
// Hi @ 0x4216D5: 0x1001 local; 0x1007 literal; 0x1008.. ops; JT_FIELD_REF
//   0x1015 boolCondAdvance@423239 DONE soft; 0x1016 boolCondSkip@423267 DONE soft;
//   0x1017 caseEqSkip@423608 DONE soft;
//   0x101F pushNulls@423585 DONE soft; 0x4025 Object_callMethod@423725 DONE soft;
//   0x4026 Thread_callMethod@4236EE DONE soft; 0x401C Object_getField@4237CE
//   DONE soft; 0x101B/0x101E DONE soft.
// Helpers: ValueStack_pop@0x423D40, ValueStack_push@0x423BA0,
//   VMThread_popOperand@0x41F7D0, VMThread_popFrameRestorePc@0x41F7B0,
//   CallFrame_cleanup@0x407BA0, VMThread_canContinue@0x423B90,
//   Value_getInt@0x41BD90 → JVM_booleanConversion@0x417840,
//   VMThread_opDefault_advance@0x4238ED, Thread_evalName@0x4208E0,
//   Thread_callMethod@0x4207C0, Value_getArrayElement@0x423EA0,
//   Value_assignOp@0x41ACB0 / Value_assign@0x41AB60.
// ---------------------------------------------------------------------------

namespace {

// PE @ 0x423B90: !(flags & 0x82).
bool vmthread_can_continue(const VmThread* thr) {
  return thr && (thr->pe.flags & kVmThreadFlagSkipMask) == 0;
}

// PE ValueStack_push @ 0x423BA0: push onto curr_frame operand (+0x28 hdr).
// The PE pushes a Value*, and ValueField_ctor @ 0x423EC0 makes that pointer a
// *variable* (its vtbl getType returns 2) when the operand names a storage
// location. Value_assignOp @ 0x41ACB0 and op 0x1008 case 10 @ 0x423059 reject
// anything else with "stack access: value on top (instead of variable)".
// JvmValue is a 4-byte union shared with every native, so instead of widening
// it the frame carries operand_ref parallel to operand: None is a plain Value,
// Local/Field/Static are the ValueField cases.
void vmthread_value_stack_push_ref(VmThread* thr, const JvmValue& v,
                                   const VmValueRef& ref) {
  if (!thr) return;
  if (thr->curr_frame) {
    thr->curr_frame->operand.push_back(v);
    thr->curr_frame->operand_ref.resize(thr->curr_frame->operand.size() - 1);
    thr->curr_frame->operand_ref.push_back(ref);
    thr->curr_frame->pe.op_count =
        static_cast<uint32_t>(thr->curr_frame->operand.size());
  }
  thr->operand.push_back(v);
}

void vmthread_value_stack_push(VmThread* thr, const JvmValue& v) {
  vmthread_value_stack_push_ref(thr, v, VmValueRef{});
}

// PE VMThread_popOperand @ 0x41F7D0 / ValueStack_pop @ 0x423D40.
bool vmthread_pop_operand_ref(VmThread* thr, JvmValue* out,
                              VmValueRef* ref_out) {
  if (ref_out) *ref_out = VmValueRef{};
  if (!thr || !out) return false;
  if (thr->curr_frame && !thr->curr_frame->operand.empty()) {
    *out = thr->curr_frame->operand.back();
    auto& refs = thr->curr_frame->operand_ref;
    if (ref_out && refs.size() == thr->curr_frame->operand.size())
      *ref_out = refs.back();
    if (!refs.empty()) refs.pop_back();
    thr->curr_frame->operand.pop_back();
    thr->curr_frame->pe.op_count =
        static_cast<uint32_t>(thr->curr_frame->operand.size());
    if (!thr->operand.empty()) thr->operand.pop_back();
    return true;
  }
  if (!thr->operand.empty()) {
    *out = thr->operand.back();
    thr->operand.pop_back();
    return true;
  }
  return false;
}

bool vmthread_pop_operand(VmThread* thr, JvmValue* out) {
  return vmthread_pop_operand_ref(thr, out, nullptr);
}

// PE Value_assign @ 0x41AB60: JVM_assignmentConversion @ 0x4178F0 coerces the
// source to the DESTINATION descriptor before copying the payload. The host
// approximates that table with Int/Float coercion only; Object_MarkGrey
// @ 0x4198B0 on L/[ has no host equivalent.
JvmValue vmthread_assign_coerce(const JvmValue& dst, const JvmValue& src) {
  if (dst.tag == JvmTag::Int && src.tag == JvmTag::Float)
    return JvmValue::make_int(static_cast<int32_t>(src.v.f));
  if (dst.tag == JvmTag::Float && src.tag == JvmTag::Int)
    return JvmValue::make_float(static_cast<float>(src.v.i));
  return src;
}

// Soft PE Value_assignOp @ 0x41ACB0 switch(a4): compute the value written
// back into dst. '#' is plain assign; compounds reuse Value_* already Soft.
// ')'/'*'/'+' fall to PE "unsupported assignmenttype" — Soft leaves dst.
JvmValue vmthread_assign_op_result(const JvmValue& dst, const JvmValue& src,
                                   int32_t op) {
  auto as_i = [](const JvmValue& v) -> int32_t {
    if (v.tag == JvmTag::Int) return v.v.i;
    if (v.tag == JvmTag::Float) return static_cast<int32_t>(v.v.f);
    return 0;
  };
  auto as_f = [](const JvmValue& v) -> float {
    if (v.tag == JvmTag::Float) return v.v.f;
    if (v.tag == JvmTag::Int) return static_cast<float>(v.v.i);
    return 0.f;
  };
  switch (op) {
    case '#':  // Value_assign
      return src;
    case '$': {  // *= Value_mul @ 0x41B860
      if (dst.tag == JvmTag::Float || src.tag == JvmTag::Float)
        return JvmValue::make_float(as_f(dst) * as_f(src));
      return JvmValue::make_int(as_i(dst) * as_i(src));
    }
    case '%': {  // /= Value_div @ 0x41B970
      if (dst.tag == JvmTag::Float || src.tag == JvmTag::Float) {
        const float r = as_f(src);
        return JvmValue::make_float(r == 0.f ? 0.f : (as_f(dst) / r));
      }
      const int32_t r = as_i(src);
      return JvmValue::make_int(r == 0 ? 0 : (as_i(dst) / r));
    }
    case '&': {  // %= Value_mod @ 0x41BBF0
      const int32_t r = as_i(src);
      return JvmValue::make_int(r == 0 ? 0 : (as_i(dst) % r));
    }
    case '\'': {  // += Value_add @ 0x41B4D0
      if (dst.tag == JvmTag::Float || src.tag == JvmTag::Float)
        return JvmValue::make_float(as_f(dst) + as_f(src));
      return JvmValue::make_int(as_i(dst) + as_i(src));
    }
    case '(':  // -= Value_neg(src)+Value_add
      if (dst.tag == JvmTag::Float || src.tag == JvmTag::Float)
        return JvmValue::make_float(as_f(dst) - as_f(src));
      return JvmValue::make_int(as_i(dst) - as_i(src));
    case ',':  // &= integral
      return JvmValue::make_int(as_i(dst) & as_i(src));
    case '-':  // ^= integral
      return JvmValue::make_int(as_i(dst) ^ as_i(src));
    case '.':  // |= integral
      return JvmValue::make_int(as_i(dst) | as_i(src));
    default:
      return dst;  // PE unsupported assignmenttype
  }
}

// PE Value_assignOp @ 0x41ACB0 with op '#': mutate the destination in place.
// Only a ValueField has somewhere to write, which is why a None ref fails.
bool vmthread_ref_assign(VmThread* thr, const VmValueRef& dst,
                         const JvmValue& src) {
  switch (dst.kind) {
    case VmRefKind::Local: {
      if (!thr || !thr->curr_frame) return false;
      auto& locals = thr->curr_frame->locals;
      if (dst.local_index >= locals.size()) return false;
      locals[dst.local_index] =
          vmthread_assign_coerce(locals[dst.local_index], src);
      return true;
    }
    case VmRefKind::Field: {
      if (!dst.obj || dst.name.empty()) return false;
      // PE Object_getFieldAt hands back the ValueField; the host TREE bag is
      // keyed by name, so write through the typed setters.
      if (src.tag == JvmTag::Obj)
        tree_field_set_obj(dst.obj, dst.name.c_str(), src.v.o);
      else if (src.tag == JvmTag::Float)
        tree_field_set_float(dst.obj, dst.name.c_str(), src.v.f);
      else
        tree_field_set_int(dst.obj, dst.name.c_str(), src.v.i);
      return true;
    }
    case VmRefKind::Static:
      // PE Class_getFieldAt @ 0x404800 hands back the class-owned ValueField,
      // so a static is assignable exactly like an instance field.
      if (!dst.owner || dst.name.empty()) return false;
      if (JvmValue* slot = tree_static_slot(dst.owner, dst.name, true)) {
        *slot = vmthread_assign_coerce(*slot, src);
        return true;
      }
      return false;
    case VmRefKind::None:
    default:
      return false;  // PE "stack access: value on top (instead of variable)"
  }
}

// PE CallFrame_cleanup @ 0x407BA0 + popFrameRestorePc @ 0x41F7B0:
// unlink/release curr_frame; thr+0x20 → parent; thr+0x24 ← parent+0x2C (code).
void vmthread_pop_frame_restore_pc(VmThread* thr) {
  if (!thr) return;
  VmCallFrame* dying = thr->curr_frame;
  if (!dying) {
    thr->pe.unk_24 = 0;  // PE PC @ +0x24
    return;
  }
  VmCallFrame* parent = nullptr;
  if (thr->frame_list) {
    auto& frames = thr->frame_list->frames;
    for (std::size_t i = 0; i < frames.size(); ++i) {
      if (frames[i] != dying) continue;
      if (i > 0) parent = frames[i - 1];
      frames.erase(frames.begin() + static_cast<std::ptrdiff_t>(i));
      break;
    }
    // Fork: the frames vector is authoritative. The old fallback to
    // dying->dllist_prev followed a stale link to an already-released
    // frame (double release → pool handed a live caller frame to a
    // nested thread). Unlink the neighbours properly instead.
    if (dying->dllist_prev) dying->dllist_prev->dllist_next = dying->dllist_next;
    if (dying->dllist_next) dying->dllist_next->dllist_prev = dying->dllist_prev;
    thr->frame_list->cursor =
        parent ? parent : (frames.empty() ? nullptr : frames.back());
  } else if (dying->dllist_prev) {
    parent = dying->dllist_prev;
  }
  dying->dllist_prev = nullptr;
  dying->dllist_next = nullptr;
  // Soft leaf frames have pe.code==0 (invokeMethod ctor); parent keeps saved
  // resume in pe.code (PE writePcToFrame @ 0x41F7C0 / invokeMethod @ 0x420237).
  const uint32_t resume = parent ? parent->pe.code : dying->pe.code;
  callframe_pool_release(dying);
  thr->curr_frame = parent;
  thr->pe.curr_frame = parent ? 1u : 0u;
  thr->pe.unk_24 = resume;  // PE popFrameRestorePc: thr+0x24 = frame+0x2C
}

bool jni_sig_returns_void(const std::string& sig) {
  const auto rp = sig.rfind(')');
  if (rp == std::string::npos) return sig == "V" || sig.empty();
  return rp + 1 < sig.size() && sig[rp + 1] == 'V';
}

// Soft stand-in for the PE invoke sites that share one control shape:
//   ret != 0 → opDefault_advance (+8 past the call-site node)
//   ret == 0 → afterPc (thr+0x24 replaced by the callee); when the callee
//              returns via op42/op43, popFrameRestorePc lands back on the
//              call-site node and THEN opDefault_advance supplies the +8.
// Soft has no thr+0x24 code-pointer switch, so a queued Java body is drained
// inline via j->invoke and the +8 is applied here. Used by op 0x21/0x22/0x23
// (PC on the opcode) and by op 0x24 after Thread_evalName (PC on terminator).
enum class StreamCall : int8_t { Fail = -1, Advance = 0, SelfAdvanced = 1 };

StreamCall vmthread_stream_call(VmThread* thr, InvObject* self,
                                const char* class_fqn, const char* method) {
  if (!thr || !class_fqn || !method || !method[0]) return StreamCall::Fail;
  VmCallFrame* before = thr->curr_frame;
  VmPendingInvoke saved = thr->pending;
  const int ret = vmthread_thread_call_method(thr, self, class_fqn, method);
  if (vm_trace_on())
    std::fprintf(stderr, "[pe-stream]   call %s.%s ret=%d before=%p cur=%p frames=%zu pc=%u\n",
                 class_fqn, method, ret, static_cast<void*>(before),
                 static_cast<void*>(thr->curr_frame),
                 thr->frame_list ? thr->frame_list->frames.size() : size_t{0}, thr->pe.unk_24);
  if (ret == 0 && thr->pending.live) {
    Jvm* j = jvm_active();
    if (!j) {
      if (thr->curr_frame && thr->curr_frame != before)
        vmthread_pop_frame_restore_pc(thr);
      thr->pending = saved;
      return StreamCall::Fail;
    }
    const bool ret_void = jni_sig_returns_void(thr->pending.signature);
    JvmValue result =
        j->invoke(thr->pending.class_fqn.c_str(), thr->pending.method.c_str(),
                  thr->pending.signature.c_str(), thr->pending.args,
                  /*is_static=*/self == nullptr);
    if (thr->curr_frame && thr->curr_frame != before)
      vmthread_pop_frame_restore_pc(thr);
    if (vm_trace_on())
      std::fprintf(stderr, "[pe-stream]   after-pop(java) cur=%p frames=%zu pc=%u\n",
                   static_cast<void*>(thr->curr_frame),
                   thr->frame_list ? thr->frame_list->frames.size() : size_t{0}, thr->pe.unk_24);
    thr->pending = saved;
    if (!ret_void) vmthread_value_stack_push(thr, result);
    thr->pe.unk_24 += 8;  // PE op42/43 post-return default_advance
    return StreamCall::SelfAdvanced;
  }
  if (thr->curr_frame && thr->curr_frame != before)
    vmthread_pop_frame_restore_pc(thr);
  thr->pending = saved;
  if (ret < 0) return StreamCall::Fail;
  if (vm_trace_on())
    std::fprintf(stderr, "[pe-stream]   after-pop cur=%p frames=%zu pc=%u\n",
                 static_cast<void*>(thr->curr_frame),
                 thr->frame_list ? thr->frame_list->frames.size() : size_t{0}, thr->pe.unk_24);
  if (ret == 1 && thr->has_native_ret) {
    // Fork: PE native path pushes the result on the resumed frame.
    vmthread_value_stack_push(thr, thr->native_ret);
  }
  thr->has_native_ret = false;
  return StreamCall::Advance;  // native/empty: leave PC for default +8
}

}  // namespace

// PE op16 DONE @ 0x423932: flags |= 0x40 (RunThreadsBudgeted → STOP).
void vmthread_op16_done(VmThread* thr) {
  if (!thr) return;
  thr->pe.flags |= kVmThreadFlagDone;
}

// Soft slice of Value_getInt @ 0x41BD90 → JVM_booleanConversion @ 0x417840:
// F→0/1 by ==0.f; L/[ → nonzero ptr→1; else payload+8 as int. Soft reads
// JvmTag without mutating TOS (PE in-place type rewrite OOS without Value*).
static bool vmthread_value_getint_truthy(const JvmValue& v) {
  switch (v.tag) {
    case JvmTag::Int:
      return v.v.i != 0;
    case JvmTag::Float:
      return v.v.f != 0.f;
    case JvmTag::Obj:
      return v.v.o != nullptr;
    default:
      return false;
  }
}

// PE op4 @ 0x42116C (VMThread_op4_condSkip): ValueStack_pop; optional
// ValueField(getType==2) clone OOS soft; push; Value_getInt; if truthy
// thr+0x24 += 8 @ 0x4211D7; fallthrough default advance (caller / loop).
void vmthread_op4_cond_skip(VmThread* thr) {
  if (!thr) return;
  JvmValue v;
  if (!vmthread_pop_operand(thr, &v)) v = JvmValue{};
  vmthread_value_stack_push(thr, v);
  if (vmthread_value_getint_truthy(v)) thr->pe.unk_24 += 8;  // PE @ 0x4211D7
}

// PE op5 @ 0x4211E0 (VMThread_op5_condSkip): same pop→push as op4; if
// Value_getInt falsy thr+0x24 += 8 @ 0x42124B (polarity of op4).
void vmthread_op5_cond_skip(VmThread* thr) {
  if (!thr) return;
  JvmValue v;
  if (!vmthread_pop_operand(thr, &v)) v = JvmValue{};
  vmthread_value_stack_push(thr, v);
  if (!vmthread_value_getint_truthy(v)) thr->pe.unk_24 += 8;  // PE @ 0x42124B
}

// PE op24 @ 0x4216BB: popOperand + vtbl dtor(1) — soft: discard TOS.
void vmthread_op24_pop_dtor(VmThread* thr) {
  JvmValue discarded;
  (void)vmthread_pop_operand(thr, &discarded);
}

// PE op40 @ 0x42164E: popOperand + ValueStack_push (copy/refresh TOS).
void vmthread_op40_pop_push(VmThread* thr) {
  JvmValue v;
  if (!vmthread_pop_operand(thr, &v)) return;
  vmthread_value_stack_push(thr, v);
}

// PE op32 @ 0x421398 (VMThread_op32_arrayAccess / case-map[32]=6):
// ValueStack_pop TOS=array (type '['), then index (type 'I', >=0); optional
// ValueField(getType==2) clone via ValuePool+Value_ctorCopy OOS soft;
// Value_getArrayElement @ 0x423EA0 → Instance_getArrayElement @ 0x408830
// (SoftArray length/elements fields); push element; release temps;
// VMThread_opDefault_advance PC+8. Soft: TREE vector element_at; no PE
// type-cstr / ScriptError / SoftArray field ids — null/OOB → push null.
void vmthread_op32_array_access(VmThread* thr) {
  if (!thr) return;
  JvmValue arr;
  JvmValue idx;
  // PE @ 0x42139E first pop = array (TOS); @ 0x4213D3 second = index.
  if (!vmthread_pop_operand(thr, &arr)) arr = JvmValue{};
  if (!vmthread_pop_operand(thr, &idx)) idx = JvmValue{};
  InvObject* elem = nullptr;
  const int32_t i =
      (idx.tag == JvmTag::Int)
          ? idx.v.i
          : (idx.tag == JvmTag::Float ? static_cast<int32_t>(idx.v.f) : 0);
  if (arr.tag == JvmTag::Obj && arr.v.o && i >= 0)
    elem = tree_vector_element_at(arr.v.o, i);  // PE @ 0x421437
  vmthread_value_stack_push(thr, JvmValue::make_obj(elem));
  thr->pe.unk_24 += 8;  // PE @ 0x4214B7 → VMThread_opDefault_advance
}

// PE op45 @ 0x4210EC (VMThread_op45_assignStore / case-map[45]=0xF):
// pop RHS (ValueField type2 → ValuePool+ctorCopy); pop LHS must getType==2
// else ScriptError "stack access: value on top..."; Value_assignOp(thr+0x14,
// LHS, RHS, '#') @ 0x41ACB0 → Value_assign @ 0x41AB60 (copy F/I/L/[
// payload; L/[ may Object_MarkGrey); release RHS; opDefault_advance.
// Soft: no Value* / ValueField — pop both (consume stack like PE); assign
// mutation + MarkGrey OOS; soft PC+=8.
void vmthread_op45_assign_store(VmThread* thr) {
  if (!thr) return;
  JvmValue rhs;
  JvmValue lhs;
  VmValueRef lhs_ref;
  if (!vmthread_pop_operand(thr, &rhs)) rhs = JvmValue{};  // PE @ 0x4210F2
  if (!vmthread_pop_operand_ref(thr, &lhs, &lhs_ref))      // PE @ 0x421127
    lhs = JvmValue{};
  // PE checks getType()==2 here and raises ScriptError otherwise; the host
  // simply fails the assign, which leaves the stream walkable either way.
  (void)vmthread_ref_assign(thr, lhs_ref, rhs);  // PE Value_assignOp('#')
  thr->pe.unk_24 += 8;  // PE → VMThread_opDefault_advance
}

// PE op41 @ 0x421583 (VMThread_op41_poolAlloc / case-map[41]=0x0C):
// INLINE ValuePool_alloc on JVM_ValuePool @ 0x62F130 (freelist +0x10 else
// slab grow +512 / malloc 0x1800) — same body as ValuePool_alloc @ 0x423C00
// but inlined (no call). Init Value{Value_vtbl, *[JVM_typeDesc_I_box]="I",
// payload+8=0}; ++JVM_liveObjectCount @ 0x62F138; PtrVec_push(*(frame+0x28)).
// Null alloc → PtrVec_push(nullptr) @ 0x42163B. Then opDefault_advance PC+8.
// Soft: no ValuePool PE slabs / liveObjectCount — push make_int(0); soft PC+=8.
void vmthread_op41_pool_alloc(VmThread* thr) {
  if (!thr) return;
  vmthread_value_stack_push(thr, JvmValue::make_int(0));  // PE @ 0x421583
  thr->pe.unk_24 += 8;  // PE @ 0x421636 → VMThread_opDefault_advance
}

// PE op42 ret-void @ 0x421697: CallFrame dtor; popFrameRestorePc; empty→DONE.
void vmthread_op42_ret_void(VmThread* thr) {
  if (!thr) return;
  vmthread_pop_frame_restore_pc(thr);
  if (thr->pe.unk_24 == 0) vmthread_op16_done(thr);
}

// PE op43 ret-val @ 0x421662: pop; dtor; popFrame; push; empty→DONE.
void vmthread_op43_ret_val(VmThread* thr) {
  if (!thr) return;
  JvmValue ret;
  if (!vmthread_pop_operand(thr, &ret)) ret = JvmValue{};
  vmthread_pop_frame_restore_pc(thr);
  vmthread_value_stack_push(thr, ret);
  if (thr->pe.unk_24 == 0) vmthread_op16_done(thr);
}

// PE op 0x1001 local-load @ 0x421714: locals[idx] → operand. The PE pushes the
// ValueField* out of the frame+0x18 PtrVec, not a copy, so the slot stays
// assignable — the host mirrors that with a Local reference.
void vmthread_op1001_local_load(VmThread* thr, uint32_t idx) {
  if (!thr || !thr->curr_frame) return;
  const auto& locals = thr->curr_frame->locals;
  if (idx >= locals.size()) {
    vmthread_value_stack_push(thr, JvmValue{});
    return;
  }
  VmValueRef ref;
  ref.kind = VmRefKind::Local;
  ref.local_index = idx;
  vmthread_value_stack_push_ref(thr, locals[idx], ref);
}

// PE op 0x1002 localDeclare @ 0x421752: ObjectPool_alloc + Value_ctorWithType
// (name = ConstantPool_getUtf8 of the following node — stock always 0x1006;
// optional preceding 0x402E type → Value+0xC) + ValueField_vtbl; append the
// ValueField* to CallFrame+0x18 locals PtrVec. No operand push. Then PC+8, or
// when the next word is 0x2C (',') set thr+0x24 there and fall into
// loc_422D74 (push that ValueField* as assignable lvalue, then +8). Soft: no
// ObjectPool/ValueField — push_back an empty slot (imm names the slot the
// comma path would reload; well-formed trees append at that index). Name /
// type nodes stay in-stream for opDefault_advance like the PE PC.
void vmthread_op1002_local_declare(VmThread* thr, uint32_t idx) {
  if (!thr || !thr->curr_frame) return;
  auto& locals = thr->curr_frame->locals;
  // PE @ 0x4217C2 always appends; idx is the comma-path lookup key.
  locals.push_back(JvmValue{});
  (void)idx;
  thr->curr_frame->pe.local_count =
      static_cast<uint32_t>(locals.size());
}

// PE op 0x1007 literal @ 0x4218A5 — soft slice of type-tag switch47 (int/float/obj).
void vmthread_op1007_literal(VmThread* thr, const JvmValue& lit) {
  vmthread_value_stack_push(thr, lit);
}

namespace {
// Soft PE Value_eq @ 0x41C5C0: numericPromotion then I/L/[ dword== or F float==
// → write boolean into a2 payload as type I. Soft: no Value* mutate — return bool.
bool vmthread_soft_value_eq(const JvmValue& a, const JvmValue& b) {
  if (a.tag == JvmTag::Float || b.tag == JvmTag::Float) {
    const float fa =
        (a.tag == JvmTag::Float)
            ? a.v.f
            : (a.tag == JvmTag::Int ? static_cast<float>(a.v.i) : 0.f);
    const float fb =
        (b.tag == JvmTag::Float)
            ? b.v.f
            : (b.tag == JvmTag::Int ? static_cast<float>(b.v.i) : 0.f);
    return fa == fb;
  }
  if (a.tag == JvmTag::Obj || b.tag == JvmTag::Obj)
    return a.tag == b.tag && a.v.o == b.v.o;
  if (a.tag == JvmTag::Int || b.tag == JvmTag::Int) {
    const int32_t ia =
        (a.tag == JvmTag::Int)
            ? a.v.i
            : (a.tag == JvmTag::Float ? static_cast<int32_t>(a.v.f) : 0);
    const int32_t ib =
        (b.tag == JvmTag::Int)
            ? b.v.i
            : (b.tag == JvmTag::Float ? static_cast<int32_t>(b.v.f) : 0);
    return ia == ib;
  }
  return a.tag == b.tag;
}

// Soft of Value_lt/gt/le/ge @ 0x41C9E0 / 0x41C790 / 0x41CB60 / 0x41C860:
// numericPromotion then F/I ordered compare → I bool. Obj: pointer order.
// op: '<' '>' '<=' '>='
bool vmthread_soft_value_cmp(const JvmValue& a, const JvmValue& b, char op) {
  if (a.tag == JvmTag::Obj || b.tag == JvmTag::Obj) {
    const uintptr_t pa =
        (a.tag == JvmTag::Obj) ? reinterpret_cast<uintptr_t>(a.v.o) : 0u;
    const uintptr_t pb =
        (b.tag == JvmTag::Obj) ? reinterpret_cast<uintptr_t>(b.v.o) : 0u;
    switch (op) {
      case '<':
        return pa < pb;
      case '>':
        return pa > pb;
      case 'L':  // <=
        return pa <= pb;
      case 'G':  // >=
        return pa >= pb;
      default:
        return false;
    }
  }
  if (a.tag == JvmTag::Float || b.tag == JvmTag::Float) {
    const float fa =
        (a.tag == JvmTag::Float)
            ? a.v.f
            : (a.tag == JvmTag::Int ? static_cast<float>(a.v.i) : 0.f);
    const float fb =
        (b.tag == JvmTag::Float)
            ? b.v.f
            : (b.tag == JvmTag::Int ? static_cast<float>(b.v.i) : 0.f);
    switch (op) {
      case '<':
        return fa < fb;
      case '>':
        return fa > fb;
      case 'L':
        return fa <= fb;
      case 'G':
        return fa >= fb;
      default:
        return false;
    }
  }
  const int32_t ia = (a.tag == JvmTag::Int) ? a.v.i : 0;
  const int32_t ib = (b.tag == JvmTag::Int) ? b.v.i : 0;
  switch (op) {
    case '<':
      return ia < ib;
    case '>':
      return ia > ib;
    case 'L':
      return ia <= ib;
    case 'G':
      return ia >= ib;
    default:
      return false;
  }
}

// Soft PE VMThread_pcSkipByOperand @ 0x423645: PC += imm*8 (insn stride).
void vmthread_pc_skip_by_operand(VmThread* thr, int32_t imm) {
  if (!thr) return;
  // Fork: the PE adds a SIGNED imm*8 (shl eax,3; add). Loops jump
  // backwards (rpkScan node 50: goto -26); clamping to 0 pinned the PC.
  if (imm > 0x10000) imm = 0x10000;
  if (imm < -0x10000) imm = -0x10000;
  const int64_t next = static_cast<int64_t>(thr->pe.unk_24) +
                       static_cast<int64_t>(imm) * 8;
  thr->pe.unk_24 = next < 0 ? 0u : static_cast<uint32_t>(next);
}
}  // namespace

// PE op 0x1015 boolCondAdvance @ 0x423239: pop → JVM_booleanConversion;
// release; falsy → VMThread_pcSkipByOperand(imm*8) → afterPc; truthy →
// opDefault_advance+8. Exact invert of 0x1016 @ 0x423267 (jpt_422E91 slot
// (eax-0x1008)==0xD). Soft: pop; falsy → skip imm*8; else PC+=8.
void vmthread_op1015_bool_cond_advance(VmThread* thr, int32_t imm) {
  if (!thr) return;
  JvmValue v;
  if (!vmthread_pop_operand(thr, &v)) v = JvmValue{};
  if (vmthread_value_getint_truthy(v))
    thr->pe.unk_24 += 8;  // PE @ 0x42325C → opDefault_advance
  else
    vmthread_pc_skip_by_operand(thr, imm);  // PE @ 0x423262 → afterPc
}

// PE op 0x1016 boolCondSkip @ 0x423267: pop → JVM_booleanConversion; release;
// truthy → VMThread_pcSkipByOperand(imm*8) → afterPc; else default+8.
// Soft: pop (discard); truthy → skip imm*8; else PC+=8. No Value* release.
void vmthread_op1016_bool_cond_skip(VmThread* thr, int32_t imm) {
  if (!thr) return;
  JvmValue v;
  if (!vmthread_pop_operand(thr, &v)) v = JvmValue{};
  if (vmthread_value_getint_truthy(v))
    vmthread_pc_skip_by_operand(thr, imm);  // PE → afterPc (no extra +8)
  else
    thr->pe.unk_24 += 8;  // PE → opDefault_advance
}

// PE op 0x1017 caseEqSkip @ 0x423608: pop A,B; Value_eq mutates A→I bool;
// equal → release both + pcSkipByOperand; else release A, push B, +8.
// Soft: pop A then B; soft eq; equal → skip; else push B + PC+=8.
void vmthread_op1017_case_eq_skip(VmThread* thr, int32_t imm) {
  if (!thr) return;
  JvmValue a;
  JvmValue b;
  if (!vmthread_pop_operand(thr, &a)) a = JvmValue{};  // PE @ 0x42360A
  if (!vmthread_pop_operand(thr, &b)) b = JvmValue{};  // PE @ 0x423613
  if (vmthread_soft_value_eq(a, b))
    vmthread_pc_skip_by_operand(thr, imm);  // PE @ 0x423645 → afterPc
  else {
    vmthread_value_stack_push(thr, b);  // PE @ 0x42366D keep switch subject
    thr->pe.unk_24 += 8;
  }
}

// PE op 0x101F pushNulls @ 0x423585: imm=N; loop N× ValuePool_alloc empty
// Value (type I / payload 0); PtrVec_push each; then opDefault_advance.
// Soft: push N× make_int(0); soft PC+=8. ValuePool slabs OOS. Cap N soft.
void vmthread_op101F_push_nulls(VmThread* thr, int32_t count) {
  if (!thr) return;
  if (count < 0) count = 0;
  if (count > 256) count = 256;
  for (int32_t i = 0; i < count; ++i)
    vmthread_value_stack_push(thr, JvmValue::make_int(0));  // PE empty I
  thr->pe.unk_24 += 8;
}

namespace {
// PE walks the super chain through the class CP (class+0x40 → ConstantPool_
// getClass → getUtf8 → JVM_findOrLoadClassByName). The host keeps super_name,
// so resolve through the active Jvm and load on demand.
const JvmClass* class_super(const JvmClass* c) {
  if (!c || c->super_name.empty()) return nullptr;
  Jvm* j = jvm_active();
  if (!j) return nullptr;
  if (!j->find_class(c->super_name.c_str())) j->load_class(c->super_name.c_str());
  return j->find_class(c->super_name.c_str());
}
}  // namespace

// PE Class_findFieldSlot @ 0x00405690. For each class up the super chain it
// scans two PtrVecs: i=1 the STATIC one at class+0x54, then i=0 the INSTANCE
// one at class+0x4C, each BACKWARD from count-1 @ 0x4056AF, comparing
// ConstantPool_getUtf8(class+0x24, field+8) against the wanted name.
// On a hit it records the owner in dword_62F11C @ 0x40575E, then:
//   static   → slot | 0x40000000                            @ 0x4057D3
//   instance → slot + Σ ancestor instance-field counts       @ 0x4057CA
//              (the sum walks j+0x4C counts from the owner's super upward)
//   instance with no super → slot as-is                      @ 0x4057DC
// Both vectors missing the name → retry on the super; chain exhausted → -1.
int32_t vmthread_class_find_field_slot(const JvmClass* cls,
                                       const std::string& name,
                                       const JvmClass** owner_out) {
  for (const JvmClass* c = cls; c; c = class_super(c)) {
    for (int pass = 1; pass >= 0; --pass) {
      const std::vector<JvmFieldDecl>& vec =
          (pass == 1) ? c->static_fields : c->instance_fields;
      if (vec.empty()) continue;
      for (size_t k = vec.size(); k-- > 0;) {  // PE scans count-1 → 0
        if (vec[k].name != name) continue;
        if (owner_out) *owner_out = c;  // PE dword_62F11C @ 0x40575E
        const int32_t slot = static_cast<int32_t>(k);
        if (pass == 1) return slot | 0x40000000;
        int32_t base = 0;  // PE @ 0x405799 sum of ancestor instance counts
        for (const JvmClass* a = class_super(c); a; a = class_super(a))
          base += static_cast<int32_t>(a->instance_fields.size());
        return base + slot;
      }
    }
  }
  return -1;
}

// PE ConstantPool_resolveFieldRef @ 0x00406D00 feeding op 0x101B: the FIELDREF
// entry caches its slot at +0x10, and the 0x40000000 bit of that cached slot is
// what 0x4234D9 tests to pick Class_getFieldAt over Object_getFieldAt. The host
// const pool only keeps the resolved name, so re-run the slot search to learn
// the same bit instead of trusting the declaration site.
bool vmthread_resolve_field_ref(const JvmClass* cp_cls, const JvmClass* cls,
                                uint32_t cp_index,
                                std::string* name_out, bool* is_static_out,
                                const JvmClass** owner_out) {
  // Fork: the CP index belongs to the class whose CODE is running
  // (cp_cls); the slot search starts at the context class (cls), which
  // differs after a ClassName segment (GameLogic.RID_* read from Zed_VT).
  if (!cp_cls || !cls || cp_index >= cp_cls->const_mref_name.size()) return false;
  const std::string& n = cp_cls->const_mref_name[cp_index];
  if (n.empty()) return false;
  const JvmClass* owner = nullptr;
  const int32_t slot = vmthread_class_find_field_slot(cls, n, &owner);
  if (name_out) *name_out = n;
  if (owner_out) *owner_out = owner;
  if (is_static_out) *is_static_out = slot != -1 && (slot & 0x40000000) != 0;
  return true;
}

// PE op 0x101B JT_FIELD_REF @ 0x4234B4: CP getFieldRef_Index; -1 →
// resolveFieldRef; bit0x40000000 → static Class_getFieldAt else
// Object_getFieldAt(this=frame+0x30); no this → ScriptError; push field
// Value; opDefault_advance. Soft: instance → locals[0] Obj as this; static →
// the class-owned bag standing in for the Instance at class+0x1D0;
// miss/null this → push null (no ScriptError); soft PC+=8.
void vmthread_op101B_field_ref(VmThread* thr, const char* name,
                               bool is_static, const JvmClass* owner) {
  if (!thr) return;
  if (is_static) {
    // PE Class_getFieldAt @ 0x00404800 = ensureInitialized + getFieldAt on the
    // ordinary Instance parked at class+0x1D0; the host twin is the static bag.
    JvmValue* slot =
        (owner && name && name[0]) ? tree_static_slot(owner, name, false)
                                   : nullptr;
    VmValueRef ref;
    if (slot) {
      ref.kind = VmRefKind::Static;
      ref.owner = owner;
      ref.name = name;
    }
    vmthread_value_stack_push_ref(
        thr, slot ? *slot : JvmValue::make_obj(nullptr), ref);
    thr->pe.unk_24 += 8;
    return;
  }
  InvObject* self = nullptr;
  if (thr->curr_frame && !thr->curr_frame->locals.empty() &&
      thr->curr_frame->pe.instance != 0 &&  // Fork: static frame has no this
      thr->curr_frame->locals[0].tag == JvmTag::Obj)
    self = thr->curr_frame->locals[0].v.o;  // Soft stand-in for frame+0x30
  if (!self || !name || !name[0]) {
    vmthread_value_stack_push(thr, JvmValue::make_obj(nullptr));
    thr->pe.unk_24 += 8;
    return;
  }
  // PE pushes the ValueField* Object_getFieldAt returned, so the field stays
  // assignable; op29_field_get only yields the value, hence the ref beside it.
  vmthread_op29_field_get(thr, self, name);  // TREE field bag
  if (thr->curr_frame && !thr->curr_frame->operand_ref.empty()) {
    VmValueRef& top = thr->curr_frame->operand_ref.back();
    top.kind = VmRefKind::Field;
    top.obj = self;
    top.name = name;
  }
  thr->pe.unk_24 += 8;  // PE → VMThread_opDefault_advance
}

namespace {
// Same ambiguity as op29 @ 0x4214BC: the PE hands back a typed Value*, the host
// TREE bag is untyped, so probe obj → int → float and settle on 0.
JvmValue vmthread_field_value(InvObject* obj, const std::string& name) {
  if (!obj || name.empty()) return JvmValue::make_obj(nullptr);
  // Fork: the bag slot is typed once written; read it as-is. An unwritten
  // field takes the default of its DECLARED type (PE ValueField ctor with
  // the FILD type): null for L/[ , 0.f for F, 0 otherwise. The old probe
  // returned Int 0 for an unset String field (Dialog.title -> createText
  // received an int where a String was declared).
  if (JvmValue* slot = tree_field_slot(obj, name, false)) {
    if (slot->tag != JvmTag::Void) return *slot;
  }
  if (Jvm* j = jvm_active()) {
    const char* hc = tree_host_class(obj);
    const JvmClass* c = (hc && hc[0]) ? j->find_class(hc) : nullptr;
    for (int guard = 0; c && guard < 64; ++guard) {
      for (const JvmFieldDecl& fd : c->instance_fields) {
        if (fd.name != name) continue;
        const char t = fd.type.empty() ? 'I' : fd.type[0];
        if (t == 'L' || t == '[') return JvmValue::make_obj(nullptr);
        if (t == 'F') return JvmValue::make_float(0.f);
        return JvmValue::make_int(0);
      }
      if (c->super_name.empty()) break;
      if (!j->find_class(c->super_name.c_str())) j->load_class(c->super_name.c_str());
      c = j->find_class(c->super_name.c_str());
    }
  }
  return JvmValue::make_int(0);
}
}  // namespace

// Soft ≡ PE JVM_getClass @ 0x410890 name prep: strip L…; , keep [arrays,
// slash→dot. Shared by class literals (0x1007 tag 0x1E) and Class CP resolve.
std::string vmthread_normalize_class_desc(const char* desc) {
  if (!desc || !desc[0]) return {};
  if (desc[0] == '[') return desc;
  const char* p = desc;
  size_t n = std::strlen(desc);
  if (p[0] == 'L' && n >= 2 && p[n - 1] == ';') {
    ++p;
    n -= 2;
  }
  std::string out(p, n);
  for (char& c : out)
    if (c == '/') c = '.';
  return out;
}

// Soft ≡ PE JVM_resolveClassFromCP @ 0x4117A0:
//   ConstantPool_getClass(cp, idx) requires kind==4, returns utf8 index;
//   getUtf8 + JVM_getClass. TUFA stores Class refs as int32 utf8 indices in
//   const_ints[], so the soft path is const_ints[idx] → const_strings[utf8].
const JvmClass* vmthread_resolve_class_from_cp(const JvmClass* cls,
                                               uint32_t imm) {
  if (!cls) return nullptr;
  Jvm* j = jvm_active();
  if (!j) return nullptr;
  std::string name;
  if (imm < cls->const_int_valid.size() && cls->const_int_valid[imm]) {
    const int32_t utf8 = cls->const_ints[imm];
    if (utf8 >= 0 &&
        static_cast<size_t>(utf8) < cls->const_strings.size())
      name = cls->const_strings[static_cast<size_t>(utf8)];
  }
  if (name.empty() && imm < cls->const_strings.size())
    name = cls->const_strings[imm];
  name = vmthread_normalize_class_desc(name.c_str());
  if (name.empty()) return nullptr;
  if (!j->find_class(name.c_str())) j->load_class(name.c_str());
  return j->find_class(name.c_str());
}

// PE Thread_evalName_fieldPath @ 0x420D20, reached only from op 0x1011
// @ 0x422E98. It walks insn.imm segment nodes, advancing the PC by 8 BEFORE
// reading each one (@ 0x420D4F), so on return the PC sits on the LAST segment
// node and the op site's opDefault_advance supplies the final +8.
//
// Loop counter is remaining=count-1 (PE @ 0x420D34): each iteration lands on
// one segment node; the trailing check exits when remaining was already 0.
// seg0 0x1019 @ 0x420D5A: JVM_resolveClassFromCP, then PC+=8 / remaining--
// to land on the companion field node (0x1001/0x101B) in the SAME iteration.
// Else ctx = frame+0x30 ? obj+0xC : frame+0x34. Later segments need a live
// object payload with an L/[ descriptor.
//
// Per segment: 0x1001 locals[imm], 0x101B CP fieldref (static bit →
// Class_getFieldAt on the context class), else getUtf8+Object_getField
// (seg0 → "parse error (2)").
bool vmthread_op1011_eval_name(VmThread* thr, const JvmClass* cls,
                               const TreeBody& code, uint32_t* pc,
                               int32_t segments, JvmValue* out,
                               VmValueRef* out_ref) {
  if (!thr || !cls || !pc || !out || !out_ref) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op1011 fail L%d\n", __LINE__); return false; }
  if (segments <= 0 || segments > 64) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op1011 fail L%d\n", __LINE__); return false; }
  *out = JvmValue{};
  *out_ref = VmValueRef{};

  InvObject* self = nullptr;  // PE frame+0x30
  if (thr->curr_frame && !thr->curr_frame->locals.empty() &&
      thr->curr_frame->pe.instance != 0 &&  // Fork: static frame has no this
      thr->curr_frame->locals[0].tag == JvmTag::Obj)
    self = thr->curr_frame->locals[0].v.o;

  const JvmClass* ctx_class = cls;
  int32_t remaining = segments - 1;  // PE @ 0x420D34
  bool first = true;
  for (;;) {
    *pc += 8;  // PE advances before reading the segment node
    size_t idx = *pc / 8u;
    if (idx >= code.nodes.size()) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op1011 fail L%d\n", __LINE__); return false; }
    TreeNode n = code.nodes[idx];
    uint16_t op = vmthread_pe_opcode(n);
    bool after_classname = false;

    if (first && op == 0x1019) {
      // PE @ 0x420D5A: resolve class, refuse "classname found alone", then
      // land on the companion node in this same iteration.
      ctx_class = vmthread_resolve_class_from_cp(cls, n.imm);
      if (!ctx_class) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op1011 fail L%d\n", __LINE__); return false; }
      if (remaining == 0) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op1011 fail L%d\n", __LINE__); return false; }
      *pc += 8;
      --remaining;
      idx = *pc / 8u;
      if (idx >= code.nodes.size()) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op1011 fail L%d\n", __LINE__); return false; }
      n = code.nodes[idx];
      op = vmthread_pe_opcode(n);
      after_classname = true;
    }

    InvObject* recv =
        first ? self : (out->tag == JvmTag::Obj ? out->v.o : nullptr);
    if (!first && !recv) {
      // PE Thread_evalName_fieldPath @ 0x562280: "[illegal fieldaccess]
      // null.%s" ScriptError, then the path yields null and the thread
      // continues. Fork: do the same instead of bailing the whole body.
      if (vm_trace_on())
        std::fprintf(stderr, "[pe-stream]   script error: illegal fieldaccess null.<seg %d> in %s\n",
                     segments - remaining - 1, cls->name.c_str());
      *out = JvmValue::make_obj(nullptr);
      *out_ref = VmValueRef{};
      // skip the remaining segment nodes so the PC lands on the last one
      while (remaining > 0) { *pc += 8; --remaining; }
      return true;
    }

    if (op == 0x1001) {  // PE @ 0x420F99 locals[imm]
      const uint32_t li = n.imm;
      if (!thr->curr_frame || li >= thr->curr_frame->locals.size()) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op1011 fail L%d\n", __LINE__); return false; }
      *out = thr->curr_frame->locals[li];
      out_ref->kind = VmRefKind::Local;
      out_ref->local_index = li;
      out_ref->obj = nullptr;
      out_ref->owner = nullptr;
      out_ref->name.clear();
    } else if (op == 0x101B) {  // PE @ 0x420E87 CP fieldref
      std::string fname;
      bool is_static = false;
      const JvmClass* owner = nullptr;
      if (!vmthread_resolve_field_ref(cls, ctx_class, n.imm, &fname, &is_static,
                                      &owner))
        { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op1011 fail L%d\n", __LINE__); return false; }
      if (is_static) {
        JvmValue* slot = tree_static_slot(owner, fname, false);
        if (!slot) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op1011 fail L%d\n", __LINE__); return false; }
        *out = *slot;
        out_ref->kind = VmRefKind::Static;
        out_ref->owner = owner;
        out_ref->obj = nullptr;
        out_ref->name = fname;
      } else {
        if (!recv) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op1011 fail L%d\n", __LINE__); return false; }
        *out = vmthread_field_value(recv, fname);
        out_ref->kind = VmRefKind::Field;
        out_ref->obj = recv;
        out_ref->owner = nullptr;
        out_ref->name = fname;
      }
    } else {
      // PE @ 0x420E4E: seg0 non-1001/101B → ScriptError "parse error (2)".
      // Soft: after 0x1019, utf8/0x4012 companion = static field on the
      // resolved class (stock MainMenu ClassName.CMD_* / MUSIC_*).
      if (n.imm >= cls->const_strings.size()) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op1011 fail L%d\n", __LINE__); return false; }
      const std::string& fname = cls->const_strings[n.imm];
      if (fname.empty()) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op1011 fail L%d\n", __LINE__); return false; }
      if (first) {
        if (!after_classname || !ctx_class) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op1011 fail L%d\n", __LINE__); return false; }
        JvmValue* slot = tree_static_slot(ctx_class, fname, true);
        if (!slot) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op1011 fail L%d\n", __LINE__); return false; }
        *out = *slot;
        out_ref->kind = VmRefKind::Static;
        out_ref->owner = ctx_class;
        out_ref->obj = nullptr;
        out_ref->name = fname;
      } else {
        *out = vmthread_field_value(recv, fname);
        out_ref->kind = VmRefKind::Field;
        out_ref->obj = recv;
        out_ref->owner = nullptr;
        out_ref->name = fname;
      }
    }

    // PE @ 0x420FB7: exit when remaining was already 0 before the dec.
    const int32_t prev = remaining;
    --remaining;
    first = false;
    if (prev == 0) break;
  }
  return true;
}

// Static twin — remaining=count-1 walk matching the PE loop, including the
// 0x1019 → companion collapse on seg0. Callers must first exclude op24 count
// carriers (prev pe-op == 0x24) — those never reach fieldPath.
bool vmthread_op1011_shape_walkable(const TreeBody& code, size_t idx) {
  const int32_t segments = static_cast<int32_t>(code.nodes[idx].imm);
  if (segments <= 0 || segments > 64) return false;
  size_t pc = idx;
  int32_t remaining = segments - 1;
  bool first = true;
  for (;;) {
    if (++pc >= code.nodes.size()) return false;
    uint16_t op = vmthread_pe_opcode(code.nodes[pc]);
    if (first && op == 0x1019) {
      // Companion may be 0x1001/0x101B (PE) or utf8/0x4012 (Soft static).
      if (remaining == 0) return false;
      if (++pc >= code.nodes.size()) return false;
      --remaining;
    } else if (first && op != 0x1001 && op != 0x101B) {
      return false;
    }
    const int32_t prev = remaining;
    --remaining;
    first = false;
    if (prev == 0) break;
  }
  return true;
}

// PE Thread_evalName @ 0x4208E0, reached only from op 0x24 (case 36) @ 0x4212E4.
// Distinct from Thread_evalName_fieldPath @ 0x420D20 (op 0x1011): this one
// READS the node at the PC, has a 0x101A method case, and returns a call
// target rather than a ValueField. The call site has already advanced PC past
// the 0x24 node before entering.
//
// Header @ 0x420900: node 0x101A → force count=1 and stay on it; else
// imm=segCount and advance past this count-bearing node (often a raw carrier
// whose pe-op is 0x1011 — only the imm matters).
// Segments: 0x1019 classname (JVM_resolveClassFromCP @ 0x4117A0 + companion),
// 0x1001 local, 0x101B fieldref, 0x101A methodref (pop recv, resolve, RETURN),
// else utf8/0x4012 → script method marker (RETURN) or Object_getField continue.
// On success PC sits ON the terminator; the call site then invokeMethods and
// either opDefault_advance (+8 past it) or afterPc (Java queued).
bool vmthread_op24_eval_name(VmThread* thr, const JvmClass* cls,
                             const TreeBody& code, uint32_t* pc,
                             std::string* class_out, std::string* method_out,
                             InvObject** self_out) {
  if (!thr || !cls || !pc || !class_out || !method_out || !self_out)
    { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
  class_out->clear();
  method_out->clear();
  *self_out = nullptr;

  size_t idx = *pc / 8u;
  if (idx >= code.nodes.size()) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
  const TreeNode& hdr = code.nodes[idx];
  const uint16_t hop = vmthread_pe_opcode(hdr);

  int32_t remaining = 0;
  if (hop == 0x101A) {
    remaining = 1;  // PE @ 0x420902: force count=1, do not advance yet
  } else {
    remaining = static_cast<int32_t>(hdr.imm);
    if (remaining <= 0 || remaining > 64) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
    *pc += 8;  // PE @ 0x420912: advance past the count-bearing node
    idx = *pc / 8u;
  }

  InvObject* frame_this = nullptr;
  if (thr->curr_frame && !thr->curr_frame->locals.empty() &&
      thr->curr_frame->pe.instance != 0 &&  // Fork: static frame has no this
      thr->curr_frame->locals[0].tag == JvmTag::Obj)
    frame_this = thr->curr_frame->locals[0].v.o;

  const JvmClass* ctx_class = cls;
  InvObject* ctx_obj = frame_this;
  bool first = true;
  JvmValue prev{};

  while (remaining > 0) {
    idx = *pc / 8u;
    if (idx >= code.nodes.size()) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
    const TreeNode& n = code.nodes[idx];
    const uint16_t op = vmthread_pe_opcode(n);

    if (first) {
      if (op == 0x1019) {
        // PE @ 0x42093A: resolve class, refuse alone, land on companion.
        ctx_class = vmthread_resolve_class_from_cp(cls, n.imm);
        if (!ctx_class) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
        if (remaining <= 1) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }  // classname alone
        *pc += 8;
        --remaining;
        idx = *pc / 8u;
        if (idx >= code.nodes.size()) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
        // Re-fetch companion as the real segment node (still first).
        const TreeNode& cn = code.nodes[idx];
        const uint16_t cop = vmthread_pe_opcode(cn);
        // Fall through using companion — rewrite locals via goto-style block.
        // Soft: handle companion inline then continue the while.
        if (cop == 0x1001) {
          const uint32_t li = cn.imm;
          if (!thr->curr_frame || li >= thr->curr_frame->locals.size())
            { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
          prev = thr->curr_frame->locals[li];
          *pc += 8;
          first = false;
          --remaining;
          continue;
        }
        if (cop == 0x101B) {
          std::string fname;
          bool is_static = false;
          const JvmClass* owner = nullptr;
          if (!vmthread_resolve_field_ref(cls, ctx_class, cn.imm, &fname, &is_static,
                                          &owner))
            { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
          if (is_static) {
            JvmValue* slot = tree_static_slot(owner, fname, false);
            if (!slot) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
            prev = *slot;
          } else {
            if (!ctx_obj) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
            prev = vmthread_field_value(ctx_obj, fname);
          }
          *pc += 8;
          first = false;
          --remaining;
          continue;
        }
        if (cop == 0x101A) {
          // method terminator after classname: `ClassName.method(...)` is a
          // static call. Fork: nothing on the operand stack is a receiver
          // here (the old pop grabbed the argc box or a leftover value).
          InvObject* recv = nullptr;
          if (cn.imm >= cls->const_mref_name.size()) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
          const std::string& mname = cls->const_mref_name[cn.imm];
          if (mname.empty()) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
          thr->call_sig_hint = (cn.imm < cls->const_mref_sig.size())
                                   ? cls->const_mref_sig[cn.imm]
                                   : std::string();
          *method_out = mname;
          if (recv) {
            *self_out = recv;
            const char* cnm = tree_host_class(recv);
            *class_out = (cnm && cnm[0]) ? cnm : ctx_class->name;
          } else {
            *self_out = nullptr;
            *class_out = ctx_class->name;
          }
          return true;
        }
        // utf8 / 0x4012 script method on companion
        if (cn.imm >= cls->const_strings.size()) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
        const std::string& mname = cls->const_strings[cn.imm];
        if (mname.empty()) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
        *method_out = mname;
        *self_out = nullptr;  // Fork: ClassName.method(...) is a static call
        *class_out = ctx_class->name;
        return true;
      }
      // PE @ 0x420981: ctx = frame+0x30 ? this+0xC : frame+0x34
      ctx_class = cls;
      ctx_obj = frame_this;
    } else {
      // PE @ 0x42099B: "Thread::evalName: null.methodcall()" ScriptError.
      // Fork: remember it and keep walking so the PC lands on the method
      // terminator; the call site then skips the call and yields null.
      if (prev.tag != JvmTag::Obj || !prev.v.o) {
        if (vm_trace_on())
          std::fprintf(stderr, "[pe-stream]   script error: null.methodcall() in %s\n",
                       cls->name.c_str());
        thr->null_recv_error = true;
        ctx_obj = nullptr;
      } else {
        ctx_obj = prev.v.o;
      }
    }

    if (op == 0x1001) {  // PE @ 0x420B62 locals[imm]
      const uint32_t li = n.imm;
      if (!thr->curr_frame || li >= thr->curr_frame->locals.size()) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
      prev = thr->curr_frame->locals[li];
      *pc += 8;  // PE LABEL_45
      first = false;
      --remaining;
      continue;
    }

    if (op == 0x101B) {  // PE @ 0x4209F0 CP fieldref
      std::string fname;
      bool is_static = false;
      const JvmClass* owner = nullptr;
      if (!vmthread_resolve_field_ref(cls, ctx_class, n.imm, &fname, &is_static,
                                      &owner))
        { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
      if (is_static) {
        JvmValue* slot = tree_static_slot(owner, fname, false);
        if (!slot) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
        prev = *slot;
      } else {
        if (!ctx_obj) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
        prev = vmthread_field_value(ctx_obj, fname);
      }
      *pc += 8;
      first = false;
      --remaining;
      continue;
    }

    if (op == 0x101A) {
      // PE @ 0x420C0D: pop recv; resolve method from the fieldref at this node;
      // return the call target. PC stays ON the 0x101A (no advance here).
      // Fork: when a previous path segment (local / field) already produced
      // the receiver (`ff.close()` compiles to 0x1001 ff, 0x101A close), the
      // host keeps it in `prev` instead of the operand stack, so use it
      // rather than popping an unrelated value.
      JvmValue recv_v;
      if (!first && prev.tag == JvmTag::Obj && prev.v.o) {
        recv_v = prev;
      } else if (first) {
        // Fork: a leading methodref is an implicit-this call (`first(path,
        // FILES_DIRS)` inside FindFile); the args + argc box are on the
        // operand stack, the receiver is not. Chained `expr.m()` is TODO.
        recv_v = frame_this ? JvmValue::make_obj(frame_this) : JvmValue{};
      } else if (!vmthread_pop_operand(thr, &recv_v)) {
        recv_v = JvmValue{};
      }
      InvObject* recv =
          (recv_v.tag == JvmTag::Obj) ? recv_v.v.o : nullptr;
      if (n.imm >= cls->const_mref_name.size()) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
      const std::string& mname = cls->const_mref_name[n.imm];
      if (mname.empty()) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
      thr->call_sig_hint = (n.imm < cls->const_mref_sig.size())
                               ? cls->const_mref_sig[n.imm]
                               : std::string();
      // Soft: ACC_STATIC is not on the host CP cache. Prefer an instance call
      // when the popped recv is live (PE Object_getFieldAt / instance path);
      // otherwise treat as static against the context class.
      *method_out = mname;
      if (recv) {
        *self_out = recv;
        // PE: obj->clazz. A host-created object (Viewport from GfxEngine)
        // may carry no class tag; the methodref's own class is then the
        // right owner (viewport.getHeight() -> java.render.Viewport).
        const char* cn = tree_host_class(recv);
        const std::string mc = (n.imm < cls->const_mref_class.size())
                                   ? cls->const_mref_class[n.imm]
                                   : std::string();
        *class_out = (cn && cn[0]) ? cn : (!mc.empty() ? mc : ctx_class->name);
      } else {
        *self_out = nullptr;
        *class_out = ctx_class->name;
      }
      return true;
    }

    // PE @ 0x4209FF: utf8 name (0x4012 and any other carrier). On seg0, or
    // when Object_getField misses, build the script-method marker and return.
    // Soft: always take the script-method path — that is the invoke op 0x24
    // needs; a pure field hit continuing the walk cannot produce a call target
    // for this opcode (exhausted count → nullptr → "script error").
    if (n.imm >= cls->const_strings.size()) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
    const std::string& mname = cls->const_strings[n.imm];
    if (mname.empty()) { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }
    *method_out = mname;
    *self_out = first ? frame_this : ctx_obj;
    *class_out = ctx_class->name;
    if (*self_out) {
      const char* cn = tree_host_class(*self_out);
      if (cn && cn[0]) *class_out = cn;
    }
    // PC stays ON the name carrier (PE returns without advancing).
    return true;
  }
  { if (vm_trace_on()) std::fprintf(stderr, "[pe-stream]   op24 fail L%d\n", __LINE__); return false; }  // PE exhausted without a method → nullptr → "script error"
}