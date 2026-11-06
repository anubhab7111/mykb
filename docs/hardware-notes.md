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
- [ ] Step 1, load read-only: `kbd_type` = , `backlight_raw` = , zones match the baseline: ; table:
- [ ] Step 2, same colors written back: table unchanged after a reload:
- [ ] Step 3, physical zones: hardware 0 = , 1 = , 2 = , 3 = ; table unchanged after a reload:
- [ ] Step 4, backlight: `backlight_raw` off/on: ; at each Fn level: ; writing colors while off turns it
      on: ; table unchanged after a reload:
- [ ] Step 5, suspend twice: ; colors survive with nothing to re-apply:
- [ ] Step 6, cold boot: shown before login: ; `zones` after login:
- [ ] Step 7, rate test: 5 writes/s: ; 10 writes/s: ; table unchanged after a reload: ; default frame rate
      picked:
- [ ] Step 8, unload: boot colors restored:
