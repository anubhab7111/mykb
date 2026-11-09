# omen-kbd

Keyboard color manager for the **HP OMEN 16-k0xxx (board 8A4E)** and its 4-zone RGB backlight
(left, middle, right, WASD), built for a Hyprland desktop. One binary holds the user daemon, the
CLI, a Waybar module, a health check (`doctor`) and the Hyprland hook generator.

- **Profiles:** static colors or software effects, plus brightness and on/off. Your last colors come
  back after reboot, suspend, the Fn key and daemon restarts.
- **Mode colors:** hold a configured modifier set (Super, Super+Shift, Ctrl, …) or enter a Hyprland
  submap, and the whole keyboard shows that mode's color until you release it.
- **Rules:** react to the power profile, battery, CPU temperature, game mode, lock and idle.
- **Theme sync:** `theme:<name>` colors come from the rice's current theme and follow theme switches.

## Status

| Part | State |
|---|---|
| Daemon, CLI, Waybar stream, Hyprland hook, `doctor` | Done. Tested against a mock backend and fake sysfs trees (GoogleTest, ASan + UBSan). |
| Kernel module `hp_omen_kbd` | Tested in a VM against a firmware emulator, and validated on this laptop's real firmware (P4, 2026-11-06). The stock kernel has no keyboard-RGB interface, so until the module is loaded, `omen-kbd daemon` exits with "no keyboard backend" and `doctor` reports a FAIL. Use `--backend mock` to try everything. |
| Hardware validation | Done (P4): `docs/hardware-validation.md`, results in `docs/hardware-notes.md`. |
| Install and soak | Done (P5, 2026-11-07): installed, autostarted, all desktop hooks working; results in `docs/hardware-notes.md`. |
| Reactions | Built and unit-tested against the mock backend and fake Hyprland, swaync and Claude Code inputs ([Reactions](#reactions)). The three producer snippets are not applied yet, and the live checks on the real keyboard are pending. |

## Safety

- The kernel's own `hp-wmi` is never replaced or patched. The daemon runs as your user, not root.
- No keylogging: the Hyprland hook looks only at the 8 modifier keys and logs nothing.
- Colors are written only through the small `hp_omen_kbd` module. It is allowlisted to board 8A4E
  and was validated on that board's real firmware (P4, `docs/hardware-notes.md`); `readonly=1` still
  makes it refuse every write. It enforces ≥ 50 ms between writes and an hourly write budget. Each change is one atomic write of all four zones.
- A static profile costs zero writes. Effects are opt-in (`[effects] enabled = false`) and pause on
  battery, idle, lock and Fn-off.
- `off` switches the firmware backlight off and never writes black into the zones.
- The daemon's tests and the demo use the mock backend, and the module's tests run it inside a VM.
  None of them touch the keyboard or write under this machine's `/sys`.

## Build and install

Arch packages: `cmake ninja gcc sdbus-cpp nlohmann-json tomlplusplus cli11 gtest`, plus `lua` for the
hook's behaviour tests (they skip without it).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOMEN_KBD_SANITIZE=OFF
cmake --build build
cmake --install build --prefix ~/.local         # → ~/.local/bin/omen-kbd
install -Dm644 packaging/omen-kbd.service ~/.config/systemd/user/omen-kbd.service
systemctl --user daemon-reload
```

`~/.local/bin` is on the PATH of interactive shells (via `~/.bashrc`), but not on the PATH of
Hyprland, hypridle or Waybar. So the snippets call `~/.local/bin/omen-kbd` by full path, and
`gen-hyprland` writes the generating binary's full path into the hook.

## Try it without hardware

The mock backend prints every frame as colored blocks and never touches the keyboard. Point
`XDG_STATE_HOME` at a scratch directory so the demo leaves your real state file alone. Stop the demo
before starting the real daemon, because both use the same socket.

```bash
XDG_STATE_HOME=/tmp/omen-kbd-demo omen-kbd daemon --backend mock --mock-render &
omen-kbd status
omen-kbd waybar &                            # what Waybar sees: one JSON line per change
omen-kbd set ff0000 00ff00 0000ff ffffff    # left middle right wasd: one write
omen-kbd hint super                          # the Super mode color; expires after 15 s
omen-kbd profile next
omen-kbd brightness -50
omen-kbd off; omen-kbd on                    # backlight only; colors are kept
omen-kbd effect wave                         # exit 1: effects are opt-in
kill %2 %1
```

## Commands

| Command | What it does |
|---|---|
| `status [--json]` | Current profile, zones, brightness, hint, active rules, backend, write count. |
| `set <color> [<color> <color> <color>]` | Custom colors: one for every zone, or left middle right wasd. A color is `#rrggbb` or `theme:<name>`. |
| `profile <name\|next\|prev\|custom>` | Switch profile. |
| `brightness <0-100\|+N\|-N>` | Brightness. `0` turns the backlight off; colors are kept. |
| `on`, `off`, `toggle` | Firmware backlight. Colors are kept. |
| `effect <static\|breathe\|wave\|cycle\|gradient> [--speed 1-100]` | Run an effect. Needs `[effects] enabled = true`. |
| `hint <mods\|clear>` | Show a mode color. Used by the Hyprland hook. |
| `idle on\|off` | Idle gate. Used by hypridle. |
| `flag <gamemode\|locked> on\|off` | Flags for rules and the lock gate. |
| `reload` | Re-read `config.toml` and the theme palette. A broken file is rejected and the old config stays. |
| `waybar` | Stream status lines for a Waybar custom module. |
| `doctor` | Read-only checks: driver and `readonly`, permissions, competing writers, hp-wmi DKMS packages, daemon, hooks, theme. |
| `gen-hyprland` | Print the Hyprland hook (`kbdlight.lua`) for mode colors. |
| `notify [--app TEXT] [--desktop-entry TEXT] [--urgency low\|normal\|critical]` | Report a desktop notification, for the notification flash. Used by `rice-notify-sound` (`notify-reaction.sh`). Urgency is case-insensitive. |
| `agent <waiting\|working\|done\|end> --session ID [--window ADDR] [--source NAME]` | Report what a coding-agent session is doing, for the Claude lamp. Used by `omen-kbd-agent-hook`. `--window` is the Hyprland address of the session's terminal; `--source` defaults to `claude`. |
| `daemon [--backend auto\|mock\|hp-omen-kbd\|mainline-led] [--mock-render] [--no-sources]` | Run the daemon. `auto` only picks a real backend. |

Exit codes: `0` ok; `1` the daemon or the keyboard write reported an error (message on stderr), or
`doctor` found a FAIL; `2` the daemon is not running; `64` bad usage.

## Configuration and files

- `~/.config/omen-kbd/config.toml`: yours; omen-kbd never writes it. Start from
  `packaging/config.example.toml`, which is also the built-in default when the file is missing. Run
  `omen-kbd reload` after editing.
- `~/.local/state/omen-kbd/state.json`: current profile, custom colors, brightness, on/off and the
  lifetime write counter. Written atomically, at most once a second.
- `$XDG_RUNTIME_DIR/omen-kbd.sock`: the daemon's socket (mode 0600, JSON lines).

## Reactions

Short keyboard reactions to what happens on the desktop. Each one has a small, fixed write cost, and
the tests pin those costs. They are driven by the `[reactions]` tables in `config.toml`; a missing
`[reactions]` table means they are off.

| Reaction | When | What the keyboard does | Writes |
|---|---|---|---|
| Workspace sweep | You switch workspace | The accent color (`theme:lavender`) lights one zone at a time, left to right going up, right to left going down, 120 ms a step. WASD lights with the left zone. | 4 |
| Window opened | A window opens | Every zone moves 50% toward white for 120 ms. | 2 |
| Window closed | A window closes | Every zone dips to 35% for 120 ms. | 2 |
| Attention light | A window that is not focused asks for attention (Hyprland `urgent`) | The right zone in `theme:yellow` blinks twice, then holds steady until every such window has been focused or closed. | 4 per episode |
| Fullscreen video | The focused window is in true fullscreen and its class is in the rule's list (`mpv`, `vlc`, `firefox`, `chromium`) | Brightness is capped at 15% by a rule. Super+F only maximizes and does not count. | 1 in, 1 out |
| Notification flash | swaync receives a notification | Every zone blinks once for 150 ms: `theme:red` if critical, the app's color from a table (ZapZap `theme:green`, Discord `#5865f2`), otherwise `theme:blue`. | 2 |
| Claude lamp: waiting | A Claude Code session needs approval or input | WASD in `theme:peach` blinks twice, then holds steady until the session continues, finishes or ends. | 4 per episode |
| Claude lamp: finished | A Claude Code session finishes its turn | WASD blinks once for 150 ms in `theme:green`. | 2 |

- **Holding costs nothing.** The two waiting lights (attention, Claude) blink and then hold a steady
  color. They never pulse, so a lamp that stays lit for hours writes nothing more.
- **Bursts are bounded.** Workspace switches during a sweep are absorbed. Window pulses are limited
  to one per 500 ms. At most one flash waits in a queue. More windows or sessions asking for
  attention while a lamp is lit cost 0. `max_per_minute` caps all the short reactions.
- **Silent cases cost 0 writes.** Reactions are off; the keyboard is dark (backlight off after the
  Fn key, brightness 0, lid closed, locked or idle with the gate set to `off`, suspended); or the
  hourly write budget is spent. The short reactions also stay silent while you are away (locked or
  idle) and are dropped, not saved. Notifications are skipped during do-not-disturb, for low
  urgency, and when the notifying app is the focused window. A lamp keeps its state while silent
  and shows steady, with no blinks, when the keyboard is visible again.
- **On top of a running effect** reactions add no writes: the effect's own ticks carry them, so a
  step may land up to one tick late (about 100 ms).
- **Claude lamp and focus.** For a session started directly in a terminal, the hook reports that
  terminal's Hyprland window. The lamp then does not light while that terminal is focused, clears
  when you focus it, and the "finished" flash is skipped while it is focused. Background and nested
  sessions have no window link, so their lamp and flash show whatever is focused. The same goes for
  tmux, zellij and terminals that serve several windows from one process.
- **Layers, bottom to top:** profile or effect, rule overlay, lamps, mode hint (Super, Ctrl,
  submap), short reaction, brightness caps and the backlight gates. A held modifier hides the lamps
  while it is held; a notification during fullscreen video flashes at the dimmed brightness.
- **Turning things off.** `enabled = false` turns everything off. One reaction goes off with its own
  neutral value: `open = 0`, `close = 1`, or `blinks = 0` together with `zones = []`. Colors are
  `#rrggbb` or `theme:<name>`.
- **What is never read or kept.** Keystrokes, window titles, notification text, Claude prompts and
  replies. Only a window's address and class are used, in memory. The Hyprland parser drops titles
  before anything else sees them, and nothing from Hyprland, swaync or Claude Code is logged except
  errors. The hook script reads three fields of its input (the event name, the session id and the
  notification type), never stores the input, prints nothing and always exits 0.
- **Producers are snippets.** omen-kbd never edits `config.toml`, `rice-notify-sound` or
  `~/.claude/settings.json`; the three snippets in `packaging/snippets/` are listed under Desktop
  integration. The Claude hook needs `jq`, and its tests need `bash` and `jq` (they skip without
  `jq`, and the `shellcheck` test skips without `shellcheck`).

## Desktop integration

`packaging/snippets/` holds one file per change. Each file starts with where it goes in `~/dotfiles`.
omen-kbd never edits these files itself.

1. **Start at login:** `autostart.lua`, after installing the unit above.
2. **Mode colors:** run
   `~/.local/bin/omen-kbd gen-hyprland > ~/dotfiles/config-hypr/.config/hypr/modules/kbdlight.lua`.
   Then add `require("modules.kbdlight")` to `hyprland.lua`. Re-run it after changing `[hints]` or
   moving the binary.
3. **Lock and idle gates:** `hypridle.conf`.
   - hyprlock never sets logind's `LockedHint`, and unlocking it sends no logind `Unlock`, so the
     daemon cannot see the lock from logind alone.
   - hypridle's `on_lock_cmd` / `on_unlock_cmd` run on every real session lock and unlock, however
     it started.
   - Without those two lines the keyboard stays lit on the lock screen.
4. **Waybar:** `waybar-module.jsonc` and `waybar-style.css`. Click for the next profile,
   right-click to toggle, scroll for brightness.
5. **Theme switches:** `rice-theme.md`.
6. **Game mode:** `gamemode.lua`.
7. **Rofi menu:** `kbd-menu.sh` (profile, brightness, power, effect, color, doctor). Install it as
   `~/.local/bin/omen-kbd-menu` and bind it with the full path; the header comment has both lines.
8. **Reactions config:** `reactions-config.toml`. Paste it into `~/.config/omen-kbd/config.toml`
   just before the rule named "power saver dims", then run `omen-kbd reload`. It holds the
   `[reactions]` tables and the rule `fullscreen video dims`. Rules are first-match per category,
   and both rules cap brightness, so the fullscreen rule has to come first.
9. **Notification flash:** `notify-reaction.sh`. swaync runs only one script per notification, so
   this replaces the subshell at the end of `rice-notify-sound` instead of being a second script.
   It adds one `omen-kbd notify` line inside the not-in-do-not-disturb branch, next to the ping,
   detached and with its output sent to `/dev/null`.
10. **Claude lamp:** `claude-code-hooks.json` and the hook script. `cmake --install` puts
    `omen-kbd-agent-hook` next to `omen-kbd`, in `~/.local/bin`; it needs `jq`. Merge the `hooks`
    entries of the JSON file into `~/.claude/settings.json`, next to the existing `SessionStart`
    hook, and leave out its `_comment` line. The three busy events (`UserPromptSubmit`,
    `PostToolUse`, `PostToolUseFailure`) run with `"async": true`, so the hook cannot slow Claude
    Code. Every `omen-kbd` and `hyprctl` call inside it is capped at 1 s.

`omen-kbd doctor` lists what is still missing.

## Kernel module (P3)

`kernel/hp-omen-kbd/` is a small out-of-tree module that bridges sysfs to the firmware's color table.
`core.c` holds every decision and also compiles into userspace tests; `main.c` adds what needs the
kernel. It loads only on board 8A4E, with the HP BIOS WMI interface, and with a four-zone keyboard.
Anywhere else it returns `ENODEV`. Nothing loads it automatically until the package is installed.

**P4 validated it on this machine's real firmware on 2026-11-06** (`docs/hardware-validation.md`,
results in `docs/hardware-notes.md`). Its default is now `readonly=0`.

P5 installs it as a DKMS package that loads it at every boot (`docs/p5-install.md`). Before that,
load it only by hand, the way the P4 runbook does. The tests below load it
only inside QEMU, which boots this machine's own kernel with an ACPI table standing in for HP's
firmware. That checks the module's logic and how it behaves in the kernel.

Arch packages: `base-devel linux-headers qemu-system-x86 acpica libarchive dkms namcap`.

```bash
make -C kernel/hp-omen-kbd                    # build against the running kernel
make -C kernel/hp-omen-kbd check              # W=1 with -Werror, checkpatch --strict, module and docs checks
make -C kernel/hp-omen-kbd vmtest-gate        # the load gate, in a VM; no acpica needed
make -C kernel/hp-omen-kbd vmtest             # every scenario, against the firmware emulator
kernel/hp-omen-kbd/tests/check-packaging.sh   # DKMS and the Arch package, built as you, nothing installed
```

`kernel/hp-omen-kbd/tests/vm/run.sh <scenario>` runs one scenario and keeps its console output in
`tests/vm/out/<scenario>/console.log`. A scenario lists commands (`load`, `write`, `suspend`, …), and
its `.expected` file lists exactly what they must print. After a kernel update without a reboot,
`run.sh` refuses to run, because the VM would test another kernel than the running one.

- Loading it on the hardware, one approved step at a time: `docs/hardware-validation.md`.
- If something goes wrong: `docs/recovery.md`.

## Development

- Debug build and tests, with AddressSanitizer and UndefinedBehaviorSanitizer:
  ```bash
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOMEN_KBD_SANITIZE=ON
  cmake --build build && ctest --test-dir build --output-on-failure
  ```
  Builds use `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`.
- **Formatting:** `.clang-format` (LLVM base, 4 spaces, 100 columns); check with
  `clang-format --dry-run --Werror <files>`. Make sure your editor's formatter picks up this file.
  A formatter that sends the buffer on stdin without the file name falls back to LLVM's 2-space
  style.
- **Static analysis:** `run-clang-tidy -p build src` (checks in `.clang-tidy`).
- **clangd:** `.clangd` points at `build/compile_commands.json`. Sources are globbed at build time,
  so a new file shows bogus errors until the next `cmake --build build`; restart clangd after that.
- **Layout:**
  - `src/core`: pure logic (colors, zones, palette, effects, rules, compositor).
  - `src/backend`: mock, hp-omen-kbd sysfs and mainline LED backends.
  - `src/daemon`: model, epoll loop, writer thread.
  - `src/sources`: Hyprland, theme, hwmon, system D-Bus.
  - `src/ipc`, `src/cli`: socket protocol and command line.
  - `tests/`: GoogleTest.
  - `kernel/hp-omen-kbd`: the module, its core tests and the VM scenarios.
  - `packaging/`: service unit, config example, snippets, udev rule, sysusers, PKGBUILD.
- The Rust prototype this port follows is kept at the git tag `rust-prototype`.

## Documents

- Design spec (bugs found in omen-space, alternatives, design, phases):
  `docs/superpowers/specs/2026-10-21-omen-kbd-design.md`
- Hardware notes for this laptop: `docs/hardware-notes.md`
- Hardware validation runbook (P4): `docs/hardware-validation.md`
- Install and soak runbook (P5): `docs/p5-install.md`
- Recovery: `docs/recovery.md`
- Implementation plans: `docs/superpowers/plans/`
