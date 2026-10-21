//! Pure daemon state: config, palette, persisted state, system facts, gates and hints → compositor
//! inputs and status. No I/O, no clock of its own: callers pass `now`.

use std::time::{Duration, Instant};

pub use crate::backend::BackendError;
pub use crate::config::{CUSTOM_PROFILE, Config, GateAction, normalize_mods};
pub use crate::core::color::Rgb;
pub use crate::core::compositor::{Inputs, Output, ResolvedOverlay};
pub use crate::core::effects::Effect;
pub use crate::core::palette::{ColorRef, Palette};
pub use crate::core::rules::{Facts, Overlay, RuleOutcome, evaluate};
pub use crate::core::zones::Frame;
pub use crate::ipc::{PowerAction, Request, Status};
pub use crate::state::{EffectOverride, PersistedState};

const EFFECTS_OFF: &str = "animations are off: set [effects] enabled = true in ~/.config/omen-kbd/config.toml, then omen-kbd reload";

/// A change in what the daemon knows about the system.
#[derive(Debug, Clone, PartialEq)]
pub enum FactUpdate {
    PowerProfile(String),
    OnBattery(bool),
    BatteryPct(f64),
    CpuTemp(f64),
    Locked(bool),
    LidClosed(bool),
}

/// What a request changed: nothing, the frame, or the frame and the persisted state.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Change {
    None,
    Output,
    OutputAndPersist,
}

pub struct Model {
    config: Config,
    palette: Palette,
    persisted: PersistedState,
    facts: Facts,
    suspended: bool,
    lid_closed: bool,
    hw_backlight: Option<bool>,
    /// Modifier hint (normalized mods) and when it was set.
    hint: Option<(String, Instant)>,
    submap: Option<String>,
    budget_exhausted: bool,
    last_error: Option<String>,
    /// Long-lived warnings (config/palette/state problems).
    warnings: Vec<String>,
    /// Warnings found while resolving the current frame (recomputed by `inputs`).
    frame_warnings: Vec<String>,
    backend: String,
}

impl Model {
    pub fn new(
        config: Config,
        palette: Palette,
        persisted: PersistedState,
        backend: &str,
    ) -> Model {
        let mut m = Model {
            config,
            palette,
            persisted,
            facts: Facts::default(),
            suspended: false,
            lid_closed: false,
            hw_backlight: None,
            hint: None,
            submap: None,
            budget_exhausted: false,
            last_error: None,
            warnings: Vec::new(),
            frame_warnings: Vec::new(),
            backend: backend.to_string(),
        };
        m.fix_active_profile();
        m
    }

    pub fn config(&self) -> &Config {
        &self.config
    }

    pub fn persisted(&self) -> &PersistedState {
        &self.persisted
    }

    /// Applies a request. Status/Subscribe/Reload are handled by the run loop (`Change::None`).
    pub fn handle(&mut self, req: &Request, now: Instant) -> Result<Change, String> {
        match req {
            Request::Status | Request::Subscribe | Request::Reload => Ok(Change::None),
            Request::Set { colors } => {
                if colors.len() != 1 && colors.len() != 4 {
                    return Err(format!(
                        "set takes 1 or 4 colors (left middle right wasd), got {}",
                        colors.len()
                    ));
                }
                for c in colors {
                    ColorRef::parse(c).map_err(|e| e.to_string())?;
                }
                self.persisted.custom = Some(colors.clone());
                self.persisted.active_profile = CUSTOM_PROFILE.to_string();
                self.persisted.on = true;
                self.persisted.effect_override = None;
                Ok(Change::OutputAndPersist)
            }
            Request::Profile { select } => {
                let names: Vec<&str> = self
                    .config
                    .profiles
                    .iter()
                    .map(|p| p.name.as_str())
                    .collect();
                let len = names.len();
                let cur = names
                    .iter()
                    .position(|n| *n == self.persisted.active_profile);
                let target = match select.as_str() {
                    "next" => names[cur.map_or(0, |i| (i + 1) % len)],
                    "prev" => names[cur.map_or(len - 1, |i| (i + len - 1) % len)],
                    s if s == CUSTOM_PROFILE && self.persisted.custom.is_some() => CUSTOM_PROFILE,
                    s if names.contains(&s) => s,
                    other => return Err(format!("unknown profile: {other}")),
                }
                .to_string();
                self.persisted.active_profile = target;
                self.persisted.effect_override = None;
                Ok(Change::OutputAndPersist)
            }
            Request::Brightness { value } => {
                let v = value.trim();
                let current = i32::from(self.persisted.brightness);
                let bad = || format!("brightness must be 0-100, +N or -N, got {v:?}");
                let new = if v.starts_with('+') || v.starts_with('-') {
                    let delta: i32 = v.parse().map_err(|_| bad())?;
                    (current + delta).clamp(0, 100)
                } else {
                    let n: i32 = v.parse().map_err(|_| bad())?;
                    if !(0..=100).contains(&n) {
                        return Err(bad());
                    }
                    n
                };
                self.persisted.brightness = new as u8;
                Ok(Change::OutputAndPersist)
            }
            Request::Power { action } => {
                self.persisted.on = match action {
                    PowerAction::On => true,
                    PowerAction::Off => false,
                    PowerAction::Toggle => !self.persisted.on,
                };
                Ok(Change::OutputAndPersist)
            }
            Request::Effect { name, speed } => {
                if !self.config.effects.enabled {
                    return Err(EFFECTS_OFF.to_string());
                }
                let effect: Effect = name.parse()?;
                if effect == Effect::Static {
                    self.persisted.effect_override = None;
                } else {
                    let speed = match speed {
                        Some(s) => *s,
                        None => self.profile_look(&self.shown_profile(), &mut Vec::new()).2,
                    };
                    if !(1..=100).contains(&speed) {
                        return Err(format!("speed must be 1-100, got {speed}"));
                    }
                    self.persisted.effect_override = Some(EffectOverride { effect, speed });
                }
                Ok(Change::OutputAndPersist)
            }
            Request::Hint { mods } => {
                if mods.trim().eq_ignore_ascii_case("clear") {
                    self.hint = None;
                    return Ok(Change::Output);
                }
                let key = normalize_mods(mods)?;
                if !self.config.hints.modifiers.contains_key(&key) {
                    return Err(format!("no color configured for {key}"));
                }
                self.hint = Some((key, now));
                Ok(Change::Output)
            }
            Request::Idle { on } => {
                self.facts.idle = *on;
                Ok(Change::Output)
            }
            Request::Flag { name, on } => match name.as_str() {
                "gamemode" => {
                    self.facts.gamemode = *on;
                    Ok(Change::Output)
                }
                other => Err(format!("unknown flag: {other}")),
            },
        }
    }

    pub fn replace_config(&mut self, config: Config, palette: Palette) {
        self.config = config;
        self.palette = palette;
        self.fix_active_profile();
        if let Some((mods, _)) = &self.hint
            && !self.config.hints.modifiers.contains_key(mods)
        {
            self.hint = None;
        }
    }

    pub fn set_palette(&mut self, palette: Palette) {
        self.palette = palette;
    }

    pub fn apply_fact(&mut self, f: FactUpdate) {
        match f {
            FactUpdate::PowerProfile(p) => self.facts.power_profile = Some(p),
            FactUpdate::OnBattery(b) => self.facts.on_battery = Some(b),
            FactUpdate::BatteryPct(p) => self.facts.battery_pct = Some(p),
            FactUpdate::CpuTemp(t) => self.facts.cpu_temp_c = Some(t),
            FactUpdate::Locked(l) => {
                self.facts.locked = l;
                if l {
                    self.hint = None;
                }
            }
            FactUpdate::LidClosed(c) => self.lid_closed = c,
        }
    }

    pub fn set_suspended(&mut self, s: bool) {
        self.suspended = s;
        if s {
            self.hint = None;
        }
    }

    pub fn set_submap(&mut self, s: Option<String>) {
        self.submap = s.filter(|n| !n.is_empty());
    }

    /// What the firmware reports (e.g. after the Fn backlight key).
    pub fn set_hw_backlight(&mut self, on: bool) {
        self.hw_backlight = Some(on);
    }

    pub fn add_warning(&mut self, w: String) {
        if !self.warnings.contains(&w) {
            self.warnings.push(w);
        }
    }

    /// Records a writer outcome. `Ok(true)` = zones were written.
    pub fn record_write(&mut self, r: &Result<bool, BackendError>) {
        match r {
            Ok(wrote) => {
                if *wrote {
                    self.persisted.lifetime_writes += 1;
                    self.budget_exhausted = false;
                }
                self.last_error = None;
            }
            Err(e) => {
                if *e == BackendError::Budget {
                    self.budget_exhausted = true;
                }
                self.last_error = Some(e.to_string());
            }
        }
    }

    /// When the current modifier hint expires, if one is shown.
    pub fn hint_deadline(&self) -> Option<Instant> {
        self.hint
            .as_ref()
            .map(|(_, at)| *at + Duration::from_secs(self.config.hints.timeout_s))
    }

    /// Animation allowed by config and gates, ignoring the Fn-key state, and something animates.
    /// While true the run loop keeps probing the backlight so the Fn key can pause/resume effects.
    pub fn wants_animation(&self) -> bool {
        if !self.animation_allowed() {
            return false;
        }
        let rules = self.rules();
        let effect = self.shown_effect(&self.shown_profile_for(&rules));
        let pulsing = matches!(rules.overlay, Some(Overlay::Zones { pulse: true, .. }));
        effect.is_animated() || pulsing
    }

    /// Compositor inputs for `now`, or `None` while suspended.
    pub fn inputs(&mut self, now: Instant) -> Option<Inputs> {
        if self.suspended {
            return None;
        }
        if self.hint_deadline().is_some_and(|d| now >= d) {
            self.hint = None;
        }
        let rules = self.rules();
        let mut warnings = Vec::new();
        let name = self.shown_profile_for(&rules);
        let (base, profile_effect, profile_speed) = self.profile_look(&name, &mut warnings);
        let (effect, speed) = match self.persisted.effect_override {
            Some(o) => (o.effect, o.speed),
            None => (profile_effect, profile_speed),
        };
        let overlay = rules.overlay.as_ref().map(|o| match o {
            Overlay::Zones {
                zones,
                color,
                pulse,
            } => ResolvedOverlay::Zones {
                zones: zones.clone(),
                color: self.resolve(color, &mut warnings),
                pulse: *pulse,
            },
            Overlay::Tint { color, strength } => ResolvedOverlay::Tint {
                color: self.resolve(color, &mut warnings),
                strength: *strength,
            },
        });
        let hint = self.hint_color(&mut warnings);
        let inputs = Inputs {
            base,
            effect,
            speed,
            animate: self.animation_allowed() && self.hw_backlight != Some(false),
            overlay,
            hint,
            brightness: self.effective_brightness(&rules),
            backlight: self.backlight_wanted(),
        };
        self.frame_warnings = warnings;
        Some(inputs)
    }

    pub fn status(&self, out: Option<&Output>) -> Status {
        let rules = self.rules();
        let name = self.shown_profile_for(&rules);
        let hint = self.hint.as_ref().map(|(m, _)| m.clone()).or_else(|| {
            self.submap
                .as_ref()
                .filter(|s| self.config.hints.submaps.contains_key(*s))
                .map(|s| format!("submap:{s}"))
        });
        let mut warnings = self.warnings.clone();
        for w in &self.frame_warnings {
            if !warnings.contains(w) {
                warnings.push(w.clone());
            }
        }
        Status {
            effect: self.shown_effect(&name).name().to_string(),
            profile: name,
            effects_enabled: self.config.effects.enabled,
            animated: out.is_some_and(|o| o.animated),
            brightness: self.persisted.brightness,
            on: self.persisted.on,
            backlight: out.map_or_else(|| self.backlight_wanted(), |o| o.backlight),
            zones: out.map_or_else(Default::default, |o| o.zones.map(|z| z.to_hex())),
            hint,
            active_rules: rules.active,
            facts: self.facts.clone(),
            backend: self.backend.clone(),
            last_error: self.last_error.clone(),
            budget_exhausted: self.budget_exhausted,
            lifetime_writes: self.persisted.lifetime_writes,
            warnings,
        }
    }

    // ── internals ────────────────────────────────────────────────────────────

    fn rules(&self) -> RuleOutcome {
        evaluate(&self.config.rules, &self.facts)
    }

    /// The profile currently shown: a rule's profile, else the user's choice.
    fn shown_profile_for(&self, rules: &RuleOutcome) -> String {
        rules
            .profile
            .clone()
            .unwrap_or_else(|| self.persisted.active_profile.clone())
    }

    fn shown_profile(&self) -> String {
        self.shown_profile_for(&self.rules())
    }

    fn shown_effect(&self, profile: &str) -> Effect {
        match self.persisted.effect_override {
            Some(o) => o.effect,
            None => self.profile_look(profile, &mut Vec::new()).1,
        }
    }

    fn animation_allowed(&self) -> bool {
        let gates = &self.config.gates;
        self.config.effects.enabled
            && !self.budget_exhausted
            && !(self.facts.on_battery == Some(true) && gates.pause_effects_on_battery)
    }

    fn effective_brightness(&self, rules: &RuleOutcome) -> u8 {
        let gates = &self.config.gates;
        let mut b = self.persisted.brightness;
        if let Some(cap) = rules.brightness_cap {
            b = b.min(cap);
        }
        let dim = (self.facts.locked && gates.on_lock == GateAction::Dim)
            || (self.facts.idle && gates.on_idle == GateAction::Dim);
        if dim {
            b = b.min(gates.dim_brightness);
        }
        b
    }

    fn backlight_wanted(&self) -> bool {
        let gates = &self.config.gates;
        self.persisted.on
            && !self.lid_closed
            && !(self.facts.locked && gates.on_lock == GateAction::Off)
            && !(self.facts.idle && gates.on_idle == GateAction::Off)
    }

    fn hint_color(&self, warnings: &mut Vec<String>) -> Option<Rgb> {
        if let Some((mods, _)) = &self.hint
            && let Some(c) = self.config.hints.modifiers.get(mods)
        {
            return Some(self.resolve(c, warnings));
        }
        let submap = self.submap.as_ref()?;
        let c = self.config.hints.submaps.get(submap)?;
        Some(self.resolve(c, warnings))
    }

    fn resolve(&self, c: &ColorRef, warnings: &mut Vec<String>) -> Rgb {
        c.resolve(&self.palette, self.config.theme.colors)
            .unwrap_or_else(|name| {
                let w = format!("unknown theme color: {name}");
                if !warnings.contains(&w) {
                    warnings.push(w);
                }
                Rgb::WHITE
            })
    }

    /// Base frame, effect and speed of a profile (`custom` = the colors from `omen-kbd set`).
    fn profile_look(&self, name: &str, warnings: &mut Vec<String>) -> (Frame, Effect, u8) {
        if name == CUSTOM_PROFILE {
            let refs: Vec<ColorRef> = self
                .persisted
                .custom
                .iter()
                .flatten()
                .filter_map(|s| ColorRef::parse(s).ok())
                .collect();
            return (self.frame_from(&refs, warnings), Effect::Static, 50);
        }
        match self.config.profile(name) {
            Some(p) => (self.frame_from(&p.colors, warnings), p.effect, p.speed),
            None => {
                warnings.push(format!("unknown profile {name:?}"));
                ([Rgb::WHITE; 4], Effect::Static, 50)
            }
        }
    }

    fn frame_from(&self, refs: &[ColorRef], warnings: &mut Vec<String>) -> Frame {
        match refs {
            [one] => [self.resolve(one, warnings); 4],
            [a, b, c, d] => [
                self.resolve(a, warnings),
                self.resolve(b, warnings),
                self.resolve(c, warnings),
                self.resolve(d, warnings),
            ],
            _ => {
                warnings.push("stored custom colors are invalid; showing white".to_string());
                [Rgb::WHITE; 4]
            }
        }
    }

    /// Falls back to the first profile when the saved one no longer exists.
    fn fix_active_profile(&mut self) {
        let p = &self.persisted.active_profile;
        let valid = (p == CUSTOM_PROFILE && self.persisted.custom.is_some())
            || self.config.profile(p).is_some();
        if !valid {
            let first = self.config.profiles[0].name.clone();
            self.add_warning(format!(
                "profile {p:?} is not in the config; using {first:?}"
            ));
            self.persisted.active_profile = first;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::compositor::compose;
    use std::time::Duration;

    const R: Rgb = Rgb::new(255, 0, 0);
    const G: Rgb = Rgb::new(0, 255, 0);
    const B: Rgb = Rgb::new(0, 0, 255);
    const W: Rgb = Rgb::WHITE;

    fn palette() -> Palette {
        Palette::from_waybar_css(
            "@define-color mauve #cba6f7;\n@define-color blue #89b4fa;\n@define-color teal #94e2d5;\n@define-color peach #fab387;\n@define-color red #f38ba8;\n",
        )
    }

    fn model() -> Model {
        Model::new(
            Config::embedded_default(),
            palette(),
            PersistedState::default_for("default"),
            "mock",
        )
    }

    /// Explicit hex colors so expectations are literals.
    const HEX_CFG: &str = r##"
[effects]
enabled = true
[hints]
timeout_s = 15
[hints.modifiers]
super = "#0000ff"
[hints.submaps]
resize = "#00ff00"
[[profiles]]
name = "rgbw"
colors = ["#ff0000", "#00ff00", "#0000ff", "#ffffff"]
effect = "wave"
[[profiles]]
name = "game"
colors = ["#202020", "#202020", "#202020", "#ff0000"]
[[rules]]
name = "gamemode"
when = { gamemode = true }
profile = "game"
[[rules]]
name = "power saver dims"
when = { power_profile = "power-saver" }
brightness_cap = 40
"##;

    fn hex_model() -> Model {
        Model::new(
            Config::parse(HEX_CFG).unwrap(),
            palette(),
            PersistedState::default_for("rgbw"),
            "mock",
        )
    }

    fn set(m: &mut Model, colors: &[&str]) -> Result<Change, String> {
        m.handle(
            &Request::Set {
                colors: colors.iter().map(|c| c.to_string()).collect(),
            },
            Instant::now(),
        )
    }

    fn req(m: &mut Model, r: Request) -> Result<Change, String> {
        m.handle(&r, Instant::now())
    }

    #[test]
    fn set_custom_colors_persists_and_outputs() {
        let mut m = model();
        assert_eq!(
            set(&mut m, &["#ff0000", "#00ff00", "#0000ff", "#ffffff"]),
            Ok(Change::OutputAndPersist)
        );
        assert_eq!(m.persisted().active_profile, "custom");
        assert!(m.persisted().on);
        assert_eq!(m.inputs(Instant::now()).unwrap().base, [R, G, B, W]);
        assert!(set(&mut m, &["#ff0000", "#00ff00"]).is_err());
        assert!(set(&mut m, &["red"]).is_err());
        assert_eq!(set(&mut m, &["#00ff00"]), Ok(Change::OutputAndPersist));
        assert_eq!(m.inputs(Instant::now()).unwrap().base, [G; 4]);
    }

    #[test]
    fn profile_next_cycles_config_order_skipping_custom() {
        let mut m = model();
        let next = |m: &mut Model| {
            req(
                m,
                Request::Profile {
                    select: "next".into(),
                },
            )
            .unwrap();
            m.persisted().active_profile.clone()
        };
        assert_eq!(next(&mut m), "game");
        assert_eq!(next(&mut m), "white");
        assert_eq!(next(&mut m), "default");
        req(
            &mut m,
            Request::Profile {
                select: "prev".into(),
            },
        )
        .unwrap();
        assert_eq!(m.persisted().active_profile, "white");
        set(&mut m, &["#123456"]).unwrap();
        assert_eq!(next(&mut m), "default", "from custom, next is the first");
        assert!(
            req(
                &mut m,
                Request::Profile {
                    select: "custom".into()
                }
            )
            .is_ok()
        );
        let e = req(
            &mut m,
            Request::Profile {
                select: "nope".into(),
            },
        )
        .unwrap_err();
        assert!(e.contains("unknown profile: nope"), "{e}");
    }

    #[test]
    fn brightness_relative_clamps() {
        let mut m = model();
        let b = |m: &mut Model, v: &str| {
            req(m, Request::Brightness { value: v.into() }).map(|_| m.persisted().brightness)
        };
        assert_eq!(b(&mut m, "90"), Ok(90));
        assert_eq!(b(&mut m, "+30"), Ok(100));
        assert_eq!(b(&mut m, "-150"), Ok(0));
        assert!(b(&mut m, "101").is_err());
        assert!(b(&mut m, "abc").is_err());
    }

    #[test]
    fn lid_and_lock_gates_turn_backlight_off() {
        let mut m = model();
        assert!(m.inputs(Instant::now()).unwrap().backlight);
        m.apply_fact(FactUpdate::LidClosed(true));
        assert!(!m.inputs(Instant::now()).unwrap().backlight);
        m.apply_fact(FactUpdate::LidClosed(false));
        m.apply_fact(FactUpdate::Locked(true));
        assert!(
            !m.inputs(Instant::now()).unwrap().backlight,
            "on_lock = off"
        );
        m.apply_fact(FactUpdate::Locked(false));
        req(
            &mut m,
            Request::Power {
                action: PowerAction::Off,
            },
        )
        .unwrap();
        assert!(!m.inputs(Instant::now()).unwrap().backlight);
        req(
            &mut m,
            Request::Power {
                action: PowerAction::Toggle,
            },
        )
        .unwrap();
        assert!(m.inputs(Instant::now()).unwrap().backlight);
    }

    #[test]
    fn idle_dim_lowers_brightness() {
        let mut m = model();
        req(&mut m, Request::Idle { on: true }).unwrap();
        let i = m.inputs(Instant::now()).unwrap();
        assert_eq!(i.brightness, 20);
        assert!(i.backlight);
        assert_eq!(m.persisted().brightness, 100, "user brightness unchanged");
    }

    #[test]
    fn hint_expires_after_timeout() {
        let mut m = hex_model();
        let t0 = Instant::now();
        m.handle(
            &Request::Hint {
                mods: "SUPER".into(),
            },
            t0,
        )
        .unwrap();
        assert_eq!(m.inputs(t0).unwrap().hint, Some(B));
        assert_eq!(m.hint_deadline(), Some(t0 + Duration::from_secs(15)));
        assert_eq!(m.inputs(t0 + Duration::from_secs(16)).unwrap().hint, None);
    }

    #[test]
    fn lock_clears_hint() {
        let mut m = hex_model();
        req(
            &mut m,
            Request::Hint {
                mods: "super".into(),
            },
        )
        .unwrap();
        m.apply_fact(FactUpdate::Locked(true));
        assert_eq!(m.inputs(Instant::now()).unwrap().hint, None);
    }

    #[test]
    fn modifier_hint_beats_submap() {
        let mut m = hex_model();
        m.set_submap(Some("resize".into()));
        assert_eq!(m.inputs(Instant::now()).unwrap().hint, Some(G));
        req(
            &mut m,
            Request::Hint {
                mods: "super".into(),
            },
        )
        .unwrap();
        assert_eq!(m.inputs(Instant::now()).unwrap().hint, Some(B));
        req(
            &mut m,
            Request::Hint {
                mods: "clear".into(),
            },
        )
        .unwrap();
        assert_eq!(m.inputs(Instant::now()).unwrap().hint, Some(G));
        m.set_submap(None);
        assert_eq!(m.inputs(Instant::now()).unwrap().hint, None);
    }

    #[test]
    fn unconfigured_hint_is_an_error() {
        let mut m = hex_model();
        let e = req(
            &mut m,
            Request::Hint {
                mods: "ctrl+alt".into(),
            },
        )
        .unwrap_err();
        assert!(e.contains("no color configured for ctrl+alt"), "{e}");
    }

    #[test]
    fn effect_request_errors_when_disabled() {
        let mut m = model();
        let e = req(
            &mut m,
            Request::Effect {
                name: "wave".into(),
                speed: None,
            },
        )
        .unwrap_err();
        assert!(e.contains("enabled = true"), "{e}");
        let mut m = hex_model();
        assert_eq!(
            req(
                &mut m,
                Request::Effect {
                    name: "breathe".into(),
                    speed: Some(80)
                }
            ),
            Ok(Change::OutputAndPersist)
        );
        let i = m.inputs(Instant::now()).unwrap();
        assert_eq!((i.effect, i.speed), (Effect::Breathe, 80));
    }

    #[test]
    fn battery_pauses_effects() {
        let mut m = hex_model();
        assert!(m.inputs(Instant::now()).unwrap().animate);
        m.apply_fact(FactUpdate::OnBattery(true));
        assert!(!m.inputs(Instant::now()).unwrap().animate);
        assert!(!m.wants_animation());
    }

    #[test]
    fn fn_key_off_pauses_but_keeps_probing() {
        let mut m = hex_model();
        m.set_hw_backlight(false);
        assert!(!m.inputs(Instant::now()).unwrap().animate);
        assert!(
            m.wants_animation(),
            "keep probing so the Fn key can resume it"
        );
    }

    #[test]
    fn budget_error_disables_animation() {
        let mut m = hex_model();
        m.record_write(&Err(BackendError::Budget));
        assert!(!m.inputs(Instant::now()).unwrap().animate);
        let s = m.status(None);
        assert!(s.budget_exhausted);
        assert!(s.last_error.unwrap().contains("budget"));
        m.record_write(&Ok(true));
        assert!(m.inputs(Instant::now()).unwrap().animate);
        assert_eq!(m.persisted().lifetime_writes, 1);
        assert_eq!(m.status(None).last_error, None);
    }

    #[test]
    fn rule_profile_overrides_user_selection() {
        let mut m = hex_model();
        req(
            &mut m,
            Request::Flag {
                name: "gamemode".into(),
                on: true,
            },
        )
        .unwrap();
        let g = Rgb::new(0x20, 0x20, 0x20);
        assert_eq!(m.inputs(Instant::now()).unwrap().base, [g, g, g, R]);
        let s = m.status(None);
        assert_eq!(s.profile, "game");
        assert_eq!(s.active_rules, vec!["gamemode".to_string()]);
        assert_eq!(
            m.persisted().active_profile,
            "rgbw",
            "user selection is kept"
        );
    }

    #[test]
    fn unknown_flag_is_an_error() {
        let mut m = model();
        let e = req(
            &mut m,
            Request::Flag {
                name: "turbo".into(),
                on: true,
            },
        )
        .unwrap_err();
        assert!(e.contains("unknown flag: turbo"), "{e}");
    }

    #[test]
    fn brightness_cap_from_rule() {
        let mut m = hex_model();
        m.apply_fact(FactUpdate::PowerProfile("power-saver".into()));
        assert_eq!(m.inputs(Instant::now()).unwrap().brightness, 40);
    }

    #[test]
    fn unknown_theme_color_warns_and_uses_white() {
        let cfg = Config::parse(
            "[[profiles]]\nname = 'x'\ncolors = ['theme:nope', '#000000', '#000000', '#000000']\n",
        )
        .unwrap();
        let mut m = Model::new(cfg, palette(), PersistedState::default_for("x"), "mock");
        let i = m.inputs(Instant::now()).unwrap();
        assert_eq!(i.base[0], W);
        assert!(
            m.status(None)
                .warnings
                .iter()
                .any(|w| w.contains("unknown theme color: nope")),
            "{:?}",
            m.status(None).warnings
        );
    }

    #[test]
    fn suspended_yields_no_inputs() {
        let mut m = hex_model();
        req(
            &mut m,
            Request::Hint {
                mods: "super".into(),
            },
        )
        .unwrap();
        m.set_suspended(true);
        assert!(m.inputs(Instant::now()).is_none());
        m.set_suspended(false);
        assert_eq!(
            m.inputs(Instant::now()).unwrap().hint,
            None,
            "suspend cleared the hint"
        );
    }

    #[test]
    fn status_reports_output_zones() {
        let mut m = hex_model();
        set(&mut m, &["#ff0000", "#00ff00", "#0000ff", "#ffffff"]).unwrap();
        let out = compose(&m.inputs(Instant::now()).unwrap(), 0.0);
        let s = m.status(Some(&out));
        assert_eq!(s.zones, ["FF0000", "00FF00", "0000FF", "FFFFFF"]);
        assert_eq!(s.profile, "custom");
        assert_eq!(s.backend, "mock");
        assert!(s.backlight);
    }
}
