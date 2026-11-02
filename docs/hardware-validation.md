# Hardware validation (phase P4)

This is the first time `hp_omen_kbd` meets the real firmware. Until now it has only run in a VM
against an emulator, which checks the module's logic and how it behaves in the kernel, not whether
this BIOS answers the way the module expects.

**Every command in this document is P4, on hardware, with your go-ahead.**

- Do one step at a time, and only after you have agreed to it.
- You run everything with `sudo` in your own terminal.
- If a stop condition fires, stop. Do not go on to the next step. `docs/recovery.md` gets you back.
- Record results under "P4 results" in `docs/hardware-notes.md`.

Two firmware calls have never run on this board. Their shape comes from the upstream patch series:

| Call | Where it is first proven | If it fails |
|---|---|---|
| Keyboard type (`0x20008`/`0x2B`) | Step 1 | The module refuses to load. |
| Backlight set (`0x20009`/`0x05`) | Step 4 | The write returns an error. |

The color table calls and the backlight read did run here, through `hp-omen-extra`, before P1.

## Before you start

Run these from the repository root, on the kernel you will test. After a kernel update, run them
again: `insmod` refuses a module built for another kernel.

```bash
make -C kernel/hp-omen-kbd check vmtest   # must be green; leaves the module built
D=/sys/devices/platform/hp-omen-kbd       # the steps below use this
```

Nothing else may write the colors. Run `omen-kbd doctor` and look at "competing writers". Do not
start the daemon: that is P5. Until P5 the attributes are root-only for writing, so writes go through
`sudo tee`. Reading needs no `sudo`.

## Step 1: load read-only

**Proves:** the load gate passes on the real board, the keyboard-type query works, and the module
reads the same colors the old driver read. Nothing is written.

```bash
sudo insmod kernel/hp-omen-kbd/hp_omen_kbd.ko readonly=1
sudo dmesg | grep hp_omen_kbd
cat $D/zones $D/backlight $D/info
V=$(cat $D/zones)                         # steps 2 to 4 write this back
```

**Expected:**

- `insmod` prints nothing. The first two `dmesg` lines below appear once per boot at most, and are
  normal for a module you built yourself:
  ```
  hp_omen_kbd: loading out-of-tree module taints kernel.
  hp_omen_kbd: module verification failed: signature and/or required key missing - tainting kernel
  hp_omen_kbd: HP 8A4E keyboard type <1 or 2>, backlight 0x<xx>, zones 40E0D0 40E0D0 40E0D0 0099ED, readonly=1
  hp_omen_kbd: table 00000000: <16 bytes>
  ... 8 table lines in all ...
  ```
- `zones` reads `40E0D0 40E0D0 40E0D0 0099ED` and `backlight` reads `1`: the P1 baseline.
- `info` shows `board=8A4E`, `kbd_type` 1 or 2, `readonly=1`, `writes_total=0`, `last_error=0`.
- The keyboard does not change.

**Record:** `kbd_type`, `backlight_raw`, the 8 table lines (bytes 0-24 are the header nobody
documents), and whether the zones match the baseline.

**Stop if:**

- `insmod` fails with `No such device`. One of the gates refused, and `dmesg` says which:
  - `board not on the allowlist`;
  - `HP BIOS WMI interface ... not found`;
  - `keyboard type query failed (<errno>); not binding`, which means the unproven call does not work
    on this BIOS;
  - `keyboard type <n> is not a four-zone keyboard; not binding`.

  No parameter works around these. The fix is a reviewed code change.
- `insmod` fails with `Input/output error` or `Operation not supported`: a read the old driver did
  fine fails now.
- `zones` differs from the baseline **and** from what you see on the keyboard. If it differs from the
  baseline but matches the keyboard, the firmware changed the colors at some boot; record that and go
  on, using the new value as `V`.
- The table is all zeros or all `ff`.
- `dmesg` shows `BUG:`, `Oops`, `WARNING:` or `ACPI Error`.

## Step 2: write back the same colors

**Proves:** a SET reaches the firmware and leaves everything else alone. It is a no-op on purpose.

```bash
sudo rmmod hp_omen_kbd
sudo insmod kernel/hp-omen-kbd/hp_omen_kbd.ko readonly=0
echo "$V" | sudo tee $D/zones
cat $D/zones $D/info
sudo dmesg | grep hp_omen_kbd | tail -n 12
```

**Expected:** the readback equals `$V`, `info` has `writes_total=1`, `header_changed=0` and
`last_error=0`, and nothing changes on the keyboard.

**Record:** the header bytes again. They should be the same as in step 1.

**Stop if:** the keyboard visibly changes, `tee` reports an error, `header_changed=1`, or the table
lines changed between the two loads.

## Step 3: find the physical zone order

**Proves:** which hardware zone is where, and that red, green and blue are in that order. The default
zone map expects hardware 0 = right, 1 = middle, 2 = left, 3 = WASD.

```bash
echo "FF0000 000000 000000 000000" | sudo tee $D/zones   # hardware 0, red
echo "000000 00FF00 000000 000000" | sudo tee $D/zones   # hardware 1, green
echo "000000 000000 0000FF 000000" | sudo tee $D/zones   # hardware 2, blue
echo "000000 000000 000000 FFFFFF" | sudo tee $D/zones   # hardware 3, white
echo "$V" | sudo tee $D/zones
```

Run them one at a time, and look at the keyboard after each.

**Expected:** exactly one zone lit each time, in the color the comment names.

**Record:** the physical zone for each hardware number. If it differs from the default, the fix is the
zone map in `config.toml`, not a stop.

**Stop if:** a color is wrong (say red shows as green), more than one zone lights, or keys outside
the four zones change.

## Step 4: backlight on and off

**Proves:** the unproven backlight SET works, "off" keeps the colors, and bit 7 of the reply really
means on. A read of `backlight` asks the firmware only if the last answer is over 200 ms old, so the
`sleep` makes these reads show what the firmware reports, not what the module just wrote.

```bash
echo 0 | sudo tee $D/backlight            # the keyboard goes dark
sleep 1; cat $D/backlight $D/zones $D/info
echo 1 | sudo tee $D/backlight            # the same colors come back
sleep 1; cat $D/backlight $D/zones $D/info
```

**Expected:** after `0`, `backlight` reads `0`, `backlight_raw` has bit 7 clear (`0x64` if the firmware
reports back what it was sent), and `zones` is unchanged. After `1`, `backlight` reads `1`,
`backlight_raw` has bit 7 set (`0xe4`), and the keyboard shows the same colors as before.

**Record:** both `backlight_raw` values.

Then press the Fn backlight key through all its levels. After each press, wait a second and run:

```bash
cat $D/backlight $D/info
```

**Record:** `backlight_raw` at each level, and whether `backlight` matched what you saw.

Last, check whether writing colors while the backlight is off turns it on:

```bash
echo 0 | sudo tee $D/backlight
echo "$V" | sudo tee $D/zones             # look at the keyboard: on or off?
cat $D/backlight
echo 1 | sudo tee $D/backlight
```

**Record:** on or off. Either answer is fine, but the daemon has to be told about it.

**Stop if:**

- `echo 0` reports `Input/output error` or `Operation not supported`. The SET's shape is wrong for
  this BIOS. The Fn key still works; the fix is a reviewed code change.
- The colors differ after turning the backlight back on.
- `backlight` disagrees with the keyboard at any Fn level: the bit-7 decode is wrong.

## Step 5: suspend and resume

**Proves:** the colors survive sleep, and the module's re-apply works on real firmware.

```bash
cat $D/info                               # note writes_total
systemctl suspend                         # wake it up, then wait 2 s
cat $D/zones $D/backlight $D/info
sudo dmesg | grep hp_omen_kbd | tail -n 3
```

Do it twice.

**Expected:** the same colors after each wake, `last_error=0`, and no `re-applying colors after resume
failed` line. `writes_total` grows by 2 per resume: zones and backlight, both written in step 4.

With the daemon running (P5), each resume gives two zone writes: the module's re-apply and the
daemon's forced rewrite. That is expected, not a bug.

To see what the firmware does on its own, do it once more with nothing to re-apply:

```bash
sudo rmmod hp_omen_kbd                    # restores the colors of the last load
sudo insmod kernel/hp-omen-kbd/hp_omen_kbd.ko readonly=1
systemctl suspend                         # wake it up, then look at the keyboard
```

**Record:** whether the colors survived this last one with no help (`info` still has
`writes_total=0`).

**Stop if:** the keyboard stays dark or wrong after a wake with the module's re-apply, `last_error` is
not 0, or `dmesg` shows `BUG:`, `Oops`, `WARNING:` or `ACPI Error`.

## Step 6: cold boot

**Proves:** whether the firmware keeps colors with the power off. That decides how careful the write
budget and the opt-in for effects must stay.

```bash
sudo rmmod hp_omen_kbd                    # skip if it is not loaded
sudo insmod kernel/hp-omen-kbd/hp_omen_kbd.ko readonly=0 restore_on_unload=0
echo "FF00FF 00FFFF FF00FF 00FFFF" | sudo tee $D/zones
sudo poweroff
```

Unplug the charger, wait 30 seconds, plug it back in and start the machine. Look at the keyboard
**before logging in**. After login, read what the firmware has now:

```bash
sudo insmod kernel/hp-omen-kbd/hp_omen_kbd.ko readonly=1
cat $D/zones $D/backlight
```

**Record:**

- what the keyboard showed before login, and what `zones` reads now;
- if the colors were kept, the firmware stores them across a power-off, so revisit the write budget
  and the effects opt-in with that in mind;
- this also closes the open "full power-off" item in `docs/hardware-notes.md`.

Confirming the profile after login needs the daemon, so that part moves to P5.

**Stop if:** the machine does not start normally. Then see `docs/recovery.md`.

## Step 7: rate test

**Proves:** what repeated writes cost, so the default effect rate can be picked. Budget used: at most
900 of the 40000 writes per hour.

```bash
sudo rmmod hp_omen_kbd
sudo insmod kernel/hp-omen-kbd/hp_omen_kbd.ko readonly=0
cat $D/info                               # note writes_total

# 5 writes per second for 60 s
time sudo bash -c 'for i in $(seq 150); do
    echo "40E0D0 40E0D0 40E0D0 0099ED" >/sys/devices/platform/hp-omen-kbd/zones; sleep 0.2
    echo "0099ED 0099ED 0099ED 40E0D0" >/sys/devices/platform/hp-omen-kbd/zones; sleep 0.2
done'
cat $D/info
sudo dmesg | grep -iE 'hp_omen_kbd|acpi' | tail -n 5

# 10 writes per second for 60 s
time sudo bash -c 'for i in $(seq 300); do
    echo "40E0D0 40E0D0 40E0D0 0099ED" >/sys/devices/platform/hp-omen-kbd/zones; sleep 0.1
    echo "0099ED 0099ED 0099ED 40E0D0" >/sys/devices/platform/hp-omen-kbd/zones; sleep 0.1
done'
cat $D/info
sudo dmesg | grep -iE 'hp_omen_kbd|acpi' | tail -n 5
```

A write runs the firmware call in the writer's context, so the `user` and `sys` times from `time` hold
most of the cost. For a view of the whole machine while a loop runs, install `sysstat` and run
`pidstat 5 12` in a second terminal.

**Expected:** no new `ACPI` errors, `last_error=0`, and a `writes_total` that grew by about 300 and then
600.

**Record:** `real`, `user` and `sys` for both rates, the `writes_total` growth, any flicker, and how the
machine felt (input lag, audio dropouts). Then pick the default frame rate.

**Stop if:** `dmesg` shows `ACPI` errors, `last_error` is not 0, or the machine stutters.

## Step 8: unload

**Proves:** unload puts the colors back, leaves no attributes behind, and logs clean.

```bash
sudo rmmod hp_omen_kbd
sudo insmod kernel/hp-omen-kbd/hp_omen_kbd.ko readonly=0
cat $D/zones                              # the boot colors of this load
echo "FF0000 00FF00 0000FF FFFFFF" | sudo tee $D/zones
sudo rmmod hp_omen_kbd
sudo dmesg | grep hp_omen_kbd | tail -n 1
ls $D
```

**Expected:** `hp_omen_kbd: restored the boot colors`, the keyboard back to what `zones` read before
the write, and `ls` saying there is no such directory.

**Stop if:** the line says `restoring the boot colors failed`, or the colors do not come back. Then
see `docs/recovery.md`.

**To finish:** the keyboard still shows what steps 6 and 7 left behind. To put your baseline back:

```bash
sudo insmod kernel/hp-omen-kbd/hp_omen_kbd.ko readonly=0 restore_on_unload=0
echo "40E0D0 40E0D0 40E0D0 0099ED" | sudo tee $D/zones
sudo rmmod hp_omen_kbd
```

## Sign-off

After all eight steps pass, a reviewed commit flips the default of `readonly` to 0. Only then does P5
install the package. If you stopped at any step, the module stays out of autoload and nothing about
your system has changed: a reboot leaves no trace of it apart from colors the firmware kept.
