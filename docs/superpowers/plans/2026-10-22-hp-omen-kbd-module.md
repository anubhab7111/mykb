# hp_omen_kbd Kernel Module Implementation Plan (spec phase P3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Write, test and build the `hp_omen_kbd` kernel module and its DKMS/udev/sysusers packaging, ready for the P4 hardware validation, without ever loading it on the host.

**Architecture:** Every decision the module makes lives in a pure C core (`core.c`/`core.h`): text formats, color-table patching, firmware request/reply bytes, error mapping, write budget, rate limit and the `info` line. The core is compiled into the module and into userspace tests run by CTest. Those tests also check the core's formats against the C++ daemon and `doctor`. The kernel glue (`main.c`) adds the load gate, the WMI calls, the platform device (attributes created before the add uevent), the mutex-serialized write path, the resume re-apply and the unload restore. The only place P3 ever loads the module is a QEMU VM booting this machine's own kernel. There, an ACPI table that emulates HP's BIOS WMI method runs the whole module against fake firmware.

**Tech Stack:** C (kernel style, kbuild against `/usr/lib/modules/$(uname -r)/build`, Linux 7.2.9-arch1-1). DKMS 3.4.4. QEMU/KVM with a static C init. `iasl` (package `acpica`) for the firmware emulator. `checkpatch.pl`. CMake/CTest (C11 core tests, GoogleTest contract tests). makepkg/namcap, `udevadm verify`, `systemd-sysusers`.

**Spec:** `docs/superpowers/specs/2026-10-21-omen-kbd-design.md`: "Component 1 — kernel module", the Phases table and "P4 — hardware validation protocol". **Facts:** `docs/hardware-notes.md`. **Daemon contract (as built in P2):** `src/backend/hp_omen_kbd.cpp`, `map_errno` in `src/backend/backend.cpp`, and `info_field`/`run_checks` in `src/cli/doctor.cpp`. **Branch:** continue on `worktree-omen-kbd-design`, or on a branch made from it if P2 has been merged.

## Global Constraints

**Safety**
- P3 never loads the module on the host. Never run `insmod`, `modprobe`, `rmmod` or `dkms install`; never write under `/sys`; never use `sudo`. The one exception is the user running `sudo pacman -S --needed acpica` themselves, if they accept the firmware emulator (Task 5). The module is loaded only inside QEMU, by `tests/vm/run.sh`.
- No autoload: no `MODULE_ALIAS` and no `MODULE_DEVICE_TABLE`. A `dmi:` or `wmi:` alias would make udev load the module at every boot. Enforced by checking that `modinfo` shows no `alias:` line.
- Load gate, in this order, anything else → `-ENODEV`. Adding a board is a code change, never a parameter.
  1. `dmi_check_system()` on a table with `DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "HP")` and `DMI_EXACT_MATCH(DMI_BOARD_NAME, "8A4E")`.
  2. `wmi_has_guid("5FB7F034-2C63-45E9-BE91-3D44E2C707E4")`.
  3. The keyboard type is 1 or 2.
- The module does not claim the WMI GUID. It uses only the legacy `wmi_evaluate_method()`/`wmi_has_guid()` (exported GPL, so `MODULE_LICENSE("GPL")`), never touches `hp-wmi`, and its DKMS package ships only `hp_omen_kbd.ko`.
- A zone write fetches the color table fresh from the firmware just before the SET and patches only bytes 25–36. Bytes 0–24 and 37–127 go back exactly as the firmware just returned them. "Off" never touches the color table. Zones 4–7 do not exist.
- Errors are negative errnos, never positive byte counts:
  - BIOS return code 0x03 or 0x04 → `-EOPNOTSUPP`;
  - any other non-zero code, an ACPI failure, a non-buffer reply or a short reply → `-EIO`.

**Firmware calls**
- Request (`bios_args`): little-endian u32 signature `0x55434553` ("SECU"), then u32 command, u32 query and u32 datasize, then a 128-byte data area (144 bytes in all).
- Reply: an ACPI buffer of u32 sigpass, then u32 return code, then data.
- `wmi_evaluate_method(guid, 0, method, …)` takes its method id from the requested reply size: 0 → 1, 1–4 → 2, 5–128 → 3, 129–1024 → 4, 1025–4096 → 5.

| Call | command | query | input data | insize | outsize | method | proven on this 8A4E |
|---|---|---|---|---|---|---|---|
| keyboard type | 0x20008 | 0x2B | u8 0 | 1 | 1 | 2 | no (upstream v10 patch shape); P4 step 1 proves it |
| color table GET | 0x20009 | 0x02 | 128 zero bytes | 128 | 128 | 3 | yes (hp-omen-extra, before P1) |
| color table SET | 0x20009 | 0x03 | the 128-byte table | 128 | 128 | 3 | yes (hp-omen-extra) |
| backlight GET | 0x20009 | 0x04 | u32 0 | 4 | 4 | 2 | yes (hp-omen-extra read `1` in P1) |
| backlight SET | 0x20009 | 0x05 | u8 0xE4 (on) / 0x64 (off) | 1 | 1 | 2 | no (v10 shape); P4 step 4 proves it |

Backlight decoding: on ⇔ bit 7 (0x80) of the first reply byte. 0xE4 is on and 0x64 is off; other Fn-key levels may differ in the low bits.

**Interface: must match the daemon and `doctor` exactly as they are today**
- **Directory:** `/sys/devices/platform/hp-omen-kbd/`, from a platform device named `"hp-omen-kbd"` with `PLATFORM_DEVID_NONE`. Its attributes are set in `pdev->dev.groups` **before** `platform_device_add()`, so the files exist when the add uevent fires (fixes omen-space bug K6).
- **`zones`:**
  - Read: `RRGGBB RRGGBB RRGGBB RRGGBB\n` (uppercase, hardware order), served from the cache with no firmware call.
  - Write: exactly 4 colors or 1 (= all zones). Each color is 6 hex digits in either case, with one ASCII space between colors and one optional trailing `\n`. Anything else is `-EINVAL`.
  - The daemon writes `"%s %s %s %s\n"` of uppercase `Rgb::to_hex()`.
- **`backlight`:** read gives `0\n` or `1\n` (a firmware read, cached for 200 ms). Write takes `0` or `1` plus an optional `\n`.
- **`info`:** one line of space-separated `key=value` tokens, exactly:
  `board=8A4E kbd_type=<u> readonly=<0|1> min_interval_ms=<u> max_writes_per_hour=<u> writes_last_hour=<u> writes_total=<u> budget_exhausted=<0|1> last_error=<d> backlight_raw=0x<xx> header_changed=<0|1> header=<50 lowercase hex chars>\n`
  - `writes_total` counts SETs the firmware accepted.
  - `last_error` is the latest firmware errno (0 if none ever failed).
  - `doctor` reads the `readonly=` token.
- **errno contract** (the daemon's `map_errno`): `-EBUSY` means the hourly budget is exhausted, `-EPERM` means readonly, `-EINVAL` means bad input, and `-EIO`/`-EOPNOTSUPP` mean the firmware failed. Input is parsed before any guard runs, so bad input is `-EINVAL` even when readonly.

**Parameters** (all `0444`, so fixed while the module is loaded)
- `readonly` (bool, default 1): writes fail with `-EPERM`.
- `min_interval_ms` (uint, default 50): clamped to [33, 1000].
- `max_writes_per_hour` (uint, default 40000): a rolling hour in one-minute buckets; 0 means no writes; exhausted → `-EBUSY`.
- `restore_on_unload` (bool, default 1).

**Behaviour**
- **Write path (zones and backlight), in order:**
  1. Parse the input.
  2. `mutex_lock`.
  3. Readonly check.
  4. Sleep the rest of `min_interval_ms` since the last SET.
  5. Budget check. The budget counts SET calls, including failed ones.
  6. For zones only: fresh GET; if bytes 0–24 differ from the boot copy, log it once; patch bytes 25–36.
  7. SET.
  8. Update the cache and counters, unlock, and return `count` or the errno.
  
  Every firmware call holds the mutex; the call wrapper says so with `lockdep_assert_held`.
- **Probe:** keyboard type, then the boot GET of the table and the backlight. Any failure refuses the load with its errno; the keyboard-type checks refuse with `-ENODEV`.
- **Resume/restore:** if anything was written since load, a delayed work runs 1000 ms later. It re-applies the cached zones if zones were written (fresh GET → patch → SET), and the wanted backlight if the backlight was written. It goes through the same interval and budget guards. The probe at load takes the mutex too.
- **Unload, in order:**
  1. Unregister the device, so no more stores can arrive.
  2. `cancel_delayed_work_sync`.
  3. If zones were written and `restore_on_unload` is set, SET the boot colors (fresh GET → patch the boot zones → SET). The interval applies; the budget does not.
  4. Unregister the driver.
- **Log lines.** `pr_fmt` is `KBUILD_MODNAME ": "`. The text is exact, because the VM tests match it:
  - `board not on the allowlist (only HP 8A4E)`
  - `HP BIOS WMI interface 5FB7F034-2C63-45E9-BE91-3D44E2C707E4 not found`
  - `keyboard type query failed (%d); not binding`
  - `keyboard type %u is not a four-zone keyboard; not binding`
  - `HP 8A4E keyboard type %u, backlight 0x%02x, zones %s, readonly=%d`, followed by `print_hex_dump(KERN_INFO, KBUILD_MODNAME ": table ", DUMP_PREFIX_OFFSET, 16, 1, table, 128, false)`
  - `color table header (bytes 0-24) differs from the boot copy; keeping the firmware's bytes` (logged once)
  - `re-applying colors after resume failed (%d)`
  - `restored the boot colors`, or `restoring the boot colors failed (%d)`

**Gates**
- **Core and contract tests:** `cmake --build build && ctest --test-dir build --output-on-failure`. Debug builds run with ASan+UBSan. C flags: `-std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`.
- **Module:** `make -C kernel/hp-omen-kbd check`, which runs three things:
  - a kbuild `W=1` build with `KCFLAGS=-Werror` (zero warnings);
  - `checkpatch.pl --strict --no-tree -f` on `main.c core.c core.h`: 0 errors and 0 warnings (CHECKs are fixed or ledgered);
  - `tests/check-module.sh`.
- **VM:**
  - `make -C kernel/hp-omen-kbd vmtest-gate`: the gate scenarios, which need no extra packages;
  - `make -C kernel/hp-omen-kbd vmtest`: every scenario; needs `iasl`.
  
  In both, each scenario's `VM` lines must equal its `.expected` file.
- **Style:** kernel C uses kernel style (tabs). `kernel/.clang-format` sets `DisableFormat: true` so the repo's C++ style never touches it; checkpatch is the style gate. `core.h` holds the only `#ifdef __KERNEL__`. The userspace build gets its libc headers and the `u8`/`u32`/`u64`/`s64` typedefs from `kernel/hp-omen-kbd/tests/userspace.h`, which is never shipped or checkpatch'd.

## Review Focus

1. **Malformed `zones`/`backlight` writes.** No newline, `\r\n`, `#`, two spaces, a trailing space, 2/3/5 colors, a 5- or 7-digit color, empty input, an embedded NUL or a 4096-byte write must all give `-EINVAL` exactly per the rule. Lowercase and no-newline input are accepted. Pinned: `ZonesParserIsStrict` (Task 1), VM `rw-invalid` (Task 6).
2. **Firmware replies.** A reply shorter than 8 bytes, data shorter than requested, codes 0x02/0x03/0x04/0x05/0xFF or a non-buffer object must give a negative errno, copy nothing and never return a positive value. Pinned: `ReplyParsingNeverReturnsPositive` (Task 2), VM `emu-short-reply` (Task 5) and `rw-firmware-error` (Task 6).
3. **Budget and clock edges.** Exactly the maximum within an hour, the oldest minute expiring, `max_writes_per_hour=0`, and the clock jumping hours forward or going backwards: never more than the maximum per rolling hour, and no overflow. Pinned: `BudgetIsARollingHour` (Task 2), VM `rw-budget` (Task 6).
4. **Firmware bytes outside 25–36 that differ from the boot copy.** They are written back exactly as just read, and a header change is logged once. Pinned: `PatchTouchesOnlyZoneBytes` (Task 1), VM `rw-write-and-reload` (the emulator's header and tail patterns survive) and `rw-header-drift` (a header that changes after boot is kept, not "fixed", and logged once) (Task 6).
5. **Teardown and concurrency.** A resume re-apply pending at unload, a write racing unload, and two writers must be serialized by the mutex, with the work cancelled before the restore, no use-after-free, and a clean kernel log. Pinned: VM `rw-resume-reapply` and `rw-restore-on-unload` with `kmsg-clean` (Task 6). The reviewer checks lock coverage deliberately.

---

### Task 1: Core — text formats and the color table

**Files:**
- Create: `kernel/hp-omen-kbd/core.h`, `kernel/hp-omen-kbd/core.c`, `kernel/hp-omen-kbd/tests/core_test.c`, `kernel/hp-omen-kbd/tests/userspace.h`, `kernel/.clang-format`.
- Modify `CMakeLists.txt`:
  - `LANGUAGES C CXX`;
  - library `hokcore` from `core.c`, with C11 and the C flags above, and PUBLIC include dirs `kernel/hp-omen-kbd` and `kernel/hp-omen-kbd/tests`;
  - executable `hok-core-tests`, registered with `add_test(NAME hok-core COMMAND hok-core-tests)`.
- Modify `.gitignore`: kbuild outputs under `kernel/hp-omen-kbd/` and `kernel/hp-omen-kbd/tests/vm/out/`.

**Interfaces — Produces** (in `core.h`, plain C, wrapped in `extern "C"` for C++ callers):
- Constants: `HOK_ZONES 4`, `HOK_TABLE_SIZE 128`, `HOK_COLOR_OFFSET 25`, `HOK_HEADER_SIZE 25`, `HOK_ZONES_TEXT_LEN 28`.
- `struct hok_rgb { u8 r, g, b; };`
- `int hok_parse_zones(const char *buf, size_t len, struct hok_rgb out[HOK_ZONES]);` returns 0 or `-EINVAL`. One color fills all four zones; `out` is left untouched on error.
- `int hok_format_zones(char *buf, size_t size, const struct hok_rgb zones[HOK_ZONES]);` returns 28 after writing the uppercase line plus NUL; returns `-EINVAL` if `size < 29`.
- `int hok_parse_bool(const char *buf, size_t len);` returns 0 or 1 for `0` or `1` with an optional single `\n`, else `-EINVAL`.
- `void hok_table_get_zones(const u8 *table, struct hok_rgb out[HOK_ZONES]);` and `void hok_table_set_zones(u8 *table, const struct hok_rgb zones[HOK_ZONES]);`. Zone i is bytes 25+3i, 26+3i and 27+3i (R, G, B).
- `bool hok_header_equal(const u8 *a, const u8 *b);` compares bytes 0–24.
- `struct hok_info { const char *board; u8 kbd_type; bool readonly; unsigned int min_interval_ms; u32 max_writes_per_hour; u32 writes_last_hour; u64 writes_total; bool budget_exhausted; int last_error; u8 backlight_raw; bool header_changed; u8 header[HOK_HEADER_SIZE]; };`
- `int hok_format_info(char *buf, size_t size, const struct hok_info *info);` returns the length of the exact `info` line from Global Constraints, or `-EINVAL` if it does not fit.

- [ ] **Step 1: Write the failing tests** in `tests/core_test.c`. Use plain C: a `CHECK(expr)` macro counts failures, and `main` returns non-zero if any check failed. One function per test:
  - **`ZonesParserAcceptsTheDaemonsLine`:**
    - `"FF0000 00FF00 0000FF FFFFFF\n"` → 0 and `{255,0,0} {0,255,0} {0,0,255} {255,255,255}`;
    - the same line without `\n`, and the lowercase line, give the same result;
    - `"40E0D0\n"` fills all four zones.
  - **`ZonesParserIsStrict`:** every case gives `-EINVAL` and leaves `out` unchanged:
    - empty and malformed lines: `""`, `"\n"`, `"FF0000\n\n"`, `"FF0000\r\n"`;
    - wrong color counts: `"FF0000 00FF00\n"`, `"FF0000 00FF00 0000FF\n"`, five colors;
    - bad spacing: `"FF0000  00FF00 0000FF FFFFFF\n"` (two spaces), `"FF0000 00FF00 0000FF FFFFFF \n"`, `" FF0000\n"`;
    - bad colors: `"#FF0000\n"`, `"FF000\n"`, `"FF00000\n"`, `"GG0000\n"`;
    - `"FF\0" "000\n"` (length 7, with an embedded NUL);
    - 4096 bytes of `'F'`.
  - **`ZonesFormatIsTheDaemonsLine`:** three zones `{0x40,0xE0,0xD0}` plus `{0x00,0x99,0xED}` give `"40E0D0 40E0D0 40E0D0 0099ED\n"` and return 28; a buffer of size 28 gives `-EINVAL`.
  - **`BoolParser`:** `"0"`, `"1"`, `"0\n"`, `"1\n"` give 0/1; `"2"`, `"10"`, `" 1"`, `"1 "`, `""` and `"true"` give `-EINVAL`.
  - **`PatchTouchesOnlyZoneBytes`:** fill a table with `table[i] = (u8)(i * 7 + 3)` and set zones. Bytes 0–24 and 37–127 are unchanged, bytes 25–36 hold the zones, and `hok_table_get_zones` round-trips.
  - **`HeaderCompare`:** equal tables → true; a difference in byte 24 → false; a difference in byte 25 → true.
  - **`InfoLineIsExact`:**
    - Input `{ "8A4E", 2, true, 50, 40000, 3, 12, false, 0, 0xE4, false, header 0x00…0x18 }` gives exactly `"board=8A4E kbd_type=2 readonly=1 min_interval_ms=50 max_writes_per_hour=40000 writes_last_hour=3 writes_total=12 budget_exhausted=0 last_error=0 backlight_raw=0xe4 header_changed=0 header=000102030405060708090a0b0c0d0e0f101112131415161718\n"`.
    - `last_error = -5` prints `last_error=-5`.
    - A 64-byte buffer gives `-EINVAL`.
- [ ] **Step 2: Run it to verify it fails.**
  Run: `cmake --build build && ctest --test-dir build -R hok-core --output-on-failure`
  Expected: FAIL (`core.c` does not exist yet).
- [ ] **Step 3: Implement `core.c`** in kernel style. From the libraries use only `snprintf`, `memcpy` and `memcmp`, which exist in both worlds; parse hex digits by hand.
- [ ] **Step 4: Run the gate.** Expected: ctest all green.
- [ ] **Step 5: Commit** `feat(kernel): module core — sysfs text formats and color table`.

### Task 2: Core — firmware protocol and guards

**Files:** Modify `kernel/hp-omen-kbd/core.h`, `core.c`, `tests/core_test.c`.

**Interfaces — Produces:**
- Constants: `HOK_SIGNATURE 0x55434553`, `HOK_CMD_GAMING 0x20008`, `HOK_CMD_BACKLIGHT 0x20009`, `HOK_Q_KBD_TYPE 0x2B`, `HOK_Q_COLOR_GET 0x02`, `HOK_Q_COLOR_SET 0x03`, `HOK_Q_BL_GET 0x04`, `HOK_Q_BL_SET 0x05`, `HOK_BL_ON 0xE4`, `HOK_BL_OFF 0x64`, `HOK_ARGS_SIZE 144`, `HOK_REPLY_HEADER 8`, `HOK_INTERVAL_MIN_MS 33`, `HOK_INTERVAL_MAX_MS 1000`, `HOK_BUDGET_SLOTS 60`.
- `size_t hok_build_args(u8 dst[HOK_ARGS_SIZE], u32 command, u32 query, const u8 *data, u32 datasize);` writes a little-endian header, copies the data and zeroes the rest (`datasize ≤ 128`); returns `HOK_ARGS_SIZE`.
- `int hok_method_id(size_t outsize);` follows the table in Global Constraints; above 4096 it returns `-EINVAL`.
- `int hok_bios_errno(u32 code);`: 0 → 0; 3 or 4 → `-EOPNOTSUPP`; anything else → `-EIO`.
- `int hok_parse_reply(const u8 *reply, size_t len, u8 *out, size_t outsize);` returns 0 after copying `outsize` bytes, else a negative errno. It never copies anything on error.
- `bool hok_backlight_on(u8 raw);` tests bit 7. `bool hok_kbd_type_supported(u8 type);` accepts 1 and 2.
- `unsigned int hok_clamp_interval(unsigned int ms);` clamps to [33, 1000].
- `u64 hok_wait_ms(bool written, s64 last_ms, s64 now_ms, unsigned int interval_ms);` returns:
  - 0 if nothing was written yet, or if `now - last ≥ interval`;
  - otherwise `interval - elapsed`;
  - the full `interval` if the clock went backwards.
- Budget:
  - `struct hok_budget { u32 count[HOK_BUDGET_SLOTS]; s64 minute[HOK_BUDGET_SLOTS]; };`
  - `void hok_budget_init(struct hok_budget *b);`
  - `u32 hok_budget_used(const struct hok_budget *b, s64 now_s);`
  - `bool hok_budget_take(struct hok_budget *b, s64 now_s, u32 max_per_hour);`
  
  Slot `minute % 60` holds the count for its absolute minute. "Used" is the sum of slots whose minute falls in `(now_min − 60, now_min]`. A take fails when used ≥ max.

- [ ] **Step 1: Write the failing tests:**
  - **`ArgsLayout`:**
    - `hok_build_args(dst, 0x20009, 0x03, table, 128)` returns 144. `dst[0..15]` is `53 45 43 55 09 00 02 00 03 00 00 00 80 00 00 00`, and `dst[16..143]` is the table.
    - Keyboard-type args (`0x20008`, `0x2B`, `{0}`, 1): bytes 4–7 are `08 00 02 00`, bytes 8–11 are `2B 00 00 00`, bytes 12–15 are `01 00 00 00`, and bytes 16–143 are all zero.
  - **`MethodIdFollowsReplySize`:** 0→1, 1→2, 4→2, 5→3, 128→3, 129→4, 1024→4, 1025→5, 4096→5, 4097→`-EINVAL`.
  - **`ReplyParsingNeverReturnsPositive`:**
    - The OK reply `{0,0,0,0, 0,0,0,0, 0xE4}` with outsize 1 gives 0 and copies `0xE4`.
    - A 136-byte reply with outsize 128 gives 0 and copies the data.
    - Codes 3 and 4 give `-EOPNOTSUPP`; codes 2, 5 and 0xFF give `-EIO`.
    - Length 7 gives `-EIO`; length 8 with outsize 1 gives `-EIO`.
    - On every error the output still holds its sentinel `0xAA`, and every return value is ≤ 0.
  - **`BacklightDecodesBitSeven`:** `0xE4`, `0xB2` and `0x80` are on; `0x64`, `0x32` and `0x00` are off.
  - **`KeyboardTypeGate`:** 1 and 2 → true; 0, 3, 4, 5 and 255 → false.
  - **`IntervalClampAndWait`:**
    - Clamp: 0→33, 33→33, 50→50, 1000→1000, 100000→1000.
    - Wait: nothing written → 0; last 1000, now 1020, interval 50 → 30; now 1050 → 0; now 990 (clock went backwards) → 50.
  - **`BudgetIsARollingHour`:**
    - With max 3 at t=0, three takes succeed and the fourth fails. At t=3599 (minute 59) takes still fail. At t=3600 (minute 60, minute 0 has left the window) a take succeeds.
    - With max 60 and one take per minute for 60 minutes, all succeed. A 61st take in minute 59 fails; in minute 60 it succeeds.
    - Max 0 always fails.
    - After a jump to t=10⁹, used is 0.
    - With the clock going backwards (t=3600, then t=0), no window ever holds more than max, and nothing crashes.
- [ ] **Step 2: Run them to verify they fail.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run the gate.** Expected: green.
- [ ] **Step 5: Commit** `feat(kernel): module core — firmware protocol, write budget and rate limit`.

### Task 3: Contract tests with the daemon and `doctor`

**Files:** Create `tests/test_kernel_contract.cpp`; Modify `CMakeLists.txt` (link `hokcore` into `omen-kbd-tests`).

**Interfaces — Consumes:**
- Task 1's `hok_parse_zones`, `hok_parse_bool` and `hok_format_info`.
- `omenkbd::sysfs::open_hp_omen_kbd(sys_root)` (`src/backend/sysfs.hpp`).
- `run_checks(const DoctorEnv &)` (`src/cli/doctor.hpp`; fields `sys_root`, `proc_root`, `dkms_root`, `hypr_dir`, `theme_dir`, `socket`).

- [ ] **Step 1: Write the failing tests** (GoogleTest, `#include "core.h"`):
  - **`TEST(KernelContract, DaemonZonesLineIsAccepted)`:** create fake `<tmp>/devices/platform/hp-omen-kbd/{zones,backlight}` files. Call `write_zones({40E0D0, 40E0D0, 40E0D0, 0099ED})` through the daemon backend. Passing the file's bytes to `hok_parse_zones` returns 0 with those colors in that order.
  - **`TEST(KernelContract, DaemonBacklightLineIsAccepted)`:** `set_backlight(true)` and `set_backlight(false)` write a file that `hok_parse_bool` reads as 1 and 0.
  - **`TEST(KernelContract, DoctorReadsTheModulesInfoLine)`:** write `hok_format_info` output with `readonly = true` to `<tmp>/devices/platform/hp-omen-kbd/info`. `run_checks` (other roots set to empty temp dirs) reports the Warn containing `loaded with readonly=1`. With `readonly = false` it reports the Ok `hp-omen-kbd accepts writes (readonly=0)`.
- [ ] **Step 2: Run them to verify they fail.** Expected: FAIL at compile or link time.
- [ ] **Step 3: Wire CMake.**
- [ ] **Step 4: Run the gate.** Expected: green.
- [ ] **Step 5: Commit** `test: module text formats match the daemon and doctor`.

### Task 4: Module skeleton, load gate and the VM harness

**Files:** Create in `kernel/hp-omen-kbd/`:
- `main.c`;
- `Kbuild` (`obj-m := hp_omen_kbd.o`, `hp_omen_kbd-y := main.o core.o`);
- `Makefile`, with targets `all` (kbuild `modules` against `KDIR ?= /usr/lib/modules/$(shell uname -r)/build`), `check`, `vmtest-gate`, `vmtest` and `clean`;
- `tests/check-module.sh`, `tests/vm/init.c`, `tests/vm/run.sh`;
- scenarios `tests/vm/gate-plain.{scenario,expected}`, `gate-board-no-wmi`, `gate-board-substring`.

**Interfaces — Produces:**
- **The module, read-only at this stage.** It has:
  - the parameters, the load gate, the keyboard-type probe, and the boot GET of the table and backlight, followed by the probe log line and hex dump;
  - a platform driver `"hp-omen-kbd"`, whose PM ops are wired in Task 6;
  - a platform device whose `dev.groups` hold `zones` (0444 show, from the cache), `backlight` (0444 show, with the 200 ms cache) and `info` (0444).
  
  Exit unregisters both.
- **The call wrapper** `static int hok_call(u32 command, u32 query, const void *in, u32 insize, void *out, u32 outsize)`:
  - builds kzalloc'd args with `hok_build_args`;
  - calls `wmi_evaluate_method(guid, 0, hok_method_id(outsize), …)`;
  - an ACPI status other than `AE_OK`, or an object other than `ACPI_TYPE_BUFFER`, gives `-EIO`; otherwise `hok_parse_reply` decides;
  - records `last_error`, asserts `lockdep_assert_held`, and frees the ACPI output.
- **The VM scenario language,** run by `tests/vm/init.c` as a static PID 1.
  - Setup: it mounts devtmpfs, proc and sysfs, reopens `/dev/console`, and loads `/wmi.ko.zst` with `MODULE_INIT_COMPRESSED_FILE`.
  - Then it runs `/scenario` line by line and powers off.
  - Each command prints exactly one `VM …` line; errors print as `strerrorname_np` names such as `ENODEV`. `<attr>` paths are relative to `/sys/devices/platform/hp-omen-kbd/`.
  
  | Command | Prints |
  |---|---|
  | `load <params…>` | `VM load: ok\|<ERR>` (`finit_module` of `/hp_omen_kbd.ko`) |
  | `unload` | `VM unload: ok\|<ERR>` (`delete_module("hp_omen_kbd", O_NONBLOCK)`) |
  | `read <attr>` | `VM read <attr>: <content without the final \n>\|<ERR>` |
  | `write <attr> <text>` (`\n` escape allowed) | `VM write <attr>: ok\|<ERR>` |
  | `info <key>` | `VM info <key>=<value>` |
  | `slow-write <attr> <min_ms> <text>` | `VM slow-write <attr>: ok` if it succeeded and took ≥ min_ms, else `VM slow-write <attr>: <ERR\|fast N ms>` |
  | `suspend` | `VM suspend: ok\|<ERR>` (writes `devices` to `/sys/power/pm_test`, then `mem` to `/sys/power/state`; returns after about 5 s) |
  | `sleep <ms>` | nothing |
  | `kmsg <substring>` | `VM kmsg: <message>` for each matching kernel message (text after the `;`, including the `hp_omen_kbd: ` prefix) |
  | `kmsg-clean` | `VM kmsg-clean: yes`, or one `VM kmsg-dirty: <message>` per line containing `BUG:`, `Oops`, `WARNING:`, `Call Trace` or `general protection` |
- **`tests/vm/run.sh <scenario>`:**
  - It refuses to run unless the version in `file -bL /boot/vmlinuz-linux` equals `uname -r` and the module's vermagic matches.
  - It builds `out/<scenario>/initramfs.cpio` with `bsdtar --format=newc`. Contents: `init`, `wmi.ko.zst` (from `/usr/lib/modules/$(uname -r)/kernel/drivers/platform/wmi/`), `hp_omen_kbd.ko` and `scenario`.
  - It runs `timeout 120 qemu-system-x86_64 -enable-kvm -cpu host -m 256 -nographic -no-reboot -kernel /boot/vmlinuz-linux -initrd … -append 'console=ttyS0 panic=-1 loglevel=4' <extra>`. `<extra>` comes from the scenario's `# qemu:` header line, where `@SSDT@` stands for the compiled emulator.
  - It compares the `VM` lines with `<scenario>.expected` (`diff -u`) and exits 0 only on a match.
  
  These mechanics were proven on this machine on 2026-10-22 with a probe module:
  - KVM is accessible to the user;
  - the compressed `wmi.ko` loads;
  - `-smbios type=2,manufacturer=HP,product=8A4E` sets the DMI board fields;
  - a `pm_test` suspend returns after about 5.2 s.

- [ ] **Step 1: Write the failing checks.**
  - `tests/check-module.sh` asserts on `modinfo kernel/hp-omen-kbd/hp_omen_kbd.ko`: `license: GPL`, `depends: wmi`, a vermagic that starts with `uname -r`, `parm:` lines for `readonly`, `min_interval_ms`, `max_writes_per_hour` and `restore_on_unload`, and no `alias:` line.
  - The same script checks the source statically, for the K6 property: `main.c` assigns `dev.groups` before `platform_device_add`, and never calls `sysfs_create_group(s)` or sets a driver `.dev_groups`.
  - **`gate-plain`** (no QEMU extras): `load` → `VM load: ENODEV`; `kmsg board not on the allowlist` → `VM kmsg: hp_omen_kbd: board not on the allowlist (only HP 8A4E)`; `kmsg-clean`.
  - **`gate-board-no-wmi`** (`-smbios type=2,manufacturer=HP,product=8A4E`): `ENODEV`, the "HP BIOS WMI interface … not found" line, and a clean log.
  - **`gate-board-substring`** (`product=18A4E`): `ENODEV` and the allowlist line. This proves the match is exact, not a substring match.
- [ ] **Step 2: Run them to verify they fail.**
  Run: `make -C kernel/hp-omen-kbd check vmtest-gate`
  Expected: FAIL (there is no module yet).
- [ ] **Step 3: Implement** the module (per Interfaces above and Global Constraints), the Makefile, `init.c` and `run.sh`. Every init failure unwinds in reverse order.
- [ ] **Step 4: Run the gates.** Expected: `make -C kernel/hp-omen-kbd check vmtest-gate` green, and ctest still green.
- [ ] **Step 5: Commit** `feat(kernel): hp_omen_kbd load gate, read-only sysfs and VM harness`.

### Task 5: Firmware emulator (needs `acpica`)

**Run this task only if the user accepted the emulator and installed `acpica`.** Otherwise:
- ledger a ruling and skip it;
- in Task 6, gate on `check vmtest-gate` only. The `rw-*` scenarios are still written and wait for `iasl`; P4's first load with `readonly=1` still guards the hardware.

The emulator checks the module's logic and its kernel integration (locking, sysfs, teardown). It does not check firmware compatibility; that stays P4's job.

**Files:**
- Create: `kernel/hp-omen-kbd/tests/vm/hp-bios-wmi.asl`; scenarios `emu-basic`, `emu-kbd-type-3`, `emu-kbd-type-0`, `emu-kbd-query-fails`, `emu-short-reply`.
- Modify `Makefile`: `vmtest` compiles each variant with `iasl -p out/hp-bios-wmi[-<variant>] [-D <SYMBOL>]`; without `iasl` it fails with `install acpica for the emulator scenarios`.
- Modify `run.sh`: a `# ssdt: <SYMBOL>` header line picks the variant.

**Emulator behaviour.** It is an SSDT loaded with `-acpitable file=@SSDT@`, together with `-smbios type=2,manufacturer=HP,product=8A4E`.
- **Device:** `Device (\_SB.HPEM)` with `_HID "PNP0C14"`, `_UID "HPOMENEMU"` and a `_WDG` holding one 20-byte block:
  - GUID bytes `34 F0 B7 5F 63 2C E9 45 BE 91 3D 44 E2 C7 07 E4`;
  - object id `"AA"`, instance count 1, flags 0x02 (method).
  
  That makes the kernel call method `WMAA(instance, method_id, input)`.
- **Reply:** u32 `sigpass 0`, then u32 return code, then data. A signature other than "SECU" gives code 2 and no data; an unknown command or query gives code 3.
- **Calls:**
  - `0x20008/0x2B` → code `KBD_RET` (default 0) plus 1 byte `KBD_TYPE` (default 2).
  - `0x20009/0x02` → the 128-byte table. Variant `SHORT_GET` returns only 4 bytes in total.
  - `0x20009/0x03` → code `SET_RET` (default 0). On 0 the table becomes the input data. The reply carries 128 data bytes.
  - `0x20009/0x04` → 4 bytes `BL 00 00 00`.
  - `0x20009/0x05` → `BL` becomes data[0]; the reply carries 1 data byte.
- **Initial state:**
  - table bytes 0–24 = `0x01…0x19`;
  - bytes 25–36 = `40E0D0 40E0D0 40E0D0 0099ED` (the P1 baseline in hardware order);
  - bytes 37–127 = `0xA5`;
  - `BL = 0xE4`.
- **Variants** (via `iasl -D`): `KBD_TYPE_3`, `KBD_TYPE_0`, `KBD_RET_3`, `SHORT_GET`, plus two used in Task 6: `SET_RET_5`, and `HEADER_DRIFT` (after every accepted SET, table byte 0 goes up by 1).

- [ ] **Step 1: Write the failing scenarios.** The first one asserts that the VM sees the GUID: a broken `_WDG` must fail the test, not quietly pass as `ENODEV`.
  - **`emu-basic`**, in order:
    - `load` → `ok`;
    - `info kbd_type` → `2`, `info readonly` → `1`;
    - `info header` → `0102030405060708090a0b0c0d0e0f10111213141516171819`;
    - `read zones` → `40E0D0 40E0D0 40E0D0 0099ED`;
    - `read backlight` → `1`, `info backlight_raw` → `0xe4`;
    - `kmsg keyboard type 2` matches the probe line;
    - `unload` → `ok`, then `kmsg-clean`.
  - **`emu-kbd-type-3`** and **`emu-kbd-type-0`:** `load` → `ENODEV`, with the kmsg line "keyboard type 3 is not a four-zone keyboard; not binding" (type 0 for the second).
  - **`emu-kbd-query-fails`** (`KBD_RET_3`): `ENODEV`, with "keyboard type query failed (-95); not binding".
  - **`emu-short-reply`** (`SHORT_GET`): `load` → `EIO`, with a clean log.
- [ ] **Step 2: Run them to verify they fail.** Expected: FAIL (there is no SSDT yet).
- [ ] **Step 3: Write the ASL.** Expected: `iasl` compiles it with no warnings.
- [ ] **Step 4: Run the gates.** Expected: `make -C kernel/hp-omen-kbd check vmtest` green.
- [ ] **Step 5: Commit** `test(kernel): HP BIOS WMI emulator for VM tests`.

### Task 6: Write path, resume re-apply and unload restore

**Files:** Modify `kernel/hp-omen-kbd/main.c`. Create the scenarios `rw-readonly`, `rw-invalid`, `rw-write-and-reload`, `rw-header-drift`, `rw-restore-on-unload`, `rw-off-keeps-colors`, `rw-budget`, `rw-interval`, `rw-interval-clamp`, `rw-firmware-error`, `rw-resume-reapply` and `rw-resume-nothing-written`.

**Interfaces — Produces:**
- `zones` and `backlight` become 0644, with the write path from Global Constraints.
- `.pm = pm_sleep_ptr(&ops)`. `.resume` and `.restore` schedule the re-apply work (`HOK_RESUME_DELAY_MS 1000`) when anything was written.
- The exit order is the unload order from Global Constraints.

- [ ] **Step 1: Write the failing scenarios.** All run with the emulator and the HP/8A4E SMBIOS.
  - **`rw-readonly`:**
    - `load` → ok;
    - `write zones FF0000 00FF00 0000FF FFFFFF\n` → `EPERM`; `write backlight 0\n` → `EPERM`;
    - `write zones nonsense\n` → `EINVAL` (parsing comes first);
    - `info writes_total` → `0`.
  - **`rw-invalid`** (`readonly=0`):
    - `write zones FF0000 00FF00\n` → `EINVAL`; `write zones #FF0000\n` → `EINVAL`;
    - `write zones ff0000 00ff00 0000ff ffffff\n` → ok, then `read zones` → `FF0000 00FF00 0000FF FFFFFF`.
  - **`rw-write-and-reload`** (`readonly=0 restore_on_unload=0`):
    - write `112233 445566 778899 AABBCC\n` → ok; `info writes_total` → `1`;
    - `unload`, then `load` → ok;
    - `read zones` → `112233 445566 778899 AABBCC`;
    - `info header` → the emulator pattern; `info header_changed` → `0`.
  - **`rw-header-drift`** (`HEADER_DRIFT`, `readonly=0 restore_on_unload=0`):
    - two zone writes → ok, ok; `info header_changed` → `1`;
    - `kmsg differs from the boot copy` → exactly one line;
    - `unload`, then `load`;
    - `info header` → `0302030405060708090a0b0c0d0e0f10111213141516171819`. Byte 0 reads 03 because the drifted 02 was written back as read; restoring the boot copy would have given 02.
  - **`rw-restore-on-unload`** (`readonly=0`):
    - write → ok, then `unload`; `kmsg restored the boot colors` finds the line;
    - `load`; `read zones` → `40E0D0 40E0D0 40E0D0 0099ED`.
  - **`rw-off-keeps-colors`** (`readonly=0 restore_on_unload=0`):
    - write zones `112233 445566 778899 AABBCC\n`;
    - `write backlight 0\n` → ok; `read backlight` → `0`;
    - `unload`, then `load`;
    - `read zones` → `112233 …`; `read backlight` → `0`; `info backlight_raw` → `0x64`.
  - **`rw-budget`** (`readonly=0 max_writes_per_hour=2`):
    - two writes → ok; a third → `EBUSY`;
    - `info budget_exhausted` → `1`; `info writes_last_hour` → `2`.
  - **`rw-interval`** (`readonly=0 min_interval_ms=400`): one write, then `slow-write zones 380 …` → ok.
  - **`rw-interval-clamp`:** `load min_interval_ms=1` gives `info min_interval_ms` → `33`. After `unload`, `load min_interval_ms=100000` gives `1000`.
  - **`rw-firmware-error`** (`SET_RET_5`, `readonly=0`): write → `EIO`; `info last_error` → `-5`; `info writes_total` → `0`.
  - **`rw-resume-reapply`** (`readonly=0`):
    - one zone write and `write backlight 0\n`; `info writes_total` → `2`;
    - `suspend` → ok, then `sleep 2000`;
    - `info writes_total` → `4` (zones and backlight re-applied); `info last_error` → `0`; `read backlight` → `0`;
    - `kmsg-clean`.
  - **`rw-resume-nothing-written`** (`readonly=0`): `suspend`, then `sleep 1500`; `info writes_total` → `0`.
- [ ] **Step 2: Run them to verify they fail.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run the gates.** Expected: `make -C kernel/hp-omen-kbd check vmtest` and ctest green.
- [ ] **Step 5: Commit** `feat(kernel): hp_omen_kbd write path, resume re-apply and unload restore`.

### Task 7: DKMS and packaging, built as the user

**Files:** Create `kernel/hp-omen-kbd/dkms.conf`, `kernel/hp-omen-kbd/tests/check-packaging.sh`, `packaging/udev/99-omen-kbd.rules` (the spec's name), `packaging/sysusers/omen-kbd.conf`, `packaging/modules-load/hp-omen-kbd.conf`, `packaging/arch/PKGBUILD`, `packaging/arch/hp-omen-kbd-dkms.install`.

- **`dkms.conf`:**
  - `PACKAGE_NAME="hp-omen-kbd"`, `PACKAGE_VERSION="0.1.0"`;
  - `BUILT_MODULE_NAME[0]="hp_omen_kbd"`, `DEST_MODULE_LOCATION[0]="/updates/dkms"`;
  - `AUTOINSTALL="yes"`;
  - `MAKE[0]="make -C ${kernel_source_dir} M=${dkms_tree}/${PACKAGE_NAME}/${PACKAGE_VERSION}/build modules"`.
  
  There is no `CLEAN` line: DKMS 3.4 reports it as deprecated.
- **udev:** `ACTION=="add|change", SUBSYSTEM=="platform", KERNEL=="hp-omen-kbd", RUN+="/usr/bin/chgrp omen-kbd /sys%p/zones /sys%p/backlight", RUN+="/usr/bin/chmod 0664 /sys%p/zones /sys%p/backlight"`.
- **sysusers:** `g omen-kbd -`. **modules-load:** `hp_omen_kbd`.
- **PKGBUILD:** `pkgname=hp-omen-kbd-dkms`, `pkgver=0.1.0` (equal to `PACKAGE_VERSION`), `arch=(any)`, `license=(GPL-2.0-only)`, `depends=(dkms)`.
  - `package()` installs, from `$startdir/../..`:
    - `Kbuild Makefile dkms.conf core.c core.h main.c` (no tests) to `/usr/src/hp-omen-kbd-$pkgver/`;
    - the udev rule to `/usr/lib/udev/rules.d/`;
    - the sysusers file to `/usr/lib/sysusers.d/`;
    - the modules-load file to `/usr/lib/modules-load.d/`.
  - The `.install` file's `post_install` prints two things: join the group (`sudo usermod -aG omen-kbd $USER`, then log in again), and that the module loads at the next boot (or now with `sudo modprobe hp_omen_kbd`).
  
  Installing the package is P5, after P4's sign-off.

- [ ] **Step 1: Write the checks first** into `tests/check-packaging.sh`, and run them. Expected: FAIL.
  - **DKMS build as the user into a temp tree:**
    - copy the module into `$T/src/hp-omen-kbd-0.1.0/`;
    - run `dkms add -m hp-omen-kbd -v 0.1.0 --sourcetree $T/src --dkmstree $T/tree`, then `dkms build … -k $(uname -r)`;
    - expect exit 0 and `Building module(s)... done.`;
    - `$T/tree/hp-omen-kbd/0.1.0/$(uname -r)/x86_64/module/hp_omen_kbd.ko.zst` exists, and its `modinfo` shows `depends: wmi` and no `alias:`.
    
    This was proven with a probe module on 2026-10-22. DKMS also prints `Warning: Failed to sign module`, because `/var/lib/dkms/mok.key` is root-only. That is expected for a user build; this kernel does not enforce signatures.
  - **udev:** `udevadm verify packaging/udev/99-omen-kbd.rules` prints `Success: 1` and `Fail: 0`, and exits 0.
  - **sysusers:** `systemd-sysusers --dry-run packaging/sysusers/omen-kbd.conf` prints `Creating group 'omen-kbd' …` and `Would write /etc/group…`, and exits 0.
  
  Both commands were proven as the user on 2026-10-22 with the exact rule and sysusers lines below.
  - **package:**
    - in `packaging/arch`, `BUILDDIR=$T/b PKGDEST=$T/p makepkg -f --nodeps` builds the package;
    - `bsdtar -tf` lists the files above;
    - `namcap` on the package reports no errors.
  - **versions:** `PACKAGE_VERSION` in `dkms.conf` equals `pkgver`.
- [ ] **Step 2: Write the files.**
- [ ] **Step 3: Run the checks.** Expected: green.
- [ ] **Step 4: Commit** `build(kernel): DKMS, udev rule, sysusers and PKGBUILD for hp-omen-kbd`.

### Task 8: P4 runbook, recovery and README

**Files:** Create `docs/hardware-validation.md`, `docs/recovery.md`; Modify `README.md` (a kernel module section) and `docs/hardware-notes.md` (link the runbook).

- **`hardware-validation.md`** turns the spec's P4 protocol into a runbook.
  - Every step gives: what it proves, the exact commands, the expected output, what to record, and its stop conditions. The user runs anything with `sudo` in their own terminal, and each step needs their go-ahead.
  - **Loading:** from the kbuild output, `sudo insmod kernel/hp-omen-kbd/hp_omen_kbd.ko readonly=1` (after `make -C kernel/hp-omen-kbd`), then `dmesg | grep hp_omen_kbd`.
  - **Step 1:**
    - Reference values: zones `40E0D0 40E0D0 40E0D0 0099ED` and backlight `1` (the P1 baseline).
    - `kbd_type` must be 1 or 2.
    - **Stop** if the module refuses to load because the keyboard-type query fails or reports another type. No parameter works around the allowlist; the fix is a reviewed code change.
  - **Step 2:**
    - Reload with `readonly=0`.
    - Write back exactly what step 1 read (`echo "<read value>" | sudo tee …/zones`).
    - The readback is identical, `info writes_total=1` and `info header_changed=0`.
  - **Step 4:** record `info backlight_raw` at each Fn-key level (proving the bit-7 decode), and whether writing colors while the backlight is off turns it on.
  - **Step 5:** with the daemon running, each resume gives two writes (the module's re-apply and the daemon's forced rewrite). That is expected, not a bug.
  - **Step 7 (rate test):** write at 5 and then 10 writes per second for 60 s each with a shell loop; record CPU (`pidstat`) and `dmesg`.
  - **Sign-off:** after all eight steps pass, a reviewed commit flips `readonly`'s default to 0. Only then does P5 install the package.
- **`recovery.md`** covers:
  - writing known colors: `echo "40E0D0 40E0D0 40E0D0 0099ED" | sudo tee …/zones`;
  - turning the backlight on with `backlight 1`;
  - `sudo rmmod hp_omen_kbd`, which restores the boot colors;
  - a reboot, which clears everything the module did except colors the firmware kept;
  - `modprobe.blacklist=hp_omen_kbd` on the kernel command line, once the module is installed;
  - BIOS defaults.
- **README:** a "Kernel module (P3)" section with the build, check and VM commands, the rule that the module is never loaded on the host before P4, and links to the two documents.

- [ ] **Step 1: Write the docs.** Every command in them is either run by a P3 check above or marked "P4, on hardware, with your go-ahead".
- [ ] **Step 2: Run the gates.** Expected: ctest and `make -C kernel/hp-omen-kbd check` still green.
- [ ] **Step 3: Commit** `docs: P4 hardware-validation runbook and recovery`.
