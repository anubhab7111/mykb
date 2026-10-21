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
- [ ] Colors shown before login after a **full power-off**: not yet tested (do it before P4, or as P4 step 6).
- [ ] Fn backlight key: does stock hp-wmi emit any input event (`libinput debug-events` / `wev`)? Not yet
      tested → the daemon uses the designed fallback (probe `backlight` every 3 s only while an effect is
      wanted).
