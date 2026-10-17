// Time functions. Guest time_t, long and the timespec/timeval fields are 32-bit.
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>
#include <cwchar>

#include "hle.h"

namespace zb::hle {
namespace {

// Guest struct tm: 9 ints, long tm_gmtoff, const char* tm_zone (44 bytes).
void tm_to_guest(gaddr g, const struct tm& t) {
    i32 f[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    memcpy(hp(g), f, sizeof(f));
    wr<i32>(g + 36, static_cast<i32>(t.tm_gmtoff));
    wr<u32>(g + 40, t.tm_zone ? static_string(t.tm_zone) : 0);
}

struct tm tm_from_guest(gaddr g) {
    i32 f[9];
    memcpy(f, hp(g), sizeof(f));
    struct tm t{};
    t.tm_sec = f[0];
    t.tm_min = f[1];
    t.tm_hour = f[2];
    t.tm_mday = f[3];
    t.tm_mon = f[4];
    t.tm_year = f[5];
    t.tm_wday = f[6];
    t.tm_yday = f[7];
    t.tm_isdst = f[8];
    t.tm_gmtoff = rd<i32>(g + 36);
    u32 zone = rd<u32>(g + 40);
    t.tm_zone = zone ? hp<const char>(zone) : nullptr;
    return t;
}

gtime t_time(gp<i32> out) {
    gtime now = static_cast<gtime>(time(nullptr));
    if (out.a) *out.get() = now;
    return now;
}

i32 t_gettimeofday(gp<GTimeval> tv, gaddr tz) {
    timeval h;
    struct timezone z;
    gettimeofday(&h, &z);
    if (tv.a) *tv.get() = GTimeval{static_cast<i32>(h.tv_sec), static_cast<i32>(h.tv_usec)};
    if (tz) memcpy(hp(tz), &z, 8);
    return 0;
}

i32 t_clock_gettime(JitCtx& j, i32 clk, gp<GTimespec> ts) {
    timespec h;
    if (clock_gettime(clk, &h)) {
        sync_errno(j);
        return -1;
    }
    *ts.get() = GTimespec{static_cast<i32>(h.tv_sec), static_cast<i32>(h.tv_nsec)};
    return 0;
}

glong t_clock() { return static_cast<glong>(clock()); }

i32 t_nanosleep(JitCtx& j, gp<GTimespec> req, gp<GTimespec> rem) {
    timespec h{req->tv_sec, req->tv_nsec}, left{};
    timespec t0{}, t1{};
    if (g_debug) clock_gettime(CLOCK_MONOTONIC, &t0);
    int r = nanosleep(&h, &left);
    if (g_debug) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        cpu::note_wait("nanosleep", (t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec));
    }
    if (r) {
        sync_errno(j);
        if (rem.a) *rem.get() = GTimespec{static_cast<i32>(left.tv_sec), static_cast<i32>(left.tv_nsec)};
    }
    return r;
}

i32 t_usleep(u32 us) {
    if (!g_debug) return usleep(us);
    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int r = usleep(us);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    cpu::note_wait("usleep", (t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec));
    return r;
}
u32 t_sleep(u32 s) { return sleep(s); }

gaddr t_gmtime_r(gp<i32> t, gaddr out) {
    time_t v = *t.get();
    struct tm r;
    if (!gmtime_r(&v, &r)) return 0;
    tm_to_guest(out, r);
    return out;
}
gaddr t_localtime_r(gp<i32> t, gaddr out) {
    time_t v = *t.get();
    struct tm r;
    if (!localtime_r(&v, &r)) return 0;
    tm_to_guest(out, r);
    return out;
}
gaddr t_gmtime(JitCtx& j, gp<i32> t) { return t_gmtime_r(t, tls_buffer(j, tls::kTm)); }
gaddr t_localtime(JitCtx& j, gp<i32> t) { return t_localtime_r(t, tls_buffer(j, tls::kTm)); }

gtime t_mktime(gaddr g) {
    struct tm t = tm_from_guest(g);
    time_t r = mktime(&t);
    tm_to_guest(g, t);
    return static_cast<gtime>(r);
}

double t_difftime(gtime a, gtime b) { return static_cast<double>(a) - static_cast<double>(b); }

gsize t_strftime(gstr buf, gsize max, gcstr fmt, gaddr tmg) {
    struct tm t = tm_from_guest(tmg);
    return static_cast<gsize>(strftime(buf, max, fmt, &t));
}

gsize t_wcsftime(gp<wchar_t> buf, gsize max, gp<const wchar_t> fmt, gaddr tmg) {
    struct tm t = tm_from_guest(tmg);
    return static_cast<gsize>(wcsftime(buf, max, fmt, &t));
}

}  // namespace

void register_time() {
    REG(time, t_time);
    REG(gettimeofday, t_gettimeofday);
    REG(clock_gettime, t_clock_gettime);
    REG(clock, t_clock);
    REG(nanosleep, t_nanosleep);
    REG(usleep, t_usleep);
    REG(sleep, t_sleep);
    REG(gmtime, t_gmtime);
    REG(gmtime_r, t_gmtime_r);
    REG(localtime, t_localtime);
    REG(localtime_r, t_localtime_r);
    REG(mktime, t_mktime);
    REG(difftime, t_difftime);
    REG(strftime, t_strftime);
    REG(wcsftime, t_wcsftime);
}

}  // namespace zb::hle
