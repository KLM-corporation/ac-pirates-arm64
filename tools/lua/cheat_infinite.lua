-- Infinite resources: tops up the soft currency ("gold") and the secondary
-- resources of the player profile up to the game's own limits. The runtime runs
-- this chunk a few times per second while the cheat is on (it is embedded in
-- src/cheat/cheat.cpp at build time, see CMakeLists.txt).
local p = playerProfile
if type(p) ~= "table" then return end

local function top_up(name, limit)
  local ok, cur = pcall(function() return p[name] end)
  if ok and type(cur) == "number" and cur < limit then
    pcall(function() p[name] = limit end)
  end
end

local ok, smax = pcall(function() return p.SoftCurrencyMaxLimit end)
if not ok or type(smax) ~= "number" then smax = 9999999 end
local ok2, rmax = pcall(function() return p.SecondaryResourceMaxLimit end)
if not ok2 or type(rmax) ~= "number" then rmax = 999 end

top_up("SoftCurrency", smax)
top_up("Wood", rmax)
top_up("Supplies", rmax)
top_up("Books", rmax)
