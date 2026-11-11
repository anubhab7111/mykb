# Keyboard reactions: rulings and deferred minors

Decisions the controller took while executing `2026-11-08-keyboard-reactions.md` (subagent-driven),
copied verbatim from the execution ledger, plus the review findings that were deferred. The plan's own
rulings R1 to R13 are in the plan; P1 to P25 below were made during execution. Each ruling reads
`Ruling: what — why — what it costs if wrong`; the spec stays the authority and every ruling is cheap to reverse.

## Rulings made during execution

- Ruling P1: test bodies shown as `{}` or with a `// ...` comment in the plan are specifications, not code. The implementer writes the full assertions that the name and comment state. Reviewers treat an empty or assertion-free test as a defect. — The plan abbreviated them to stay readable. — Cost if wrong: one fix round per abbreviated test.

- Ruling P2: `ColorRef` is not default-constructible, so config structs give color members a default initializer like `OverlayCfg` does (`= ColorRef::hex(kBlack)`), and `default_reactions()` sets the real spec defaults. — The plan's struct sketch omitted initializers. — Cost if wrong: none (a compile error would surface it at once).

- Ruling P3: Task 1's user-run scripts (`~/omen-kbd-reactions/*.sh`) are handed over now; their logs are read before Task 14. If a log is missing, proceed with R9 (fullscreen predicate) and R10 (event order) as written and carry them as unverified in the final report. — The user may not run them mid-session, and plan rulings, not stalls. — Cost if wrong: a one- or two-line change in Task 14 or 15.

- Ruling P4: models. Implementers `sonnet` (the mid-tier floor, since tasks are prose plus signatures); `opus` for Tasks 11 and 13 (judgment, ordering, timing). Task reviewers `sonnet`, except `opus` for Tasks 10, 11, 13 and 15. Final review `opus`. Fix rounds 4 and 5 go one tier up. — Turn count beats token price. — Cost if wrong: extra fix rounds.

- Ruling P5: `apply(const Frame &, const LayerOps &)` keeps the spec's name and signature, and every call site (tests and `compose()`) writes `omenkbd::apply(...)`. Unqualified calls pick `std::apply` through ADL because both arguments are `std::array`s. — Renaming would touch the spec, the plan and every later brief. — Cost if wrong: one rename if a later task trips over it. Carry into the Task 8, 10 and 11 dispatches (any code or test that calls `apply`).

- Ruling P6: leave `ClipPlayer::clear()` as implemented (full reset) and do not open a fix round. — Task 10's callers clear only while no clip can be offered (keyboard dark, user away, suspended, disabled), a flap needs a human, hypridle or a rules fact, a single slot bounds clips per visible span, and the writer's budget guard still backstops the kernel limit. — Cost if wrong: a two-line deletion plus one inverted test. Task 10's dispatch and the whole-branch review reconsider it.

- Ruling P7: adopt the reviewer's Fix A: in `LampSet::raise` for `LampKind::Agent` with a window (new key or re-raise; an ignored raise erases nothing), erase every Urgent key carrying that window, so a window never holds keys of both kinds; the existing urgent-raise-absorbed rule stays. The spec's section 5.5 rule is extended to say so (docs commit 04a4c5f). — The spec is the authority and the brief followed its literal one-directional text; the delayed hook makes urgent-first the normal order, and the write cost is unchanged (the removal rides on the agent lamp's first frame). — Cost if wrong: one function and two tests.

- Ruling P8: in `LookBook::notified`, an empty identity (an `omen-kbd notify` call with no `--app` / `--desktop-entry`, or an entry with no dot) never matches, and an empty `focused_class` never matches an identity (R10: before the first focus event "skip if focused" does not skip). — Without it a bare `omen-kbd notify` would be skipped as "focused" at startup and the app table would match empty keys. — Cost if wrong: one comparison.

- Ruling P9: the user asked (2026-11-09, "spawn multiple agents to get tasks done parallelly") for parallel agents, which overrides the skill's one-implementer-at-a-time default. Tasks 9, 10 and 16 run in parallel with Task 8, each in its own git worktree and branch from 688694c (`kr/task-9`, `kr/task-10`, `kr/task-16`, worktrees under `.claude/worktrees/kr-task-N`), because their file sets are disjoint from each other and from Task 8 and none consumes another's output. Dependencies still bind: Task 11 waits for 8, 9 and 10; Task 12 waits for 11 (shared protocol/test_ipc and `Status.reactions`); Task 13 waits for 11 and 12; Task 14 for 9 and 13; Task 15 for 14. Each parallel task is reviewed on its own branch SHAs, then cherry-picked onto `keyboard-reactions` in task order by the controller after Task 8 has committed (never while an implementer has the main worktree dirty). — Isolation removes the file-conflict risk the default guards against. — Cost if wrong: a cherry-pick conflict in a shared file (CMakeLists.txt, README.md, packaging/config.example.toml), resolved by the controller; parallel runs also drain the usage limit faster (Task 5 already hit it once).

- Ruling P10: the first parallel dispatch (Tasks 9, 10, 16 into manually created worktrees `kr-task-N`) failed: all three agents returned BLOCKED with no code written, because the harness limits a subagent's writes and git operations to the worktree of the session that spawned it (the main task worktree). — The sanctioned route is the Agent tool's `isolation: "worktree"`, which gives each agent a writable worktree of its own. The three tasks are re-dispatched that way (Ruling P9 otherwise stands). The shared instructions live in `parallel-implementer-notes.md`: each agent verifies that `840cb0a` (Task 8's commit) is an ancestor of its HEAD, else resets its fresh worktree to it; reports go to `the job scratch directory, reports/task-N-report.md` (the agent cannot write into the main worktree's workspace); I read the commit SHA and branch from the agent's reply and cherry-pick from the shared object store. The unused manual worktrees and `kr/task-N` branches are removed. — Cost if wrong: the isolated worktree is created from a different base than assumed; the agents reset it to 840cb0a. A lost day of parallelism otherwise: nothing else was lost.

- Ruling P11: Claude Code hooks `async` is a plain non-blocking flag on command hooks (read from the hooks page by the blocked Task 16 agent): `"async": true` goes on `UserPromptSubmit`, `PostToolUse` and `PostToolUseFailure` only; never `asyncRewake`; never on `Notification`, `Stop` or `SessionEnd`. Recorded in `hooks-facts.md`. — The hooks print nothing, and the script bounds every call with `timeout 1`. — Cost if wrong: remove the flag from three entries in the snippet.

- Ruling P14: fold Task 10's minors (1), (2) and (3) into Task 11's dispatch instead of opening a fix round (they are cheap, edge-case, and Task 11's reload path and Model rig are where they are best pinned); they edit `src/daemon/reactions.{hpp,cpp}` and `tests/test_reactions.cpp`, which Task 11's brief does not list, by this ruling. — Cost if wrong: Task 11's reviewer sees a few extra reactions hunks; a separate fix commit otherwise.

- Ruling P15 (R13 in global-constraints.md, and appended to task-14-brief.md): `HyprlandTracker` ignores an `activewindowv2` whose address equals the current focused address (no `FocusChanged`, no `CheckFullscreen`); `Connected` / `Disconnected` reset it; new test `HyprlandTracker.RepeatedActiveWindowForTheSameWindowIsIgnored`. — Without it a focused terminal whose title keeps changing would fire a hyprctl query and wake the daemon about once a second. — Cost if wrong: one comparison in the tracker.

- Ruling P16: while the daemon is ticking (an animated output with a tick armed), a delivered signal is applied WITHOUT an immediate recompute and the next tick samples it; `next_wake` considers `reaction_deadline()` only when the last output was not animated. The test rig's `signal()` mirrors this (no `sample()` while the last output was animated), the animated test asserts equal totals, and a mid-tick burst on `wavy` (20 opens 50 ms apart at 10 fps) and plain `FocusChanged` signals add 0 writes. Task 13's `deliver_signal` implements the same rule (carried in its dispatch; appended to task-13-brief.md). — Spec section 2 and 5.7 promise zero extra writes on a running effect, and at 10 fps the effect alone is 36 000 of the 40 000 writes an hour. — Cost if wrong: a one-line condition in `deliver_signal` and the rig.

- Ruling P17: instead of forgetting the backlight reading in `handle(Power)` / `set_suspended`, `Model::reaction_deadline()` reports a past sentinel (due now) while reactions are enabled, not suspended, and the current `visibility()` differs from the visibility the façade last saw (`Reactions` exposes a small `visibility_differs(Visibility)`); a past boundary already means "update overdue", so the loop wakes, recomputes, and `update` records the new visibility (no spin). This also heals the spent-budget case (`record_write` clears `budget_exhausted_` with no recompute). The rig models the daemon's forget-after-compose for a forced Power write. — General and Model-side (no daemon.cpp change); Task 13's `on_wake` recomputes when the deadline is due. — Cost if wrong: swap to the explicit `forget_hw_backlight()` calls in `handle(Power On/Toggle)` and `set_suspended(false)`.

- Ruling P12: a pid that owns SEVERAL windows in `hyprctl clients -j` gives no `--window` (the hook treats it as no match). — Spec section 5.12 (line 488) lists "a terminal that serves several windows from one process" as a no-match case, and the spec is the authority over the brief's looser "first ancestor pid that owns a window". — Cost if wrong: a one-line jq revert; a daemon-style terminal then never links its sessions to a window.

- Ruling P13: keep the hook clearing the waiting lamp on ANY `PostToolUse` / `PostToolUseFailure` for the session id, including events from subagents (which carry the parent's `session_id` plus `agent_id`); do not filter on `agent_id`. — Filtering would leave the lamp stuck on after a subagent's own permission prompt is approved (the only clearing event would carry `agent_id`), which is worse than a rare early clear while the main session waits on a different prompt. — Cost if wrong: one jq condition; record the limitation in the README.

- Ruling P18: Task 14 is dispatched in parallel with the Task 13 review, with BASE 87f0d9a (the unreviewed Task 13 commit), because the user asked for parallel agents and Task 14 only needs `Event` to hold a `Signal`. — The review of Task 13 may produce fixes in daemon.cpp; Task 14 touches only the `run_daemon` wiring there. — Cost if wrong: a cherry-pick conflict resolved by the controller, or a rebase of Task 14 onto the fixed Task 13.

- Ruling P19: the window-address normalization (lowercase hex, no 0x; R8) lives in ONE shared header-only helper `src/core/window_address.hpp` used by the socket2 parser and by `to_signal` (it replaces the private `normalized_window` in protocol.cpp); no second copy. — Carry-forward from the Task 12 review. — Cost if wrong: a trivial move.

- Ruling P20: `HyprlandTracker` holds the focused address as `std::optional<std::string>` (nullopt after construction, `Connected` and `Disconnected`); an `ActiveWindowV2` equal to the held value, an empty address included, emits nothing; the first event after a reset always emits (an empty address gives `FocusChanged{"", ""}`). — P15/R13 left the initial state ambiguous. — Cost if wrong: one line in the tracker.

- Ruling P21: when `HyprlandBridge::configure` turns `fullscreen_wanted` on while the source is connected, the bridge issues one query itself (the source connects inside the bridge constructor before the first configure, so the Connected-time CheckFullscreen was ignored); turning it off emits FullscreenApp{nullopt} once and drops any late reply. — Without it the fullscreen fact is unknown until the first focus change after the daemon starts. — Cost if wrong: one condition in configure.

- Ruling P22: the bridge lowercases the window class when building FullscreenApp (mixed-case test: Firefox/MPV -> firefox/mpv), and Task 15 fixtures use fullscreen 0, 1, 2 and 3. — The rule condition compares lowercased names (Task 9 carry-forward). — Cost if wrong: one call.

- Ruling P23: an empty or whitespace-only hyprctl reply is a FAILURE (nullopt, keep the last value); `{}` stays an ANSWER that clears the fact. — My dispatch text "`{}` or an empty reply when nothing is focused" was loose wording and the spec (5.9) says a failure keeps the last value; Hyprland answers `{}` for no focus, so empty can only be a hiccup, and un-dimming mid-movie is worse than a short extra dim (the next focus, workspace, close or fullscreen event re-queries). — Cost if wrong: re-adding the empty-text branch.

- Ruling P24: the whole-branch review (17.5) runs BEFORE the live checks (17.4), so the user tests the final code once instead of twice; if the live checks find a defect, a short extra fix round follows. — The plan listed live checks first, but a review fix pass changes the daemon the user would just have tested. — Cost if wrong: one more live-check pass.

- Final: Ruling P25: fix BOTH Important findings in one pass, as two parallel isolated implementers on separate files from BASE 206fb72, each test-first, then ONE scoped re-review of both. Fix A: the tracker gains a pure `replay()` (FocusChanged for the held focus plus UrgentRaised per urgent address) and `HyprlandBridge::configure` emits it only on the off->on edge (no replay when reactions stay on). Fix B: `to_signal` and the CLI reject an empty or over-128-byte `--session` (exit 64; this also closes the Task 12 minor 1) and `LampSet` keeps at most 64 keys per kind, evicting the oldest on a new key. — The focus gap is reachable on the README install path right before the live checks; unbounded keys are a cheap local-socket hardening. — Cost if wrong: a few lines each.

- Final: Ruling (daemon-restart focus seeding): do NOT seed focus from `j/activewindow` at connect; keep R10. After a daemon restart the first event after `Connected` always emits (P20) and terminals churn their titles, which re-emits the focus events within seconds; only the off->on reload path is a persistent gap and Fix A closes it. — Cost if wrong: a short window after a daemon restart where skip-if-focused does not skip.

- Final: Ruling (declined-to-judge lines, all stand): (a) a fact change such as fullscreen on an animated base recomputes off-tick, as other facts did before this branch — one extra off-tick write per fullscreen change, rare; (b) a queued clip starts late from its first step after a long loop stall — played late, never replayed or backfilled; (c) `claude -p` run inside another session Bash tool is linked to the outer terminal — outside the spec process model, cost: a possibly wrong link; (d) `source` and `desktop-entry` are required on the notify and agent wire — only omen-kbd own CLI produces them; (e) a synchronous Stop hook can take about 3 s while Hyprland hangs with no window cached — the desktop is hung anyway; (f) idle_prompt from background sessions may keep the amber lamp up to the 1 h TTL — spec section 7 accepts it, and the user can drop idle_prompt from the matcher; (g) an openwindow storm from XWayland popups is bounded by the 500 ms cooldown and the rate limit as specified; (h) signal-triggered reads repeat on every signal when the probe always fails — reads, not writes, and a failing probe means broken hardware.

## Whole-branch review (opus)

- Final review (opus, 2748e9e..206fb72): no Critical; Important 1 (after `[reactions]` goes off -> on via reload, the tracker still holds the focused window and P15 suppresses repeats, so the facade never learns the focus until the user focuses another window: a linked agent lamp lights while the user looks at that terminal, done flashes, skip-if-focused does nothing, an already-urgent window never lights); Important 2 (agent lamp keys unbounded: `--session` is any string and the daemon keeps every key for 3600 s while LampSet iterates all keys per frame). Both are small, local fixes; verdict "With fixes".

## Deferred minors (reviewer findings left open on purpose)

- Task 2: minor (deferred): signal aggregates have no default member initializers, so later tasks must spell out every field (`signal::AgentWaiting{"s", std::nullopt}`, `signal::FocusChanged{"w", "cls"}`); the plan's one-field shorthand in Task 10 and 16 comments is not compilable. Carry into the Task 10, 12, 14, 16 dispatches.

- Task 2: minor (deferred): the implementer's report elides the GREEN build output; the reviewer's own -Werror and clang-tidy runs were clean, so non-blocking.

- Task 3: minor (deferred, from the report): `ops_at` and `next_boundary` expect `elapsed >= 0`; `flash` with `blinks < 1` gives one ON step; `Clip` and `ClipStep` have no `==`.

- Task 3: minor (deferred, reviewer): the `apply` comment in clip.hpp (~227-230) reads ambiguously about const lvalues (wording only); `elapsed >= 0` is documented but not asserted.

- Task 4: minor (deferred, reviewer): (1) `clear()` resets the rate and window-cooldown history, so the "at most `max_per_minute` starts in any rolling minute" cap holds between `clear()` calls only (reviewer's smallest change: delete `starts_.clear()` and `last_window_start_.reset()` from `clear()` and invert `ClearAlsoForgetsTheWindowCooldownAndTheRateCount`); (2) no test pins that a busy or cooldown drop leaves the rate budget untouched, and no `max_per_minute = 0` test; (3) the `try_start` comment says "the minute before now" while the window is half-open (< 60 s).

- Task 5: minor (deferred, reviewer): (a) an Agent kind whose zones were emptied by `set_style` (keys kept) still absorbs urgent raises for its window (add `!agent.style.zones.empty()` to the check); (b) a re-raise keeps the window of the first raise (ruled acceptable; reviewer suggests last-wins); (c) `layer` and `next_change` ignore the TTL until `expire(now)` runs, so a caller that caches a boundary without expiring first would spin; (d) no test pins the OFF-phase show-through on a zone shared with a lit Urgent kind, the same key in both kinds, an empty key, or backwards time; (e) `intro_at` rebuilds the clip on every `layer` / `next_change` call (fine under Task 6's bounds).

- Task 5: minor (deferred, re-review): (f) no test makes a re-raise erase an Urgent key (the setup needs the unreachable window-mismatch case; moving the erase after the held-key check would still pass); (g) the `raise` header comment says a held key "only refreshes its TTL", then lists the Urgent erase (wording).

- Task 6: minor (deferred, reviewer): (1) `sub_table` in `src/reactions_config.cpp` is a near-copy of the one in `src/config.cpp` (`OMENKBD_TRY` is now a third copy): move a shared `tomlr::sub_table` into `util/toml_read.hpp` in the whole-branch cleanup; (2) the zone error names the table and the value but not the `zones` key; (3) an unquoted dotted app key (`com.example.app = ...`) is read as a nested table and gives a misleading error: add a hint or an example comment; (4) no test for a non-table root value (`reactions = 1`).

- Task 7: minor (deferred, reviewer): (1) `NotificationColorPrecedence` pins app-name-before-entry only, not entry-before-last-segment or app-before-last-segment; (2) `resolve_apps` could lowercase keys itself to enforce the Task 6 invariant (a collision would then keep the first entry silently); (3) the all-zones vector is rebuilt per notification.

- Task 8: minor (deferred, reviewer): no layer test at brightness 0 or with `backlight = false`; `Mix` is never exercised through the compositor, nor a non-Paint op in the lamps layer.

- Task 9: minor (deferred, reviewer): (1) the lowercase invariant of `Facts::fullscreen_app` is documented but not enforced in `Model::apply_fact` / `Condition::holds` (the plan puts the lowercasing in Task 15); (2) `Model.FullscreenFactSelectsTheCap` never clears the fact while no other cap applies (mpv then nullopt then 100 is not the literal sequence); (3) an empty-string class in `fullscreen_app` is accepted (deliberate, not pinned) and the non-empty-array test does not assert the rule name; (4) `tests/test_config.cpp` relies on transitive includes (`<algorithm>`, `<cstddef>`, `<vector>`).

- Task 10: minor (deferred, reviewer): (1) focus goes stale across a disable/enable cycle (the disabled branch does not clear `focused_window_` / `focused_class_`); (2) the cached boundary can be too late after `reconfigure` (a shorter `agent.ttl_s` or `blink_ms`) until the next `update`; (3) an empty-string window is stored as a key linked to "" and `FocusChanged{""}` calls `clear_window("")` (an empty workspace would clear a Claude lamp); (4) the `on_signal` header comment does not mention the `!short_ok` clear; (5) the namespace-scope `const TimePoint t0 = Clock::now();` in test_reactions.cpp, test_clip_player.cpp and test_lamps.cpp (dynamic initialization, trivially safe); (6) no facade-level test for AgentDone queued behind Notify, UrgentRaised{""}, a late update with a queued clip, the same `now` twice or backwards `now`; (7) an intro that started while visible keeps its timing across a short dark span (practically unreachable).

- Task 16: minor (deferred, reviewer): (1) the hook hangs if fd 0 is closed (not empty): `$(jq ...)` reads its own pipe; unrealistic, one-line guard `{ : <&0; } 2>/dev/null || exit 0`; (2) a failed `hyprctl` lookup is not cached, and Stop is synchronous (2010 ms with both `hyprctl` and `omen-kbd` hung): a short negative cache would bound it; (5) the hang test bounds are loose (900 ms to 3000 ms), no test times the SessionEnd path or closed stdin; (7) `bugprone-command-processor` on the test's `std::system` / `popen` is moot (the gate covers `src`; `tests/test_gen_doctor.cpp` uses the same pattern).

- Task 16: minor (deferred, re-review): the removed `_comment` also carried the reason why `Notification`, `Stop` and `SessionEnd` stay synchronous (it now lives only in hooks-facts.md and Ruling P11; a clause in README item 10 would restore it); "its backlight reading" in README.md ~130-132 reads as if it belongs to the Fn press (wording); the JSON snippet is now the only snippet without a leading where-it-goes note (README item 10 carries it); a synchronous `Notification` / `Stop` hook can run `hyprctl` then `omen-kbd`, so one hook's worst case is about 3 s (each call is capped at 1.5 s).

- Task 11: minor (deferred, reviewer): (1) learning that the keyboard is dark while a lamp is lit costs one invisible write (the writer still believes the backlight is on; spec: only a reaction within 5 s of an Fn press can write unseen); (3) the rig's end-of-run sample is a wake the daemon never takes (covered by the frame_times assertions in the fix round); (4) no test exercises `window_or_none` in AgentWaiting / AgentDone on its own (defense in depth); `Status.reactions.playing` turns on and off, so `broadcast()` pushes two extra status lines per short reaction to subscribers and `playing` lags until the next frame (Task 13 judges).

- Task 11: minor (deferred, re-review): removing the end-of-run sample weakens `LampHeldForSixtySecondsAddsNothing` (it now takes no wake at all in `run_for(60s)`; one sampling step at the end, such as a `FocusChanged` to an unrelated window or a check of `model().inputs(now())->lamps`, would restore the model-state check); the sentinel doc in model.hpp names only `record_write` clearing a spent budget (setting it is also a change with no frame, and tested); a comment at test_reaction_costs.cpp ~512 ("so may a probe before `on`") is wrong (periodic probes run only while animating).

- Task 12: minor (deferred, reviewer): (1) an empty session id is accepted by the CLI and `to_signal` (only a hand-typed call can send it: the hook's regex needs 1 to 128 characters; the lamp key "" cannot collide with a real session); smallest fix: a non-empty validator on `--session` (exit 64) and an error from `to_signal` for an empty session on the four agent states; (2) a non-hex `--window` string is harmless (only compared as a string; its lamp is never dismissed by focus and ends by working, done, end or TTL); (3) tests/test_ipc.cpp:87 has a raw-string literal over 100 columns (the brief's own text); (4) the CLI sends the canonical lowercase urgency, `to_signal` accepts any case (fine); (5) the CLI exits 64 on an empty `--urgency` (the brief mandates it): if swaync ever left `SWAYNC_URGENCY` empty the flash would be dropped silently (the live check confirms swaync exports it).

- Task 13: minor (deferred, reviewer): (1) the probe branch of on_writer recomputes BEFORE the pending signals are flushed, so after a stale reading where the Fn key was turned back on, the held lamps and the first frame of a new clip can cost one extra write (lamps, clip+lamps, lamps instead of clip+lamps, lamps); the brief mandates flush after release_probe_waiters(), and applying the batch before that recompute would fix it; (2) the pre-existing probe recompute adds one off-tick frame while an effect ticks (bounded; the implementer disclosed it; recompute there only when the reading changed or no effect ticks); (3) a reload that turns [reactions] off while signals are pending can still re-probe once via the stale-read retry (R4 says not even a read); clear pending_signals_ in the Reload branch when the new config disables reactions; (4) SignalsOnARunningEffectAddNoWrites only bounds the count (a daemon that dropped signals while ticking would also pass); assert that a tick frame shows the lift or that playing was observed.

- Task 14: minor (deferred, reviewer): (1) the tracker never clears app_class_ after a changed focus consumes it, so a lost activewindow line (a title over 64 KiB) makes FocusChanged carry the previous window class; fix with std::exchange(app_class_, {}); (2) pre-existing HyprlandSource overflow reset parses the tail of an oversized line as a fresh line; a discard-until-newline flag would close it; (3) no test has an embedded NUL or a 64 KiB boundary line (safe by construction); (4) after Disconnected the tracker resets focus but the facade keeps focused_window_ and focused_class_ until the next focus event (stale class for notification matching right after a Hyprland restart; a FocusChanged{"",""} on Disconnected would fix it); (5) normalize_window_address has no length or hex check and the urgent set has no cap (hardening only); (6) the WindowAddress helper test lives in test_hyprland_tracker.cpp; (7) cannot verify from the diff whether Hyprland sanitizes newlines inside titles before writing a socket2 line (pre-existing line framing).

- Task 15: minor (deferred, reviewer): (1) a non-UTF-8 title anywhere in the reply fails the whole parse (nlohmann strict UTF-8), so if the title becomes invalid while an app is fullscreen the exit is never seen until a valid reply (entering fullscreen fails safe; worth a comment or a test pinning the behaviour; could not verify whether Hyprland sanitizes titles); (5) every request() kills a query in flight, so a steady stream of CheckFullscreen spaced under 100 ms would starve the query (not realistic with R13 dedupe; the brief says the latest wins).

- Task 15: minor (deferred, re-reviewer): tests/test_hyprctl_query.cpp ~245-251 (EachRequestRestartsTheDebounce) samples `last = Clock::now()` after request() returns while the loop thread arms the 100 ms timer as soon as it runs the posted request; a test-thread preemption over about 10 ms makes the >= 90 ms bound fail on a correct implementation (sample `last` before request() to make it strictly safe); TimesOutAndReportsNullopt `< 1 s` has 600 ms of slack.

- Final: minor (deferred): (1) the reactions snippet is never parsed by the product parser (test_agent_hook.cpp ~571-591 only compares text with the example config and has a stale comment): add a parse_config test on a config shaped like the user with the snippet spliced before "power saver dims", asserting rule order and that the cap 15 wins while power-saver is active; (2) the cost rig does not model the read-before-start path: the probe branch of on_writer recomputes before the pending batch is flushed (same as Task 13 minor 1), one extra write when a lamp is held and the reading flips off -> on; (3) one failed fullscreen query after `fullscreen>>0` leaves the keyboard dimmed until the next focus, workspace, close or fullscreen event (a single bounded retry about 500 ms later would heal it; P23 upheld; related to Task 15 minor 1, a non-UTF-8 title fails the whole reply); (4) socket2 overflow tail parsed as a fresh line can raise an urgent key that never clears (urgent has no TTL; a discard-until-newline flag in HyprlandSource closes it; unlikely given Wayland message limits; same as Task 14 minor 2); (5) on an animated base reactions are sampled only at ticks: below about 8 fps a 120 ms pulse can fall between ticks and never show (0 writes; the user runs 10 fps; document in the README or a config comment); (6) README "Turning things off" says enabled = false turns everything off, but the fullscreen dim is a rule and keeps working (R4), and the README waiting row drops "0 if you are looking at a linked session"; (7) spec drift: header still says draft, section 4 still mentions the uncommitted reformat, section 5.5 `raise` argument order differs from the code, section 5.7 still says to reuse awaiting_probe_ (R3 superseded it), and section 2 row 7 absorb bullet promises no second light while P7 makes the normal order (urgent about 6 s before the hook) show yellow on the right zone then amber WASD, 3+3+1 = 7 writes per episode instead of 4: update section 2 AFTER live check 4 confirms the order; (8) an empty SWAYNC_URGENCY would make the CLI exit 64 and drop the flash silently (the live check confirms swaync exports it); (9) re-grades upheld: P6 (tick clears the player when not short_ok) and Task 14 minor 4 (stale facade focus after Disconnected) stay Minor.

- Final fix B: minor (deferred, re-reviewer): tests/test_cli.cpp ~420-423 the exit_code_of calls are not guarded by the preceding parse assertion, so if the validator regressed the test would reach the real daemon socket (same pattern as the existing notify --urgency test; make the parse checks ASSERT or skip exit_code_of when parse succeeded); no test raises an Urgent key for the window of an evicted Agent key to show it is no longer absorbed (structurally guaranteed).


## Closed after the live checks (2026-11-11)

- The empty `SWAYNC_URGENCY` case from the Task 12 and Final deferred minors is fixed in commit
  `ff2ecbc`: `notify-reaction.sh` now passes `"${SWAYNC_URGENCY:-normal}"`, with
  `AgentHook.NotifySnippetFallsBackToNormalWhenSwayncLeavesTheUrgencyEmpty` (RED, then GREEN).
- Spec §5.5's claim (and the matching §2 absorb bullet, reworded in the clean-up below) that the terminal's urgent signal "usually arrives first" on a Claude permission
  prompt is corrected: the live run cost 3 writes, so no urgent signal came (Ruling P7 still covers a
  terminal that does ring its bell). The README "enabled = false" wording and the other minors above
  stay open.

## Deferred-minors clean-up (2026-11-11, branch `deferred-minors`)

Five parallel groups worked the list above, then one Opus review of the whole diff (verdict: Ready,
no Critical or Important findings). Every behavior fix has a test that failed first.

**Fixed (behavior):**
- Task 5 a: an agent lamp whose zones were emptied by a reload no longer absorbs the attention light.
- Task 7 2: `LookBook` lowercases the notification app keys itself.
- Task 6 2 and 3: the zone error names the `zones` key; an unquoted dotted app key is told to be quoted.
- Task 9 1: the fullscreen fact is lowercased where it enters the model.
- Task 13 1 and Final 2: a backlight read and the signals that waited for it show in one frame (3 writes
  became 2); Task 13 3: a reload that turns reactions off drops the signals waiting for a read.
- Task 14 1: a focus change never carries the previous window's class; Task 14 4: a Hyprland disconnect
  clears the facade's focus (chain test in `test_reactions.cpp`); Task 14 2 and Final 4: the tail of an
  oversized socket2 line is discarded, not parsed as a line.
- Task 14 5 and Task 12 2: a window address is 1 to 64 hex digits, anything else is invalid at every caller
  (`waiting` and `done` refuse a bad `--window`, `working` and `end` ignore it).
- Task 15 1: a non-UTF-8 title no longer fails the fullscreen query (`src/util/utf8.hpp`); Final 3: one retry
  about 600 ms after a failed query while the fact names a fullscreen app.
- Task 16 1: the hook exits at once when stdin is closed (`{ : 3<&0; }`; the brief's `<&0` form does not
  work in bash); the hook also omits an address of more than 64 digits.
- Review Minor 1: `working` and `end` ignore a bad `--window` in both the CLI and `to_signal`.

**Fixed (tests, refactors, docs):** Task 3, 4, 5, 7, 8, 9, 10, 11, 13 and 14 test gaps and comments; one shared
`tomlr::sub_table` and `OMENKBD_TRY`; the reactions snippet parsed in a user-shaped config (Final 1); the
`exit_code_of` guard (Final fix B); README (slow-effect fps rule, `enabled = false`, waiting cost, hook timing,
`--window` rule); the spec's header, API lists, pending-signal and absorb text, and live-check 4 result.

**Closed without work:** Task 12 1 (Fix B), Task 12 5 and Final 8 (`ff2ecbc`), Task 10 3 (guards already
existed; pin tests added), Task 10 1 (already fixed on main by `cb925c1`).

**Declined, with the reason:**
- Task 2 (both) and the Task 3 report items: plan-text and process notes; documented, deliberate.
- Task 4 1: contradicts Ruling P6 (`clear()` stays a full reset). Task 5 b: ruled acceptable.
- Task 5 c, e, f and Task 10 2, 5, 7: callers expire first, trivial cost or unreachable.
- Task 11 1 and 3: spec-documented or moot; Task 11 4 `playing` broadcast churn: benign.
- Task 13 2: no deterministic failing test exists (the extra frame mostly moves the tick phase).
- Task 14 7 and urgent-set cap, Task 15 5: unverifiable here, bounded, unreachable with the R13 dedupe.
- Task 16 2 (negative cache would invert `AFailedWindowLookupIsNotCached`), 5, 7, and a leading comment for the
  JSON snippet (JSON cannot carry one).

**Rulings made during the clean-up:**
- Ruling: after a successful read the waiting signals are applied before `release_probe_waiters()`. — a
  waiting `status` or `toggle` then sees them, and the read plus its signals cost one frame — cost if wrong:
  swap two lines. (G3; the reviewer agreed.)
- Ruling: Task 13 2 is declined (see above). (G3; the reviewer agreed.)
- Ruling: a bare `0x` is invalid everywhere, not "no window". (G4; the reviewer agreed.)
- Ruling: an invalid `--window` is refused only for `waiting` and `done`. — G4 had refused it for every
  state; the review showed a wrapper that always adds `--window` could then not clear a lamp for an hour —
  cost if wrong: re-widen two checks.
- Ruling: the `--window` validator is a check in the `agent` callback (still exit 64), not an option
  validator, so it can see the state. (controller)
- Ruling: T6 3's hint appears only for a dotted key written on one line; a dotted table header keeps the
  plain message. (G2)
- Ruling: T9 3 keeps an empty class in `fullscreen_app` accepted (it matches only a window whose class is
  empty), pinned by a test. (G2)
- Ruling: T10 1 gets no new test: the fix was already on main. (G1)
- Ruling: the hook guard is `{ : 3<&0; }`, because bash accepts `<&0` on a closed fd 0. (G5)
- Ruling: the reviewer's "declined to judge" items stand as assumptions: Hyprland sends `activewindow` before
  `activewindowv2` (seen in the Task 1 socket2 spike log) and an empty `activewindowv2>>` means nothing
  focused (assumed: the spike never recorded an empty one); the
  dotted-key detection relies on the vendored toml++ source positions; the `3<&0` guard is plain redirection
  and not bash-version specific; Hyprland's newline handling in titles (Task 14 7) is unverified.

**Still open (new from the review, all Minor):**
- A malformed `activewindowv2` (invalid address, dropped by the parser) leaves the class from the preceding
  `activewindow` line pending, so the next valid focus could carry it. Hyprland never sends one.
- `Hyprland.TheTailOfAnOversizedLineIsNotALine` passes on the old code too if the reader has not drained the
  65,537 bytes within 200 ms (no flake, but no teeth under load).
- `HyprlandBridge.ADisconnectCancelsAPendingRetry` would still pass without the cancel (the source reconnects
  later than the retry); the other two cancel paths are pinned.
- The `window_or_none` guard in `Reactions` stays defense in depth: no test can fail without it.
