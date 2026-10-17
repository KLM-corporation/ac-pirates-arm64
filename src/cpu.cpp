#include "cpu.h"

#include <pthread.h>
#include <sys/system_properties.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include "mem.h"

namespace zb {

using Dynarmic::A32::CoprocReg;

namespace {

constexpr size_t kMaxProcessors = 512;
constexpr size_t kMaxThunks = 16384;
constexpr u32 kThunkAreaSize = kMaxThunks * 8;
// Guest stack for host threads that call into guest code (UI thread, audio,
// Java threads). Reserved lazily, so it can be generous.
constexpr u32 kGuestStackSize = 8 * 1024 * 1024;
constexpr u32 kStackGuard = 64 * 1024;

struct ThunkInfo {
    ThunkFn fn = nullptr;
    std::string name;
};

std::unique_ptr<Dynarmic::ExclusiveMonitor> g_monitor;
std::mutex g_proc_lock;
std::vector<u32> g_free_procs;

std::array<ThunkInfo, kMaxThunks> g_thunks;
// Debug statistics (only updated when g_debug is set).
std::array<std::atomic<u32>, kMaxThunks> g_thunk_calls{};
std::array<std::atomic<u16>, kMaxThunks> g_thunk_last_thread{};
enum MemStat { kMemRead, kMemWrite, kMemExclusive, kMemStatCount };
std::array<std::atomic<u64>, kMemStatCount> g_mem_calls{};
std::atomic<u32> g_dump_generation{0};
std::mutex g_dump_lock;
std::string g_dump_prefix;
std::atomic<u32> g_thunk_count{0};
gaddr g_thunk_area = 0;
gaddr g_return_trap = 0;
std::mutex g_reg_lock;
std::unordered_map<std::string, u32> g_thunk_by_name;
std::unordered_map<std::string, gaddr> g_data_by_name;
std::unordered_map<std::string, gaddr> g_missing;

cpu::Symbolizer g_symbolizer = nullptr;
ThunkFn g_syscall_handler = nullptr;

constexpr u32 kMaxHooks = 128;
struct HookInfo {
    ThunkFn fn = nullptr;
    u16 orig = 0;  // the displaced 16-bit `push` (entry hooks)
    bool replace = false;  // replace hook: the handler decides where execution continues
    const char* name = "";
};
std::array<HookInfo, kMaxHooks> g_hooks;
u32 g_hook_count = 0;

// Emulates a 16-bit Thumb `push {reglist[, lr]}`.
void emulate_push16(JitCtx& j, u16 insn) {
    auto& r = j.r();
    u32 list = insn & 0xFF;
    bool lr = insn & 0x100;
    u32 sp = r[13] - 4 * (static_cast<u32>(__builtin_popcount(list)) + (lr ? 1u : 0u));
    r[13] = sp;
    for (unsigned i = 0; i < 8; ++i) {
        if (list & (1u << i)) {
            wr<u32>(sp, r[i]);
            sp += 4;
        }
    }
    if (lr) wr<u32>(sp, r[14]);
}

std::mutex g_threads_lock;
std::vector<JitCtx*> g_all_ctx;  // for cache invalidation
std::atomic<u32> g_next_thread_id{1};

pthread_key_t g_thread_key;
thread_local GuestThread* t_thread = nullptr;

u32 alloc_proc_id() {
    std::lock_guard lk(g_proc_lock);
    if (g_free_procs.empty()) fatal("out of JIT processor slots");
    u32 id = g_free_procs.back();
    g_free_procs.pop_back();
    return id;
}

void free_proc_id(u32 id) {
    std::lock_guard lk(g_proc_lock);
    g_free_procs.push_back(id);
}

// Address of the last instruction word fetched for translation on this thread.
thread_local gaddr t_last_fetch = 0;

// Coprocessor instructions. CP15 provides the TLS registers and legacy
// barriers. Anything else is compiled into a trap that reports the guest
// instruction if it is ever executed: Dynarmic cannot compile a coprocessor
// exception, and translated-but-unreached code must not abort the process.
class Coproc final : public Dynarmic::A32::Coprocessor {
public:
    Coproc(JitCtx* ctx, unsigned cp) : ctx_(ctx), cp_(cp) {}

    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override {
        return trap("CDP");
    }

    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override {
        if (cp_ == 15 && !two && opc1 == 0) {
            if (CRn == CoprocReg::C7) return Callback{&Noop, std::nullopt};  // legacy barriers
            if (CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc2 == 2) return &ctx_->tpidrurw;
        }
        return trap("MCR");
    }

    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override { return trap("MCRR"); }

    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override {
        if (cp_ == 15 && !two && opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0) {
            if (opc2 == 2) return &ctx_->tpidrurw;
            if (opc2 == 3) return &ctx_->tpidruro;
        }
        return trap("MRC");
    }

    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, CoprocReg) override { return trap("MRRC"); }

    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override {
        return trap("LDC");
    }

    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override {
        return trap("STC");
    }

private:
    struct TrapInfo {
        JitCtx* ctx;
        unsigned cp;
        const char* kind;
        gaddr pc;
    };

    Callback trap(const char* kind) {
        gaddr pc = t_last_fetch;
        ZLOGW("translated unsupported coprocessor instruction %s p%u near 0x%08x (%s)", kind, cp_, pc,
              cpu::symbolize(pc).c_str());
        return Callback{&Trap, new TrapInfo{ctx_, cp_, kind, pc}};
    }

    static std::uint64_t Trap(void* user, std::uint32_t, std::uint32_t) {
        auto* ti = static_cast<TrapInfo*>(user);
        cpu::dump_state(*ti->ctx, "coprocessor instruction");
        fatal("guest executed unsupported coprocessor instruction %s p%u at 0x%08x", ti->kind, ti->cp, ti->pc);
    }

    static std::uint64_t Noop(void*, std::uint32_t, std::uint32_t) { return 0; }
    JitCtx* ctx_;
    unsigned cp_;
};

[[noreturn]] void memory_fault(JitCtx& j, gaddr a, bool write, unsigned size) {
    ZLOGE("guest %s fault: %u bytes at 0x%08x", write ? "write" : "read", size, a);
    cpu::dump_state(j, "memory fault");
    fatal("guest memory fault at 0x%08x", a);
}

class Callbacks final : public Dynarmic::A32::UserCallbacks {
public:
    explicit Callbacks(JitCtx* ctx) : ctx_(ctx) {}

    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A32::VAddr a) override {
        t_last_fetch = a;
        if (!mem::is_mapped(a)) return std::nullopt;
        return rd<u32>(a);
    }

    std::uint8_t MemoryRead8(Dynarmic::A32::VAddr a) override { return read<u8>(a); }
    std::uint16_t MemoryRead16(Dynarmic::A32::VAddr a) override { return read<u16>(a); }
    std::uint32_t MemoryRead32(Dynarmic::A32::VAddr a) override { return read<u32>(a); }
    std::uint64_t MemoryRead64(Dynarmic::A32::VAddr a) override { return read<u64>(a); }
    void MemoryWrite8(Dynarmic::A32::VAddr a, std::uint8_t v) override { write(a, v); }
    void MemoryWrite16(Dynarmic::A32::VAddr a, std::uint16_t v) override { write(a, v); }
    void MemoryWrite32(Dynarmic::A32::VAddr a, std::uint32_t v) override { write(a, v); }
    void MemoryWrite64(Dynarmic::A32::VAddr a, std::uint64_t v) override { write(a, v); }

    bool MemoryWriteExclusive8(Dynarmic::A32::VAddr a, std::uint8_t v, std::uint8_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive16(Dynarmic::A32::VAddr a, std::uint16_t v, std::uint16_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive32(Dynarmic::A32::VAddr a, std::uint32_t v, std::uint32_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive64(Dynarmic::A32::VAddr a, std::uint64_t v, std::uint64_t e) override { return cas(a, v, e); }

    bool IsReadOnlyMemory(Dynarmic::A32::VAddr a) override { return mem::is_readonly(a); }

    void InterpreterFallback(Dynarmic::A32::VAddr pc, size_t n) override {
        cpu::dump_state(*ctx_, "interpreter fallback");
        fatal("dynarmic interpreter fallback at 0x%08x (%zu instructions)", pc, n);
    }

    void CallSVC(std::uint32_t swi) override;

    void ExceptionRaised(Dynarmic::A32::VAddr pc, Dynarmic::A32::Exception e) override {
        using E = Dynarmic::A32::Exception;
        switch (e) {
        case E::SendEvent:
        case E::SendEventLocal:
        case E::WaitForInterrupt:
        case E::WaitForEvent:
        case E::Yield:
        case E::PreloadData:
        case E::PreloadDataWithIntentToWrite:
        case E::PreloadInstruction:
            return;
        default:
            break;
        }
        static const char* names[] = {"UndefinedInstruction", "UnpredictableInstruction", "DecodeError", "SendEvent",
                                      "SendEventLocal", "WaitForInterrupt", "WaitForEvent", "Yield", "Breakpoint",
                                      "PreloadData", "PreloadDataWithIntentToWrite", "PreloadInstruction",
                                      "NoExecuteFault"};
        unsigned idx = static_cast<unsigned>(e);
        const char* name = idx < std::size(names) ? names[idx] : "?";
        ZLOGE("guest exception %s at pc=0x%08x (%s)", name, pc, cpu::symbolize(pc).c_str());
        cpu::dump_state(*ctx_, name);
        fatal("guest exception %s", name);
    }

    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return ~0ull; }

private:
    template <typename T>
    T read(gaddr a) {
        if (g_debug) g_mem_calls[kMemRead].fetch_add(1, std::memory_order_relaxed);
        if (!mem::is_mapped(a) || !mem::is_mapped(a + sizeof(T) - 1)) memory_fault(*ctx_, a, false, sizeof(T));
        return rd<T>(a);
    }

    template <typename T>
    void write(gaddr a, T v) {
        if (g_debug) g_mem_calls[kMemWrite].fetch_add(1, std::memory_order_relaxed);
        if (!mem::is_mapped(a) || !mem::is_mapped(a + sizeof(T) - 1)) memory_fault(*ctx_, a, true, sizeof(T));
        wr<T>(a, v);
    }

    template <typename T>
    bool cas(gaddr a, T v, T expected) {
        if (g_debug) g_mem_calls[kMemExclusive].fetch_add(1, std::memory_order_relaxed);
        if (!mem::is_mapped(a)) memory_fault(*ctx_, a, true, sizeof(T));
        return __atomic_compare_exchange_n(hp<T>(a), &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }

    JitCtx* ctx_;
};

struct CtxStorage {
    std::unique_ptr<Callbacks> cb;
    std::array<std::shared_ptr<Coproc>, 16> cps;
};
std::unordered_map<JitCtx*, CtxStorage> g_storage;  // guarded by g_threads_lock

bool g_fastmem = true;

std::unique_ptr<JitCtx> make_ctx(GuestThread* t, int depth) {
    auto ctx = std::make_unique<JitCtx>();
    ctx->thread = t;
    ctx->proc_id = alloc_proc_id();
    ctx->tpidruro = t->tls;
    CtxStorage st;
    st.cb = std::make_unique<Callbacks>(ctx.get());
    for (unsigned i = 0; i < 16; ++i) {
        if (i != 10 && i != 11) st.cps[i] = std::make_shared<Coproc>(ctx.get(), i);  // 10/11 are VFP
    }

    Dynarmic::A32::UserConfig c;
    c.callbacks = st.cb.get();
    c.processor_id = ctx->proc_id;
    c.global_monitor = g_monitor.get();
    c.arch_version = Dynarmic::A32::ArchVersion::v7;
    c.optimizations = Dynarmic::all_safe_optimizations | Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP |
                      Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN |
                      Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
    c.unsafe_optimizations = true;
    c.page_table = mem::page_table();
    if (g_fastmem) {
        // Guest memory is identity-mapped in the low 4 GiB: a guest address is the host address. Loads and stores
        // become single instructions; an access that faults (unmapped page) is recompiled to use the page table.
        c.fastmem_pointer = 0;
        c.recompile_on_fastmem_failure = true;
    }
    for (unsigned i = 0; i < 16; ++i) c.coprocessors[i] = st.cps[i];
    c.define_unpredictable_behaviour = true;
    c.enable_cycle_counting = false;
    c.always_little_endian = true;
    c.code_cache_size = depth == 0 ? 128u * 1024 * 1024 : 32u * 1024 * 1024;
    ctx->jit = std::make_unique<Dynarmic::A32::Jit>(c);

    std::lock_guard lk(g_threads_lock);
    g_storage.emplace(ctx.get(), std::move(st));
    g_all_ctx.push_back(ctx.get());
    return ctx;
}

void destroy_ctx(JitCtx* ctx) {
    {
        std::lock_guard lk(g_threads_lock);
        std::erase(g_all_ctx, ctx);
    }
    ctx->jit.reset();
    {
        std::lock_guard lk(g_threads_lock);
        g_storage.erase(ctx);
    }
    free_proc_id(ctx->proc_id);
}

std::vector<cpu::ThreadExitHook> g_destroy_hooks;

void thread_destructor(void* p) {
    auto* t = static_cast<GuestThread*>(p);
    // C++ thread_local objects are already gone here: these hooks only use the GuestThread.
    for (cpu::ThreadExitHook h : g_destroy_hooks) h(t);
    for (auto& c : t->pool) destroy_ctx(c.get());
    t->pool.clear();
    if (t->owns_stack && t->stack_lo) mem::unmap_anon(t->stack_lo, t->stack_hi - t->stack_lo);
    if (t->tls) mem::unmap_anon(t->tls, tls::kSize);
    delete t;
}

gaddr write_thunk_code(u32 index) {
    gaddr a = g_thunk_area + index * 8;
    wr<u32>(a, 0xEF000000u | (cpu::kSvcThunkBase + index));  // svc #imm24
    wr<u32>(a + 4, 0xE12FFF1Eu);                              // bx lr
    return a;
}

}  // namespace

void Callbacks::CallSVC(std::uint32_t swi) {
    JitCtx& j = *ctx_;
    if (swi == cpu::kSvcReturn) {
        j.returned = true;
        j.jit->HaltExecution();
        return;
    }
    if (swi >= cpu::kSvcThunkBase) {
        u32 idx = swi - cpu::kSvcThunkBase;
        if (idx < g_thunk_count.load(std::memory_order_acquire)) {
            if (g_debug) {
                cpu::dump_own_map_if_requested(j.thread);
                g_thunk_calls[idx].fetch_add(1, std::memory_order_relaxed);
                g_thunk_last_thread[idx].store(static_cast<u16>(j.thread->id), std::memory_order_relaxed);
            }
            j.cur_thunk = g_thunk_area + idx * 8;
            g_thunks[idx].fn(j);
            j.cur_thunk = 0;
            if (j.thread->exiting) j.jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
    }
    if (swi >= cpu::kSvcHookBase && swi < cpu::kSvcHookBase + kMaxHooks) {
        u32 idx = swi - cpu::kSvcHookBase;
        if (idx < g_hook_count) {
            const HookInfo& h = g_hooks[idx];
            if (h.replace) {
                h.fn(j);
                return;
            }
            j.skip_function = false;
            h.fn(j);
            if (j.skip_function) {
                j.skip_function = false;
                cpu::continue_at(j, j.r()[14]);  // back to the caller; the displaced push never ran
            } else {
                emulate_push16(j, h.orig);
            }
            return;
        }
    }
    if (swi == 0 && g_syscall_handler) {
        g_syscall_handler(j);
        return;
    }
    cpu::dump_state(j, "unknown svc");
    fatal("unknown guest svc #0x%x", swi);
}

std::array<u32, 16>& JitCtx::r() { return jit->Regs(); }
u32 JitCtx::sp() { return jit->Regs()[13]; }

u32 JitCtx::arg(unsigned i) {
    auto& regs = jit->Regs();
    if (i < 4) return regs[i];
    return rd<u32>(regs[13] + 4 * (i - 4));
}

void JitCtx::set_ret(u32 v) { jit->Regs()[0] = v; }

void JitCtx::set_ret64(u64 v) {
    jit->Regs()[0] = static_cast<u32>(v);
    jit->Regs()[1] = static_cast<u32>(v >> 32);
}

void JitCtx::set_errno(int e) { wr<i32>(thread->tls + tls::kErrno, e); }
int JitCtx::guest_errno() { return rd<i32>(thread->tls + tls::kErrno); }

JitCtx* GuestThread::top() { return depth > 0 ? pool[depth - 1].get() : nullptr; }

namespace cpu {

void init() {
    {
        char v[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.acp64.fastmem", v) > 0 && v[0] == '0') g_fastmem = false;
        ZLOGI("JIT memory access: %s", g_fastmem ? "direct (fastmem)" : "page table");
    }
    std::set_terminate([] {
        if (GuestThread* t = current_thread_if_any(); t && t->top()) dump_state(*t->top(), "std::terminate");
        ZLOGE("std::terminate on this thread; last translated guest address 0x%08x (%s)", t_last_fetch,
              symbolize(t_last_fetch).c_str());
        abort();
    });
    g_monitor = std::make_unique<Dynarmic::ExclusiveMonitor>(kMaxProcessors);
    for (u32 i = kMaxProcessors; i-- > 0;) g_free_procs.push_back(i);
    pthread_key_create(&g_thread_key, thread_destructor);

    g_thunk_area = mem::map_anon(kThunkAreaSize + 4096, PROT_READ | PROT_WRITE, "zb-thunks");
    if (!g_thunk_area) fatal("cannot allocate the thunk area");
    g_return_trap = g_thunk_area + kThunkAreaSize;
    wr<u32>(g_return_trap, 0xEF000000u | kSvcReturn);
    wr<u32>(g_return_trap + 4, 0xE1200070u);  // bkpt #0 (never reached)
}

GuestThread* current_thread_if_any() { return t_thread; }

GuestThread* new_thread_state(u32 stack_size) {
    auto* t = new GuestThread();
    t->id = g_next_thread_id.fetch_add(1);
    if (t->id > 0xFFFF) fatal("too many guest threads");
    stack_size = (stack_size + 0xFFFF) & ~0xFFFFu;
    t->tls = mem::map_anon(tls::kSize, PROT_READ | PROT_WRITE, "zb-guest-tls");
    // The lowest 64 KiB of the mapping is a guard: overflows fault cleanly.
    t->stack_size = stack_size;
    t->stack_lo = mem::map_anon(stack_size + kStackGuard, PROT_READ | PROT_WRITE, "zb-guest-stack");
    if (!t->tls || !t->stack_lo) fatal("cannot allocate guest thread state");
    int err = 0;
    mem::guest_mprotect(t->stack_lo, kStackGuard, PROT_NONE, &err);
    t->stack_hi = t->stack_lo + kStackGuard + stack_size;
    wr<u32>(t->tls + tls::kSelf, t->tls);
    return t;
}

void adopt_thread(GuestThread* t) {
    t_thread = t;
    pthread_setspecific(g_thread_key, t);
}

GuestThread* current_thread() {
    if (t_thread) return t_thread;
    GuestThread* t = new_thread_state(kGuestStackSize);
    adopt_thread(t);
    return t;
}

u64 call(gaddr fn, const u32* words, size_t nwords) {
    GuestThread* t = current_thread();
    if (t->depth >= 16) fatal("guest call nesting too deep");
    u32 sp;
    if (t->depth == 0) {
        sp = t->stack_hi;
    } else {
        sp = t->pool[t->depth - 1]->jit->Regs()[13] - 256;  // leave room below the caller's frame
    }
    if (static_cast<int>(t->pool.size()) <= t->depth) t->pool.push_back(make_ctx(t, t->depth));
    JitCtx* j = t->pool[t->depth].get();
    t->depth++;

    size_t nstack = nwords > 4 ? nwords - 4 : 0;
    sp = (sp - static_cast<u32>(nstack) * 4) & ~7u;
    for (size_t i = 0; i < nstack; ++i) wr<u32>(sp + 4 * static_cast<u32>(i), words[4 + i]);
    auto& regs = j->jit->Regs();
    for (size_t i = 0; i < 4; ++i) regs[i] = i < nwords ? words[i] : 0;
    regs[13] = sp;
    regs[14] = g_return_trap;
    regs[15] = fn & ~1u;
    j->jit->SetCpsr(0x10u | ((fn & 1) ? 0x20u : 0u));
    j->returned = false;

    while (!j->returned && !t->exiting) j->jit->Run();

    u64 ret = regs[0] | (static_cast<u64>(regs[1]) << 32);
    t->depth--;
    return ret;
}

gaddr register_thunk(std::string_view name, ThunkFn fn) {
    std::lock_guard lk(g_reg_lock);
    std::string key(name);
    if (auto it = g_thunk_by_name.find(key); it != g_thunk_by_name.end()) {
        g_thunks[it->second].fn = fn;  // later registrations override
        return g_thunk_area + it->second * 8;
    }
    u32 idx = g_thunk_count.load();
    if (idx >= kMaxThunks) fatal("too many thunks");
    g_thunks[idx].fn = fn;
    g_thunks[idx].name = key;
    gaddr a = write_thunk_code(idx);
    g_thunk_by_name.emplace(std::move(key), idx);
    g_thunk_count.store(idx + 1, std::memory_order_release);
    return a;
}

gaddr make_thunk(ThunkFn fn, std::string_view debug_name) {
    std::lock_guard lk(g_reg_lock);
    u32 idx = g_thunk_count.load();
    if (idx >= kMaxThunks) fatal("too many thunks");
    g_thunks[idx].fn = fn;
    g_thunks[idx].name = std::string(debug_name);
    gaddr a = write_thunk_code(idx);
    g_thunk_count.store(idx + 1, std::memory_order_release);
    return a;
}

gaddr find_thunk(std::string_view name) {
    std::lock_guard lk(g_reg_lock);
    auto it = g_thunk_by_name.find(std::string(name));
    return it == g_thunk_by_name.end() ? 0 : g_thunk_area + it->second * 8;
}

void register_data(std::string_view name, gaddr addr) {
    std::lock_guard lk(g_reg_lock);
    g_data_by_name[std::string(name)] = addr;
}

gaddr find_data(std::string_view name) {
    std::lock_guard lk(g_reg_lock);
    auto it = g_data_by_name.find(std::string(name));
    return it == g_data_by_name.end() ? 0 : it->second;
}

static void missing_import(JitCtx& j) {
    const char* name = thunk_name_for_addr(j.cur_thunk);
    dump_state(j, "missing import");
    fatal("guest called unimplemented import '%s'", name);
}

gaddr missing_import_stub(std::string_view name) {
    {
        std::lock_guard lk(g_reg_lock);
        if (auto it = g_missing.find(std::string(name)); it != g_missing.end()) return it->second;
    }
    gaddr a = make_thunk(&missing_import, name);
    std::lock_guard lk(g_reg_lock);
    g_missing.emplace(std::string(name), a);
    return a;
}

const char* thunk_name_for_addr(gaddr a) {
    if (a < g_thunk_area || a >= g_thunk_area + kThunkAreaSize) return "?";
    u32 idx = (a - g_thunk_area) / 8;
    return idx < g_thunk_count.load() ? g_thunks[idx].name.c_str() : "?";
}

void invalidate_code(gaddr addr, u32 len) {
    std::lock_guard lk(g_threads_lock);
    for (JitCtx* c : g_all_ctx) c->jit->InvalidateCacheRange(addr, len);
}

bool install_thumb_entry_hook(gaddr fn, ThunkFn hook, const char* name) {
    if (!(fn & 1) || g_hook_count >= kMaxHooks) return false;
    gaddr a = fn & ~1u;
    u16 insn = rd<u16>(a);
    if ((insn & 0xFE00) != 0xB400) {  // push {reglist[, lr]}
        ZLOGW("entry hook %s: unexpected first instruction 0x%04x", name, insn);
        return false;
    }
    g_hooks[g_hook_count] = HookInfo{hook, insn, false, name};
    wr<u16>(a, static_cast<u16>(0xDF00 | (kSvcHookBase + g_hook_count)));  // svc #imm8
    g_hook_count++;
    return true;
}

bool install_thumb_replace_hook(gaddr fn, ThunkFn hook, const char* name) {
    if (!(fn & 1) || g_hook_count >= kMaxHooks) return false;
    gaddr a = fn & ~1u;
    g_hooks[g_hook_count] = HookInfo{hook, rd<u16>(a), true, name};
    wr<u16>(a, static_cast<u16>(0xDF00 | (kSvcHookBase + g_hook_count)));  // svc #imm8
    g_hook_count++;
    return true;
}

void skip_function(JitCtx& j) { j.skip_function = true; }

void continue_at(JitCtx& j, gaddr target) {
    j.r()[15] = target & ~1u;
    u32 cpsr = j.jit->Cpsr();
    j.jit->SetCpsr((target & 1) ? (cpsr | 0x20u) : (cpsr & ~0x20u));
}

void set_symbolizer(Symbolizer s) { g_symbolizer = s; }

std::string symbolize(gaddr a) {
    if (a >= g_thunk_area && a < g_thunk_area + kThunkAreaSize) return std::string("thunk:") + thunk_name_for_addr(a);
    if (g_symbolizer) return g_symbolizer(a);
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08x", a);
    return buf;
}

void set_syscall_handler(ThunkFn fn) { g_syscall_handler = fn; }

static std::vector<ThreadExitHook> g_exit_hooks;

void add_thread_exit_hook(ThreadExitHook h) { g_exit_hooks.push_back(h); }

void add_thread_destroy_hook(ThreadExitHook h) { g_destroy_hooks.push_back(h); }

void run_thread_exit_hooks(GuestThread* t) {
    for (ThreadExitHook h : g_exit_hooks) h(t);
}

void dump_all_threads(const char* why) {
    std::lock_guard lk(g_threads_lock);
    ZLOGW("==== all guest threads (%s) ====", why);
    for (JitCtx* c : g_all_ctx) {
        if (!c->jit->IsExecuting()) continue;
        auto& r = c->jit->Regs();
        ZLOGW("thread %u (depth %d): pc=%s lr=%s thunk=%s r0=%08x r1=%08x r2=%08x", c->thread->id,
              c->thread->depth, symbolize(r[15]).c_str(), symbolize(r[14]).c_str(),
              c->cur_thunk ? thunk_name_for_addr(c->cur_thunk) : "-", r[0], r[1], r[2]);
    }
}

struct WaitKey {
    u32 thread;
    u32 lr;
    const char* what;
    bool operator==(const WaitKey& o) const { return thread == o.thread && lr == o.lr && what == o.what; }
};
struct WaitKeyHash {
    size_t operator()(const WaitKey& k) const { return (static_cast<size_t>(k.thread) << 32) ^ k.lr ^ reinterpret_cast<uintptr_t>(k.what); }
};
struct WaitStat {
    int64_t ns = 0;
    u32 count = 0;
};
std::mutex g_waits_lock;
std::unordered_map<WaitKey, WaitStat, WaitKeyHash> g_waits;

void note_wait(const char* what, int64_t ns) {
    GuestThread* t = current_thread_if_any();
    JitCtx* j = t ? t->top() : nullptr;
    if (!j) return;
    WaitKey k{t->id, j->r()[14] & ~1u, what};
    if (ns > 20000000) {
        ZLOGI("long wait: thread %u, %.1f ms in %s from %s", t->id, ns / 1e6, what, symbolize(k.lr).c_str());
    }
    std::lock_guard lk(g_waits_lock);
    auto& w = g_waits[k];
    w.ns += ns;
    w.count++;
}

void log_call_stats(double seconds) {
    static std::array<u32, kMaxThunks> last{};
    static std::array<u64, kMemStatCount> last_mem{};
    struct Row {
        u32 idx;
        u32 calls;
    };
    std::vector<Row> rows;
    u32 n = g_thunk_count.load(std::memory_order_acquire);
    u64 total = 0;
    for (u32 i = 0; i < n; ++i) {
        u32 now = g_thunk_calls[i].load(std::memory_order_relaxed);
        u32 d = now - last[i];
        last[i] = now;
        total += d;
        if (d) rows.push_back({i, d});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.calls > b.calls; });
    std::string line;
    for (size_t i = 0; i < rows.size() && i < 14; ++i) {
        char buf[96];
        snprintf(buf, sizeof(buf), " %s=%.0f(t%u)", g_thunks[rows[i].idx].name.c_str(), rows[i].calls / seconds,
                 static_cast<unsigned>(g_thunk_last_thread[rows[i].idx].load(std::memory_order_relaxed)));
        line += buf;
    }
    u64 mem[kMemStatCount];
    for (int i = 0; i < kMemStatCount; ++i) {
        u64 now = g_mem_calls[i].load(std::memory_order_relaxed);
        mem[i] = now - last_mem[i];
        last_mem[i] = now;
    }
    ZLOGI("calls/s: thunks %.0f, memory callbacks r=%.0f w=%.0f excl=%.0f |%s", total / seconds, mem[kMemRead] / seconds,
          mem[kMemWrite] / seconds, mem[kMemExclusive] / seconds, line.c_str());
    std::vector<std::pair<WaitKey, WaitStat>> waits;
    {
        std::lock_guard lk(g_waits_lock);
        waits.assign(g_waits.begin(), g_waits.end());
        g_waits.clear();
    }
    std::sort(waits.begin(), waits.end(), [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    std::string wl;
    for (size_t i = 0; i < waits.size() && i < 8; ++i) {
        char buf[200];
        snprintf(buf, sizeof(buf), " [t%u %s x%u %.1f ms/s from %s]", waits[i].first.thread, waits[i].first.what,
                 waits[i].second.count, waits[i].second.ns / 1e6 / seconds, symbolize(waits[i].first.lr).c_str());
        wl += buf;
    }
    if (!wl.empty()) ZLOGI("waits:%s", wl.c_str());
}

void write_thread_map(GuestThread* t, FILE* f) {
    for (auto& c : t->pool) {
        u64 lo = 0, hi = 0;
        auto map = c->jit->GetBlockMap(&lo, &hi);
        fprintf(f, "jit %u %llx %llx %zu %d\n", t->id, static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi),
                map.size(), static_cast<int>(gettid()));
        for (const auto& e : map) {
            fprintf(f, "b %llx %08x %d\n", static_cast<unsigned long long>(e.host_entry), e.guest_pc, e.thumb ? 1 : 0);
        }
    }
}

void profile_dump(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) {
        ZLOGW("profile_dump: cannot open %s", path);
        return;
    }
    GuestThread* t = current_thread_if_any();
    if (t) write_thread_map(t, f);
    u32 n = g_thunk_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < n; ++i) {
        fprintf(f, "t %08x %s %u\n", g_thunk_area + i * 8, g_thunks[i].name.c_str(), g_thunk_calls[i].load());
    }
    fclose(f);
    // Every other guest thread writes its own map (only it may read its JITs) at its next thunk call.
    {
        std::lock_guard lk(g_dump_lock);
        g_dump_prefix = path;
    }
    g_dump_generation.fetch_add(1);
    ZLOGI("profile_dump: thread %u -> %s (other threads follow as %s.t<id>)", t ? t->id : 0, path, path);
}

void dump_own_map_if_requested(GuestThread* t) {
    u32 gen = g_dump_generation.load(std::memory_order_relaxed);
    if (t->dump_seen == gen) return;
    t->dump_seen = gen;
    std::string path;
    {
        std::lock_guard lk(g_dump_lock);
        path = g_dump_prefix + ".t" + std::to_string(t->id);
    }
    if (FILE* f = fopen(path.c_str(), "w")) {
        write_thread_map(t, f);
        fclose(f);
    }
}

void dump_state(JitCtx& j, const char* why) {
    auto& r = j.jit->Regs();
    ZLOGE("---- guest state (%s), thread %u depth %d ----", why, j.thread->id, j.thread->depth);
    for (int i = 0; i < 16; i += 4) {
        ZLOGE("r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x", i, r[i], i + 1, r[i + 1], i + 2, r[i + 2], i + 3, r[i + 3]);
    }
    ZLOGE("cpsr=%08x fpscr=%08x", j.jit->Cpsr(), j.jit->Fpscr());
    ZLOGE("pc: %s", symbolize(r[15]).c_str());
    ZLOGE("lr: %s", symbolize(r[14]).c_str());
    if (j.cur_thunk) ZLOGE("in thunk: %s", thunk_name_for_addr(j.cur_thunk));
    // Heuristic backtrace: words on the stack that point into known code.
    u32 sp = r[13];
    int found = 0;
    for (u32 a = sp & ~3u; a < sp + 4096 && found < 24; a += 4) {
        if (!mem::is_mapped(a)) break;
        u32 v = rd<u32>(a);
        if (!mem::is_readonly(v & ~1u)) continue;
        std::string s = symbolize(v & ~1u);
        if (s.empty() || s[0] == '0') continue;
        ZLOGE("  stack[+0x%03x] %08x %s", a - sp, v, s.c_str());
        found++;
    }
}

}  // namespace cpu
}  // namespace zb
