-- Queen Anne's Revenge (ship id FRIGATE4): the game sold it as an in-app purchase that can no longer be
-- bought. This grants it to the player with the game's own BuyShip (the function its shop calls once a ship
-- is paid for). The runtime runs it about once a second after the code was typed, because the game replaces
-- the player's ship data while it loads the save (and at other moments): the ship is then simply granted
-- again, so it is there whenever the game is played. Returns true once the ship is owned.
--
-- The main menu works on a throw-away default profile (session number 0, still "in the main menu"); the real
-- one is loaded in the background and replaces its data. Nothing is granted to the default one.
local p = playerProfile
if type(p) ~= "table" then return false end

local loaded = false
pcall(function()
  loaded = (tonumber(p._SessionNumber) or 0) >= 1 or p._IsInMainMenu == false
end)
if not loaded then return false end

local owned = false
pcall(function()
  if not p:IsShipBought("FRIGATE4") then p:BuyShip("FRIGATE4") end
  owned = p:IsShipBought("FRIGATE4") and true or false
end)
return owned
