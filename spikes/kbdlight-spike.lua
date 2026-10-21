-- kbdlight spike: log held-modifier sets (no omen-kbd needed)
-- Load:   hyprctl eval "dofile('<abs path>/spikes/kbdlight-spike.lua')"  (re-loading silences older copies)
-- Unload: hyprctl eval "_G.omen_kbd_spike_gen = -1"   (or any later `hyprctl reload`)
local MODS = { [133]="super", [134]="super", [37]="ctrl", [105]="ctrl",
               [64]="alt", [108]="alt", [50]="shift", [62]="shift" }
local ORDER = { "super", "ctrl", "alt", "shift" }
local WANTED = { ["super"]=true, ["super+shift"]=true, ["ctrl"]=true }
local DELAY_MS = 250
local TICK_MS = 50
local RAW_LIMIT = 0   -- XKB codes already confirmed (133/50/37/64); no raw keycode logging in this version
local held, sent, pending, raw_left, ticks_left = {}, false, nil, RAW_LIMIT, 0
_G.omen_kbd_spike_gen = (_G.omen_kbd_spike_gen or 0) + 1
local GEN = _G.omen_kbd_spike_gen
local function active() return _G.omen_kbd_spike_gen == GEN end
local function log(msg) hl.exec_cmd("logger -t omen-kbd-spike '" .. msg .. "'") end
local function current_set()
  local present, parts = {}, {}
  for code in pairs(held) do present[MODS[code]] = true end
  for _, m in ipairs(ORDER) do if present[m] then parts[#parts + 1] = m end end
  return table.concat(parts, "+")
end
-- A Hyprland 0.56 "oneshot" timer is destroyed after it fires (its handle goes dead), so use one
-- "repeat" ticker that runs only while a wanted modifier set is held and stops itself after DELAY_MS.
local ticker
ticker = hl.timer(function()
  if not active() then ticker:set_enabled(false); return end
  ticks_left = ticks_left - 1
  if ticks_left > 0 then return end
  ticker:set_enabled(false)
  if pending ~= nil and pending == current_set() then log("hint " .. pending); sent = true end
end, { timeout = TICK_MS, type = "repeat" })
ticker:set_enabled(false)
hl.on("input.keyboard.key", function(keycode, _time, state)
  if not active() then return end
  if state == 1 and raw_left > 0 then
    raw_left = raw_left - 1
    log("raw press " .. keycode .. (raw_left == 0 and " (raw logging now off)" or ""))
  end
  if MODS[keycode] == nil or state == 2 then return end
  if state == 1 then held[keycode] = true else held[keycode] = nil end
  local set = current_set()
  ticker:set_enabled(false)
  pending = nil
  if WANTED[set] then
    pending = set
    ticks_left = math.floor(DELAY_MS / TICK_MS)
    ticker:set_enabled(true)
  elseif sent then
    log("hint clear"); sent = false
  end
end)
log("spike loaded (generation " .. GEN .. ")")
