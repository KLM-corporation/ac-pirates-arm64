// NativeActivity glue and libandroid for the guest.
#pragma once

#include <android/native_activity.h>
#include <android/native_window.h>

#include "../common.h"

namespace zb::android {

void register_thunks();  // libandroid, EGL, GLES, OpenSL ES
void on_jni_load(JavaVM* vm);
void on_create(ANativeActivity* activity, void* saved_state, size_t saved_size);

ANativeWindow* host_window(u32 handle);

void register_egl_gles();
void register_opensl();

}  // namespace zb::android
