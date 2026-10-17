// Guest CPU: Dynarmic A32 JIT instances, host<->guest calls and thunks.
//
// Every host thread that runs guest code owns a GuestThread with a guest stack
// and a small pool of JIT contexts. Dynarmic's Run() is not re-entrant, so a
// guest -> host -> guest call (callbacks, JNI natives) uses the next context of
// the pool, with its stack placed below the caller's.
#pragma once

#include <array>
#include <memory>
#include <string_view>
#include <vector>

#include "common.h"

namespace Dynarmic::A32 {
class Jit;
}

namespace zb {

struct JitCtx;
struct GuestThread;

// Thunk handler: reads guest arguments from the context, writes the result.
using ThunkFn = void (*)(JitCtx&);

namespace cpu {

constexpr u32 kSvcReturn = 0x00F00001;  // "return to host" trap
constexpr u32 kSvcThunkBase = 0x00100000;

void init();

// The current thread's guest state (created on first use).
GuestThread* current_thread();
GuestThread* current_thread_if_any();
// Allocates guest state for a thread that does not exist yet (pthread_create),
// then binds it to the calling host thread.
GuestThread* new_thread_state(u32 stack_size);
void adopt_thread(GuestThread* t);

// Builds AAPCS (soft-float) argument words for a guest call.
class Args {
public:
    Args& u32v(u32 v) {
        w_.push_back(v);
        return *this;
    }
    Args& i32v(i32 v) { return u32v(static_cast<u32>(v)); }
    Args& u64v(u64 v) {
        if (w_.size() < 4 && (w_.size() & 1)) w_.push_back(0);  // even register pair (r3 is skipped)
        if (w_.size() >= 4 && ((w_.size() - 4) & 1)) w_.push_back(0);  // 8-byte aligned stack slot
        w_.push_back(static_cast<u32>(v));
        w_.push_back(static_cast<u32>(v >> 32));
        return *this;
    }
    Args& f32v(float f) {
        u32 b;
        memcpy(&b, &f, 4);
        return u32v(b);
    }
    Args& f64v(double d) {
        u64 b;
        memcpy(&b, &d, 8);
        return u64v(b);
    }
    const std::vector<u32>& words() const { return w_; }

private:
    std::vector<u32> w_;
};

// Calls guest function `fn` (bit 0 selects Thumb). Returns r0 | r1 << 32.
u64 call(gaddr fn, const u32* words, size_t nwords);
inline u64 call(gaddr fn, const Args& a) { return call(fn, a.words().data(), a.words().size()); }
inline u64 call(gaddr fn) { return call(fn, nullptr, 0); }

// Thunks.
gaddr register_thunk(std::string_view name, ThunkFn fn);
gaddr make_thunk(ThunkFn fn, std::string_view debug_name);  // anonymous thunk
gaddr find_thunk(std::string_view name);
void register_data(std::string_view name, gaddr addr);
gaddr find_data(std::string_view name);
gaddr missing_import_stub(std::string_view name);
const char* thunk_name_for_addr(gaddr a);

// Invalidate translated code in every JIT (after guest code changes).
void invalidate_code(gaddr addr, u32 len);

// Thumb `svc #imm8` numbers [kSvcHookBase, kSvcHookBase + 128) belong to entry hooks.
constexpr u32 kSvcHookBase = 0x80;

// Entry hook on a Thumb function (bit 0 of `fn` set) that starts with a 16-bit
// `push`. That instruction is overwritten with an `svc`; when it executes, the
// push is emulated and `hook` runs with the guest registers as they are at the
// function entry (arguments in r0-r3). The hook may call guest code. The code
// must still be writable: call it from an elf::add_patch_hook callback.
bool install_thumb_entry_hook(gaddr fn, ThunkFn hook, const char* name);

// Replace hook on a Thumb function (bit 0 of `fn` set): its first 16-bit instruction becomes an `svc`, and the
// handler runs instead of the function. It must say where execution continues with continue_at() (the return
// address in lr for a plain return, or another function for a tail call). Same writability rule as above.
bool install_thumb_replace_hook(gaddr fn, ThunkFn hook, const char* name);
// From a hook: continue the guest at `target` (bit 0 selects Thumb).
void continue_at(JitCtx& j, gaddr target);
// From an entry hook: return to the caller right away instead of running the function (set r0/r1 first).
void skip_function(JitCtx& j);

// Logs registers and a short backtrace of the current guest frame.
void dump_state(JitCtx& j, const char* why);
// Logs where every guest thread currently is (diagnostics for hangs; racy).
void dump_all_threads(const char* why);

// Profiling (debug builds of the property debug.acp64.verbose only):
// logs the thunks (imported functions) and memory callbacks used during the last interval;
void log_call_stats(double seconds);
// writes the compiled-block map of the calling thread's JITs (host entry -> guest pc) and the thunk table.
void profile_dump(const char* path);
void dump_own_map_if_requested(GuestThread* t);
// Profiling: time the calling guest thread spent blocked in a host wait (futex, sleep), charged to the guest code
// that called the current import (its return address).
void note_wait(const char* what, int64_t ns);

// Maps a guest code address to "symbol+offset" (provided by the ELF loader).
using Symbolizer = std::string (*)(gaddr);
void set_symbolizer(Symbolizer s);
std::string symbolize(gaddr a);

// Called for `svc #0` executed by guest code (raw Linux EABI syscalls).
void set_syscall_handler(ThunkFn fn);

// Hooks run when a guest-created thread finishes (e.g. JNI detach).
using ThreadExitHook = void (*)(GuestThread*);
void add_thread_exit_hook(ThreadExitHook h);
void run_thread_exit_hooks(GuestThread* t);
// Hooks run when any host thread that ran guest code ends (pthread key destructor: thread_local objects are
// already destroyed when they run).
void add_thread_destroy_hook(ThreadExitHook h);

}  // namespace cpu

struct JitCtx;

struct GuestThread {
    u32 id = 0;           // small unique id (mutex ownership), 1..65535
    gaddr tls = 0;        // guest TLS block; also the guest pthread_t value
    gaddr stack_lo = 0;   // guest stack [lo, hi)
    gaddr stack_hi = 0;
    u32 stack_size = 0;
    std::vector<std::unique_ptr<JitCtx>> pool;
    int depth = 0;
    bool exiting = false;  // pthread_exit requested
    u32 exit_value = 0;
    std::array<u32, 128> keys{};  // pthread_getspecific values
    void* host_env = nullptr;      // JNIEnv* when attached
    bool jni_attached_by_us = false;  // the runtime attached this thread to the VM (detached when it exits)
    gaddr guest_env = 0;           // guest JNIEnv*
    bool owns_stack = true;
    u32 dump_seen = 0;             // profiling: last block-map dump request handled

    JitCtx* top();  // innermost active context, or nullptr
};

// TLS block layout (guest memory, 4 KiB per thread).
namespace tls {
constexpr u32 kErrno = 0x000;
constexpr u32 kSelf = 0x004;
constexpr u32 kJniEnv = 0x008;    // guest JNIEnv (pointer to the function table)
constexpr u32 kTm = 0x010;        // struct tm (44 bytes)
constexpr u32 kStrerror = 0x040;  // 256 bytes
constexpr u32 kNtoa = 0x140;      // 32 bytes
constexpr u32 kStrsignal = 0x160;  // 64 bytes
constexpr u32 kPasswd = 0x1A0;    // struct passwd (24 bytes) + strings
constexpr u32 kScratch = 0x400;   // 3 KiB general scratch
constexpr u32 kSize = 0x1000;
}  // namespace tls

struct JitCtx {
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    GuestThread* thread = nullptr;
    u32 proc_id = 0;
    bool returned = false;
    gaddr cur_thunk = 0;  // for diagnostics
    bool skip_function = false;  // set by an entry hook that returns to the caller itself
    u32 tpidruro = 0;
    u32 tpidrurw = 0;

    std::array<u32, 16>& r();
    u32 sp();

    // Reads the i-th 32-bit argument word (r0-r3, then the stack).
    u32 arg(unsigned i);
    void set_ret(u32 v);
    void set_ret64(u64 v);
    void set_errno(int e);
    int guest_errno();
};

}  // namespace zb
