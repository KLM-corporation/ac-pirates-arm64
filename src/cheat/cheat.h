// Trainer for the game's Lua scripts.
//
// The game logic (economy included) is LuaJIT code running inside the guest
// library, which exports the whole Lua C API. The runtime hooks the entry of
// lua_pcall: while the cheat is on it runs a Lua chunk of its own a few times per
// second on the calling thread, between two game calls, so the Lua state is never
// touched concurrently.
#pragma once

#include <string>

#include "../common.h"

namespace zb::cheat {

// Registers the hooks (call after cpu::init and the HLE registration, before the game library loads).
void init();

// Directory the app can write to from adb (debug probe scripts and results; only used with debug.acp64.verbose=1).
void set_data_dir(const std::string& dir);

// Private directory that remembers whether the cheat is on, and restores it.
void set_state_dir(const std::string& dir);

// Cheat code typed by the player (called from the UI thread). Returns 0 for an unknown code, 1 when infinite resources were
// just turned on and 2 when they were turned off (the same code toggles them), 3 when the Queen Anne's Revenge was granted, 4
// when it is accepted but will only be granted once the player's game is loaded: the notifier is then called with
// kEventQueenGranted when it happens.
int submit_code(const std::string& code);

constexpr int kEventQueenGranted = 1;
using Notifier = void (*)(int event);
void set_notifier(Notifier notifier);

// Lua chunk run once on the game's Lua thread, as soon as it is known (a few seconds after start); used for
// performance settings of the game itself.
void add_startup_lua(const std::string& name, const std::string& source);
// Lua chunk run on the game's Lua thread every `period_ms` (same use).
void add_periodic_lua(const std::string& name, const std::string& source, int period_ms);
// Same, with the chunk's source produced by `make_source` at each run (called on the game's Lua thread).
void add_periodic_lua(const std::string& name, std::string (*make_source)(), int period_ms);

// Infinite resources: the player's gold, wood, supplies and books are topped up to the game's own limits.
void set_infinite_resources(bool on);
bool infinite_resources();

}  // namespace zb::cheat
