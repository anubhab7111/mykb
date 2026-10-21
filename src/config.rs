//! User configuration, `~/.config/omen-kbd/config.toml`. The daemon only ever reads this file.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use serde::Deserialize;

use crate::core::effects::Effect;
use crate::core::palette::{ColorRef, ThemeColors};
use crate::core::rules::Rule;
use crate::core::zones::ZoneMap;

/// The shipped example config, also used when the user has no config file.
const EXAMPLE: &str = include_str!("../packaging/config.example.toml");

/// Profile name reserved for colors set with `omen-kbd set`.
pub const CUSTOM_PROFILE: &str = "custom";

const MODIFIER_ORDER: [&str; 4] = ["super", "ctrl", "alt", "shift"];

#[derive(thiserror::Error, Debug, Clone, PartialEq)]
#[error("{0}")]
pub struct ConfigError(pub String);

#[derive(Debug, Clone, PartialEq)]
pub struct Config {
    pub device: DeviceCfg,
    pub effects: EffectsCfg,
    pub theme: ThemeCfg,
    pub gates: GatesCfg,
    pub hints: HintsCfg,
    pub profiles: Vec<ProfileCfg>,
    pub rules: Vec<Rule>,
}

#[derive(Deserialize, Debug, Clone, Copy, PartialEq, Default)]
#[serde(deny_unknown_fields, default)]
pub struct DeviceCfg {
    pub zone_map: ZoneMap,
}

#[derive(Deserialize, Debug, Clone, Copy, PartialEq)]
#[serde(deny_unknown_fields, default)]
pub struct EffectsCfg {
    pub enabled: bool,
    pub fps: u8,
}

impl Default for EffectsCfg {
    fn default() -> Self {
        EffectsCfg {
            enabled: false,
            fps: 10,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct ThemeCfg {
    pub colors: ThemeColors,
    /// Directory holding the current theme's files (`~` already expanded).
    pub dir: PathBuf,
}

#[derive(Deserialize, Debug, Clone, Copy, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum GateAction {
    Off,
    Dim,
    Keep,
}

#[derive(Deserialize, Debug, Clone, Copy, PartialEq)]
#[serde(deny_unknown_fields, default)]
pub struct GatesCfg {
    pub on_lock: GateAction,
    pub on_idle: GateAction,
    pub dim_brightness: u8,
    pub pause_effects_on_battery: bool,
}

impl Default for GatesCfg {
    fn default() -> Self {
        GatesCfg {
            on_lock: GateAction::Off,
            on_idle: GateAction::Dim,
            dim_brightness: 20,
            pause_effects_on_battery: true,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct HintsCfg {
    pub timeout_s: u64,
    pub delay_ms: u64,
    /// Keys are normalized modifier sets (see [`normalize_mods`]).
    pub modifiers: BTreeMap<String, ColorRef>,
    pub submaps: BTreeMap<String, ColorRef>,
}

#[derive(Deserialize, Debug, Clone, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct ProfileCfg {
    pub name: String,
    /// One color (all zones) or four (left, middle, right, wasd).
    pub colors: Vec<ColorRef>,
    #[serde(default)]
    pub effect: Effect,
    #[serde(default = "default_speed")]
    pub speed: u8,
}

fn default_speed() -> u8 {
    50
}

#[derive(Deserialize, Default)]
#[serde(deny_unknown_fields, default)]
struct RawConfig {
    device: DeviceCfg,
    effects: EffectsCfg,
    theme: RawTheme,
    gates: GatesCfg,
    hints: RawHints,
    profiles: Vec<ProfileCfg>,
    rules: Vec<Rule>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields, default)]
struct RawTheme {
    colors: ThemeColors,
    dir: String,
}

impl Default for RawTheme {
    fn default() -> Self {
        RawTheme {
            colors: ThemeColors::Vivid,
            dir: "~/.config/theming/current".to_string(),
        }
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields, default)]
struct RawHints {
    timeout_s: u64,
    delay_ms: u64,
    modifiers: BTreeMap<String, ColorRef>,
    submaps: BTreeMap<String, ColorRef>,
}

impl Default for RawHints {
    fn default() -> Self {
        RawHints {
            timeout_s: 15,
            delay_ms: 250,
            modifiers: BTreeMap::new(),
            submaps: BTreeMap::new(),
        }
    }
}

impl Config {
    /// Parses and validates a config. Error messages name the offending field.
    pub fn parse(s: &str) -> Result<Config, ConfigError> {
        let raw: RawConfig = toml::from_str(s).map_err(|e| ConfigError(e.to_string()))?;
        let err = |m: String| Err(ConfigError(m));

        if raw.profiles.is_empty() {
            return err("config needs at least one [[profiles]] entry".into());
        }
        let mut names: Vec<&str> = Vec::new();
        for p in &raw.profiles {
            let n = p.name.as_str();
            if n.trim().is_empty() {
                return err("profiles: name must not be empty".into());
            }
            if n == CUSTOM_PROFILE {
                return err(format!(
                    "profile name {CUSTOM_PROFILE:?} is reserved for `omen-kbd set`"
                ));
            }
            if names.contains(&n) {
                return err(format!("duplicate profile name {n:?}"));
            }
            names.push(n);
            if p.colors.len() != 1 && p.colors.len() != 4 {
                return err(format!(
                    "profile {n:?}: colors must list 1 or 4 colors (left, middle, right, wasd), got {}",
                    p.colors.len()
                ));
            }
            if !(1..=100).contains(&p.speed) {
                return err(format!(
                    "profile {n:?}: speed must be 1-100, got {}",
                    p.speed
                ));
            }
        }
        if !(1..=20).contains(&raw.effects.fps) {
            return err(format!("effects.fps must be 1-20, got {}", raw.effects.fps));
        }
        if raw.gates.dim_brightness > 100 {
            return err(format!(
                "gates.dim_brightness must be 0-100, got {}",
                raw.gates.dim_brightness
            ));
        }
        if raw.hints.timeout_s == 0 {
            return err("hints.timeout_s must be at least 1".into());
        }
        let mut modifiers = BTreeMap::new();
        for (k, v) in raw.hints.modifiers {
            let key =
                normalize_mods(&k).map_err(|e| ConfigError(format!("hints.modifiers: {e}")))?;
            modifiers.insert(key, v);
        }
        for r in &raw.rules {
            r.validate().map_err(ConfigError)?;
            if let Some(p) = &r.profile
                && !names.contains(&p.as_str())
            {
                return err(format!("rule {:?}: unknown profile {p:?}", r.name));
            }
        }

        Ok(Config {
            device: raw.device,
            effects: raw.effects,
            theme: ThemeCfg {
                colors: raw.theme.colors,
                dir: expand_home(&raw.theme.dir),
            },
            gates: raw.gates,
            hints: HintsCfg {
                timeout_s: raw.hints.timeout_s,
                delay_ms: raw.hints.delay_ms,
                modifiers,
                submaps: raw.hints.submaps,
            },
            profiles: raw.profiles,
            rules: raw.rules,
        })
    }

    /// Loads `path`; a missing file yields [`Config::embedded_default`].
    pub fn load(path: &Path) -> Result<Config, ConfigError> {
        match std::fs::read_to_string(path) {
            Ok(s) => Config::parse(&s).map_err(|e| ConfigError(format!("{}: {e}", path.display()))),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Config::embedded_default()),
            Err(e) => Err(ConfigError(format!("{}: {e}", path.display()))),
        }
    }

    pub fn embedded_default() -> Config {
        Config::parse(EXAMPLE).expect("the shipped example config is valid")
    }

    /// `$XDG_CONFIG_HOME/omen-kbd/config.toml` (default `~/.config/omen-kbd/config.toml`).
    pub fn path() -> PathBuf {
        let base = std::env::var_os("XDG_CONFIG_HOME")
            .filter(|v| !v.is_empty())
            .map(PathBuf::from)
            .unwrap_or_else(|| expand_home("~/.config"));
        base.join("omen-kbd/config.toml")
    }

    pub fn profile(&self, name: &str) -> Option<&ProfileCfg> {
        self.profiles.iter().find(|p| p.name == name)
    }

    /// True if any rule needs the CPU temperature (so the hwmon poller should run).
    pub fn needs_cpu_temp(&self) -> bool {
        self.rules.iter().any(|r| r.when.cpu_temp_above.is_some())
    }
}

/// Normalizes a modifier set like `"Shift+SUPER"` to `"super+shift"`: case-insensitive,
/// `+`-separated, deduplicated, ordered super, ctrl, alt, shift.
pub fn normalize_mods(s: &str) -> Result<String, String> {
    let mut present = [false; 4];
    for part in s.split('+') {
        let part = part.trim().to_ascii_lowercase();
        match MODIFIER_ORDER.iter().position(|m| *m == part) {
            Some(i) => present[i] = true,
            None => {
                return Err(format!(
                    "unknown modifier {part:?} in {s:?} (use super, ctrl, alt, shift joined by +)"
                ));
            }
        }
    }
    Ok(MODIFIER_ORDER
        .iter()
        .zip(present)
        .filter(|(_, p)| *p)
        .map(|(m, _)| *m)
        .collect::<Vec<_>>()
        .join("+"))
}

/// Expands a leading `~` to `$HOME`.
pub fn expand_home(p: &str) -> PathBuf {
    let home = || {
        std::env::var_os("HOME")
            .map(PathBuf::from)
            .unwrap_or_default()
    };
    if p == "~" {
        home()
    } else if let Some(rest) = p.strip_prefix("~/") {
        home().join(rest)
    } else {
        PathBuf::from(p)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn example_config_parses() {
        let c = Config::embedded_default();
        assert_eq!(c.profiles.len(), 3);
        assert!(!c.effects.enabled);
        assert!(c.hints.modifiers.contains_key("super+shift"));
        assert_eq!(c.hints.timeout_s, 15);
        assert_eq!(c.gates.on_idle, GateAction::Dim);
        assert_eq!(c.rules.len(), 3);
    }

    #[test]
    fn missing_file_gives_default() {
        assert_eq!(
            Config::load(Path::new("/nonexistent/x.toml"))
                .unwrap()
                .profiles[0]
                .name,
            "default"
        );
    }

    #[test]
    fn normalizes_modifier_keys() {
        assert_eq!(normalize_mods("Shift+SUPER").unwrap(), "super+shift");
        assert_eq!(normalize_mods("alt+ctrl+alt").unwrap(), "ctrl+alt");
        assert!(normalize_mods("hyper").is_err());
        assert!(normalize_mods("").is_err());
    }

    #[test]
    fn modifier_keys_in_config_are_normalized() {
        let c = Config::parse(
            "[hints.modifiers]\n\"Shift+Super\" = '#0000ff'\n[[profiles]]\nname='a'\ncolors=['#000000']",
        )
        .unwrap();
        assert!(c.hints.modifiers.contains_key("super+shift"));
    }

    #[test]
    fn rejects_invalid() {
        for (snippet, needle) in [
            (
                "[[profiles]]\nname='a'\ncolors=['#000000','#000000']",
                "colors",
            ),
            (
                "[effects]\nfps=30\n[[profiles]]\nname='a'\ncolors=['#000000']",
                "fps",
            ),
            (
                "[[profiles]]\nname='a'\ncolors=['#000000']\n[[profiles]]\nname='a'\ncolors=['#000000']",
                "duplicate",
            ),
            ("[[profiles]]\nname='custom'\ncolors=['#000000']", "custom"),
            (
                "[[profiles]]\nname='a'\ncolors=['#000000']\n[[rules]]\nname='r'\nwhen={}\nprofile='zzz'",
                "zzz",
            ),
            (
                "[hints.modifiers]\nhyper='#000000'\n[[profiles]]\nname='a'\ncolors=['#000000']",
                "hyper",
            ),
            (
                "[[profiles]]\nname='a'\ncolors=['#000000']\nspeed=0",
                "speed",
            ),
            (
                "[bogus]\nx=1\n[[profiles]]\nname='a'\ncolors=['#000000']",
                "bogus",
            ),
            ("", "profile"),
        ] {
            let e = Config::parse(snippet).unwrap_err().0;
            assert!(e.contains(needle), "{snippet:?} -> {e}");
        }
    }

    #[test]
    fn theme_dir_expands_home() {
        let home = std::env::var("HOME").unwrap();
        let c = Config::parse("[theme]\ndir = '~/x/y'\n[[profiles]]\nname='a'\ncolors=['#000000']")
            .unwrap();
        assert_eq!(c.theme.dir, Path::new(&home).join("x/y"));
    }

    #[test]
    fn needs_cpu_temp_only_with_a_temperature_rule() {
        assert!(!Config::embedded_default().needs_cpu_temp());
        let c = Config::parse(
            "[[profiles]]\nname='a'\ncolors=['#000000']\n[[rules]]\nname='hot'\nwhen={ cpu_temp_above = 90 }\nbrightness_cap = 10",
        )
        .unwrap();
        assert!(c.needs_cpu_temp());
        assert_eq!(c.profile("a").unwrap().speed, 50);
        assert!(c.profile("b").is_none());
    }
}
