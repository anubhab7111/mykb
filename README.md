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
| Kernel module `hp_omen_kbd` | **Not written yet** (next phase). The stock kernel has no keyboard-RGB interface, so until the module is loaded, `omen-kbd daemon` exits with "no keyboard backend" and `doctor` reports a FAIL. Use `--backend mock` to try everything. |
| Hardware validation | After the module, one approved step at a time (see the spec). |

## Safety

- The kernel's own `hp-wmi` is never replaced or patched. The daemon runs as your user, not root.
- No keylogging: the Hyprland hook looks only at the 8 modifier keys and logs nothing.
- Colors are written only through the small `hp_omen_kbd` module. It is allowlisted to board 8A4E
  and loads `readonly=1` until validation. It enforces ≥ 50 ms between writes and an hourly write
  budget. Each change is one atomic write of all four zones.
- A static profile costs zero writes. Effects are opt-in (`[effects] enabled = false`) and pause on
  battery, idle, lock and Fn-off.
- `off` switches the firmware backlight off and never writes black into the zones.
- Tests and the demo use the mock backend. Nothing in this repo writes under `/sys`.

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

`omen-kbd doctor` lists what is still missing.

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
- The Rust prototype this port follows is kept at the git tag `rust-prototype`.

## Documents

- Design spec (bugs found in omen-space, alternatives, design, phases):
  `docs/superpowers/specs/2026-10-21-omen-kbd-design.md`
- Hardware notes for this laptop: `docs/hardware-notes.md`
- Implementation plans: `docs/superpowers/plans/`
