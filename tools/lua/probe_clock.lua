-- Game clock and frame rate settings, to check the game's speed at a frame rate: run it twice some seconds apart
-- and compare how far AbsoluteTime moved with the logcat timestamps of the two lines.
local mp = rawget(_G, "MainProcess")
local ok, loop = false, nil
if mp then ok, loop = pcall(mp.GetEntity, mp, "TagGameLoop") end
if not ok then loop = nil end
local P = rawget(_G, "GP") and GP.PARAMS
acp64_log(string.format("!clock AbsoluteTime=%.3f Dt=%.5f FrameCount=%s TargetElapsedTime=%s IsFixedTimeStep=%s FpsClampingTarget=%s",
  tonumber(rawget(_G, "AbsoluteTime")) or -1, tonumber(rawget(_G, "Dt")) or -1, tostring(rawget(_G, "FrameCount")),
  tostring(loop and loop.TargetElapsedTime), tostring(loop and loop.IsFixedTimeStep),
  tostring(P and P.FpsClampingTarget)))
