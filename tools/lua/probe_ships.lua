-- Ship data: player ship tables and every string mentioning the Queen Anne's Revenge.
local log = acp64_log
local p = playerProfile

local function keys(t)
  local r = {}
  for k in pairs(t) do r[#r + 1] = tostring(k) end
  table.sort(r)
  return r
end

local function dump(t, path, depth, maxdepth, seen)
  seen = seen or {}
  if seen[t] then log(path .. " -> (seen)") return end
  seen[t] = true
  local n = 0
  for k, v in pairs(t) do
    n = n + 1
    if n > 60 then log(path .. " ... truncated") break end
    local tv = type(v)
    local ks = tostring(k)
    if tv == "table" then
      log(path .. "." .. ks .. " = {table " .. #keys(v) .. " keys}")
      if depth < maxdepth then dump(v, path .. "." .. ks, depth + 1, maxdepth, seen) end
    elseif tv == "function" then
      log(path .. "." .. ks .. " : function")
    else
      log(path .. "." .. ks .. " = " .. tostring(v))
    end
  end
end

log("== PlayerStatsTable keys: " .. table.concat(keys(p.PlayerStatsTable), ", "))
for _, name in ipairs({ "_PlayerShipsData", "PlayerShipsCustomization", "ShipLockByMission", "_PlayerSaveData" }) do
  log("== " .. name)
  local ok, t = pcall(function() return p[name] end)
  if ok and type(t) == "table" then dump(t, name, 0, 3) else log(tostring(t)) end
end

-- Every table reachable from _G and the registry that has a key or a string value mentioning the ship.
local pats = { "queen", "revenge", "blackbeard", "barbanegra", "qar" }
local visited, queue, qh, qt = {}, {}, 1, 0
local function push(v, path)
  local tv = type(v)
  if tv ~= "table" then return end
  if visited[v] then return end
  visited[v] = true
  qt = qt + 1
  queue[qt] = { v, path }
end
local function match(s)
  local l = string.lower(s)
  for _, pat in ipairs(pats) do
    if string.find(l, pat, 1, true) then return true end
  end
  return false
end
push(_G, "_G")
if debug and debug.getregistry then push(debug.getregistry(), "REG") end
local nodes, hits = 0, 0
while qh <= qt and nodes < 400000 do
  local item = queue[qh]
  queue[qh] = nil
  qh = qh + 1
  local t, path = item[1], item[2]
  nodes = nodes + 1
  for k, v in pairs(t) do
    local ks = type(k) == "string" and k or ("[" .. tostring(k) .. "]")
    local hit = (type(k) == "string" and match(k)) or (type(v) == "string" and match(v))
    if hit then
      hits = hits + 1
      if hits <= 600 then log("HIT " .. path .. "." .. ks .. " = " .. (type(v) == "table" and "{table}" or string.sub(tostring(v), 1, 80))) end
    end
    push(v, path .. "." .. ks)
  end
end
log("!ships probe done: nodes=" .. nodes .. " hits=" .. hits)
