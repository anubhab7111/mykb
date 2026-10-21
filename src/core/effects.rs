//! Zone-level software effects. All functions are pure: a frame is a function of the base colors,
//! the time since the effect started, and the speed.

use std::f64::consts::TAU;
use std::str::FromStr;

use serde::{Deserialize, Serialize};

pub use crate::core::color::Rgb;
pub use crate::core::zones::Frame;

/// A profile's lighting effect. Only `Static` costs no hardware writes over time.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Effect {
    #[default]
    Static,
    Breathe,
    Wave,
    Cycle,
    Gradient,
}

impl Effect {
    pub fn is_animated(self) -> bool {
        self != Effect::Static
    }

    pub fn name(self) -> &'static str {
        match self {
            Effect::Static => "static",
            Effect::Breathe => "breathe",
            Effect::Wave => "wave",
            Effect::Cycle => "cycle",
            Effect::Gradient => "gradient",
        }
    }
}

impl FromStr for Effect {
    type Err = String;

    fn from_str(s: &str) -> Result<Effect, String> {
        match s.to_ascii_lowercase().as_str() {
            "static" => Ok(Effect::Static),
            "breathe" => Ok(Effect::Breathe),
            "wave" => Ok(Effect::Wave),
            "cycle" => Ok(Effect::Cycle),
            "gradient" => Ok(Effect::Gradient),
            _ => Err(format!(
                "unknown effect {s:?} (expected static, breathe, wave, cycle or gradient)"
            )),
        }
    }
}

/// Effect period in seconds: speed 100 ⇒ 0.5 s, speed 1 ⇒ 10 s (speed clamped to 1..=100).
pub fn period_secs(speed: u8) -> f64 {
    let s = f64::from(speed.clamp(1, 100));
    0.5 + 9.5 * (100.0 - s) / 99.0
}

/// Brightness factor for pulsing alert overlays: 1 s period, range [0.2, 1], 1 at t = 0.
pub fn pulse_factor(t: f64) -> f64 {
    0.6 + 0.4 * (TAU * t).cos()
}

/// Wave/cycle position of each logical zone: left, middle, right, and WASD (which sits on the left).
const POS: [f64; 4] = [0.0, 1.0, 2.0, 0.0];

/// Renders `effect` at time `t` seconds. `Static` returns `base` unchanged.
pub fn render(effect: Effect, base: &Frame, t: f64, speed: u8) -> Frame {
    let p = period_secs(speed);
    match effect {
        Effect::Static => *base,
        Effect::Breathe => {
            let f = 0.55 + 0.45 * (TAU * t / p).cos();
            base.map(|c| multiply(c, f))
        }
        Effect::Wave => std::array::from_fn(|i| {
            let k = 0.5 - 0.5 * (TAU * (t / p - POS[i] / 3.0)).cos();
            Rgb::lerp(base[0], base[1], k)
        }),
        Effect::Cycle => {
            std::array::from_fn(|i| Rgb::from_hsv((t / p + POS[i] / 3.0).rem_euclid(1.0), 1.0, 1.0))
        }
        Effect::Gradient => std::array::from_fn(|i| {
            let x = 4.0 * t / p + i as f64;
            let k = x.floor().rem_euclid(4.0) as usize;
            Rgb::lerp(base[k], base[(k + 1) % 4], x - x.floor())
        }),
    }
}

/// Multiplies every channel by `f` in [0, 1], rounding half away from zero.
fn multiply(c: Rgb, f: f64) -> Rgb {
    let m = |x: u8| (f64::from(x) * f.clamp(0.0, 1.0)).round() as u8;
    Rgb::new(m(c.r), m(c.g), m(c.b))
}

#[cfg(test)]
mod tests {
    use super::*;

    const R: Rgb = Rgb::new(255, 0, 0);
    const G: Rgb = Rgb::new(0, 255, 0);
    const B: Rgb = Rgb::new(0, 0, 255);
    const BASE: Frame = [R, G, B, Rgb::WHITE];

    fn p() -> f64 {
        period_secs(50)
    }

    #[test]
    fn period_bounds() {
        assert_eq!(period_secs(100), 0.5);
        assert_eq!(period_secs(1), 10.0);
        assert_eq!(period_secs(0), 10.0, "speed is clamped to 1..=100");
    }

    #[test]
    fn static_is_identity() {
        assert_eq!(render(Effect::Static, &BASE, 3.7, 50), BASE);
    }

    #[test]
    fn breathe_full_at_zero_dim_at_half() {
        assert_eq!(render(Effect::Breathe, &BASE, 0.0, 50), BASE);
        assert_eq!(
            render(Effect::Breathe, &BASE, p() / 2.0, 50)[0],
            Rgb::new(26, 0, 0)
        );
    }

    #[test]
    fn wave_phases() {
        let f = render(Effect::Wave, &BASE, 0.0, 50);
        assert_eq!(f[0], R);
        assert_eq!(f[3], R);
        assert_eq!(f[1], Rgb::new(64, 191, 0));
    }

    #[test]
    fn cycle_starts_at_primaries() {
        assert_eq!(render(Effect::Cycle, &BASE, 0.0, 50), [R, G, B, R]);
    }

    #[test]
    fn gradient_starts_at_base_and_blends() {
        assert_eq!(render(Effect::Gradient, &BASE, 0.0, 50), BASE);
        assert_eq!(
            render(Effect::Gradient, &BASE, p() / 8.0, 50)[0],
            Rgb::new(128, 128, 0)
        );
    }

    #[test]
    fn pulse_range() {
        assert_eq!(pulse_factor(0.0), 1.0);
        assert!((pulse_factor(0.5) - 0.2).abs() < 1e-9);
    }

    #[test]
    fn names_and_animation_flag() {
        assert_eq!(
            serde_json::to_string(&Effect::Breathe).unwrap(),
            "\"breathe\""
        );
        assert_eq!("wave".parse::<Effect>().unwrap(), Effect::Wave);
        assert!("sparkle".parse::<Effect>().is_err());
        assert!(!Effect::Static.is_animated());
        assert!(Effect::Cycle.is_animated());
    }
}
