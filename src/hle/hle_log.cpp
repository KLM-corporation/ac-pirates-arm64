// liblog.
#include <android/log.h>

#include "hle.h"

namespace zb::hle {
namespace {

std::string guest_tag(u32 tag) { return std::string("G:") + (tag ? hp<const char>(tag) : "?"); }

i32 l_write(i32 prio, gcstr tag, gcstr text) {
    return __android_log_write(prio, guest_tag(tag.a).c_str(), text.a ? text.get() : "(null)");
}

void l_print(JitCtx& j) {
    ArgReader r(j);
    i32 prio = static_cast<i32>(r.w32());
    u32 tag = r.w32(), fmt = r.w32();
    u32 ap = r.va_list_here();
    std::string s = vformat(hp<char>(fmt), ap);
    j.set_ret(static_cast<u32>(__android_log_write(prio, guest_tag(tag).c_str(), s.c_str())));
}

void l_vprint(JitCtx& j) {
    ArgReader r(j);
    i32 prio = static_cast<i32>(r.w32());
    u32 tag = r.w32(), fmt = r.w32(), ap = r.w32();
    std::string s = vformat(hp<char>(fmt), ap);
    j.set_ret(static_cast<u32>(__android_log_write(prio, guest_tag(tag).c_str(), s.c_str())));
}

void l_assert(JitCtx& j) {
    ArgReader r(j);
    u32 cond = r.w32(), tag = r.w32(), fmt = r.w32();
    u32 ap = r.va_list_here();
    std::string s = fmt ? vformat(hp<char>(fmt), ap) : std::string("assertion failed");
    cpu::dump_state(j, "__android_log_assert");
    fatal("guest assertion (%s) [%s]: %s", cond ? hp<const char>(cond) : "", guest_tag(tag).c_str(), s.c_str());
}

}  // namespace

void register_log() {
    REG(__android_log_write, l_write);
    REG_RAW(__android_log_print, l_print);
    REG_RAW(__android_log_vprint, l_vprint);
    REG_RAW(__android_log_assert, l_assert);
}

void register_all() {
    register_libc();
    register_math();
    register_stdio();
    register_pthread();
    register_posix();
    register_net();
    register_time();
    register_dl();
    register_log();
}

}  // namespace zb::hle
