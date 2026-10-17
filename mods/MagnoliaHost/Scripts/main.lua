-- MagnoliaHost debug / AI helpers. Shipping logic is C++ (native/host); deleting this file changes no gameplay.
-- Native API registered by main.dll:
--   LIE_HostStatus()                         one-line state + oracle counters
--   LIE_HostAct(x, y, held, taps, frames)    scripted input through the normal Host input path
--   LIE_HostDebug(cmd)                       "rebase" | "menu" | "restore" | "kill" | "terrain" | "lilac"

LIE = LIE or {}

-- Dev automation only (tools\start.ps1 sets LIE_DEV_AUTOCONTINUE=1): continue save slot 0 from the EM title.
if os.getenv("LIE_DEV_AUTOCONTINUE") == "1" then
    local step = 0
    local handle
    handle = LoopInGameThreadWithDelay(700, function()
        pcall(function()
            if step == 0 then
                local t = FindFirstOf("WBP_Title_C")
                if t and t:IsValid() then
                    step = 1
                    t:BndEvt__WBP_Title_WBP_Button_Generic_Start_K2Node_ComponentBoundEvent_1_OnPressed__DelegateSignature({})
                end
            elseif step == 1 then
                local s = FindFirstOf("WBP_SlotSelection_C")
                if s and s:IsValid() and s.WBP_SlotSelection_Entry:IsValid() then
                    step = 2
                    s:OnSlotConfirmed(s.WBP_SlotSelection_Entry)
                end
            elseif step == 2 then
                local c = FindFirstOf("WBP_SaveSlotContext_C")
                if c and c:IsValid() then
                    step = 3
                    c:BndEvt__WBP_SaveSlotContext_WBP_Button_Generic_Load_K2Node_ComponentBoundEvent_0_OnPressed__DelegateSignature({})
                end
            end
        end)
        if step >= 3 then CancelDelayedAction(handle) end
    end)
end

function LIE.walk(dir, frames) return LIE_HostAct(dir, 0.0, 0, 0, frames or 30) end
function LIE.tap(cmd, frames) local b = 1 << cmd; return LIE_HostAct(0.0, 0.0, b, b, frames or 4) end

print("[MagnoliaHost] debug Lua loaded\n")
