#include "cheat_ui.h"

#include <atomic>
#include <chrono>
#include <mutex>

#include "../cheat/cheat.h"
#include "../common.h"
#include "../jni/jni_bridge.h"

namespace zb::android {
namespace {

jclass g_class = nullptr;        // global reference
jmethodID g_show = nullptr;
jmethodID g_notify = nullptr;
jobject g_activity = nullptr;    // global reference
std::atomic<int64_t> g_last_show{0};

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool clear_exception(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

}  // namespace

void cheat_ui_init(JNIEnv* env, jobject activity) {
    jclass activity_class = env->GetObjectClass(activity);
    jmethodID get_loader = env->GetMethodID(activity_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = get_loader ? env->CallObjectMethod(activity, get_loader) : nullptr;
    jclass loader_class = env->FindClass("java/lang/ClassLoader");
    jmethodID load_class =
        loader_class ? env->GetMethodID(loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") : nullptr;
    jstring name = env->NewStringUTF("com.acp64.CheatDialog");
    jobject cls = (loader && load_class) ? env->CallObjectMethod(loader, load_class, name) : nullptr;
    if (clear_exception(env) || !cls) {
        ZLOGW("cheat ui: com.acp64.CheatDialog is not in this APK; the code window is unavailable");
        return;
    }
    g_show = env->GetStaticMethodID(static_cast<jclass>(cls), "show", "(Landroid/app/Activity;)V");
    if (clear_exception(env) || !g_show) {
        ZLOGW("cheat ui: CheatDialog.show not found");
        return;
    }
    g_notify = env->GetStaticMethodID(static_cast<jclass>(cls), "notifyEvent", "(Landroid/app/Activity;I)V");
    if (clear_exception(env)) g_notify = nullptr;
    g_class = static_cast<jclass>(env->NewGlobalRef(cls));
    g_activity = env->NewGlobalRef(activity);
    zb::cheat::set_notifier(&cheat_ui_notify);
    ZLOGI("cheat ui: ready (volume up, up, down, down opens the code window)");
}

void cheat_ui_notify(int event) {
    if (!g_class || !g_notify || !g_activity) return;
    JNIEnv* env = jni::env();
    env->CallStaticVoidMethod(g_class, g_notify, g_activity, static_cast<jint>(event));
    if (clear_exception(env)) ZLOGW("cheat ui: CheatDialog.notifyEvent threw");
}

void cheat_ui_show() {
    if (!g_class || !g_show || !g_activity) return;
    int64_t now = now_ms();
    int64_t last = g_last_show.load();
    if (now - last < 1500 || !g_last_show.compare_exchange_strong(last, now)) return;
    ZLOGI("cheat ui: opening the code window");
    JNIEnv* env = jni::env();
    env->CallStaticVoidMethod(g_class, g_show, g_activity);
    if (clear_exception(env)) ZLOGW("cheat ui: CheatDialog.show threw");
}

namespace {

// Fingers currently on the screen, as the game sees them; the window never opens under a finger.
std::atomic<int> g_pointers{0};
std::atomic<bool> g_show_pending{false};

void request_show() {
    if (g_pointers.load() == 0) {
        cheat_ui_show();
    } else {
        g_show_pending = true;
    }
}

// The secret: Volume Up, Volume Up, Volume Down, Volume Down within 4 seconds. Hardware keys cannot be hit
// by accident while playing and, unlike touches, never disturb the game's touch handling (a touch gesture
// would also be eaten by phones that reserve three fingers for the screenshot).
void on_key(const AInputEvent* event) {
    int32_t code = AKeyEvent_getKeyCode(event);
    if (g_debug) {
        ZLOGD("input: key code %d action %d repeat %d", code, AKeyEvent_getAction(event),
              AKeyEvent_getRepeatCount(event));
    }
    if (AKeyEvent_getAction(event) != AKEY_EVENT_ACTION_DOWN || AKeyEvent_getRepeatCount(event) != 0) return;
    constexpr int64_t kWindowMs = 4000;
    constexpr int64_t kSameKeyMs = 60;  // some key presses reach the game twice, one frame apart
    static std::mutex lock;
    static int32_t codes[4] = {};
    static int64_t times[4] = {};
    static unsigned count = 0;
    static int32_t last_code = 0;
    static int64_t last_time = 0;
    bool open = false;
    {
        std::lock_guard lk(lock);
        int64_t now = now_ms();
        bool duplicate = code == last_code && now - last_time < kSameKeyMs;
        last_code = code;
        last_time = now;
        if (duplicate) return;
        if (code != AKEYCODE_VOLUME_UP && code != AKEYCODE_VOLUME_DOWN) {
            count = 0;  // any other key breaks the sequence
            return;
        }
        codes[count % 4] = code;
        times[count % 4] = now;
        count++;
        if (count >= 4) {
            // the slot count % 4 holds the oldest of the last four presses
            bool match = codes[count % 4] == AKEYCODE_VOLUME_UP && codes[(count + 1) % 4] == AKEYCODE_VOLUME_UP &&
                         codes[(count + 2) % 4] == AKEYCODE_VOLUME_DOWN && codes[(count + 3) % 4] == AKEYCODE_VOLUME_DOWN;
            if (match && now - times[count % 4] <= kWindowMs) {
                count = 0;
                open = true;
            }
        }
    }
    if (open) request_show();
}

void on_motion(const AInputEvent* event) {
    int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
    int pointers = static_cast<int>(AMotionEvent_getPointerCount(event));
    if (g_debug && action != AMOTION_EVENT_ACTION_MOVE) {
        ZLOGD("input: motion action %d (raw 0x%x) with %d pointers", action, AMotionEvent_getAction(event), pointers);
    }
    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN: g_pointers = 1; break;
    case AMOTION_EVENT_ACTION_POINTER_DOWN: g_pointers = pointers; break;
    case AMOTION_EVENT_ACTION_POINTER_UP: g_pointers = pointers - 1; break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_CANCEL: g_pointers = 0; break;
    default: break;
    }
    if (g_pointers.load() == 0 && g_show_pending.exchange(false)) cheat_ui_show();
}

}  // namespace

void cheat_ui_on_input(const AInputEvent* event) {
    switch (AInputEvent_getType(event)) {
    case AINPUT_EVENT_TYPE_KEY: on_key(event); break;
    case AINPUT_EVENT_TYPE_MOTION: on_motion(event); break;
    default: break;
    }
}

}  // namespace zb::android

// Called by CheatDialog when the player validates a code: 0 unknown, 1 on, 2 off.
extern "C" JNIEXPORT jint JNICALL Java_com_acp64_CheatDialog_nativeSubmit(JNIEnv* env, jclass, jstring code) {
    const char* chars = code ? env->GetStringUTFChars(code, nullptr) : nullptr;
    int result = zb::cheat::submit_code(chars ? chars : "");
    if (chars) env->ReleaseStringUTFChars(code, chars);
    return result;
}
