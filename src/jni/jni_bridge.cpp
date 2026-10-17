#include "jni_bridge.h"

#include <array>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../hle/hle.h"
#include "../mem.h"

namespace zb::jni {
namespace {

using hle::gcstr;

JavaVM* g_vm = nullptr;
gaddr g_guest_vm = 0;
gaddr g_env_table = 0;

// ------------------------------------------------------------ reference handles

enum class Kind : u8 { Free, Local, Global, Weak };

struct Slot {
    jobject obj = nullptr;
    Kind kind = Kind::Free;
    u32 gen = 0;
    u32 next_free = UINT32_MAX;
};

std::mutex g_ref_lock;
std::vector<Slot> g_slots;
u32 g_free_head = UINT32_MAX;

using Frame = std::vector<std::pair<u32, u32>>;  // (slot, generation)
thread_local std::vector<Frame> t_frames;

u32 handle_of(u32 idx) { return (idx << 2) | 2; }
bool idx_of(u32 h, u32* idx) {
    if ((h & 3) != 2) return false;
    *idx = h >> 2;
    return true;
}

u32 alloc_slot(jobject o, Kind k, u32* gen_out) {
    std::lock_guard lk(g_ref_lock);
    u32 idx;
    if (g_free_head != UINT32_MAX) {
        idx = g_free_head;
        g_free_head = g_slots[idx].next_free;
    } else {
        idx = static_cast<u32>(g_slots.size());
        if (idx >= (1u << 29)) fatal("JNI handle table exhausted");
        g_slots.emplace_back();
    }
    Slot& s = g_slots[idx];
    s.obj = o;
    s.kind = k;
    s.gen++;
    if (gen_out) *gen_out = s.gen;
    return idx;
}

void free_slot_locked(u32 idx) {
    Slot& s = g_slots[idx];
    s.obj = nullptr;
    s.kind = Kind::Free;
    s.next_free = g_free_head;
    g_free_head = idx;
}

Kind kind_of(u32 h) {
    u32 idx;
    if (!idx_of(h, &idx)) return Kind::Free;
    std::lock_guard lk(g_ref_lock);
    return idx < g_slots.size() ? g_slots[idx].kind : Kind::Free;
}

void release_handle(u32 h) {
    u32 idx;
    if (!idx_of(h, &idx)) return;
    std::lock_guard lk(g_ref_lock);
    if (idx < g_slots.size() && g_slots[idx].kind != Kind::Free) free_slot_locked(idx);
}

Frame& top_frame() {
    if (t_frames.empty()) t_frames.emplace_back();
    return t_frames.back();
}

void release_frame(const Frame& f) {
    std::lock_guard lk(g_ref_lock);
    for (auto [idx, gen] : f) {
        if (idx < g_slots.size() && g_slots[idx].gen == gen && g_slots[idx].kind == Kind::Local) free_slot_locked(idx);
    }
}

// ------------------------------------------------------------ method / field ids

struct MethodInfo {
    jmethodID id = nullptr;
    std::string args;  // one char per parameter: ZBCSIJFDL ('L' covers arrays)
    char ret = 'V';
    bool known = true;
    std::string name;  // "name(sig)", for tracing
};
struct FieldInfo {
    jfieldID id = nullptr;
};

std::mutex g_id_lock;
std::vector<MethodInfo> g_methods;
std::unordered_map<jmethodID, u32> g_method_idx;
std::vector<FieldInfo> g_fields;
std::unordered_map<jfieldID, u32> g_field_idx;

bool parse_sig(const char* sig, std::string* args, char* ret) {
    const char* p = sig;
    if (*p++ != '(') return false;
    while (*p && *p != ')') {
        char c = *p;
        if (c == '[') {
            while (*p == '[') p++;
            if (*p == 'L') {
                while (*p && *p != ';') p++;
            }
            if (*p) p++;
            args->push_back('L');
        } else if (c == 'L') {
            while (*p && *p != ';') p++;
            if (*p) p++;
            args->push_back('L');
        } else {
            args->push_back(c);
            p++;
        }
    }
    if (*p != ')') return false;
    p++;
    *ret = (*p == '[') ? 'L' : *p;
    return true;
}

u32 method_handle(jmethodID id, const char* sig, const char* name = nullptr) {
    if (!id) return 0;
    std::lock_guard lk(g_id_lock);
    if (auto it = g_method_idx.find(id); it != g_method_idx.end()) return (it->second + 1) << 3;
    MethodInfo mi;
    mi.id = id;
    if (!sig || !parse_sig(sig, &mi.args, &mi.ret)) mi.known = false;
    mi.name = std::string(name ? name : "?") + (sig ? sig : "");
    u32 idx = static_cast<u32>(g_methods.size());
    g_methods.push_back(std::move(mi));
    g_method_idx.emplace(id, idx);
    return (idx + 1) << 3;
}

const MethodInfo& method(u32 h) {
    std::lock_guard lk(g_id_lock);
    u32 idx = (h >> 3) - 1;
    if (h == 0 || idx >= g_methods.size()) fatal("invalid guest jmethodID 0x%x", h);
    return g_methods[idx];
}

u32 field_handle(jfieldID id) {
    if (!id) return 0;
    std::lock_guard lk(g_id_lock);
    if (auto it = g_field_idx.find(id); it != g_field_idx.end()) return (it->second + 1) << 3;
    u32 idx = static_cast<u32>(g_fields.size());
    g_fields.push_back({id});
    g_field_idx.emplace(id, idx);
    return (idx + 1) << 3;
}

jfieldID field(u32 h) {
    std::lock_guard lk(g_id_lock);
    u32 idx = (h >> 3) - 1;
    if (h == 0 || idx >= g_fields.size()) fatal("invalid guest jfieldID 0x%x", h);
    return g_fields[idx].id;
}

// ------------------------------------------------------------ helpers

jobject H(u32 h) { return to_host(h); }
jclass HC(u32 h) { return static_cast<jclass>(to_host(h)); }

struct VaReader {
    u32 ap;
    u32 w32() {
        u32 v = rd<u32>(ap);
        ap += 4;
        return v;
    }
    u64 w64() {
        ap = (ap + 7) & ~7u;
        u64 v = rd<u64>(ap);
        ap += 8;
        return v;
    }
};

void decode_va(const MethodInfo& mi, u32 ap, jvalue* out) {
    if (!mi.known) fatal("JNI call through a method id with unknown signature");
    VaReader va{ap};
    for (size_t i = 0; i < mi.args.size(); ++i) {
        switch (mi.args[i]) {
        case 'Z': out[i].z = static_cast<jboolean>(va.w32()); break;
        case 'B': out[i].b = static_cast<jbyte>(va.w32()); break;
        case 'C': out[i].c = static_cast<jchar>(va.w32()); break;
        case 'S': out[i].s = static_cast<jshort>(va.w32()); break;
        case 'I': out[i].i = static_cast<jint>(va.w32()); break;
        case 'J': out[i].j = static_cast<jlong>(va.w64()); break;
        case 'F': {  // promoted to double
            u64 b = va.w64();
            double d;
            memcpy(&d, &b, 8);
            out[i].f = static_cast<jfloat>(d);
            break;
        }
        case 'D': {
            u64 b = va.w64();
            memcpy(&out[i].d, &b, 8);
            break;
        }
        default: out[i].l = H(va.w32()); break;
        }
    }
}

void decode_jvalues(const MethodInfo& mi, gaddr arr, jvalue* out) {
    if (!mi.known) fatal("JNI call through a method id with unknown signature");
    for (size_t i = 0; i < mi.args.size(); ++i) {
        gaddr a = arr + 8 * static_cast<u32>(i);
        switch (mi.args[i]) {
        case 'Z': out[i].z = rd<u8>(a); break;
        case 'B': out[i].b = rd<i8>(a); break;
        case 'C': out[i].c = rd<u16>(a); break;
        case 'S': out[i].s = rd<i16>(a); break;
        case 'I': out[i].i = rd<i32>(a); break;
        case 'J': out[i].j = rd<i64>(a); break;
        case 'F': out[i].f = rd<float>(a); break;
        case 'D': out[i].d = rd<double>(a); break;
        default: out[i].l = H(rd<u32>(a)); break;
        }
    }
}

enum CallKind { kVirtual = 0, kNonvirtual = 1, kStatic = 2 };
enum VarKind { kDots = 0, kVaList = 1, kArray = 2 };

template <char R>
u64 invoke(JNIEnv* e, int kind, jobject obj, jclass cls, jmethodID mid, const jvalue* a) {
#define ZB_CALL(Type) \
    (kind == kVirtual ? e->Call##Type##MethodA(obj, mid, a) \
                      : kind == kNonvirtual ? e->CallNonvirtual##Type##MethodA(obj, cls, mid, a) \
                                            : e->CallStatic##Type##MethodA(cls, mid, a))
    if constexpr (R == 'V') {
        if (kind == kVirtual) e->CallVoidMethodA(obj, mid, a);
        else if (kind == kNonvirtual) e->CallNonvirtualVoidMethodA(obj, cls, mid, a);
        else e->CallStaticVoidMethodA(cls, mid, a);
        return 0;
    } else if constexpr (R == 'Z') {
        return ZB_CALL(Boolean);
    } else if constexpr (R == 'B') {
        return static_cast<u32>(static_cast<i32>(ZB_CALL(Byte)));
    } else if constexpr (R == 'C') {
        return ZB_CALL(Char);
    } else if constexpr (R == 'S') {
        return static_cast<u32>(static_cast<i32>(ZB_CALL(Short)));
    } else if constexpr (R == 'I') {
        return static_cast<u32>(ZB_CALL(Int));
    } else if constexpr (R == 'J') {
        return static_cast<u64>(ZB_CALL(Long));
    } else if constexpr (R == 'F') {
        jfloat f = ZB_CALL(Float);
        u32 b;
        memcpy(&b, &f, 4);
        return b;
    } else if constexpr (R == 'D') {
        jdouble d = ZB_CALL(Double);
        u64 b;
        memcpy(&b, &d, 8);
        return b;
    } else {
        return new_local(ZB_CALL(Object));
    }
#undef ZB_CALL
}

template <char R, int Kind, int Var>
void call_thunk(JitCtx& j) {
    ArgReader r(j);
    r.w32();  // env
    jobject obj = nullptr;
    jclass cls = nullptr;
    if (Kind != kStatic) obj = H(r.w32());
    if (Kind != kVirtual) cls = HC(r.w32());
    const MethodInfo& mi = method(r.w32());
    ZLOGD("JNI call %s", mi.name.c_str());
    std::array<jvalue, 64> args{};
    if (mi.args.size() > args.size()) fatal("JNI call with too many arguments");
    if (Var == kDots) decode_va(mi, r.va_list_here(), args.data());
    else if (Var == kVaList) decode_va(mi, r.w32(), args.data());
    else decode_jvalues(mi, r.w32(), args.data());
    u64 v = invoke<R>(env(), Kind, obj, cls, mi.id, args.data());
    if (R == 'J' || R == 'D') j.set_ret64(v);
    else j.set_ret(static_cast<u32>(v));
}

template <int Var>
void new_object_thunk(JitCtx& j) {
    ArgReader r(j);
    r.w32();
    jclass cls = HC(r.w32());
    const MethodInfo& mi = method(r.w32());
    std::array<jvalue, 64> args{};
    if (Var == kDots) decode_va(mi, r.va_list_here(), args.data());
    else if (Var == kVaList) decode_va(mi, r.w32(), args.data());
    else decode_jvalues(mi, r.w32(), args.data());
    j.set_ret(new_local(env()->NewObjectA(cls, mi.id, args.data())));
}

std::string class_name(JNIEnv* e, jclass c) {
    static jmethodID get_name = nullptr;
    if (!get_name) {
        jclass cc = e->FindClass("java/lang/Class");
        get_name = e->GetMethodID(cc, "getName", "()Ljava/lang/String;");
        e->DeleteLocalRef(cc);
    }
    auto s = static_cast<jstring>(e->CallObjectMethod(c, get_name));
    const char* u = e->GetStringUTFChars(s, nullptr);
    std::string r(u);
    e->ReleaseStringUTFChars(s, u);
    e->DeleteLocalRef(s);
    for (char& ch : r) {
        if (ch == '.') ch = '/';
    }
    return r;
}

std::string descriptor_of(JNIEnv* e, jclass c) {
    std::string n = class_name(e, c);
    static const std::unordered_map<std::string, std::string> prim = {
        {"int", "I"}, {"long", "J"}, {"float", "F"}, {"double", "D"}, {"boolean", "Z"},
        {"byte", "B"}, {"char", "C"}, {"short", "S"}, {"void", "V"}};
    if (auto it = prim.find(n); it != prim.end()) return it->second;
    if (!n.empty() && n[0] == '[') return n;
    return "L" + n + ";";
}

// Rebuilds a method signature from a java.lang.reflect.Method/Constructor.
std::string reflected_signature(JNIEnv* e, jobject m) {
    jclass mc = e->GetObjectClass(m);
    jmethodID params = e->GetMethodID(mc, "getParameterTypes", "()[Ljava/lang/Class;");
    jmethodID rett = e->GetMethodID(mc, "getReturnType", "()Ljava/lang/Class;");
    e->ExceptionClear();
    std::string sig = "(";
    auto arr = static_cast<jobjectArray>(e->CallObjectMethod(m, params));
    for (jsize i = 0, n = arr ? e->GetArrayLength(arr) : 0; i < n; ++i) {
        auto c = static_cast<jclass>(e->GetObjectArrayElement(arr, i));
        sig += descriptor_of(e, c);
        e->DeleteLocalRef(c);
    }
    sig += ")";
    if (rett) {
        auto rc = static_cast<jclass>(e->CallObjectMethod(m, rett));
        sig += descriptor_of(e, rc);
        e->DeleteLocalRef(rc);
    } else {
        e->ExceptionClear();
        sig += "V";  // constructor
    }
    e->DeleteLocalRef(mc);
    return sig;
}

// ------------------------------------------------------------ registered natives

std::mutex g_nat_lock;
std::unordered_map<std::string, const NativeEntry*> g_natives;  // "cls.name sig"
std::unordered_map<std::string, const NativeEntry*> g_natives_by_name;  // "name sig" (unique only)

// ------------------------------------------------------------ JNIEnv functions

#define E env()

i32 j_GetVersion(u32) { return JNI_VERSION_1_6; }
u32 j_DefineClass(u32, gcstr name, u32, gaddr, i32) {
    ZLOGW("JNI DefineClass(%s) is not supported", name.a ? name.get() : "?");
    return 0;
}
u32 j_FindClass(u32, gcstr name) {
    jclass c = E->FindClass(name);
    if (!c) ZLOGD("FindClass(%s) failed", name.get());
    return new_local(c);
}
u32 j_FromReflectedMethod(u32, u32 m) {
    JNIEnv* e = E;
    jmethodID id = e->FromReflectedMethod(H(m));
    std::string sig = reflected_signature(e, H(m));
    return method_handle(id, sig.c_str());
}
u32 j_FromReflectedField(u32, u32 f) { return field_handle(E->FromReflectedField(H(f))); }
u32 j_ToReflectedMethod(u32, u32 cls, u32 mid, u8 is_static) {
    return new_local(E->ToReflectedMethod(HC(cls), method(mid).id, is_static));
}
u32 j_GetSuperclass(u32, u32 cls) { return new_local(E->GetSuperclass(HC(cls))); }
u8 j_IsAssignableFrom(u32, u32 a, u32 b) { return E->IsAssignableFrom(HC(a), HC(b)); }
u32 j_ToReflectedField(u32, u32 cls, u32 fid, u8 is_static) {
    return new_local(E->ToReflectedField(HC(cls), field(fid), is_static));
}
i32 j_Throw(u32, u32 t) { return E->Throw(static_cast<jthrowable>(H(t))); }
i32 j_ThrowNew(u32, u32 cls, gcstr msg) { return E->ThrowNew(HC(cls), msg.a ? msg.get() : nullptr); }
u32 j_ExceptionOccurred(u32) { return new_local(E->ExceptionOccurred()); }
void j_ExceptionDescribe(u32) { E->ExceptionDescribe(); }
void j_ExceptionClear(u32) { E->ExceptionClear(); }
void j_FatalError(JitCtx& j, u32, gcstr msg) {
    cpu::dump_state(j, "JNI FatalError");
    fatal("guest JNI FatalError: %s", msg.a ? msg.get() : "");
}
i32 j_PushLocalFrame(u32, i32 cap) {
    i32 r = E->PushLocalFrame(cap);
    if (r == 0) t_frames.emplace_back();
    return r;
}
u32 j_PopLocalFrame(u32, u32 result) {
    jobject res = E->PopLocalFrame(H(result));
    if (t_frames.size() > 1) {
        release_frame(t_frames.back());
        t_frames.pop_back();
    }
    return new_local(res);
}
u32 j_NewGlobalRef(u32, u32 o) { return new_global_handle(H(o)); }
void j_DeleteGlobalRef(u32, u32 h) {
    if (kind_of(h) != Kind::Global) return;
    E->DeleteGlobalRef(H(h));
    release_handle(h);
}
void j_DeleteLocalRef(u32, u32 h) {
    if (kind_of(h) != Kind::Local) return;
    E->DeleteLocalRef(H(h));
    release_handle(h);
}
u8 j_IsSameObject(u32, u32 a, u32 b) { return E->IsSameObject(H(a), H(b)); }
u32 j_NewLocalRef(u32, u32 o) { return new_local(E->NewLocalRef(H(o))); }
i32 j_EnsureLocalCapacity(u32, i32 n) { return E->EnsureLocalCapacity(n); }
u32 j_AllocObject(u32, u32 cls) { return new_local(E->AllocObject(HC(cls))); }
u32 j_GetObjectClass(u32, u32 o) { return new_local(E->GetObjectClass(H(o))); }
u8 j_IsInstanceOf(u32, u32 o, u32 cls) { return E->IsInstanceOf(H(o), HC(cls)); }
u32 j_GetMethodID(u32, u32 cls, gcstr name, gcstr sig) {
    jmethodID id = E->GetMethodID(HC(cls), name, sig);
    if (!id) ZLOGD("GetMethodID(%s %s) failed", name.get(), sig.get());
    return method_handle(id, sig, name);
}
u32 j_GetStaticMethodID(u32, u32 cls, gcstr name, gcstr sig) {
    jmethodID id = E->GetStaticMethodID(HC(cls), name, sig);
    if (!id) ZLOGD("GetStaticMethodID(%s %s) failed", name.get(), sig.get());
    return method_handle(id, sig, name);
}
u32 j_GetFieldID(u32, u32 cls, gcstr name, gcstr sig) { return field_handle(E->GetFieldID(HC(cls), name, sig)); }
u32 j_GetStaticFieldID(u32, u32 cls, gcstr name, gcstr sig) {
    return field_handle(E->GetStaticFieldID(HC(cls), name, sig));
}

// Fields. Guest value types: jboolean u8, jbyte i8, jchar u16, jshort i16,
// jint i32, jlong i64, jfloat float, jdouble double, objects u32 handles.
u32 j_GetObjectField(u32, u32 o, u32 f) { return new_local(E->GetObjectField(H(o), field(f))); }
void j_SetObjectField(u32, u32 o, u32 f, u32 v) { E->SetObjectField(H(o), field(f), H(v)); }
u32 j_GetStaticObjectField(u32, u32 c, u32 f) { return new_local(E->GetStaticObjectField(HC(c), field(f))); }
void j_SetStaticObjectField(u32, u32 c, u32 f, u32 v) { E->SetStaticObjectField(HC(c), field(f), H(v)); }

#define ZB_FIELDS(Name, GT)                                                                               \
    GT j_Get##Name##Field(u32, u32 o, u32 f) { return E->Get##Name##Field(H(o), field(f)); }              \
    void j_Set##Name##Field(u32, u32 o, u32 f, GT v) { E->Set##Name##Field(H(o), field(f), v); }          \
    GT j_GetStatic##Name##Field(u32, u32 c, u32 f) { return E->GetStatic##Name##Field(HC(c), field(f)); } \
    void j_SetStatic##Name##Field(u32, u32 c, u32 f, GT v) { E->SetStatic##Name##Field(HC(c), field(f), v); }
ZB_FIELDS(Boolean, u8)
ZB_FIELDS(Byte, i8)
ZB_FIELDS(Char, u16)
ZB_FIELDS(Short, i16)
ZB_FIELDS(Int, i32)
ZB_FIELDS(Long, i64)
ZB_FIELDS(Float, float)
ZB_FIELDS(Double, double)
#undef ZB_FIELDS

// Strings: the guest receives copies in guest memory.
u32 j_NewString(u32, gaddr chars, i32 len) { return new_local(E->NewString(hp<jchar>(chars), len)); }
i32 j_GetStringLength(u32, u32 s) { return E->GetStringLength(static_cast<jstring>(H(s))); }
gaddr j_GetStringChars(u32, u32 s, gp<u8> is_copy) {
    JNIEnv* e = E;
    auto str = static_cast<jstring>(H(s));
    jsize n = e->GetStringLength(str);
    gaddr buf = mem::gmalloc(static_cast<u32>(n + 1) * 2);
    e->GetStringRegion(str, 0, n, hp<jchar>(buf));
    wr<u16>(buf + static_cast<u32>(n) * 2, 0);
    if (is_copy.a) *is_copy.get() = 1;
    return buf;
}
void j_ReleaseStringChars(u32, u32, gaddr chars) { mem::gfree(chars); }
u32 j_NewStringUTF(u32, gcstr s) { return s.a ? new_local(E->NewStringUTF(s)) : 0; }
i32 j_GetStringUTFLength(u32, u32 s) { return E->GetStringUTFLength(static_cast<jstring>(H(s))); }
gaddr j_GetStringUTFChars(u32, u32 s, gp<u8> is_copy) {
    JNIEnv* e = E;
    auto str = static_cast<jstring>(H(s));
    if (!str) return 0;
    const char* u = e->GetStringUTFChars(str, nullptr);
    if (!u) return 0;
    gaddr g = mem::gstrdup(u);
    e->ReleaseStringUTFChars(str, u);
    if (is_copy.a) *is_copy.get() = 1;
    return g;
}
void j_ReleaseStringUTFChars(u32, u32, gaddr chars) { mem::gfree(chars); }
void j_GetStringRegion(u32, u32 s, i32 start, i32 len, gaddr buf) {
    E->GetStringRegion(static_cast<jstring>(H(s)), start, len, hp<jchar>(buf));
}
void j_GetStringUTFRegion(u32, u32 s, i32 start, i32 len, gaddr buf) {
    E->GetStringUTFRegion(static_cast<jstring>(H(s)), start, len, hp<char>(buf));
}
gaddr j_GetStringCritical(u32 env_, u32 s, gp<u8> is_copy) { return j_GetStringChars(env_, s, is_copy); }
void j_ReleaseStringCritical(u32, u32, gaddr chars) { mem::gfree(chars); }

// Arrays.
// ART aborts the process on GetArrayLength(null); report it and return 0.
i32 j_GetArrayLength(JitCtx& j, u32, u32 a) {
    jobject arr = H(a);
    if (!arr) {
        ZLOGW("guest called GetArrayLength(null)");
        cpu::dump_state(j, "GetArrayLength(null)");
        return 0;
    }
    return E->GetArrayLength(static_cast<jarray>(arr));
}
u32 j_NewObjectArray(u32, i32 n, u32 cls, u32 init) { return new_local(E->NewObjectArray(n, HC(cls), H(init))); }
u32 j_GetObjectArrayElement(u32, u32 a, i32 i) {
    return new_local(E->GetObjectArrayElement(static_cast<jobjectArray>(H(a)), i));
}
void j_SetObjectArrayElement(u32, u32 a, i32 i, u32 v) {
    E->SetObjectArrayElement(static_cast<jobjectArray>(H(a)), i, H(v));
}

#define ZB_ARRAYS(Name, JT, AT)                                                                           \
    u32 j_New##Name##Array(u32, i32 n) { return new_local(E->New##Name##Array(n)); }                      \
    gaddr j_Get##Name##ArrayElements(u32, u32 a, gp<u8> is_copy) {                                        \
        JNIEnv* e = E;                                                                                    \
        auto arr = static_cast<AT>(H(a));                                                                 \
        jsize n = e->GetArrayLength(arr);                                                                 \
        gaddr buf = mem::gmalloc(static_cast<u32>(n > 0 ? n : 1) * sizeof(JT));                           \
        e->Get##Name##ArrayRegion(arr, 0, n, hp<JT>(buf));                                                \
        if (is_copy.a) *is_copy.get() = 1;                                                                \
        return buf;                                                                                       \
    }                                                                                                     \
    void j_Release##Name##ArrayElements(u32, u32 a, gaddr elems, i32 mode) {                              \
        if (mode != JNI_ABORT) {                                                                          \
            JNIEnv* e = E;                                                                                \
            auto arr = static_cast<AT>(H(a));                                                             \
            e->Set##Name##ArrayRegion(arr, 0, e->GetArrayLength(arr), hp<JT>(elems));                     \
        }                                                                                                 \
        if (mode != JNI_COMMIT) mem::gfree(elems);                                                        \
    }                                                                                                     \
    void j_Get##Name##ArrayRegion(u32, u32 a, i32 start, i32 len, gaddr buf) {                            \
        E->Get##Name##ArrayRegion(static_cast<AT>(H(a)), start, len, hp<JT>(buf));                        \
    }                                                                                                     \
    void j_Set##Name##ArrayRegion(u32, u32 a, i32 start, i32 len, gaddr buf) {                            \
        E->Set##Name##ArrayRegion(static_cast<AT>(H(a)), start, len, hp<const JT>(buf));                  \
    }
ZB_ARRAYS(Boolean, jboolean, jbooleanArray)
ZB_ARRAYS(Byte, jbyte, jbyteArray)
ZB_ARRAYS(Char, jchar, jcharArray)
ZB_ARRAYS(Short, jshort, jshortArray)
ZB_ARRAYS(Int, jint, jintArray)
ZB_ARRAYS(Long, jlong, jlongArray)
ZB_ARRAYS(Float, jfloat, jfloatArray)
ZB_ARRAYS(Double, jdouble, jdoubleArray)
#undef ZB_ARRAYS

u32 elem_size(JNIEnv* e, jarray a) {
    static jclass cls[8];
    static const char* names[8] = {"[Z", "[B", "[C", "[S", "[I", "[J", "[F", "[D"};
    static const u32 sizes[8] = {1, 1, 2, 2, 4, 8, 4, 8};
    static std::once_flag once;
    std::call_once(once, [&] {
        for (int i = 0; i < 8; ++i) {
            jclass c = e->FindClass(names[i]);
            cls[i] = static_cast<jclass>(e->NewGlobalRef(c));
            e->DeleteLocalRef(c);
        }
    });
    for (int i = 0; i < 8; ++i) {
        if (e->IsInstanceOf(a, cls[i])) return sizes[i];
    }
    return 0;
}

gaddr j_GetPrimitiveArrayCritical(u32, u32 a, gp<u8> is_copy) {
    JNIEnv* e = E;
    auto arr = static_cast<jarray>(H(a));
    u32 es = elem_size(e, arr);
    jsize n = e->GetArrayLength(arr);
    gaddr buf = mem::gmalloc(static_cast<u32>(n > 0 ? n : 1) * (es ? es : 1));
    void* src = e->GetPrimitiveArrayCritical(arr, nullptr);
    if (src) {
        memcpy(hp(buf), src, static_cast<size_t>(n) * es);
        e->ReleasePrimitiveArrayCritical(arr, src, JNI_ABORT);
    }
    if (is_copy.a) *is_copy.get() = 1;
    return buf;
}

void j_ReleasePrimitiveArrayCritical(u32, u32 a, gaddr buf, i32 mode) {
    if (mode != JNI_ABORT) {
        JNIEnv* e = E;
        auto arr = static_cast<jarray>(H(a));
        u32 es = elem_size(e, arr);
        jsize n = e->GetArrayLength(arr);
        void* dst = e->GetPrimitiveArrayCritical(arr, nullptr);
        if (dst) {
            memcpy(dst, hp(buf), static_cast<size_t>(n) * es);
            e->ReleasePrimitiveArrayCritical(arr, dst, 0);
        }
    }
    if (mode != JNI_COMMIT) mem::gfree(buf);
}

i32 j_RegisterNatives(u32, u32 cls, gaddr methods, i32 n) {
    JNIEnv* e = E;
    jclass c = HC(cls);
    std::string cname = class_name(e, c);
    std::vector<JNINativeMethod> host;
    for (i32 i = 0; i < n; ++i) {
        const char* name = hp<const char>(rd<u32>(methods + 12 * i));
        const char* sig = hp<const char>(rd<u32>(methods + 12 * i + 4));
        gaddr fn = rd<u32>(methods + 12 * i + 8);
        const NativeEntry* ent = nullptr;
        {
            std::lock_guard lk(g_nat_lock);
            std::string key = cname + "." + name + " " + sig;
            if (auto it = g_natives.find(key); it != g_natives.end()) ent = it->second;
            else if (auto it2 = g_natives_by_name.find(std::string(name) + " " + sig); it2 != g_natives_by_name.end())
                ent = it2->second;
        }
        if (!ent) {
            ZLOGE("RegisterNatives: no trampoline for %s.%s %s", cname.c_str(), name, sig);
            continue;
        }
        *ent->slot = fn;
        host.push_back({name, sig, ent->host_fn});
        ZLOGD("RegisterNatives %s.%s %s -> %s", cname.c_str(), name, sig, cpu::symbolize(fn & ~1u).c_str());
    }
    if (host.empty()) return n == 0 ? 0 : JNI_ERR;
    return e->RegisterNatives(c, host.data(), static_cast<jint>(host.size()));
}
i32 j_UnregisterNatives(u32, u32 cls) { return E->UnregisterNatives(HC(cls)); }
i32 j_MonitorEnter(u32, u32 o) { return E->MonitorEnter(H(o)); }
i32 j_MonitorExit(u32, u32 o) { return E->MonitorExit(H(o)); }
i32 j_GetJavaVM(u32, gp<u32> out) {
    *out.get() = g_guest_vm;
    return JNI_OK;
}
u32 j_NewWeakGlobalRef(u32, u32 o) {
    jweak w = E->NewWeakGlobalRef(H(o));
    if (!w) return 0;
    return handle_of(alloc_slot(w, Kind::Weak, nullptr));
}
void j_DeleteWeakGlobalRef(u32, u32 h) {
    if (kind_of(h) != Kind::Weak) return;
    E->DeleteWeakGlobalRef(H(h));
    release_handle(h);
}
u8 j_ExceptionCheck(u32) { return E->ExceptionCheck(); }
u32 j_NewDirectByteBuffer(u32, gaddr addr, i64 cap) { return new_local(E->NewDirectByteBuffer(hp(addr), cap)); }
gaddr j_GetDirectBufferAddress(u32, u32 b) {
    void* p = E->GetDirectBufferAddress(H(b));
    if (reinterpret_cast<uintptr_t>(p) > 0xFFFFFFFFu) {
        ZLOGE("GetDirectBufferAddress: buffer at %p is outside guest memory", p);
        return 0;
    }
    return ga(p);
}
i64 j_GetDirectBufferCapacity(u32, u32 b) { return E->GetDirectBufferCapacity(H(b)); }
i32 j_GetObjectRefType(u32, u32 h) {
    switch (kind_of(h)) {
    case Kind::Local: return JNILocalRefType;
    case Kind::Global: return JNIGlobalRefType;
    case Kind::Weak: return JNIWeakGlobalRefType;
    default: return JNIInvalidRefType;
    }
}

#undef E

// ------------------------------------------------------------ JavaVM (invoke interface)

i32 vm_DestroyJavaVM(u32) { return JNI_ERR; }

i32 vm_attach(gp<u32> penv, gaddr args, bool daemon) {
    GuestThread* t = cpu::current_thread();
    if (!t->host_env) {
        JavaVMAttachArgs a{JNI_VERSION_1_6, nullptr, nullptr};
        if (args) {
            a.version = rd<i32>(args);
            u32 name = rd<u32>(args + 4);
            a.name = name ? hp<char>(name) : nullptr;
            a.group = H(rd<u32>(args + 8));
        }
        JNIEnv* e = nullptr;
        jint r = daemon ? g_vm->AttachCurrentThreadAsDaemon(&e, args ? &a : nullptr)
                        : g_vm->AttachCurrentThread(&e, args ? &a : nullptr);
        if (r != JNI_OK) return r;
        t->jni_attached_by_us = true;
        bind_env(e);
    }
    if (penv.a) *penv.get() = t->guest_env;
    return JNI_OK;
}
i32 vm_AttachCurrentThread(u32, gp<u32> penv, gaddr args) { return vm_attach(penv, args, false); }
i32 vm_AttachCurrentThreadAsDaemon(u32, gp<u32> penv, gaddr args) { return vm_attach(penv, args, true); }

void detach_current() {
    GuestThread* t = cpu::current_thread_if_any();
    if (!t || !t->host_env) return;
    for (auto& f : t_frames) release_frame(f);
    t_frames.clear();
    t->host_env = nullptr;
    if (t->jni_attached_by_us) {
        t->jni_attached_by_us = false;
        g_vm->DetachCurrentThread();
    }
}

// The engine attaches and detaches its threads around each Java call (SparkSystem::JNIEnvWrapper): android_main
// does it every 16 ms, and every attachment creates a new java.lang.Thread ("Thread-N"), with the work and garbage
// that goes with it. The thread stays attached to the VM instead (its local references are released, as a detach
// would); it is really detached when it exits.
i32 vm_DetachCurrentThread(u32) {
    GuestThread* t = cpu::current_thread_if_any();
    if (t && t->host_env) {
        for (auto& f : t_frames) release_frame(f);
        t_frames.clear();
    }
    return JNI_OK;
}

i32 vm_GetEnv(u32, gp<u32> penv, i32 version) {
    GuestThread* t = cpu::current_thread();
    if (!t->host_env) {
        JNIEnv* e = nullptr;
        if (g_vm->GetEnv(reinterpret_cast<void**>(&e), version) != JNI_OK) {
            *penv.get() = 0;
            return JNI_EDETACHED;
        }
        bind_env(e);
    }
    *penv.get() = t->guest_env;
    return JNI_OK;
}

void on_thread_exit(GuestThread* t) {
    if (t->host_env) {
        ZLOGD("guest thread %u exiting while attached: detaching", t->id);
        detach_current();
    }
}

// A host thread (not created by the guest) that the guest attached, ending: detach it, or ART aborts the process.
void on_thread_destroy(GuestThread* t) {
    if (t->host_env && t->jni_attached_by_us) {
        t->host_env = nullptr;
        t->jni_attached_by_us = false;
        g_vm->DetachCurrentThread();
    }
}

void build_tables() {
    std::array<gaddr, 233> f{};
    auto T = [](ThunkFn fn, const char* name) { return cpu::make_thunk(fn, name); };
#define J(idx, name) f[idx] = T(&adapt<j_##name>, "JNI_" #name)
#define JR(idx, name, ...) f[idx] = T(__VA_ARGS__, "JNI_" #name)
    J(4, GetVersion);
    J(5, DefineClass);
    J(6, FindClass);
    J(7, FromReflectedMethod);
    J(8, FromReflectedField);
    J(9, ToReflectedMethod);
    J(10, GetSuperclass);
    J(11, IsAssignableFrom);
    J(12, ToReflectedField);
    J(13, Throw);
    J(14, ThrowNew);
    J(15, ExceptionOccurred);
    J(16, ExceptionDescribe);
    J(17, ExceptionClear);
    J(18, FatalError);
    J(19, PushLocalFrame);
    J(20, PopLocalFrame);
    J(21, NewGlobalRef);
    J(22, DeleteGlobalRef);
    J(23, DeleteLocalRef);
    J(24, IsSameObject);
    J(25, NewLocalRef);
    J(26, EnsureLocalCapacity);
    J(27, AllocObject);
    JR(28, NewObject, &new_object_thunk<kDots>);
    JR(29, NewObjectV, &new_object_thunk<kVaList>);
    JR(30, NewObjectA, &new_object_thunk<kArray>);
    J(31, GetObjectClass);
    J(32, IsInstanceOf);
    J(33, GetMethodID);
#define ZB_CALLS(base, Kind, Name)                                                         \
    JR(base + 0, Call##Name##Object, &call_thunk<'L', Kind, kDots>);                       \
    JR(base + 1, Call##Name##ObjectV, &call_thunk<'L', Kind, kVaList>);                    \
    JR(base + 2, Call##Name##ObjectA, &call_thunk<'L', Kind, kArray>);                     \
    JR(base + 3, Call##Name##Boolean, &call_thunk<'Z', Kind, kDots>);                      \
    JR(base + 4, Call##Name##BooleanV, &call_thunk<'Z', Kind, kVaList>);                   \
    JR(base + 5, Call##Name##BooleanA, &call_thunk<'Z', Kind, kArray>);                    \
    JR(base + 6, Call##Name##Byte, &call_thunk<'B', Kind, kDots>);                         \
    JR(base + 7, Call##Name##ByteV, &call_thunk<'B', Kind, kVaList>);                      \
    JR(base + 8, Call##Name##ByteA, &call_thunk<'B', Kind, kArray>);                       \
    JR(base + 9, Call##Name##Char, &call_thunk<'C', Kind, kDots>);                         \
    JR(base + 10, Call##Name##CharV, &call_thunk<'C', Kind, kVaList>);                     \
    JR(base + 11, Call##Name##CharA, &call_thunk<'C', Kind, kArray>);                      \
    JR(base + 12, Call##Name##Short, &call_thunk<'S', Kind, kDots>);                       \
    JR(base + 13, Call##Name##ShortV, &call_thunk<'S', Kind, kVaList>);                    \
    JR(base + 14, Call##Name##ShortA, &call_thunk<'S', Kind, kArray>);                     \
    JR(base + 15, Call##Name##Int, &call_thunk<'I', Kind, kDots>);                         \
    JR(base + 16, Call##Name##IntV, &call_thunk<'I', Kind, kVaList>);                      \
    JR(base + 17, Call##Name##IntA, &call_thunk<'I', Kind, kArray>);                       \
    JR(base + 18, Call##Name##Long, &call_thunk<'J', Kind, kDots>);                        \
    JR(base + 19, Call##Name##LongV, &call_thunk<'J', Kind, kVaList>);                     \
    JR(base + 20, Call##Name##LongA, &call_thunk<'J', Kind, kArray>);                      \
    JR(base + 21, Call##Name##Float, &call_thunk<'F', Kind, kDots>);                       \
    JR(base + 22, Call##Name##FloatV, &call_thunk<'F', Kind, kVaList>);                    \
    JR(base + 23, Call##Name##FloatA, &call_thunk<'F', Kind, kArray>);                     \
    JR(base + 24, Call##Name##Double, &call_thunk<'D', Kind, kDots>);                      \
    JR(base + 25, Call##Name##DoubleV, &call_thunk<'D', Kind, kVaList>);                   \
    JR(base + 26, Call##Name##DoubleA, &call_thunk<'D', Kind, kArray>);                    \
    JR(base + 27, Call##Name##Void, &call_thunk<'V', Kind, kDots>);                        \
    JR(base + 28, Call##Name##VoidV, &call_thunk<'V', Kind, kVaList>);                     \
    JR(base + 29, Call##Name##VoidA, &call_thunk<'V', Kind, kArray>)
    ZB_CALLS(34, kVirtual, );
    ZB_CALLS(64, kNonvirtual, Nonvirtual);
    J(94, GetFieldID);
    J(95, GetObjectField);
    J(96, GetBooleanField);
    J(97, GetByteField);
    J(98, GetCharField);
    J(99, GetShortField);
    J(100, GetIntField);
    J(101, GetLongField);
    J(102, GetFloatField);
    J(103, GetDoubleField);
    J(104, SetObjectField);
    J(105, SetBooleanField);
    J(106, SetByteField);
    J(107, SetCharField);
    J(108, SetShortField);
    J(109, SetIntField);
    J(110, SetLongField);
    J(111, SetFloatField);
    J(112, SetDoubleField);
    J(113, GetStaticMethodID);
    ZB_CALLS(114, kStatic, Static);
#undef ZB_CALLS
    J(144, GetStaticFieldID);
    J(145, GetStaticObjectField);
    J(146, GetStaticBooleanField);
    J(147, GetStaticByteField);
    J(148, GetStaticCharField);
    J(149, GetStaticShortField);
    J(150, GetStaticIntField);
    J(151, GetStaticLongField);
    J(152, GetStaticFloatField);
    J(153, GetStaticDoubleField);
    J(154, SetStaticObjectField);
    J(155, SetStaticBooleanField);
    J(156, SetStaticByteField);
    J(157, SetStaticCharField);
    J(158, SetStaticShortField);
    J(159, SetStaticIntField);
    J(160, SetStaticLongField);
    J(161, SetStaticFloatField);
    J(162, SetStaticDoubleField);
    J(163, NewString);
    J(164, GetStringLength);
    J(165, GetStringChars);
    J(166, ReleaseStringChars);
    J(167, NewStringUTF);
    J(168, GetStringUTFLength);
    J(169, GetStringUTFChars);
    J(170, ReleaseStringUTFChars);
    J(171, GetArrayLength);
    J(172, NewObjectArray);
    J(173, GetObjectArrayElement);
    J(174, SetObjectArrayElement);
    J(175, NewBooleanArray);
    J(176, NewByteArray);
    J(177, NewCharArray);
    J(178, NewShortArray);
    J(179, NewIntArray);
    J(180, NewLongArray);
    J(181, NewFloatArray);
    J(182, NewDoubleArray);
    J(183, GetBooleanArrayElements);
    J(184, GetByteArrayElements);
    J(185, GetCharArrayElements);
    J(186, GetShortArrayElements);
    J(187, GetIntArrayElements);
    J(188, GetLongArrayElements);
    J(189, GetFloatArrayElements);
    J(190, GetDoubleArrayElements);
    J(191, ReleaseBooleanArrayElements);
    J(192, ReleaseByteArrayElements);
    J(193, ReleaseCharArrayElements);
    J(194, ReleaseShortArrayElements);
    J(195, ReleaseIntArrayElements);
    J(196, ReleaseLongArrayElements);
    J(197, ReleaseFloatArrayElements);
    J(198, ReleaseDoubleArrayElements);
    J(199, GetBooleanArrayRegion);
    J(200, GetByteArrayRegion);
    J(201, GetCharArrayRegion);
    J(202, GetShortArrayRegion);
    J(203, GetIntArrayRegion);
    J(204, GetLongArrayRegion);
    J(205, GetFloatArrayRegion);
    J(206, GetDoubleArrayRegion);
    J(207, SetBooleanArrayRegion);
    J(208, SetByteArrayRegion);
    J(209, SetCharArrayRegion);
    J(210, SetShortArrayRegion);
    J(211, SetIntArrayRegion);
    J(212, SetLongArrayRegion);
    J(213, SetFloatArrayRegion);
    J(214, SetDoubleArrayRegion);
    J(215, RegisterNatives);
    J(216, UnregisterNatives);
    J(217, MonitorEnter);
    J(218, MonitorExit);
    J(219, GetJavaVM);
    J(220, GetStringRegion);
    J(221, GetStringUTFRegion);
    J(222, GetPrimitiveArrayCritical);
    J(223, ReleasePrimitiveArrayCritical);
    J(224, GetStringCritical);
    J(225, ReleaseStringCritical);
    J(226, NewWeakGlobalRef);
    J(227, DeleteWeakGlobalRef);
    J(228, ExceptionCheck);
    J(229, NewDirectByteBuffer);
    J(230, GetDirectBufferAddress);
    J(231, GetDirectBufferCapacity);
    J(232, GetObjectRefType);
#undef J
#undef JR
    for (size_t i = 4; i < f.size(); ++i) {
        if (!f[i]) fatal("JNI table entry %zu missing", i);
    }
    g_env_table = mem::gmalloc(static_cast<u32>(f.size() * 4));
    for (size_t i = 0; i < f.size(); ++i) wr<u32>(g_env_table + 4 * static_cast<u32>(i), f[i]);

    gaddr inv = mem::gcalloc(8, 4);
    wr<u32>(inv + 12, cpu::make_thunk(&adapt<vm_DestroyJavaVM>, "JavaVM_DestroyJavaVM"));
    wr<u32>(inv + 16, cpu::make_thunk(&adapt<vm_AttachCurrentThread>, "JavaVM_AttachCurrentThread"));
    wr<u32>(inv + 20, cpu::make_thunk(&adapt<vm_DetachCurrentThread>, "JavaVM_DetachCurrentThread"));
    wr<u32>(inv + 24, cpu::make_thunk(&adapt<vm_GetEnv>, "JavaVM_GetEnv"));
    wr<u32>(inv + 28, cpu::make_thunk(&adapt<vm_AttachCurrentThreadAsDaemon>, "JavaVM_AttachCurrentThreadAsDaemon"));
    g_guest_vm = mem::gmalloc(4);
    wr<u32>(g_guest_vm, inv);
}

}  // namespace

void init(JavaVM* vm) {
    static std::once_flag once;
    std::call_once(once, [vm] {
        g_vm = vm;
        build_tables();
        cpu::add_thread_exit_hook(&on_thread_exit);
        cpu::add_thread_destroy_hook(&on_thread_destroy);
    });
}

JavaVM* host_vm() { return g_vm; }
gaddr guest_vm() { return g_guest_vm; }

gaddr bind_env(JNIEnv* e) {
    GuestThread* t = cpu::current_thread();
    t->host_env = e;
    if (!t->guest_env) {
        t->guest_env = t->tls + tls::kJniEnv;
        wr<u32>(t->guest_env, g_env_table);
    }
    return t->guest_env;
}

JNIEnv* env() {
    GuestThread* t = cpu::current_thread();
    if (t->host_env) return static_cast<JNIEnv*>(t->host_env);
    JNIEnv* e = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK) {
        ZLOGW("guest thread %u used JNI without attaching; attaching it", t->id);
        if (g_vm->AttachCurrentThread(&e, nullptr) != JNI_OK) fatal("AttachCurrentThread failed");
        t->jni_attached_by_us = true;
    }
    bind_env(e);
    return e;
}

u32 new_local(jobject o) {
    if (!o) return 0;
    u32 gen;
    u32 idx = alloc_slot(o, Kind::Local, &gen);
    top_frame().emplace_back(idx, gen);
    return handle_of(idx);
}

u32 new_global_handle(jobject o) {
    if (!o) return 0;
    jobject g = env()->NewGlobalRef(o);
    return handle_of(alloc_slot(g, Kind::Global, nullptr));
}

jobject to_host(u32 h) {
    if (!h) return nullptr;
    u32 idx;
    if (!idx_of(h, &idx)) fatal("invalid guest JNI reference 0x%08x", h);
    std::lock_guard lk(g_ref_lock);
    if (idx >= g_slots.size() || g_slots[idx].kind == Kind::Free) {
        ZLOGE("stale guest JNI reference 0x%08x", h);
        return nullptr;
    }
    return g_slots[idx].obj;
}

NativeFrame::NativeFrame(JNIEnv* e) {
    genv_ = bind_env(e);
    if (t_frames.empty()) t_frames.emplace_back();
    t_frames.emplace_back();
}

NativeFrame::~NativeFrame() {
    release_frame(t_frames.back());
    t_frames.pop_back();
}

void set_native_table(const NativeEntry* entries, size_t n) {
    std::lock_guard lk(g_nat_lock);
    std::unordered_map<std::string, int> name_count;
    for (size_t i = 0; i < n; ++i) name_count[std::string(entries[i].name) + " " + entries[i].sig]++;
    for (size_t i = 0; i < n; ++i) {
        const NativeEntry& e = entries[i];
        g_natives[std::string(e.cls) + "." + e.name + " " + e.sig] = &e;
        std::string key = std::string(e.name) + " " + e.sig;
        if (name_count[key] == 1) g_natives_by_name[key] = &e;
    }
}

}  // namespace zb::jni
