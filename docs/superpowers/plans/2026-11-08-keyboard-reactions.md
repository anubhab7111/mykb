# Keyboard Reactions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the OMEN keyboard's four zones react to what the user does and to what happens on the desktop (workspace sweep, window pulses, attention light, fullscreen dim, notification flash, Claude lamp), each at a fixed, tested, minimal write cost.

**Architecture:** Producers report *what happened* as typed `Signal`s: the daemon's Hyprland reader, `omen-kbd notify` (swaync) and `omen-kbd agent` (Claude Code hooks). A pure `Reactions` façade turns signals into two layers, `lamps` and `transient`, that `compose()` draws between the profile and the brightness stage. Its parts are `LookBook` (how a signal looks), `ClipPlayer` (short reactions with policies) and `LampSet` (lights that blink, then hold). Fullscreen is a rules *fact*. The writer is unchanged and writes only when the frame changes, so write cost equals the number of distinct frames.

**Tech Stack:** C++23, CMake + Ninja, GoogleTest (ASan + UBSan), toml++, nlohmann_json, CLI11, the existing epoll loop. bash + jq for the Claude Code hook.

**Spec:** `docs/superpowers/specs/2026-11-08-keyboard-reactions-design.md` (branch `keyboard-reactions`, commit `b61f8db`). Executors read it first; §2 is the contract.

## Context

The user picked six reactions from a brainstorm (2026-11-08) and asked for minimal writes per feature and clean abstraction and encapsulation. The spec was approved after a fresh-reader check. This plan builds it bottom-up: pure core first (every piece unit-tested to exact values), then the model, the daemon, the Hyprland and hook producers, and last the user-present live checks.

## Decisions at approval

- **Execution method: subagent-driven.** This is the user's choice (2026-11-08); I had recommended native.
  - A fresh implementer and a fresh reviewer per task, then one whole-branch review.
  - Each implementer sees only its own task, plus Global Constraints and the Rulings. That is why every task carries an Interfaces block with exact signatures.
  - The controller (not a subagent) runs Task 0, Task 1 and steps 17.3–17.4, because they need the user or write scripts outside the repo. Subagents take Tasks 2–16.
- **Editor reformat of `src/sources/hyprland.cpp`: discard it.** The user said yes. Task 0.2 runs `git restore src/sources/hyprland.cpp`.

## Global Constraints

- **Hardware and kernel (spec §4):**
  - Four zones, logical order left, middle, right, wasd. One atomic 4-zone write per frame.
  - The kernel enforces ≥ 50 ms between writes and 40 000 writes/hour.
  - Never write under `/sys`, never load a module, never use sudo. Every automated test uses the mock backend or pure objects.
- **Privacy:** only a window's address and class are kept, in memory. Titles are dropped inside the socket2 parser. Nothing from Hyprland, swaync or Claude Code is logged except errors.
- **Dotfiles:** omen-kbd never edits `~/.config/hypr`, `~/.config/omen-kbd/config.toml`, `~/.claude/settings.json` or `rice-notify-sound`. These ship as snippets the user applies.
- **C++:**
  - `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`, `.clang-format` (4 spaces, 100 columns). Run `clang-format -i` on touched files, and clang-tidy stays clean.
  - Pure core code includes no daemon header and has no clock: callers pass `TimePoint now`.
  - `Output.animated` keeps its meaning: clips and lamps never set it.
- **Git:**
  - Branch `keyboard-reactions`. Stage by explicit path. Never push to main, never force-push; the user merges.
  - Commit messages end with the attribution lines from the session reminder (`Co-Authored-By: …` and `Claude-Session: …`).
- **Environment guard:** plain commands, no `git -C`, no inline awk programs, no complex pipelines. Scratch scripts go under `$CLAUDE_JOB_DIR/tmp`.
- **Live checks:** the user is present, one step at a time, through the normal CLI and daemon only. Never ask the user to unplug the charger.
- **Commands used throughout:**
  - Build: `cmake --build build --target omen-kbd-tests`.
  - Run: `./build/omen-kbd-tests --gtest_filter='<Suite>.*'`.
  - Full suite: `ctest --test-dir build --output-on-failure`.
  - A new `.cpp` is picked up on the next build (the globs use `CONFIGURE_DEPENDS`).

## Rulings made while planning (the spec stays the authority; each is cheap to reverse)

- **R1, `blinks`.** `blinks = n` counts lit phases: ON, OFF, ON, … with n lit phases, the last one steady. 0 and 1 both mean "light steadily at once". An empty `zones` list disables a lamp. This makes the spec's "(on, off, on)" and its "4 per episode" agree: 3 writes to light, 1 to clear. Cost if wrong: one constant.
- **R2, gap.** A queued short clip starts `blink_ms` after the previous one ends. Otherwise two identical back-to-back flashes merge into one long flash and cost fewer writes than the spec's "≤ 4".
- **R3, pending signals.** One pending-signal list covers every signal, and the read-before-start rule lives in `Daemon::deliver_signal`. `notify` and `agent` are answered at once.
  - The spec said to reuse `awaiting_probe_`, but that path drops a request whose client has gone. Hooks time out after 1 s, and the lamp state would be lost.
  - The user-visible behavior is the spec's.
- **R4, off means off.** While `[reactions]` is off, signals are dropped before any probe or query. The fullscreen query runs only while a rule uses `fullscreen_app` (`Config::needs_fullscreen()`, like `needs_cpu_temp()`).
- **R5, clock.** The `Clock`/`TimePoint`/`Duration` aliases move to `src/core/clock.hpp`. `src/daemon/clock.hpp` includes it, so the pure core includes no daemon header.
- **R6, Hyprland structure.** `HyprlandSource` emits `HyprEvent` and gains `Connected`/`Disconnected`. A new `HyprlandBridge` owns source, tracker and query, and is the only thing `run_daemon` wires. `ConfigReloaded` is not parsed because it has no consumer.
- **R7, cached boundary.** `Reactions` caches its next boundary at the end of `on_signal` and `update`. `Model::reaction_deadline()` returns it.
- **R8, addresses.** Window addresses are normalized (lowercase hex, no `0x`) at the two edges: the socket2 parser and `to_signal`. Hyprland's events omit `0x`, and `hyprctl clients -j` includes it.
- **R9, fullscreen predicate.** True fullscreen is `(fullscreen & 2) != 0` (Hyprland's mode bits: 1 maximized, 2 fullscreen) until Task 1's spike confirms it.
- **R10, focus.** Focus is tracked from `activewindowv2`, with the class taken from the preceding `activewindow` line. There is no query-seeding at connect. Before the first focus event, "skip if focused" does not skip.
- **R11, not in this plan.** `doctor` additions (the spec leaves them to the builder), and lamp state surviving a daemon restart (the lamp is dark until the next event).
- **R12, nested-session rule verified (2026-11-08, read-only).** On this machine the interactive session is one `claude` process directly under the shell (alacritty ← bash ← claude). Background sessions are `claude bg-pty-host …` (the host) ← `claude --session-id … --agent claude` (the session), and idle spares are `bg-pty-host` ← `bg-spare`; all of them sit under `claude daemon run --origin transient`, which the interactive `claude` spawned. Every one reports `comm` = `claude`.
  - So "the session's own `claude` has a `claude` parent" marks exactly the background sessions, and the interactive session links to its window.
  - Only background and spare processes carry `CLAUDE_CODE_SESSION_KIND` / `CLAUDE_BG_*` / `CLAUDE_JOB_DIR`. That is an undocumented fallback if the ancestry rule ever breaks, not a second rule.

## Review Focus (each line is pinned by a test in the owning task)

1. A time jump (suspend/resume, a stalled loop) must not replay or backfill clips and intros → Task 4 `ATimeJumpRetiresFinishedClipsWithoutReplayingThem`, Task 5 `ALateSampleAfterTheIntroShowsSteady`.
2. A config reload while a reaction is active, including turning `[reactions]` off → Task 10 `ReconfigureKeepsLampKeysAndLetsAPlayingClipFinish`, `ReconfigureToDisabledClearsLampsAndClips`.
3. Hostile or odd socket2 lines (commas and `>>` in titles, bad numbers, huge lines) → Task 14 `HyprlandParse.*`.
4. Hook robustness: bad JSON, no `jq`, missing or hanging `omen-kbd`, nested sessions → Task 16 `AgentHook.*`.
5. A lamp left lit by a missed event (Hyprland restart, killed agent) → Task 5 TTL, Task 10 `UrgentResetClearsTheAttentionLight`, Task 14 `DisconnectClearsUrgentFullscreenAndSubmap`.
6. A signal flood while a backlight read is pending → Task 13 `FloodWhileAReadIsPendingStaysBounded`.

## File structure

| File | Responsibility |
|---|---|
| `src/core/clock.hpp` | `Clock`, `TimePoint`, `Duration` (R5) |
| `src/core/signals.{hpp,cpp}` | `Urgency`, the closed `Signal` variant, `may_start_reaction` |
| `src/core/clip.{hpp,cpp}` | `ZoneOp`, `LayerOps`, `Clip`, builders, `apply` |
| `src/core/clip_player.{hpp,cpp}` | one playing slot + one queued, channel policies, rate limit |
| `src/core/lamps.{hpp,cpp}` | `LampSet`: keyed lights that blink then hold, TTL, window links |
| `src/core/reactions_cfg.hpp` | `ReactionsCfg` (pure data, spec §5.11 defaults) |
| `src/core/reactions_status.hpp` | `ReactionsStatus` (shared by the façade and the IPC status) |
| `src/core/lookbook.{hpp,cpp}` | `LookBook`: config → how each signal looks; the notification policy |
| `src/reactions_config.{hpp,cpp}` | `parse_reactions` (strict TOML) |
| `src/daemon/reactions.{hpp,cpp}` | `Reactions` façade, `Visibility`, `Layers` |
| `src/sources/hyprland_tracker.{hpp,cpp}` | pure: `HyprEvent` → signals, facts, fullscreen checks |
| `src/sources/hyprctl_query.{hpp,cpp}` | async one-shot `j/activewindow` over the request socket |
| `src/sources/hyprland_bridge.{hpp,cpp}` | wires source + tracker + query into daemon `Event`s |
| `packaging/agent-hook/omen-kbd-agent-hook` | bash + jq Claude Code hook |
| `packaging/snippets/{reactions-config.toml,claude-code-hooks.json,notify-reaction.sh}` | pieces the user applies |
| Modified | `config.{hpp,cpp}`, `core/{rules,compositor}.*`, `daemon/{model,events,daemon}.*`, `ipc/{protocol,socket}.*`, `cli/{cli,format}.cpp`, `sources/hyprland.*`, `backend/mock.*`, `CMakeLists.txt`, `packaging/config.example.toml`, `README.md` |

Tests: one `tests/test_<piece>.cpp` per new unit, plus `tests/reaction_rig.hpp` and `tests/test_reaction_costs.cpp` (Task 11). Existing test files grow where noted.

## Task index

| # | Task | Output |
|---|---|---|
| 0 | Preflight | plan saved, baseline green, reformat decision applied |
| 1 | Confirm two external facts | fullscreen mode values, Claude hook JSON fields |
| 2 | Clock + signals | `core/clock.hpp`, `core/signals.*` |
| 3 | Clips | `core/clip.*` |
| 4 | ClipPlayer | `core/clip_player.*` |
| 5 | LampSet | `core/lamps.*` |
| 6 | `[reactions]` config | `ReactionsCfg`, strict parser, example config |
| 7 | LookBook | `core/lookbook.*` |
| 8 | Compositor layers | `Inputs.lamps` / `Inputs.transient` |
| 9 | Fullscreen fact + rule | `Facts.fullscreen_app`, `Condition.fullscreen_app`, example rule |
| 10 | Reactions façade | `daemon/reactions.*` |
| 11 | Model integration + the write-count contract | `Model` API, status, rig, G1 tests |
| 12 | IPC + CLI `notify` / `agent` | requests, `to_signal`, commands, status line |
| 13 | Daemon wiring | signals, deadlines, read-before-start |
| 14 | Hyprland events + bridge | parser, tracker, bridge, wiring |
| 15 | Fullscreen via hyprctl | `HyprctlQuery`, bridge integration |
| 16 | Hook script, snippets, docs | the producers the user applies |
| 17 | Verify, live checks, finish | whole-branch review, draft PR |

---

### Task 0: Preflight (no product code)

**Files:** Create `docs/superpowers/plans/2026-11-08-keyboard-reactions.md` (a copy of this plan).

- [ ] **0.1** Copy this plan to the file above. `git add docs/superpowers/plans/2026-11-08-keyboard-reactions.md`, commit `docs: implementation plan for keyboard reactions`.
- [ ] **0.2** The user approved discarding the editor's reformat: run `git restore src/sources/hyprland.cpp` and confirm `git status --short` prints nothing. If the editor reformats that file again later, never stage its 2-space form.
- [ ] **0.3** Baseline: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOMEN_KBD_SANITIZE=ON`, then `cmake --build build`, then `ctest --test-dir build --output-on-failure`.
  Expected: all pass. Note the test count and which tests skip (luac, dbus-daemon), so later runs are comparable.

### Task 1: Confirm two external facts (read-only)

**Files:** Modify `docs/superpowers/specs/2026-11-08-keyboard-reactions-design.md` §4 and §5.12. Create `~/omen-kbd-reactions/fullscreen-spike.sh` and `~/omen-kbd-reactions/socket2-order.sh` (outside the repo).

- [ ] **1.1** Write the spike script. It polls `hyprctl activewindow -j` every 0.5 s for 40 s and prints only `class`, `fullscreen` and `fullscreenClient` (never titles) to `~/omen-kbd-reactions/fullscreen-spike.log`. It is read-only and refuses to run as root.
- [ ] **1.2** Ask the user to run it and, within 40 s:
  - (a) put a video fullscreen with the player's own button;
  - (b) press Super+F in a terminal;
  - (c) press Super+Shift+F in a terminal.

  Read the log. Expected: (a) and (c) show bit 2 set (`2`), (b) shows `1`. Edit spec §4 ("What still needs checking") with the observed values. If they differ from R9, change only `is_true_fullscreen` in Task 15. If the user defers, keep R9's default.
- [ ] **1.3** Fetch `https://code.claude.com/docs/en/hooks` and confirm:
  - the stdin field names `session_id`, `hook_event_name` and `notification_type`;
  - the events `Notification`, `UserPromptSubmit`, `PostToolUse`, `PostToolUseFailure`, `Stop` and `SessionEnd`.

  They become the sample JSON in Task 16. If a name differs, only Task 16's jq paths and its snippet change.
- [ ] **1.4** Confirm the order and format of Hyprland's focus events (R10, R8). Write `~/omen-kbd-reactions/socket2-order.sh` (read-only, outside the repo). It listens on socket2 for 20 s and prints only event names, plus the data of `activewindowv2`, `closewindow`, `urgent` and `fullscreen` (addresses and 0/1). It never prints the data of `activewindow` or `openwindow`, which carry titles.
  - Ask the user to switch focus between two windows a few times, open and close a terminal, and switch workspace.
  - Expected: each focus change shows `activewindow` and then `activewindowv2`, and addresses have no `0x` prefix.
  - If the order is reversed, the tracker emits `FocusChanged` on `ActiveWindow`, using the address from the preceding `activewindowv2`. That is a two-line change to Task 14's table.
  - If addresses do carry `0x`, R8's normalization already covers it.
- [ ] **1.5** Update spec §5.12's process example with the layout in R12 (its pids are stale and it omits the daemon/host pairs). Commit the spec edits: `docs: reactions design, Hyprland and process facts confirmed`.

### Task 2: Clock aliases and signals

**Files:** Create `src/core/clock.hpp`, `src/core/signals.hpp`, `src/core/signals.cpp`, `tests/test_signals.cpp`. Modify `src/daemon/clock.hpp`.

**Interfaces:**
- Produces:
  - `core/clock.hpp`: `using Clock = std::chrono::steady_clock; using TimePoint = Clock::time_point; using Duration = Clock::duration;`.
  - `enum class Urgency { Low, Normal, Critical }` (ordered), `std::optional<Urgency> urgency_from_name(std::string_view)` (case-insensitive), `std::string_view name(Urgency)`.
  - `namespace signal`, every struct with `friend bool operator==(const T &, const T &) = default`:

    | Struct | Fields |
    |---|---|
    | `WorkspaceSwitched` | `int from, to` |
    | `WindowOpened`, `WindowClosed`, `UrgentReset` | (none) |
    | `UrgentRaised` | `std::string window` |
    | `UrgentCleared` | `std::string window` |
    | `FocusChanged` | `std::string window, app_class` |
    | `Notified` | `std::string app, desktop_entry; Urgency urgency` |
    | `AgentWaiting` | `std::string session; std::optional<std::string> window` |
    | `AgentWorking` | `std::string session` |
    | `AgentDone` | `std::string session; std::optional<std::string> window` |
    | `AgentEnded` | `std::string session` |

  - `using Signal = std::variant<…all of the above…>`.
  - `bool may_start_reaction(const Signal &)`: true for `WorkspaceSwitched`, `WindowOpened`, `WindowClosed`, `UrgentRaised`, `Notified`, `AgentWaiting`, `AgentDone`.

- [ ] **2.1** Write `tests/test_signals.cpp`:
  ```cpp
  TEST(Signals, UrgencyNamesAreCaseInsensitive) {
      EXPECT_EQ(urgency_from_name("Critical"), Urgency::Critical);
      EXPECT_EQ(urgency_from_name("LOW"), Urgency::Low);
      EXPECT_FALSE(urgency_from_name("urgent").has_value());
  }
  TEST(Signals, UrgencyOrdersLowBelowNormalBelowCritical) {
      EXPECT_LT(Urgency::Low, Urgency::Normal);
      EXPECT_LT(Urgency::Normal, Urgency::Critical);
  }
  TEST(Signals, OnlyStartingSignalsMayStartAReaction) {
      EXPECT_TRUE(may_start_reaction(signal::WindowOpened{}));
      EXPECT_TRUE(may_start_reaction(signal::AgentWaiting{"s", std::nullopt}));
      EXPECT_FALSE(may_start_reaction(signal::FocusChanged{"a", "b"}));
      EXPECT_FALSE(may_start_reaction(signal::AgentWorking{"s"}));
      EXPECT_FALSE(may_start_reaction(signal::UrgentReset{}));
  }
  ```
- [ ] **2.2** Run the tests. Expected: the build fails (`core/signals.hpp: No such file`).
- [ ] **2.3** Implement. `daemon/clock.hpp` becomes `#include "core/clock.hpp"` with no definitions of its own (R5).
- [ ] **2.4** Run the build and `Signals.*`, then the full suite. Expected: all pass, and the suite is still green with the same skips as Task 0.3.
- [ ] **2.5** Commit `feat: signals and core clock aliases` with the explicit paths.

### Task 3: Clips

**Files:** Create `src/core/clip.hpp`, `src/core/clip.cpp`, `tests/test_clip.cpp`.

**Interfaces:**
- Consumes: `Rgb`, `Rgb::lerp`, `multiply` (`core/effects.hpp`), `Frame`, `Zone`, `index()` (`core/zones.hpp`), `Duration`.
- Produces:
  ```cpp
  struct Paint { Rgb color; };                  // replaces the zone
  struct Mix   { Rgb toward; double amount; };  // Rgb::lerp(below, toward, amount)
  struct Scale { double factor; };              // multiply(below, factor)
  using ZoneOp = std::variant<Paint, Mix, Scale>;                  // each with defaulted ==
  using LayerOps = std::array<std::optional<ZoneOp>, 4>;           // logical order; nullopt = show what is below
  struct ClipStep { LayerOps ops; Duration hold; };
  struct Clip {
      std::vector<ClipStep> steps;
      Duration total() const;
      LayerOps ops_at(Duration elapsed) const;                     // empty ops at/after total()
      std::optional<Duration> next_boundary(Duration elapsed) const; // strictly after elapsed; nullopt at/after the end
  };
  bool empty(const LayerOps &);
  Frame apply(const Frame &below, const LayerOps &);
  enum class Direction { Up, Down };
  enum class WasdWith { Left, Middle, None };
  Clip sweep(Direction, Rgb color, WasdWith, Duration step);   // 3 steps; Up = left,middle,right; Down = right,middle,left
  Clip flash(Rgb color, const std::vector<Zone> &zones, int blinks, Duration on, Duration off); // ON (OFF ON)*, blinks >= 1
  Clip lift(double toward_white, Duration);                    // one step: Mix{kWhite, a} on all four zones
  Clip dip(double factor, Duration);                           // one step: Scale{f} on all four zones
  ```
  `WasdWith` decides which sweep step also lights WASD: the Left step, the Middle step, or never.

- [ ] **3.1** Write `tests/test_clip.cpp` (helper `paint_on({zones}, color)` builds a `LayerOps`):
  ```cpp
  TEST(Clip, SweepUpLightsLeftMiddleRightWithWasdOnTheLeftStep) {
      const Clip c = sweep(Direction::Up, kC, WasdWith::Left, 120ms);
      ASSERT_EQ(c.steps.size(), 3u);
      EXPECT_EQ(c.steps[0].ops, paint_on({Zone::Left, Zone::Wasd}, kC));
      EXPECT_EQ(c.steps[1].ops, paint_on({Zone::Middle}, kC));
      EXPECT_EQ(c.steps[2].ops, paint_on({Zone::Right}, kC));
      EXPECT_EQ(c.total(), 360ms);
  }
  TEST(Clip, SweepDownReversesTheOrderKeepingWasdWithLeft) {
      // steps: Right | Middle | Left+Wasd
  }
  TEST(Clip, WasdWithMiddleAndNone) {
      // Middle: the middle step also paints Wasd. None: Wasd is never painted.
  }
  TEST(Clip, FlashWithOneBlinkIsOneStepAndTwoBlinksAreOnOffOn) {
      EXPECT_EQ(flash(kC, all, 1, 150ms, 150ms).steps.size(), 1u);
      const Clip two = flash(kC, all, 2, 150ms, 150ms);
      ASSERT_EQ(two.steps.size(), 3u);
      EXPECT_TRUE(empty(two.steps[1].ops));
      EXPECT_EQ(two.total(), 450ms);
  }
  TEST(Clip, LiftAndDipUseTheSameMathAsTheEffects) {
      for (std::size_t i = 0; i < 4; ++i) {
          EXPECT_EQ(apply(f, lift(0.5, 120ms).steps[0].ops)[i], Rgb::lerp(f[i], kWhite, 0.5));
          EXPECT_EQ(apply(f, dip(0.35, 120ms).steps[0].ops)[i], multiply(f[i], 0.35));
      }
  }
  TEST(Clip, OpsAtWalksTheStepsAndIsEmptyAfterTheEnd) {
      // 0 and 119 ms -> step 0; 120 ms -> step 1; 359 ms -> step 2; 360 ms -> empty
  }
  TEST(Clip, NextBoundaryIsStrictlyAfter) {
      // (0)=120ms, (119)=120ms, (120)=240ms, (240)=360ms, (360)=nullopt
  }
  TEST(Clip, ApplyLeavesZonesWithoutAnOpUntouched) {}
  ```
- [ ] **3.2** Run `Clip.*`. Expected: the build fails (`core/clip.hpp: No such file`).
- [ ] **3.3** Implement. `apply` goes zone by zone. `ops_at` and `next_boundary` walk cumulative holds.
- [ ] **3.4** Run `Clip.*`. Expected: all pass.
- [ ] **3.5** Commit `feat: clips (sweep, flash, lift, dip)`.

### Task 4: ClipPlayer

**Files:** Create `src/core/clip_player.hpp`, `src/core/clip_player.cpp`, `tests/test_clip_player.cpp`.

**Interfaces:**
- Consumes: `Clip`, `LayerOps`, `TimePoint`, `Duration`.
- Produces:
  ```cpp
  enum class Channel { Workspace, Window, Notify, AgentDone };
  std::string_view name(Channel);   // "workspace", "window", "notify", "agent-done"
  enum class Offer { Started, Queued, Dropped };
  struct PlayerLimits { std::uint32_t max_per_minute = 30; Duration window_cooldown = 500ms; Duration gap = 150ms; };
  class ClipPlayer {
    explicit ClipPlayer(PlayerLimits = {});
    void set_limits(PlayerLimits);
    Offer offer(Channel, Clip, TimePoint now);
    void advance(TimePoint now);                       // retire a finished clip; start the queued one
    LayerOps layer(TimePoint now) const;               // empty ops unless a clip is mid-step
    std::optional<TimePoint> next_change(TimePoint now) const;  // next step boundary / clip end / queued start; nullopt when idle
    void clear();
    std::optional<Channel> playing() const;
  };
  ```
- Policies:
  - `Workspace`: dropped while a clip plays or waits.
  - `Window`: dropped while busy, and dropped until `window_cooldown` after the last window clip *started*.
  - `Notify` and `AgentDone`: start if idle; otherwise queue one; a second while one is queued is dropped.
  - A queued clip starts `gap` after the playing one ends (R2), or at `now` if that time has passed (time jump).
  - Rate: at most `max_per_minute` starts in any rolling minute. An offer that would start now is dropped past that; a queued clip that cannot start is discarded.

- [ ] **4.1** Write `tests/test_clip_player.cpp` (`t0 = Clock::now()`; a 150 ms `flash`, a 360 ms `sweep`):
  ```cpp
  TEST(ClipPlayer, StartsAnIdleOfferAndFollowsTheSteps) {
      EXPECT_EQ(p.offer(Channel::Workspace, sweep_clip(), t0), Offer::Started);
      EXPECT_EQ(p.layer(t0 + 130ms), sweep_clip().steps[1].ops);
      EXPECT_EQ(p.playing(), Channel::Workspace);
      EXPECT_EQ(p.next_change(t0), t0 + 120ms);
      p.advance(t0 + 360ms);
      EXPECT_FALSE(p.playing().has_value());
      EXPECT_TRUE(empty(p.layer(t0 + 360ms)));
  }
  TEST(ClipPlayer, WorkspaceOfferWhileBusyIsDropped) {}
  TEST(ClipPlayer, WindowClipsHonourTheCooldownFromTheLastWindowStart) {
      // offer at t0 Started; at t0+300ms Dropped; at t0+500ms Started
  }
  TEST(ClipPlayer, NotifyQueuesOneAndDropsTheRest) {
      // t0 Started, t0+20ms Queued, t0+40ms Dropped
  }
  TEST(ClipPlayer, QueuedClipStartsAfterTheGapSoTwoFlashesAreTwoFlashes) {
      // flash 150 ms, gap 150 ms: advance(t0+150ms) -> idle, layer(t0+200ms) empty,
      // next_change(t0+150ms) == t0+300ms; advance(t0+300ms) -> Notify playing, step 0
  }
  TEST(ClipPlayer, RateLimitCapsStartsPerRollingMinute) {
      // limits {30, 0ms, 0ms}; 100 window offers 600 ms apart: exactly 30 Started;
      // advance + offer at t0+61s -> Started again
  }
  TEST(ClipPlayer, ATimeJumpRetiresFinishedClipsWithoutReplayingThem) {
      // offer A, queue B; advance(t0 + 10min): B plays from the jump time (step 0),
      // next_change == late + 150ms; A is gone
  }
  TEST(ClipPlayer, ClearEmptiesEverything) {}
  ```
- [ ] **4.2** Run `ClipPlayer.*`. Expected: the build fails (no header).
- [ ] **4.3** Implement. State: playing `{channel, clip, start}`, queued `{channel, clip}`, ring of start times, last window start.
- [ ] **4.4** Run `ClipPlayer.*`. Expected: all pass.
- [ ] **4.5** Commit `feat: ClipPlayer with channel policies and rate limit`.

### Task 5: LampSet

**Files:** Create `src/core/lamps.hpp`, `src/core/lamps.cpp`, `tests/test_lamps.cpp`.

**Interfaces:**
- Consumes: `Clip`, `flash`, `LayerOps`, `Paint`, `Zone`, `Rgb`, `TimePoint`, `Duration`.
- Produces:
  ```cpp
  enum class LampKind { Agent, Urgent };            // Agent outranks Urgent where zones overlap
  std::string_view name(LampKind);                  // "agent", "urgent"
  struct LampStyle { std::vector<Zone> zones; Rgb color{}; int blinks = 2; Duration blink = 150ms; std::optional<Duration> ttl; };
  class LampSet {
    void set_style(LampKind, LampStyle);            // keys and intro progress are kept
    void raise(LampKind, std::string key, std::optional<std::string> window, TimePoint now, bool play_intro);
    void clear(LampKind, std::string_view key);
    void clear_window(std::string_view window);     // every key linked to it, both kinds
    void clear_kind(LampKind);
    void expire(TimePoint now);
    LayerOps layer(TimePoint now) const;
    std::optional<TimePoint> next_change(TimePoint now) const;  // intro boundary or earliest TTL expiry
    std::vector<LampKind> active() const;           // Agent first
    bool empty() const;
  };
  ```
- Rules:
  - The intro is `flash(color, zones, max(1, blinks), blink, blink)` starting when a kind goes from no keys to some, and only if `play_intro`. After it the layer is steady `Paint`.
  - Re-raising an existing key refreshes its TTL and replays nothing.
  - An `Urgent` raise is absorbed (ignored) if any `Agent` key carries that same `window`.
  - Empty `zones` means raises are ignored.

- [ ] **5.1** Write `tests/test_lamps.cpp` (style: zones `{Wasd}`, blinks 2, blink 150 ms, TTL 1 h on Agent):
  ```cpp
  TEST(LampSet, IntroIsOnOffOnThenSteady) {
      lamps.raise(LampKind::Agent, "s1", std::nullopt, t0, true);
      EXPECT_EQ(lamps.layer(t0), on);                 // Paint on Wasd
      EXPECT_TRUE(empty(lamps.layer(t0 + 150ms)));
      EXPECT_EQ(lamps.layer(t0 + 300ms), on);
      EXPECT_EQ(lamps.layer(t0 + 10s), on);
      EXPECT_EQ(lamps.next_change(t0), t0 + 150ms);
      EXPECT_EQ(lamps.next_change(t0 + 300ms), t0 + 1h);   // only the TTL is left
  }
  TEST(LampSet, ALateSampleAfterTheIntroShowsSteady) {
      // first sample at t0 + 10min is steady, never a replayed blink
  }
  TEST(LampSet, OneBlinkOrZeroLightsSteadilyAtOnce) {}
  TEST(LampSet, NoIntroWhenNotVisibleOrAlreadyLit) {}
  TEST(LampSet, LampStaysLitUntilTheLastKeyIsCleared) {}
  TEST(LampSet, RaisingAnExistingKeyRefreshesTtlWithoutReplayingTheIntro) {}
  TEST(LampSet, AgentKeysExpireAfterTtlUrgentKeysDoNot) {}
  TEST(LampSet, ClearWindowClearsEveryLinkedKeyOfBothKinds) {}
  TEST(LampSet, UrgentRaiseForAWindowThatHasAnAgentKeyIsAbsorbed) {}
  TEST(LampSet, AgentWinsASharedZone) {}
  TEST(LampSet, EmptyZonesDisableTheKind) {}
  TEST(LampSet, RestyleKeepsKeys) {}
  ```
- [ ] **5.2** Run `LampSet.*`. Expected: the build fails (no header).
- [ ] **5.3** Implement. State per kind: `{style, keys: map<key, {window, raised_at}>, intro_start}`. `layer` paints Urgent first, then Agent.
- [ ] **5.4** Run `LampSet.*`. Expected: all pass.
- [ ] **5.5** Commit `feat: LampSet (blink, hold, TTL, window links)`.

### Task 6: `[reactions]` config

**Files:** Create `src/core/reactions_cfg.hpp`, `src/reactions_config.hpp`, `src/reactions_config.cpp`, `tests/test_reactions_config.cpp`. Modify `src/config.hpp`, `src/config.cpp`, `packaging/config.example.toml`, `tests/test_config.cpp`.

**Interfaces:**
- Consumes: `tomlr::check_keys`, `opt`, `opt_int_in` (`util/toml_read.hpp`), `ColorRef::parse`, `zone_from_name`, `Urgency`, `WasdWith`.
- Produces:
  ```cpp
  struct ReactionsCfg {
      bool enabled = false;                               // true when the table is present
      std::uint32_t step_ms = 120, blink_ms = 150, max_per_minute = 30;
      struct Workspace { ColorRef color; WasdWith wasd_with = WasdWith::Left; } workspace;
      struct Window { double open = 0.5, close = 0.35; std::uint32_t cooldown_ms = 500; } window;
      struct Notify { ColorRef color, critical; Urgency min_urgency = Urgency::Normal;
                      bool skip_if_focused = true; std::map<std::string, ColorRef, std::less<>> apps; } notify;
      struct Urgent { std::vector<Zone> zones{Zone::Right}; ColorRef color; int blinks = 2; } urgent;
      struct Agent { std::vector<Zone> zones{Zone::Wasd}; ColorRef waiting, done; int blinks = 2;
                     std::uint32_t ttl_s = 3600; } agent;
  };
  ReactionsCfg default_reactions();                       // spec §5.11 defaults, enabled = false
  std::expected<ReactionsCfg, std::string> parse_reactions(const toml::table &t);   // enabled defaults to true
  Config::reactions                                       // ReactionsCfg; default_reactions() when the table is absent
  ```
- Defaults and ranges are the §5.11 block, summarized below.

  | Key | Default | Range |
  |---|---|---|
  | `step_ms` | 120 | 60–1000 |
  | `blink_ms` | 150 | 60–1000 |
  | `max_per_minute` | 30 | 1–600 |
  | `workspace.color` | `theme:lavender` | |
  | `workspace.wasd_with` | `left` | `left`, `middle`, `none` |
  | `window.open` | 0.5 | 0–1 |
  | `window.close` | 0.35 | 0–1 |
  | `window.cooldown_ms` | 500 | 0–10000 |
  | `notify.color` | `theme:blue` | |
  | `notify.critical` | `theme:red` | |
  | `notify.min_urgency` | `normal` | |
  | `notify.skip_if_focused` | true | |
  | `notify.apps` | `zapzap = "theme:green"`, `discord = "#5865f2"` | |
  | `urgent.zones` | `["right"]` | |
  | `urgent.color` | `theme:yellow` | |
  | `urgent.blinks` | 2 | 0–5 |
  | `agent.zones` | `["wasd"]` | |
  | `agent.waiting` | `theme:peach` | |
  | `agent.done` | `theme:green` | |
  | `agent.blinks` | 2 | 0–5 |
  | `agent.ttl_s` | 3600 | 60–86400 |

- [ ] **6.1** Write `tests/test_reactions_config.cpp`:
  ```cpp
  TEST(ReactionsConfig, MissingTableMeansOff) { /* parse_config(minimal).reactions.enabled == false */ }
  TEST(ReactionsConfig, EmptyTableTurnsItOnWithTheSpecDefaults) { /* every default in the table above */ }
  TEST(ReactionsConfig, UnknownKeysAreErrorsNamingTheField) {
      // "[reactions]\nbogus = 1"  -> error contains "reactions: unknown field `bogus`"
      // "[reactions.window]\nx = 1" -> "reactions.window: unknown field `x`"
  }
  TEST(ReactionsConfig, EveryBoundIsEnforced) {
      // 59/1001 step_ms, 59/1001 blink_ms, 0/601 max_per_minute, window.open -0.1/1.5,
      // window.close 1.5, cooldown_ms 10001, blinks 6, ttl_s 59/86401: each error names the field
  }
  TEST(ReactionsConfig, ZonesColorsAndUrgencyAreValidated) {
      // zones=["top"], color="nope", min_urgency="loud" are errors; zones=[] is allowed (disables the lamp)
  }
  TEST(ReactionsConfig, NotifyAppKeysAreLowercased) { /* apps = { ZapZap = "theme:green" } -> key "zapzap" */ }
  ```
- [ ] **6.2** Run `ReactionsConfig.*`. Expected: the build fails (headers missing).
- [ ] **6.3** Implement the struct, `default_reactions()` and `parse_reactions` (strict, errors prefixed `reactions` / `reactions.<table>`). In `config.cpp`, add `"reactions"` to the allowed root keys and parse the table when present.
- [ ] **6.4** Add the whole block to `packaging/config.example.toml` (it is the embedded default, so `embedded_default()` must still parse). Extend `tests/test_config.cpp`: `EmbeddedDefaultEnablesReactions` asserts `embedded_default().reactions.enabled`.
- [ ] **6.5** Run `ReactionsConfig.*:Config*`, then the full suite. Expected: all pass.
- [ ] **6.6** Commit `feat: [reactions] config`.

### Task 7: LookBook

**Files:** Create `src/core/lookbook.hpp`, `src/core/lookbook.cpp`, `tests/test_lookbook.cpp`.

**Interfaces:**
- Consumes: `ReactionsCfg`, `Clip` builders, `LampStyle`, `PlayerLimits`, `signal::Notified`, `ColorRef`.
- Produces:
  ```cpp
  using Resolver = std::function<Rgb(const ColorRef &)>;
  class LookBook {
    LookBook(const ReactionsCfg &, const Resolver &);   // resolves every color now
    std::optional<Clip> workspace(int from, int to) const;   // nullopt if equal; Up when to > from
    std::optional<Clip> window_opened() const;               // nullopt when open <= 0
    std::optional<Clip> window_closed() const;               // nullopt when close >= 1
    std::optional<Clip> notified(const signal::Notified &, std::string_view focused_class) const;
    std::optional<Clip> agent_done() const;                  // nullopt when agent zones are empty
    LampStyle urgent_style() const;                          // no TTL
    LampStyle agent_style() const;                           // TTL = ttl_s
    PlayerLimits limits() const;                             // max_per_minute, cooldown_ms, gap = blink_ms
  };
  ```
- Notification policy:
  - Identities, lowercased: the app name, the desktop entry, and the entry's last dot-segment.
  - Skip when `urgency < min_urgency`.
  - Skip when `skip_if_focused` and the focused class equals any identity.
  - Color: critical → `critical`; else the first identity found in `apps`; else `color`.
  - The clip is `flash(color, all four zones, 1, blink_ms, blink_ms)`.

- [ ] **7.1** Write `tests/test_lookbook.cpp`:
  ```cpp
  TEST(LookBook, WorkspaceSweepDirectionAndStep) {
      EXPECT_EQ(look.workspace(1, 2)->steps.size(), 3u);   // Up: Left+Wasd, Middle, Right
      /* (3, 1) is Down; (2, 2) is nullopt; each step holds step_ms */
  }
  TEST(LookBook, WindowPulsesUseTheConfiguredAmountsAndCanBeSwitchedOff) {
      /* open = 0 -> nullopt; close = 1 -> nullopt; defaults give lift(0.5) and dip(0.35), step_ms each */
  }
  TEST(LookBook, NotificationColorPrecedence) {
      // critical beats the app table; the table beats the default; keys match the app name,
      // the desktop entry or its last segment, case-insensitively ("ZapZap" / "com.rtosta.zapzap")
  }
  TEST(LookBook, LowUrgencyIsSkippedByDefault) {}
  TEST(LookBook, SkipsTheFocusedAppUnlessTurnedOff) {
      // focused "com.rtosta.zapzap" vs app "ZapZap": nullopt; skip_if_focused = false: a clip
  }
  TEST(LookBook, NotifyFlashIsOneBlinkOnAllZones) {}
  TEST(LookBook, AgentDoneUsesAgentZonesAndTheDoneColor) {}
  TEST(LookBook, StylesCarryZonesColorBlinksAndTtl) {}
  TEST(LookBook, EveryColorIsResolvedOnceAtConstruction) {}   // counting resolver
  ```
- [ ] **7.2** Run `LookBook.*`. Expected: the build fails (no header).
- [ ] **7.3** Implement. `limits().window_cooldown` comes from `window.cooldown_ms`, and `gap` from `blink_ms`.
- [ ] **7.4** Run `LookBook.*`. Expected: all pass.
- [ ] **7.5** Commit `feat: LookBook (signal looks, notification policy)`.

### Task 8: Compositor layers

**Files:** Modify `src/core/compositor.hpp`, `src/core/compositor.cpp`, `tests/test_compositor.cpp`.

**Interfaces:**
- Consumes: `LayerOps`, `apply`.
- Produces: `Inputs` gains `LayerOps lamps{}; LayerOps transient{};` as the last members, so existing designated initializers still compile. `compose()` order:
  1. effect or base
  2. overlay or tint
  3. `apply(lamps)`
  4. hint fill
  5. `apply(transient)`
  6. brightness scale

  `Output.animated` is unchanged.

- [ ] **8.1** Add tests:
  ```cpp
  TEST(Compositor, LampsDrawAboveTheProfileAndBelowTheHint) {
      /* lamps Paint on Wasd shows over kBase; with a hint set, the hint fills every zone */
  }
  TEST(Compositor, TransientDrawsAboveTheHint) {}
  TEST(Compositor, BrightnessScalesLampsAndTransient) { /* brightness 50: Paint white -> {128,128,128} */ }
  TEST(Compositor, ReactionsNeverMakeTheFrameAnimated) { /* static base + lamps + transient -> animated == false */ }
  TEST(Compositor, EmptyLayersChangeNothing) {}
  ```
- [ ] **8.2** Run `Compositor.*`. Expected: the new tests fail, the old ones pass.
- [ ] **8.3** Implement in `compose()`. Do not touch the `animated` expression.
- [ ] **8.4** Run `Compositor.*`. Expected: all pass.
- [ ] **8.5** Commit `feat: compositor draws lamp and transient layers`.

### Task 9: Fullscreen fact and rule condition

**Files:** Modify `src/core/rules.hpp`, `src/core/rules.cpp`, `src/config.hpp`, `src/config.cpp`, `src/daemon/model.hpp`, `src/daemon/model.cpp`, `src/ipc/protocol.cpp`, `packaging/config.example.toml`, `tests/test_rules.cpp`, `tests/test_config.cpp`, `tests/test_model.cpp`, `tests/test_ipc.cpp`.

**Interfaces:**
- Produces:
  - `Facts::fullscreen_app` (`std::optional<std::string>`, the lowercased class while the focused window is in true fullscreen).
  - `Condition::fullscreen_app` (`std::optional<std::vector<std::string>>`, lowercased, `"*"` matches any fullscreen app). It holds only when the fact is set and listed.
  - `namespace fact { struct FullscreenApp { std::optional<std::string> v; }; }`, added to `FactUpdate`.
  - `bool Config::needs_fullscreen() const`.
  - The status facts JSON gains `"fullscreen_app"` (null when absent; decoding tolerates a missing key).
  - The example config gets the rule `fullscreen video dims` (`fullscreen_app = ["mpv", "vlc", "firefox", "chromium"]`, `brightness_cap = 15`) **between `gamemode` and `power saver dims`**, because rules are first-match per category.

- [ ] **9.1** Add tests:
  ```cpp
  TEST(Rules, FullscreenAppConditionMatchesCaseInsensitivelyAndStarMatchesAny) {}
  TEST(Rules, FullscreenAppConditionDoesNotHoldWithoutAFullscreenWindow) {}
  TEST(Rules, FullscreenAppMustBeANonEmptyArrayOfStrings) { /* parse errors name the field */ }
  TEST(Config, NeedsFullscreenOnlyWhenARuleUsesIt) {}
  TEST(Config, EmbeddedDefaultDimsFullscreenVideoBeforePowerSaver) {
      // rule "fullscreen video dims": cap 15, apps mpv, vlc, firefox, chromium; index before "power saver dims"
  }
  TEST(Model, FullscreenFactSelectsTheCap) {
      // apply_fact(FullscreenApp{"mpv"}) -> inputs().brightness == 15; nullopt -> back to 100;
      // status().facts.fullscreen_app == "mpv"
  }
  TEST(Ipc, StatusFactsRoundTripFullscreenApp) {}
  ```
- [ ] **9.2** Run `Rules.*:Config.*:Model.*:Ipc.*`. Expected: the new tests fail or the build fails.
- [ ] **9.3** Implement. `Model::apply_fact` gets a branch for the new fact. Lowercase class names once, in the parser and at the fact's source.
- [ ] **9.4** Run the same filter, then the full suite. Expected: all pass.
- [ ] **9.5** Commit `feat: fullscreen_app fact and rule condition`.

### Task 10: Reactions façade

**Files:** Create `src/core/reactions_status.hpp`, `src/daemon/reactions.hpp`, `src/daemon/reactions.cpp`, `tests/test_reactions.cpp`.

**Interfaces:**
- Consumes: `LookBook`, `ClipPlayer`, `LampSet`, `Signal`, `ReactionsCfg`, `Resolver`.
- Produces:
  ```cpp
  struct ReactionsStatus { bool enabled = false; std::vector<std::string> lamps; std::optional<std::string> playing; };  // ==
  struct Visibility { bool keyboard_visible; bool user_present; };
  struct Layers { LayerOps lamps; LayerOps transient; };
  class Reactions {
    void reconfigure(const ReactionsCfg &, const Resolver &);   // lamps keep keys; a playing clip finishes with its old look; enabled=false clears everything
    void restyle(const Resolver &);                             // palette changed: re-resolve, keep state
    void on_signal(const Signal &, TimePoint now, Visibility);
    void update(TimePoint now, Visibility);                     // advance time + apply visibility; once per frame before layers()
    void suppress();                                            // drop clips (suspend); keep lamps
    Layers layers(TimePoint now) const;
    std::optional<TimePoint> next_change() const;               // cached by on_signal/update (R7)
    ReactionsStatus status() const;
  };
  ```
- `short_ok = keyboard_visible && user_present`. It gates every clip and every intro (`play_intro = short_ok`).
- `layers().lamps` is **empty unless the last visibility had `keyboard_visible`**. The composed frame still says "backlight on" after the Fn key, so a lamp that changed while the keyboard is dark would be written to zones nobody sees. When the keyboard becomes visible again, the lamps show steady, at a cost of one write.
- It tracks `focused_window_` and `focused_class_` from `FocusChanged`.
- Signal handling:

  | Signal | Action |
  |---|---|
  | `WorkspaceSwitched` | offer `workspace(from, to)` on `Workspace` |
  | `WindowOpened` / `WindowClosed` | offer on `Window` |
  | `Notified` | offer `notified(n, focused_class_)` on `Notify` |
  | `UrgentRaised{w}` | ignored if `w == focused_window_`; else `raise(Urgent, w, w, …)` |
  | `UrgentCleared{w}` | `clear(Urgent, w)` |
  | `UrgentReset` | `clear_kind(Urgent)` |
  | `FocusChanged{w, c}` | record focus, `clear_window(w)` |
  | `AgentWaiting{s, w}` | ignored if `w == focused_window_`; else `raise(Agent, s, w, …)` |
  | `AgentWorking{s}` | `clear(Agent, s)` |
  | `AgentDone{s, w}` | `clear(Agent, s)`, then offer the done flash on `AgentDone` unless `w` is focused |
  | `AgentEnded{s}` | `clear(Agent, s)` |

  When disabled, every signal is ignored.
- `update` expires lamps, advances the player, and clears the player when `!short_ok`.

- [ ] **10.1** Write `tests/test_reactions.cpp` (a resolver mapping each `ColorRef` to a fixed color):
  ```cpp
  TEST(Reactions, DisabledDoesNothing) {}
  TEST(Reactions, WorkspaceSwitchPlaysASweepThenEmptiesTheLayer) {
      // layers(t0).transient has Paint on Left+Wasd; +120 ms Middle; +240 ms Right; +360 ms empty;
      // next_change() walks 120, 240, 360 ms
  }
  TEST(Reactions, ShortReactionsNeedAVisiblePresentKeyboard) {
      // {true,false} and {false,true}: no clip. A lamp raised then is steady at once (no blink).
  }
  TEST(Reactions, ShortClipsAreDroppedWhenTheUserLeavesMidClip) {}
  TEST(Reactions, WaitingAgentLightsTheLampAndFocusingItsWindowClearsIt) {}
  TEST(Reactions, WaitingAgentWhoseWindowIsFocusedDoesNotLight) {}
  TEST(Reactions, AgentDoneClearsTheLampAndFlashesUnlessItsWindowIsFocused) {}
  TEST(Reactions, UrgentOnTheFocusedWindowIsIgnoredAndFocusClearsIt) {}
  TEST(Reactions, UrgentFromAnAgentsWindowIsAbsorbed) {}
  TEST(Reactions, UrgentResetClearsTheAttentionLight) {}
  TEST(Reactions, ReconfigureKeepsLampKeysAndLetsAPlayingClipFinish) {}
  TEST(Reactions, ReconfigureToDisabledClearsLampsAndClips) {}
  TEST(Reactions, RestyleChangesTheLampColorWithoutLosingKeys) {}
  TEST(Reactions, LampsAreHiddenWhileTheKeyboardIsNotVisible) {
      // raise with {false,true}: layers().lamps empty; update with {true,true}: Paint, steady (no blink)
  }
  TEST(Reactions, SuppressDropsClipsButKeepsLamps) {}
  TEST(Reactions, NextChangeCoversIntroClipAndTtl) {}
  TEST(Reactions, StatusListsLampsAndThePlayingChannel) {}
  ```
- [ ] **10.2** Run `Reactions.*`. Expected: the build fails (no header).
- [ ] **10.3** Implement. Call `lamps_.expire(now)` and `player_.advance(now)` at the top of `on_signal`. Keep the stored `ReactionsCfg` so `restyle` can re-resolve. Refresh the cached boundary as the minimum of `lamps_.next_change(now)` and `player_.next_change(now)`.
- [ ] **10.4** Run `Reactions.*`. Expected: all pass.
- [ ] **10.5** Commit `feat: Reactions facade`.

### Task 11: Model integration and the write-count contract (G1)

**Files:** Create `tests/reaction_rig.hpp`, `tests/test_reaction_costs.cpp`. Modify `src/daemon/model.hpp`, `src/daemon/model.cpp`, `src/ipc/protocol.hpp`, `src/ipc/protocol.cpp`, `src/daemon/daemon.cpp` (two call-site updates only), `tests/test_model.cpp`, `tests/test_ipc.cpp`.

**Interfaces:**
- Consumes: `Reactions`, `Visibility`, `ReactionsStatus`, `Signal`.
- Produces:
  ```cpp
  // Model
  void apply_signal(const Signal &, TimePoint now);                 // a no-op while reactions are off
  std::optional<TimePoint> reaction_deadline() const;               // nullopt while suspended
  void set_hw_backlight(bool on, TimePoint now);                    // signature gains `now`
  bool backlight_reading_fresh(TimePoint now, Duration max_age) const;
  // forget_hw_backlight() also forgets the time
  // inputs(now): calls reactions_.update(now, visibility()) and fills Inputs.lamps / Inputs.transient
  // Visibility: keyboard_visible = !suspended_ && backlight_wanted() && effective_brightness(rules()) > 0
  //             && hw_backlight_ != false && !budget_exhausted_;  user_present = !locked && !idle
  // set_suspended(true) calls reactions_.suppress(); replace_config -> reactions_.reconfigure;
  // set_palette -> reactions_.restyle  (resolver = a lambda over Model::resolve, passed per call, never stored)
  // Status gains `ReactionsStatus reactions`; status JSON gains "reactions": {enabled, lamps, playing}
  ```
- Test rig (`tests/reaction_rig.hpp`):
  ```cpp
  class ReactionRig {
    explicit ReactionRig(std::string_view config_toml = kRigConfig);
    Model &model();
    void signal(const Signal &);                     // model.apply_signal(now), then sample()
    void fact(const FactUpdate &);                   // model.apply_fact, then sample()
    void request(const Request &);                   // model.handle(r, now), then sample()
    void run_for(Duration);
    std::size_t writes() const;                      // distinct (backlight, hw zones) outputs after the first
    const std::vector<Frame> &frames() const;
  };
  ```
  - `sample()`: `inputs(now_)` → `compose(in, seconds since start)`, and count a change.
  - `run_for`:
    ```
    end = now_ + d
    while now_ < end:
        next = end
        if last output animated: next = min(next, now_ + 1000/fps ms)           // ticks sample the reactions
        else if model.reaction_deadline(): next = min(next, *deadline)          // mirrors Daemon::next_wake
        now_ = next; sample()
    ```
  - `kRigConfig`: a static profile `plain` (`#101010 #202020 #303030 #404040`), a `cycle` profile `wavy` (effects enabled, 10 fps), `[reactions]` with hex colors, and the rule `fullscreen_app = ["mpv"]` with `brightness_cap = 15`.

- [ ] **11.1** Write `tests/test_reaction_costs.cpp`. These numbers are the contract. Each test builds a fresh rig, applies the stimulus, runs for 2 s unless stated, and asserts `writes()`:
  ```cpp
  TEST(ReactionCosts, WorkspaceSweepCostsFour)            // WorkspaceSwitched{1,2}: 4; frames = [L+W lit, M lit, R lit, base]
  TEST(ReactionCosts, FiveFastSwitchesCostFour)           // switches at 0, 20, 40, 60, 80 ms: still 4
  TEST(ReactionCosts, WindowOpenOrCloseCostsTwo)          // WindowOpened: 2; WindowClosed: 2
  TEST(ReactionCosts, WindowStormCostsAtMostFour)         // 20 opens in 1 s (every 50 ms): exactly 4
  TEST(ReactionCosts, NotifyFlashCostsTwo)                // Notified{ZapZap...}: 2
  TEST(ReactionCosts, NotifyBurstCostsFour)               // 10 in 200 ms: 4 (on, off, on, off)
  TEST(ReactionCosts, AttentionLightCostsFourPerEpisode)  // UrgentRaised{a} -> 3 (on, off, on); FocusChanged{a} -> +1; total 4
  TEST(ReactionCosts, MoreUrgentWindowsAddNothingWhileLit)  // raise a (3), raise b +0, focus a +0 (b still lit), focus b +1
  TEST(ReactionCosts, LampHeldForSixtySecondsAddsNothing) // after the intro, run_for(60s): +0
  TEST(ReactionCosts, ClaudeWaitCostsFourAndFinishCostsTwo) // AgentWaiting 3, AgentWorking +1; AgentDone alone 2
  TEST(ReactionCosts, ClaudeLampWindowRules)
      // focus 0xw, then AgentWaiting{s1, 0xw}: 0 writes.
      // AgentWaiting{s2, 0xo} lights (3); FocusChanged{0xo} clears it (+1).
      // With an agent key on 0xo, UrgentRaised{0xo} adds 0. AgentDone{s, 0xw} while 0xw is focused adds 0.
  TEST(ReactionCosts, FullscreenInAndOutCostTwo)          // FullscreenApp{"mpv"} 1; repeated 0; nullopt +1
  TEST(ReactionCosts, ReactionsAreSilentWhileLockedIdleOrOff)
      // on_lock/on_idle = keep: short reactions 0, a lamp raised then is steady (+1, no blink);
      // on_lock = off, idle off, user off, brightness 0: 0 writes after the transition
  TEST(ReactionCosts, ReactionsOnAnAnimatedBaseAddNoWrites)  // profile wavy: writes in 1 s with a sweep == without
  TEST(ReactionCosts, RateLimitCapsAStormOverAMinute)     // window events every 600 ms for 60 s: clips <= 30, writes <= 60
  TEST(ReactionCosts, ADarkKeyboardCostsNothing)
      // set_hw_backlight(false, now): WindowOpened, UrgentRaised and AgentWaiting each add 0;
      // set_hw_backlight(true, now) then adds +1 and shows the lamps steady, with no blink
  ```
  Also in `tests/test_model.cpp`: `SuspendClearsClipsButKeepsLamps`, `PaletteChangeRestylesLamps`, `HwBacklightReadingIsFreshFor5s`, `StatusReportsReactions`. In `tests/test_ipc.cpp`: `StatusReactionsRoundTrip` and `StatusWithoutReactionsStillDecodes` (an older daemon's JSON leaves the default).
- [ ] **11.2** Run `ReactionCosts.*:Model.*:Ipc.*`. Expected: the build fails (`reaction_rig.hpp` and the Model API are missing).
- [ ] **11.3** Implement the Model changes and update the two `set_hw_backlight` call sites in `daemon.cpp` (`Clock::now()` as `now`), plus every test call. Then write the rig.
- [ ] **11.4** Run the same filter, then the full suite. Expected: all pass, and each cost test asserts the exact number above.
- [ ] **11.5** Commit `feat: Model reactions, status and the write-count contract tests`.

### Task 12: IPC and CLI `notify` / `agent`

**Files:** Modify `src/ipc/protocol.hpp`, `src/ipc/protocol.cpp`, `src/cli/cli.cpp`, `src/cli/format.cpp`, `tests/test_ipc.cpp`, `tests/test_cli.cpp`.

**Interfaces:**
- Consumes: `Signal`, `urgency_from_name`.
- Produces:
  ```cpp
  namespace req {
  struct Notify { std::string app, desktop_entry, urgency; };                       // defaulted ==
  struct Agent  { std::string state, session; std::optional<std::string> window; std::string source; };
  }
  // Request += req::Notify, req::Agent
  std::optional<std::expected<Signal, std::string>> to_signal(const Request &);     // nullopt for other requests
  ```
  - Wire (kebab-case, like the rest): `{"cmd":"notify","app":…,"desktop-entry":…,"urgency":…}` and `{"cmd":"agent","state":…,"session":…,"window":null|"…","source":…}`.
  - `to_signal` rules:
    - urgency is case-insensitive;
    - state is one of `waiting`, `working`, `done`, `end`;
    - the window is normalized (R8: lowercase, no `0x`);
    - an unknown urgency or state is an error string.
  - CLI:
    - `omen-kbd notify [--app TEXT] [--desktop-entry TEXT] [--urgency low|normal|critical]`: defaults `""`, `""`, `normal`; the urgency is validated case-insensitively and a bad value exits 64.
    - `omen-kbd agent <waiting|working|done|end> --session ID [--window ADDR] [--source NAME]`: `--session` is required, `--source` defaults to `claude`.
  - Status text: a `reactions` line after `rules`: `off`, or `on` plus `  lamps agent,urgent` and `  playing workspace` when present.

- [ ] **12.1** Write tests:
  ```cpp
  TEST(Ipc, NotifyRoundTripsWithKebabCaseKeys) {
      EXPECT_EQ(encode(Request{req::Notify{"ZapZap", "com.rtosta.zapzap", "critical"}}),
                R"({"cmd":"notify","app":"ZapZap","desktop-entry":"com.rtosta.zapzap","urgency":"critical"})");
  }
  TEST(Ipc, AgentRoundTripsWithAndWithoutWindow) {}
  TEST(Ipc, ToSignalMapsEveryStateAndNormalizesTheWindow) {
      // "0xABC" -> window "abc"; "Critical" -> Urgency::Critical; bad state / urgency -> error;
      // req::Status -> nullopt
  }
  TEST(Cli, NotifyParsesOptionsAndDefaults) {}
  TEST(Cli, NotifyRejectsAnUnknownUrgencyWithUsage64) {}
  TEST(Cli, AgentRequiresSessionAndAValidState) {}
  TEST(Cli, AgentParsesWindowAndSource) {}
  TEST(Format, StatusShowsReactionsOffWhenDisabled) { /* contains "reactions   off\n" */ }
  TEST(Format, StatusShowsLampsAndTheClipPlaying) {
      /* contains "reactions   on  lamps agent,urgent  playing workspace\n" */
  }
  ```
- [ ] **12.2** Run `Ipc.*:Cli.*:Format.*`. Expected: the new tests fail or the build fails.
- [ ] **12.3** Implement. The `encode`/`request_from` visitors gain the two types, and `to_signal` goes in `protocol.cpp`. Until Task 13 the daemon answers both requests with the plain status (inert but green).
- [ ] **12.4** Run the same filter, then the full suite. Expected: all pass.
- [ ] **12.5** Commit `feat: notify and agent requests, commands and status line`.

### Task 13: Daemon wiring (signals, deadlines, read-before-start)

**Files:** Modify `src/daemon/events.hpp`, `src/daemon/daemon.hpp`, `src/daemon/daemon.cpp`, `src/backend/mock.hpp`, `src/backend/mock.cpp`, `tests/test_daemon.cpp`.

**Interfaces:**
- Consumes: `Model::apply_signal`, `reaction_deadline`, `backlight_reading_fresh`, `to_signal`, `Signal`, `may_start_reaction`.
- Produces:
  - `Event` gains `Signal` as an alternative.
  - `MockHandle::backlight_reads()`, a thread-safe count of `read_backlight` calls.
  - In `Daemon`:
    ```cpp
    struct PendingSignal { Signal signal; TimePoint deadline; };
    std::vector<PendingSignal> pending_signals_;
    void deliver_signal(const Signal &);       // R3, R4
    void flush_pending_signals();
    ```
    with `kBacklightFresh = 5s` and `kMaxPendingSignals = 1024`.
  - `deliver_signal`:
    1. Return at once when `!model_.config().reactions.enabled` (R4: no probe).
    2. If `pending_signals_` is empty and not (`may_start_reaction(s)` and a stale reading): apply, `recompute()`, return.
    3. Otherwise queue it with `deadline = now + kReplyTimeout` (order is kept, so a clearing signal never overtakes a waiting one) and call `writer_->probe()`.
    4. At `kMaxPendingSignals`, flush at once as if the read had timed out.
  - `flush_pending_signals` applies every pending signal in order, then `recompute()` once.
  - `on_request`: `to_signal(r)` has a value → on error respond with a failure; else `deliver_signal` and respond with the plain status. Neither waits for the write nor goes through `needs_backlight_read`.
  - `handle_event`: a `Signal` goes to `deliver_signal`.
  - `on_writer` (probe branch): call `flush_pending_signals()` after `release_probe_waiters()`, and make the stale-read retry condition `!awaiting_probe_.empty() || !pending_signals_.empty()`.
  - `next_wake()` considers `model_.reaction_deadline()` **only when the last output was not animated**, and the earliest pending-signal deadline.
  - `on_wake()` recomputes when `due(model_.reaction_deadline())`, and flushes when a pending deadline is due.

- [ ] **13.1** Write tests (static profile config with `[reactions]` and hex colors; `daemon_->post(Event{signal::…})` for Hyprland-style signals):
  ```cpp
  TEST_F(DaemonTest, WindowOpenedSignalPlaysAPulseAndRestores)       // +2 zone writes, last frame == base
  TEST_F(DaemonTest, NotifyRequestIsAnsweredAtOnceAndFlashes)        // reply well under 500 ms, +2 writes
  TEST_F(DaemonTest, AgentWaitingHoldsTheLampWithoutFurtherWrites)   // +3, still +3 after 1 s, working -> +4
  TEST_F(DaemonTest, StaleBacklightReadingCausesOneReadBeforeAReaction)  // backlight_reads +1
  TEST_F(DaemonTest, TwoReactionsWithinFiveSecondsCauseOneRead)
  TEST_F(DaemonTest, ReactionOnADarkKeyboardWritesNothing)           // set_hw_backlight(false), signal, +0 zone writes
  TEST_F(DaemonTest, DisabledReactionsIgnoreSignalsWithoutReading)   // config without [reactions]: reads == 0
  TEST_F(DaemonTest, OrderIsKeptWhileAReadIsPending)                 // waiting then working: the lamp ends dark
  TEST_F(DaemonTest, FloodWhileAReadIsPendingStaysBounded)           // 5000 WindowOpened posts: daemon answers status
                                                                     // within 2 s and the writes stay <= 8
  ```
- [ ] **13.2** Run `DaemonTest.*`. Expected: the build fails (`backlight_reads`, `Event` alternative).
- [ ] **13.3** Implement the mock counter and the daemon changes.
- [ ] **13.4** Run `DaemonTest.*`, then the full suite. Expected: all pass, with no new flakes over three runs of `DaemonTest.*`.
- [ ] **13.5** Commit `feat: daemon delivers signals with read-before-start`.

### Task 14: Hyprland events and the bridge

**Files:** Create `src/sources/hyprland_tracker.hpp`, `src/sources/hyprland_tracker.cpp`, `src/sources/hyprland_bridge.hpp`, `src/sources/hyprland_bridge.cpp`, `tests/test_hyprland_tracker.cpp`. Modify `src/sources/hyprland.hpp`, `src/sources/hyprland.cpp`, `src/daemon/daemon.cpp` (`run_daemon` wiring), `tests/test_sources.cpp`.

**Interfaces:**
- Consumes: `Signal`, `FactUpdate`, `event::Submap`, `Event`.
- Produces:
  ```cpp
  namespace hypr {
  struct Submap { std::optional<std::string> name; };  struct WorkspaceV2 { int id; };
  struct OpenWindow { std::string address; };          struct CloseWindow { std::string address; };
  struct Urgent { std::string address; };              struct ActiveWindow { std::string app_class; };   // title dropped here
  struct ActiveWindowV2 { std::string address; };      // empty = nothing focused
  struct Fullscreen { bool on; };                      struct Connected {};  struct Disconnected {};
  }
  using HyprEvent = std::variant<hypr::Submap, hypr::WorkspaceV2, hypr::OpenWindow, hypr::CloseWindow,
                                 hypr::Urgent, hypr::ActiveWindow, hypr::ActiveWindowV2, hypr::Fullscreen,
                                 hypr::Connected, hypr::Disconnected>;      // each with defaulted ==
  std::optional<HyprEvent> parse_socket2_line(std::string_view);            // pure; nullopt for anything else
  // HyprlandSource::Emit becomes std::function<void(const HyprEvent &)>; it emits Connected on a
  // successful connect and Disconnected on EOF/error; connected_path() returns the socket2 path in use.
  struct CheckFullscreen {};
  using TrackerOut = std::variant<Signal, FactUpdate, event::Submap, CheckFullscreen>;
  class HyprlandTracker { std::vector<TrackerOut> on(const HyprEvent &); };
  class HyprlandBridge {
    using Emit = std::function<void(const Event &)>;
    HyprlandBridge(EventLoop &, HyprlandSource::PathFn, Emit);
    void configure(bool reactions_on);          // Task 15 extends it to configure(bool, bool)
  };
  ```
- Parser rules:
  - Fields are split only as far as needed, and the title is never kept.
  - `workspacev2>>ID,NAME`: the id is parsed from the text before the first comma; a bad number gives nullopt.
  - `activewindow>>CLASS,TITLE`: the class is the text before the first comma.
  - `fullscreen>>0|1`: anything else is nullopt.
  - Addresses are normalized (R8: lowercase, no `0x`).
  - `submap>>` with an empty name is the default submap.
- Tracker rules (state: current workspace id, focused address and class, the class seen by the last `ActiveWindow`, the urgent set):

  | Event | Output |
  |---|---|
  | `Connected` | reset state; `CheckFullscreen` |
  | `Disconnected` | `UrgentReset`, `FactUpdate{FullscreenApp{nullopt}}`, `event::Submap{nullopt}`; reset state |
  | `Submap` | `event::Submap` |
  | `WorkspaceV2{id}` | `id <= 0` ignored. First id after a (re)connect only recorded. A different id: `WorkspaceSwitched{prev, id}`. Always `CheckFullscreen` for ids > 0. |
  | `OpenWindow` | `WindowOpened` |
  | `CloseWindow{a}` | `WindowClosed`; if `a` was urgent, erase it and emit `UrgentCleared{a}`; `CheckFullscreen` |
  | `Urgent{a}` | ignored if `a` is the focused window; else add it and emit `UrgentRaised{a}` |
  | `ActiveWindow{c}` | remember `c` (no output) |
  | `ActiveWindowV2{a}` | set focus (class from the remembered one), remove `a` from the urgent set, emit `FocusChanged{a, class}`, `CheckFullscreen` |
  | `Fullscreen` | `CheckFullscreen` |

- Bridge: `HyprlandSource` → `HyprlandTracker` → `Emit`. `Signal` outputs are forwarded only while `reactions_on`. `FactUpdate` and `Submap` always pass. `CheckFullscreen` is ignored until Task 15. `run_daemon` constructs `std::optional<HyprlandBridge>` where it built `HyprlandSource`, and calls `configure(config.reactions.enabled)` at start and from `sync_sources`.

- [ ] **14.1** Write the tests (`HyprlandParse.*`, `HyprlandTracker.*`, plus the updated `HyprlandSource` and bridge cases in `test_sources.cpp`, reusing the fake socket2 listener):
  ```cpp
  TEST(HyprlandParse, ParsesEveryEventWeUse) {}
  TEST(HyprlandParse, TitlesWithCommasAndDoubleArrowsNeverShiftFields) {
      // "openwindow>>abc,1,Alacritty,a, b >> c,d" -> OpenWindow{"abc"}
      // "activewindow>>firefox,Title, with >> and ,commas" -> ActiveWindow{"firefox"}
  }
  TEST(HyprlandParse, MalformedLinesAreIgnoredNotThrown) {
      // "workspacev2>>abc,x", "workspacev2>>", "fullscreen>>2", "fullscreen>>", "activewindowv2",
      // ">>", "", an unknown event, a 100 KB line: all nullopt
  }
  TEST(HyprlandParse, AddressesAreNormalized) { /* "activewindowv2>>0xABCdef" -> "abcdef" */ }
  TEST(HyprlandParse, SubmapStillParses) { /* resize, and empty = default submap */ }
  TEST(HyprlandTracker, FirstWorkspaceEventOnlyRecordsTheId) {}
  TEST(HyprlandTracker, WorkspaceSwitchEmitsFromAndToAndSpecialIdsAreIgnored) {}
  TEST(HyprlandTracker, OpenAndCloseWindowEmitPulses) {}
  TEST(HyprlandTracker, UrgentIsIgnoredForTheFocusedWindow) {}
  TEST(HyprlandTracker, UrgentRaisesUntilFocusedOrClosed) {}
  TEST(HyprlandTracker, FocusChangedCarriesTheClassFromThePrecedingActiveWindowLine) {}
  TEST(HyprlandTracker, FocusWorkspaceFullscreenAndCloseRequestAFullscreenCheck) {}
  TEST(HyprlandTracker, DisconnectClearsUrgentFullscreenAndSubmap) {}
  TEST(HyprlandTracker, ConnectedResetsStateAndChecksFullscreen) {}
  TEST(HyprlandBridge, ForwardsSignalsOnlyWhileReactionsAreOn) {}
  TEST(HyprlandBridge, SubmapAndDisconnectBehaveAsBefore) {}   // the old HyprlandSource behavior, now via the bridge
  ```
- [ ] **14.2** Run `HyprlandParse.*:HyprlandTracker.*:HyprlandBridge.*`. Expected: the build fails (types missing).
- [ ] **14.3** Implement, update `HyprlandSource` and the existing source tests, and wire the bridge in `run_daemon`. Run `clang-format -i` on `hyprland.cpp` so no whitespace noise creeps in.
- [ ] **14.4** Run `HyprlandParse.*:HyprlandTracker.*:HyprlandBridge.*:Sources*` and the full suite. Expected: all pass.
- [ ] **14.5** Commit `feat: Hyprland events become signals through a tracker and bridge`. Check `git diff --stat` shows no whole-file reformat.

### Task 15: Fullscreen through hyprctl

**Files:** Create `src/sources/hyprctl_query.hpp`, `src/sources/hyprctl_query.cpp`, `tests/test_hyprctl_query.cpp`. Modify `src/ipc/socket.hpp`, `src/ipc/socket.cpp`, `src/sources/hyprland.cpp`, `src/sources/hyprland_bridge.hpp`, `src/sources/hyprland_bridge.cpp`, `src/daemon/daemon.cpp`, `tests/test_sources.cpp`.

**Interfaces:**
- Consumes: `EventLoop` timers and watches, `UniqueFd`, `fact::FullscreenApp`, `Config::needs_fullscreen`.
- Produces:
  ```cpp
  // ipc/socket.hpp (moved from hyprland.cpp's anonymous namespace)
  UniqueFd connect_nonblocking(const std::filesystem::path &);       // non-blocking, close-on-exec, or empty
  // sources/hyprctl_query.hpp
  struct ActiveWindowInfo { std::string address, app_class; int fullscreen = 0; };
  std::optional<ActiveWindowInfo> parse_active_window(std::string_view json);   // pure; nullopt for {} or garbage; address normalized
  class HyprctlQuery {
    using Result = std::function<void(std::optional<ActiveWindowInfo>)>;        // nullopt on error or timeout
    HyprctlQuery(EventLoop &, Result);
    void request(const std::filesystem::path &request_socket);
  };
  constexpr bool is_true_fullscreen(int mode) { return (mode & 2) != 0; }       // R9, Task 1
  // HyprlandBridge::configure(bool reactions_on, bool fullscreen_wanted)
  ```
- `HyprctlQuery` behavior:
  - Writes `j/activewindow` and reads to EOF.
  - Debounced 100 ms after the last `request`, one request in flight (the latest wins), 300 ms timeout, 64 KiB cap.
  - Never blocks the loop.
- Bridge behavior:
  - On `CheckFullscreen`, if `fullscreen_wanted`, query `<dir of the connected socket2>/.socket.sock`.
  - A result with `is_true_fullscreen(fullscreen)` becomes `FullscreenApp{lowercase(class)}`; otherwise `FullscreenApp{nullopt}`. It is emitted only when it differs from the last value.
  - A timeout or error keeps the last value.
  - Turning `fullscreen_wanted` off emits `FullscreenApp{nullopt}` once.
  - `run_daemon`'s `sync_sources` passes `c.reactions.enabled` and `c.needs_fullscreen()`.

- [ ] **15.1** Write tests with a fake request server (a listener on `<tmp>/hypr/sig/.socket.sock` that replies and closes):
  ```cpp
  TEST(HyprctlQuery, ParsesActiveWindowJson) {
      // {"address":"0xABC","class":"mpv","fullscreen":2} -> {"abc","mpv",2}; "{}" and garbage -> nullopt
  }
  TEST(HyprctlQuery, AnswersFromAFakeRequestSocket) {}
  TEST(HyprctlQuery, TimesOutAndReportsNullopt) {}               // the server never replies: nullopt within ~400 ms
  TEST(HyprctlQuery, DebouncesBurstsIntoOneRequest) {}           // 5 request() calls in 50 ms: one connection
  TEST(HyprctlQuery, CapsTheReplySize) {}                        // a 1 MiB reply: nullopt
  TEST(HyprlandBridge, FullscreenFactFollowsTheFocusedWindow) {
      // fake socket2: activewindowv2 + fullscreen>>1 -> query says mpv, fullscreen 2 -> FullscreenApp{"mpv"};
      // then fullscreen 0 -> nullopt; identical replies emit once
  }
  TEST(HyprlandBridge, KeepsTheLastFullscreenValueWhenTheQueryFails) {}
  TEST(HyprlandBridge, NoQueriesWhenFullscreenIsNotWanted) {}    // no connection to the request socket
  TEST(HyprlandBridge, ConnectSeedsTheFullscreenState) {}        // Connected -> one query
  ```
- [ ] **15.2** Run `HyprctlQuery.*:HyprlandBridge.*`. Expected: the build fails (no header).
- [ ] **15.3** Implement. Move `connect_to` into `ipc/socket` as `connect_nonblocking` and use it from both sources.
- [ ] **15.4** Run `HyprctlQuery.*:HyprlandBridge.*` and the full suite. Expected: all pass.
- [ ] **15.5** Commit `feat: fullscreen state through an async hyprctl query`.

### Task 16: Hook script, snippets, docs

**Files:** Create `packaging/agent-hook/omen-kbd-agent-hook` (mode 755), `packaging/snippets/claude-code-hooks.json`, `packaging/snippets/reactions-config.toml`, `packaging/snippets/notify-reaction.sh`, `tests/test_agent_hook.cpp`. Modify `CMakeLists.txt` (`install(PROGRAMS … DESTINATION bin)`), `README.md`.

**Interfaces:**
- Consumes: the CLI from Task 12, the sample JSON confirmed in Task 1.3.
- Produces: the hook, with these environment overrides (for tests): `OMEN_KBD_BIN` (default `$HOME/.local/bin/omen-kbd`), `OMEN_KBD_PROC` (default `/proc`), `OMEN_KBD_START_PID` (default `$PPID`), `OMEN_KBD_HYPRCTL` (default `hyprctl`), `OMEN_KBD_AGENT_COMM` (default `claude`), `XDG_RUNTIME_DIR` for markers under `omen-kbd/agent/`.
- Hook behavior (it never prints and always exits 0):
  - `jq` or the input missing, or bad JSON or no `session_id`: do nothing.
  - Every `omen-kbd` call is wrapped in `timeout 1`.
  - Event mapping:
    - `Notification` with `notification_type` in `permission_prompt|idle_prompt|elicitation_dialog`: write the session marker, `agent waiting --session ID [--window ADDR] --source claude`.
    - `UserPromptSubmit`, `PostToolUse`, `PostToolUseFailure`: only when the marker exists, remove it, `agent working --session ID`.
    - `Stop`: remove the marker, `agent done --session ID [--window ADDR]`.
    - `SessionEnd`: remove the marker and the window cache, `agent end --session ID`.
    - Any other `notification_type`: ignore.
  - Window discovery, cached per session in `<session>.window`:
    1. Find the session's own `claude` (the nearest ancestor whose `comm` equals `OMEN_KBD_AGENT_COMM`). The hook may sit under a `sh` between it and `claude`; the walk skips it.
    2. If that process's parent is also `claude`, the session is nested (R12: a background session under its `bg-pty-host`): **no** `--window`.
    3. Otherwise keep walking up and take the first ancestor pid that owns a window in `hyprctl clients -j`; its `address` is the value.
    4. No match: no `--window`.

- [ ] **16.1** Write `tests/test_agent_hook.cpp`. It runs the script with `bash`, a fake `omen-kbd` that logs its arguments, a fake `hyprctl`, a fake `/proc` tree under a `TempDir`, and sample JSON per event:
  ```cpp
  TEST(AgentHook, PermissionPromptSendsWaitingWithTheTerminalWindow) {
      // the interactive layout of R12: 100:alacritty <- 101:bash <- 102:claude <- 110:sh <- hook
      // (start pid 110); hyprctl lists pid 100 as 0xabc
      // -> "agent waiting --session S1 --window 0xabc --source claude"
  }
  TEST(AgentHook, BackgroundSessionGetsNoWindow) {
      // the background layout of R12: 100:alacritty <- 101:bash <- 102:claude <- 103:claude (daemon)
      // <- 104:claude (bg-pty-host) <- 105:claude (session) <- hook (start pid 105) -> no --window
  }
  TEST(AgentHook, NoWindowOwnerMeansNoWindowArgument) {}
  TEST(AgentHook, PostToolUseOnlyClearsAWaitingSession) {
      // no marker: no call; after a Notification: one "agent working"; a second PostToolUse: none
  }
  TEST(AgentHook, StopSendsDoneAndClearsTheMarker) {}
  TEST(AgentHook, SessionEndSendsEndAndRemovesCaches) {}
  TEST(AgentHook, OtherNotificationTypesAreIgnored) { /* auth_success -> no call */ }
  TEST(AgentHook, IsSilentAndExitsZeroOnBadInput) { /* invalid JSON, empty stdin, no session_id */ }
  TEST(AgentHook, SurvivesAMissingOrHangingBinary) {
      // OMEN_KBD_BIN missing -> rc 0; a fake that sleeps 5 s -> returns within ~1.5 s
  }
  TEST(AgentHook, ExitsZeroWithoutJq) { /* env PATH=/nonexistent /bin/bash script */ }
  TEST(AgentHook, PassesShellcheck) { /* skip if shellcheck is absent */ }
  ```
- [ ] **16.2** Run `AgentHook.*`. Expected: fails (the script does not exist).
- [ ] **16.3** Write the script (bash + jq, `set -u`, no `set -e`, a `trap 'exit 0' EXIT`). Write the three snippets; each starts with where it goes:
  - `reactions-config.toml`: the `[reactions]` block plus the fullscreen rule, to paste before "power saver dims".
  - `claude-code-hooks.json`: a valid `hooks` block with `$HOME/.local/bin/omen-kbd-agent-hook` for the six events, and a `Notification` matcher `permission_prompt|idle_prompt|elicitation_dialog`.
  - `notify-reaction.sh`: the replacement subshell for `rice-notify-sound` that runs `omen-kbd notify --app "$SWAYNC_APP_NAME" --desktop-entry "$SWAYNC_DESKTOP_ENTRY" --urgency "$SWAYNC_URGENCY"` inside the not-in-DND branch, output to `/dev/null`.

  Add `install(PROGRAMS …)` to `CMakeLists.txt`.
- [ ] **16.4** README:
  - A "Reactions" section: what each reaction does, the config, the write costs, and what is never read.
  - The new commands in the table.
  - Three desktop-integration items: config snippet, notify line, Claude hooks.
  - A Status-table row.
- [ ] **16.5** Run `AgentHook.*`, `shellcheck` on the script and snippets, and the full suite. Expected: all pass. Also time the idle path (`PostToolUse` with no marker) and note the median in the commit message; it should stay under 30 ms.
- [ ] **16.6** Commit `feat: Claude Code hook, snippets and README for reactions`.

### Task 17: Verify, live checks, finish

**Files:** Modify `docs/hardware-notes.md` (a "Reactions live checks" section), `README.md` (Status row).

- [ ] **17.1** Whole-branch gates:
  - the full suite under ASan + UBSan;
  - `clang-format --dry-run --Werror` on every touched file;
  - `run-clang-tidy -p build src`;
  - `shellcheck` on the hook and snippets.

  Expected: all clean.
- [ ] **17.2** Install check without touching the user's real install: `cmake --install build --prefix $CLAUDE_JOB_DIR/tmp/prefix`. Expected: `bin/omen-kbd` and `bin/omen-kbd-agent-hook` are present.
- [ ] **17.3** Mock demo (no hardware): run `omen-kbd daemon --backend mock --mock-render --no-sources` with `XDG_STATE_HOME` pointing at a scratch dir. Drive it with `omen-kbd notify --app zapzap`, `omen-kbd agent waiting --session x`, `omen-kbd agent working --session x`. Expected: the printed frames match spec §2.
- [ ] **17.4** Live checks, with the user present, one approved step at a time (spec §8). Provide a confirmation-prompt script under `~/omen-kbd-reactions/` (like `bind-menu.sh`). The user installs the new binary and applies the snippets: config block, one line in `rice-notify-sound`, hooks block.
  - Switch to a static profile first (`omen-kbd profile baseline`); restore the previous profile afterwards.
  - For each reaction, read `omen-kbd status --json` → `lifetime_writes` before and after, and compare with the spec table: sweep +4, window open +2, close +2, notification +2, attention light +3 then +1, fullscreen +1 / +1, Claude wait +3 then +1, finish +2.
  - A ZapZap notification while it is unfocused and while it is focused (class vs app name).
  - A scratch Claude session: amber after a few seconds on a permission prompt, cleared by focusing its terminal, green on finish, no flash when the terminal is focused, and a background job still lights while the terminal is focused.
  - Note whether the prompt makes Alacritty urgent (the absorb rule must keep the right zone dark).
  - Record the results in `docs/hardware-notes.md`.
- [ ] **17.5** One whole-branch review by a fresh reviewer on the most capable model (per the execution skill): the review package, this plan, the spec and the rulings above. Sort the findings, run one fix pass (each fix with a test that failed first), and ledger the deferred minors.
- [ ] **17.6** Finish: push `keyboard-reactions`, open a draft PR, and end with the report (rulings, deferred minors, what the user must apply). The user merges. Never push to main.

---

## Self-review

**Spec coverage.**

| Spec | Task |
|---|---|
| §2 rows 1–8 | 4, 5, 7, 10, 11 (counts), 14 (triggers) |
| Silent cases, effect pass-through | 10, 11, 13 |
| Layer order | 8 |
| §5.2 signals | 2 |
| §5.3 clips | 3 |
| §5.4 ClipPlayer | 4 |
| §5.5 LampSet | 5 |
| §5.6 LookBook / Reactions | 7, 10 |
| §5.7 compositor / daemon | 8, 11, 13 |
| §5.8 fullscreen rule | 9, 15 |
| §5.9 Hyprland side | 14, 15 |
| §5.10 IPC / CLI / status | 11, 12 |
| §5.11 config | 6 |
| §5.12 producers | 16 |
| §6 verification | every task, plus 11 and 13 |
| §8 live checks | 1, 17 |

**Gaps:** none. `doctor` additions are left to the builder by the spec and are out of scope (R11).

**Type consistency, checked across tasks.**
- `Duration` comes from Task 2 and is used in 3–7.
- `PlayerLimits` (Task 4) comes back from `LookBook::limits()` (Task 7).
- `LampStyle` (Task 5) comes back from `LookBook` (Task 7).
- `Resolver` (Task 7) is used by Tasks 10 and 11.
- `Signal` (Task 2) is used by 10–15.
- `FactUpdate::FullscreenApp` (Task 9) is used by 14 and 15.
- `Visibility` and `ReactionsStatus` (Task 10) are used by 11 and 12.
- `Model::apply_signal`, `reaction_deadline` and `backlight_reading_fresh` (Task 11) are used by 13.
- `to_signal` (Task 12) is used by 13.
- `Event ⊃ Signal` (Task 13) comes before the bridge emits it (Tasks 14 and 15).

**Review Focus:** every line maps to a named test in its owning task (the list near the top).

**Proportion:** the plan is about 1.8 times the spec's length (1,100 lines against 620). Code blocks are test names and assertions plus interface signatures; bodies appear only for the rig's `run_for` loop, which the signatures and tests don't determine.

## Execution handoff

When the user approves this plan, copy it to `docs/superpowers/plans/2026-11-08-keyboard-reactions.md` (Task 0.1) and execute it with **superpowers:subagent-driven-development** (the user's choice). The controller runs Tasks 0 and 1, then dispatches a fresh implementer plus a fresh reviewer for each of Tasks 2–16 in order (each depends on the earlier ones' interfaces). It then does the live checks (17.3–17.4) with the user and finishes with the whole-branch review.
