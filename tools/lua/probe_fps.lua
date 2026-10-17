-- Where does the game read its frame-rate cap (FpsClamping / FpsClampingTarget)?
local log = acp64_log
local ju = jit.util or require("jit.util")

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

local pats = { "fpsclamp", "targetfps", "framerate", "maxfps", "settimestep", "fixedstep", "timescale", "deltatime" }
local function match(s)
  local l = string.lower(s)
  for _, p in ipairs(pats) do if string.find(l, p, 1, true) then return p end end
end

local visited, queue, qh, qt = {}, {}, 1, 0
local function push(v, path)
  local tv = type(v)
  if (tv ~= "table" and tv ~= "function") or visited[v] then return end
  visited[v] = true
  qt = qt + 1
  queue[qt] = { v, path }
end
push(_G, "_G")

local hits = 0
while qh <= qt do
  local item = queue[qh]
  queue[qh] = nil
  qh = qh + 1
  local v, path = item[1], item[2]
  if type(v) == "table" then
    for k, x in pairs(v) do
      local ks = type(k) == "string" and k or ("[" .. tostring(k) .. "]")
      if type(k) == "string" and match(k) and hits < 300 then
        hits = hits + 1
        local tx = type(x)
        log("KEY " .. path .. "." .. ks .. " : " .. tx .. ((tx == "number" or tx == "string" or tx == "boolean") and (" = " .. tostring(x)) or ""))
      end
      push(x, path .. "." .. ks)
    end
  else
    local c = consts(v)
    for _, s in ipairs(c) do
      if match(s) and hits < 300 then
        hits = hits + 1
        log("FUNC " .. path .. " uses \"" .. s .. "\" | " .. string.sub(table.concat(c, " "), 1, 200))
        break
      end
    end
  end
end
log("!fps probe done: hits=" .. hits)
