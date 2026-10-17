// Loader for ARM32 (ELFCLASS32, EM_ARM) shared objects into guest memory.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "common.h"

namespace zb::elf {

struct Module {
    std::string name;
    gaddr bias = 0;   // load bias: guest address = bias + p_vaddr
    gaddr lo = 0;     // mapped range [lo, hi)
    gaddr hi = 0;
    gaddr rx_lo = 0;  // read-only/executable segment
    gaddr rx_hi = 0;
    gaddr exidx = 0;
    u32 exidx_count = 0;
    gaddr init = 0;
    std::vector<gaddr> init_array;
    std::unordered_map<std::string, gaddr> exports;  // includes the Thumb bit for functions
    std::vector<std::pair<gaddr, std::string>> sorted_funcs;  // for symbolization
    std::vector<std::string> missing;  // imports that resolved to stubs
};

// Resolves an undefined symbol. Returns 0 if unknown.
using Resolver = gaddr (*)(const std::string& name, bool is_func);

// Called for every loaded module after relocation, while its code is still
// writable (the runtime patches game code through it).
using PatchHook = void (*)(Module*);
void add_patch_hook(PatchHook hook);

Module* load(const std::string& name, const u8* data, size_t size, Resolver resolve);
void run_initializers(Module* m);

Module* find_module(const std::string& name);
Module* module_for_addr(gaddr a);
gaddr lookup(const std::string& name);  // exported symbol across loaded modules

}  // namespace zb::elf
