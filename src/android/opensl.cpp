// OpenSL ES for the guest.
//
// Guest interface pointers are proxies in guest memory: [0] guest vtable,
// [8] host interface pointer. Interface IDs are passed through unchanged:
// libwilhelm compares GUID contents, and guest memory is host-addressable.
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../hle/hle.h"
#include "../mem.h"
#include "android_glue.h"

namespace zb::android {
namespace {

using hle::gcstr;

enum Type { kObject, kEngine, kPlay, kVolume, kBufferQueue, kSimpleBufferQueue, kTypeCount, kUnknown };

struct Proxy {
    const void* host;
    Type type;
    gaddr owner;
};

std::mutex g_lock;
std::unordered_map<const void*, gaddr> g_by_host;
std::unordered_map<gaddr, Proxy> g_proxies;
std::array<gaddr, kTypeCount> g_vtables{};

struct KnownIid {
    const char* name;
    const SLInterfaceID* host;
    Type type;
};
const KnownIid kIids[] = {
    {"SL_IID_ENGINE", &SL_IID_ENGINE, kEngine},
    {"SL_IID_PLAY", &SL_IID_PLAY, kPlay},
    {"SL_IID_VOLUME", &SL_IID_VOLUME, kVolume},
    {"SL_IID_BUFFERQUEUE", &SL_IID_BUFFERQUEUE, kBufferQueue},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", &SL_IID_ANDROIDSIMPLEBUFFERQUEUE, kSimpleBufferQueue},
};

Type type_of_iid(gaddr iid) {
    for (const KnownIid& k : kIids) {
        if (memcmp(hp(iid), *k.host, sizeof(SLInterfaceID_)) == 0) return k.type;
    }
    return kUnknown;
}

gaddr proxy_for(const void* host, Type type, gaddr owner) {
    if (!host) return 0;
    std::lock_guard lk(g_lock);
    if (auto it = g_by_host.find(host); it != g_by_host.end()) return it->second;
    gaddr p = mem::gcalloc(1, 16);
    wr<u32>(p, g_vtables[type]);
    wr<u64>(p + 8, reinterpret_cast<u64>(host));
    g_by_host[host] = p;
    g_proxies[p] = Proxy{host, type, owner};
    return p;
}

template <typename T>
T host_of(gaddr p) {
    return reinterpret_cast<T>(rd<u64>(p + 8));
}

std::vector<SLInterfaceID> iids(u32 n, gaddr arr) {
    std::vector<SLInterfaceID> v(n);
    for (u32 i = 0; i < n; ++i) v[i] = hp<const SLInterfaceID_>(rd<u32>(arr + 4 * i));
    return v;
}

// Converted SLDataSource/SLDataSink with stable storage for the pieces.
struct DataConv {
    SLDataLocator_OutputMix mix{};
    SLDataLocator_URI uri{};
    SLDataLocator_Address addr{};
    SLDataFormat_MIME mime{};
    SLDataSource src{};

    bool convert(gaddr g) {
        if (!g) return false;
        gaddr loc = rd<u32>(g), fmt = rd<u32>(g + 4);
        src.pLocator = convert_locator(loc);
        src.pFormat = convert_format(fmt);
        return true;
    }

    void* convert_locator(gaddr loc) {
        if (!loc) return nullptr;
        u32 type = rd<u32>(loc);
        switch (type) {
        case SL_DATALOCATOR_OUTPUTMIX:
            mix.locatorType = type;
            mix.outputMix = host_of<SLObjectItf>(rd<u32>(loc + 4));
            return &mix;
        case SL_DATALOCATOR_URI:
            uri.locatorType = type;
            uri.URI = hp<SLchar>(rd<u32>(loc + 4));
            return &uri;
        case SL_DATALOCATOR_ADDRESS:
            addr.locatorType = type;
            addr.pAddress = hp(rd<u32>(loc + 4));
            addr.length = rd<u32>(loc + 8);
            return &addr;
        default:  // buffer queues and Android FD locators have no pointers
            return hp(loc);
        }
    }

    void* convert_format(gaddr fmt) {
        if (!fmt) return nullptr;
        u32 type = rd<u32>(fmt);
        if (type == SL_DATAFORMAT_MIME) {
            mime.formatType = type;
            mime.mimeType = hp<SLchar>(rd<u32>(fmt + 4));
            mime.containerType = rd<u32>(fmt + 8);
            return &mime;
        }
        return hp(fmt);  // PCM formats are plain integers
    }
};

// Destroyed objects keep their guest proxies (the game may still hold the
// pointers); calls through them fail instead of touching freed host memory.
void kill_tree(gaddr object) {
    std::lock_guard lk(g_lock);
    for (auto& [p, proxy] : g_proxies) {
        if ((p == object || proxy.owner == object) && proxy.host) {
            g_by_host.erase(proxy.host);
            proxy.host = nullptr;
            wr<u64>(p + 8, 0);
        }
    }
}

u32 dead(gaddr proxy) {
    ZLOGW("OpenSL call through destroyed object 0x%08x", proxy);
    return SL_RESULT_PRECONDITIONS_VIOLATED;
}

#define HOST(T, v)                    auto v = host_of<T>(s);           if (!v) return dead(s)

// ------------------------------------------------------------ callbacks

struct CbRec {
    gaddr fn, ctx, proxy;
};

void object_cb(SLObjectItf, const void* ctx, SLuint32 event, SLresult result, SLuint32 param, void*) {
    auto* r = static_cast<const CbRec*>(ctx);
    cpu::call(r->fn, cpu::Args().u32v(r->proxy).u32v(r->ctx).u32v(event).u32v(result).u32v(param).u32v(0));
}
void play_cb(SLPlayItf, void* ctx, SLuint32 event) {
    auto* r = static_cast<CbRec*>(ctx);
    cpu::call(r->fn, cpu::Args().u32v(r->proxy).u32v(r->ctx).u32v(event));
}
void bq_cb(SLBufferQueueItf, void* ctx) {
    auto* r = static_cast<CbRec*>(ctx);
    cpu::call(r->fn, cpu::Args().u32v(r->proxy).u32v(r->ctx));
}
void sbq_cb(SLAndroidSimpleBufferQueueItf, void* ctx) {
    auto* r = static_cast<CbRec*>(ctx);
    cpu::call(r->fn, cpu::Args().u32v(r->proxy).u32v(r->ctx));
}

// ------------------------------------------------------------ SLObjectItf

using Obj = SLObjectItf;
u32 o_Realize(gaddr s, u32 async) {
    HOST(Obj, o);
    return (*o)->Realize(o, async);
}
u32 o_Resume(gaddr s, u32 async) {
    HOST(Obj, o);
    return (*o)->Resume(o, async);
}
u32 o_GetState(gaddr s, gp<u32> st) {
    HOST(Obj, o);
    return (*o)->GetState(o, st);
}
u32 o_GetInterface(gaddr s, gaddr iid, gp<u32> out) {
    HOST(Obj, o);
    Type t = type_of_iid(iid);
    if (t == kUnknown) {
        ZLOGW("OpenSL GetInterface: unsupported interface id");
        return SL_RESULT_FEATURE_UNSUPPORTED;
    }
    void* itf = nullptr;
    SLresult r = (*o)->GetInterface(o, hp<const SLInterfaceID_>(iid), &itf);
    if (r == SL_RESULT_SUCCESS) *out.get() = proxy_for(itf, t, s);
    ZLOGD("OpenSL GetInterface(0x%08x, type %d) -> 0x%08x (%u)", s, t, r == SL_RESULT_SUCCESS ? *out.get() : 0, r);
    return r;
}
u32 o_RegisterCallback(gaddr s, gaddr fn, gaddr ctx) {
    HOST(Obj, o);
    if (!fn) return (*o)->RegisterCallback(o, nullptr, nullptr);
    return (*o)->RegisterCallback(o, object_cb, new CbRec{fn, ctx, s});
}
void o_AbortAsyncOperation(gaddr s) {
    auto o = host_of<Obj>(s);
    if (o) (*o)->AbortAsyncOperation(o);
}
void o_Destroy(gaddr s) {
    auto o = host_of<Obj>(s);
    ZLOGD("OpenSL Destroy(proxy 0x%08x, host %p)", s, static_cast<const void*>(o));
    if (!o) {
        dead(s);
        return;
    }
    (*o)->Destroy(o);
    kill_tree(s);
}
u32 o_SetPriority(gaddr s, i32 prio, u32 preemptable) {
    HOST(Obj, o);
    return (*o)->SetPriority(o, prio, preemptable);
}
u32 o_GetPriority(gaddr s, gp<i32> prio, gp<u32> preemptable) {
    HOST(Obj, o);
    return (*o)->GetPriority(o, prio, preemptable);
}
u32 o_SetLossOfControlInterfaces(gaddr s, i16 n, gaddr ids, u32 enabled) {
    HOST(Obj, o);
    auto v = iids(static_cast<u32>(n), ids);
    return (*o)->SetLossOfControlInterfaces(o, n, v.data(), enabled);
}

// ------------------------------------------------------------ SLEngineItf

using Eng = SLEngineItf;
u32 e_CreateAudioPlayer(gaddr s, gp<u32> out, gaddr src, gaddr snk, u32 n, gaddr ids, gaddr req) {
    HOST(Eng, e);
    DataConv a, b;
    a.convert(src);
    b.convert(snk);
    SLDataSink sink{b.src.pLocator, b.src.pFormat};
    auto v = iids(n, ids);
    SLObjectItf obj = nullptr;
    SLresult r = (*e)->CreateAudioPlayer(e, &obj, &a.src, &sink, n, v.data(), hp<const SLboolean>(req));
    if (r == SL_RESULT_SUCCESS) *out.get() = proxy_for(obj, kObject, 0);
    ZLOGD("OpenSL CreateAudioPlayer -> 0x%08x (%u)", r == SL_RESULT_SUCCESS ? *out.get() : 0, r);
    return r;
}
u32 e_CreateAudioRecorder(gaddr s, gp<u32> out, gaddr src, gaddr snk, u32 n, gaddr ids, gaddr req) {
    HOST(Eng, e);
    DataConv a, b;
    a.convert(src);
    b.convert(snk);
    SLDataSink sink{b.src.pLocator, b.src.pFormat};
    auto v = iids(n, ids);
    SLObjectItf obj = nullptr;
    SLresult r = (*e)->CreateAudioRecorder(e, &obj, &a.src, &sink, n, v.data(), hp<const SLboolean>(req));
    if (r == SL_RESULT_SUCCESS) *out.get() = proxy_for(obj, kObject, 0);
    return r;
}
u32 e_CreateOutputMix(gaddr s, gp<u32> out, u32 n, gaddr ids, gaddr req) {
    HOST(Eng, e);
    auto v = iids(n, ids);
    SLObjectItf obj = nullptr;
    SLresult r = (*e)->CreateOutputMix(e, &obj, n, v.data(), hp<const SLboolean>(req));
    if (r == SL_RESULT_SUCCESS) *out.get() = proxy_for(obj, kObject, 0);
    return r;
}
u32 e_Unsupported(JitCtx& j) {
    ZLOGW("unsupported OpenSL engine call (%s)", cpu::thunk_name_for_addr(j.cur_thunk));
    return SL_RESULT_FEATURE_UNSUPPORTED;
}
u32 e_QueryNumSupportedInterfaces(gaddr s, u32 id, gp<u32> num) {
    HOST(Eng, e);
    return (*e)->QueryNumSupportedInterfaces(e, id, num);
}
u32 e_QuerySupportedInterfaces(gaddr s, u32 id, u32 index, gp<u32> out) {
    HOST(Eng, e);
    SLInterfaceID iid = nullptr;
    SLresult r = (*e)->QuerySupportedInterfaces(e, id, index, &iid);
    if (r == SL_RESULT_SUCCESS) {
        static std::mutex m;
        static std::unordered_map<const void*, gaddr> copies;
        std::lock_guard lk(m);
        auto it = copies.find(iid);
        if (it == copies.end()) it = copies.emplace(iid, mem::gdup(iid, sizeof(SLInterfaceID_))).first;
        *out.get() = it->second;
    }
    return r;
}
u32 e_QueryNumSupportedExtensions(gaddr s, gp<u32> num) {
    HOST(Eng, e);
    return (*e)->QueryNumSupportedExtensions(e, num);
}
u32 e_QuerySupportedExtension(gaddr s, u32 index, gaddr name, gp<i16> len) {
    HOST(Eng, e);
    return (*e)->QuerySupportedExtension(e, index, hp<SLchar>(name), len);
}
u32 e_IsExtensionSupported(gaddr s, gaddr name, gp<u32> out) {
    HOST(Eng, e);
    return (*e)->IsExtensionSupported(e, hp<const SLchar>(name), out);
}

// ------------------------------------------------------------ SLPlayItf

using Play = SLPlayItf;
#define P HOST(Play, p)
u32 p_SetPlayState(gaddr s, u32 st) {
    P;
    return (*p)->SetPlayState(p, st);
}
u32 p_GetPlayState(gaddr s, gp<u32> st) {
    P;
    return (*p)->GetPlayState(p, st);
}
u32 p_GetDuration(gaddr s, gp<u32> ms) {
    P;
    return (*p)->GetDuration(p, ms);
}
u32 p_GetPosition(gaddr s, gp<u32> ms) {
    P;
    return (*p)->GetPosition(p, ms);
}
u32 p_RegisterCallback(gaddr s, gaddr fn, gaddr ctx) {
    P;
    if (!fn) return (*p)->RegisterCallback(p, nullptr, nullptr);
    return (*p)->RegisterCallback(p, play_cb, new CbRec{fn, ctx, s});
}
u32 p_SetCallbackEventsMask(gaddr s, u32 m) {
    P;
    return (*p)->SetCallbackEventsMask(p, m);
}
u32 p_GetCallbackEventsMask(gaddr s, gp<u32> m) {
    P;
    return (*p)->GetCallbackEventsMask(p, m);
}
u32 p_SetMarkerPosition(gaddr s, u32 ms) {
    P;
    return (*p)->SetMarkerPosition(p, ms);
}
u32 p_ClearMarkerPosition(gaddr s) {
    P;
    return (*p)->ClearMarkerPosition(p);
}
u32 p_GetMarkerPosition(gaddr s, gp<u32> ms) {
    P;
    return (*p)->GetMarkerPosition(p, ms);
}
u32 p_SetPositionUpdatePeriod(gaddr s, u32 ms) {
    P;
    return (*p)->SetPositionUpdatePeriod(p, ms);
}
u32 p_GetPositionUpdatePeriod(gaddr s, gp<u32> ms) {
    P;
    return (*p)->GetPositionUpdatePeriod(p, ms);
}
#undef P

// ------------------------------------------------------------ SLVolumeItf

using Vol = SLVolumeItf;
#define VO HOST(Vol, v)
u32 v_SetVolumeLevel(gaddr s, i16 l) {
    VO;
    return (*v)->SetVolumeLevel(v, l);
}
u32 v_GetVolumeLevel(gaddr s, gp<i16> l) {
    VO;
    return (*v)->GetVolumeLevel(v, l);
}
u32 v_GetMaxVolumeLevel(gaddr s, gp<i16> l) {
    VO;
    return (*v)->GetMaxVolumeLevel(v, l);
}
u32 v_SetMute(gaddr s, u32 m) {
    VO;
    return (*v)->SetMute(v, m);
}
u32 v_GetMute(gaddr s, gp<u32> m) {
    VO;
    return (*v)->GetMute(v, m);
}
u32 v_EnableStereoPosition(gaddr s, u32 e) {
    VO;
    return (*v)->EnableStereoPosition(v, e);
}
u32 v_IsEnabledStereoPosition(gaddr s, gp<u32> e) {
    VO;
    return (*v)->IsEnabledStereoPosition(v, e);
}
u32 v_SetStereoPosition(gaddr s, i16 pos) {
    VO;
    return (*v)->SetStereoPosition(v, pos);
}
u32 v_GetStereoPosition(gaddr s, gp<i16> pos) {
    VO;
    return (*v)->GetStereoPosition(v, pos);
}
#undef VO

// ------------------------------------------------------------ buffer queues

u32 bq_Enqueue(gaddr s, gaddr buf, u32 size) {
    HOST(SLBufferQueueItf, q);
    return (*q)->Enqueue(q, hp<const void>(buf), size);
}
u32 bq_Clear(gaddr s) {
    HOST(SLBufferQueueItf, q);
    return (*q)->Clear(q);
}
u32 bq_GetState(gaddr s, gaddr st) {
    HOST(SLBufferQueueItf, q);
    return (*q)->GetState(q, hp<SLBufferQueueState>(st));
}
u32 bq_RegisterCallback(gaddr s, gaddr fn, gaddr ctx) {
    HOST(SLBufferQueueItf, q);
    if (!fn) return (*q)->RegisterCallback(q, nullptr, nullptr);
    return (*q)->RegisterCallback(q, bq_cb, new CbRec{fn, ctx, s});
}
u32 sbq_Enqueue(gaddr s, gaddr buf, u32 size) {
    HOST(SLAndroidSimpleBufferQueueItf, q);
    return (*q)->Enqueue(q, hp<const void>(buf), size);
}
u32 sbq_Clear(gaddr s) {
    HOST(SLAndroidSimpleBufferQueueItf, q);
    return (*q)->Clear(q);
}
u32 sbq_GetState(gaddr s, gaddr st) {
    HOST(SLAndroidSimpleBufferQueueItf, q);
    return (*q)->GetState(q, hp<SLAndroidSimpleBufferQueueState>(st));
}
u32 sbq_RegisterCallback(gaddr s, gaddr fn, gaddr ctx) {
    HOST(SLAndroidSimpleBufferQueueItf, q);
    if (!fn) return (*q)->RegisterCallback(q, nullptr, nullptr);
    return (*q)->RegisterCallback(q, sbq_cb, new CbRec{fn, ctx, s});
}

// ------------------------------------------------------------ slCreateEngine

u32 sl_CreateEngine(gp<u32> out, u32 nopts, gaddr opts, u32 n, gaddr ids, gaddr req) {
    auto v = iids(n, ids);
    SLObjectItf obj = nullptr;
    SLresult r = slCreateEngine(&obj, nopts, hp<const SLEngineOption>(opts), n, v.data(), hp<const SLboolean>(req));
    ZLOGI("slCreateEngine -> %u", r);
    if (r == SL_RESULT_SUCCESS) *out.get() = proxy_for(obj, kObject, 0);
    return r;
}

gaddr vtable(std::initializer_list<std::pair<ThunkFn, const char*>> fns) {
    gaddr t = mem::gcalloc(static_cast<u32>(fns.size()), 4);
    u32 i = 0;
    for (auto [fn, name] : fns) wr<u32>(t + 4 * i++, cpu::make_thunk(fn, name));
    return t;
}

#define M(f) std::pair<ThunkFn, const char*>{&adapt<f>, "SL_" #f}

}  // namespace

void register_opensl() {
    g_vtables[kObject] = vtable({M(o_Realize), M(o_Resume), M(o_GetState), M(o_GetInterface), M(o_RegisterCallback),
                                 M(o_AbortAsyncOperation), M(o_Destroy), M(o_SetPriority), M(o_GetPriority),
                                 M(o_SetLossOfControlInterfaces)});
    g_vtables[kEngine] = vtable({M(e_Unsupported), M(e_Unsupported), M(e_CreateAudioPlayer), M(e_CreateAudioRecorder),
                                 M(e_Unsupported), M(e_Unsupported), M(e_Unsupported), M(e_CreateOutputMix),
                                 M(e_Unsupported), M(e_Unsupported), M(e_QueryNumSupportedInterfaces),
                                 M(e_QuerySupportedInterfaces), M(e_QueryNumSupportedExtensions),
                                 M(e_QuerySupportedExtension), M(e_IsExtensionSupported)});
    g_vtables[kPlay] = vtable({M(p_SetPlayState), M(p_GetPlayState), M(p_GetDuration), M(p_GetPosition),
                               M(p_RegisterCallback), M(p_SetCallbackEventsMask), M(p_GetCallbackEventsMask),
                               M(p_SetMarkerPosition), M(p_ClearMarkerPosition), M(p_GetMarkerPosition),
                               M(p_SetPositionUpdatePeriod), M(p_GetPositionUpdatePeriod)});
    g_vtables[kVolume] = vtable({M(v_SetVolumeLevel), M(v_GetVolumeLevel), M(v_GetMaxVolumeLevel), M(v_SetMute),
                                 M(v_GetMute), M(v_EnableStereoPosition), M(v_IsEnabledStereoPosition),
                                 M(v_SetStereoPosition), M(v_GetStereoPosition)});
    g_vtables[kBufferQueue] = vtable({M(bq_Enqueue), M(bq_Clear), M(bq_GetState), M(bq_RegisterCallback)});
    g_vtables[kSimpleBufferQueue] = vtable({M(sbq_Enqueue), M(sbq_Clear), M(sbq_GetState), M(sbq_RegisterCallback)});

    for (const KnownIid& k : kIids) {
        gaddr guid = mem::gdup(*k.host, sizeof(SLInterfaceID_));
        gaddr var = mem::gmalloc(4);
        wr<u32>(var, guid);
        cpu::register_data(k.name, var);
    }
    REG(slCreateEngine, sl_CreateEngine);
}

}  // namespace zb::android
