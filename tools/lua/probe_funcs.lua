-- Prints the constants (field and function names) used by selected PlayerProfile functions.
local log = acp64_log
local ju = jit.util or require("jit.util")
local def = ClassDefinitionMap.PlayerProfile[1]

local function dumpfunc(name, f)
  if type(f) ~= "function" then
    log(name .. ": " .. type(f))
    return
  end
  local info = ju.funcinfo(f)
  if not info or not info.gcconsts then
    log(name .. ": (C function or no info)")
    return
  end
  local parts = {}
  for i = 1, info.gcconsts do
    local ok, k = pcall(ju.funck, f, -i)
    if ok then parts[#parts + 1] = (type(k) == "string") and ('"' .. k .. '"') or type(k) end
  end
  local nums = {}
  for i = 0, (info.nconsts or 0) - 1 do
    local ok, k = pcall(ju.funck, f, i)
    if ok then nums[#nums + 1] = tostring(k) end
  end
  log(name .. " [params " .. tostring(info.params) .. ", ups " .. tostring(info.upvalues) .. ", bc " ..
      tostring(info.bytecodes) .. "] gc: " .. table.concat(parts, " ") .. " | nums: " .. table.concat(nums, " "))
end

for _, n in ipairs({ "RewardHardCurrency", "RewardSoftCurrency", "RewardWood", "RewardSupplies", "RewardBooks",
                     "CanAffordCost", "SET_SoftCurrency", "GET_SoftCurrency", "GET_SoftCurrencyMaxLimit",
                     "SET_Wood", "SET_Supplies", "SET_Books", "GET_MoneySpent" }) do
  dumpfunc(n, def[n])
end

local mt = getmetatable(playerProfile)
dumpfunc("mt.__index", mt.__index)
dumpfunc("mt.__newindex", mt.__newindex)

for _, n in ipairs({ "SoftCurrency", "SoftCurrencyMaxLimit", "Wood", "Supplies", "Books", "MoneySpent" }) do
  local ok, v = pcall(function() return playerProfile[n] end)
  log("playerProfile." .. n .. " -> " .. tostring(ok) .. " " .. tostring(v))
end
log("!funcs probe done")
