// In-game window for typing cheat codes: opened by a secret sequence of volume keys and
// implemented in Java (tools/java/com/acp64/CheatDialog.java, added to the APK).
#pragma once

#include <android/input.h>
#include <jni.h>

namespace zb::android {

// Finds com.acp64.CheatDialog through the activity's class loader. Safe to call
// when the class is missing (the feature is then simply unavailable).
void cheat_ui_init(JNIEnv* env, jobject activity);

// Opens the code window. May be called from any thread; repeated calls within a
// moment are ignored.
void cheat_ui_show();

// Shows a message about something that happened after a cheat code was typed (cheat::Notifier).
void cheat_ui_notify(int event);

// Looks at an input event on its way to the game: volume up, up, down, down open the window.
void cheat_ui_on_input(const AInputEvent* event);

}  // namespace zb::android
