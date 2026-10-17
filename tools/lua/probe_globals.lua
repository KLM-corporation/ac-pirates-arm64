-- Dumps the game's Lua globals to probe.out (see src/cheat/cheat.cpp).
-- Push it as <app external files dir>/probe.lua with debug.acp64.verbose=1.
local log = acp64_log
local seen = {}
local lines, MAXLINES = 0, 80000

local function out(s)
  lines = lines + 1
  if lines <= MAXLINES then log(s) end
end

local function key(k)
  if type(k) == "string" then return k end
  return "[" .. tostring(k) .. "]"
end

local function dump(t, path, depth)
  if seen[t] then
    out(path .. " -> same as " .. seen[t])
    return
  end
  seen[t] = path
  local nfunc, n = 0, 0
  for k, v in pairs(t) do
    n = n + 1
    if n > 400 then
      out(path .. " ... truncated")
      break
    end
    local tv = type(v)
    if tv == "function" then
      nfunc = nfunc + 1
    elseif tv == "table" then
      local p = path .. "." .. key(k)
      out(p .. " = {table}")
      if depth < 6 then dump(v, p, depth + 1) end
    elseif tv == "number" or tv == "boolean" then
      out(path .. "." .. key(k) .. " = " .. tostring(v))
    elseif tv == "string" then
      out(path .. "." .. key(k) .. ' = "' .. string.sub(v, 1, 60) .. '"')
    else
      out(path .. "." .. key(k) .. " : " .. tv)
    end
  end
  if nfunc > 0 then out(path .. " (+" .. nfunc .. " functions)") end
end

dump(_G, "_G", 0)
if debug and debug.getregistry then dump(debug.getregistry(), "REG", 0) end
log("!probe done: " .. lines .. " lines")
