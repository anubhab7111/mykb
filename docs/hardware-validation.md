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
That driver's daemon threw its errors away, so nobody has seen how the firmware answers a SET.

## What the module's own readouts can and cannot show

- `zones` is a cache: it shows what was last asked for, even if the firmware dropped the write.
- `header` and `header_changed` in `info` compare against the copy taken at load, and only update on
  the next write.

So neither one shows what a SET did to the table. **A reload does:** it reads the table from the
firmware and logs all 128 bytes. The steps below compare those lines with the ones from step 1. The
module is loaded with `restore_on_unload=0` for this, so the unload writes nothing.

Step 0 found how far that goes on this BIOS (BIOS F.21; details in `docs/hardware-notes.md`):

- The firmware keeps the colors and the backlight byte in BIOS memory.
- A SET stores bytes 25–36 there and raises a flag. Code outside ACPI applies the flag later.
- So a reload shows what BIOS memory holds, not what the LEDs show. **Only your eyes show that a write
  was applied**, and the light can follow the call by a moment.

## Before you start

Run these from the repository root, on the kernel you will test. After a kernel update, run them
again: `insmod` refuses a module built for another kernel.

```bash
make -C kernel/hp-omen-kbd check vmtest   # must be green; leaves the module built
mkdir -p ~/omen-kbd-p4                    # saved colors and tables; survives the reboot in step 6
```

**Keep a second terminal open on the kernel log through every step**, and start it again after the
reboot in step 6:

```bash
sudo dmesg -W
```

It prints only new messages. That matters: this machine logs dozens of `ACPI Error` lines at every
boot, and those are not yours to worry about. Below, "a new kernel error" means a line in this
terminal containing `BUG:`, `Oops`, `WARNING:`, `ACPI Error` or `ACPI BIOS Error`. It is a stop
condition in every step.

**One exception.** About every 30 minutes, and sometimes after a wake, the EC asks the OS for the
time. Its handler, `EC0._Q33`, reads the clock from CMOS (region `CMS0`), and Linux has no handler for
that region. So this group of lines is not yours, and not a stop:

```
ACPI Error: No handler for Region [CMS0] (...) [SystemCMOS] (...)
ACPI Error: Region SystemCMOS (ID=5) has no handler (...)
ACPI Error: Aborting method \_SB.PC00.LPCB.EC0._Q33 due to previous error (AE_NOT_EXIST) (...)
```

It was logged on boots long before the module existed. Any other `ACPI Error`, or one naming another
method, still stops you.

**Paste each `insmod` line as one line.** In step 2 a pasted line arrived split in two, so
`restore_on_unload=0` ran as a shell variable and the module kept its default. So every load that needs
`restore_on_unload=0` is followed by a line that prints the value the module got. If that line prints
`Y`, run `sudo rmmod hp_omen_kbd` before doing anything else; nothing has been written yet, so the unload
writes nothing. Then load again.

Nothing else may write the colors. Run `omen-kbd doctor` and look at "competing writers". Do not
start the daemon: that is P5. Until P5 the attributes are root-only for writing, so writes go through
`sudo tee`. Reading needs no `sudo`.

## Step 0: read the firmware's own code (optional, recommended)

**Proves:** what the real method does with these calls. The emulator cannot tell you this, because it
was written from the same understanding as the module. This only reads the ACPI tables; it runs no
firmware code and does not load the module.

```bash
mkdir -p ~/omen-kbd-p4/acpi
cd ~/omen-kbd-p4/acpi
sudo acpidump -b
iasl -d *.dat
grep -l 'Method (WMAA' *.dsl
cd -
```

**Read `WMAA` in the file `grep` names, and what it calls. Record:**

- how long the reply buffer is for each method id. The module expects at least 8 + 1 bytes for method
  2 with a keyboard type or backlight set, 8 + 4 for a backlight read, and 8 + 128 for method 3;
- whether the method is `Serialized`;
- what command `0x20008` query `0x2B` reaches, if the code does not vanish into SMM first.

**Stop if:** a reply is shorter than the module expects. The module would call that an error even
when the firmware did the work.

## Step 1: load read-only

**Proves:** the load gate passes on the real board, the keyboard-type query works, and the module
reads the same colors the old driver read. Nothing is written.

```bash
D=/sys/devices/platform/hp-omen-kbd
K=kernel/hp-omen-kbd/hp_omen_kbd.ko
sudo insmod $K readonly=1
sudo dmesg --notime | grep hp_omen_kbd
cat $D/zones $D/backlight $D/info
cat $D/zones > ~/omen-kbd-p4/colors
sudo dmesg --notime | grep 'hp_omen_kbd: table' | tail -n 8 > ~/omen-kbd-p4/table-step1
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
- The table, as step 0 predicts:
  - byte 0 is `03`;
  - bytes 25–36 are the zone colors, red-green-blue per zone, starting 9 bytes into line `00000010`;
  - every other byte is `00`.

  With the baseline colors, lines 2 and 3 read:
  ```
  hp_omen_kbd: table 00000010: 00 00 00 00 00 00 00 00 00 40 e0 d0 40 e0 d0 40
  hp_omen_kbd: table 00000020: e0 d0 00 99 ed 00 00 00 00 00 00 00 00 00 00 00
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
  fine fails now. `color table reads as all ...; refusing it` in `dmesg` means the firmware answered
  with a table of all `00` or all `ff`.
- A table byte other than 0 and 25–36 is not `00`, or byte 0 is not `03`: the reply did not come from
  the code step 0 read.
- `zones` differs from the baseline **and** from what you see on the keyboard. If it differs from the
  baseline but matches the keyboard, the firmware changed the colors at some boot; record that and go
  on. The steps below use the colors saved above, not the baseline.
- A new kernel error.

## Step 2: write back the same colors

**Proves:** a SET is accepted and leaves every other byte alone. It is a no-op on purpose, so it cannot
show that the firmware applied anything; step 3 shows that.

```bash
D=/sys/devices/platform/hp-omen-kbd
K=kernel/hp-omen-kbd/hp_omen_kbd.ko
sudo rmmod hp_omen_kbd
sudo insmod $K readonly=0 restore_on_unload=0
cat /sys/module/hp_omen_kbd/parameters/restore_on_unload   # must print N
sudo tee $D/zones < ~/omen-kbd-p4/colors
cat $D/info
```

**Expected:** `info` has `writes_total=1`, `header_changed=0` and `last_error=0`, and nothing changes
on the keyboard.

Now see what the firmware really holds:

```bash
D=/sys/devices/platform/hp-omen-kbd
K=kernel/hp-omen-kbd/hp_omen_kbd.ko
sudo rmmod hp_omen_kbd
sudo insmod $K readonly=0 restore_on_unload=0
cat /sys/module/hp_omen_kbd/parameters/restore_on_unload   # must print N
sudo dmesg --notime | grep 'hp_omen_kbd: table' | tail -n 8 | diff ~/omen-kbd-p4/table-step1 -
```

**Expected:** `diff` prints nothing: all 128 bytes are as they were before the first SET.

**Stop if:** the keyboard visibly changes, `tee` reports an error, `diff` prints anything, or a new
kernel error. An error from `tee` does not prove the write was not applied, so look at the keyboard
and at the table after a reload before deciding anything.

## Step 3: find the physical zone order

**Proves:** the firmware applies a SET, which hardware zone is where, and that red, green and blue are
in that order. The default zone map expects hardware 0 = right, 1 = middle, 2 = left, 3 = WASD.

```bash
D=/sys/devices/platform/hp-omen-kbd
echo "FF0000 000000 000000 000000" | sudo tee $D/zones   # hardware 0, red
echo "000000 00FF00 000000 000000" | sudo tee $D/zones   # hardware 1, green
echo "000000 000000 0000FF 000000" | sudo tee $D/zones   # hardware 2, blue
echo "000000 000000 000000 FFFFFF" | sudo tee $D/zones   # hardware 3, white
sudo tee $D/zones < ~/omen-kbd-p4/colors
```

Run them one at a time. After each, wait a second (the firmware applies a write after the call
returns), then look at the keyboard. Then compare the table again. The colors
are back to what they were, so all 128 bytes must match:

```bash
D=/sys/devices/platform/hp-omen-kbd
K=kernel/hp-omen-kbd/hp_omen_kbd.ko
sudo rmmod hp_omen_kbd
sudo insmod $K readonly=0 restore_on_unload=0
cat /sys/module/hp_omen_kbd/parameters/restore_on_unload   # must print N
sudo dmesg --notime | grep 'hp_omen_kbd: table' | tail -n 8 | diff ~/omen-kbd-p4/table-step1 -
```

**Expected:** exactly one zone lit each time, in the color the comment names, and `diff` prints
nothing.

**Record:** the physical zone for each hardware number. If it differs from the default, the fix is the
zone map in `config.toml`, not a stop.

**Stop if:** a color is wrong (say red shows as green), more than one zone lights, keys outside the
four zones change, `diff` prints anything, or a new kernel error.

## Step 4: backlight on and off

**Proves:** the unproven backlight SET works, "off" keeps the colors, and bit 7 of the reply really
means on. A read of `backlight` asks the firmware only if the last answer is over 200 ms old, so the
`sleep` makes these reads show what the firmware reports, not what the module just wrote.

```bash
D=/sys/devices/platform/hp-omen-kbd
echo 0 | sudo tee $D/backlight            # the keyboard goes dark
sleep 1; cat $D/backlight $D/info
echo 1 | sudo tee $D/backlight            # the same colors come back
sleep 1; cat $D/backlight $D/info
```

**Expected:** after `0`, the keyboard goes dark, `backlight` reads `0` and `backlight_raw` is `0x64`.
After `1`, the keyboard lights up again with the same colors, `backlight` reads `1` and `backlight_raw`
is `0xe4`.

Step 0 found that the firmware stores the byte it is sent and returns it unchanged when read. So these
reads only show that the call went through; **only the keyboard shows whether the light obeyed.** At
step 1 the firmware's own value was `0x80` (lit). `0xe4` and `0x64` carry the value 100 (`0x64`) in their
low bits, which this keyboard, with on and off only, may ignore.

**Record:** both `backlight_raw` values, and whether the light after `1` looks the same as before
`0` (same brightness).

Then press the Fn backlight key (on and off only, on this keyboard) a few times. After each press, wait
a second and run:

```bash
D=/sys/devices/platform/hp-omen-kbd
cat $D/backlight $D/info
```

**Record:** `backlight_raw` after each press, and whether `backlight` matched what you saw.

A read returns a byte the firmware keeps in BIOS memory, which a WMI backlight set writes. If
`backlight_raw` does not follow the Fn key, the Fn key does not update that byte. That is a finding,
not a stop: the daemon then cannot see Fn-key changes by reading `backlight`. Record it for P5.

Next, check whether writing colors while the backlight is off turns it on:

```bash
D=/sys/devices/platform/hp-omen-kbd
echo 0 | sudo tee $D/backlight
sudo tee $D/zones < ~/omen-kbd-p4/colors  # look at the keyboard: on or off?
sleep 1; cat $D/backlight
echo 1 | sudo tee $D/backlight
```

**Record:** on or off. Either answer is fine, but the daemon has to be told about it.

Last, compare the table:

```bash
D=/sys/devices/platform/hp-omen-kbd
K=kernel/hp-omen-kbd/hp_omen_kbd.ko
sudo rmmod hp_omen_kbd
sudo insmod $K readonly=0 restore_on_unload=0
cat /sys/module/hp_omen_kbd/parameters/restore_on_unload   # must print N
sudo dmesg --notime | grep 'hp_omen_kbd: table' | tail -n 8 | diff ~/omen-kbd-p4/table-step1 -
```

**Stop if:**

- `echo 0` reports `Input/output error` or `Operation not supported`. The SET's shape is wrong for
  this BIOS. The Fn key still works; the fix is a reviewed code change.
- The colors differ after turning the backlight back on, or `diff` prints anything.
- The keyboard does not go dark after `echo 0`, or does not light up after `echo 1`: the firmware does
  not act on these bytes.
- After an Fn-key press, `backlight_raw` changes but its bit 7 disagrees with the light: the bit-7
  decode is wrong. If it does not change at all, record that; it is not a stop (see above).
- A new kernel error.

## Step 5: suspend and resume

**Proves:** the colors survive sleep, and the module's re-apply works on real firmware.

```bash
D=/sys/devices/platform/hp-omen-kbd
sudo tee $D/zones < ~/omen-kbd-p4/colors  # something for the re-apply to do
echo 1 | sudo tee $D/backlight
cat $D/info                               # writes_total=2
systemctl suspend                         # wake it up, then wait 2 s
cat $D/backlight $D/info
```

Run the last three lines twice.

**Expected:** the same colors on the keyboard after each wake, `last_error=0`, and no
`re-applying colors after resume failed` in the log. `writes_total` grows by 2 per resume: one SET for
the zones and one for the backlight.

With the daemon running (P5), each resume gives two zone writes: the module's re-apply and the
daemon's forced rewrite. That is expected, not a bug.

To see what the firmware does on its own, do it once more with nothing to re-apply:

```bash
D=/sys/devices/platform/hp-omen-kbd
K=kernel/hp-omen-kbd/hp_omen_kbd.ko
sudo rmmod hp_omen_kbd
sudo insmod $K readonly=1
systemctl suspend                         # wake it up, then look at the keyboard
cat $D/info
```

**Record:** whether the colors survived this last one with no help (`info` still has
`writes_total=0`).

**Stop if:** the keyboard stays dark or wrong after a wake with the module's re-apply, `last_error` is
not 0, or a new kernel error.

## Step 6: cold boot

**Proves:** whether the firmware keeps colors with the power off. That decides how careful the write
budget and the opt-in for effects must stay.

```bash
D=/sys/devices/platform/hp-omen-kbd
K=kernel/hp-omen-kbd/hp_omen_kbd.ko
sudo rmmod hp_omen_kbd                    # skip if it is not loaded
sudo insmod $K readonly=0 restore_on_unload=0
cat /sys/module/hp_omen_kbd/parameters/restore_on_unload   # must print N
echo "FF00FF 00FFFF FF00FF 00FFFF" | sudo tee $D/zones
sudo poweroff
```

Unplug the charger, wait 30 seconds, plug it back in and start the machine. Look at the keyboard
**before logging in**.

**Before you unplug, check the battery** (run this before `poweroff`):
`cat /sys/class/power_supply/BAT0/capacity`.

- If it reads 0, or the battery is dead, unplugging cuts all power, the EC's and the CMOS clock's
  included. The next boot then reports a CMOS error and may load the BIOS defaults. In 2026-11 that is
  what happened.
- That tests a total power loss, not a normal power-off. With a dead battery, power on without
  unplugging. After login, start `sudo dmesg -W` again in the second terminal, and read what
the firmware has now:

```bash
D=/sys/devices/platform/hp-omen-kbd
K=kernel/hp-omen-kbd/hp_omen_kbd.ko
sudo insmod $K readonly=1
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

**Proves:** what repeated writes cost, so the default effect rate can be picked, and that they leave
the rest of the table alone. Budget used: at most 900 of the 40000 writes per hour.

```bash
D=/sys/devices/platform/hp-omen-kbd
K=kernel/hp-omen-kbd/hp_omen_kbd.ko
sudo rmmod hp_omen_kbd
sudo insmod $K readonly=0 restore_on_unload=0
cat /sys/module/hp_omen_kbd/parameters/restore_on_unload   # must print N
cat $D/zones > ~/omen-kbd-p4/colors-step7
sudo dmesg --notime | grep 'hp_omen_kbd: table' | tail -n 8 > ~/omen-kbd-p4/table-step7
cat $D/info                               # note writes_total

# SMI count (MSR 0x34): 60 s with no writes, for comparison
sudo dd if=/dev/cpu/0/msr bs=8 count=1 skip=52 iflag=skip_bytes status=none | od -An -tu8
sleep 60
sudo dd if=/dev/cpu/0/msr bs=8 count=1 skip=52 iflag=skip_bytes status=none | od -An -tu8

# 5 writes per second for 60 s
time sudo bash -c 'for i in $(seq 150); do
    echo "40E0D0 40E0D0 40E0D0 0099ED" >/sys/devices/platform/hp-omen-kbd/zones; sleep 0.2
    echo "0099ED 0099ED 0099ED 40E0D0" >/sys/devices/platform/hp-omen-kbd/zones; sleep 0.2
done'
cat $D/info
sudo dd if=/dev/cpu/0/msr bs=8 count=1 skip=52 iflag=skip_bytes status=none | od -An -tu8

# 10 writes per second for 60 s
time sudo bash -c 'for i in $(seq 300); do
    echo "40E0D0 40E0D0 40E0D0 0099ED" >/sys/devices/platform/hp-omen-kbd/zones; sleep 0.1
    echo "0099ED 0099ED 0099ED 40E0D0" >/sys/devices/platform/hp-omen-kbd/zones; sleep 0.1
done'
cat $D/info
sudo dd if=/dev/cpu/0/msr bs=8 count=1 skip=52 iflag=skip_bytes status=none | od -An -tu8

# All those writes must have left the rest of the table alone
sudo tee $D/zones < ~/omen-kbd-p4/colors-step7
sudo rmmod hp_omen_kbd
sudo insmod $K readonly=0 restore_on_unload=0
cat /sys/module/hp_omen_kbd/parameters/restore_on_unload   # must print N
sudo dmesg --notime | grep 'hp_omen_kbd: table' | tail -n 8 | diff ~/omen-kbd-p4/table-step7 -
```

The ACPI part of a write is cheap: step 0 found it only stores bytes in memory. The real work is done
later, outside ACPI, so `time` cannot see it. The SMI counter can:

- If it grows with the write rate, compared with the idle minute, each write costs a System Management
  Interrupt. Every CPU stops for one, so this is the number that picks the frame rate.
- If it does not grow, the firmware polls the flag, and writes faster than its poll merge into one.

For a view of the whole machine while a loop runs, install `sysstat` and run `pidstat 5 12` in a third
terminal.

**Expected:** `last_error=0`, a `writes_total` that grew by about 300 and then 600, and `diff` printing
nothing.

**Record:** the SMI count growth for the idle minute and both rates, `real`, `user` and `sys` for both
rates, the `writes_total` growth, any flicker, and how the
machine felt (input lag, audio dropouts). Then pick the default frame rate.

**Stop if:** `last_error` is not 0, `diff` prints anything, the machine stutters, or a new kernel error.

## Step 8: unload

**Proves:** unload puts the colors back, leaves no attributes behind, and logs clean.

```bash
D=/sys/devices/platform/hp-omen-kbd
K=kernel/hp-omen-kbd/hp_omen_kbd.ko
sudo rmmod hp_omen_kbd
sudo insmod $K readonly=0
cat $D/zones                              # the boot colors of this load
echo "FF0000 00FF00 0000FF FFFFFF" | sudo tee $D/zones
sudo rmmod hp_omen_kbd
sudo dmesg --notime | grep hp_omen_kbd | tail -n 1
ls $D
```

**Expected:** `hp_omen_kbd: restored the boot colors`, the keyboard back to what `zones` read before
the write, and `ls` saying there is no such directory.

**Stop if:** the line says `restoring the boot colors failed`, or the colors do not come back. Then
see `docs/recovery.md`.

**To finish:** the keyboard still shows what steps 6 and 7 left behind. To put your baseline back:

```bash
D=/sys/devices/platform/hp-omen-kbd
K=kernel/hp-omen-kbd/hp_omen_kbd.ko
sudo insmod $K readonly=0 restore_on_unload=0
cat /sys/module/hp_omen_kbd/parameters/restore_on_unload   # must print N
echo "40E0D0 40E0D0 40E0D0 0099ED" | sudo tee $D/zones
sudo rmmod hp_omen_kbd
```

## Sign-off

After all eight steps pass, a reviewed commit flips the default of `readonly` to 0. Only then does P5
install the package. If you stopped at any step, the module stays out of autoload and nothing about
your system has changed: a reboot leaves no trace of it apart from colors the firmware kept.
