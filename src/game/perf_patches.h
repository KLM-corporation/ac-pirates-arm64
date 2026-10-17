// Performance patches for the game's ARM32 code (Assassin's Creed Pirates 2.9.1), installed while the library is
// loaded. Each one can be turned off with a system property for comparisons (see perf_patches.cpp).
#pragma once

namespace zb::game {

// Registers the patches with the ELF loader (call before the game library is loaded).
void init_perf_patches();

}  // namespace zb::game
