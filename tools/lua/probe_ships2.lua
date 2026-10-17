-- Which game functions mention the FRIGATE4 ship or the ShipBought flag, and what the ship tables say about it.
local log = acp64_log
local ju = jit.util or require("jit.util")
local p = playerProfile

local function consts(f)
  local out = {}
  local info = ju.funcinfo(f)
  if not info or not info.gcconsts then return out end
  for i = 1, info.gcconsts do
    local ok, k = pcall(ju.funck, f, -i)
    if ok and type(k) == "string" then out[#out + 1] = k end
  end
  return out
end

local wanted = { "FRIGATE4", "ShipBought", "blackbeard", "BlackBeard", "BuyShip", "UnlockShip", "CurrentShip", "ShipsUpgrades" }
local function interesting(list)
  local hit = {}
  for _, c in ipairs(list) do
    for _, w in ipairs(wanted) do
      if string.find(c, w, 1, true) then hit[#hit + 1] = c break end
    end
  end
  return hit
end

local seen = {}
local function scan(tbl, label)
  if seen[tbl] then return end
  seen[tbl] = true
  for k, v in pairs(tbl) do
    if type(v) == "function" then
      local c = consts(v)
      local hit = interesting(c)
      if #hit > 0 then
        log("FUNC " .. label .. "." .. tostring(k) .. " [" .. table.concat(hit, ",") .. "] all: " .. string.sub(table.concat(c, " "), 1, 260))
      end
    end
  end
end

for name, defs in pairs(ClassDefinitionMap) do
  if type(defs) == "table" then
    for i, d in pairs(defs) do
      if type(d) == "table" then scan(d, name .. "[" .. tostring(i) .. "]") end
    end
  end
end

local function dump(t, path, depth, maxdepth)
  local n = 0
  for k, v in pairs(t) do
    n = n + 1
    if n > 40 then log(path .. " ...") break end
    if type(v) == "table" then
      log(path .. "." .. tostring(k) .. " = {table}")
      if depth < maxdepth then dump(v, path .. "." .. tostring(k), depth + 1, maxdepth) end
    else
      log(path .. "." .. tostring(k) .. " = " .. tostring(v))
    end
  end
end

local st = p.PlayerStatsTable
log("== Ship_NameTable")
if type(st.Ship_NameTable) == "table" then dump(st.Ship_NameTable, "Ship_NameTable", 0, 1) end
log("== Ship_StatsTable.FRIGATE4")
if type(st.Ship_StatsTable) == "table" and type(st.Ship_StatsTable.FRIGATE4) == "table" then dump(st.Ship_StatsTable.FRIGATE4, "FRIGATE4", 0, 2) end
log("== Ship_StatsTable keys")
if type(st.Ship_StatsTable) == "table" then
  local ks = {}
  for k in pairs(st.Ship_StatsTable) do ks[#ks + 1] = tostring(k) end
  table.sort(ks)
  log(table.concat(ks, ", "))
end
log("== _BoughtInAppPacks / _HasBuyInApp: " .. tostring(p._BoughtInAppPacks) .. " / " .. tostring(p._HasBuyInApp))
log("!ships2 probe done")
