-- Lists the player profile: numeric fields and the methods of its class chain.
local log = acp64_log
local p = playerProfile
log("profile type: " .. type(p))

local nums = {}
for k, v in pairs(p) do
  if type(v) == "number" then nums[#nums + 1] = tostring(k) .. " = " .. tostring(v) end
end
table.sort(nums)
log("== numeric fields (" .. #nums .. ")")
for _, s in ipairs(nums) do log(s) end

local mt = getmetatable(p)
local depth = 0
while mt and depth < 10 do
  local idx = rawget(mt, "__index")
  log("== metatable level " .. depth .. ": __index is " .. type(idx))
  if type(idx) == "table" then
    local names = {}
    for k, v in pairs(idx) do names[#names + 1] = tostring(k) .. " : " .. type(v) end
    table.sort(names)
    log("   " .. #names .. " members")
    for _, s in ipairs(names) do log("   " .. s) end
    mt = getmetatable(idx)
  else
    break
  end
  depth = depth + 1
end
log("!profile probe done")
