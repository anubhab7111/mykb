//! Layer compositor: base effect → rule overlay → mode hint → brightness → backlight gate.
//! Pure: the output is a function of the inputs and the time since start.

pub use crate::core::color::Rgb;
use crate::core::effects::multiply;
pub use crate::core::effects::{Effect, pulse_factor, render};
pub use crate::core::zones::{Frame, Zone};

/// A rule overlay with its colors already resolved against the palette.
#[derive(Debug, Clone, PartialEq)]
pub enum ResolvedOverlay {
    Zones {
        zones: Vec<Zone>,
        color: Rgb,
        pulse: bool,
    },
    Tint {
        color: Rgb,
        strength: f64,
    },
}

/// Everything the compositor needs for one frame (logical zone order).
#[derive(Debug, Clone, PartialEq)]
pub struct Inputs {
    pub base: Frame,
    pub effect: Effect,
    pub speed: u8,
    /// Whether animation is allowed right now (effects enabled, not paused by a gate).
    pub animate: bool,
    pub overlay: Option<ResolvedOverlay>,
    pub hint: Option<Rgb>,
    pub brightness: u8,
    /// Whether the backlight should be on (user on, not gated off).
    pub backlight: bool,
}

/// The frame to show, in logical zone order.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Output {
    pub backlight: bool,
    pub zones: Frame,
    /// True while the frame changes over time (the daemon must keep ticking).
    pub animated: bool,
}

pub fn compose(i: &Inputs, t: f64) -> Output {
    let mut zones = if i.animate && i.effect.is_animated() {
        render(i.effect, &i.base, t, i.speed)
    } else {
        i.base
    };

    let mut pulsing = false;
    match &i.overlay {
        Some(ResolvedOverlay::Tint { color, strength }) => {
            for z in zones.iter_mut() {
                *z = Rgb::lerp(*z, *color, *strength);
            }
        }
        Some(ResolvedOverlay::Zones {
            zones: which,
            color,
            pulse,
        }) => {
            let c = if *pulse && i.animate {
                pulsing = true;
                multiply(*color, pulse_factor(t))
            } else {
                *color
            };
            for z in which {
                zones[z.index()] = c;
            }
        }
        None => {}
    }

    if let Some(h) = i.hint {
        zones = [h; 4];
    }

    let zones = zones.map(|z| z.scale(i.brightness));
    let backlight = i.backlight && i.brightness > 0;
    let animated =
        backlight && i.hint.is_none() && i.animate && (i.effect.is_animated() || pulsing);
    Output {
        backlight,
        zones,
        animated,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const R: Rgb = Rgb::new(255, 0, 0);
    const G: Rgb = Rgb::new(0, 255, 0);
    const B: Rgb = Rgb::new(0, 0, 255);
    const W: Rgb = Rgb::WHITE;
    const BASE: Frame = [R, G, B, W];

    fn inputs() -> Inputs {
        Inputs {
            base: BASE,
            effect: Effect::Static,
            speed: 50,
            animate: false,
            overlay: None,
            hint: None,
            brightness: 100,
            backlight: true,
        }
    }

    #[test]
    fn static_passthrough() {
        assert_eq!(
            compose(&inputs(), 3.0),
            Output {
                backlight: true,
                zones: BASE,
                animated: false
            }
        );
    }

    #[test]
    fn effect_ignored_when_not_animating() {
        let mut i = inputs();
        i.effect = Effect::Cycle;
        let out = compose(&i, 0.0);
        assert_eq!(out.zones, BASE);
        assert!(!out.animated);
        i.animate = true;
        let out = compose(&i, 0.0);
        assert_eq!(out.zones, [R, G, B, R], "cycle at t=0 when animating");
        assert!(out.animated);
    }

    #[test]
    fn tint_half_blends() {
        let mut i = inputs();
        i.overlay = Some(ResolvedOverlay::Tint {
            color: Rgb::BLACK,
            strength: 0.5,
        });
        let z = compose(&i, 0.0).zones;
        assert_eq!(z[0], Rgb::new(128, 0, 0));
        assert_eq!(z[3], Rgb::new(128, 128, 128));
    }

    #[test]
    fn zone_overlay_replaces_only_named_zones() {
        let mut i = inputs();
        i.overlay = Some(ResolvedOverlay::Zones {
            zones: vec![Zone::Right],
            color: Rgb::new(10, 20, 30),
            pulse: false,
        });
        assert_eq!(compose(&i, 0.0).zones, [R, G, Rgb::new(10, 20, 30), W]);
    }

    #[test]
    fn hint_beats_effect_and_overlay() {
        let mut i = inputs();
        i.effect = Effect::Cycle;
        i.animate = true;
        i.overlay = Some(ResolvedOverlay::Tint {
            color: Rgb::BLACK,
            strength: 0.5,
        });
        i.hint = Some(Rgb::new(0x89, 0x26, 0xff));
        assert_eq!(compose(&i, 1.3).zones, [Rgb::new(0x89, 0x26, 0xff); 4]);
    }

    #[test]
    fn brightness_scales_and_zero_turns_backlight_off() {
        let mut i = inputs();
        i.brightness = 50;
        let out = compose(&i, 0.0);
        assert_eq!(out.zones[0], Rgb::new(128, 0, 0));
        assert!(out.backlight);
        i.brightness = 0;
        i.effect = Effect::Wave;
        i.animate = true;
        let out = compose(&i, 0.0);
        assert!(!out.backlight);
        assert!(!out.animated);
    }

    #[test]
    fn backlight_input_false_means_off_and_not_animated() {
        let mut i = inputs();
        i.backlight = false;
        i.effect = Effect::Wave;
        i.animate = true;
        let out = compose(&i, 0.0);
        assert!(!out.backlight);
        assert!(!out.animated);
    }

    #[test]
    fn animated_false_with_hint() {
        let mut i = inputs();
        i.effect = Effect::Breathe;
        i.animate = true;
        i.hint = Some(G);
        assert!(!compose(&i, 0.0).animated);
    }

    #[test]
    fn pulse_overlay_animates_only_when_allowed() {
        let mut i = inputs();
        i.overlay = Some(ResolvedOverlay::Zones {
            zones: vec![Zone::Right],
            color: R,
            pulse: true,
        });
        let out = compose(&i, 0.5);
        assert_eq!(out.zones[2], R);
        assert!(!out.animated);
        i.animate = true;
        let out = compose(&i, 0.5);
        assert_eq!(out.zones[2], Rgb::new(51, 0, 0));
        assert!(out.animated, "a pulsing overlay keeps the frame animated");
    }
}
