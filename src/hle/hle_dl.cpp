// dlopen/dlsym over guest modules and the thunk registry.
#include <mutex>
#include <set>
#include <unordered_map>

#include "../elf_loader.h"
#include "hle.h"

namespace zb::hle {
namespace {

constexpr u32 kGlobalHandle = 0x10;
constexpr u32 kSystemHandle = 0x20;
constexpr u32 kModuleBase = 0x100;
constexpr u32 kRtldDefault = 0xFFFFFFFFu;
constexpr u32 kRtldNext = 0xFFFFFFFEu;

std::mutex g_lock;
std::string g_error;
std::unordered_map<u32, elf::Module*> g_handles;

const std::set<std::string> kSystemLibs = {
    "libc.so",  "libm.so",       "libdl.so",        "liblog.so",       "libz.so",
    "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so",    "libandroid.so",
    "libOpenSLES.so", "libstdc++.so", "libjnigraphics.so", "libOpenMAXAL.so"};

std::string base_name(const char* path) {
    const char* s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void set_error(const std::string& e) {
    std::lock_guard lk(g_lock);
    g_error = e;
}

gaddr d_dlopen(gcstr name, i32 flags) {
    if (!name.a) return kGlobalHandle;
    std::string b = base_name(name);
    if (elf::Module* m = elf::find_module(b)) {
        std::lock_guard lk(g_lock);
        for (auto& [h, mod] : g_handles) {
            if (mod == m) return h;
        }
        u32 h = kModuleBase + static_cast<u32>(g_handles.size());
        g_handles[h] = m;
        return h;
    }
    if (kSystemLibs.count(b)) return kSystemHandle;
    ZLOGW("dlopen(%s, 0x%x): not available", name.get(), flags);
    set_error("dlopen failed: library \"" + std::string(name.get()) + "\" not found");
    return 0;
}

gaddr resolve_any(const std::string& s) {
    if (gaddr a = elf::lookup(s)) return a;
    if (gaddr a = cpu::find_thunk(s)) return a;
    return cpu::find_data(s);
}

gaddr d_dlsym(gaddr handle, gcstr sym) {
    std::string s = sym.get();
    gaddr r = 0;
    if (handle == kRtldDefault || handle == kRtldNext || handle == kGlobalHandle) {
        r = resolve_any(s);
    } else if (handle == kSystemHandle) {
        r = cpu::find_thunk(s);
        if (!r) r = cpu::find_data(s);
    } else {
        elf::Module* m = nullptr;
        {
            std::lock_guard lk(g_lock);
            auto it = g_handles.find(handle);
            if (it != g_handles.end()) m = it->second;
        }
        if (m) {
            auto it = m->exports.find(s);
            r = it == m->exports.end() ? 0 : it->second;
        }
    }
    if (!r) {
        ZLOGW("dlsym(0x%x, %s): not found", handle, s.c_str());
        set_error("undefined symbol: " + s);
    } else {
        ZLOGD("dlsym(0x%x, %s) -> 0x%08x", handle, s.c_str(), r);
    }
    return r;
}

i32 d_dlclose(gaddr) { return 0; }

gaddr d_dlerror() {
    std::lock_guard lk(g_lock);
    if (g_error.empty()) return 0;
    gaddr r = static_string(g_error);
    g_error.clear();
    return r;
}

}  // namespace

void register_dl() {
    REG(dlopen, d_dlopen);
    REG(dlsym, d_dlsym);
    REG(dlclose, d_dlclose);
    REG(dlerror, d_dlerror);
}

}  // namespace zb::hle
