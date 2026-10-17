// Process-wide setup: guest memory, CPU, thunks, and loading the game library.
#pragma once

#include "elf_loader.h"

namespace zb::runtime {

// Initializes guest memory, the JIT and every HLE library. Idempotent.
void init();

// Loads an ARM32 library image (already in host memory) into the guest.
elf::Module* load_library(const std::string& name, const u8* data, size_t size);

}  // namespace zb::runtime
