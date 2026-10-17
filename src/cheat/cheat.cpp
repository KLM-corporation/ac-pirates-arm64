#include "cheat.h"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "../cpu.h"
#include "../elf_loader.h"
#include "../mem.h"
#include "cheat_lua.h"

namespace zb::cheat {
namespace {

constexpr i32 kGlobalsIndex = -10002;  // LUA_GLOBALSINDEX (Lua 5.1 / LuaJIT 2.0)
constexpr int64_t kApplyEveryMs = 250;
constexpr int64_t kProbePollMs = 1000;

struct LuaApi {
    gaddr gettop = 0, settop = 0, checkstack = 0, loadbuffer = 0, pcall = 0, tolstring = 0, pushcclosure = 0,
          setfield = 0, toboolean = 0;
    bool ok() const {
        return gettop && settop && checkstack && loadbuffer && pcall && tolstring && pushcclosure && setfield &&
               toboolean;
    }
};

LuaApi g_api;
gaddr g_log_fn = 0;  // guest-callable C function acp64_log(string)
gaddr g_profdump_fn = 0;  // acp64_profdump(path): JIT block map of the calling thread, for profiling
std::string g_dir;        // debug probe directory
std::string g_state_dir;  // private state directory

std::mutex g_lock;  // one Lua chunk of ours at a time
thread_local bool t_busy = false;
std::atomic<u64> g_calls{0};
std::atomic<int64_t> g_probe_checked{0};
std::atomic<int64_t> g_applied{0};
std::atomic<bool> g_infinite{false};
// Queen Anne's Revenge: once the code was typed (and remembered in the state directory), the game thread makes sure the
// ship is owned about once a second. The state tells who reports the first grant to the player.
enum QueenState { kQueenIdle = 0, kQueenWaiting = 1, kQueenDeferred = 2 };
std::atomic<bool> g_queen_on{false};
std::atomic<int> g_queen_state{kQueenIdle};
std::atomic<int64_t> g_queen_tried{0};
std::atomic<bool> g_queen_owned{false};
Notifier g_notifier = nullptr;
std::unordered_set<u32> g_helper_states;  // lua_States that got acp64_log (guarded by g_lock)

FILE* g_out = nullptr;  // probe output (guarded by g_lock)
struct timespec g_probe_mtime{};

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

u32 gcall(gaddr fn, std::initializer_list<u32> words) {
    cpu::Args args;
    for (u32 w : words) args.u32v(w);
    return static_cast<u32>(cpu::call(fn, args));
}

std::string lua_str(u32 L, i32 idx) {
    thread_local gaddr len_slot = mem::gcalloc(1, 4);
    gaddr s = gcall(g_api.tolstring, {L, static_cast<u32>(idx), len_slot});
    if (!s) return {};
    return std::string(hp<const char>(s), rd<u32>(len_slot));
}

// Runs a chunk on `L`, leaving the Lua stack as it was. Returns the error text, empty on success. With `result`, the
// chunk's first return value is read as a boolean.
std::string run_chunk(u32 L, const std::string& source, const char* chunk_name, bool* result = nullptr) {
    gaddr code = mem::gdup(source.data(), static_cast<u32>(source.size()));
    gaddr name = mem::gstrdup(chunk_name);
    u32 top = gcall(g_api.gettop, {L});
    gcall(g_api.checkstack, {L, 8});
    u32 rc = gcall(g_api.loadbuffer, {L, code, static_cast<u32>(source.size()), name});
    if (rc == 0) rc = gcall(g_api.pcall, {L, 0, result ? 1u : 0u, 0});
    std::string err;
    if (rc != 0) {
        err = lua_str(L, -1);
        if (err.empty()) err = "error " + std::to_string(rc);
    } else if (result) {
        *result = gcall(g_api.toboolean, {L, static_cast<u32>(-1)}) != 0;
    }
    gcall(g_api.settop, {L, top});
    mem::gfree(code);
    mem::gfree(name);
    return err;
}

bool read_file(const std::string& path, std::string* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    out->clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    fclose(f);
    return true;
}

// acp64_log(s): appends a line to <data dir>/probe.out; lines starting with '!' also go to logcat.
void lua_log(JitCtx& j) {
    u32 L = j.r()[0];
    std::string s = lua_str(L, 1);
    for (char& c : s) {
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t') c = '?';
    }
    if (g_out) {
        fwrite(s.data(), 1, s.size(), g_out);
        fputc('\n', g_out);
    }
    if (!s.empty() && s[0] == '!') ZLOGI("lua: %s", s.c_str() + 1);
    j.set_ret(0);
}

void lua_profdump(JitCtx& j) {
    std::string path = lua_str(j.r()[0], 1);
    if (!path.empty()) cpu::profile_dump(path.c_str());
    j.set_ret(0);
}

void ensure_helpers(u32 L) {
    if (!g_helper_states.insert(L).second) return;
    gcall(g_api.checkstack, {L, 4});
    for (auto [fn, global] : {std::pair{g_log_fn, "acp64_log"}, std::pair{g_profdump_fn, "acp64_profdump"}}) {
        gcall(g_api.pushcclosure, {L, fn, 0});
        gaddr name = mem::gstrdup(global);
        gcall(g_api.setfield, {L, static_cast<u32>(kGlobalsIndex), name});
        mem::gfree(name);
    }
}

// <data dir>/probe.lua is run whenever it changes (only with the property debug.acp64.verbose=1).
void run_probe_if_changed(u32 L) {
    std::string path = g_dir + "/probe.lua";
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return;
    if (st.st_mtim.tv_sec == g_probe_mtime.tv_sec && st.st_mtim.tv_nsec == g_probe_mtime.tv_nsec) return;
    g_probe_mtime = st.st_mtim;
    std::string source;
    if (!read_file(path, &source)) return;
    std::string out_path = g_dir + "/probe.out";
    if (g_out) fclose(g_out);
    g_out = fopen(out_path.c_str(), "w");
    ZLOGI("probe.lua changed (%zu bytes): running on lua_State 0x%08x", source.size(), L);
    int64_t t0 = now_ms();
    std::string err = run_chunk(L, source, "=probe");
    if (!err.empty()) {
        ZLOGW("probe.lua failed: %s", err.c_str());
        if (g_out) fprintf(g_out, "ERROR: %s\n", err.c_str());
    }
    ZLOGI("probe.lua finished in %lld ms", static_cast<long long>(now_ms() - t0));
    if (g_out) fflush(g_out);
}

// Debug statistics: which threads and Lua states call lua_pcall.
std::mutex g_stats_lock;
std::map<std::pair<u32, u32>, u64> g_stats;
int64_t g_stats_printed = 0;

void note_call(JitCtx& j, u32 L, int64_t now) {
    std::lock_guard lk(g_stats_lock);
    g_stats[{j.thread->id, L}]++;
    if (g_stats_printed == 0) g_stats_printed = now;
    if (now - g_stats_printed < 10000) return;
    g_stats_printed = now;
    std::string line;
    for (auto& [k, v] : g_stats) {
        char buf[64];
        snprintf(buf, sizeof(buf), " [thread %u, L=0x%08x: %llu]", k.first, k.second, static_cast<unsigned long long>(v));
        line += buf;
    }
    ZLOGI("lua_pcall (total %llu):%s", static_cast<unsigned long long>(g_calls.load()), line.c_str());
}

// The thread that calls lua_pcall most often runs the game's scripts. Our chunks only run
// there, never on a helper thread that might be using the Lua state at the same time.
std::mutex g_thread_lock;
std::map<u32, u32> g_window_counts;
int64_t g_window_start = 0;
std::atomic<u32> g_main_tid{0};

bool is_main_lua_thread(JitCtx& j, int64_t now) {
    std::lock_guard lk(g_thread_lock);
    g_window_counts[j.thread->id]++;
    if (g_window_start == 0) g_window_start = now;
    if (now - g_window_start >= 5000) {
        u32 best = 0, best_calls = 0;
        for (auto& [tid, calls] : g_window_counts) {
            if (calls > best_calls) {
                best = tid;
                best_calls = calls;
            }
        }
        if (best_calls >= 20) g_main_tid = best;
        g_window_counts.clear();
        g_window_start = now;
    }
    return g_main_tid.load() == j.thread->id;
}

struct StartupChunk {
    std::string name;
    std::string source;
};
std::vector<StartupChunk> g_startup_chunks;  // set before the game starts, then only read
std::atomic<bool> g_startup_done{false};
struct PeriodicChunk {
    std::string name;
    std::string source;
    int period_ms;
    int64_t last = 0;   // guarded by g_lock
    int failures = 0;
};
std::vector<PeriodicChunk> g_periodic_chunks;  // set before the game starts

void run_startup_chunks(u32 L) {
    for (const auto& c : g_startup_chunks) {
        std::string err = run_chunk(L, c.source, ("=" + c.name).c_str());
        if (err.empty()) {
            ZLOGI("startup chunk %s done", c.name.c_str());
        } else {
            ZLOGW("startup chunk %s failed: %s", c.name.c_str(), err.c_str());
        }
    }
}

void tick(JitCtx& j, u32 L) {
    if (t_busy) return;
    int64_t now = now_ms();
    if (g_debug) note_call(j, L, now);
    if (!is_main_lua_thread(j, now)) return;
    if (!g_startup_done.load(std::memory_order_relaxed)) {
        std::unique_lock lk(g_lock, std::try_to_lock);
        if (lk && !g_startup_done.load()) {
            t_busy = true;
            run_startup_chunks(L);
            t_busy = false;
            g_startup_done = true;
        }
    }
    for (auto& c : g_periodic_chunks) {
        if (now - c.last < c.period_ms || c.failures >= 5) continue;
        std::unique_lock lk(g_lock, std::try_to_lock);
        if (!lk) break;
        c.last = now;
        t_busy = true;
        std::string err = run_chunk(L, c.source, ("=" + c.name).c_str());
        t_busy = false;
        if (!err.empty()) {
            c.failures++;
            ZLOGW("periodic chunk %s failed: %s", c.name.c_str(), err.c_str());
        }
    }
    bool probe_due = g_debug && !g_dir.empty() && now - g_probe_checked.load() >= kProbePollMs;
    bool apply_due = g_infinite.load() && now - g_applied.load() >= kApplyEveryMs;
    int64_t queen_period = g_queen_state.load() != kQueenIdle ? kApplyEveryMs : 1000;
    bool queen_due = g_queen_on.load() && now - g_queen_tried.load() >= queen_period;
    if (!probe_due && !apply_due && !queen_due) return;
    std::unique_lock lk(g_lock, std::try_to_lock);
    if (!lk) return;
    t_busy = true;
    if (g_debug) ensure_helpers(L);
    if (probe_due) {
        g_probe_checked = now;
        run_probe_if_changed(L);
    }
    if (apply_due) {
        g_applied = now;
        std::string err = run_chunk(L, kCheatLua, "=cheat");
        static int reported = 0;
        if (!err.empty() && reported++ < 5) ZLOGW("cheat chunk failed: %s", err.c_str());
    }
    if (queen_due) {
        g_queen_tried = now;
        bool owned = false;
        std::string err = run_chunk(L, kQueenLua, "=queen", &owned);
        static int reported = 0;
        if (!err.empty() && reported++ < 5) ZLOGW("queen chunk failed: %s", err.c_str());
        if (owned) {
            if (!g_queen_owned.exchange(true)) ZLOGI("cheat: Queen Anne's Revenge is owned");
            // Tell the player once. If the UI thread is still waiting it sees the state change and reports it.
            if (g_queen_state.exchange(kQueenIdle) == kQueenDeferred && g_notifier) g_notifier(kEventQueenGranted);
        } else {
            g_queen_owned = false;
        }
    }
    t_busy = false;
}

// Entry of lua_pcall(L, nargs, nresults, errfunc): r0 = L.
void on_pcall(JitCtx& j) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    tick(j, j.r()[0]);
}

void patch_module(elf::Module* m) {
    if (m->name != "libSparkApp.so") return;
    auto find = [&](const char* sym) -> gaddr {
        auto it = m->exports.find(sym);
        return it == m->exports.end() ? 0 : it->second;
    };
    g_api.gettop = find("lua_gettop");
    g_api.settop = find("lua_settop");
    g_api.checkstack = find("lua_checkstack");
    g_api.loadbuffer = find("luaL_loadbuffer");
    g_api.pcall = find("lua_pcall");
    g_api.tolstring = find("lua_tolstring");
    g_api.pushcclosure = find("lua_pushcclosure");
    g_api.setfield = find("lua_setfield");
    g_api.toboolean = find("lua_toboolean");
    if (!g_api.ok()) {
        ZLOGW("cheat: the game library does not export the Lua API; trainer disabled");
        return;
    }
    if (!cpu::install_thumb_entry_hook(g_api.pcall, &on_pcall, "lua_pcall")) {
        ZLOGW("cheat: cannot hook lua_pcall; trainer disabled");
        g_api = {};
        return;
    }
    ZLOGI("cheat: hooked lua_pcall at 0x%08x", g_api.pcall & ~1u);
}

}  // namespace

void init() {
    g_log_fn = cpu::make_thunk(&lua_log, "acp64_log");
    g_profdump_fn = cpu::make_thunk(&lua_profdump, "acp64_profdump");
    elf::add_patch_hook(&patch_module);
}

void add_startup_lua(const std::string& name, const std::string& source) {
    g_startup_chunks.push_back({name, source});
}

void add_periodic_lua(const std::string& name, const std::string& source, int period_ms) {
    g_periodic_chunks.push_back({name, source, period_ms});
}

void set_data_dir(const std::string& dir) { g_dir = dir; }

void set_state_dir(const std::string& dir) {
    g_state_dir = dir;
    struct stat st{};
    if (stat((dir + "/infinite_resources").c_str(), &st) == 0) {
        g_infinite = true;
        ZLOGI("cheat: infinite resources are on (restored)");
    }
    if (stat((dir + "/queen_anne").c_str(), &st) == 0) {
        g_queen_on = true;
        ZLOGI("cheat: Queen Anne's Revenge is unlocked (restored)");
    }
}

void set_notifier(Notifier notifier) { g_notifier = notifier; }

void set_infinite_resources(bool on) {
    g_infinite = on;
    if (g_state_dir.empty()) return;
    std::string marker = g_state_dir + "/infinite_resources";
    if (on) {
        if (FILE* f = fopen(marker.c_str(), "w")) fclose(f);
    } else {
        remove(marker.c_str());
    }
}

bool infinite_resources() { return g_infinite.load(); }

int submit_code(const std::string& raw) {
    std::string code;
    for (char c : raw) {
        if (!isspace(static_cast<unsigned char>(c))) code += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    if (code == "queen") {
        if (!g_state_dir.empty()) {
            if (FILE* f = fopen((g_state_dir + "/queen_anne").c_str(), "w")) fclose(f);
        }
        g_queen_state = kQueenWaiting;
        g_queen_tried = 0;
        g_queen_on = true;
        ZLOGI("cheat: Queen Anne's Revenge requested");
        // The game thread grants it at its next Lua call, normally within a second. While the player is still on
        // the main menu there is no game loaded yet: then it is granted later and the notifier tells the player.
        for (int i = 0; i < 40 && g_queen_state.load() == kQueenWaiting; ++i) usleep(50 * 1000);
        int expected = kQueenWaiting;
        if (g_queen_state.compare_exchange_strong(expected, kQueenDeferred)) return 4;
        return 3;
    }
    if (code != "klm") {
        ZLOGI("cheat: unknown code");
        return 0;
    }
    bool on = !g_infinite.load();
    set_infinite_resources(on);
    ZLOGI("cheat: infinite resources %s", on ? "on" : "off");
    return on ? 1 : 2;
}

}  // namespace zb::cheat
