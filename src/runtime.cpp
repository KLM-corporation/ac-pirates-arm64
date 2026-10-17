#include "runtime.h"

#include <sys/system_properties.h>

#include <mutex>

#include "cheat/cheat.h"
#include "game/perf_patches.h"
#include "cpu.h"
#include "hle/hle.h"
#include "mem.h"

namespace zb::runtime {
namespace {

std::once_flag g_once;

gaddr resolve(const std::string& name, bool) {
    if (gaddr a = elf::lookup(name)) return a;
    if (gaddr a = cpu::find_thunk(name)) return a;
    return cpu::find_data(name);
}

}  // namespace

void init() {
    std::call_once(g_once, [] {
        char v[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.acp64.verbose", v) > 0 && v[0] == '1') g_debug = true;
        mem::init();
        cpu::init();
        hle::register_all();
        cheat::init();
        game::init_perf_patches();
        ZLOGI("runtime initialized");
    });
}

elf::Module* load_library(const std::string& name, const u8* data, size_t size) {
    return elf::load(name, data, size, &resolve);
}

}  // namespace zb::runtime
