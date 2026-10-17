// Shared definitions for the ARM32-on-ARM64 runtime ("zb").
//
// Guest memory is identity-mapped into the low 4 GiB of the host process, so a
// guest address is also a valid host address once zero-extended.
#pragma once

#include <android/log.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace zb {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// A guest (32-bit) address.
using gaddr = u32;

inline void* hp(gaddr a) { return reinterpret_cast<void*>(static_cast<uintptr_t>(a)); }

template <typename T>
inline T* hp(gaddr a) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(a));
}

// Host pointer to guest address. Only valid for pointers into guest memory.
gaddr ga_checked(const void* p, const char* what);
inline gaddr ga(const void* p) { return static_cast<gaddr>(reinterpret_cast<uintptr_t>(p)); }

// Typed guest pointer used in thunk signatures.
template <typename T>
struct gp {
    u32 a = 0;
    T* get() const { return hp<T>(a); }
    operator T*() const { return get(); }
    T* operator->() const { return get(); }
    explicit operator bool() const { return a != 0; }
};

template <typename T>
inline T rd(gaddr a) {
    T v;
    std::memcpy(&v, hp(a), sizeof(T));
    return v;
}

template <typename T>
inline void wr(gaddr a, T v) {
    std::memcpy(hp(a), &v, sizeof(T));
}

extern bool g_debug;
extern bool g_log_to_stderr;

void log_print(int prio, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace zb

#define ZLOGI(...) ::zb::log_print(ANDROID_LOG_INFO, __VA_ARGS__)
#define ZLOGW(...) ::zb::log_print(ANDROID_LOG_WARN, __VA_ARGS__)
#define ZLOGE(...) ::zb::log_print(ANDROID_LOG_ERROR, __VA_ARGS__)
#define ZLOGD(...)                                                \
    do {                                                          \
        if (::zb::g_debug) ::zb::log_print(ANDROID_LOG_DEBUG, __VA_ARGS__); \
    } while (0)
