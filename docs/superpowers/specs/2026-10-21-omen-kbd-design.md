# omen-kbd — a keyboard color manager for the HP OMEN 16-k0xxx (board 8A4E)

> Status: **approved by the user 2026-10-21.** No code, no hardware writes yet. **Until P1 is done: no reboots and no kernel updates** (`pacman -Syu` bumping
> `linux` makes DKMS build omen-space's hp-wmi fork for the new kernel).

## Context

The user controls the keyboard backlight of an **OMEN by HP 16-k0xxx (DMI board 8A4E, BIOS F.21,
Arch, kernel 7.2.9)** with [omen-space](https://github.com/yunusemreyl/omen-space) and finds it very buggy.
They want: (1) the bugs in omen-space's keyboard-color implementation identified, (2) research into
alternatives, (3) a design for their own keyboard color manager, with hardware safety as a top priority.

### What the hardware actually is (verified read-only on this machine)

- Keyboard is the PS/2 "AT Translated Set 2" device; **no USB/HID per-key RGB controller** (lsusb/hidraw).
  So this is the **4-zone RGB backlight controlled through HP's BIOS WMI interface**
  (GUID `5FB7F034-…-3D44E2C707E4`, command `0x20009`, 128-byte color table, colors at byte 25).
- Current zone colors (read via sysfs): `E19508 2EFD53 1E6AF7 D102AC`; zones 4–7 read `000000`.
- Firmware has **3 hardware backlight levels (off / L1 / L2)** driven by the Fn key; the event codes
  (`0x30021aa`, `0x33221aa`, `0x36421aa`) are currently ignored by the driver keymap.
- Mainline hp-wmi (the backed-up stock 7.2.9 module) has **no keyboard RGB support** — depends only on
  `sparse-keymap, rfkill, wmi` (no `led-class-multicolor`), no kbd/LED strings. 8A4E is not in its board
  list either; today's power profiles (`cool quiet balanced performance`) look like the generic path, so
  they should survive a return to stock hp-wmi (verified in P1).
- Upstream patch (v10, see Part 2) documents the hardware zone order as **0=right, 1=center, 2=left,
  3=wasd** — matching omen-space's "classic" mapping — and a keyboard-type query (`0x20008`/`0x2B`).

### What is installed right now (a mess, and part of why it's "buggy")

| Component | State |
|---|---|
| `omen-space-daemon` (installed 2025-09-28, older than upstream v2.1.8) | enabled, running, mode `cycle`; **~13% of a CPU core continuously** (4m35s CPU in 34 min) |
| DKMS `hp-omen-extra` 2.1.2 | installed; builds **both** `hp-omen-extra.ko` **and a replacement `hp-wmi.ko`** (stock module moved aside by DKMS) |
| Loaded `hp_wmi` | srcversion `EE58EE85…` matches **no file on disk** — it's the old pelrun-based `hp-omen-wmi-dkms` build loaded at boot (removed today 15:20). **The next reboot — even before any migration — loads omen-space's hp-wmi fork from `updates/dkms`.** |
| DKMS `omen-rgb-keyboard` 1.4 | installed, not loaded (another RGB driver for the same hardware) |
| `/usr/src` leftovers | `hp-omen-wmi-{0.6.2,0.9,0.10}`, `hp-omen-extra-2.0.7` |
| `/etc/modules-load.d/hp-omen-extra.conf` | autoloads `hp_omen_extra` |
| `openrgb.service` | installed, disabled |
| `/etc/modprobe.d` | **no** hp_wmi blacklist (good — stock module can come back cleanly) |

---

## Part 1 — Bugs found in omen-space (keyboard color path, upstream v2.1.8 `3c090d1`)

Severity: **H** = causes user-visible breakage or hardware/system risk, **M** = wrong behavior in common use, **L** = edge case/quality.

### Kernel companion driver — `driver/hp-omen-extra.c` (unchanged upstream)

| # | Sev | Bug | Where |
|---|---|---|---|
| K1 | H | **Non-atomic read-modify-write per zone.** Each zone write = WMI GET of the whole 128-byte table, patch 3 bytes, WMI SET. The mutex only wraps each single WMI call, not the GET→SET pair, so two concurrent zone writes lose updates (flicker/wrong colors). Also not serialized against any other driver writing the same table. | `zone_store` 206–244; lock 117–119 |
| K2 | H | **Positive firmware error codes leak out as byte counts.** `hp_wmi_perform_query` returns the BIOS return code (e.g. 3, 5); `brightness_show/store` and `win_lock_show/store` return it directly. A sysfs `show` returning 3 = "3 bytes of output"; a `store` returning 3 = "consumed 3 bytes" → userspace retries/garbage. | 257–258, 283, 302–303, 325 |
| K3 | H | **No hardware allowlist.** Loads on any machine with the HP BIOS WMI GUID (every HP laptop) and exposes raw writes to lighting command `0x20009`. | init 360 |
| K4 | M | **Exposes zone0–7 on a 4-zone keyboard.** Writing zone4–7 overwrites table bytes 37–48, whose meaning is undocumented. The daemon's "8-zone" override can trigger this. | 178, 331–338 |
| K5 | M | **No suspend/resume handling.** No PM ops → nothing restores colors if firmware resets them. | (absent) |
| K6 | M | **udev race:** attributes added with `sysfs_create_groups` *after* `platform_device_register_simple` → the `add` uevent can fire before files exist, so the chgrp/chmod rule may miss them. | 366–375 |
| K7 | L | `brightness_show` compares the whole u32 to `0xE4`; any extra bits → reports "off". | 261 |
| K8 | L | Hex input not range-checked (`1FFFFFF` silently truncated). | 222 |
| K9 | L | `win_lock` uses a guessed protocol; on this board it returns `ENODEV` (observed). | 287–326 |

### Daemon RGB engine — `src/omen-space-daemon/src/rgb/mod.rs`

| # | Sev | Bug | Where |
|---|---|---|---|
| D1 | H | **Software animation hammers ACPI/WMI.** 20 Hz loop × 4 zones × (GET+SET) = up to **160 ACPI method calls/s**, forever, in animated modes. Likely the main cause of today's ~13% CPU (it coincides with `cycle` mode; earlier boots used far less, and the daemon has other loops). Never pauses on battery, lid close, idle, or screen lock. | 626–766, sleep 764 |
| D2 | H | **Turning the backlight "off" writes black into all zones** (as well as brightness 0). If the firmware/Fn key turns the backlight back on, the keyboard is black → "lighting stuck off". | 822–825 |
| D3 | H | **No suspend/resume re-apply.** Static colors are applied only at start + one 5 s deferred retry. | 515–525; main.rs |
| D4 | H | **Config data loss on restart.** `SetColor` grows `colors` to 17 entries and saves; `RgbConfig::load` truncates to 8 → colors 8–16 (lightbar/desktop) are lost after every restart. Index 8 is also overloaded as "all zones", so zone 8 can never be set. | 446–452 vs 869–876; 862, 872 |
| D5 | M | **Non-atomic config writes** (`fs::write` straight to `/etc/omen-space/rgb.json`); a crash mid-write corrupts it and `unwrap_or_default()` silently resets all settings. Saved on *every* D-Bus call and every brightness tick. | 438–466 |
| D6 | M | **All hardware errors swallowed** (`let _ = fs::write`); every D-Bus method returns `"OK"` even when nothing changed. | 152, 160, 174, 181; 881/898/915 |
| D7 | M | **Global mutex held across blocking sysfs I/O inside async code** (std::fs in tokio tasks) → D-Bus calls stall behind animation frames. | 629–763, 768–848 |
| D8 | M | **Late driver never picked up by the animation loop** (only `apply_state` re-detects) → unit uses an `ExecStartPre=/bin/sleep 2` workaround. | 637–640; service file |
| D9 | M | **Re-detection drops the user's zone-count override** (calls `detect()` not `detect_with_override`). | 776–781 |
| D10 | M | **Fn-key brightness changes are invisible** to the daemon (hp-wmi keymap marks them KE_IGNORE) → daemon state and real backlight drift apart. | hp-wmi.c keymap 551–553 |
| D11 | L | Animation `color_cache` not updated by `apply_state` → stale skips after mode/power changes. | 756–759 vs 841–845 |
| D12 | L | Negative D-Bus ints cast to `u32` (brightness −1 → 100). | 892, 909 |
| D13 | L | `uleds` brightness bridge never reconnects after an error and is one-way (daemon changes don't update the desktop slider). | 593–623 |
| D14 | L | Per-key map saved under `/root/.config/…` but the unit has `ProtectHome=read-only` → save always fails. | 37; service file |
| D15 | L | Per-key animations computed for 104 keys are then collapsed to 4 zone colors (`set_zone_colors` instead of `write_per_key_colors`). (Not this laptop.) | 740 |

### GUI / CLI (`src/omen-gui/src/keyboardrgb/*`, `daemon_client.rs`)

| # | Sev | Bug | Where |
|---|---|---|---|
| G1 | H | **Saved colors never load in the GUI**: it reads `state["zones"]` (object), the daemon returns `colors` (array) → always shows default `#0099ED`, and the next click pushes that stale default to the hardware. | page.rs 81–96 |
| G2 | M | **"_sync" calls are fire-and-forget** `rt.spawn`s → slider drags fire dozens of concurrent D-Bus calls that can land **out of order**; each one also writes the config file and the hardware. | daemon_client.rs 245–255, 436–447 |
| G3 | M | Choosing "wave" silently **resets brightness to 100**; moving the brightness slider **forces direction to `ltr`**. | keyboard.rs 146–152, 644–646 |
| G4 | L | Two unrelated "zone override" settings with different meanings (GUI `zone_override` 1–4 = layout kind; daemon `zone_count_override` 4/8). | page.rs 26–52 |
| G5 | L | Error notification text hard-coded in Turkish. | daemon_client.rs 442 |

### System-level / packaging

| # | Sev | Issue |
|---|---|---|
| S1 | H | **Replaces the kernel's own `hp-wmi`** (fans, hotkeys, platform profile, rfkill) with a 3,900-line fork just to ship RGB. 8A4E is not in any of the fork's board tables. omen-space's own known-issues log records an install that left a system **unbootable** (#211) from this approach. |
| S2 | M | Multiple RGB drivers co-installed with no detection of competing **RGB** writers (conflict detector only looks for other hp-wmi DKMS builders). |
| S3 | M | Monolithic root daemon: RGB shares a process with fan/undervolt/MSR/auto-updater code; any panic or slow subsystem affects lighting, and vice-versa. |

---

## Part 2 — Research on alternatives

| Option | Approach | Verdict for this laptop |
|---|---|---|
| **Upstream hp-wmi multicolor LED series** (Edip Hazuri, v1 Feb 2026 → [v10 Jun 2026](https://lkml.rescloud.iu.edu/hypermail/linux/kernel/2606.1/13298.html)) | Per-zone `led_classdev_mc`: `hp::kbd_zoned_backlight-{right,center,left,wasd}`, max 255; gated on keyboard-type query; Fn-key changes via `brightness_hw_changed`; per-zone GET+SET, no cache/lock; no explicit resume restore | **The future standard interface.** Not in mainline: the [7.3 pdx86 merge](https://github.com/torvalds/linux/commit/5b05bb3f6c57) has no keyboard LED work; no evidence of being queued. Earlier attempts ([2021](https://lkml.iu.edu/hypermail/linux/kernel/2110.3/00191.html), [2023](https://lkml.iu.edu/2301.1/09123.html), [2024](https://lkml.iu.edu/hypermail/linux/kernel/2407.0/08119.html)) never landed. Design must be able to switch to it. |
| [pelrun/hp-omen-linux-module](https://github.com/pelrun/hp-omen-linux-module) (+ forks; AUR `hp-omen-wmi-dkms`) | **Replaces hp-wmi** (based on Linux 5.17); `rgb_zones/zone00-03` per-zone RMW | Self-described "totally experimental… could easily crash your machine". Rejected (replaces hp-wmi). It is the driver currently loaded in memory. |
| [OmenLinux/omen-rgb-keyboard](https://github.com/OmenLinux/omen-rgb-keyboard) | Separate module; **animations on a kernel timer**, kernel-side file I/O for state (`filp_open`/`kernel_write`), fan control, mute LED; warns of conflicts with hp_wmi | Rejected: effects + file I/O in kernel, overlaps hp-wmi, same per-zone RMW. |
| [nevetscar/hp-omen-gaming-wmi-dkms](https://github.com/nevetscar/hp-omen-gaming-wmi-dkms) | **Replaces hp-wmi**; atomic `fourzone_color` (24 hex chars) | Good idea (atomic write) — borrowed; replacement of hp-wmi rejected. Targets board 8E35. |
| [omen-space](https://github.com/yunusemreyl/omen-space) | Rust root daemon + `hp-omen-extra` + hp-wmi fork | Bugs in Part 1. |
| OpenRGB | — | No evidence it supports HP OMEN laptop WMI keyboards (installed here, service disabled). Not a path. |
| OmenMon (Windows) | Reference for HP BIOS WMI calls | Protocol reference only. |
| `acpi_call` | Arbitrary ACPI method execution from userspace (root) | Rejected on safety grounds. |

**Protocol facts used by the design** (consistent across hp-omen-extra, omen-rgb-keyboard and the v10
patch): BIOS GUID `5FB7F034-…`, signature `SECU`; command `0x20009` types `0x02`/`0x03` = get/set the
128-byte color table (RGB at offset 25, zone×3); types `0x04`/`0x05` = get/set backlight (`0xE4` on,
`0x64` off); command `0x20008` type `0x2B` = keyboard type (`1` = 4-zone + numpad, `2` = 4-zone no numpad,
`3` = per-key, `4`/`5` = single zone, `0` = none). Upstream notes "turning the backlight on via WMI turns
all zones on". **No firmware-native animation commands are documented anywhere** → effects are software.
**No WMI brightness levels are documented** (only on/off) → brightness is RGB scaling.

**Key design takeaways:** (1) a kernel piece is unavoidable today; (2) keep it as small as possible and
separate from hp-wmi; (3) do the atomic 4-zone write nobody upstream does; (4) structure the daemon so a
future mainline LED backend drops in and the custom module can be deleted.

## Part 3 — Design: `omen-kbd`

### Decisions the user made (2026-10-21)

- **Pain points to eliminate:** colors not sticking (sleep/reboot/Fn key) and lag / wrong colors.
- **Replace omen-space fully** (remove it and its hp-wmi fork; go back to the stock kernel hp-wmi).
- **Interface:** CLI + config file, plus a **Waybar module**.
- **Behaviours:** static zones + profiles; gentle animations; react to system state; follow desktop theme.
- **Keybinding lighting = "Mode colors":** holding a modifier (after a delay) or entering a Hyprland submap
  repaints the whole keyboard in that mode's color (e.g. Super = mauve, Super+Shift = blue, Ctrl = peach,
  resize submap = teal); release → back to the profile. (True per-key bind lighting is impossible on this
  4-zone hardware.)
- **Migrate off omen-space first**, before building anything (the next reboot would otherwise load
  omen-space's untested hp-wmi fork).
- The user sees their **last colors before login after a full power-off**. That's confounded (omen-space's
  root daemon starts before the greeter and repaints), so persistence in non-volatile memory is **not
  established** — but the user chose the conservative policy anyway: **animations are opt-in, and the
  kernel enforces an hourly write budget.** First clean test: P1 step 7. (Weeks of omen-space writing at up
  to ~80/s without failure is weak evidence *against* per-write flash commits.) The result revisits the
  budget and opt-in default.
- Pre-login colors don't matter: applying the profile at login is enough (no root boot step).

### Calls Claude made (please check while reading)

- One binary for daemon + CLI (C++23 — the user's choice, replacing the original Rust call), C DKMS
  module; Unix-socket JSON IPC instead of D-Bus.
- The kernel module exposes **sysfs + a group** (`omen-kbd`) rather than a `/dev` node with seat ACLs.
- Allowlist is **board 8A4E only**; `readonly=1` by default until P4 is signed off.
- Default limits: ≥ 50 ms between writes, 40 000 writes/hour budget, 10 fps effect default (max 20).
- Mode colors via a **Lua hook inside Hyprland** (only modifier changes leave the compositor), 250 ms
  hold delay, 15 s safety expiry; fallback to bare-modkey binds only if the spike fails.
- Theme colors made **vivid** by default (pastels look white on LEDs).
- "Off" = firmware backlight off, colors untouched.
- Config file is never rewritten by the daemon; runtime state lives in `~/.local/state/omen-kbd/`.
- User-config changes are delivered as patches to `~/dotfiles`, never written by the tool.

### Goals

1. Colors you set **always stick**: across reboot, suspend/resume, Fn-key toggles, daemon restarts.
2. **Instant, correct** response: one atomic write for all 4 zones; latest command always wins.
3. **Cheap**: zero hardware traffic when nothing changes; animations capped and auto-paused.
4. **Safe by construction**: tiny kernel surface, hardware allowlist, read-only first load, rate limit
   enforced in the kernel, never replaces or patches any in-tree driver.
5. Integrates with the user's Hyprland rice: live binds, submaps, rice theming, Waybar, hypridle, gamemode.

### Non-goals

- Fan control, power limits, undervolt, MUX, overlays (omen-space's other features). Power modes stay with
  stock `platform_profile` + `tlp-pd` (already running).
- Per-key effects (hardware doesn't support them). Support for other HP models in v1.
- GUI app (Waybar + CLI cover it; can be added later on the same socket API).

### Anti-goals (would make it a failure even if it "works")

- Replacing/forking `hp-wmi`, blacklisting in-tree modules, or touching initramfs/bootloader.
  (Autoloading our small, allowlisted module via `modules-load.d` is fine: on any mismatch it returns
  `-ENODEV` and cannot block boot.)
- A root daemon, or reading every keystroke system-wide (keylogger-shaped) to implement mode colors.
- Continuous ACPI **writes** while nothing changes, or periodic reads faster than every few seconds;
  writes to undocumented bytes of the firmware color table.
- Silently reporting success when the hardware write failed.

### Architecture

```
 Hyprland Lua hook/socket2 ┐        ┌─ UPower, tlp-pd (PowerProfiles API), logind (system D-Bus)
 rice-theme / gamemode ────┤        ├─ hwmon temps (poll only if a rule needs it)
 hypridle, CLI, Waybar ────┤        │
                           ▼        ▼
                 ┌────────────────────────────────┐   systemd --user service
                 │ omen-kbd daemon (Rust, tokio)  │   (runs as the user, not root)
                 │  sources → compositor → writer │
                 └───────────────┬────────────────┘
                                 │ one write = all 4 zones (sysfs, group-writable)
                 ┌───────────────▼────────────────┐
                 │ hp_omen_kbd.ko (DKMS, ~300 LoC)│   allowlist: board 8A4E only
                 │ rate-limit + budget + resume   │   readonly=1 until validated
                 └───────────────┬────────────────┘
                                 │ WMI 0x20009 GET/SET (same calls every existing driver uses)
                          HP BIOS / EC (4-zone keyboard)
```

A kernel piece is unavoidable: the stock kernel has no keyboard-RGB interface, and the only userspace
alternative (`acpi_call`, arbitrary ACPI execution as root) is rejected on safety grounds. The module is
deliberately **temporary**: the daemon talks to hardware only through a `Backend` trait with three
implementations — `mock`, `hp-omen-kbd` (our module), and `mainline-led` (the upstream
`hp::kbd_zoned_backlight-*` multicolor LEDs, 4 writes per frame, effects capped lower). When a kernel with
the upstream series is installed, the daemon selects `mainline-led`, `doctor` says to remove the DKMS
module, and nothing else changes. The daemon refuses to start if both are present (two writers).

### Component 1 — kernel module `hp_omen_kbd` (C, DKMS)

Purpose: the smallest correct bridge from sysfs to the firmware color table. No effects, no file I/O,
no input handling, no fan/power code. It does **not** claim the WMI GUID and does **not** replace `hp-wmi`
(DKMS ships only `hp_omen_kbd.ko`).

- **Load gate:** `dmi_check_system()` allowlist (`DMI_BOARD_VENDOR=HP`, `DMI_BOARD_NAME=8A4E`) **and**
  `wmi_has_guid(5FB7F034-…)`; otherwise `-ENODEV`. Adding a board is a code change, not a module param.
- **Probe (read-only):** query keyboard type (cmd `0x20008`, type `0x2B`) and **refuse to bind unless it
  is `1` or `2` (four-zone)**; GET color table (cmd `0x20009`, type `0x02`, 128 B) and backlight
  (type `0x04`); keep a `boot_table` copy; log keyboard type + a hex dump at `pr_info` (needed for P4).
  (Detecting a mainline `hp::kbd_zoned_backlight-*` LED is done by the daemon and `doctor`, not the
  module — there is no clean in-kernel lookup of LED devices by name. Packaging notes say to remove the
  DKMS module once such a kernel is installed.)
- **Module params:** `readonly` (default **1** until hardware validation is signed off; writes return
  `-EPERM`), `min_interval_ms` (default 50, clamped to ≥ 33 → hard cap ~30 writes/s),
  `max_writes_per_hour` (default 40000 ≈ one opted-in 10 fps effect running a full hour; rolling window;
  when exhausted, writes fail with `-EBUSY` and `info` says so — a runaway guard, since colors appear to
  live in non-volatile memory), `restore_on_unload` (default 1).
- **sysfs** (`/sys/devices/platform/hp-omen-kbd/`), attributes attached **before** the device is added
  (fixes omen-space bug K6):
  - `zones` (rw): read = last known 4 colors `RRGGBB RRGGBB RRGGBB RRGGBB\n` (cache; no ACPI call);
    write = exactly 4 hex colors (or 1 = all). Strict parsing; anything else `-EINVAL`.
  - `backlight` (rw): `0`/`1` (firmware on/off via types `0x04`/`0x05`, `0xE4`/`0x64`, decoded by bit,
    not `== 0xE4`). Reads hit firmware but are cached for 200 ms. **"Off" never touches the color table**
    — this is the direct fix for omen-space bug D2 (black zones after the Fn key turns lighting back on).
  - `info` (ro): board, readonly, min_interval, write count, last error, header bytes 0–24 of boot table.
- **Write path (synchronous):** `zones_store` takes the mutex, waits out the remainder of
  `min_interval_ms` (bounded), checks the hourly budget (`-EBUSY` if exhausted), then does **fresh GET →
  patch only bytes 25–36 → SET** and returns the **real result** (count or negative errno) to the writer.
  Coalescing ("latest frame wins") lives in the daemon's writer, not the kernel. Unknown bytes are always
  preserved exactly as the firmware just returned them; if header bytes differ from the boot snapshot it is
  logged once (not silently "fixed"). Zones 4–7 do not exist. The only work item is the resume re-apply.
- **Errors:** BIOS return codes map to negative errnos (`-EIO`, `-EOPNOTSUPP`); never returned as positive
  byte counts (omen-space bug K2). Last error visible in `info`.
- **Power management:** on `resume`/`restore`, if colors were written since load, re-apply them (and the
  backlight state) via the same worker after a short delay.
- **Unload:** if anything was written and `restore_on_unload=1`, SET the boot colors back.
- **Permissions:** files are 0644 root; a udev rule `chgrp omen-kbd` + `chmod 0664` on `zones`/`backlight`.
  The user joins group `omen-kbd`.

### Component 2 — `omen-kbd` (Rust; one binary: daemon + CLI)

Runs as a **systemd user service**, so it has natural access to the Hyprland socket, the theme files,
and the session D-Bus — no root, no D-Bus policy files, no polkit. Because this rice starts Hyprland via
`start-hyprland` (no uwsm), `graphical-session.target` is never active; the unit therefore has no
`WantedBy` and is started by one line in `modules/autostart.lua`
(`systemctl --user start omen-kbd.service`), alongside the rice's other session services.

User-config edits (autostart line, `kbdlight.lua`, hypridle listener, `rice-theme` reload hook, Waybar
module, gamemode hook) are proposed as patches to the user's stow repo `~/dotfiles/` — never written
by the tool.

**Internal pipeline (actor style, no shared mutex):**
`sources` (events) → `compositor` (layer stack → target frame) → `writer` (owns the backend; writes only
when the frame changes; one in-flight write; latest frame wins). Animations tick only while an animated
layer is on top.

**Layer stack (lowest → highest priority):**

| Layer | Contents | Set by |
|---|---|---|
| Base profile | static 4 colors **or** an effect, brightness | user (CLI/Waybar), rules that switch profile |
| Rule overlays | tint / alert on some zones (e.g. low-battery pulse on the right zone, hot-CPU tint) | rules engine |
| Mode hint | whole-keyboard mode color while a modifier is held / a submap is active | Hyprland Lua hook + socket2 |
| Gates (force) | off (user), lid closed, suspend, lock/idle (dim or off), Fn backlight off (pause) | logind, hypridle, module `backlight` |

**Colors** are `#rrggbb` or `theme:<name>`. **Brightness** 0–100 is RGB scaling in software (the
firmware only documents on/off); brightness 0 or `off` = firmware backlight off, colors untouched.

**Profiles & effects (zone-level, software — no firmware animation commands exist):** `static`,
`breathe`, `wave` (across L→M→R, WASD follows one), `cycle` (hue rotation), `gradient` (from theme
colors), `pulse` (for alerts). **Animations are opt-in**: `[effects] enabled = false` by default;
until enabled, effect profiles fall back to their first frame (static) and `omen-kbd effect …` explains
how to enable them in one line. Frame rate default **10 fps** (final default set by the P4 rate test),
configurable, hard max 20 (kernel caps at ~30 anyway). Default auto-pause: on battery → static fallback;
idle/lock/Fn-off/suspend → stop ticking. A static profile costs zero hardware traffic. If the kernel
reports the hourly budget exhausted (`-EBUSY`), the daemon drops to static and surfaces it in
`status`/Waybar. The daemon also keeps a lifetime write counter in `state.json` (shown by `status`) so
write volume is visible over time. Mode colors and rules are event-driven (a few writes per event).

**Rules engine (declarative, event-driven, first-match per category):** conditions limited to a fixed
set: `power_profile`, `on_battery`, `battery_below`, `cpu_temp_above`, `gamemode`, `locked`, `idle`.
Actions: switch `profile`, set `brightness`, or add an `overlay`.

**Mode colors (keybinding feature):**
- Modifiers: a small Lua module in the user's Hyprland config (`modules/kbdlight.lua`, required from
  `hyprland.lua`) uses Hyprland 0.56's `hl.on("input.keyboard.key", fn(keycode, time, state))`. It keeps
  only the 8 modifier keycodes (Super/Shift/Ctrl/Alt, L and R; XKB codes 133/134, 50/62, 37/105,
  64/108), tracks the held set, and **only if the set has a configured mode color and is still held after
  the delay** (an `hl.timer`, default 250 ms) runs `omen-kbd hint <mods>` via `hl.exec_cmd`; on release it
  runs `omen-kbd hint clear` only if a hint was sent. Typing (Shift+letter) and quick chords (Super+Q)
  therefore spawn nothing. Every other key is ignored inside the compositor; no keystrokes leave
  Hyprland, no evdev, no `input` group. Release is reliable because every release fires the event.
  `omen-kbd gen-hyprland` prints this Lua module (with the mode-colored modifier sets from config); the
  user saves it into their config — the tool never writes into `~/.config/hypr` itself.
  _Fallback if the spike fails:_ bare-modkey binds `hl.bind("Super_L", …, { non_consuming = true })` +
  `{ release = true }` (documented flags; chord-release behavior unverified).
- Submaps: daemon subscribes to Hyprland `socket2` (`submap>>NAME`, empty = default) — no binds needed.
- **First P2 task is a software-only spike** proving the Lua hook: `omen-kbd hint` just logs; check
  Super+Q chords, rapid taps, Super held across a workspace switch, and lock/unlock.
- Safety net in the daemon: a hint expires after 15 s (configurable) and is cleared on lock/suspend, so a
  lost release can never leave the keyboard stuck in a mode color.
- Colors per mode in config (`super = "theme:mauve"`, `"super+shift" = "theme:blue"`, `ctrl =
  "theme:peach"`, `submap.resize = "theme:teal"`).

**Theme sync:** palette = `~/.config/theming/current/omen-kbd.toml` if the theme ships one, else parsed
from `~/.config/theming/current/waybar-colors.css` (`@define-color name #hex`). Pastel theme colors
(Mocha mauve `#cba6f7`, blue `#89b4fa`…) look near-white on RGB LEDs, so by default theme colors are
made **vivid** (hue kept, saturation/value boosted); `theme_colors = "exact"` turns that off. _(My call.)_ Reload triggered by a
one-line `omen-kbd reload` added to `rice-theme`'s `reload_apps()` (user's rice repo), with an inotify
watch on the `current` link as a fallback.

**System-state sources:** `net.hadess.PowerProfiles` (ActiveProfile), UPower DisplayDevice
(Percentage/State), logind (`PrepareForSleep`, session `Lock`/`Unlock`, `LidClosed`) — all signals, no
polling (the power-profiles service here is `tlp-pd`, which implements the same
`net.hadess.PowerProfiles` interface). **Idle** comes from hypridle (it publishes nothing on D-Bus): the
existing keyboard listener in `hypridle.conf` (currently `brightnessctl -sd rgb:kbd_backlight …`, a device
that doesn't exist) is **replaced** by `on-timeout = omen-kbd idle on` / `on-resume = omen-kbd idle off`,
keeping its timeout and `condition_cmd`.
**Gamemode**: the rice's `rice.gamemode.toggle()` calls `omen-kbd flag gamemode on|off`. hwmon CPU
package temp polled every 2 s **only** if a temperature rule exists. Fn-key state: if P1 step 7 shows
stock hp-wmi emits an input event for the Fn backlight key, listen to that; otherwise module `backlight`
is read **only while an effect is running** (every 3 s, to pause when lighting is off) and on demand for
`status`/Waybar — never polled with a static profile.

**Boot behavior:** the module restores colors only on resume, and the daemon starts with the Hyprland
session, so **before login the keyboard shows whatever the firmware/EC kept**. After login the daemon
applies the saved profile. (P4 checks what a cold boot actually shows.)

**Files:**
- `~/.config/omen-kbd/config.toml` — user-owned; the daemon **never writes it**; `omen-kbd reload`.
- `~/.local/state/omen-kbd/state.json` — current profile/brightness/on-off; atomic write (tmp+rename),
  debounced 1 s (fixes omen-space D4/D5).
- Zone order mapping in config: logical `left/middle/right/wasd` → hardware index; default
  L→2, M→1, R→0, WASD→3 (agrees with both omen-space and the upstream patch), **verified in P4**.

**CLI** (talks to the daemon over `$XDG_RUNTIME_DIR/omen-kbd.sock`, JSON lines, mode 0600):
`status [--json]`, `set <c> [c c c]`, `profile <name|next|prev>`, `brightness <0-100|+N|-N>`,
`on|off|toggle`, `effect <name> [--speed]`, `hint <mods|clear>`, `idle on|off`,
`flag <name> on|off` (e.g. gamemode), `reload`, `waybar`, `doctor`, `daemon [--backend mock|hp-omen-kbd|mainline-led]`.
Every command reports the real outcome (errors from the writer propagate back; omen-space bug D6).

**Waybar:** `custom/kbd` with `exec = omen-kbd waybar` (streams one JSON line per state change: text,
tooltip with zones/brightness/active rule, class = profile/mode, percentage = brightness);
`on-click` = `profile next`, `on-click-right` = `toggle`, scroll = `brightness ±10`. Styled from the
existing `colors.css`.

**`omen-kbd doctor`:** module loaded/readonly/board, sysfs permissions & group membership, competing
writers (omen-space-daemon, `hp_omen_extra`, `omen_rgb_keyboard`, OpenRGB, a `rgb_zones` from a
pelrun-style hp-wmi), DKMS packages shipping `hp-wmi.ko`, `kbdlight.lua` hook loaded, theme palette found,
hourly write budget remaining.

**Mock backend:** `omen-kbd daemon --backend mock` renders frames as truecolor blocks in the terminal
(or to a log) and never touches hardware. All features except the final sysfs write are developed and
tested against it. The real backend is never chosen implicitly as a fallback, and vice-versa.

### Language & stack (user's decision, 2026-10-22)

**C++23** for the daemon/CLI, so the user can maintain it themselves (a Rust prototype of plan Tasks
1–13 exists at git tag `rust-prototype` and is the behavioural reference for the port). GCC 16 / Clang
23, CMake + Ninja, GoogleTest; system libraries only: `sdbus-c++` (system-bus client, integrated into
the daemon's own `epoll` loop), `nlohmann-json` (IPC + state), `toml++` (config), `CLI11` (CLI).
One `epoll` event loop on the main thread plus one writer thread that owns the backend.
Tests run under AddressSanitizer + UndefinedBehaviorSanitizer. C for the kernel module. Arch
`PKGBUILD` for both (module as `*-dkms`).

### Repository layout (`~/Documents/dev/mykb`)

```
kernel/hp-omen-kbd/   hp_omen_kbd.c, Makefile, dkms.conf
CMakeLists.txt        C++23 targets: omenkbd (library), omen-kbd (binary), omen-kbd-tests
src/                  core/ (color, zones, palette, effects, rules, compositor — pure, no I/O)
                      backend/ (Backend interface; hp-omen-kbd sysfs; mainline-led; mock)
                      ipc/ daemon/ (model, event loop, writer, daemon) sources/ (hyprland, theme,
                      hwmon, system D-Bus) cli/ util/ main.cpp
tests/                GoogleTest unit + integration tests (mock backend, fake sysfs trees)
packaging/            omen-kbd.service (user), 99-omen-kbd.rules, sysusers.d, modules-load.d,
                      PKGBUILD, example config.toml, waybar snippet, hypridle/autostart snippets
                      (kbdlight.lua comes from `omen-kbd gen-hyprland`)
docs/                 spec (this design), hardware-validation.md, recovery.md
```

### Phases (each ends with something verifiable; hardware steps need the user's explicit go-ahead)

| Phase | Work | Touches hardware? |
|---|---|---|
| P0 | Commit this design as `docs/superpowers/specs/2026-10-21-omen-kbd-design.md` (in a worktree branch of `mykb`); write the implementation plan (superpowers:writing-plans) | No |
| P1 | **Migration off omen-space** (below; user runs commands; reversible; reboot to stock hp-wmi; verify hotkeys + platform_profile). Keyboard keeps its last static colors until P4. | Indirect (driver swap) |
| P2 | Rust core + mock backend + daemon + IPC/CLI + Waybar + theme + rules + mode hints (first task: Lua-hook spike); unit & integration tests | No |
| P3 | Kernel module written, reviewed, built with DKMS against the running kernel's headers; **not loaded** | No |
| P4 | **Hardware validation protocol** (below) | Yes, stepwise |
| P5 | Switch daemon to the real backend, apply dotfiles patches, start from autostart, 1-hour soak + suspend cycles | Yes |
| P6 (optional) | Propose the atomic 4-zone interface upstream (platform-driver-x86) | No |

### P1 — migration off omen-space (user-run, reversible)

1. Keep a fallback (a snapshot or a second kernel boot entry, if available).
2. Set omen-space to **static** with colors you like (`omen-cli rgb set-mode static 50`, then
   `omen-cli rgb set-color <0-3> <RRGGBB>` per zone), and **record the baseline**: `cat /sys/devices/platform/hp-omen-extra/zone{0..3}` and
   `brightness`. These are the colors the keyboard keeps until P4, and P4's reference values.
3. `systemctl disable --now omen-space-daemon`.
4. `sudo dkms remove hp-omen-extra/2.1.2 --all` (DKMS restores the stock `hp-wmi.ko` it backed up),
   `sudo dkms remove omen-rgb-keyboard/1.4 --all`; remove `/etc/modules-load.d/hp-omen-extra.conf`,
   omen-space files (`/usr/libexec/omen-space`, `/etc/omen-space`, its udev rule, D-Bus policy, sysusers
   group), leftover `/usr/src/hp-omen-*` dirs.
5. **Before rebooting** (today's forced `dkms install` means DKMS's backup isn't trusted blindly):
   `sudo depmod -a`; `modinfo -n hp_wmi` must point under `.../kernel/drivers/platform/x86/hp/`, **not**
   `updates/`; `pacman -Qkk linux 2>&1 | grep -i hp-wmi` must be clean. If any check fails:
   `sudo pacman -S linux` to reinstall the stock file, then re-check. Confirm no `hp_wmi` blacklist exists.
6. Reboot; verify the in-tree hp_wmi is loaded, hotkeys work, and `/sys/firmware/acpi/platform_profile`
   still lists profiles. If platform_profile is missing with the stock driver, stop and decide (a
   power-management question, not a keyboard one).
7. **Observe (first clean persistence test):** with nothing writing colors anymore, what does the keyboard
   show before login after this reboot, and after a later full power-off? Also press the Fn backlight key
   with `libinput debug-events` / `wev` running: does stock hp-wmi emit any input event for it? (If yes,
   the daemon can use that instead of polling.)

### P4 — hardware validation protocol (each step = one approval)

1. Load `hp_omen_kbd readonly=1`; read `dmesg` (keyboard type must be 1 or 2; table hex dump) + `zones` +
   `backlight`; compare with the **baseline recorded in P1 after omen-space was stopped in static mode**
   (today's `E19508 2EFD53 1E6AF7 D102AC` was just one frame of the `cycle` effect). Review header bytes
   0–24. **Stop if anything is unexpected.**
2. Reload `readonly=0`; write back the **identical** colors (a no-op); confirm nothing visibly changes and
   readback matches.
3. Light one zone at a time in a distinct color → confirm the physical zone order (fills the zone map).
4. `backlight` 0 then 1: colors must come back unchanged. Press the Fn backlight key and watch
   `backlight`. Check whether writing colors while off turns the backlight on.
5. Suspend/resume twice; colors must survive.
6. Cold boot (full power-off) **test**: after writing a distinctive color with the module, power off fully
   and boot — record what shows before login (firmware persistence: yes/no) and confirm the profile after
   login. Revisit the write budget / opt-in default with this result.
7. Rate test: 5 then 10 fps for 60 s each; record CPU (`pidstat`) and `dmesg`; pick the default fps.
8. Unload → boot colors restored.
Recovery doc covers: writing known colors, `backlight 1`, unloading, reboot, and BIOS defaults.

### Verification (end-to-end, after P5)

- `cargo test` (core, rules, compositor, hint timing, IPC) green; `omen-kbd daemon --backend mock`
  demo of every feature.
- `omen-kbd doctor` clean; `omen-kbd set …` changes all 4 zones in one write (`info` write count +1).
- Colors persist across: reboot, 3× suspend/resume, Fn-key off/on, `systemctl --user restart omen-kbd`.
- Hold Super → mauve after ~250 ms, release → profile; `Super+R` → teal until leaving resize.
- `rice-theme apply <other>` → keyboard follows the theme.
- With effects enabled: unplug charger → effect pauses to static. Power-saver profile → dimmer.
- Kernel guards: writes in `readonly=1` fail with `-EPERM`; consecutive writes are spaced ≥
  `min_interval_ms` apart; a failing WMI call surfaces as a CLI error; hitting `max_writes_per_hour`
  returns `-EBUSY` and the daemon drops to static. Daemon tests: a burst of 100 commands is coalesced to
  the latest frame.
- Idle CPU of the daemon ≈ 0% with a static profile; with a 10 fps effect, measured and recorded.
- Waybar module updates instantly; click/scroll actions work.
