-- Searches everything reachable from _G / the registry (tables, metatables,
-- function upvalues and environments) for keys that look economy related.
local log = acp64_log
local pats = { "softcurrency", "currency", "money", "spend", "wood", "suppl", "books", "afford", "gold", "hardcurrency" }
local visited, queue = {}, {}
local qh, qt = 1, 0

local function push(v, path)
  local tv = type(v)
  if tv ~= "table" and tv ~= "function" then return end
  if visited[v] then return end
  visited[v] = true
  qt = qt + 1
  queue[qt] = { v, path }
end

local function match(name)
  local l = string.lower(name)
  for _, p in ipairs(pats) do
    if string.find(l, p, 1, true) then return true end
  end
  return false
end

push(_G, "_G")
if debug and debug.getregistry then push(debug.getregistry(), "REG") end

local nodes, hits = 0, 0
while qh <= qt and nodes < 500000 do
  local item = queue[qh]
  queue[qh] = nil
  qh = qh + 1
  local v, path = item[1], item[2]
  nodes = nodes + 1
  if type(v) == "table" then
    for k, x in pairs(v) do
      local ks = type(k) == "string" and k or ("[" .. tostring(k) .. "]")
      local p = path .. "." .. ks
      if type(k) == "string" and match(k) then
        hits = hits + 1
        if hits < 4000 then
          local tx = type(x)
          local val = (tx == "number" or tx == "string" or tx == "boolean") and (" = " .. tostring(x)) or ""
          log(p .. " : " .. tx .. val)
        end
      end
      push(x, p)
      if type(k) == "table" then push(k, p .. ".<key>") end
    end
    local mt = getmetatable(v)
    if mt then push(mt, path .. ".<mt>") end
  else
    local i = 1
    while true do
      local n, u = debug.getupvalue(v, i)
      if n == nil then break end
      push(u, path .. ".<up" .. i .. ":" .. n .. ">")
      i = i + 1
    end
    local env = debug.getfenv and debug.getfenv(v)
    if env then push(env, path .. ".<env>") end
  end
end
log("!search done: nodes=" .. nodes .. " queued=" .. qt .. " hits=" .. hits)
