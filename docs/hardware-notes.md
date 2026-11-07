# Hardware notes — OMEN 16-k0xxx

Facts recorded on the real machine, for the hardware validation phase (P4) and later debugging.

## Machine

| Item | Value |
|---|---|
| Model | OMEN by HP Gaming Laptop 16-k0xxx (SKU `7Z4G2PA#ACJ`) |
| DMI board | `8A4E` |
| BIOS | F.21 (09/19/2023) |
| Kernel | 7.2.9-arch1-1 (single kernel, GRUB, ext4 root, no snapshots) |
| Keyboard | PS/2 "AT Translated Set 2", 4-zone RGB via HP BIOS WMI; no USB/HID per-key controller |

## Migration off omen-space (spec phase P1) — 2026-10-21

1. **Baseline frozen** with omen-space in static mode
   (`omen-cli rgb set-color` software zones left/middle/right = `40E0D0`, WASD = `0099ED`):
   hardware read-back via `hp-omen-extra` `zone0..zone3` = **`40E0D0 40E0D0 40E0D0 0099ED`**,
   backlight = **1**. With the default map (hw0 = right, hw1 = middle, hw2 = left, hw3 = WASD), this is
   turquoise on left/middle/right and blue on WASD. **This is P4 step 1's reference value.**
2. omen-space daemon stopped and disabled; GUI/tray closed.
   Measured cost of its `cycle` mode before stopping: **52 min 15 s of CPU over 6.2 h ≈ 14% of one core**
   on average; CPU time stopped growing as soon as the mode was static (bug D1 confirmed).
3. `dkms remove hp-omen-extra/2.1.2 --all` and `dkms remove omen-rgb-keyboard/1.4 --all`; DKMS restored
   the archived stock module.
4. All omen-space files, `/usr/src` leftovers (`hp-omen-extra-2.0.7/2.1.2`, `hp-omen-wmi-0.6.2/0.9/0.10`,
   `omen-rgb-keyboard-1.4`), `/etc/modules-load.d/hp-omen-extra.conf` and group `omen-hw` removed.
5. Pre-reboot checks passed: `dkms status` empty; no `updates/dkms`; `modinfo -n hp_wmi` →
   `kernel/drivers/platform/x86/hp/hp-wmi.ko.zst`, srcversion **`77ED9FA76598B1B3063E5E0`** (stock);
   `pacman -Qkk linux` → 7749 files, 0 altered; no hp_wmi blacklist or omen autoload; initramfs contains no
   hp-wmi/omen module.

## Post-reboot observations (P1 steps 6–7, boot at 2026-10-21 21:38)

- [x] Loaded `hp_wmi` srcversion is `77ED9FA76598B1B3063E5E0` (stock); `lsmod` shows only `hp_wmi`,
      `hp_bioscfg`, `wmi` — no omen modules; `/sys/devices/platform/hp-omen-extra` gone.
- [x] `platform_profile` = `performance`, choices still `cool quiet balanced performance`; kernel log has
      only `hp_wmi: Registered as platform profile handler`; hotkeys and power-profile switching work
      (user-confirmed).
- [x] Colors shown **before login** after this warm reboot, with nothing writing colors: **the last colors**
      (turquoise L/M/R, blue WASD). The firmware retains colors across a warm reboot; the EC may stay
      powered during a warm reboot, so this does not yet prove non-volatile storage.
- [~] Colors shown before login after a **full power-off**: after a *total power loss* (P4 step 6,
      dead battery) the firmware comes up with `0F84FA 710FFA F9350F FAAC0F`. A normal power-off with a
      healthy battery is still untested.
- [ ] Fn backlight key: does stock hp-wmi emit any input event (`libinput debug-events` / `wev`)? Not yet
      tested → the daemon uses the designed fallback (probe `backlight` every 3 s only while an effect is
      wanted).

## P4 results

The runbook is `docs/hardware-validation.md`. Fill this in as its steps pass. The machine was on kernel
7.2.14-arch1-1 when the module was last tested in the VM (2026-11-02).

- [x] Step 0, firmware code (2026-11-06, BIOS F.21, `acpidump` + `iasl -d`): **passed, nothing to change
      in the module.** `WMAA` is in the DSDT only. `ssdt4` and `ssdt8` did not decompile (duplicate
      names), but neither contains `WMAA`, `HWMC` or any lighting field.
  - **Reply sizes:** `WMAA` → `HWMC` always returns 8 + N bytes, with N set by the method id alone: 0, 4,
    128, 1024, 4096 for methods 1–5. A handler whose payload is larger than N gets return code 5.
    The module asks for method 2 (12 bytes) for the keyboard type and both backlight calls, and method 3
    (136 bytes) for both color table calls. Every reply is at least as long as the module expects.
  - **Serialized:** yes, and it also holds the `MUTZ` mutex. Nothing else calls `HWMC`.
  - **`0x20008`/`0x2B`** reaches `GM2B`, plain ACPI code with no SMI. It reads the EC byte `KBT0`:
    - 0 → type 0;
    - 1 → type 3 (per-key; the module refuses it);
    - 2 → type 2 or 1, chosen by a GPIO (numpad).
    So step 1 loads only if the EC reports 2.
  - **Color GET (`0x20009`/2)** builds the table on the fly: byte 0 = `03`, bytes 25–36 = the 12 color
    bytes kept in BIOS memory (`LRGB`), every other byte 0. The header can never drift, and the table can
    never be all `00` or all `ff`.
  - **Color SET (`0x20009`/3)** reads only bytes 25–36 of the input. It stores them in two BIOS memory
    copies (`LRGB`, `BRGB`) and sets a "lighting changed" flag (`LCMC`). All of these sit in a
    `SystemMemory` region, and no ACPI code reads the flag or `BRGB`: something outside ACPI (SMM or the
    EC) applies the colors later. So a success code means accepted, not lit. The reply has no data; the
    module discards its 128 zero bytes.
  - **Backlight GET (`0x20009`/4)** returns `LBRT`, a byte in BIOS memory. **Backlight SET
    (`0x20009`/5)** stores the input byte in `LBRT` unchecked and sets the same flag. The module only
    ever sends `0xE4`/`0x64`. Whether the Fn key updates `LBRT` is unknown until step 4.
  - **Consequences for the runbook:**
    - A reload's table `diff` proves what BIOS memory holds, not what the LEDs show. Only the eye proves
      a write was applied.
    - Step 7's `time` cannot see the cost, because that lives in the code that consumes the flag. The
      SMI counter (MSR `0x34`) can.
    - Whether `BRGB` reaches flash is what step 6 answers.
- [x] Step 1, load read-only (2026-11-06, kernel 7.2.14-arch1-1): **passed.**
  - **The gate and the keyboard-type query work:** `kbd_type=1`, `last_error=0`, no new kernel errors.
  - **`backlight_raw = 0x80`, not `0xE4`:** bit 7 is set and the keyboard is lit. This keyboard has only
    on and off (no brightness levels).
  - **Table exactly as step 0 predicted:** byte 0 `03`, bytes 25–36 the colors, the rest `00`.
  - **Zones `0F84FA 710FFA F9350F FAAC0F`, not the P1 baseline,** but they match the keyboard
    (user-confirmed), so the later steps use these.
  - **Where those colors came from is unknown.** Since P1 the journal shows many full power-offs and
    several unlogged ends (crash or hard power-off), and Windows is installed (OMEN Gaming Hub could
    have set them). Step 6 decides whether a power-off resets the colors.
  - No "taints kernel" lines: the NVIDIA DKMS modules had already tainted the kernel (`OE`).
- [x] Step 2, same colors written back (2026-11-06): **passed.**
  - The first SET was accepted: `writes_total=1`, `last_error=0`, `header_changed=0`. The keyboard did
    not change.
  - After a reload, the table equals step 1's byte for byte.
  - Both `insmod` lines arrived split when pasted, so `restore_on_unload=0` never reached the module.
    The first unload therefore wrote the boot colors back (`restored the boot colors`), which were the
    same colors. That makes two accepted SETs. It also ran the unload restore once on real firmware,
    ahead of step 8.
  - The runbook now uses short `insmod $K` lines and prints `restore_on_unload` after each such load.
- [x] Step 3, physical zones (2026-11-06): **passed.**
  - The firmware applies a SET, and red, green and blue are in that order.
  - Each write lit one zone in the named color, matching the default map (user-confirmed): hardware
    0 = right, 1 = middle, 2 = left, 3 = WASD. The zone map in `config.toml` needs no change.
  - After the colors were put back and the module reloaded, the table equals step 1's.
  - Both loads printed `restore_on_unload` = `N`.
- Between steps 3 and 4 (2026-11-06 21:58), `dmesg -W` showed `ACPI Error`s in `EC0._Q33`. **Not
  ours:**
  - `_Q33` is the EC asking for the RTC time. It reads CMOS region `CMS0`, which has no Linux handler.
  - The journal has it every 30 minutes (e.g. 05:35, 06:05, … on 2026-11-03), and on boots before the
    module existed (14 times on 2026-10-21).
  - This one came 70 s after the last module call, a read.
  - The runbook now lists it as the one known exception.
- [x] Step 4, backlight (2026-11-06): **passed.**
  - **The backlight SET works on this BIOS.** `echo 0` turned the keyboard dark (`backlight_raw=0x64`).
    `echo 1` brought it back with the same colors at the same brightness (`0xe4`). Both had
    `last_error=0`. All user-confirmed.
  - **The Fn key (on/off only) writes the same byte the module reads:** off gives `0x64`, on gives `0xe4`.
    So the bit-7 decode is right both ways, and `backlight` follows the Fn key. P5's fallback (read
    `backlight` every 3 s while an effect runs) can see Fn-key changes.
  - `0x80`, seen at step 1, is the BIOS's value at boot before anything wrote the byte. It lights the
    keyboard the same as `0xe4`.
  - **Writing colors while the light is off leaves it off** (user-confirmed; `backlight` still read
    `0`). The daemon can repaint while off without turning the light on.
  - After a reload, the table equals step 1's.
- [x] Step 5, suspend (2026-11-06): **passed.**
  - **Final run, from a TTY (5a):** three wakes. `writes_total` went 2 → 4 → 6 → 8, two SETs (zones and
    backlight) per wake, with `last_error=0` throughout. The kernel log was clean apart from the known
    `_Q33` group, which came exactly 1,800 s after the previous one. No NVIDIA error this time.
  - Earlier notes from the first attempt follow.
  - **Colors survived both wakes**, user-confirmed: one with the module's re-apply (5a), and one with
    the module loaded `readonly=1` and nothing to re-apply (5b, `writes_total=0`). So the firmware
    keeps colors across `s2idle` on its own.
  - The journal has only two suspends, not 5a's two plus 5b's one. No `re-applying colors after resume
    failed` was logged, but 5a's `writes_total` reads after the wake were lost, so the re-apply is not
    yet shown to have run.
  - **Hyprland hung after both wakes.** Its log stops at its own display rescan of the Intel GPU
    (`card2`, eDP-1), and the user killed it from a TTY.
  - **Evidence the module is not the cause:**
    - In 5b the module made no firmware call on resume (`hok_resume` sees nothing written and returns),
      yet Hyprland hung the same way.
    - The first fault was `NVRM: … Out of memory … _memdescAllocInternal` at 5a's suspend entry, before
      any resume code. NVIDIA saves video memory on suspend (`PreserveVideoMemoryAllocations=1`,
      `/var/tmp`). The module has no suspend callback.
    - No ACPI error near either wake.
  - **Against:** two `systemctl suspend`s on 2026-11-03, without the module, came back fine with no
    NVRM message.
  - **Settled by the user:** Hyprland hanging after a wake happened before the module existed. It is a
    known problem of this machine, not the module's; no control suspend was run. To keep a hang from
    taking the terminal down, the rest of step 5 runs from a TTY.
- [x] Step 6, cold boot (2026-11-06): **done, as a total power loss.**
  - **What happened:** the module wrote `FF00FF 00FFFF FF00FF 00FFFF`, then `poweroff`, then the
    charger was unplugged for 30 s. The battery turned out to be dead (see below), so this cut all
    power.
  - **Evidence:** the next boot showed a CMOS error, and the kernel read the RTC as 2022-01-01.
    Secure Boot stayed off, the boot order was unchanged, and the clock re-synced over NTP.
  - **Result: the colors did not survive.** The keyboard changed (user-confirmed). After login the
    firmware read `0F84FA 710FFA F9350F FAAC0F`, `backlight_raw=0x80`, and a table identical to step 1's.
  - **So `0F84FA 710FFA F9350F FAAC0F` with `0x80` is the firmware's state after a power loss.** That
    is where step 1's colors came from: the journal's unlogged boot ends since 2026-10-31 match charger
    unplugs with a dead battery.
  - **Still open:** a normal power-off with a healthy battery, with the EC kept powered. A total power
    loss also cannot tell "kept only in powered memory" from "stored, but reset when the BIOS loaded
    defaults after the CMOS error". Nothing so far shows colors being written to flash. Repeat this
    step after the battery is replaced.
  - **Battery (not P4's doing; found here):** BAT0 reads 2.74 V against an 11.58 V design minimum, and
    "Not charging". It holds 42 of 83 Wh after 132 cycles. UPower already had it at 3.14 V and
    "pending-charge" on 2026-10-31. Until it is replaced, unplugging the charger is a hard power cut.
- [x] Step 7, rate test (2026-11-06, `~/omen-kbd-p4/step7.sh`, log `step7.log`): **passed.**

  | Phase | Writes | `real` | `user` | `sys` | SMIs |
  |---|---|---|---|---|---|
  | Idle, 60 s | 0 | – | – | – | 0 |
  | 5/s | 300 in 61.1 s (4.9/s) | 61.0 s | 0.10 s | 0.90 s | 0 |
  | 10/s | 600 in 62.2 s (9.6/s) | 62.2 s | 0.24 s | 1.82 s | 0 |

  - `last_error=0` throughout. After the colors were put back and the module reloaded, the table was
    unchanged. No kernel errors at all during the run.
  - **CPU cost:** about 3 ms of `sys` per write (one GET and one SET through the ACPI interpreter). That
    is an upper bound: it includes forking `sleep` once per write. At 10 writes/s that is about 3% of one
    core, at 5/s about 1.5%.
  - **The SMI counter (MSR `0x34`) did not move in any phase, idle included.** So either writes cause no
    SMI, or this counter does not count them. Either way, the cost visible to the OS is the ACPI time
    above.
  - The flicker was smooth at both rates, and the machine stayed responsive (user-confirmed).
  - **Default frame rate picked: 10 fps**, which is the daemon's existing default (`src/config.hpp`).
- [x] Step 8, unload (2026-11-06, `~/omen-kbd-p4/step8.sh`, log `step8.log`): **passed.**
  - With `restore_on_unload=1`, the module loaded with `0F84FA 710FFA F9350F FAAC0F`, wrote
    `FF0000 00FF00 0000FF FFFFFF` and was unloaded.
  - The unload logged `restored the boot colors` (the last module line), the sysfs directory was gone,
    and there were no kernel errors.
  - The keyboard showed red/green/blue/white, then the boot colors again (user-confirmed).
  - Afterwards `~/omen-kbd-p4/set-colors.sh` put the P1 baseline `40E0D0 40E0D0 40E0D0 0099ED` back.

### P4 sign-off (2026-11-06)

All eight steps passed. The module's `readonly` default is now 0, and P5 may install the package.

**Unchanged, by the user's decision:**
- **Write budget** (40000/hour) and **effects opt-in.** They stay until a normal power-off with a
  healthy battery shows whether colors persist (step 6 is still open).
- **10 fps** default.

**Known issues found during P4, none of them the module's:**
- **Dead battery:** charger unplugs are hard power cuts and reset CMOS.
- **Hyprland hangs after a wake.**
- **`EC0._Q33` ACPI errors** every 30 minutes.

**For P5:**
- The Fn key updates `backlight` (`0x64`/`0xe4`), so polling it every 3 s while an effect runs sees
  Fn-key changes.
- Color writes while the light is off leave it off.
- After a total power loss the firmware starts with `0F84FA 710FFA F9350F FAAC0F` and `0x80`.

## P5 results

Runbook: `docs/p5-install.md`. Kernel 7.2.14-arch1-1, BIOS F.21.

- **Step 1, package (2026-11-07).**
  - `hp-omen-kbd-dkms` 0.1.0-1 was built with `makepkg` and installed with `pacman -U`.
  - The package holds only the module sources, the udev rule, sysusers and modules-load files.
  - DKMS built it into `updates/dkms/`, and its `srcversion` `3F33C2E8F892E41D55D8E72` equals the
    module P4 tested.
  - `hp_wmi` is still the stock in-tree module.
  - sysusers created group `omen-kbd` (gid 949), and the user joined it.
  - The module has no modalias, so nothing loaded it during the install.
  - The battery read 2.87 V.
- **Between steps 1 and 2: another total power loss.**
  - The machine lost power while it was shut down. At the next boot the RTC read
    `2022-01-01 00:00:25`, so CMOS had reset.
  - The firmware came up with `0F84FA 710FFA F9350F FAAC0F` / `0x80` again, the same state as P4
    step 6.
  - The module was not loaded at the time, so a total power loss resets the colors whoever set them.
- **Step 2, modprobe.**
  - Right after `modprobe`, `zones` and `backlight` were `-rw-rw-r-- root omen-kbd`, the first proof
    that udev's `RUN` line works.
  - The load wrote nothing: `kbd_type=1`, `readonly=0`, `writes_total=0`, `last_error=0`.
- **Step 3, reboot (warm, `systemctl reboot`).**
  - The RTC kept its time.
  - `systemd-modules-load` loaded the module at boot, logging nothing, and the module's load lines
    showed no error.
  - udev set the permissions again at boot.
  - The user manager's `Groups:` included 949.
  - Then, as the user without sudo, one write of the P1 baseline `40E0D0 40E0D0 40E0D0 0099ED`
    succeeded: `writes_total` 0 → 1, `last_error=0`, and the keyboard changed (user-confirmed). Group
    `omen-kbd` can write the keyboard.
- **Step 4, daemon and config.**
  - `omen-kbd` was installed from `p5-install` (with the I5 and I3 fixes), along with the user unit.
  - The config is the example with a first profile `baseline` = `40E0D0 40E0D0 40E0D0 0099ED`.
  - `doctor` gave OK for the config, the backend (`readonly=0`), the permissions, competing writers,
    hp-wmi DKMS and the theme palette. It warned only that the daemon was not running and the hooks
    were missing.
- **Step 5, first daemon start.**
  - The daemon is active, and its `Groups:` include 949.
  - Its first frame wrote the backlight and the zones: `writes_total` 1 → 3, `last_error=0`.
  - `backlight_raw` went `0x80` → `0xe4`. That is the firmware's post-power-loss "on" replaced by the
    standard "on"; the light did not change.
  - The keyboard looked the same (user-confirmed).
  - `status` showed `baseline`, `hp-omen-kbd`, on. No journal warnings and no kernel errors.
- **Step 6, CLI and Fn key.**
  - Every command did what it should with one write each and `last_error=0`: `set`, `profile`,
    `brightness 30`/`100`, `off`/`on` (`0x64`/`0xe4`), `profile baseline`, and `effect wave`
    refused with exit code 1. All user-confirmed.
  - **Fn key, first run: one toggle went the wrong way.** After an Fn off, `status` correctly said
    off. Then one `toggle` recorded "off" and wrote `0x64` instead of turning the light on, and the
    restart that followed started the daemon "off".
  - **Two diagnosis runs did not reproduce it** (`~/omen-kbd-p5/step6-fn*.log`):
    - one with `on` just before the Fn press;
    - one replaying step 6's exact order (`on`, `profile white`, `profile baseline`, the refused
      `effect wave`, then Fn and `status` at once).

    In both, the live firmware reads were `0` while the light was dark and `1` while it was lit,
    matching the user's eyes. `status` said off, and one `toggle` turned the light on and recorded on.
  - Open: the first run's toggle is unexplained. A daemon test replays its command sequence against
    the mock and passes.
  - The daemon restart kept its saved state.
- **Another total power loss between the diagnosis runs.**
  - At the next boot the RTC read `2022-01-01 00:00:29`.
  - The module loaded and read the firmware defaults `0F84FA 710FFA F9350F FAAC0F` with backlight
    `0x00`.
  - The daemon was not autostarted yet (step 8), so the user started it by hand. Its first frame put
    `baseline` back from `state.json`.
- **Step 7, three wakes (s2idle, from a TTY, daemon running, no lock hook yet).**
  - Three suspends, 16:49–16:50, about 4 s each.
  - After every wake the keyboard showed `baseline` (user-confirmed) and the daemon stayed active.
  - `writes_total` went 4 → 8 → 12 → 16: +4 per wake, from the module's re-apply (zones and
    backlight) and the daemon's forced rewrite (zones and backlight).
  - `last_error=0` and no kernel errors.
  - After the first wake the counter still read 4 at the 6-second mark. All four writes had landed
    before the next suspend.
- **A normal power-off keeps the colors (answers part of P4 step 6).**
  - At 16:52:30, `systemctl poweroff` with the charger connected.
  - The machine stayed off for 31 minutes and booted at 17:23 with the RTC intact.
  - At load the module read `40E0D0 40E0D0 40E0D0 0099ED` and backlight `0xe4` from the firmware,
    i.e. what the daemon had written.
  - With the three total power losses (P4 step 6, and twice during P5), all of which reset the colors
    to the firmware's own `0F84FA 710FFA F9350F FAAC0F`: the colors live in memory kept alive by
    standby power, not in non-volatile storage that survives without it.
- **Step 8, desktop hooks. Each was a dotfiles change, made one at a time and confirmed by the user.**
  1. **Autostart** (`autostart.lua:25`): after a logout and login, Hyprland started at 17:34:47 and
     the daemon at 17:34:48.
  2. **Mode colors** (`kbdlight.lua` from `gen-hyprland`, required from `hyprland.lua:15`):
     - Holding Super gave mauve, Super+Shift blue, Ctrl peach.
     - Chords (Super+Q) and Shift while typing painted nothing.
     - Super+R gave `submap:resize` = `#26FFDB`. The vivid teal is too close to the baseline turquoise
       to see, so the status stream confirmed it. A held Super inside resize showed mauve on top of it.
     - Every hint released back to `baseline`, and nothing stuck.
  3. **Lock and idle** (`hypridle.conf`: `on_lock_cmd`/`on_unlock_cmd`, plus the 150 s listener
     calling `omen-kbd idle`): dark while locked, dimmed when idle, `baseline` again afterwards.
  4. **Waybar** (`custom/kbd`): the pill shows the profile. Click, right-click, scroll and the hint
     style all worked.
  5. **Theme** (`rice-theme` `reload_apps()`):
     - With the theme-colored `default` profile, mocha `8926FF 2679FF 26FFDB FF7926` became latte
       `8426FF 1F6AFF 26F3FF FF640B` and back.
     - The daemon followed, but the user saw no change: vivid boosting makes two flavors with the same
       hues look nearly alike on the LEDs.
  6. **Game mode** (`modes.lua`, in `rice.gamemode.toggle()`): SUPER+F1 switched to `game`
     (`202020 202020 202020 FF0000`, rule `gamemode`), and SUPER+F1 again brought `baseline` back.

  Afterwards `doctor`: all 10 checks OK. `last_error=0` throughout.
- **Step 9, reboot with everything in place.**
  - `systemctl reboot` at 18:20, with the RTC intact.
  - Before login the keyboard showed `baseline`: the firmware kept the colors through the warm
    reboot, and the module read them at load (backlight `0xe4`).
  - The daemon autostarted at 18:21:10, the same second as Hyprland. hypridle and Waybar were running.
  - `doctor` all OK, and no kernel errors.
  - Soak start at 18:21:40: daemon CPU 23 ms, RSS 7.7 MB, module `writes_total=4`.
