-- LiliesGuest debug / AI helpers. Shipping logic is C++ (native/guest); deleting this file changes no gameplay.
-- Native API registered by main.dll:
--   LIE_GuestStatus()                        one-line state + oracle counters
--   LIE_GuestAct(x, y, held, taps, frames)   local scripted input (bypasses the Host)
--   LIE_GuestDebug(cmd)                      "unlock" | "restore" | "reanchor" | "isolate" | "menu" | "menu2" (spirits) | "menu3" (relics) | "menu4" (strengthen) | "terrain"

LIE = LIE or {}
function LIE.walk(dir, frames) return LIE_GuestAct(dir, 0.0, 0, 0, frames or 30) end
function LIE.tap(cmd, frames) local b = 1 << cmd; return LIE_GuestAct(0.0, 0.0, b, b, frames or 4) end

print("[LiliesGuest] debug Lua loaded\n")
