# Spike results — Hyprland Lua modifier hook (plan Task 1)

Hyprland 0.56.2 (Lua config), 2026-10-21. Loaded live with `hyprctl eval "dofile(...)"`, unloaded with the
generation flag (`_G.omen_kbd_spike_gen = -1`). Log: `journalctl -t omen-kbd-spike`.

## Run 1 (oneshot timer) — 21:57

| Check | Expected | Logged | Result |
|---|---|---|---|
| 1. Super_L, Shift_L, Ctrl_L, Alt_L (no Super_R on this laptop) | raw 133, 50, 37, 64 | `raw press 133`, `50`, `37`, `64 (raw logging now off)` | **pass** — event delivers XKB keycodes (also seen earlier: b=56, Space=65, Backspace=22) |
| 2. hold Super | `hint super` → `hint clear` | yes (21:58:08) | pass |
| 3–8 | hints | **nothing at all** | **fail** |

**Root cause (measured with standalone timers, no keys):** a `type = "oneshot"` timer is destroyed
after it fires — `is_enabled()` on its handle returns `nil` and `set_enabled(true)` never fires again.
A `type = "repeat"` timer that disables itself in its callback re-arms reliably (3/3 fires).

## Run 2 (repeat "ticker": 50 ms × 5 ticks, runs only while a wanted set is held) — 22:13

| Check | Expected | Logged | Result |
|---|---|---|---|
| 2. hold Super ~1 s | `hint super` → `hint clear` | 13:02.60 → 13:03.68 | pass |
| 3. quick Super+Q | no hint | `hint super` 13:06.87 → `clear` 13:07.29 | **flash** — a normal chord held Super ~0.67 s |
| 4. hold Super, add Shift | `super` → `super+shift`, no clear between | 13:16.98 → 13:17.76 → clear 13:18.79 | pass |
| 5. type `Hello` (done in run 1, typed in the Claude Code chat) | nothing | nothing | pass |
| 6. hold Super across Super+2 | hint stays, clear on release | 13:30.14 → clear 13:33.81 | pass |
| 7. tap Super twice, then hold | exactly one `hint super` | 13:36.27 → 13:36.79 | pass (ticker re-arms) |
| 8. hold Super, lock (Super+L), unlock with password | clear arrives; only `hint` lines | 13:56.04 → clear 13:57.54 (released on the lock screen); 14:00.63 → 14:01.06 | pass — key events reach Lua under hyprlock; nothing but hint lines logged |

## Decisions for Task 17 (`gen-hyprland`)

1. Emit the **repeat ticker** (50 ms, `floor(delay_ms / 50)` ticks, self-disabling), never a oneshot timer.
2. **A non-modifier key press cancels a pending (not yet shown) hint**; a hint already shown stays until
   the modifier set changes. Fixes the check-3 flash without lengthening the delay; logs nothing.
3. Use XKB keycodes as in the spike (133/134 super, 50/62 shift, 37/105 ctrl, 64/108 alt).
4. Keep the generation guard so re-loading the module (e.g. `hyprctl reload`) never leaves two live handlers.
