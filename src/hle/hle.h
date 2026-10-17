// High-level emulation of the libraries imported by the guest (bionic ILP32 ABI).
#pragma once

#include <cstdio>
#include <string>

#include "../thunk.h"

namespace zb::hle {

// Guest ILP32 scalar types.
using gsize = u32;
using gssize = i32;
using glong = i32;
using gulong = u32;
using goff = i32;
using gtime = i32;
using gcstr = gp<const char>;
using gstr = gp<char>;

void register_all();
void register_libc();
void register_math();
void register_stdio();
void register_pthread();
void register_posix();
void register_net();
void register_time();
void register_dl();
void register_log();

// Raw `svc #0` from guest code.
void linux_syscall(JitCtx& j);

// Formatting with a guest va_list (address of the next argument).
std::string vformat(const char* fmt, u32 ap, int* consumed_ap = nullptr);
std::wstring vwformat(const wchar_t* fmt, u32 ap);
// scanf from a string. Returns the conversion count or EOF.
int vscan_string(const char* input, const char* fmt, u32 ap);
int vscan_file(FILE* f, const char* fmt, u32 ap);

// Guest FILE* <-> host FILE*.
FILE* host_file(gaddr gf);
gaddr guest_file(FILE* f);
void forget_file(gaddr gf);

// A guest copy of a host string that stays valid forever (deduplicated).
gaddr static_string(const std::string& s);

// Per-thread guest scratch buffer of `size` bytes at TLS offset `off`.
gaddr tls_buffer(JitCtx& j, u32 off);

// Copies host errno into the guest errno slot.
inline void sync_errno(JitCtx& j) { j.set_errno(errno); }

// Writes a host pointer result into a guest `char**`-style slot.
inline void write_ptr(gaddr slot, const void* p) {
    if (slot) wr<u32>(slot, ga(p));
}

// Guest struct timespec / timeval (32-bit fields).
struct GTimespec {
    i32 tv_sec;
    i32 tv_nsec;
};
struct GTimeval {
    i32 tv_sec;
    i32 tv_usec;
};

// Guest O_* flags differ from arm64 for O_DIRECTORY, O_NOFOLLOW, O_DIRECT and O_LARGEFILE.
int open_flags_from_guest(int f);
int open_flags_to_guest(int f);

}  // namespace zb::hle
