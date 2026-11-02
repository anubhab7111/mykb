# Recovery

What to do if the keyboard or the module misbehaves. Use these in order, from the mildest. They all
apply to real hardware, so only from P4 on: before P4 the module is never loaded on this machine.

The module never replaces or changes `hp-wmi`. Fans, hotkeys and power profiles do not depend on it.

If the daemon is running (P5 on), stop it first, or it will write over what you do here:

```bash
systemctl --user stop omen-kbd.service
```

## 1. Write known colors

The P1 baseline, in hardware order:

```bash
D=/sys/devices/platform/hp-omen-kbd
echo "40E0D0 40E0D0 40E0D0 0099ED" | sudo tee $D/zones
```

A module loaded with `readonly=1` answers `Operation not permitted`. Reload it with `readonly=0`, or go
to step 3.

## 2. Turn the backlight on

```bash
D=/sys/devices/platform/hp-omen-kbd
echo 1 | sudo tee $D/backlight
```

The Fn backlight key does the same without the module. If the keyboard is on but dark, the zones are
black: go back to step 1.

## 3. Unload the module

```bash
sudo rmmod hp_omen_kbd
sudo dmesg | grep hp_omen_kbd | tail -n 3
```

If colors were written since it loaded, this puts back the colors from that load, and `dmesg` says
`restored the boot colors`. After this nothing can write the colors any more. A module loaded with
`restore_on_unload=0` leaves the colors as they are.

## 4. Reboot

A reboot clears everything the module did, except colors the firmware kept.

Before P5 the module does not come back by itself: it was loaded by hand with `insmod`. From P5 on,
`modules-load.d` loads it at every boot. On any mismatch it returns `ENODEV`, so it cannot block a
boot.

If the machine hangs, hold the power button for 10 seconds, then start it again.

## 5. Keep an installed module from loading

For one boot: in GRUB, press `e` on the boot entry, add this to the end of the line starting with
`linux`, and press Ctrl+X:

```
modprobe.blacklist=hp_omen_kbd
```

To remove it for good:

```bash
sudo pacman -R hp-omen-kbd-dkms
```

## 6. Reset the embedded controller

This changes no settings, so try it before the BIOS defaults. Shut the machine down, unplug the
charger, hold the power button for at least 15 seconds, plug the charger back in and start it.

## 7. BIOS defaults

If the firmware itself is stuck on wrong colors or a dark keyboard after all of the above, load the
BIOS defaults: press F10 at power-on to enter the BIOS setup, load the setup defaults (usually F9),
then save and exit.

**This can stop the machine booting.** It changes every BIOS setting, not only the keyboard. Secure
Boot is off on this machine, and this kernel under GRUB is not set up to pass it. HP's defaults may turn
it on (not checked on BIOS F.21). Before you save, look at these two in the same setup screens:

- **Secure Boot** must be disabled;
- **boot order** must still start with GRUB.

If it no longer boots afterwards, go back into the BIOS setup and fix those two. Note any other setting
you changed before you load the defaults.

## What the errors mean

| Error from a write | errno | Meaning | What to do |
|---|---|---|---|
| `Operation not permitted` | `EPERM` | The module is loaded with `readonly=1`. | Reload with `readonly=0`. |
| `Invalid argument` | `EINVAL` | The text is not 4 colors or 1 (`zones`), or not `0` or `1` (`backlight`). | Fix the text. |
| `Device or resource busy` | `EBUSY` | The hourly write budget is used up (`info` has `budget_exhausted=1`). | Find what is writing so much. The budget frees up as the hour rolls on; a reload starts it over. |
| `Input/output error` | `EIO` | The firmware refused or gave a bad reply (`info` has `last_error`). | Check `dmesg`, then step 3. |
| `Operation not supported` | `EOPNOTSUPP` | The firmware does not know the call. | Stop and report; it needs a code change. |
| `Permission denied` | `EACCES` | Your user may not write: not in group `omen-kbd`, or the udev rule is not installed. | Use `sudo tee`, or install the package (P5) and log in again. |
| `No such file or directory` | `ENOENT` | The module is not loaded. | Load it. |
