#include "perf_patches.h"

#include <sys/system_properties.h>
#include <time.h>

#include <atomic>
#include <bit>
#include <cerrno>
#include <cstdlib>
#include <initializer_list>
#include <string>

#include "../cpu.h"
#include "../cheat/cheat.h"
#include "../elf_loader.h"

namespace zb::game {
namespace {

// "debug.acp64.<name>" = 0 turns a patch off (for measurements); anything else, or no property, keeps it on.
bool enabled(const char* name) {
    char key[PROP_NAME_MAX];
    snprintf(key, sizeof(key), "debug.acp64.%s", name);
    char v[PROP_VALUE_MAX] = {};
    return !(__system_property_get(key, v) > 0 && v[0] == '0');
}

// True if the Thumb code at `a` starts with these halfwords.
bool code_is(gaddr a, std::initializer_list<u16> halfwords) {
    for (u16 h : halfwords) {
        if (rd<u16>(a) != h) return false;
        a += 2;
    }
    return true;
}

gaddr export_of(elf::Module* m, const char* name) {
    auto it = m->exports.find(name);
    return it == m->exports.end() ? 0 : it->second;
}

void sleep_us(i64 us) {
    timespec ts{static_cast<time_t>(us / 1000000), static_cast<long>((us % 1000000) * 1000)};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

// IdleCurThread(std::chrono::microseconds) is the engine's sleep, written as a busy loop around clock_gettime and
// sched_yield. android_main idles 16 ms with it between two input polls, which keeps a whole core busy for
// nothing (heat, and less power budget for the game thread). Sleep for real instead.
void idle_cur_thread(JitCtx& j) {
    i64 us = static_cast<i64>(j.r()[0] | (static_cast<u64>(j.r()[1]) << 32));
    if (us > 0) sleep_us(us > 1000000 ? 1000000 : us);
    cpu::continue_at(j, j.r()[14]);
}

// lua_asynchronous_gc_step(L, &running) is the body of the engine's asynchronous Lua GC: while the game thread
// renders (between StartAsyncGarbageCollection and StopAsyncGarbageCollection), a helper thread calls it in a
// loop. It forces a step even when the collector is idle, so a new full GC cycle starts as soon as the previous
// one ends: the collector never rests, and as the game thread's allocations always find it mid-cycle, they keep
// paying GC steps too (about half of the game thread's time under emulation). Paced: when the collector is idle
// and no cycle is due yet (the heap has not grown by the "pause" ratio since the last cycle), do nothing;
// otherwise run the original step.
gaddr g_async_step_loop = 0;  // the game's step loop, which the original function tail-calls
std::atomic<gaddr> g_async_global{0};     // global_State that the GC helper thread works on
std::atomic<int64_t> g_async_seen_ms{0};  // last time the helper thread asked for work

int64_t now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ll + ts.tv_nsec / 1000000;
}

void async_gc_step(JitCtx& j) {
    const gaddr L = j.r()[0];
    const gaddr g = rd<u32>(L + 0x08);         // global_State
    g_async_global.store(g, std::memory_order_relaxed);
    g_async_seen_ms.store(now_ms(), std::memory_order_relaxed);
    const u8 state = rd<u8>(g + 0x1d);          // gc.state (0: paused)
    const u32 total = rd<u32>(g + 0x14);        // gc.total
    const u32 estimate = rd<u32>(g + 0x44);     // gc.estimate: memory in use after the last cycle
    const u32 pause = rd<u32>(g + 0x48);        // gc.pause (percent)
    // A new cycle is due when the heap has grown by the "pause" ratio, as in LuaJIT's own schedule (the game
    // thread's gc.threshold is not used here: lj_gc_step below moves it).
    if (state == 0 && total < static_cast<u64>(estimate) / 100 * pause) {
        // StopAsyncGarbageCollection waits for this call to return: keep it short.
        sleep_us(100);
        j.r()[0] = 0;
        cpu::continue_at(j, j.r()[14]);
        return;
    }
    wr<u32>(g + 0x18, total >= 1024 ? total - 1024 : 0);  // the original's own threshold update
    cpu::continue_at(j, g_async_step_loop);
}

// lj_gc_step(L) is LuaJIT's incremental GC step, run by the game thread whenever it has allocated a little more
// (1 KiB) during a collection. The engine has a helper thread for this work (see above), which works while the
// game thread renders, in parallel. Leave the work to it while the heap stays reasonable: the game thread then
// spends its time on the game instead of on the GC.
void gc_step(JitCtx& j) {
    const gaddr L = j.r()[0];
    const gaddr g = rd<u32>(L + 0x08);
    if (g != g_async_global.load(std::memory_order_relaxed) || now_ms() - g_async_seen_ms.load() > 200) return;
    const u32 total = rd<u32>(g + 0x14);
    const u32 estimate = rd<u32>(g + 0x44);
    const u32 pause = rd<u32>(g + 0x48);
    const u64 limit = static_cast<u64>(estimate) / 100 * pause * 3 / 2;  // 1.5x the normal cycle trigger
    if (total >= limit) return;  // the helper thread does not keep up: collect here too
    wr<u32>(g + 0x18, total + 256 * 1024);  // threshold: no new step before another 256 KiB
    j.r()[0] = static_cast<u32>(-1);         // lj_gc_step: "no cycle finished, no debt left"
    cpu::skip_function(j);
}

// geOctreeNode<geMeshEntity>::GetFrustumIntersection(const geAxisAlignedBox& box, const geVector4* planes,
// float scale) const: 0 when the box (its extent multiplied by scale) is inside the 6 planes of the camera's
// frustum, 2 when it is outside one of them, 1 when it crosses one. The octree calls it for each of its nodes and
// objects, every frame: about 14% of the game thread at 144 fps, mostly spent in the four tiny calls it makes
// (geAxisAlignedBox::GetCenter and GetExtent) and their returns, each a block lookup for the JIT. Same computation
// natively, in the same order and without fused multiply-adds (the game's vmla/vmls round twice), so the results
// are identical.
void frustum_intersection(JitCtx& j) {
#pragma clang fp contract(off)
    const gaddr box = j.r()[1];
    const gaddr planes = j.r()[2];
    const float scale = std::bit_cast<float>(j.r()[3]);  // soft-float ABI: the float argument comes in r3
    float lo[3], hi[3];
    for (int i = 0; i < 3; i++) {
        const float center = rd<float>(box + 4 * i);  // geAxisAlignedBox: center, then half extent
        const float extent = scale * rd<float>(box + 12 + 4 * i);
        lo[i] = center - extent;
        hi[i] = center + extent;
    }
    u32 result = 0;
    for (int k = 0; k < 6; k++) {
        const gaddr p = planes + 16 * k;
        const float a = rd<float>(p), b = rd<float>(p + 4), c = rd<float>(p + 8), w = rd<float>(p + 12);
        // Corner farthest along -normal (the game tests "not negative": a NaN counts as positive)...
        const float nx = !(a < 0) ? lo[0] : hi[0];
        const float ny = !(b < 0) ? lo[1] : hi[1];
        const float nz = !(c < 0) ? lo[2] : hi[2];
        float near = ny * b;
        near = near + nx * a;
        near = near + nz * c;
        near = w + near;
        if (near > 0) {
            result = 2;  // the whole box is on the outer side of this plane
            break;
        }
        // ... and corner farthest along +normal ("greater than zero": a NaN counts as negative).
        const float px = a > 0 ? hi[0] : lo[0];
        const float py = b > 0 ? hi[1] : lo[1];
        const float pz = c > 0 ? hi[2] : lo[2];
        float far = py * b;
        far = far + px * a;
        far = far + pz * c;
        far = w + far;
        if (far > 0) result = 1;
    }
    j.r()[0] = result;
    cpu::continue_at(j, j.r()[14]);
}

void patch_game(elf::Module* m) {
    if (m->name != "libSparkApp.so") return;

    if (enabled("idle")) {
        gaddr fn = export_of(m, "_Z13IdleCurThreadNSt6chrono8durationIxSt5ratioILx1ELx1000000EEEE");
        if (fn && code_is(fn & ~1u, {0xb570, 0xed2d, 0x8b02}) &&
            cpu::install_thumb_replace_hook(fn, &idle_cur_thread, "IdleCurThread")) {
            ZLOGI("perf: IdleCurThread sleeps instead of spinning");
        } else {
            ZLOGW("perf: IdleCurThread not patched (unexpected code)");
        }
    }

    if (enabled("gcpace")) {
        gaddr fn = export_of(m, "lua_asynchronous_gc_step");
        gaddr a = fn & ~1u;
        // ldr r2,[r0,#8]; ldr r3,[r2,#0x14]; cmp.w r3,#0x400 ... b.w <loop> at +0x12
        if (fn && code_is(a, {0x6882, 0x6953, 0xf5b3, 0x6f80}) && code_is(a + 0x12, {0xf003, 0xb8d9}) &&
            cpu::install_thumb_replace_hook(fn, &async_gc_step, "lua_asynchronous_gc_step")) {
            g_async_step_loop = (a + 0x12 + 4 + 0x31b2) | 1;
            ZLOGI("perf: asynchronous Lua GC paced");
            // lj_gc_step, 0x30e0 bytes after lua_asynchronous_gc_step in this build of LuaJIT
            gaddr step = a + 0x30e0;
            if (enabled("gcdefer") && code_is(step, {0xb5f8, 0x4606, 0x6885, 0xf44f, 0x447f}) &&
                cpu::install_thumb_entry_hook(step | 1, &gc_step, "lj_gc_step")) {
                ZLOGI("perf: game-thread GC steps left to the GC thread");
            }
        } else {
            ZLOGW("perf: lua_asynchronous_gc_step not patched (unexpected code)");
        }
    }

    if (enabled("frustum")) {
        gaddr fn = export_of(m, "_ZNK12geOctreeNodeI12geMeshEntityE22GetFrustumIntersectionERK16geAxisAlignedBoxPK9geVector4f");
        gaddr center = export_of(m, "_ZNK16geAxisAlignedBox9GetCenterEv");
        gaddr extent = export_of(m, "_ZNK16geAxisAlignedBox9GetExtentEv");
        gaddr a = fn & ~1u;
        // push {r4-r6, lr}; mov r0, r1; vpush {d8, d9}; mov r5, r1; mov r4, r2; vmov s19, r3; bl GetCenter; ...
        // and the box layout read by GetCenter (bx lr: center at +0) and GetExtent (adds r0, #12; bx lr).
        if (fn && center && extent && code_is(a, {0xb570, 0x4608, 0xed2d, 0x8b04, 0x460d, 0x4614, 0xee09, 0x3a90}) &&
            code_is(center & ~1u, {0x4770}) && code_is(extent & ~1u, {0x300c, 0x4770}) &&
            cpu::install_thumb_replace_hook(a | 1, &frustum_intersection, "GetFrustumIntersection")) {
            ZLOGI("perf: octree frustum test runs natively");
        } else {
            ZLOGW("perf: GetFrustumIntersection not patched (unexpected code)");
        }
    }
}

}  // namespace

namespace {

// Sets the game's frame rate target to FPS (the frame limiter of its game loop and the target of the next levels).
const char kFpsLua[] = R"LUA(
local fps = FPS
local P = rawget(_G, "GP") and GP.PARAMS
if P and type(P.FpsClampingTarget) == "number" and P.FpsClampingTarget ~= fps then P.FpsClampingTarget = fps end
local mp = rawget(_G, "MainProcess")
if mp then
  local ok, loop = pcall(mp.GetEntity, mp, "TagGameLoop")
  if ok and type(loop) == "table" and loop.IsFixedTimeStep and type(loop.TargetElapsedTime) == "number" then
    local d = loop.TargetElapsedTime - 1 / fps
    if d > 1e-6 or d < -1e-6 then loop.TargetElapsedTime = 1 / fps end
  end
end
)LUA";

std::atomic<int> g_target_fps{60};
bool g_fixed_fps = false;

std::string make_fps_chunk() {
    std::string lua = kFpsLua;
    lua.replace(lua.find("FPS"), 3, std::to_string(g_target_fps.load(std::memory_order_relaxed)));
    return lua;
}

}  // namespace

int target_fps() { return g_target_fps.load(std::memory_order_relaxed); }

bool fixed_fps() { return g_fixed_fps; }

void set_target_fps(int fps) {
    if (!g_fixed_fps && fps >= 30 && fps <= 240) g_target_fps.store(fps, std::memory_order_relaxed);
}

void init_perf_patches() {
    elf::add_patch_hook(&patch_game);
    // LuaJIT 2.0 compiles hot Lua code to ARM machine code that must sit within 32 MB of its VM, inside this huge
    // library: it never finds room, so it records traces, fails to place them and starts again, hundreds of times
    // a second (each try maps and unmaps memory). The interpreter alone is faster.
    if (enabled("luajit_off")) {
        cheat::add_startup_lua("luajit_off", "local ok, jit = pcall(require, 'jit') if ok and jit then jit.off() jit.flush() end");
    }
    // The game runs its 3D scenes at 30 frames per second on phones (GP.PARAMS.FpsClampingTarget, read when a level
    // is created to set the game loop's frame time; menus already run at 60). Its clock follows real time (the
    // global Dt is the measured frame time), so a higher target only makes it smoother. The target is the
    // display's highest refresh rate, once the game's window exists (android_glue.cpp); the property
    // debug.acp64.fps (30 to 240) fixes it instead.
    char v[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.acp64.fps", v) > 0 && atoi(v) >= 30 && atoi(v) <= 240) {
        g_target_fps = atoi(v);
        g_fixed_fps = true;
    }
    if (enabled("fps60")) {
        cheat::add_periodic_lua("fps", &make_fps_chunk, 2000);
        ZLOGI("perf: frame rate target %d fps%s", g_target_fps.load(), g_fixed_fps ? " (fixed)" : " until the display is known");
    }
}

}  // namespace zb::game
