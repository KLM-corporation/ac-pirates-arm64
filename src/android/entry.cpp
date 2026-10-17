// Exported entry points of the arm64 libSparkApp.so.
#include <jni.h>

#include "android_glue.h"

extern "C" JNIEXPORT void ANativeActivity_onCreate(ANativeActivity* activity, void* saved_state, size_t saved_size) {
    zb::android::on_create(activity, saved_state, saved_size);
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    zb::android::on_jni_load(vm);
    return JNI_VERSION_1_6;
}
