#include "android_glue.h"

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/sensor.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../cheat/cheat.h"
#include "../elf_loader.h"
#include "../game/perf_patches.h"
#include "../hle/hle.h"
#include "../jni/jni_bridge.h"
#include "../mem.h"
#include "../runtime.h"
#include "cheat_ui.h"
#include "handles.h"

namespace zb::jni {
void register_generated_natives();
}

namespace zb::android {
namespace {

using hle::gcstr;
using hle::gsize;

PtrMap<AAssetManager> g_asset_mgrs(0x11);
PtrMap<AAsset> g_assets(0x12);
PtrMap<AConfiguration> g_configs(0x13);
PtrMap<AInputQueue> g_queues(0x14);
PtrMap<AInputEvent> g_events(0x15);
PtrMap<ALooper> g_loopers(0x16);
PtrMap<ANativeWindow> g_windows(0x17);
PtrMap<ASensorManager> g_sensor_mgrs(0x18);
PtrMap<const ASensor> g_sensors(0x19);
PtrMap<ASensorEventQueue> g_sensor_queues(0x1A);

struct GuestCallback {
    gaddr fn;
    gaddr data;
};

int looper_trampoline(int fd, int events, void* data) {
    auto* cb = static_cast<GuestCallback*>(data);
    return static_cast<int>(cpu::call(cb->fn, cpu::Args().i32v(fd).i32v(events).u32v(cb->data)));
}

// Host-side `data` for looper registrations: the guest pointer itself when there
// is no callback, otherwise a record used by looper_trampoline.
void* looper_data(gaddr fn, gaddr data, ALooper_callbackFunc* cb_out) {
    if (!fn) {
        *cb_out = nullptr;
        return reinterpret_cast<void*>(static_cast<uintptr_t>(data));
    }
    *cb_out = &looper_trampoline;
    return new GuestCallback{fn, data};
}

// ------------------------------------------------------------ assets / configuration

u32 a_AssetManager_fromJava(u32, u32 obj) {
    return g_asset_mgrs.to_guest(AAssetManager_fromJava(jni::env(), jni::to_host(obj)));
}
u32 a_AssetManager_open(u32 mgr, gcstr name, i32 mode) {
    AAsset* a = AAssetManager_open(g_asset_mgrs.to_host(mgr), name, mode);
    ZLOGD("AAssetManager_open(%s) -> %p", name.get(), a);
    return g_assets.to_guest(a);
}
void a_Asset_close(u32 h) {
    AAsset* a = g_assets.to_host(h);
    g_assets.remove(a);
    if (a) AAsset_close(a);
}
i32 a_Asset_getLength(u32 h) { return static_cast<i32>(AAsset_getLength64(g_assets.to_host(h))); }
i32 a_Asset_getRemainingLength(u32 h) { return static_cast<i32>(AAsset_getRemainingLength64(g_assets.to_host(h))); }
i32 a_Asset_read(u32 h, gaddr buf, gsize n) { return AAsset_read(g_assets.to_host(h), hp(buf), n); }
i32 a_Asset_seek(u32 h, i32 off, i32 whence) {
    return static_cast<i32>(AAsset_seek64(g_assets.to_host(h), off, whence));
}

u32 a_Configuration_new() { return g_configs.to_guest(AConfiguration_new()); }
void a_Configuration_delete(u32 h) {
    AConfiguration* c = g_configs.to_host(h);
    g_configs.remove(c);
    if (c) AConfiguration_delete(c);
}
void a_Configuration_fromAssetManager(u32 c, u32 m) {
    AConfiguration_fromAssetManager(g_configs.to_host(c), g_asset_mgrs.to_host(m));
}
void a_Configuration_getCountry(u32 c, gaddr out) { AConfiguration_getCountry(g_configs.to_host(c), hp<char>(out)); }
void a_Configuration_getLanguage(u32 c, gaddr out) { AConfiguration_getLanguage(g_configs.to_host(c), hp<char>(out)); }
i32 a_Configuration_getSdkVersion(u32 c) { return AConfiguration_getSdkVersion(g_configs.to_host(c)); }

// ------------------------------------------------------------ input

// When the system takes a touch away from the app (ACTION_CANCEL: the three-finger screenshot of some phones,
// a window taking the focus...), the game never lets go of those fingers: its touch handling stays stuck and
// nothing answers the touch screen until the app is restarted. The cancel is therefore replaced, for the
// game, by the lifts of every pointer it lists, as if the fingers had been raised one after the other.
struct LiftEvent {
    AInputEvent* base;                            // the cancel event; its pointers are listed in the same order
    i32 action;                                   // POINTER_UP (with the pointer index) or UP
    u32 count;                                    // pointers this event lists (the first ones of the base)
    std::shared_ptr<std::atomic<int>> remaining;  // lifts of this cancel the game has not finished yet
};
constexpr u32 kLiftTag = 0x1B;
PtrMap<LiftEvent> g_lifts(kLiftTag);
std::mutex g_lift_lock;
std::deque<LiftEvent*> g_pending_lifts;

// How the game registered the input queue with its looper (to wake it when lifts are waiting).
struct InputSource {
    bool valid = false;
    i32 ident = 0;
    gaddr fn = 0;
    gaddr data = 0;
    void* host_data = nullptr;
};
InputSource g_input_source;

bool is_lift(u32 h) { return (h & 0xFF) == kLiftTag; }

// Real events as they are; lifts resolve to the cancel event they come from.
AInputEvent* ev(u32 h) {
    if (is_lift(h)) {
        LiftEvent* l = g_lifts.to_host(h);
        return l ? l->base : nullptr;
    }
    return g_events.to_host(h);
}
i32 a_InputEvent_getDeviceId(u32 e) { return AInputEvent_getDeviceId(ev(e)); }
i32 a_InputEvent_getSource(u32 e) { return AInputEvent_getSource(ev(e)); }
i32 a_InputEvent_getType(u32 e) { return AInputEvent_getType(ev(e)); }
i32 a_KeyEvent_getAction(u32 e) { return AKeyEvent_getAction(ev(e)); }
i32 a_KeyEvent_getKeyCode(u32 e) { return AKeyEvent_getKeyCode(ev(e)); }
i32 a_KeyEvent_getMetaState(u32 e) { return AKeyEvent_getMetaState(ev(e)); }
i32 a_KeyEvent_getScanCode(u32 e) { return AKeyEvent_getScanCode(ev(e)); }
i32 a_MotionEvent_getAction(u32 e) {
    if (is_lift(e)) {
        LiftEvent* l = g_lifts.to_host(e);
        return l ? l->action : 0;
    }
    return AMotionEvent_getAction(ev(e));
}
float a_MotionEvent_getAxisValue(u32 e, i32 axis, u32 i) { return AMotionEvent_getAxisValue(ev(e), axis, i); }
u32 a_MotionEvent_getPointerCount(u32 e) {
    if (is_lift(e)) {
        LiftEvent* l = g_lifts.to_host(e);
        return l ? l->count : 0;
    }
    return static_cast<u32>(AMotionEvent_getPointerCount(ev(e)));
}
i32 a_MotionEvent_getPointerId(u32 e, u32 i) { return AMotionEvent_getPointerId(ev(e), i); }
float a_MotionEvent_getPressure(u32 e, u32 i) { return AMotionEvent_getPressure(ev(e), i); }
float a_MotionEvent_getX(u32 e, u32 i) { return AMotionEvent_getX(ev(e), i); }
float a_MotionEvent_getY(u32 e, u32 i) { return AMotionEvent_getY(ev(e), i); }

void a_InputQueue_attachLooper(u32 q, u32 looper, i32 ident, gaddr fn, gaddr data) {
    ALooper_callbackFunc cb;
    void* d = looper_data(fn, data, &cb);
    g_input_source = InputSource{true, ident, fn, data, d};
    AInputQueue_attachLooper(g_queues.to_host(q), g_loopers.to_host(looper), ident, cb, d);
}
void a_InputQueue_detachLooper(u32 q) {
    g_input_source.valid = false;
    AInputQueue_detachLooper(g_queues.to_host(q));
}

bool lifts_pending() {
    std::lock_guard lk(g_lift_lock);
    return !g_pending_lifts.empty();
}

i32 a_InputQueue_getEvent(u32 q, gp<u32> out) {
    {
        std::lock_guard lk(g_lift_lock);
        if (!g_pending_lifts.empty()) {
            LiftEvent* l = g_pending_lifts.front();
            g_pending_lifts.pop_front();
            *out.get() = g_lifts.to_guest(l);
            return 0;
        }
    }
    AInputEvent* e = nullptr;
    i32 r = AInputQueue_getEvent(g_queues.to_host(q), &e);
    if (r < 0) {
        *out.get() = 0;
        return r;
    }
    cheat_ui_on_input(e);
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION &&
        (AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK) == AMOTION_EVENT_ACTION_CANCEL &&
        AMotionEvent_getPointerCount(e) >= 1) {
        size_t n = AMotionEvent_getPointerCount(e);
        ZLOGI("touch cancelled by the system (%zu pointers): lifting them for the game", n);
        auto remaining = std::make_shared<std::atomic<int>>(static_cast<int>(n));
        std::vector<LiftEvent*> lifts;
        for (size_t k = n; k-- > 0;) {  // the last pointer first, like fingers coming off the screen
            i32 action = k == 0 ? AMOTION_EVENT_ACTION_UP
                                : (AMOTION_EVENT_ACTION_POINTER_UP |
                                   (static_cast<i32>(k) << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT));
            lifts.push_back(new LiftEvent{e, action, static_cast<u32>(k + 1), remaining});
        }
        {
            std::lock_guard lk(g_lift_lock);
            for (size_t i = 1; i < lifts.size(); ++i) g_pending_lifts.push_back(lifts[i]);
        }
        *out.get() = g_lifts.to_guest(lifts[0]);
        return 0;
    }
    *out.get() = g_events.to_guest(e);
    return r;
}
i32 a_InputQueue_preDispatchEvent(u32 q, u32 e) {
    if (is_lift(e)) return 0;  // touch events are never taken by the keyboard
    return AInputQueue_preDispatchEvent(g_queues.to_host(q), ev(e));
}
void a_InputQueue_finishEvent(u32 q, u32 e, i32 handled) {
    if (is_lift(e)) {
        LiftEvent* l = g_lifts.to_host(e);
        if (!l) return;
        g_lifts.remove(l);
        AInputEvent* base = l->base;
        bool last = l->remaining->fetch_sub(1) == 1;
        delete l;
        if (last) AInputQueue_finishEvent(g_queues.to_host(q), base, handled);  // the cancel itself is finished once
        return;
    }
    AInputEvent* event = ev(e);
    g_events.remove(event);
    AInputQueue_finishEvent(g_queues.to_host(q), event, handled);
}

// ------------------------------------------------------------ looper

u32 a_Looper_prepare(i32 opts) { return g_loopers.to_guest(ALooper_prepare(opts)); }
i32 a_Looper_addFd(u32 looper, i32 fd, i32 ident, i32 events, gaddr fn, gaddr data) {
    ALooper_callbackFunc cb;
    void* d = looper_data(fn, data, &cb);
    return ALooper_addFd(g_loopers.to_host(looper), fd, ident, events, cb, d);
}
i32 a_Looper_pollAll(i32 timeout, gp<i32> out_fd, gp<i32> out_events, gp<u32> out_data) {
    // Lifts of a cancelled touch are waiting: let the game's input handler drain them now, the real
    // queue has nothing new that would wake the looper.
    if (g_input_source.valid && lifts_pending()) {
        if (g_input_source.fn == 0) {
            if (out_fd.a) *out_fd.get() = -1;
            if (out_events.a) *out_events.get() = ALOOPER_EVENT_INPUT;
            if (out_data.a) *out_data.get() = g_input_source.data;
            return g_input_source.ident;
        }
        cpu::call(g_input_source.fn, cpu::Args().i32v(-1).i32v(ALOOPER_EVENT_INPUT).u32v(g_input_source.data));
    }
    int fd = 0, events = 0;
    void* data = nullptr;
    int r;
    do r = ALooper_pollOnce(timeout, &fd, &events, &data);
    while (r == ALOOPER_POLL_CALLBACK);
    if (out_fd.a) *out_fd.get() = fd;
    if (out_events.a) *out_events.get() = events;
    if (out_data.a) *out_data.get() = static_cast<u32>(reinterpret_cast<uintptr_t>(data));
    return r;
}

// ------------------------------------------------------------ windows / sensors

// The system destroys the window whenever the game goes to the background or another activity covers it, but the
// engine keeps asking about it for a moment (until it has handled the destroy callback, and again before the new
// window exists). Those questions get the last known answers instead of a null window.
std::atomic<i32> g_win_width{2400}, g_win_height{1080}, g_win_format{1};

ANativeWindow* live_window(u32 w) { return g_windows.find_host(w); }

i32 a_NativeWindow_getFormat(u32 w) {
    ANativeWindow* nw = live_window(w);
    return nw ? (g_win_format = ANativeWindow_getFormat(nw)) : g_win_format.load();
}
i32 a_NativeWindow_getHeight(u32 w) {
    ANativeWindow* nw = live_window(w);
    return nw ? (g_win_height = ANativeWindow_getHeight(nw)) : g_win_height.load();
}
i32 a_NativeWindow_getWidth(u32 w) {
    ANativeWindow* nw = live_window(w);
    return nw ? (g_win_width = ANativeWindow_getWidth(nw)) : g_win_width.load();
}
i32 a_NativeWindow_setBuffersGeometry(u32 w, i32 width, i32 height, i32 fmt) {
    ANativeWindow* nw = live_window(w);
    return nw ? ANativeWindow_setBuffersGeometry(nw, width, height, fmt) : 0;
}

u32 a_SensorManager_getInstance() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return g_sensor_mgrs.to_guest(ASensorManager_getInstance());
#pragma clang diagnostic pop
}
u32 a_SensorManager_getDefaultSensor(u32 m, i32 type) {
    return g_sensors.to_guest(ASensorManager_getDefaultSensor(g_sensor_mgrs.to_host(m), type));
}
u32 a_SensorManager_createEventQueue(u32 m, u32 looper, i32 ident, gaddr fn, gaddr data) {
    ALooper_callbackFunc cb;
    void* d = looper_data(fn, data, &cb);
    return g_sensor_queues.to_guest(
        ASensorManager_createEventQueue(g_sensor_mgrs.to_host(m), g_loopers.to_host(looper), ident, cb, d));
}
i32 a_SensorManager_destroyEventQueue(u32 m, u32 q) {
    ASensorEventQueue* queue = g_sensor_queues.to_host(q);
    g_sensor_queues.remove(queue);
    return ASensorManager_destroyEventQueue(g_sensor_mgrs.to_host(m), queue);
}
i32 a_SensorEventQueue_enableSensor(u32 q, u32 s) {
    return ASensorEventQueue_enableSensor(g_sensor_queues.to_host(q), g_sensors.to_host(s));
}
i32 a_SensorEventQueue_disableSensor(u32 q, u32 s) {
    return ASensorEventQueue_disableSensor(g_sensor_queues.to_host(q), g_sensors.to_host(s));
}
i32 a_SensorEventQueue_setEventRate(u32 q, u32 s, i32 usec) {
    return ASensorEventQueue_setEventRate(g_sensor_queues.to_host(q), g_sensors.to_host(s), usec);
}
// ASensorEvent has the same 104-byte layout on both ABIs.
i32 a_SensorEventQueue_getEvents(u32 q, gaddr events, gsize count) {
    return static_cast<i32>(ASensorEventQueue_getEvents(g_sensor_queues.to_host(q), hp<ASensorEvent>(events), count));
}

// ------------------------------------------------------------ NativeActivity

struct Activity {
    ANativeActivity* host = nullptr;
    gaddr guest = 0;
    gaddr callbacks = 0;
};
Activity g_activity;
std::once_flag g_load_once;

void a_NativeActivity_finish(gaddr) {
    if (g_activity.host) ANativeActivity_finish(g_activity.host);
}

enum Cb {
    kOnStart, kOnResume, kOnSaveInstanceState, kOnPause, kOnStop, kOnDestroy, kOnWindowFocusChanged,
    kOnNativeWindowCreated, kOnNativeWindowResized, kOnNativeWindowRedrawNeeded, kOnNativeWindowDestroyed,
    kOnInputQueueCreated, kOnInputQueueDestroyed, kOnContentRectChanged, kOnConfigurationChanged, kOnLowMemory,
};

gaddr guest_cb(Cb which) { return g_activity.callbacks ? rd<u32>(g_activity.callbacks + 4 * which) : 0; }

// Watchdog: an activity callback that blocks the UI thread for seconds ends in
// an ANR; log where every guest thread is when that happens.
std::atomic<int64_t> g_cb_since{0};
std::atomic<int> g_cb_which{-1};

void watchdog() {
    bool reported = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        int64_t since = g_cb_since.load();
        if (!since) {
            reported = false;
            continue;
        }
        int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
        // The engine never finishes its threads when the activity is destroyed
        // (the UI thread then waits forever). The activity is going away anyway:
        // end the process instead of letting Android report it as not responding.
        if (g_cb_which.load() == kOnDestroy && now - since > 2'000'000'000ll) {
            ZLOGW("game engine did not shut down within 2 s of onDestroy: exiting the process");
            _exit(0);
        }
        if (!reported && now - since > 3'000'000'000ll) {
            ZLOGW("activity callback %d has been blocking the UI thread for 3 s", g_cb_which.load());
            cpu::dump_all_threads("UI thread blocked");
            reported = true;
        }
    }
}

u64 forward(ANativeActivity* a, Cb which, const cpu::Args& extra) {
    gaddr fn = guest_cb(which);
    if (!fn) return 0;
    static std::once_flag started;
    std::call_once(started, [] { std::thread(watchdog).detach(); });
    ZLOGD("activity callback %d -> guest", which);
    g_cb_which = which;
    g_cb_since = std::chrono::steady_clock::now().time_since_epoch().count();
    jni::NativeFrame frame(a->env);
    cpu::Args args;
    args.u32v(g_activity.guest);
    for (u32 w : extra.words()) args.u32v(w);
    u64 r = cpu::call(fn, args);
    g_cb_since = 0;
    ZLOGD("activity callback %d done", which);
    return r;
}

void on_start(ANativeActivity* a) { forward(a, kOnStart, {}); }
void on_resume(ANativeActivity* a) { forward(a, kOnResume, {}); }
void on_pause(ANativeActivity* a) { forward(a, kOnPause, {}); }
void on_stop(ANativeActivity* a) { forward(a, kOnStop, {}); }
void on_destroy(ANativeActivity* a) { forward(a, kOnDestroy, {}); }
void on_low_memory(ANativeActivity* a) { forward(a, kOnLowMemory, {}); }
void on_configuration_changed(ANativeActivity* a) { forward(a, kOnConfigurationChanged, {}); }
void on_window_focus_changed(ANativeActivity* a, int focused) {
    forward(a, kOnWindowFocusChanged, cpu::Args().i32v(focused));
}

void* on_save_instance_state(ANativeActivity* a, size_t* out_len) {
    *out_len = 0;
    if (!guest_cb(kOnSaveInstanceState)) return nullptr;
    gaddr len_slot = mem::gcalloc(1, 4);
    gaddr data = static_cast<gaddr>(forward(a, kOnSaveInstanceState, cpu::Args().u32v(len_slot)));
    u32 len = rd<u32>(len_slot);
    mem::gfree(len_slot);
    if (!data || !len) return nullptr;
    void* copy = malloc(len);
    memcpy(copy, hp(data), len);
    mem::gfree(data);
    *out_len = len;
    return copy;
}

// ANativeWindow_setFrameRate exists from Android 11 on, in libnativewindow.so.
using SetFrameRateFn = int32_t (*)(ANativeWindow*, float, int8_t);
SetFrameRateFn set_frame_rate_fn() {
    static SetFrameRateFn fn = [] {
        void* lib = dlopen("libnativewindow.so", RTLD_NOW);
        void* f = lib ? dlsym(lib, "ANativeWindow_setFrameRate") : nullptr;
        return reinterpret_cast<SetFrameRateFn>(f ? f : dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate"));
    }();
    return fn;
}

// Refresh rates of the display's modes that have its current resolution (Display.getSupportedModes()), on the UI
// thread. Empty if they cannot be read.
std::vector<int> display_refresh_rates(ANativeActivity* a) {
    JNIEnv* env = a->env;
    std::vector<int> rates;
    jclass activity_class = env->GetObjectClass(a->clazz);
    jmethodID get_wm = env->GetMethodID(activity_class, "getWindowManager", "()Landroid/view/WindowManager;");
    jobject wm = get_wm ? env->CallObjectMethod(a->clazz, get_wm) : nullptr;
    jclass wm_class = env->FindClass("android/view/WindowManager");
    jclass display_class = env->FindClass("android/view/Display");
    jclass mode_class = env->FindClass("android/view/Display$Mode");
    if (!env->ExceptionCheck() && wm && wm_class && display_class && mode_class) {
        jmethodID get_display = env->GetMethodID(wm_class, "getDefaultDisplay", "()Landroid/view/Display;");
        jmethodID get_mode = env->GetMethodID(display_class, "getMode", "()Landroid/view/Display$Mode;");
        jmethodID get_modes = env->GetMethodID(display_class, "getSupportedModes", "()[Landroid/view/Display$Mode;");
        jmethodID get_rate = env->GetMethodID(mode_class, "getRefreshRate", "()F");
        jmethodID get_width = env->GetMethodID(mode_class, "getPhysicalWidth", "()I");
        jmethodID get_height = env->GetMethodID(mode_class, "getPhysicalHeight", "()I");
        jobject display = env->ExceptionCheck() ? nullptr : env->CallObjectMethod(wm, get_display);
        jobject current = display ? env->CallObjectMethod(display, get_mode) : nullptr;
        auto modes = static_cast<jobjectArray>(display ? env->CallObjectMethod(display, get_modes) : nullptr);
        if (!env->ExceptionCheck() && current && modes) {
            const jint width = env->CallIntMethod(current, get_width);
            const jint height = env->CallIntMethod(current, get_height);
            for (jsize i = 0, n = env->GetArrayLength(modes); i < n; i++) {
                jobject mode = env->GetObjectArrayElement(modes, i);
                if (env->CallIntMethod(mode, get_width) == width && env->CallIntMethod(mode, get_height) == height) {
                    rates.push_back(static_cast<int>(env->CallFloatMethod(mode, get_rate) + 0.5f));
                }
                env->DeleteLocalRef(mode);
            }
        }
        for (jobject o : {display, current, static_cast<jobject>(modes)}) {
            if (o) env->DeleteLocalRef(o);
        }
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        rates.clear();
    }
    for (jobject o : {static_cast<jobject>(activity_class), wm, static_cast<jobject>(wm_class),
                      static_cast<jobject>(display_class), static_cast<jobject>(mode_class)}) {
        if (o) env->DeleteLocalRef(o);
    }
    return rates;
}

// The game aims for the display's highest refresh rate (unless debug.acp64.fps fixes its frame rate), and asks the
// system to run the display at that rate for its window: some phones give games 60 Hz unless they ask, and a
// frame rate that does not match the refresh rate shows frames for 2 or 3 refreshes in turn (jerky).
void set_frame_rate(ANativeActivity* a, ANativeWindow* w) {
    const std::vector<int> rates = display_refresh_rates(a);
    const int max_rate = rates.empty() ? 0 : *std::max_element(rates.begin(), rates.end());
    if (max_rate >= 30 && !game::fixed_fps()) game::set_target_fps(max_rate);
    const int fps = game::target_fps();
    if (SetFrameRateFn fn = set_frame_rate_fn()) {
        fn(w, static_cast<float>(fps), 0 /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT */);
    }
    ZLOGI("frame rate: %d fps (display up to %d Hz%s)", fps, max_rate, set_frame_rate_fn() ? "" : ", no frame rate request before Android 11");
}

void on_window_created(ANativeActivity* a, ANativeWindow* w) {
    ZLOGI("native window created: %dx%d", ANativeWindow_getWidth(w), ANativeWindow_getHeight(w));
    set_frame_rate(a, w);
    forward(a, kOnNativeWindowCreated, cpu::Args().u32v(g_windows.to_guest(w)));
}
void on_window_resized(ANativeActivity* a, ANativeWindow* w) {
    forward(a, kOnNativeWindowResized, cpu::Args().u32v(g_windows.to_guest(w)));
}
void on_window_redraw_needed(ANativeActivity* a, ANativeWindow* w) {
    forward(a, kOnNativeWindowRedrawNeeded, cpu::Args().u32v(g_windows.to_guest(w)));
}
void on_window_destroyed(ANativeActivity* a, ANativeWindow* w) {
    forward(a, kOnNativeWindowDestroyed, cpu::Args().u32v(g_windows.to_guest(w)));
    g_windows.remove(w);
}
void on_input_queue_created(ANativeActivity* a, AInputQueue* q) {
    forward(a, kOnInputQueueCreated, cpu::Args().u32v(g_queues.to_guest(q)));
}
void on_input_queue_destroyed(ANativeActivity* a, AInputQueue* q) {
    forward(a, kOnInputQueueDestroyed, cpu::Args().u32v(g_queues.to_guest(q)));
    g_queues.remove(q);
}
void on_content_rect_changed(ANativeActivity* a, const ARect* r) {
    gaddr g = mem::gmalloc(16);
    memcpy(hp(g), r, 16);
    forward(a, kOnContentRectChanged, cpu::Args().u32v(g));
    mem::gfree(g);
}

// The ARM32 library ships next to this one as lib/arm64-v8a/libSparkApp32.so
// (extracted by the package manager), or in assets/zb/ as a fallback.
bool read_game_image(AAssetManager* assets, std::vector<u8>* out) {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&read_game_image), &info) && info.dli_fname) {
        std::string path = info.dli_fname;
        path = path.substr(0, path.rfind('/') + 1) + "libSparkApp32.so";
        if (FILE* f = fopen(path.c_str(), "rb")) {
            fseek(f, 0, SEEK_END);
            out->resize(static_cast<size_t>(ftell(f)));
            fseek(f, 0, SEEK_SET);
            size_t n = fread(out->data(), 1, out->size(), f);
            fclose(f);
            if (n == out->size()) {
                ZLOGI("game image: %s (%zu bytes)", path.c_str(), n);
                return true;
            }
        }
    }
    if (!assets) return false;
    AAsset* asset = AAssetManager_open(assets, "zb/libSparkApp.so", AASSET_MODE_BUFFER);
    if (!asset) return false;
    auto* buf = static_cast<const u8*>(AAsset_getBuffer(asset));
    out->assign(buf, buf + AAsset_getLength64(asset));
    AAsset_close(asset);
    ZLOGI("game image: assets/zb/libSparkApp.so (%zu bytes)", out->size());
    return true;
}

void load_game(JNIEnv* env, AAssetManager* assets) {
    std::vector<u8> image;
    if (!read_game_image(assets, &image)) fatal("the ARM32 game library is missing from the APK");
    jni::NativeFrame frame(env);
    elf::Module* m = runtime::load_library("libSparkApp.so", image.data(), image.size());
    jni::register_generated_natives();
    elf::run_initializers(m);
}

}  // namespace

ANativeWindow* host_window(u32 handle) { return g_windows.find_host(handle); }

void register_thunks() {
    REG(AAssetManager_fromJava, a_AssetManager_fromJava);
    REG(AAssetManager_open, a_AssetManager_open);
    REG(AAsset_close, a_Asset_close);
    REG(AAsset_getLength, a_Asset_getLength);
    REG(AAsset_getRemainingLength, a_Asset_getRemainingLength);
    REG(AAsset_read, a_Asset_read);
    REG(AAsset_seek, a_Asset_seek);
    REG(AConfiguration_new, a_Configuration_new);
    REG(AConfiguration_delete, a_Configuration_delete);
    REG(AConfiguration_fromAssetManager, a_Configuration_fromAssetManager);
    REG(AConfiguration_getCountry, a_Configuration_getCountry);
    REG(AConfiguration_getLanguage, a_Configuration_getLanguage);
    REG(AConfiguration_getSdkVersion, a_Configuration_getSdkVersion);
    REG(AInputEvent_getDeviceId, a_InputEvent_getDeviceId);
    REG(AInputEvent_getSource, a_InputEvent_getSource);
    REG(AInputEvent_getType, a_InputEvent_getType);
    REG(AKeyEvent_getAction, a_KeyEvent_getAction);
    REG(AKeyEvent_getKeyCode, a_KeyEvent_getKeyCode);
    REG(AKeyEvent_getMetaState, a_KeyEvent_getMetaState);
    REG(AKeyEvent_getScanCode, a_KeyEvent_getScanCode);
    REG(AMotionEvent_getAction, a_MotionEvent_getAction);
    REG(AMotionEvent_getAxisValue, a_MotionEvent_getAxisValue);
    REG(AMotionEvent_getPointerCount, a_MotionEvent_getPointerCount);
    REG(AMotionEvent_getPointerId, a_MotionEvent_getPointerId);
    REG(AMotionEvent_getPressure, a_MotionEvent_getPressure);
    REG(AMotionEvent_getX, a_MotionEvent_getX);
    REG(AMotionEvent_getY, a_MotionEvent_getY);
    REG(AInputQueue_attachLooper, a_InputQueue_attachLooper);
    REG(AInputQueue_detachLooper, a_InputQueue_detachLooper);
    REG(AInputQueue_getEvent, a_InputQueue_getEvent);
    REG(AInputQueue_preDispatchEvent, a_InputQueue_preDispatchEvent);
    REG(AInputQueue_finishEvent, a_InputQueue_finishEvent);
    REG(ALooper_prepare, a_Looper_prepare);
    REG(ALooper_addFd, a_Looper_addFd);
    REG(ALooper_pollAll, a_Looper_pollAll);
    REG(ANativeWindow_getFormat, a_NativeWindow_getFormat);
    REG(ANativeWindow_getHeight, a_NativeWindow_getHeight);
    REG(ANativeWindow_getWidth, a_NativeWindow_getWidth);
    REG(ANativeWindow_setBuffersGeometry, a_NativeWindow_setBuffersGeometry);
    REG(ASensorManager_getInstance, a_SensorManager_getInstance);
    REG(ASensorManager_getDefaultSensor, a_SensorManager_getDefaultSensor);
    REG(ASensorManager_createEventQueue, a_SensorManager_createEventQueue);
    REG(ASensorManager_destroyEventQueue, a_SensorManager_destroyEventQueue);
    REG(ASensorEventQueue_enableSensor, a_SensorEventQueue_enableSensor);
    REG(ASensorEventQueue_disableSensor, a_SensorEventQueue_disableSensor);
    REG(ASensorEventQueue_setEventRate, a_SensorEventQueue_setEventRate);
    REG(ASensorEventQueue_getEvents, a_SensorEventQueue_getEvents);
    REG(ANativeActivity_finish, a_NativeActivity_finish);
    register_egl_gles();
    register_opensl();
}

void on_jni_load(JavaVM* vm) {
    runtime::init();
    jni::init(vm);
    JNIEnv* env = nullptr;
    vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    std::call_once(g_load_once, [env] {
        register_thunks();
        load_game(env, nullptr);
    });
}

void on_create(ANativeActivity* a, void* saved, size_t saved_size) {
    ZLOGI("ANativeActivity_onCreate (sdk %d, internal %s)", a->sdkVersion, a->internalDataPath);
    runtime::init();
    if (a->externalDataPath) cheat::set_data_dir(a->externalDataPath);
    if (a->internalDataPath) cheat::set_state_dir(a->internalDataPath);
    cheat_ui_init(a->env, a->clazz);
    jni::init(a->vm);
    std::call_once(g_load_once, [a] {
        register_thunks();
        load_game(a->env, a->assetManager);
    });

    jni::NativeFrame frame(a->env);
    gaddr act = mem::gcalloc(1, 40);
    gaddr cbs = mem::gcalloc(16, 4);
    wr<u32>(act + 0, cbs);
    wr<u32>(act + 4, jni::guest_vm());
    wr<u32>(act + 8, frame.genv());
    wr<u32>(act + 12, jni::new_global_handle(a->clazz));
    wr<u32>(act + 16, a->internalDataPath ? hle::static_string(a->internalDataPath) : 0);
    wr<u32>(act + 20, a->externalDataPath ? hle::static_string(a->externalDataPath) : 0);
    wr<i32>(act + 24, a->sdkVersion);
    wr<u32>(act + 32, g_asset_mgrs.to_guest(a->assetManager));
    wr<u32>(act + 36, a->obbPath ? hle::static_string(a->obbPath) : 0);
    g_activity = Activity{a, act, cbs};

    gaddr saved_g = 0;
    if (saved && saved_size) saved_g = mem::gdup(saved, static_cast<u32>(saved_size));
    gaddr entry = elf::lookup("ANativeActivity_onCreate");
    if (!entry) fatal("guest ANativeActivity_onCreate not found");
    cpu::call(entry, cpu::Args().u32v(act).u32v(saved_g).u32v(static_cast<u32>(saved_size)));

    ANativeActivityCallbacks* cb = a->callbacks;
    cb->onStart = on_start;
    cb->onResume = on_resume;
    cb->onSaveInstanceState = on_save_instance_state;
    cb->onPause = on_pause;
    cb->onStop = on_stop;
    cb->onDestroy = on_destroy;
    cb->onWindowFocusChanged = on_window_focus_changed;
    cb->onNativeWindowCreated = on_window_created;
    cb->onNativeWindowResized = on_window_resized;
    cb->onNativeWindowRedrawNeeded = on_window_redraw_needed;
    cb->onNativeWindowDestroyed = on_window_destroyed;
    cb->onInputQueueCreated = on_input_queue_created;
    cb->onInputQueueDestroyed = on_input_queue_destroyed;
    cb->onContentRectChanged = on_content_rect_changed;
    cb->onConfigurationChanged = on_configuration_changed;
    cb->onLowMemory = on_low_memory;
    ZLOGI("guest ANativeActivity_onCreate returned");
}

}  // namespace zb::android
