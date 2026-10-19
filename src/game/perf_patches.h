// Performance patches for the game's ARM32 code (Assassin's Creed Pirates 2.9.1), installed while the library is
// loaded, and the frame rate and image quality settings of its 3D scenes. Each one can be turned off with a system
// property for comparisons (see perf_patches.cpp).
#pragma once

namespace zb::game {

// Registers the patches with the ELF loader (call before the game library is loaded).
void init_perf_patches();

// Frame rate the game aims for (frames per second).
int target_fps();
// True when the property debug.acp64.fps fixes it.
bool fixed_fps();
// Changes it (to the display's refresh rate); ignored when it is fixed.
void set_target_fps(int fps);

}  // namespace zb::game
