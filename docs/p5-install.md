# Install and soak (phase P5)

P4 proved the module on this machine's firmware, with every load and write done by hand. P5 makes it
permanent:
- the package loads the module at every boot;
- udev lets the `omen-kbd` group write it;
- the daemon runs as your user and writes the colors;
- the desktop hooks drive it.

Then one hour of normal use and three wakes show that it holds up.

**Every step that loads the module or writes the keyboard needs your go-ahead.**

- Do one step at a time, after you have agreed to it.
- You run every `sudo` command in your own terminal.
- If a stop condition fires, stop. `docs/recovery.md` gets you back; its section 5 removes the package.
- Record results under "P5 results" in `docs/hardware-notes.md`.

Three firsts happen here. Nothing has ever exercised them, not even the VM, which has no udevd:

| What | First proven in | If it fails |
|---|---|---|
| udev's `RUN` line gives `zones`/`backlight` to group `omen-kbd` | Step 2 | Only root can write. Not a stop: the daemon cannot start until it is fixed. |
| `systemd-modules-load` loads the module at boot | Step 3 | No keyboard colors until a manual `modprobe`. Not a stop. |
| The daemon writes the real keyboard | Step 5 | The CLI prints the error, and `info` shows `last_error`. |

## Rules carried over from P4

- **Never unplug the charger.** The battery is dead (2.74 V), so unplugging is a hard power cut that
  resets CMOS. So the spec's "unplug the charger, the effect pauses" check is not done in P5. The
  on-battery rules stay untested until the battery is replaced.
- **Reboot with `systemctl reboot`, never a power-off.** Whether the colors survive a normal
  power-off with a healthy battery is still open (P4 step 6).
- **Suspend only from a TTY** (Ctrl+Alt+F3). Hyprland hangs after a wake, which predates the module.
  The daemon is a user service, not part of the Hyprland session, so it keeps running there.
- **Keep the kernel log open** in a second terminal through steps 2–9: `sudo dmesg -W`. A new line
  with `BUG:`, `Oops`, `WARNING:`, `ACPI Error` or `ACPI BIOS Error` is a stop. The `EC0._Q33` /
  `SystemCMOS` group every 30 minutes is the known exception (see `docs/hardware-validation.md`).

## Before you start

From the repository root, on the branch you will install:

```bash
make -C kernel/hp-omen-kbd check vmtest      # green; leaves hp_omen_kbd.ko built, to compare in step 1
kernel/hp-omen-kbd/tests/check-packaging.sh  # green
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DOMEN_KBD_SANITIZE=OFF
cmake --build build-release && ctest --test-dir build-release
mkdir -p ~/omen-kbd-p5
```

The keyboard should show the P1 baseline that P4 put back: `40E0D0 40E0D0 40E0D0 0099ED` in hardware
order (right, middle, left, WASD). Check the battery, so you know the rules above still apply:

```bash
cat /sys/class/power_supply/BAT0/voltage_now   # about 2700000 (2.7 V): never unplug
```

## Step 1: install the package

Builds and installs the package. Nothing loads yet.

```bash
cd packaging/arch
BUILDDIR=~/omen-kbd-p5/build PKGDEST=~/omen-kbd-p5 makepkg -f
cd ../..
sudo pacman -U ~/omen-kbd-p5/hp-omen-kbd-dkms-0.1.0-1-any.pkg.tar.zst
sudo usermod -aG omen-kbd "$USER"
```

pacman's hooks then do three things:
- DKMS builds the module for every installed kernel;
- `systemd-sysusers` creates the group;
- udev reloads its rules.

`usermod` takes effect at the next login, so step 3's reboot covers it.

**Check:**

```bash
pacman -Ql hp-omen-kbd-dkms
dkms status hp-omen-kbd
modinfo -n hp_omen_kbd
modinfo -F srcversion hp_omen_kbd
modinfo -F srcversion kernel/hp-omen-kbd/hp_omen_kbd.ko
modinfo -n hp_wmi
getent group omen-kbd
lsmod | grep hp_omen_kbd
```

- The package lists only the module sources under `/usr/src/hp-omen-kbd-0.1.0/` plus:
  - `99-omen-kbd.rules`;
  - `sysusers.d/omen-kbd.conf`;
  - `modules-load.d/hp-omen-kbd.conf`.

  No `hp-wmi` anywhere.
- `dkms status` says `installed` for the running kernel.
- `modinfo -n hp_omen_kbd` is under `updates/dkms/`.
- **The two `srcversion` lines are equal.** The installed module is built from the same source P4
  tested.
- `hp_wmi` is still the stock module, under `kernel/drivers/platform/x86/hp/`.
- The group exists, and `lsmod` prints nothing.

**Stop if** the DKMS build failed, the `srcversion`s differ, or anything named `hp-wmi` or `hp_wmi`
was installed.

## Step 2: load it and check the udev rule

This loads the module with its defaults: `readonly=0`, writes allowed. Loading writes nothing. The
keyboard must not change.

```bash
sudo modprobe hp_omen_kbd
D=/sys/devices/platform/hp-omen-kbd
ls -l $D/zones $D/backlight
cat $D/zones
cat $D/info
cat /sys/module/hp_omen_kbd/parameters/readonly
```

- `zones` and `backlight` are `-rw-rw-r--` owned by `root omen-kbd`. **This is udev's `RUN` line
  working.**
- `zones` shows the baseline `40E0D0 40E0D0 40E0D0 0099ED`, the keyboard looks the same, and
  `readonly` is `N`.
- In `info`: `kbd_type=1`, `writes_total=0`, `last_error=0`.

**If the files are still `root root` with `-rw-r--r--`:** the rule did not run. Not a stop. Collect
this and stop the step; the fix is a reviewed change to the rule:

```bash
udevadm info -a /sys/devices/platform/hp-omen-kbd | head -n 20
sudo udevadm test /sys/devices/platform/hp-omen-kbd 2>&1 | grep -i -E 'omen|RUN'
journalctl -b -u systemd-udevd | tail -n 20
```

**Stop if** the keyboard changed, `kbd_type` is not 1, or a new kernel error appears.

## Step 3: reboot

Proves the boot path: `systemd-modules-load` loads the module, udev fixes the permissions on its
own, and your new group membership arrives.

```bash
systemctl reboot
```

Before you log in, note what the keyboard shows. It should be the baseline: a warm reboot keeps the
colors. After logging in:

```bash
D=/sys/devices/platform/hp-omen-kbd
lsmod | grep hp_omen_kbd
journalctl -b -u systemd-modules-load
journalctl -b -k | grep hp_omen_kbd
ls -l $D/zones $D/backlight
id -nG
grep Groups /proc/$(pgrep -u "$USER" -x systemd)/status
getent group omen-kbd
```

- `lsmod` lists `hp_omen_kbd`. The `systemd-modules-load` log has no error for it.
- The kernel log shows the module's usual load lines, with `kbd_type=1` and the table, and no
  `ENODEV`. This is the first time `wmi_has_guid` runs this early in boot.
- The files are `root omen-kbd`, `-rw-rw-r--`.
- `id -nG` lists `omen-kbd`. **The user manager's `Groups:` line contains the gid `getent` printed.**
  The daemon inherits its groups from that process, not from your shell.

If the module did not load at boot but `sudo modprobe hp_omen_kbd` works: not a stop. Record the
`systemd-modules-load` and kernel lines, and go on with the manual load. The daemon starts at
login, after it.

## Step 4: install the daemon and the config

No sudo and no keyboard writes. Everything goes under your home.

```bash
cmake --install build-release --prefix ~/.local
install -Dm644 packaging/omen-kbd.service ~/.config/systemd/user/omen-kbd.service
systemctl --user daemon-reload
install -Dm644 packaging/config.example.toml ~/.config/omen-kbd/config.toml
~/.local/bin/omen-kbd doctor
```

**`config.toml` is yours.** omen-kbd never writes it. Its first profile is what step 5 paints. The
example's `default` profile uses theme colors: mauve, blue, teal and peach, made vivid. Edit it
before step 5 if you want something else first.

**Expected from `doctor`:**
- The module is OK, with `readonly=0` and board 8A4E.
- `zones` and `backlight` are writable by you.
- No competing writer, and no hp-wmi DKMS package.
- The theme palette is found.
- The daemon is not running yet.
- The hooks are not installed yet (step 7).

**Stop if** `doctor` reports a competing writer, or that you cannot write `zones`.

## Step 5: first daemon start

This is the first time the daemon writes the real keyboard. Expect one backlight write and one zone
write, so `writes_total` grows by 2.

```bash
D=/sys/devices/platform/hp-omen-kbd
cat $D/info
systemctl --user start omen-kbd.service
sleep 1
cat $D/info
~/.local/bin/omen-kbd status
journalctl --user -u omen-kbd -b --no-pager
grep Groups /proc/$(systemctl --user show -P MainPID omen-kbd)/status
```

- The keyboard shows the first profile, `default` unless you changed it.
- `writes_total` grew by 2, `last_error=0`.
- `status` shows the profile, the zones, `backend hp-omen-kbd`, and on.
- The journal has no warning beyond a missing hook, and the daemon's `Groups:` contains the
  `omen-kbd` gid.

**Stop if** the keyboard shows anything other than the profile, the daemon exits, or `last_error` is
not 0. Run `systemctl --user stop omen-kbd`, then follow `docs/recovery.md`.

## Step 6: the CLI on the real keyboard

Each line changes the keyboard. Wait a second between lines and watch it.

```bash
~/.local/bin/omen-kbd set ff0000 00ff00 0000ff ffffff   # left red, middle green, right blue, WASD white
~/.local/bin/omen-kbd profile white
~/.local/bin/omen-kbd brightness 30                    # dimmer white
~/.local/bin/omen-kbd brightness 100
~/.local/bin/omen-kbd off                              # dark; colors kept
~/.local/bin/omen-kbd on                               # white again
~/.local/bin/omen-kbd profile default
~/.local/bin/omen-kbd effect wave                      # refused: effects are opt-in (exit 1)
```

**Then the Fn backlight key** (the I5 fix, on the real firmware):

1. Press the Fn backlight key once: the keyboard goes dark. `~/.local/bin/omen-kbd status` says
   `off`.
2. `~/.local/bin/omen-kbd toggle`, once: the light comes back with the profile.
3. Press the Fn key twice (off, on): `status` says `on`.

**And a restart:**

```bash
systemctl --user restart omen-kbd.service
~/.local/bin/omen-kbd status
```

The keyboard keeps the profile. The daemon remembers it in `~/.local/state/omen-kbd/state.json`.

**Stop if** a command reports success but the keyboard did not change, or a toggle after the Fn key
needs two presses.

## Step 7: the desktop hooks

Each hook is a change to `~/dotfiles`, which is stowed into `~/.config`. **You make each change**;
omen-kbd never edits these files, and Hyprland reloads its config when you save. Do them one at a
time, in this order. Each file in `packaging/snippets/` says where it goes.

1. **Autostart:** `autostart.lua`. Test: log out and in. `systemctl --user is-active omen-kbd` says
   `active`.
2. **Mode colors:**

   ```bash
   ~/.local/bin/omen-kbd gen-hyprland > ~/dotfiles/config-hypr/.config/hypr/modules/kbdlight.lua
   ```

   Then add `require("modules.kbdlight")` to `hyprland.lua`. Test:
   - Hold Super: mauve after about 250 ms, and the profile again on release.
   - Super+Q and plain typing paint nothing.
   - Super+R: teal until you leave resize.
3. **Lock and idle:** `hypridle.conf`, then restart hypridle (`pkill hypridle; hypridle &`, or log
   out and in). Test:
   - Lock: the keyboard goes dark. Unlock: the profile.
   - Leave it idle for 150 s: it dims to 20 %. A key brings it back.
4. **Waybar:** `waybar-module.jsonc` and `waybar-style.css`. Test:
   - Click: next profile. Right-click: toggle. Scroll: brightness.
   - After an Fn-key off, the module shows off.
5. **Theme:** `rice-theme.md`. Test: `rice-theme apply <another theme>`. The keyboard follows within
   a second.
6. **Game mode:** `gamemode.lua`. Test: SUPER+F1. The `game` profile shows (dark, red WASD), and
   SUPER+F1 again brings the previous one back.

Then `~/.local/bin/omen-kbd doctor` reports no WARN or FAIL.

## Step 8: reboot with everything in place

```bash
systemctl reboot
```

After login, with nothing typed:
- The keyboard shows the saved profile.
- `systemctl --user is-active omen-kbd` says `active`.
- `~/.local/bin/omen-kbd doctor` is clean.

## Step 9: three wakes

From a TTY (Ctrl+Alt+F3), with the daemon running:

```bash
D=/sys/devices/platform/hp-omen-kbd
grep -o 'writes_total=[0-9]*' $D/info
systemctl suspend
```

Wake it with a key. Then run these, and repeat the suspend twice more:

```bash
D=/sys/devices/platform/hp-omen-kbd
grep -o 'writes_total=[0-9]*' $D/info
~/.local/bin/omen-kbd status
```

- The keyboard shows the profile after each wake.
- Each wake adds about 4 to `writes_total`: the module's re-apply (zones and backlight) and the
  daemon's forced rewrite (zones and backlight).
- `status` says on, with no error.

If Hyprland hangs afterwards, `pkill Hyprland` from the TTY and log in again, as in P4. The daemon
keeps running, and `systemctl --user is-active omen-kbd` stays `active`.

## Step 10: one hour of normal use

Use the machine as usual for an hour: hold modifiers, lock, switch themes, scroll the Waybar
module. Before and after, from any terminal:

```bash
D=/sys/devices/platform/hp-omen-kbd
date; grep -o 'writes_total=[0-9]*' $D/info
ps -o pid,etime,time,rss -C omen-kbd
journalctl --user -u omen-kbd -b --no-pager | tail -n 20
```

- The daemon's CPU `time` grows by well under a second for the hour: a static profile writes nothing
  when nothing changes.
- `writes_total` grows by a few per hint, lock or theme switch, and not at all while you only type.
- The journal has no new warnings, and the kernel log no new errors.
- `rss` stays about the same.

## Sign-off

After all ten steps pass:
- `docs/hardware-notes.md` gets the P5 results;
- the README's status table moves to "installed and in daily use".

Still open after P5, until the battery is replaced:
- whether the colors survive a normal power-off (P4 step 6);
- the on-battery rules and effect pause.
