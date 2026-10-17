#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "common.h"

namespace zb {

bool g_debug = false;
bool g_log_to_stderr = false;

static constexpr const char* kTag = "ACP64";

static void vlog(int prio, const char* fmt, va_list ap) {
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    __android_log_write(prio, kTag, buf);
    if (g_log_to_stderr) {
        fprintf(stderr, "%s\n", buf);
        fflush(stderr);
    }
}

void log_print(int prio, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(prio, fmt, ap);
    va_end(ap);
}

void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(ANDROID_LOG_FATAL, fmt, ap);
    va_end(ap);
    abort();
}

gaddr ga_checked(const void* p, const char* what) {
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    if (v > 0xFFFFFFFFu) fatal("host pointer %p (%s) is outside the guest address space", p, what);
    return static_cast<gaddr>(v);
}

}  // namespace zb
