# omen-kbd Daemon + CLI Implementation Plan (spec phase P2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the `omen-kbd` Rust binary (user daemon + CLI) with every feature of the spec working end-to-end against a **mock backend**, plus the two real sysfs backends tested against fake sysfs trees — no hardware is touched.

**Architecture:** One binary. Pure `core` (colors, zones, palette, effects, rules, compositor) → `daemon::model` (pure state machine) → `daemon::run` (tokio loop: IPC, sources, timers) → `daemon::writer` (owns a `Backend`, coalesces with a `watch` channel). CLI talks to the daemon over a Unix socket with JSON lines.

**Tech Stack:** Rust 1.99 / edition 2024, tokio, zbus 5 (tokio), serde/serde_json/toml, clap 4 (derive), inotify, thiserror/anyhow, log/env_logger, libc; dev: tempfile.

**Spec:** `docs/superpowers/specs/2026-10-21-omen-kbd-design.md` (read it first; this plan implements its Component 2, phase P2).

**Out of scope here (separate plans):** the kernel module, udev/sysusers/modules-load files and PKGBUILD (P3/P4 plan); migration (P1 is a user-run runbook in the spec); switching to the real backend and applying dotfiles patches (P5 plan).

## Global Constraints

- Never touch hardware: tests and demos use `--backend mock` or fake sysfs trees under a temp dir (`sys_root`). Nothing in this plan writes under `/sys`.
- The daemon runs as the user, never root; no evdev, no `input` group.
- The tool never writes `~/.config/omen-kbd/config.toml` or anything under `~/.config/hypr`; user-config changes ship as snippets in `packaging/snippets/`.
- Socket: `$XDG_RUNTIME_DIR/omen-kbd.sock`, mode `0600`, newline-delimited JSON.
- State: `$XDG_STATE_HOME/omen-kbd/state.json` (default `~/.local/state/omen-kbd/state.json`), atomic tmp+rename, debounced 1 s.
- Logical zone order everywhere in software: `[left, middle, right, wasd]`. Default zone map logical→hardware: left→2, middle→1, right→0, wasd→3.
- hp-omen-kbd sysfs: `<sys_root>/devices/platform/hp-omen-kbd/{zones,backlight,info}`; `zones` takes `RRGGBB RRGGBB RRGGBB RRGGBB\n` in **hardware** order; errno EBUSY = hourly budget exhausted, EPERM = `readonly=1`.
- mainline-led sysfs: `<sys_root>/class/leds/hp::kbd_zoned_backlight-{right,center,left,wasd}/{multi_intensity,brightness}` = hardware index 0,1,2,3.
- Backend selection `auto` picks a real backend only; `mock` is used only when explicitly requested; both real backends present → refuse to start.
- Effects: `enabled = false` by default; `fps` default 10, allowed 1..=20. Hint expiry default 15 s; Lua hold delay default 250 ms.
- Theme colors default `vivid` (rule pinned in Task 2); `exact` turns it off.
- Every CLI command reports the real outcome: exit 0 ok, 1 daemon/hardware error (message on stderr), 2 daemon unreachable.
- Install target for later phases: `cargo install --path . --root ~/.local` (→ `~/.local/bin/omen-kbd`, which is on PATH; `~/.cargo/bin` is not).
- Every task ends green on: `cargo test`, `cargo clippy --all-targets -- -D warnings`, `cargo fmt --check`.

## Review Focus

- Reloading a config with a syntax error → the previous config stays active and `omen-kbd reload` exits 1 with the parse error (test `bad_reload_keeps_previous_config`, Task 13).
- A `theme:` color missing from the current theme → that zone shows white, a warning appears in `status`, nothing crashes (test `unknown_theme_color_warns_and_uses_white`, Task 12).
- A stale socket left by a crashed daemon, or a second daemon started → stale socket is replaced; a second instance exits with "already running" (tests `bind_replaces_stale_socket`, `bind_refuses_when_running`, Task 11).
- Resume from suspend with an unchanged frame → the frame is written again once, because firmware may have reset it (test `resume_forces_identical_rewrite`, Task 13).
- Hyprland restarts / socket2 closes while a submap is active → submap hint cleared and the reader reconnects (test `socket2_disconnect_clears_submap_and_reconnects`, Task 14).

---

### Task 1: Spike — Hyprland Lua modifier hook (user-assisted, software only)

Proves the mode-colors mechanism before code depends on it. Needs the user at the keyboard; if they are unavailable, continue with Task 2 and come back — only Task 17 depends on the result.

**Files:**
- Create: `spikes/kbdlight-spike.lua`
- Create: `spikes/RESULTS.md`

- [ ] **Step 1: Write the spike module.** Same algorithm `gen-hyprland` will emit (Task 17), but logging via `logger` instead of calling `omen-kbd`:

```lua
-- kbdlight spike: log held-modifier sets (no omen-kbd needed)
local MODS = { [133]="super", [134]="super", [37]="ctrl", [105]="ctrl",
               [64]="alt", [108]="alt", [50]="shift", [62]="shift" }
local ORDER = { "super", "ctrl", "alt", "shift" }
local WANTED = { ["super"]=true, ["super+shift"]=true, ["ctrl"]=true }
local DELAY_MS = 250
local held, sent, pending = {}, false, nil
local function log(msg) hl.exec_cmd("logger -t omen-kbd-spike '" .. msg .. "'") end
local function current_set()
  local present, parts = {}, {}
  for code in pairs(held) do present[MODS[code]] = true end
  for _, m in ipairs(ORDER) do if present[m] then parts[#parts + 1] = m end end
  return table.concat(parts, "+")
end
local timer = hl.timer(function()
  if pending ~= nil and pending == current_set() then log("hint " .. pending); sent = true end
end, { timeout = DELAY_MS, type = "oneshot" })
timer:set_enabled(false)
hl.on("input.keyboard.key", function(keycode, _time, state)
  if state == 1 then log("raw press " .. keycode) end
  if MODS[keycode] == nil or state == 2 then return end
  if state == 1 then held[keycode] = true else held[keycode] = nil end
  local set = current_set()
  timer:set_enabled(false)
  pending = nil
  if WANTED[set] then
    pending = set
    timer:set_enabled(true)
  elseif sent then
    log("hint clear"); sent = false
  end
end)
log("spike loaded")
```

- [ ] **Step 2: Syntax check.** Run: `luac -p spikes/kbdlight-spike.lua` → no output.
- [ ] **Step 3: Load it live (ask the user first; undone by `hyprctl reload`).** Run: `hyprctl eval "dofile('$PWD/spikes/kbdlight-spike.lua')"` and in another terminal `journalctl -t omen-kbd-spike -f`. Expected: `spike loaded`.
- [ ] **Step 4: User performs the checks; record each result (pass/fail + log excerpt) in `spikes/RESULTS.md`:**
  1. Press Super_L once → `raw press 133` (confirms XKB codes).
  2. Hold Super ≥ 0.5 s → `hint super` once; release → `hint clear`.
  3. Super+Q quick chord → no `hint` line.
  4. Hold Super, add Shift → `hint super+shift` without an intermediate `hint clear`; release both → `hint clear`.
  5. Type `Hello` (Shift+h) → no `hint` line.
  6. Hold Super, switch workspace (Super+2), keep holding → hint stays; release → clear.
  7. Re-arming: tap Super twice within 250 ms then hold → exactly one `hint super` (confirms `set_enabled(true)` restarts the oneshot; if not, use `timer:set_timeout(DELAY_MS)` before enabling and re-test).
  8. Lock (`loginctl lock-session`) while holding Super, unlock → note whether a clear arrived (daemon expiry covers it if not).
- [ ] **Step 5: Unload.** Run: `hyprctl reload`. Expected: no further spike log lines on key presses.
- [ ] **Step 6: Decide.** If checks 2–5 pass, Task 17 emits this algorithm. If the event does not fire for modifiers, record it and Task 17 emits the fallback (bare-modkey binds with `{ non_consuming = true }` press and `{ release = true }` release, per spec).
- [ ] **Step 7: Commit.** `git add spikes && git commit -m "spike: Hyprland Lua modifier hook for mode colors"`

---

### Task 2: Crate scaffold + `core::color`

**Files:**
- Create: `Cargo.toml`, `.gitignore` (`/target`), `src/main.rs`, `src/lib.rs`, `src/core/mod.rs`, `src/core/color.rs`

**Interfaces:**
- Produces: `pub struct Rgb { pub r: u8, pub g: u8, pub b: u8 }` (`Copy, Eq, Debug, Serialize, Deserialize` as hex string) with:
  `const fn new(r,g,b)`, `const BLACK`, `const WHITE`, `fn parse(s: &str) -> Result<Rgb, ColorError>`, `fn to_hex(self) -> String` (uppercase `RRGGBB`, no `#`), `fn scale(self, pct: u8) -> Rgb`, `fn lerp(a: Rgb, b: Rgb, t: f64) -> Rgb`, `fn to_hsv(self) -> (f64, f64, f64)` (h in [0,1)), `fn from_hsv(h: f64, s: f64, v: f64) -> Rgb`, `fn vivid(self) -> Rgb`; `#[derive(thiserror::Error)] pub struct ColorError(pub String)`.

- [ ] **Step 1: `cargo init --name omen-kbd` (edition 2024) and add dependencies:**
  `cargo add tokio --features rt,rt-multi-thread,macros,net,io-util,time,sync,fs,signal`
  `cargo add zbus --no-default-features --features tokio`
  `cargo add clap --features derive` · `cargo add serde --features derive` · `cargo add serde_json toml anyhow thiserror log env_logger inotify libc futures-util`
  `cargo add --dev tempfile`
  `src/lib.rs` declares `pub mod core;` (later tasks add their modules); `src/main.rs` is a stub that calls `omen_kbd::cli::main()` once Task 16 exists — until then `fn main() {}`.
- [ ] **Step 2: Write failing tests in `src/core/color.rs`:**

```rust
#[test] fn parses_hex_with_and_without_hash() {
    assert_eq!(Rgb::parse("#cba6f7").unwrap(), Rgb::new(0xcb, 0xa6, 0xf7));
    assert_eq!(Rgb::parse("CBA6F7").unwrap(), Rgb::new(0xcb, 0xa6, 0xf7));
}
#[test] fn rejects_bad_hex() {
    for s in ["", "#fff", "#gggggg", "#1234567", "12345"] { assert!(Rgb::parse(s).is_err(), "{s}"); }
}
#[test] fn formats_uppercase_hex() { assert_eq!(Rgb::new(0xcb, 0xa6, 0xf7).to_hex(), "CBA6F7"); }
#[test] fn scales_with_rounding() {
    assert_eq!(Rgb::new(255, 128, 0).scale(50), Rgb::new(128, 64, 0));
    assert_eq!(Rgb::WHITE.scale(0), Rgb::BLACK);
    assert_eq!(Rgb::new(1, 2, 3).scale(100), Rgb::new(1, 2, 3));
}
#[test] fn lerp_endpoints_and_mid() {
    assert_eq!(Rgb::lerp(Rgb::BLACK, Rgb::WHITE, 0.0), Rgb::BLACK);
    assert_eq!(Rgb::lerp(Rgb::BLACK, Rgb::WHITE, 1.0), Rgb::WHITE);
    assert_eq!(Rgb::lerp(Rgb::BLACK, Rgb::WHITE, 0.5), Rgb::new(128, 128, 128));
}
#[test] fn hsv_roundtrip_within_one() {
    for c in [Rgb::new(203, 166, 247), Rgb::new(255, 0, 0), Rgb::new(17, 200, 90)] {
        let (h, s, v) = c.to_hsv();
        assert_close(Rgb::from_hsv(h, s, v), c, 1);
    }
}
#[test] fn vivid_boosts_saturation_keeps_hue() { assert_close(Rgb::new(0xcb, 0xa6, 0xf7).vivid(), Rgb::new(0x89, 0x26, 0xff), 1); }
#[test] fn vivid_leaves_greys() { assert_eq!(Rgb::new(128, 128, 128).vivid(), Rgb::new(128, 128, 128)); }
```
  (`assert_close(a, b, tol)` = per-channel `abs_diff <= tol`; put it in a `#[cfg(test)] pub fn` in `color.rs` so other modules' tests can use it.)
- [ ] **Step 3: Run** `cargo test color` → FAIL (not defined).
- [ ] **Step 4: Implement.** Rounding is `f64::round` (half away from zero). `scale` = `round(c * pct / 100)`. `vivid`: if `s < 0.1` return unchanged; else `s = max(s, 0.85)`, `v = 1.0`, keep `h`. Serde: serialize as `"#RRGGBB"`, deserialize via `parse`.
- [ ] **Step 5: Run** `cargo test color` → PASS; clippy + fmt clean.
- [ ] **Step 6: Commit** `feat(core): Rgb color type with parse/scale/lerp/hsv/vivid`.

---

### Task 3: `core::zones`

**Files:** Create `src/core/zones.rs`

**Interfaces:**
- Produces: `pub enum Zone { Left, Middle, Right, Wasd }` with `const ALL: [Zone; 4]`, `fn index(self) -> usize` (0..3 logical), `fn from_name(s: &str) -> Option<Zone>` (case-insensitive; `center` is an alias of `middle`), serde as lowercase name; `pub type Frame = [Rgb; 4]` (logical order); `pub struct ZoneMap { hw: [usize; 4] }` with `Default` (2,1,0,3), `fn new(left: usize, middle: usize, right: usize, wasd: usize) -> Result<ZoneMap, String>` (must be a permutation of 0..=3), `fn to_hw(&self, f: &Frame) -> [Rgb; 4]`; serde from `{ left, middle, right, wasd }` via `new`.

- [ ] **Step 1: Failing tests:**

```rust
const R: Rgb = Rgb::new(255,0,0); const G: Rgb = Rgb::new(0,255,0); const B: Rgb = Rgb::new(0,0,255);
#[test] fn default_map_matches_spec() {
    assert_eq!(ZoneMap::default().to_hw(&[R, G, B, Rgb::WHITE]), [B, G, R, Rgb::WHITE]);
}
#[test] fn rejects_non_permutation() {
    assert!(ZoneMap::new(0, 0, 1, 2).is_err());
    assert!(ZoneMap::new(0, 1, 2, 4).is_err());
}
#[test] fn zone_names() {
    assert_eq!(Zone::from_name("WASD"), Some(Zone::Wasd));
    assert_eq!(Zone::from_name("center"), Some(Zone::Middle));
    assert_eq!(Zone::from_name("top"), None);
}
#[test] fn zone_map_from_toml() {
    let m: ZoneMap = toml::from_str("left = 0\nmiddle = 1\nright = 2\nwasd = 3").unwrap();
    assert_eq!(m.to_hw(&[R, G, B, Rgb::WHITE]), [R, G, B, Rgb::WHITE]);
}
```
- [ ] **Step 2: Run** `cargo test zones` → FAIL. **Step 3: Implement** (make `Rgb::new` a `const fn` if Task 2 didn't). **Step 4: Run** → PASS. **Step 5: Commit** `feat(core): zones and logical→hardware zone map`.

---

### Task 4: `core::palette` (theme palette + color references)

**Files:** Create `src/core/palette.rs`

**Interfaces:**
- Consumes: `Rgb` (Task 2).
- Produces: `pub struct Palette` (`Default, Clone`) with `fn from_waybar_css(s: &str) -> Palette`, `fn from_theme_toml(s: &str) -> Result<Palette, String>` (format: `[palette]` table of `name = "#rrggbb"`), `fn load(theme_dir: &Path) -> Result<Palette, String>` (prefers `omen-kbd.toml`, else `waybar-colors.css`; error if neither), `fn get(&self, name: &str) -> Option<Rgb>`, `fn len(&self) -> usize`, `fn is_empty(&self) -> bool`;
  `pub enum ThemeColors { Vivid, Exact }` (serde lowercase, default Vivid);
  `pub enum ColorRef { Hex(Rgb), Theme(String) }` with `fn parse(s: &str) -> Result<ColorRef, ColorError>` (`theme:<name>` with non-empty name, else hex), serde as string, `fn resolve(&self, p: &Palette, mode: ThemeColors) -> Result<Rgb, String>` (Err = the missing name). Vivid applies to `Theme` refs only, never to explicit hex.

- [ ] **Step 1: Failing tests:**

```rust
const CSS: &str = "/* palette */\n@define-color mauve #cba6f7;\n@define-color blue #89b4fa;\n@define-color bad #zzz;\n";
#[test] fn parses_waybar_css_skipping_malformed() {
    let p = Palette::from_waybar_css(CSS);
    assert_eq!(p.get("mauve"), Some(Rgb::new(0xcb, 0xa6, 0xf7)));
    assert_eq!(p.len(), 2);
}
#[test] fn theme_toml_preferred_over_css() {
    let d = tempfile::tempdir().unwrap();
    std::fs::write(d.path().join("waybar-colors.css"), CSS).unwrap();
    std::fs::write(d.path().join("omen-kbd.toml"), "[palette]\nmauve = \"#ff00ff\"\n").unwrap();
    assert_eq!(Palette::load(d.path()).unwrap().get("mauve"), Some(Rgb::new(255, 0, 255)));
}
#[test] fn load_errors_when_no_palette_file() { assert!(Palette::load(tempfile::tempdir().unwrap().path()).is_err()); }
#[test] fn color_ref_parse() {
    assert_eq!(ColorRef::parse("theme:mauve").unwrap(), ColorRef::Theme("mauve".into()));
    assert_eq!(ColorRef::parse("#ff0000").unwrap(), ColorRef::Hex(Rgb::new(255, 0, 0)));
    assert!(ColorRef::parse("theme:").is_err());
}
#[test] fn resolve_vivid_exact_and_missing() {
    let p = Palette::from_waybar_css(CSS);
    let r = ColorRef::Theme("mauve".into());
    assert_eq!(r.resolve(&p, ThemeColors::Exact).unwrap(), Rgb::new(0xcb, 0xa6, 0xf7));
    assert_close(r.resolve(&p, ThemeColors::Vivid).unwrap(), Rgb::new(0x89, 0x26, 0xff), 1);
    assert_eq!(ColorRef::Hex(Rgb::new(0xcb,0xa6,0xf7)).resolve(&p, ThemeColors::Vivid).unwrap(), Rgb::new(0xcb,0xa6,0xf7));
    assert_eq!(ColorRef::Theme("nope".into()).resolve(&p, ThemeColors::Vivid), Err("nope".into()));
}
```
- [ ] **Step 2–4:** run → FAIL, implement, run → PASS. **Step 5: Commit** `feat(core): theme palette and color references`.

---

### Task 5: `core::effects`

**Files:** Create `src/core/effects.rs`

**Interfaces:**
- Consumes: `Rgb`, `Frame`.
- Produces: `pub enum Effect { Static, Breathe, Wave, Cycle, Gradient }` (serde lowercase; `fn is_animated(self) -> bool` false only for Static); `pub fn period_secs(speed: u8) -> f64`; `pub fn render(effect: Effect, base: &Frame, t: f64, speed: u8) -> Frame`; `pub fn pulse_factor(t: f64) -> f64`.

Pinned formulas (`P = period_secs(speed)`, speed clamped to 1..=100; position `pos`: left 0, middle 1, right 2, wasd 0 — except Gradient, where wasd is 3):
- `period_secs(s) = 0.5 + 9.5 * (100 - s) / 99` → 100 ⇒ 0.5 s, 1 ⇒ 10 s.
- Breathe: every zone `base.scale`-like multiply by `f = 0.55 + 0.45*cos(2π t/P)` (f in [0.1, 1], `f(0) = 1`), rounding per channel.
- Wave: `lerp(base[0], base[1], 0.5 - 0.5*cos(2π(t/P - pos/3)))`.
- Cycle: `from_hsv(frac(t/P + pos/3), 1.0, 1.0)`.
- Gradient: `x = 4t/P + pos`, `k = floor(x) mod 4`, `lerp(base[k], base[(k+1) mod 4], x - floor(x))`.
- `pulse_factor(t) = 0.6 + 0.4*cos(2π t)` (1 s period, range [0.2, 1]).

- [ ] **Step 1: Failing tests** (`base = [R, G, B, WHITE]`, `P = period_secs(50)`):

```rust
#[test] fn period_bounds() { assert_eq!(period_secs(100), 0.5); assert_eq!(period_secs(1), 10.0); }
#[test] fn static_is_identity() { assert_eq!(render(Effect::Static, &BASE, 3.7, 50), BASE); }
#[test] fn breathe_full_at_zero_dim_at_half() {
    assert_eq!(render(Effect::Breathe, &BASE, 0.0, 50), BASE);
    assert_eq!(render(Effect::Breathe, &BASE, P / 2.0, 50)[0], Rgb::new(26, 0, 0));
}
#[test] fn wave_phases() {
    let f = render(Effect::Wave, &BASE, 0.0, 50);
    assert_eq!(f[0], R); assert_eq!(f[3], R);
    assert_eq!(f[1], Rgb::lerp(R, G, 0.75));
}
#[test] fn cycle_starts_at_primaries() { assert_eq!(render(Effect::Cycle, &BASE, 0.0, 50), [R, G, B, R]); }
#[test] fn gradient_starts_at_base_and_blends() {
    assert_eq!(render(Effect::Gradient, &BASE, 0.0, 50), BASE);
    assert_eq!(render(Effect::Gradient, &BASE, P / 8.0, 50)[0], Rgb::lerp(R, G, 0.5));
}
#[test] fn pulse_range() { assert_eq!(pulse_factor(0.0), 1.0); assert!((pulse_factor(0.5) - 0.2).abs() < 1e-9); }
```
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `feat(core): zone-level software effects`.

---

### Task 6: `core::rules`

**Files:** Create `src/core/rules.rs`

**Interfaces:**
- Consumes: `Zone`, `ColorRef`.
- Produces:
  `#[derive(Serialize, Deserialize, Default, Clone, Debug, PartialEq)] pub struct Facts { pub power_profile: Option<String>, pub on_battery: Option<bool>, pub battery_pct: Option<f64>, pub cpu_temp_c: Option<f64>, pub gamemode: bool, pub locked: bool, pub idle: bool }`
  `pub struct Condition { power_profile: Option<String>, on_battery: Option<bool>, battery_below: Option<f64>, cpu_temp_above: Option<f64>, gamemode: Option<bool>, locked: Option<bool>, idle: Option<bool> }` (deny_unknown_fields; all present fields must hold; a needed fact that is `None` ⇒ false; empty ⇒ true)
  `pub struct Rule { pub name: String, pub when: Condition, pub profile: Option<String>, pub brightness_cap: Option<u8>, pub overlay: Option<OverlayCfg>, pub tint: Option<TintCfg> }` · `OverlayCfg { zones: Vec<Zone>, color: ColorRef, pulse: bool /*false*/ }` · `TintCfg { color: ColorRef, strength: f64 /*0..=1*/ }`
  `pub enum Overlay { Zones { zones: Vec<Zone>, color: ColorRef, pulse: bool }, Tint { color: ColorRef, strength: f64 } }`
  `pub struct RuleOutcome { pub profile: Option<String>, pub brightness_cap: Option<u8>, pub overlay: Option<Overlay>, pub active: Vec<String> }`
  `pub fn evaluate(rules: &[Rule], facts: &Facts) -> RuleOutcome` — **first match per category** (profile, brightness_cap, overlay-or-tint); `active` lists names of rules that contributed, in rule order.
  Validation (Task 7's `Config::parse` calls it, plus its own unknown-profile check): `fn validate(&self) -> Result<(), String>` — not both overlay and tint; strength in 0..=1; at least one action.

- [ ] **Step 1: Failing tests:** `first_match_per_category` (two profile rules both match → first wins; a later brightness rule still applies), `all_conditions_must_hold` (`on_battery = true, battery_below = 15` with pct 20 → no match), `unknown_fact_is_false` (`power_profile` condition with `facts.power_profile = None` → no match), `empty_condition_always_matches`, `active_lists_contributors`, `overlay_and_tint_both_is_invalid`.
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `feat(core): declarative rules engine`.

---

### Task 7: `config` — schema, validation, example config

**Files:**
- Create: `src/config.rs`, `packaging/config.example.toml`
- Modify: `src/lib.rs` (`pub mod config;`)

**Interfaces:**
- Consumes: `ZoneMap`, `Zone`, `ColorRef`, `ThemeColors`, `Effect`, `core::rules::Rule` and `Rule::validate` (Task 6).
- Produces:
  `pub struct Config { pub device: DeviceCfg, pub effects: EffectsCfg, pub theme: ThemeCfg, pub gates: GatesCfg, pub hints: HintsCfg, pub profiles: Vec<ProfileCfg>, pub rules: Vec<Rule> }`
  `DeviceCfg { zone_map: ZoneMap }` · `EffectsCfg { enabled: bool /*false*/, fps: u8 /*10, 1..=20*/ }` · `ThemeCfg { colors: ThemeColors /*vivid*/, dir: PathBuf /*"~/.config/theming/current", ~ expanded*/ }` · `GatesCfg { on_lock: GateAction /*off*/, on_idle: GateAction /*dim*/, dim_brightness: u8 /*20*/, pause_effects_on_battery: bool /*true*/ }` · `pub enum GateAction { Off, Dim, Keep }` · `HintsCfg { timeout_s: u64 /*15*/, delay_ms: u64 /*250*/, modifiers: BTreeMap<String, ColorRef> /*keys normalized*/, submaps: BTreeMap<String, ColorRef> }` · `ProfileCfg { name: String, colors: Vec<ColorRef> /*1 or 4*/, effect: Effect /*static*/, speed: u8 /*50, 1..=100*/ }`.
  `impl Config { pub fn parse(s: &str) -> Result<Config, ConfigError>; pub fn load(path: &Path) -> Result<Config, ConfigError>; pub fn embedded_default() -> Config; pub fn path() -> PathBuf /* $XDG_CONFIG_HOME/omen-kbd/config.toml */; pub fn profile(&self, name: &str) -> Option<&ProfileCfg>; pub fn needs_cpu_temp(&self) -> bool }`
  `pub fn normalize_mods(s: &str) -> Result<String, String>` (case-insensitive, `+`-separated, names super/ctrl/alt/shift, dedup, order super, ctrl, alt, shift).
  `#[derive(thiserror::Error, Debug)] #[error("{0}")] pub struct ConfigError(pub String)` — message names the offending field.
  `load` of a missing file returns `embedded_default()`; serde uses `#[serde(deny_unknown_fields, default)]` on every section.

- [ ] **Step 1: Write `packaging/config.example.toml`** (this is also the embedded default via `include_str!`):

```toml
# omen-kbd configuration. Zone order everywhere: left, middle, right, wasd.
[device]
zone_map = { left = 2, middle = 1, right = 0, wasd = 3 }

[effects]
enabled = false   # animations are opt-in (colors may live in non-volatile memory)
fps = 10

[theme]
colors = "vivid"  # or "exact"
dir = "~/.config/theming/current"

[gates]
on_lock = "off"
on_idle = "dim"
dim_brightness = 20
pause_effects_on_battery = true

[hints]
timeout_s = 15
delay_ms = 250
[hints.modifiers]
super = "theme:mauve"
"super+shift" = "theme:blue"
ctrl = "theme:peach"
[hints.submaps]
resize = "theme:teal"

[[profiles]]
name = "default"
colors = ["theme:mauve", "theme:blue", "theme:teal", "theme:peach"]

[[profiles]]
name = "game"
colors = ["#202020", "#202020", "#202020", "#ff0000"]

[[profiles]]
name = "white"
colors = ["#ffffff"]

[[rules]]
name = "gamemode"
when = { gamemode = true }
profile = "game"

[[rules]]
name = "power saver dims"
when = { power_profile = "power-saver" }
brightness_cap = 40

[[rules]]
name = "low battery"
when = { on_battery = true, battery_below = 15 }
overlay = { zones = ["right"], color = "#ff0000", pulse = true }
```
- [ ] **Step 2: Failing tests:**

```rust
#[test] fn example_config_parses() {
    let c = Config::embedded_default();
    assert_eq!(c.profiles.len(), 3);
    assert!(!c.effects.enabled);
    assert!(c.hints.modifiers.contains_key("super+shift"));
}
#[test] fn missing_file_gives_default() { assert_eq!(Config::load(Path::new("/nonexistent/x.toml")).unwrap().profiles[0].name, "default"); }
#[test] fn normalizes_modifier_keys() {
    assert_eq!(normalize_mods("Shift+SUPER").unwrap(), "super+shift");
    assert!(normalize_mods("hyper").is_err());
}
#[test] fn rejects_invalid() {
    for (snippet, needle) in [
        ("[[profiles]]\nname='a'\ncolors=['#000000','#000000']", "colors"),
        ("[effects]\nfps=30\n[[profiles]]\nname='a'\ncolors=['#000000']", "fps"),
        ("[[profiles]]\nname='a'\ncolors=['#000000']\n[[profiles]]\nname='a'\ncolors=['#000000']", "duplicate"),
        ("[[profiles]]\nname='custom'\ncolors=['#000000']", "custom"),
        ("[[profiles]]\nname='a'\ncolors=['#000000']\n[[rules]]\nname='r'\nwhen={}\nprofile='zzz'", "zzz"),
        ("[hints.modifiers]\nhyper='#000000'\n[[profiles]]\nname='a'\ncolors=['#000000']", "hyper"),
        ("[[profiles]]\nname='a'\ncolors=['#000000']\nspeed=0", "speed"),
        ("[bogus]\nx=1\n[[profiles]]\nname='a'\ncolors=['#000000']", "bogus"),
        ("", "profile"),
    ] {
        let e = Config::parse(snippet).unwrap_err().0;
        assert!(e.contains(needle), "{snippet:?} -> {e}");
    }
}
```
- [ ] **Step 3–5:** FAIL → implement (`custom` is reserved for `omen-kbd set`; call `Rule::validate` and check that rule profiles exist) → PASS. **Step 6: Commit** `feat: TOML config schema, validation and example config`.

---

### Task 8: `core::compositor`

**Files:** Create `src/core/compositor.rs`

**Interfaces:**
- Consumes: `Frame`, `Rgb`, `Zone`, `Effect`, `render`, `pulse_factor`.
- Produces:
  `pub enum ResolvedOverlay { Zones { zones: Vec<Zone>, color: Rgb, pulse: bool }, Tint { color: Rgb, strength: f64 } }`
  `pub struct Inputs { pub base: Frame, pub effect: Effect, pub speed: u8, pub animate: bool, pub overlay: Option<ResolvedOverlay>, pub hint: Option<Rgb>, pub brightness: u8, pub backlight: bool }`
  `#[derive(Clone, Copy, PartialEq, Debug)] pub struct Output { pub backlight: bool, pub zones: Frame, pub animated: bool }`
  `pub fn compose(i: &Inputs, t: f64) -> Output`

Order (pinned): (1) zones = `render(effect, base, t, speed)` if `animate && effect.is_animated()` else `base`; (2) overlay: Tint ⇒ every zone `lerp(zone, color, strength)`; Zones ⇒ listed zones = `color`, multiplied by `pulse_factor(t)` only if `pulse && animate`; (3) hint ⇒ all four zones = hint; (4) scale every zone by `brightness`; (5) `backlight = i.backlight && brightness > 0`; (6) `animated = backlight && hint.is_none() && animate && (effect.is_animated() || overlay is a pulsing Zones overlay)`.

- [ ] **Step 1: Failing tests:** `static_passthrough`, `effect_ignored_when_not_animating`, `tint_half_blends`, `zone_overlay_replaces_only_named_zones`, `hint_beats_effect_and_overlay`, `brightness_zero_turns_backlight_off`, `animated_false_with_hint`, `pulse_overlay_animates_only_when_allowed`.
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `feat(core): layer compositor`.

---

### Task 9: `backend` — trait, mock, hp-omen-kbd, mainline-led, selection

**Files:** Create `src/backend/mod.rs`, `src/backend/mock.rs`, `src/backend/hp_omen_kbd.rs`, `src/backend/mainline_led.rs`

**Interfaces:**
- Consumes: `Rgb`.
- Produces:
  `#[derive(thiserror::Error, Debug, Clone, PartialEq)] pub enum BackendError { #[error("hourly write budget exhausted")] Budget, #[error("hp_omen_kbd is read-only (readonly=1)")] ReadOnly, #[error("permission denied on {0} (are you in group omen-kbd?)")] Permission(String), #[error("backend missing: {0}")] Missing(String), #[error("I/O error: {0}")] Io(String) }`
  `pub trait Backend: Send { fn name(&self) -> &'static str; fn write_zones(&mut self, hw: [Rgb; 4]) -> Result<(), BackendError>; fn set_backlight(&mut self, on: bool) -> Result<(), BackendError>; fn read_backlight(&mut self) -> Result<bool, BackendError>; }`
  `#[derive(clap::ValueEnum, Clone, Copy, Debug)] pub enum BackendKind { Auto, Mock, HpOmenKbd, MainlineLed }` (CLI names `auto|mock|hp-omen-kbd|mainline-led`)
  `pub fn open(kind: BackendKind, sys_root: &Path) -> Result<Box<dyn Backend>, BackendError>`
  `pub fn map_io_error(e: &std::io::Error, path: &Path) -> BackendError` (EBUSY→Budget, EPERM→ReadOnly, EACCES→Permission(path), ENOENT→Missing(path), else Io)
  Mock: `pub enum MockOp { Zones([Rgb; 4]), Backlight(bool) }`; `pub struct MockHandle` (`Clone`) with `fn ops(&self) -> Vec<MockOp>`, `fn fail_with(&self, e: Option<BackendError>)` (applies to every call until cleared), `fn set_hw_backlight(&self, on: bool)` (simulates the Fn key: what `read_backlight` returns; defaults to the last `set_backlight` value, initially `true`), `fn set_delay(&self, d: Duration)` (each write sleeps `d`, to exercise coalescing); `pub fn mock(render: bool) -> (Box<dyn Backend>, MockHandle)` — when `render`, each zone write prints one line `hw0 ▇ hw1 ▇ hw2 ▇ hw3 ▇` using ANSI truecolor backgrounds (`\x1b[48;2;R;G;Bm   \x1b[0m`), in hardware order (the backend never sees logical order).
  hp-omen-kbd: base `<sys_root>/devices/platform/hp-omen-kbd`; `write_zones` writes `"{} {} {} {}\n"` of `to_hex()`; `set_backlight` writes `"1\n"`/`"0\n"`; `read_backlight` trims == `"1"`.
  mainline-led: dirs `hp::kbd_zoned_backlight-{right,center,left,wasd}` = hw 0..3; `write_zones` writes `"{r} {g} {b}\n"` to each `multi_intensity` (decimal); `set_backlight(on)` writes `"255\n"`/`"0\n"` to each `brightness`; `read_backlight` = any brightness > 0.
  `open(Auto)`: hp present = base dir exists; mainline present = all four LED dirs exist; both ⇒ `Err(Io("both hp-omen-kbd and mainline LEDs present; remove the hp-omen-kbd DKMS module"))`; none ⇒ `Err(Missing("no keyboard backend (is hp_omen_kbd loaded?)"))`. `Mock` is never returned by `Auto`.

- [ ] **Step 1: Failing tests** (fake trees under `tempfile::tempdir()` as `sys_root`):

```rust
#[test] fn hp_writes_hw_order_line() {
    let root = fake_hp_tree();                    // creates devices/platform/hp-omen-kbd/{zones,backlight}
    let mut b = open(BackendKind::HpOmenKbd, root.path()).unwrap();
    b.write_zones([Rgb::new(1,2,3), Rgb::new(255,0,0), Rgb::new(0,255,0), Rgb::new(0,0,255)]).unwrap();
    assert_eq!(read(root.path(), "devices/platform/hp-omen-kbd/zones"), "010203 FF0000 00FF00 0000FF\n");
}
#[test] fn mainline_writes_each_led() {
    let root = fake_mainline_tree();
    let mut b = open(BackendKind::MainlineLed, root.path()).unwrap();
    b.write_zones([Rgb::new(1,2,3), Rgb::BLACK, Rgb::BLACK, Rgb::WHITE]).unwrap();
    assert_eq!(read(root.path(), "class/leds/hp::kbd_zoned_backlight-right/multi_intensity"), "1 2 3\n");
    assert_eq!(read(root.path(), "class/leds/hp::kbd_zoned_backlight-wasd/multi_intensity"), "255 255 255\n");
}
#[test] fn auto_selection() {
    assert_eq!(open(BackendKind::Auto, fake_hp_tree().path()).unwrap().name(), "hp-omen-kbd");
    assert_eq!(open(BackendKind::Auto, fake_mainline_tree().path()).unwrap().name(), "mainline-led");
    assert!(matches!(open(BackendKind::Auto, tempfile::tempdir().unwrap().path()), Err(BackendError::Missing(_))));
    assert!(open(BackendKind::Auto, fake_both_tree().path()).is_err());
}
#[test] fn errno_mapping() {
    let p = Path::new("/x");
    assert_eq!(map_io_error(&io::Error::from_raw_os_error(libc::EBUSY), p), BackendError::Budget);
    assert_eq!(map_io_error(&io::Error::from_raw_os_error(libc::EPERM), p), BackendError::ReadOnly);
    assert!(matches!(map_io_error(&io::Error::from_raw_os_error(libc::EACCES), p), BackendError::Permission(_)));
}
#[test] fn mock_records_and_fails_on_demand() {
    let (mut b, h) = mock(false);
    b.set_backlight(true).unwrap();
    h.fail_with(Some(BackendError::Budget));
    assert_eq!(b.write_zones([Rgb::BLACK; 4]), Err(BackendError::Budget));
    assert_eq!(h.ops(), vec![MockOp::Backlight(true)]);
}
```
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `feat(backend): backend trait with mock, hp-omen-kbd and mainline-led`.

---

### Task 10: `state` — persisted runtime state

**Files:** Create `src/state.rs`

**Interfaces:**
- Consumes: `Effect`.
- Produces:
  `#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)] pub struct PersistedState { pub active_profile: String, pub custom: Option<Vec<String>>, pub effect_override: Option<EffectOverride>, pub brightness: u8, pub on: bool, pub lifetime_writes: u64 }`
  `#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq)] pub struct EffectOverride { pub effect: Effect, pub speed: u8 }`
  `impl PersistedState { pub fn default_for(first_profile: &str) -> Self /* brightness 100, on true */; pub fn load(path: &Path, first_profile: &str) -> (Self, Option<String>); pub fn save(&self, path: &Path) -> std::io::Result<()>; pub fn path() -> PathBuf }`
  Unknown JSON fields are ignored (forward compatibility).

- [ ] **Step 1: Failing tests:** `missing_file_gives_default`; `corrupt_file_is_moved_aside` (writes `{not json`, load returns default + `Some(warning)`, `state.json.bad` exists); `save_roundtrips_and_leaves_no_tmp` (save into a non-existent nested dir, reload equal, no `state.json.tmp`).
- [ ] **Step 2–4:** FAIL → implement (`save`: write `state.json.tmp`, `sync_all`, `rename`) → PASS. **Step 5: Commit** `feat: atomic persisted state`.

---

### Task 11: `ipc` — protocol, socket lifecycle, client

**Files:** Create `src/ipc.rs`

**Interfaces:**
- Consumes: `Facts`.
- Produces:
  `#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)] #[serde(tag = "cmd", rename_all = "kebab-case")] pub enum Request { Status, Set { colors: Vec<String> }, Profile { select: String }, Brightness { value: String }, Power { action: PowerAction }, Effect { name: String, speed: Option<u8> }, Hint { mods: String }, Idle { on: bool }, Flag { name: String, on: bool }, Reload, Subscribe }`
  `#[serde(rename_all = "kebab-case")] pub enum PowerAction { On, Off, Toggle }`
  `#[derive(Serialize, Deserialize, Debug, Clone, PartialEq, Default)] pub struct Status { pub profile: String, pub effect: String, pub effects_enabled: bool, pub animated: bool, pub brightness: u8, pub on: bool, pub backlight: bool, pub zones: [String; 4], pub hint: Option<String>, pub active_rules: Vec<String>, pub facts: Facts, pub backend: String, pub last_error: Option<String>, pub budget_exhausted: bool, pub lifetime_writes: u64, pub warnings: Vec<String> }` (`zones` are logical-order `RRGGBB` after compositing)
  `#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)] pub struct Response { pub ok: bool, pub error: Option<String>, pub status: Option<Status> }` with `Response::ok(Option<Status>)`, `Response::err(String)`
  `#[derive(thiserror::Error, Debug)] pub enum IpcError { #[error("omen-kbd daemon already running")] AlreadyRunning, #[error("daemon not running")] NotRunning, #[error("{0}")] Io(#[from] std::io::Error), #[error("protocol: {0}")] Protocol(String) }`
  `pub fn socket_path() -> PathBuf` · `pub async fn bind(path: &Path) -> Result<tokio::net::UnixListener, IpcError>` (if path exists: connect succeeds ⇒ `AlreadyRunning`; else remove stale; bind; chmod 0600) · `pub async fn request(path: &Path, req: &Request) -> Result<Response, IpcError>` (connect failure ⇒ `NotRunning`) · `pub async fn subscribe(path: &Path) -> Result<tokio::io::Lines<BufReader<OwnedReadHalf>>, IpcError>` (sends `Subscribe`; each following line is a `Response` with `status`)
  Framing helpers used by the server: `pub async fn read_request(r: &mut (impl AsyncBufRead + Unpin)) -> Result<Option<Request>, IpcError>` (None at EOF) and `pub async fn write_response(w: &mut (impl AsyncWrite + Unpin), resp: &Response) -> Result<(), IpcError>`. A connection may carry several requests in sequence; replies come back in order.

- [ ] **Step 1: Failing tests** (`#[tokio::test]`, socket in a temp dir): `request_wire_format` (`serde_json::to_string(&Request::Set{colors: vec!["#ff0000".into()]})` == `{"cmd":"set","colors":["#ff0000"]}`), `bind_sets_mode_0600`, `bind_replaces_stale_socket` (create a socket file with no listener → bind ok), `bind_refuses_when_running` (second bind while first listener alive ⇒ `AlreadyRunning`), `request_roundtrip` (tiny server task replies `Response::ok(None)`), `request_without_daemon_is_not_running`.
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `feat(ipc): unix-socket JSON protocol`.

---

### Task 12: `daemon::model` — pure state machine

**Files:** Create `src/daemon/mod.rs`, `src/daemon/model.rs`

**Interfaces:**
- Consumes: `Config`, `Palette`, `PersistedState`, `EffectOverride`, `Facts`, `evaluate`, `Inputs`/`ResolvedOverlay`/`Output`, `Request`, `Status`, `BackendError`, `normalize_mods`.
- Produces:
  `pub enum FactUpdate { PowerProfile(String), OnBattery(bool), BatteryPct(f64), CpuTemp(f64), Locked(bool), LidClosed(bool) }`
  `pub enum Change { None, Output, OutputAndPersist }`
  `pub struct Model` with:
  `fn new(config: Config, palette: Palette, persisted: PersistedState, backend: &str) -> Model`
  `fn handle(&mut self, req: &Request, now: Instant) -> Result<Change, String>` (Set/Profile/Brightness/Power/Effect/Hint/Idle/Flag; Status/Subscribe/Reload return `Ok(Change::None)` — handled by `run`)
  `fn replace_config(&mut self, config: Config, palette: Palette)` · `fn set_palette(&mut self, palette: Palette)` · `fn apply_fact(&mut self, f: FactUpdate)` · `fn set_suspended(&mut self, s: bool)` · `fn set_submap(&mut self, s: Option<String>)` · `fn set_hw_backlight(&mut self, on: bool)` · `fn add_warning(&mut self, w: String)`
  `fn record_write(&mut self, r: &Result<bool /*zones written*/, BackendError>)` (Ok(true) ⇒ `lifetime_writes += 1`, clears `last_error`; Err(Budget) ⇒ `budget_exhausted = true`; any Err ⇒ `last_error = Some(e.to_string())`)
  `fn inputs(&mut self, now: Instant) -> Option<Inputs>` (None while suspended; drops an expired hint; recomputes theme warnings)
  `fn status(&self, out: Option<&Output>) -> Status` · `fn persisted(&self) -> &PersistedState` · `fn hint_deadline(&self) -> Option<Instant>` · `fn config(&self) -> &Config`
  `fn wants_animation(&self) -> bool` (the `animate` condition below **without** the `hw_backlight` term, and the active effect is animated — tells `run` to keep probing the Fn-key state)

Pinned semantics for `inputs`:
- Profile = rule outcome's profile, else `persisted.active_profile`; `"custom"` uses `persisted.custom` (static). Colors of length 1 fill all four zones. A theme ref that fails to resolve ⇒ `Rgb::WHITE` + warning `"unknown theme color: <name>"`.
- Effect/speed = `persisted.effect_override` if set, else the profile's.
- `animate = config.effects.enabled && !budget_exhausted && !(facts.on_battery == Some(true) && gates.pause_effects_on_battery) && hw_backlight != Some(false)`.
- Hint: modifier hint (set by `Hint`, expires `timeout_s` after it was set; also cleared by `Locked(true)` and `set_suspended(true)`) else submap hint (if `hints.submaps` has the submap) else none.
- `brightness = min(persisted.brightness, rule brightness_cap, dim_brightness if (locked && on_lock == Dim) || (idle && on_idle == Dim))`.
- `backlight = persisted.on && !lid_closed && !(locked && on_lock == Off) && !(idle && on_idle == Off)`.
Requests:
- `Set{colors}`: 1 or 4 valid `ColorRef`s ⇒ `custom = colors`, `active_profile = "custom"`, `on = true`, clear `effect_override` ⇒ `OutputAndPersist`; else `Err`.
- `Profile{select}`: a configured name (or `custom` if `custom` is set), `next`/`prev` cycle config order skipping `custom` ⇒ `OutputAndPersist`; unknown ⇒ `Err("unknown profile: <x>")`.
- `Brightness{value}`: `"N"` (0..=100) sets; `"+N"`/`"-N"` adjusts, clamped ⇒ `OutputAndPersist`.
- `Power{On|Off|Toggle}` ⇒ `OutputAndPersist`.
- `Effect{name, speed}`: if `!effects.enabled` ⇒ `Err("animations are off: set [effects] enabled = true in ~/.config/omen-kbd/config.toml, then omen-kbd reload")`; `"static"` clears the override; else set override (speed default = profile speed) ⇒ `OutputAndPersist`.
- `Hint{mods}`: `"clear"` ⇒ none; else `normalize_mods`, must exist in `hints.modifiers` (else `Err("no color configured for <mods>")`) ⇒ `Output`.
- `Idle{on}` ⇒ `facts.idle`; `Flag{name: "gamemode", on}` ⇒ `facts.gamemode`; other flag names ⇒ `Err("unknown flag: <name>")`.

- [ ] **Step 1: Failing tests** (build a `Model` from `Config::embedded_default()` + a palette with mauve/blue/teal/peach/red): `set_custom_colors_persists_and_outputs`, `profile_next_cycles_config_order_skipping_custom`, `brightness_relative_clamps` (`+30` from 90 ⇒ 100; `-150` ⇒ 0), `lid_and_lock_gates_turn_backlight_off`, `idle_dim_lowers_brightness`, `hint_expires_after_timeout` (inputs at `now + 16 s` has no hint), `lock_clears_hint`, `modifier_hint_beats_submap`, `effect_request_errors_when_disabled` (message contains `enabled = true`), `battery_pauses_effects`, `budget_error_disables_animation`, `rule_profile_overrides_user_selection` (gamemode flag ⇒ base = game colors), `brightness_cap_from_rule`, `unknown_theme_color_warns_and_uses_white` (Review Focus), `suspended_yields_no_inputs`.
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `feat(daemon): pure state model`.

---

### Task 13: `daemon::writer` + `daemon::run` — async loop

**Files:** Create `src/daemon/writer.rs`, `src/daemon/run.rs`; Test: `tests/daemon_mock.rs`

**Interfaces:**
- Consumes: `Model`, `compose`, `ZoneMap::to_hw`, `Backend`, `ipc::{bind, read_request, write_response, Request, Response, Status}`, `Config::load`, `Palette::load`, `PersistedState::save`.
- Produces:
  `pub struct WriteJob { pub generation: u64, pub backlight: bool, pub hw: [Rgb; 4], pub force: bool }`
  `pub struct WriteOutcome { pub generation: u64, pub result: Result<bool /*zones written*/, BackendError> }`
  `pub enum WriterMsg { Outcome(WriteOutcome), HwBacklight(Result<bool, BackendError>) }`
  `pub fn spawn_writer(backend: Box<dyn Backend>, jobs: watch::Receiver<Option<WriteJob>>, probes: mpsc::Receiver<()>, results: mpsc::Sender<WriterMsg>) -> JoinHandle<()>` — the writer is the only owner of the backend; backend calls run in `spawn_blocking`; remembers the last successfully written `(backlight, hw)`; for a job: backlight off ⇒ `set_backlight(false)` if not already off, zones untouched; backlight on ⇒ `set_backlight(true)` if not already on, then `write_zones(hw)` if `force` or hw differs; on error the remembered state is not updated. A probe ⇒ `read_backlight()` ⇒ `WriterMsg::HwBacklight`. Coalescing comes from `watch` (latest job wins).
  `pub enum Event { Fact(FactUpdate), Sleep(bool), Submap(Option<String>), ThemeChanged, HwBacklight(bool) }`
  `pub struct RunOptions { pub backend: BackendKind, pub sys_root: PathBuf, pub config_path: PathBuf, pub state_path: PathBuf, pub socket_path: PathBuf, pub mock_render: bool, pub with_sources: bool }`
  `pub async fn run(opts: RunOptions) -> anyhow::Result<()>` (loads config — invalid at startup ⇒ error exit; loads palette — failure ⇒ empty palette + warning; loads state; opens backend; binds socket; spawns writer and, if `with_sources`, Tasks 14/15 sources; returns on SIGTERM/SIGINT after a final state save)
  `pub async fn run_with(model: Model, backend: Box<dyn Backend>, listener: UnixListener, events: mpsc::Receiver<Event>, opts: RunOptions) -> anyhow::Result<()>` (the testable core; `run` calls it)
  `pub fn event_channel() -> (mpsc::Sender<Event>, mpsc::Receiver<Event>)` (capacity 64)

Loop behavior (pinned):
- After any change: `model.inputs(now)` → `compose(inputs, secs since daemon start)` → `zone_map.to_hw` → if `(backlight, hw)` differs from the last job or `force` is pending ⇒ `generation += 1`, `jobs.send(Some(job))`. Status broadcast (`tokio::sync::broadcast<Status>`) on every recompute whose `Status` differs from the previous.
- State-changing requests reply only after a `WriteOutcome` with `generation >= g` arrives (a superseding job counts), within 2 s; its error becomes `Response::err(e.to_string())`. If no job was needed (frame unchanged), reply immediately ok. `Status` replies immediately. `Subscribe` streams one `Response` per broadcast, first one immediately.
- `Reload`: `Config::load` + `Palette::load`; config error ⇒ `Response::err(<parse error>)`, previous config kept; palette error ⇒ keep old palette + warning.
- Animation: while the last `Output.animated`, a ticker at `1000 / fps` ms recomputes. Hint deadline ⇒ wake and recompute.
- Fn-key state: while `model.wants_animation()`, send a probe every 3 s; `HwBacklight(Ok(v))` ⇒ `model.set_hw_backlight(v)` (pauses/resumes ticking). No probing with a static profile.
- Persist: `Change::OutputAndPersist` and `record_write(Ok(true))` mark state dirty; save 1 s after the first dirty mark (one save per window); also save on `Sleep(true)` and on shutdown.
- `Sleep(true)` ⇒ `set_suspended(true)`; `Sleep(false)` ⇒ `set_suspended(false)` and the next job has `force = true`.
- `ThemeChanged` ⇒ `Palette::load(config.theme.dir)` → `set_palette`, or warning on error.
- Budget error ⇒ model stops animating (Task 12); status shows it.

- [ ] **Step 1: Failing integration tests in `tests/daemon_mock.rs`** (real time — not `start_paused`, because tokio's paused clock can fire the 2 s reply timeout while a blocking backend call is still running; `#[tokio::test(flavor = "multi_thread")]`; temp dirs for socket/state; `backend::mock(false)`; helper `start(cfg_toml) -> (MockHandle, socket_path, event_tx)` that runs `run_with` in a task; test configs set `hints.timeout_s = 1` to keep runs short):

```rust
#[tokio::test(flavor = "multi_thread")] async fn set_request_writes_once_in_hw_order_and_replies_ok() {
    let (h, sock, _ev) = start(DEFAULT).await;
    let r = request(&sock, &Request::Set { colors: vec!["#ff0000".into(), "#00ff00".into(), "#0000ff".into(), "#ffffff".into()] }).await.unwrap();
    assert!(r.ok, "{r:?}");
    let zones: Vec<_> = h.ops().into_iter().filter_map(|o| match o { MockOp::Zones(z) => Some(z), _ => None }).collect();
    assert_eq!(zones.last().unwrap(), &[rgb("0000ff"), rgb("00ff00"), rgb("ff0000"), rgb("ffffff")]);
}
```
  Plus: `burst_of_100_sets_coalesces_to_latest` (`set_delay(20 ms)`; 100 concurrent `Set`s with distinct colors; all replies ok; zone writes ≤ 20; afterwards the last `MockOp::Zones` equals `Status.zones` mapped to hardware order — the keyboard ends where the daemon says it is), `fn_key_off_pauses_effect` (effects enabled, `wave`; `set_hw_backlight(false)`; within 4 s zone writes stop; `set_hw_backlight(true)` ⇒ they resume), `write_error_reaches_cli_response` (`fail_with(Some(Io("boom")))` ⇒ `Set` reply `ok == false`, error contains `boom`), `off_turns_backlight_off_without_zone_write`, `effect_ticks_at_configured_fps` (effects enabled, fps 10, profile effect `wave`; over 1 s ⇒ 8..=12 zone writes), `budget_error_drops_to_static` (fail with `Budget` during an effect ⇒ ticking stops; `status.budget_exhausted`), `resume_forces_identical_rewrite` (Review Focus: `Sleep(true)`, `Sleep(false)` ⇒ exactly one extra identical zone write), `bad_reload_keeps_previous_config` (Review Focus: write invalid TOML to the config path, `Reload` ⇒ `ok == false` with the parse error; `Status.profile` unchanged), `hint_clears_after_timeout` (timeout 1 s ⇒ hint gone after ~1.2 s), `persist_debounced_one_second` (three changes within 200 ms ⇒ state file appears once, ~1 s later, with the last brightness).
- [ ] **Step 2: Run** `cargo test --test daemon_mock` → FAIL.
- [ ] **Step 3: Implement writer, then run loop.**
- [ ] **Step 4: Run** → PASS (also `cargo test` overall).
- [ ] **Step 5: Commit** `feat(daemon): writer actor and async run loop`.

---

### Task 14: Sources — Hyprland socket2, theme watcher, hwmon

**Files:** Create `src/sources/mod.rs`, `src/sources/hyprland.rs`, `src/sources/theme.rs`, `src/sources/hwmon.rs`

**Interfaces:**
- Consumes: `Event`, `FactUpdate`, `mpsc::Sender<Event>`.
- Produces:
  `hyprland::parse_line(line: &str) -> Option<Event>` (`submap>>resize` ⇒ `Submap(Some("resize"))`; `submap>>` ⇒ `Submap(None)`; others ⇒ None)
  `hyprland::socket2_path() -> Option<PathBuf>` (`$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket2.sock`; if the variable is unset — systemd user units often lack it — the newest `.socket2.sock` under `$XDG_RUNTIME_DIR/hypr/*/`)
  `hyprland::run(path_fn: impl Fn() -> Option<PathBuf> + Send + 'static, tx) -> JoinHandle<()>` — on EOF/error sends `Submap(None)`, retries after 1 s, 2 s, then every 5 s.
  `theme::link_chain(p: &Path) -> Vec<PathBuf>` (the path plus each symlink target, until a non-symlink; relative targets resolved against the link's dir)
  `theme::run(dir: PathBuf, tx) -> JoinHandle<()>` — inotify on the parent dir of every element of the chain (`CREATE | MOVED_TO | DELETE | CLOSE_WRITE | ATTRIB`); an event naming a chain element ⇒ `ThemeChanged` (debounced 200 ms), then rebuild the chain and the watches.
  `hwmon::find_package_temp(sys_root: &Path) -> Option<PathBuf>` (`class/hwmon/*/name == "coretemp"` and `temp*_label == "Package id 0"` ⇒ matching `temp*_input`) · `hwmon::read_temp_c(p: &Path) -> Option<f64>` (millidegrees / 1000) · `hwmon::run(sys_root, tx) -> JoinHandle<()>` (every 2 s ⇒ `Fact(CpuTemp)`; `run.rs` starts it only if `config.needs_cpu_temp()`)

- [ ] **Step 1: Failing tests:** `parse_submap_lines`; `socket2_disconnect_clears_submap_and_reconnects` (Review Focus: temp `UnixListener`; server sends `submap>>resize\n` then drops the connection ⇒ events `Submap(Some("resize"))`, `Submap(None)`; a second accepted connection within 3 s proves the reconnect after the 1 s backoff); `link_chain_follows_rice_layout` (tempdir: `themes/a`, `themes/b`, `state/current → ../themes/a`, `cfg/current → ../state/current` ⇒ chain `[cfg/current, state/current, themes/a]`); `theme_swap_emits_event` (replace `state/current` with a link to `themes/b` via rename ⇒ `ThemeChanged` within 1 s, real time); `finds_package_temp` (fake `class/hwmon/hwmon3/{name="coretemp\n", temp1_label="Package id 0\n", temp1_input="45000\n"}` ⇒ `45.0`).
- [ ] **Step 2–4:** FAIL → implement → PASS. Wire them into `run` when `with_sources`. **Step 5: Commit** `feat(sources): hyprland socket2, theme watcher, hwmon`.

---

### Task 15: Sources — system D-Bus (power profile, battery, sleep, lid, lock)

**Files:** Create `src/sources/system.rs`

**Interfaces:**
- Consumes: `Event`, `FactUpdate`.
- Produces: `pub async fn run(tx: mpsc::Sender<Event>) -> JoinHandle<()>` on the **system** bus via zbus proxies (one task per source; a missing service logs one warning and that source stays off — never a crash):
  - `net.hadess.PowerProfiles` `/net/hadess/PowerProfiles` property `ActiveProfile` (initial + changes) ⇒ `Fact(PowerProfile)` (served by `tlp-pd` here).
  - `org.freedesktop.UPower` `/org/freedesktop/UPower` property `OnBattery` ⇒ `Fact(OnBattery)`; `/org/freedesktop/UPower/devices/DisplayDevice` (`org.freedesktop.UPower.Device`) property `Percentage` ⇒ `Fact(BatteryPct)`.
  - `org.freedesktop.login1` Manager: signal `PrepareForSleep(b)` ⇒ `Sleep(b)`; property `LidClosed` ⇒ `Fact(LidClosed)`.
  - Session: `Manager.GetUser(getuid())` → `org.freedesktop.login1.User` property `Display` → session object path (the daemon runs under `user@.service`, so `GetSessionByPID` would fail); signals `Lock`/`Unlock` ⇒ `Fact(Locked(true/false))`; property `LockedHint` changes ⇒ `Fact(Locked(v))`.

- [ ] **Step 1: Implement** (thin glue; no unit tests beyond compile — the mapping is one line per property).
- [ ] **Step 2: Read-only live check.** Run `RUST_LOG=info omen-kbd daemon --backend mock` (after Task 16) and `omen-kbd status --json | jq .facts`. Expected: `power_profile` equals `busctl get-property net.hadess.PowerProfiles /net/hadess/PowerProfiles net.hadess.PowerProfiles ActiveProfile`; `on_battery` and `battery_pct` match `upower -i /org/freedesktop/UPower/devices/DisplayDevice`. (Optional, user-assisted: switch the profile from the rice menu and watch `facts.power_profile` change.)
- [ ] **Step 3: Commit** `feat(sources): system D-Bus facts and sleep/lock signals`.

---

### Task 16: CLI — commands, status output, Waybar stream

**Files:** Create `src/cli.rs`; Modify `src/main.rs` (`fn main() { std::process::exit(omen_kbd::cli::main()) }`)

**Interfaces:**
- Consumes: `ipc::{request, subscribe, socket_path, Request, PowerAction, Response, Status}`, `daemon::run::{run, RunOptions}`, `BackendKind`, Task 17's `gen_hyprland`/`doctor` (add the subcommands here, implemented in Task 17).
- Produces: `pub fn main() -> i32`; `pub fn format_status(s: &Status) -> String`; `pub fn waybar_json(s: &Status) -> serde_json::Value`.

Commands (clap derive): `status [--json]`, `set <COLOR>...` (1 or 4), `profile <NAME|next|prev>`, `brightness <VALUE>` (`allow_hyphen_values` so `-10` works), `on`, `off`, `toggle`, `effect <NAME> [--speed N]`, `hint <MODS|clear>`, `idle <on|off>`, `flag <NAME> <on|off>`, `reload`, `waybar`, `doctor`, `gen-hyprland`, `daemon [--backend auto|mock|hp-omen-kbd|mainline-led] [--mock-render] [--no-sources]` (hidden `--sys-root`, default `/sys`).
Exit codes: 0 ok; 1 `Response.ok == false` (prints `omen-kbd: <error>` to stderr); 2 `NotRunning` (prints `omen-kbd: daemon not running (start it: systemctl --user start omen-kbd)`).
`format_status` lines (pinned labels): `profile`, `zones` (`L #RRGGBB  M #RRGGBB  R #RRGGBB  W #RRGGBB`), `brightness` (`N%  on|off  backlight on|off`), `hint`, `rules`, `backend` (`<name>  writes <n>`), then `error` / `budget` / `warnings` lines only when present.
`waybar_json`: `text` = `"󰌌 <profile>"` when on, `"󰌐 off"` when off; `class` = `"error"` if `last_error` or `budget_exhausted`, else `"off"`, `"hint"`, `"effect"` (animated), `"on"` in that priority; `percentage` = brightness; `tooltip` = `format_status` text. `omen-kbd waybar` prints one compact JSON line per subscribed status and flushes; when the daemon is unreachable it prints `{"text":"󰌐 ?","class":"error","tooltip":"omen-kbd daemon not running"}` and retries every 5 s.

- [ ] **Step 1: Failing tests:** `brightness_accepts_negative_delta` (`Cli::try_parse_from(["omen-kbd","brightness","-10"])` ⇒ Brightness `"-10"`), `set_requires_one_or_four_colors` (2 colors ⇒ parse error), `waybar_on_off_hint_error` (four `Status` fixtures ⇒ expected `text`/`class`/`percentage`), `format_status_has_pinned_labels`.
- [ ] **Step 2–4:** FAIL → implement → PASS.
- [ ] **Step 5: Smoke run** (mock, no hardware): `cargo run -- daemon --backend mock --mock-render --no-sources &`; then `cargo run -- set '#ff0000'` (exit 0, mock logs a write), `cargo run -- effect wave` (exit 1, message mentions `enabled = true`), `cargo run -- status`; stop the daemon; `cargo run -- status` ⇒ exit 2.
- [ ] **Step 6: Commit** `feat(cli): commands, status and waybar stream`.

---

### Task 17: `gen-hyprland` + `doctor`

**Files:** Create `src/gen_hyprland.rs`, `src/doctor.rs`

**Interfaces:**
- Consumes: `Config`, `Palette::load`, `ipc::request`, `spikes/RESULTS.md` (Task 1 decision).
- Produces:
  `pub fn gen_hyprland(cfg: &Config) -> String` — the Task 1 algorithm with `logger` replaced by `hl.exec_cmd("omen-kbd hint " .. pending)` / `hl.exec_cmd("omen-kbd hint clear")`, `WANTED` generated from `cfg.hints.modifiers` keys (sorted), `DELAY_MS = cfg.hints.delay_ms`, raw-press logging removed, header `-- generated by omen-kbd gen-hyprland: save as ~/.config/hypr/modules/kbdlight.lua and require it from hyprland.lua`. (If Task 1 chose the fallback, emit the bare-modkey bind version instead.)
  `pub enum Level { Ok, Warn, Fail }` · `pub struct Check { pub level: Level, pub msg: String }` · `pub struct DoctorEnv { pub sys_root: PathBuf, pub proc_root: PathBuf, pub dkms_root: PathBuf, pub hypr_dir: PathBuf, pub theme_dir: PathBuf, pub socket: PathBuf }` (`Default` = real paths: `/sys`, `/proc`, `/var/lib/dkms`, `~/.config/hypr`, config theme dir, `socket_path()`)
  `pub async fn run_checks(env: &DoctorEnv) -> Vec<Check>`; CLI prints `OK|WARN|FAIL  msg` per line, exit 1 if any Fail.
  Checks (pinned): (1) backend presence via the same rules as `open(Auto)`; for hp-omen-kbd also report `readonly=` from `info` (Warn if `1`); both present ⇒ Fail. (2) writability of `zones` (or each `multi_intensity`) via `libc::access(W_OK)` ⇒ Fail `not writable: sudo usermod -aG omen-kbd $USER, then log in again`. (3) competing writers: `/proc/modules` has `hp_omen_extra` or `omen_rgb_keyboard` ⇒ Fail; a `/proc/*/comm` equal to `omen-space-daem` (15-char truncation) ⇒ Fail; `openrgb` ⇒ Warn; `<sys_root>/devices/platform/hp-wmi/rgb_zones` exists ⇒ Warn. (4) DKMS: any `<dkms_root>/*/*/source/dkms.conf` with `BUILT_MODULE_NAME[n]="hp-wmi"` or `hp_wmi` ⇒ Warn naming `pkg/version`. (5) daemon: `Status` request ⇒ Ok, plus Warn for `budget_exhausted` / `last_error`; unreachable ⇒ Warn. (6) hook: some file under `hypr_dir` contains `omen-kbd hint` ⇒ Ok, else Warn `mode-colors hook not installed (omen-kbd gen-hyprland)`. (7) theme: `Palette::load(theme_dir)` non-empty ⇒ Ok, else Warn.

- [ ] **Step 1: Failing tests:** `generated_lua_lists_configured_sets` (contains `["super+shift"] = true`, `timeout = 250`, `omen-kbd hint `), `generated_lua_compiles` (pipe to `luac -p -`; skip with a message if `luac` is absent), `doctor_flags_competing_drivers` (fake `proc_root/modules` with `hp_omen_extra 20480 0 - Live 0x0` ⇒ a Fail), `doctor_flags_hpwmi_dkms` (fake `dkms_root/hp-omen-extra/2.1.2/source/dkms.conf` with `BUILT_MODULE_NAME[0]="hp-wmi"` ⇒ Warn containing `hp-omen-extra/2.1.2`), `doctor_both_backends_fail`, `doctor_hook_detected`.
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Real read-only run:** `cargo run -- doctor` on this laptop. Expected today: Fail for missing backend and for `hp_omen_extra`/omen-space (until P1 migration), Warn for the hp-wmi DKMS package — i.e. it reports the current mess accurately. **Step 6: Commit** `feat: gen-hyprland and doctor`.

---

### Task 18: Packaging snippets, README, end-to-end mock demo

**Files:**
- Create: `packaging/omen-kbd.service`, `packaging/snippets/autostart.lua`, `packaging/snippets/hypridle.conf`, `packaging/snippets/waybar-module.jsonc`, `packaging/snippets/rice-theme.md`, `packaging/snippets/gamemode.lua`
- Modify: `README.md`

**Interfaces:** Consumes the CLI from Task 16.

- [ ] **Step 1: `packaging/omen-kbd.service`** (user unit; started from Hyprland autostart, so no `[Install]`):

```ini
[Unit]
Description=omen-kbd keyboard color daemon
After=dbus.socket

[Service]
ExecStart=%h/.local/bin/omen-kbd daemon --backend auto
Restart=on-failure
RestartSec=2
```
- [ ] **Step 2: Snippets** (each file starts with a comment saying where it goes in `~/dotfiles`):
  `autostart.lua`: `"systemctl --user start omen-kbd.service"` entry for the rice's autostart list.
  `hypridle.conf`: the replacement listener body — keep the existing timeout and `condition_cmd`, with `on-timeout = omen-kbd idle on` and `on-resume = omen-kbd idle off`.
  `waybar-module.jsonc`: `"custom/kbd": { "exec": "omen-kbd waybar", "return-type": "json", "on-click": "omen-kbd profile next", "on-click-right": "omen-kbd toggle", "on-scroll-up": "omen-kbd brightness +10", "on-scroll-down": "omen-kbd brightness -10" }`.
  `rice-theme.md`: add `omen-kbd reload >/dev/null 2>&1 || true` inside `reload_apps()` in `~/.config/rice/bin/rice-theme`.
  `gamemode.lua`: call `hl.exec_cmd("omen-kbd flag gamemode on")` / `off` from `rice.gamemode.toggle()`.
- [ ] **Step 3: README** — what it is; safety notes (no hardware writes without the kernel module; readonly default; write budget); build/install (`cargo install --path . --root ~/.local`); mock demo; command list; pointer to the spec.
- [ ] **Step 4: End-to-end mock demo (no hardware).** Run: `cargo build --release && ./target/release/omen-kbd daemon --backend mock --mock-render &`. Then:
  `omen-kbd status` (default profile, theme colors, `backend mock`) · `omen-kbd set ff0000 00ff00 0000ff ffffff` (one write) · `omen-kbd hint super` (mauve) then wait 15 s (hint expires) · `omen-kbd profile next` · `omen-kbd brightness -50` · `omen-kbd off` (backlight op only) · `omen-kbd on` · `omen-kbd effect wave` (exit 1, explains opt-in) · `omen-kbd waybar | head -2`.
  Expected: each step's exit code and mock-render line match the spec; `ps -o %cpu` of the daemon ≈ 0 when idle. Stop the daemon.
- [ ] **Step 5: Full gate.** `cargo test && cargo clippy --all-targets -- -D warnings && cargo fmt --check` → all green.
- [ ] **Step 6: Commit** `docs: packaging snippets, README and mock demo`.
