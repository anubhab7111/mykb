# Keyboard reactions: design

- **Date:** 2026-11-08. **Branch:** `keyboard-reactions` (from `main` at `2748e9e`).
- **Status:** draft, waiting for the user's approval. The implementation plan follows once it is approved.
- **Builds on:** `docs/superpowers/specs/2026-10-21-omen-kbd-design.md` (the daemon, its layer
  stack, the hardware limits) and `docs/hardware-notes.md` (P4/P5 results).

## 1. Why

The user wants the keyboard lights to be **more interactive**: short light cues for what they do and
what happens on the desktop. The keyboard does not read their typing. The user chose six reactions
from a brainstorm on 2026-11-08. They added two requirements in their own words: "minimizing the
writes for each feature" and "maintain proper abstraction and encapsulation in the code".

## 2. What the user sees

Six reactions. The writes are counted on a static profile, and each write is one atomic 4-zone frame.

| # | Reaction | Trigger | Look (defaults) | Writes | Bursts |
|---|---|---|---|---|---|
| 1 | Workspace sweep | Switching to a higher or lower numbered workspace (Super+1…0, Super+scroll) | The accent color (`theme:lavender`) lights one zone at a time, left → middle → right, or right → left when going down. 120 ms per step. WASD lights with the left zone. | 4 | Switches during a sweep are absorbed, so a fast spin through workspaces costs 4. |
| 2 | Window opened | A window opens | One 120 ms step: every zone moves 50% toward white | 2 | At most one window pulse (open or close) per 500 ms; the rest are dropped. |
| 3 | Window closed | A window closes (Super+C or otherwise) | One 120 ms step: every zone dips to 35% | 2 | Same 500 ms rule as #2. |
| 4 | Attention light | A window that is not focused asks for attention (Hyprland `urgent`) | The right zone in `theme:yellow` blinks twice (on, off, on, 150 ms each), then **holds steady** until every such window has been focused or closed | 4 per episode | More urgent windows while it is lit cost 0. |
| 5 | Fullscreen video | The focused window is in true fullscreen, and its app class is in a list (default `mpv`, `vlc`, `firefox`, `chromium`) | Brightness capped at 15% (a rule) | 1 on entering, 1 on leaving | Hyprland's repeated `fullscreen` events cost 0, because the state is compared, not counted. |
| 6 | Notification flash | swaync receives a notification | Every zone blinks once for 150 ms: `theme:red` if critical, the app's color from a table (ZapZap `theme:green`, Discord `#5865f2`), otherwise `theme:blue` | 2 | At most one flash waits in a queue, so a burst costs ≤ 4. |
| 7 | Claude lamp: waiting | Any Claude Code session needs approval or input | WASD in `theme:peach` blinks twice, then **holds steady** until the session continues, finishes or ends. For a session started directly in a terminal it also clears when you focus that terminal, and does not light at all while that terminal is already focused. | 4 per episode, 0 if you are looking at a linked session | More sessions waiting while it is lit cost 0. |
| 8 | Claude lamp: finished | A Claude Code session finishes its turn | WASD blinks once, 150 ms, `theme:green`. Skipped if that session's terminal window is focused. | 2 | Queued like #6. |

How the lamp knows a session's terminal: the hook finds the terminal window, but **only for a
session started directly in a terminal** (§5.12). Background jobs and other nested sessions have no
link, so their lamp lights and flashes whatever is focused. For a linked session, the daemon uses the
window in three ways:
- it decides #8's skip;
- it clears #7 on focus;
- it lets the lamp **absorb an urgent signal from the same window**. If Claude rings the bell and
  Alacritty marks itself urgent, that does not add a second light (#4) for the same event.

The decision recorded for this table (user, 2026-11-08): the two waiting lights (#4, #7) **blink and
then hold**. They never pulse continuously. A continuous pulse would cost about 10 writes a second
for as long as something waits, which could be hours. Holding a steady color costs nothing.

**Silent cases (0 writes):**
- Reactions are disabled, or the keyboard is dark:
  - the user turned it off, or brightness is 0;
  - the lid is closed;
  - the session is locked with `on_lock = "off"`, or idle with `on_idle = "off"`;
  - the machine is suspended;
  - the hourly write budget is exhausted.
- The firmware backlight is off after the Fn key. The daemon cannot see the Fn key, so before a
  reaction starts it reads the backlight if its last reading is older than 5 s (§5.7). Only a
  reaction within 5 s of an Fn press can write unseen.
- Short reactions (#1–3, #6, #8) also stay silent while the user is away: locked or idle, whatever
  the gate does. They are dropped, not saved for later.
- Notifications are also skipped:
  - during do-not-disturb (the user's swaync script already checks this);
  - for low urgency;
  - when the notifying app is the focused window.
- The lamps (#4, #7) keep their state while silent. When the keyboard becomes visible again they
  show steady, with no blinks, at a cost of 1 write.

**On top of a running effect** (the user's `cycle` effect, for example), reactions cost **no extra
writes**. The effect's own ticks sample the reaction, so no extra wake-ups are scheduled, and the
frame changes every tick anyway. A step may land up to one tick late, about 100 ms.

**Layer order**, bottom to top:
1. profile or effect
2. rule overlay or tint
3. **lamps**
4. mode hint (Super, Ctrl, submap)
5. **short reaction**
6. brightness (rule caps, including fullscreen, and the dim gates)
7. backlight gate

The order has three consequences:
- A held modifier hides the lamps while it is held.
- A sweep shows even over the Super color, because it is feedback for the switch you just made.
- A notification during fullscreen video flashes at the dimmed brightness.

## 3. Goals, non-goals, anti-goals

**Goals**
- G1: Each reaction's write cost is small and fixed, and the tests prove it. Section 2's table is
  the contract.
- G2: One mechanism serves all six. Producers say what happened, config says how it looks, a pure
  core plays it, and the existing writer writes it. Producers never handle colors.
- G3: The desktop reactions (#1–5) need no dotfile edits, because the daemon reads Hyprland
  itself. Notifications need one line in the user's swaync script, and the Claude lamp needs one
  hook script plus a settings block.
- G4: The worst case stays bounded even when an app misbehaves (see the rate limit in §5.4).

**Non-goals (this round)**
- These brainstorm ideas were not picked: the Super-hold bind legend, the screen-share "on air"
  light, music, focused-app tint, workspace colors, timers, and typing activity.
- Smooth animations, per-key effects, and any change to the kernel module, its limits or the write
  budget.
- Other compositors. Other coding agents in v1: the CLI is agent-neutral, so an opencode hook can
  come later.

**Anti-goals** (any of these is a failure, even if the feature works)
- Continuous writes while something waits.
- Reading keystrokes, window titles or notification text, or logging any of them.
- omen-kbd editing the user's dotfiles or Claude Code settings. These changes ship as snippets the
  user applies.
- Blocking the daemon's loop on Hyprland. Hyprland is known to hang after a wake on this machine.
- A hook that can slow down or break Claude Code.

## 4. Facts this design relies on

- **Hardware and kernel** (P4, P5):
  - Four zones in the logical order left, middle, right, wasd.
  - One atomic write per frame, at least 50 ms apart.
  - The budget is 40,000 writes an hour, and a write costs about 3 ms of CPU.
  - Writes cannot wear flash, because the colors live in memory kept alive by standby power.
- **The daemon today:**
  - The writer skips a frame identical to the last one written.
  - While the backlight is off, nothing is written to the zones.
  - The loop wakes only at deadlines, plus regular ticks while an effect animates (`Daemon::next_wake`).
  - `compose()` is pure, and the model is pure: callers pass `now`.
- **Hyprland 0.57 event socket** (`socket2`, wiki "IPC", latest; the daemon is already connected for
  `submap>>`). Data columns, quoted from the wiki:
  - `workspacev2>>WORKSPACEID,WORKSPACENAME`. Emitted only when the user asks for a workspace change.
  - `openwindow>>WINDOWADDRESS,WORKSPACENAME,WINDOWCLASS,WINDOWTITLE`
  - `closewindow>>WINDOWADDRESS`
  - `urgent>>WINDOWADDRESS`
  - `activewindow>>WINDOWCLASS,WINDOWTITLE`
  - `activewindowv2>>WINDOWADDRESS`
  - `fullscreen>>0|1`. The wiki warns it "is not guaranteed to fire on/off once in succession"; some
    windows fire it several times.
  - The wiki does not say whether commas in titles or workspace names are escaped.
- **Hyprland request socket** (`.socket.sock`, same directory as socket2): `j/activewindow` returns
  the JSON that `hyprctl activewindow -j` prints, with `class` and `fullscreen` (a mode number).
  - What is known so far: with nothing fullscreen the value is `0`.
  - What still needs checking: which values mean true fullscreen and which mean Super+F's
    maximize. This is the first task of the plan (§8).
- **The user's Hyprland config:**
  - Super+F is `fullscreen({ mode = "maximized" })`, which must not dim.
  - Super+Shift+F is `fullscreen({ mode = "fullscreen" })`.
  - Video players and browsers request true fullscreen themselves.
- **swaync:**
  - It runs **one** script per notification. The user's `scripts.sound` entry runs
    `~/.config/rice/bin/rice-notify-sound` on every notification received, and that script checks
    do-not-disturb itself and detaches.
  - The script gets `SWAYNC_APP_NAME`, `SWAYNC_DESKTOP_ENTRY`, `SWAYNC_URGENCY` and others
    (`man 5 swaync`).
- **Claude Code hooks:**
  - Events: `Notification`, `UserPromptSubmit`, `PostToolUse`, `Stop` and `SessionEnd`.
  - `Notification` matchers include `permission_prompt`, `idle_prompt`, `elicitation_dialog` and
    `auth_success`.
  - The input arrives as JSON on stdin, with `hook_event_name`, `notification_type` and a session
    id. The plan checks the exact field names against the official hooks reference
    (code.claude.com/docs/en/hooks) before coding the script.
  - For some events (`UserPromptSubmit`, `SessionStart`) stdout is added to Claude's context. For
    `Stop`, exit code 2 blocks stopping. **So the hook prints nothing and always exits 0.**
  - The user already has one hook, a `SessionStart` hook in `~/.claude/settings.json`, which must
    be kept.
  - What the official reference says about timing (builder check, 2026-11-08):
    - `PermissionRequest` fires as soon as a prompt is about to show. It is **not used**: the
      `permission_prompt` notification fires only after the prompt has waited a few seconds, so
      approvals made within those few seconds cost 0 writes.
    - `PostToolUse` fires after a tool completes, and `PostToolUseFailure` after it fails.
    - Nothing fires at the moment of approval or denial.
    - `Stop` does not fire after a user interrupt.
- **Where Claude Code runs here** (checked 2026-11-08):
  - In **Alacritty**, which runs one process per window (pid 3544), with no tmux or zellij.
  - Every running session, **background jobs included**, descends from the interactive `claude`
    process in that window.
  - `preferredNotifChannel` is unset, so Claude Code uses its default.
  - Whether a prompt rings the bell, and whether Alacritty then marks itself urgent, is checked live
    (§8).
- **Repo:** C++23, CMake + Ninja, GoogleTest, ASan + UBSan, `-Werror`, `.clang-format` (4 spaces),
  clang-tidy.
  - The working tree has an **uncommitted reformat of `src/sources/hyprland.cpp`** by the user's
    editor: 2-space indents and rewrapped comments, no logic change.
  - This feature edits that file, so the user must agree to drop the reformat first, and it must
    never be staged as it is (see the memory note on the editor).

## 5. Design

### 5.1 Who knows what

The boundaries are what keep this simple. Each module is pure unless it says I/O.

| Module | Knows | Never knows |
|---|---|---|
| `HyprlandSource` (I/O) | socket2 connection, reconnects | colors, timing, window titles |
| `parse_socket2_line` (pure) | the line formats | anything stateful. Titles are dropped right here. |
| `HyprlandTracker` (pure) | current workspace id, focused window and class, urgent set | colors, timing |
| `HyprctlQuery` (I/O) | the request socket, timeouts | what fullscreen means for lighting |
| IPC and CLI `notify`, `agent` | the wire format | colors |
| `LookBook` (pure) | config → how each signal looks | time, I/O |
| `ClipPlayer`, `LampSet` (pure) | timing, coalescing, rate limit | where signals come from |
| `Reactions` (pure façade) | visibility rules, wiring the three above | sockets, the writer |
| `Model` (pure) | gates, facts, rules → compositor inputs | sockets |
| `compose()` (pure) | the pixel math of the layers | everything else |
| `Writer` (I/O) | the hardware | why a frame changed |

Data flows one way:

```
Hyprland socket2 ─► HyprlandSource ─► parse (pure) ─► HyprlandTracker (pure) ─► Signal / FactUpdate ─┐
Hyprland .socket.sock ◄─ HyprctlQuery (async, timeout) ◄─ "fullscreen may have changed" ──┘          │
swaync ─► rice-notify-sound ─► `omen-kbd notify` ─► req::Notify ─► Signal ───────────────────────────┤
Claude Code hooks ─► omen-kbd-agent-hook ─► `omen-kbd agent` ─► req::Agent ─► Signal ────────────────┤
                                                                                                     ▼
              Model ─ Reactions (LookBook + ClipPlayer + LampSet), rules (facts) ─► Inputs ─► compose() ─► Writer
```

### 5.2 Signals: what happened (`src/core/signals.hpp`)

A closed `std::variant`. It carries no colors, titles or notification text.

| Signal | From |
|---|---|
| `WorkspaceSwitched{int from, int to}` | tracker; only when both ids are > 0 and differ |
| `WindowOpened{}`, `WindowClosed{}` | tracker |
| `UrgentRaised{std::string window}`, `UrgentCleared{std::string window}`, `UrgentReset{}` | tracker |
| `FocusChanged{std::string window, std::string app_class}` | tracker. Emitted on `activewindowv2`, with the class from the `activewindow` line before it. Used to clear lamps on focus and to skip notifications from the focused app. |
| `Notified{std::string app, std::string desktop_entry, Urgency urgency}` | `req::Notify` |
| `AgentWaiting{session, std::optional<std::string> window}`, `AgentWorking{session}`, `AgentDone{session, std::optional<std::string> window}`, `AgentEnded{session}` | `req::Agent` |

Fullscreen is **not** a signal. It is a fact, `fact::FullscreenApp{std::optional<std::string> app_class}`,
because it is a state that the existing rules engine already knows how to act on (§5.8).

`Event` (in `src/daemon/events.hpp`) gains `Signal` as an alternative, so Hyprland signals reach the
model the way submaps do today.

### 5.3 Clips: a short reaction as data (`src/core/clip.hpp`)

- `ZoneOp = std::variant<Paint{Rgb}, Mix{Rgb to, double amount}, Scale{double factor}>`, applied to
  the frame below.
- `LayerOps = std::array<std::optional<ZoneOp>, 4>`, in logical zone order. `nullopt` means show
  what is below.
- `ClipStep{LayerOps ops; Duration hold;}` and `Clip{std::vector<ClipStep> steps;}`. A clip's write
  cost on a static profile is its number of visible steps, plus 1 to restore.
- Pure builders, unit-tested:
  - `sweep(Direction, Rgb, WasdWith, Duration step)` returns 3 steps.
  - `flash(Rgb, zones, int blinks, Duration on, Duration off)`.
  - `lift(double toward_white, Duration)` returns one `Mix` step.
  - `dip(double factor, Duration)` returns one `Scale` step.
- `apply(const Frame &below, const LayerOps &) -> Frame` (pure).

### 5.4 ClipPlayer: playing short reactions (`src/core/clip_player.hpp`)

- One playing slot plus one queue slot. Only one short reaction plays at a time, because four zones
  can show only one at a time readably.
- Each `Channel` has a policy:
  - `Workspace`: drop if busy.
  - `Window`: drop if busy, with a 500 ms cooldown measured from the last window clip's start.
  - `Notify` and `AgentDone`: queue one. A second one while one is queued is dropped.
- **Rate limit:** at most `max_per_minute` clips start per rolling minute, default 30, for all
  channels together. A storm costs at most about 30 × 4 writes a minute, whatever an app does.
- API (pure; every call takes `now`):
  - `offer(Channel, Clip, now) -> Offer{Started|Queued|Dropped}`
  - `layer(now) -> LayerOps`
  - `next_change(now) -> std::optional<TimePoint>`
  - `advance(now)`
  - `clear()`

### 5.5 LampSet: lights that hold (`src/core/lamps.hpp`)

- Kinds: `Agent` and `Urgent`. Each has:
  - zones, color, intro blink count, `blink_ms`, an optional TTL;
  - a priority, `Agent` above `Urgent`, used only if they share a zone.
- Each kind holds a set of keys: session ids or window addresses. A kind is lit while its set is
  not empty.
- The intro plays **only when a kind goes from no keys to some**, and only while the keyboard is
  visible and the user is present. After that it holds steady (`Paint`), at zero writes over time.
- Agent keys expire after `ttl_s`, default 3600, so a crashed session cannot leave the lamp on
  forever. Urgent keys are cleared by focus, close or a Hyprland disconnect.
- **Windows.** A key may carry the window it belongs to:
  - an urgent key's own address;
  - the terminal window an agent hook reported.

  Three rules use them:
  - Focusing a window clears every key linked to it.
  - An agent key is not recorded if its window is already focused when it is raised.
  - An urgent raise for a window that already has an agent key is absorbed: no second light for one
    event.
- API (pure):
  - `raise(kind, key, now, std::optional<std::string> window)`, `clear(kind, key)`,
    `clear_window(window)`, `clear_kind(kind)`, `expire(now)`
  - `layer(now) -> LayerOps`
  - `next_change(now)` (intro boundaries and TTL expiry)
  - `active() -> std::vector<LampKind>`

### 5.6 LookBook and Reactions

- `LookBook` (pure, built from config) turns a signal into a channel plus a clip, or into a lamp
  change. It resolves `ColorRef` through a resolver function it is given, so it never touches the
  palette's internals.
- `LookBook` owns the notification policy:
  - critical uses the critical color;
  - otherwise the app table, matched case-insensitively on the app name, the desktop entry, or the
    entry's last dot-separated segment;
  - otherwise the default color.
  - It skips `urgency < min_urgency`, and skips the notification when `skip_if_focused` is set and
    the focused class matches the app the same way.
- `Reactions` (pure façade, owned by `Model`; `src/daemon/reactions.hpp`):
  - `on_signal(const Signal &, now, Visibility)`
  - `layers(now) -> {LayerOps lamps, LayerOps transient}`
  - `next_change(now)`
  - `suppress()`, which clears the short clips when the keyboard goes dark or the user leaves
  - `reconfigure(const ReactionsCfg &)`, called on `reload`. Lamps keep their keys; a playing clip
    finishes with its old look.
  - `status()`
- `Visibility` is computed by `Model` from what it already has:
  - `keyboard_visible = !suspended && backlight_wanted() && effective_brightness > 0 && hw_backlight != false && !budget_exhausted`
  - `user_present = !locked && !idle`
  - Short clips need both; lamps need `keyboard_visible` to show.

### 5.7 Compositor and daemon changes

- `Inputs` gains `LayerOps lamps` and `LayerOps transient`. `compose()` applies them in the §2
  order. `Output.animated` keeps its meaning: clips never set it.
- `Model` gains:
  - `apply_signal(const Signal &, now)`
  - `reaction_deadline() -> std::optional<TimePoint>`
  - the `req::Notify` and `req::Agent` handling, which converts each request into a `Signal`
  - a `reactions` part of `Status`
- `Daemon::next_wake()` also considers `model_.reaction_deadline()`, **but only while the last
  output was not animated**. During an effect, the effect's ticks sample the clips, so reactions add
  no wake-ups and no writes. `on_wake()` recomputes when the reaction deadline is due.
- `notify` and `agent` requests are answered as soon as the model has applied them. Unlike `set`,
  they do not wait for the keyboard write, so hooks return fast.
- **Read before start.** The Fn key changes the firmware backlight without any event. So before
  delivering a signal that could start a short clip or a lamp intro, the daemon checks its last
  backlight reading. If that reading is unknown or older than 5 s, the daemon asks the writer thread
  for a read and delivers the signal when the reading arrives.
  - If the read times out (the existing reply timeout), the signal is delivered as if the backlight
    were on, which is today's behavior.
  - `notify` and `agent` requests reuse the existing wait-for-read path (`needs_backlight_read`,
    `awaiting_probe_`), but only when the reading is stale. Hyprland signals get a small pending
    list of their own.
  - Lamp state (raise, clear) is never lost, only delayed by one read.
  - Reads happen only when reactions do, at most one every 5 s, and never periodically.

### 5.8 Fullscreen through the rules engine

- `Facts.fullscreen_app: std::optional<std::string>` holds the lowercased class of the focused
  window while it is in true fullscreen, and nullopt otherwise. It appears in `status`.
- `Condition.fullscreen_app: std::optional<std::vector<std::string>>` holds when the fact is set and
  its class is in the list (case-insensitive). `["*"]` matches any fullscreen app.
- The example rule:

  ```toml
  [[rules]]
  name = "fullscreen video dims"
  when = { fullscreen_app = ["mpv", "vlc", "firefox", "chromium"] }
  brightness_cap = 15
  ```

- Rules stay **first-match per category**, so this rule must come before "power saver dims" in the
  user's file. The snippet and README say so. Changing caps to "lowest wins" was considered and
  rejected, because it changes existing behavior.

### 5.9 Hyprland side

- **`parse_socket2_line`** returns `std::optional<HyprEvent>`, one of:
  - `Submap`
  - `WorkspaceV2{int id}`
  - `OpenWindow{addr}`
  - `CloseWindow{addr}`
  - `Urgent{addr}`
  - `ActiveWindowV2{addr}`
  - `ActiveWindow{app_class}`: the text before the first comma; the title is dropped.
  - `Fullscreen{bool}`
  - `ConfigReloaded`

  Fields are split only as far as needed, so a comma in a title or workspace name cannot shift a
  field that is used. A malformed line returns nullopt.
- **`HyprlandTracker`** (pure; `src/sources/hyprland_tracker.hpp`):
  - `on(const HyprEvent &) -> std::vector<Out>`, where `Out` is a `Signal`, a `FactUpdate`, an
    `event::Submap`, or `CheckFullscreen`.
  - `on_disconnect()` emits `UrgentReset`, `FullscreenApp{nullopt}` and `Submap{nullopt}`.
  - Workspace direction needs a known previous id. The first switch after a (re)connect only
    records the id, unless the builder seeds it with a query.
  - Urgent: ignored for the focused window; cleared on `activewindowv2` to that window or on its
    `closewindow`.
  - `CheckFullscreen` follows every `fullscreen`, `activewindowv2`, `workspacev2` and `closewindow`.
- **`HyprctlQuery`** (I/O; `src/sources/hyprctl_query.hpp`): one async request on the daemon's
  loop.
  - It connects without blocking to `.socket.sock` in the instance directory socket2 is using,
    writes `j/activewindow`, and reads to EOF.
  - Limits: a 300 ms timeout, a 64 KiB cap, and at most one request in flight (the latest wins).
  - It is debounced, 100 ms after the last `CheckFullscreen`.
  - The result becomes `FullscreenApp{class}` when the window is in true fullscreen (the predicate
    comes from the §8 spike), and `FullscreenApp{nullopt}` otherwise. It is emitted only when it
    changes.
  - On a timeout or error the last value stays, so a hung Hyprland cannot make the light flap.
- **Privacy:** titles are dropped in the parser. Window addresses live in memory only, for the
  urgent set. The focused class stays inside `Reactions` and is not put in `Facts` or `status`; the
  only exception is `fullscreen_app`, which the rules need. Nothing from Hyprland is logged except
  errors.

### 5.10 IPC and CLI (`src/ipc/protocol.*`, `src/cli/*`)

**Commands:**
- `omen-kbd notify [--app TEXT] [--desktop-entry TEXT] [--urgency low|normal|critical]`
  - `req::Notify`. Urgency is case-insensitive, because swaync sends `Low`, `Normal`, `Critical`.
- `omen-kbd agent <waiting|working|done|end> --session ID [--window ADDR] [--source NAME]`
  - `req::Agent`. `source` defaults to `claude` and is informational for now.
  - `--window` is the Hyprland address of the session's terminal window, when the hook found one.
    The daemon uses it for the three window rules (§5.5) and to skip the `done` flash while that
    window is focused, so the hook itself decides nothing about focus.

**Wire format and exit codes:**
- kebab-case JSON, like the existing requests: `{"cmd":"notify","app":…,"desktop-entry":…,"urgency":"normal"}`.
- Exit codes are the existing ones. A bad state or urgency gives 64.

**Status:**
- `status --json` gains `"reactions": {"enabled": bool, "lamps": ["agent", …], "playing": "workspace" | null}`.
- `status` text gains one line in the same spirit.

### 5.11 Config (all optional; a missing `[reactions]` table means off)

```toml
[reactions]
enabled = true
step_ms = 120            # one step of a sweep or pulse (60–1000)
blink_ms = 150           # one on or off phase of a blink (60–1000)
max_per_minute = 30      # short reactions started per rolling minute (1–600)

[reactions.workspace]
color = "theme:lavender"
wasd_with = "left"       # "left" | "middle" | "none"

[reactions.window]
open = 0.5               # move toward white by this much (0–1; 0 = off)
close = 0.35             # dip to this fraction (0–1; 1 = off)
cooldown_ms = 500        # (0–10000)

[reactions.notify]
color = "theme:blue"
critical = "theme:red"
min_urgency = "normal"   # "low" | "normal" | "critical"
skip_if_focused = true
[reactions.notify.apps]  # matched case-insensitively (§5.6)
zapzap = "theme:green"
discord = "#5865f2"

[reactions.urgent]
zones = ["right"]
color = "theme:yellow"
blinks = 2               # (0–5); then steady

[reactions.agent]
zones = ["wasd"]
waiting = "theme:peach"
done = "theme:green"
blinks = 2
ttl_s = 3600             # (60–86400)
```

- Strict like the rest of the config: unknown keys and out-of-range values are errors that name the
  field.
- A missing sub-table uses the defaults above. A reaction is turned off with its own zero or
  neutral value (`open = 0`, `close = 1`, `blinks = 0` plus an empty `zones` list), or with
  `enabled = false`.
- `packaging/config.example.toml` (the built-in default) gets the whole block and the fullscreen
  rule.

### 5.12 Producers (shipped as snippets; omen-kbd never installs them into the user's files)

- **Notifications:** a snippet for the user's `rice-notify-sound`. Inside its existing "not in DND"
  branch it adds one detached line:
  `~/.local/bin/omen-kbd notify --app "$SWAYNC_APP_NAME" --desktop-entry "$SWAYNC_DESKTOP_ENTRY" --urgency "$SWAYNC_URGENCY"`.
  - Its output goes to `/dev/null`.
  - It is not a second swaync script entry, because swaync runs only one per notification.
- **Claude Code:** `packaging/agent-hook/omen-kbd-agent-hook`, a bash + jq script that CMake
  installs next to the binary. It maps each hook to a state:
  - `Notification` (`permission_prompt|idle_prompt|elicitation_dialog`) → `waiting --window ADDR`
  - `UserPromptSubmit`, `PostToolUse` and `PostToolUseFailure` → `working`, only if a per-session
    marker in `$XDG_RUNTIME_DIR/omen-kbd/agent/` says it was waiting
  - `Stop` → `done --window ADDR`
  - `SessionEnd` → `end`

  How it finds `ADDR`, once per session, cached next to the marker:
  - find the session's own `claude` process, the nearest `claude` among the hook's ancestors;
  - **if that process's parent is another `claude` process, the session is nested** (a background
    job or subagent), so pass **no** `--window`;
  - otherwise keep walking up and take the first ancestor whose pid owns a window in
    `hyprctl clients -j`;
  - no match (a multiplexer, a terminal that serves several windows from one process, or no window)
    also means no `--window`, and the lamp then falls back to the hooks and the TTL.

  The ancestry source (normally `/proc`) is overridable, so tests can supply fake process trees.

  Here, the interactive session `claude` 3795 sits under `bash` 3568 under Alacritty 3544, so it is
  linked. The background jobs sit under `claude` 3872, so they are not.

  Its rules:
  - It prints nothing, always exits 0, and caps each call with `timeout 1`.
  - `OMEN_KBD_BIN` overrides the binary path, for tests.
  - Missing fields make it do nothing.
- **Settings:** `packaging/snippets/claude-code-hooks.json` holds the `hooks` block the user merges
  into `~/.claude/settings.json`, next to their existing `SessionStart` hook.
- **Config:** `packaging/snippets/reactions-config.toml` holds the §5.11 block plus the rule, to
  paste into `config.toml`.
- **Docs:** README sections: Reactions, two new command rows, desktop-integration items.

## 6. Verification

- **Unit tests, pure, exact:**
  - clip builders, giving step counts and colors;
  - `ClipPlayer` policies, cooldown, rate limit and `next_change`;
  - `LampSet` keys, intro, TTL and zone priority;
  - `apply` and `compose` layer order;
  - `LookBook` notification policy;
  - the parser, including titles that contain commas and `>>`, and malformed lines;
  - the tracker: direction, the urgent lifecycle, `CheckFullscreen` triggers, disconnect;
  - the rules condition `fullscreen_app`;
  - config parsing and validation;
  - protocol round trips;
  - CLI parsing.
- **Write-count tests (the G1 contract), deterministic:**
  - A test harness drives `Model` through its deadlines with a fake `now`, composes each frame, and
    counts frame changes. That equals what the writer would write, since it skips identical frames.
  - It asserts §2's numbers exactly:
    - sweep 4; five fast switches 4;
    - window storm (20 opens in 1 s) ≤ 4; notify burst (10 in 200 ms) ≤ 4;
    - agent wait 4, finish 2; urgent episode 4; fullscreen in and out 2, with repeated
      `fullscreen` events adding 0;
    - a lamp held for 60 s adds 0;
    - locked, idle or off gives 0;
    - with an animated base, the number of frame changes over 1 s equals the effect's own, ±1;
    - rate limit: 100 window events over a minute start ≤ 30 clips;
    - the window rules: `waiting` while its window is focused gives 0; focusing the window
      afterwards clears it (+1); an urgent raise on the agent's window gives 0; `done` while its
      window is focused gives 0.
  - **Read before start**, in daemon tests with the mock backend: a stale reading triggers one read.
    If the backlight reads off, the reaction writes 0; if it reads on, the reaction plays. Two
    reactions within 5 s cause one read.
  - A few daemon-level tests with the mock backend check the wiring, using the current `wait_until`
    style.
- **Hook script test:** feed sample JSON for each event to the script, with `OMEN_KBD_BIN` pointing
  at a fake that records its arguments.
  - Assert the calls, including `--window` for a top-level session and none for a nested one, using
    fake process trees.
  - Assert that nothing is printed, and assert exit 0 even when the fake fails or is missing.
  - shellcheck must pass.
- **Live checks** (§8), with the user present.

## 7. Risks

| Risk | Mitigation |
|---|---|
| The fullscreen mode numbers are not what we expect | The §8 spike decides the predicate before coding it. |
| Hyprland hangs after a wake | The query is async, with a timeout, and keeps its last value. socket2 already reconnects. |
| Event formats drift between Hyprland versions | The parser ignores unknown lines and splits fields only as far as needed. |
| Claude Code's hook schema drifts | The script ignores missing fields, and its matchers can be edited in the snippet. |
| The lamp stays amber because `idle_prompt` fires 60 s after every finished turn (background sessions included) | The 1-hour TTL, a new prompt, or `SessionEnd` clears it. If it gets noisy, the user drops `idle_prompt` from the matcher. |
| Visual noise | Every reaction can be switched off on its own in config. |
| An approval or denial made remotely (claude.ai, phone) never focuses the terminal | The lamp then clears when the tool finishes, at the next prompt, or by TTL. A long approved command keeps it amber until it ends. |
| Background jobs run under the interactive session's process, in the same terminal | The hook links only sessions started directly in a terminal. Nested ones get no link, so their lamp lights and their "finished" flash shows even while that terminal is focused. |
| Terminals with one process for many windows (foot server, kitty single-instance), or tmux and zellij, cannot be linked | No `--window`: the lamp falls back to the hooks and the TTL. This is not the user's setup today. |
| A misbehaving app opens and closes windows all the time | The 500 ms cooldown plus the rolling-minute limit (§5.4). |

## 8. Live checks (each step approved by the user; normal daemon writes)

1. **Fullscreen spike (read-only):** while the user puts mpv in fullscreen, then uses Super+F, then
   Super+Shift+F on a browser, record `hyprctl activewindow -j` → `class`, `fullscreen` and
   `fullscreenClient`. This fixes the true-fullscreen predicate.
2. **Each reaction once,** while watching `omen-kbd status` → `lifetime_writes`. The changes must
   match §2.
3. **A ZapZap notification:** compare the app name and desktop entry with Hyprland's class, so
   skip-if-focused matches.
4. **Claude lamp:** in a scratch session with another window focused:
   - a permission prompt turns WASD amber after a few seconds;
   - focusing the terminal clears it;
   - finishing gives green, and no flash when the terminal is focused.

   - a background job that waits while the terminal **is** focused still lights the lamp.

   Also note whether the prompt makes Alacritty urgent. If it does, the absorb rule must keep the
   right zone dark.

## 9. Left to the builder

- File and type names beyond those named here.
- Internal containers.
- Whether `LookBook` sits in `src/core` or `src/daemon`.
- The exact `status` text.
- Seeding the workspace id with a query on connect.
- `doctor` additions: for example, reporting whether `[reactions]` is on and whether the Claude
  hooks reference `omen-kbd-agent-hook`.
- Default colors and timings within the stated ranges.

## 10. Decisions

- **The user's, 2026-11-08:**
  - the six reactions in §2;
  - minimal writes for each;
  - clean abstraction and encapsulation;
  - blink, then hold, for both waiting lights.
- **My calls (please check):**
  - Absorbing workspace switches during a sweep, and the one-clip-plus-one-queued player.
  - The 500 ms window cooldown, and the rate limit of 30 a minute.
  - Silence while the user is away. The lamps keep state and come back steady.
  - Reactions add nothing on top of a running effect.
  - Short reactions draw above the mode hint; lamps draw below it.
  - The notification policy: skip low urgency and the focused app; an app color table.
  - The Claude lamp on WASD; the attention light on the right zone.
  - `idle_prompt` counts as "waiting".
  - Fullscreen as a rules fact, first-match kept.
  - Typed `notify` and `agent` commands, not a generic event command.
  - Reactions off unless `[reactions]` is present.
  - Answering `notify` and `agent` without waiting for the write.
  - From the builder check:
    - **For a session started directly in a terminal, the lamp clears when you focus that
      terminal, and does not light while it is focused.** In practice this matches "goes back when
      you answer", because nothing fires at the moment of approval. An urgent signal from the same
      window is absorbed. Background jobs are never linked.
    - Read before start, with a 5 s freshness window.
    - Using the delayed `permission_prompt` notification, not `PermissionRequest`.
    - Adding `PostToolUseFailure` to the clears.
    - The daemon, not the hook, decides the "finished" skip.
