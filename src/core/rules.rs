//! Declarative rules: conditions over system facts that switch the profile, cap brightness, or add
//! an overlay. Evaluation is first-match per category.

use serde::{Deserialize, Serialize};

pub use crate::core::palette::ColorRef;
pub use crate::core::zones::{Rgb, Zone};

/// What the daemon currently knows about the system. `None` = not known (yet).
#[derive(Serialize, Deserialize, Default, Clone, Debug, PartialEq)]
pub struct Facts {
    pub power_profile: Option<String>,
    pub on_battery: Option<bool>,
    pub battery_pct: Option<f64>,
    pub cpu_temp_c: Option<f64>,
    pub gamemode: bool,
    pub locked: bool,
    pub idle: bool,
}

/// All present fields must hold. A condition on an unknown (`None`) fact does not hold.
#[derive(Deserialize, Default, Clone, Debug, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Condition {
    pub power_profile: Option<String>,
    pub on_battery: Option<bool>,
    pub battery_below: Option<f64>,
    pub cpu_temp_above: Option<f64>,
    pub gamemode: Option<bool>,
    pub locked: Option<bool>,
    pub idle: Option<bool>,
}

impl Condition {
    pub fn holds(&self, f: &Facts) -> bool {
        if let Some(p) = &self.power_profile
            && f.power_profile.as_deref() != Some(p.as_str())
        {
            return false;
        }
        if let Some(b) = self.on_battery
            && f.on_battery != Some(b)
        {
            return false;
        }
        if let Some(limit) = self.battery_below
            && !f.battery_pct.is_some_and(|pct| pct < limit)
        {
            return false;
        }
        if let Some(limit) = self.cpu_temp_above
            && !f.cpu_temp_c.is_some_and(|c| c > limit)
        {
            return false;
        }
        let flags = [
            (self.gamemode, f.gamemode),
            (self.locked, f.locked),
            (self.idle, f.idle),
        ];
        flags
            .iter()
            .all(|(want, have)| want.is_none_or(|w| w == *have))
    }
}

#[derive(Deserialize, Clone, Debug, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct OverlayCfg {
    pub zones: Vec<Zone>,
    pub color: ColorRef,
    #[serde(default)]
    pub pulse: bool,
}

#[derive(Deserialize, Clone, Debug, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct TintCfg {
    pub color: ColorRef,
    pub strength: f64,
}

#[derive(Deserialize, Clone, Debug, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Rule {
    pub name: String,
    #[serde(default)]
    pub when: Condition,
    pub profile: Option<String>,
    pub brightness_cap: Option<u8>,
    pub overlay: Option<OverlayCfg>,
    pub tint: Option<TintCfg>,
}

impl Rule {
    pub fn validate(&self) -> Result<(), String> {
        let name = &self.name;
        if self.overlay.is_some() && self.tint.is_some() {
            return Err(format!(
                "rule {name:?}: use either overlay or tint, not both"
            ));
        }
        if self.profile.is_none()
            && self.brightness_cap.is_none()
            && self.overlay.is_none()
            && self.tint.is_none()
        {
            return Err(format!(
                "rule {name:?}: needs an action (profile, brightness_cap, overlay or tint)"
            ));
        }
        if let Some(cap) = self.brightness_cap
            && cap > 100
        {
            return Err(format!(
                "rule {name:?}: brightness_cap must be 0-100, got {cap}"
            ));
        }
        if let Some(t) = &self.tint
            && !(0.0..=1.0).contains(&t.strength)
        {
            return Err(format!(
                "rule {name:?}: tint strength must be between 0 and 1, got {}",
                t.strength
            ));
        }
        if let Some(o) = &self.overlay
            && o.zones.is_empty()
        {
            return Err(format!("rule {name:?}: overlay needs at least one zone"));
        }
        Ok(())
    }
}

/// An overlay chosen by the rules, before its colors are resolved against the palette.
#[derive(Clone, Debug, PartialEq)]
pub enum Overlay {
    Zones {
        zones: Vec<Zone>,
        color: ColorRef,
        pulse: bool,
    },
    Tint {
        color: ColorRef,
        strength: f64,
    },
}

#[derive(Default, Clone, Debug, PartialEq)]
pub struct RuleOutcome {
    pub profile: Option<String>,
    pub brightness_cap: Option<u8>,
    pub overlay: Option<Overlay>,
    /// Names of the rules that contributed to this outcome, in rule order.
    pub active: Vec<String>,
}

/// First matching rule wins per category: profile, brightness cap, overlay (overlay or tint).
pub fn evaluate(rules: &[Rule], facts: &Facts) -> RuleOutcome {
    let mut out = RuleOutcome::default();
    for r in rules.iter().filter(|r| r.when.holds(facts)) {
        let mut contributed = false;
        if out.profile.is_none()
            && let Some(p) = &r.profile
        {
            out.profile = Some(p.clone());
            contributed = true;
        }
        if out.brightness_cap.is_none()
            && let Some(cap) = r.brightness_cap
        {
            out.brightness_cap = Some(cap);
            contributed = true;
        }
        if out.overlay.is_none() {
            if let Some(o) = &r.overlay {
                out.overlay = Some(Overlay::Zones {
                    zones: o.zones.clone(),
                    color: o.color.clone(),
                    pulse: o.pulse,
                });
                contributed = true;
            } else if let Some(t) = &r.tint {
                out.overlay = Some(Overlay::Tint {
                    color: t.color.clone(),
                    strength: t.strength,
                });
                contributed = true;
            }
        }
        if contributed {
            out.active.push(r.name.clone());
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rules(src: &str) -> Vec<Rule> {
        #[derive(serde::Deserialize)]
        struct W {
            rules: Vec<Rule>,
        }
        toml::from_str::<W>(src).expect("valid rules toml").rules
    }

    #[test]
    fn first_match_per_category() {
        let r = rules(
            "[[rules]]\nname = 'a'\nwhen = {}\nprofile = 'game'\n\
             [[rules]]\nname = 'b'\nwhen = {}\nprofile = 'white'\n\
             [[rules]]\nname = 'c'\nwhen = {}\nbrightness_cap = 40\n",
        );
        let out = evaluate(&r, &Facts::default());
        assert_eq!(out.profile.as_deref(), Some("game"));
        assert_eq!(out.brightness_cap, Some(40));
    }

    #[test]
    fn all_conditions_must_hold() {
        let r = rules(
            "[[rules]]\nname = 'low'\nwhen = { on_battery = true, battery_below = 15 }\nprofile = 'low'\n",
        );
        let mut f = Facts {
            on_battery: Some(true),
            battery_pct: Some(20.0),
            ..Facts::default()
        };
        assert_eq!(evaluate(&r, &f).profile, None);
        f.battery_pct = Some(10.0);
        assert_eq!(evaluate(&r, &f).profile.as_deref(), Some("low"));
        f.on_battery = Some(false);
        assert_eq!(evaluate(&r, &f).profile, None);
    }

    #[test]
    fn unknown_fact_is_false() {
        let r = rules(
            "[[rules]]\nname = 'ps'\nwhen = { power_profile = 'power-saver' }\nbrightness_cap = 40\n",
        );
        assert_eq!(evaluate(&r, &Facts::default()).brightness_cap, None);
        let f = Facts {
            power_profile: Some("power-saver".into()),
            ..Facts::default()
        };
        assert_eq!(evaluate(&r, &f).brightness_cap, Some(40));
    }

    #[test]
    fn empty_condition_always_matches() {
        let r = rules("[[rules]]\nname = 'always'\nwhen = {}\nbrightness_cap = 10\n");
        assert_eq!(evaluate(&r, &Facts::default()).brightness_cap, Some(10));
    }

    #[test]
    fn bool_and_threshold_conditions() {
        let r = rules(
            "[[rules]]\nname = 'hot'\nwhen = { cpu_temp_above = 90 }\ntint = { color = '#ff4000', strength = 0.5 }\n\
             [[rules]]\nname = 'gm'\nwhen = { gamemode = true, locked = false, idle = false }\nprofile = 'game'\n",
        );
        let f = Facts {
            cpu_temp_c: Some(95.0),
            gamemode: true,
            ..Facts::default()
        };
        let out = evaluate(&r, &f);
        assert_eq!(
            out.overlay,
            Some(Overlay::Tint {
                color: ColorRef::Hex(Rgb::new(255, 0x40, 0)),
                strength: 0.5
            })
        );
        assert_eq!(out.profile.as_deref(), Some("game"));
        let cool_locked = Facts {
            cpu_temp_c: Some(90.0),
            gamemode: true,
            locked: true,
            ..Facts::default()
        };
        let out = evaluate(&r, &cool_locked);
        assert_eq!(out.overlay, None, "90 is not above 90");
        assert_eq!(out.profile, None, "locked = false does not hold");
    }

    #[test]
    fn active_lists_contributors() {
        let r = rules(
            "[[rules]]\nname = 'r1'\nwhen = {}\nprofile = 'game'\n\
             [[rules]]\nname = 'r2'\nwhen = {}\nprofile = 'white'\n\
             [[rules]]\nname = 'r3'\nwhen = {}\noverlay = { zones = ['right'], color = '#ff0000', pulse = true }\n",
        );
        let out = evaluate(&r, &Facts::default());
        assert_eq!(out.active, vec!["r1".to_string(), "r3".to_string()]);
        assert_eq!(
            out.overlay,
            Some(Overlay::Zones {
                zones: vec![Zone::Right],
                color: ColorRef::Hex(Rgb::new(255, 0, 0)),
                pulse: true
            })
        );
    }

    #[test]
    fn overlay_and_tint_both_is_invalid() {
        let both = rules(
            "[[rules]]\nname = 'x'\nwhen = {}\noverlay = { zones = ['left'], color = '#ff0000' }\ntint = { color = '#00ff00', strength = 0.5 }\n",
        );
        assert!(both[0].validate().is_err());
        let none = rules("[[rules]]\nname = 'x'\nwhen = {}\n");
        assert!(none[0].validate().is_err());
        let strong = rules(
            "[[rules]]\nname = 'x'\nwhen = {}\ntint = { color = '#00ff00', strength = 1.5 }\n",
        );
        assert!(strong[0].validate().is_err());
        let ok = rules("[[rules]]\nname = 'x'\nwhen = {}\nbrightness_cap = 30\n");
        assert!(ok[0].validate().is_ok());
    }

    #[test]
    fn unknown_condition_field_is_rejected() {
        #[derive(serde::Deserialize)]
        #[allow(dead_code)]
        struct W {
            rules: Vec<Rule>,
        }
        assert!(
            toml::from_str::<W>("[[rules]]\nname = 'x'\nwhen = { foo = 1 }\nprofile = 'a'\n")
                .is_err()
        );
    }
}
