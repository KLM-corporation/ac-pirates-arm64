// JNI bridge: a 32-bit JNIEnv/JavaVM for guest code on top of the host JNI.
//
// Guest code sees jobject/jclass/... as 32-bit handles and jmethodID/jfieldID
// as 32-bit ids; both are translated to host values on every call.
#pragma once

#include <jni.h>

#include "../thunk.h"

namespace zb::jni {

void init(JavaVM* vm);
JavaVM* host_vm();
gaddr guest_vm();

// JNIEnv for the calling thread (attaches it if needed).
JNIEnv* env();
// Guest JNIEnv* for the calling thread, binding `e` as its host env.
gaddr bind_env(JNIEnv* e);

// Reference handles.
u32 new_local(jobject o);
u32 new_global_handle(jobject o);  // takes a new host global reference
jobject to_host(u32 h);

// Scope for a Java -> guest call: binds the env and owns the local handles
// created during the call.
class NativeFrame {
public:
    explicit NativeFrame(JNIEnv* e);
    ~NativeFrame();
    NativeFrame(const NativeFrame&) = delete;
    NativeFrame& operator=(const NativeFrame&) = delete;
    gaddr genv() const { return genv_; }
    u32 local(jobject o) { return new_local(o); }

private:
    gaddr genv_;
};

// Generated trampolines for every native method declared by the APK's dex code.
struct NativeEntry {
    const char* cls;   // "org/ubisoft/geea/spark2/MSDKJava"
    const char* name;  // "msdk_result"
    const char* sig;   // "(IILandroid/content/Intent;)V"
    void* host_fn;
    gaddr* slot;  // guest implementation
};
void set_native_table(const NativeEntry* entries, size_t n);

}  // namespace zb::jni
