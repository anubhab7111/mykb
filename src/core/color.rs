//! 8-bit RGB colors: parsing, formatting, scaling, blending and HSV helpers.

use serde::{Deserialize, Deserializer, Serialize, Serializer};

/// An 8-bit RGB color.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Rgb {
    pub r: u8,
    pub g: u8,
    pub b: u8,
}

/// A color string that is not `#RRGGBB` / `RRGGBB`.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[error("{0}")]
pub struct ColorError(pub String);

impl Rgb {
    pub const BLACK: Rgb = Rgb::new(0, 0, 0);
    pub const WHITE: Rgb = Rgb::new(255, 255, 255);

    pub const fn new(r: u8, g: u8, b: u8) -> Rgb {
        Rgb { r, g, b }
    }

    /// Parses `#RRGGBB` or `RRGGBB` (case-insensitive).
    pub fn parse(s: &str) -> Result<Rgb, ColorError> {
        let hex = s.strip_prefix('#').unwrap_or(s);
        if hex.len() != 6 || !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err(ColorError(format!("invalid color {s:?}: expected #RRGGBB")));
        }
        let channel =
            |i: usize| u8::from_str_radix(&hex[i..i + 2], 16).expect("validated hex digits");
        Ok(Rgb::new(channel(0), channel(2), channel(4)))
    }

    /// Uppercase `RRGGBB`, without `#`.
    pub fn to_hex(self) -> String {
        format!("{:02X}{:02X}{:02X}", self.r, self.g, self.b)
    }

    /// Scales every channel by `pct` percent (clamped to 100), rounding half away from zero.
    pub fn scale(self, pct: u8) -> Rgb {
        let pct = f64::from(pct.min(100));
        let f = |c: u8| (f64::from(c) * pct / 100.0).round() as u8;
        Rgb::new(f(self.r), f(self.g), f(self.b))
    }

    /// Linear blend from `a` (t = 0) to `b` (t = 1); `t` is clamped to [0, 1].
    pub fn lerp(a: Rgb, b: Rgb, t: f64) -> Rgb {
        let t = t.clamp(0.0, 1.0);
        let f = |x: u8, y: u8| (f64::from(x) + (f64::from(y) - f64::from(x)) * t).round() as u8;
        Rgb::new(f(a.r, b.r), f(a.g, b.g), f(a.b, b.b))
    }

    /// Hue in [0, 1), saturation and value in [0, 1].
    pub fn to_hsv(self) -> (f64, f64, f64) {
        let (r, g, b) = (
            f64::from(self.r) / 255.0,
            f64::from(self.g) / 255.0,
            f64::from(self.b) / 255.0,
        );
        let max = r.max(g).max(b);
        let min = r.min(g).min(b);
        let d = max - min;
        let h = if d == 0.0 {
            0.0
        } else if max == r {
            ((g - b) / d).rem_euclid(6.0) / 6.0
        } else if max == g {
            ((b - r) / d + 2.0) / 6.0
        } else {
            ((r - g) / d + 4.0) / 6.0
        };
        let s = if max == 0.0 { 0.0 } else { d / max };
        (h, s, max)
    }

    /// Inverse of [`Rgb::to_hsv`]; `h` wraps, `s` and `v` are clamped to [0, 1].
    pub fn from_hsv(h: f64, s: f64, v: f64) -> Rgb {
        let (s, v) = (s.clamp(0.0, 1.0), v.clamp(0.0, 1.0));
        let h6 = h.rem_euclid(1.0) * 6.0;
        let sector = h6.floor();
        let f = h6 - sector;
        let p = v * (1.0 - s);
        let q = v * (1.0 - s * f);
        let t = v * (1.0 - s * (1.0 - f));
        let (r, g, b) = match sector as u8 % 6 {
            0 => (v, t, p),
            1 => (q, v, p),
            2 => (p, v, t),
            3 => (p, q, v),
            4 => (t, p, v),
            _ => (v, p, q),
        };
        let c = |x: f64| (x * 255.0).round() as u8;
        Rgb::new(c(r), c(g), c(b))
    }

    /// Keeps the hue but boosts saturation to at least 0.85 and value to 1, so pastel theme colors
    /// stay distinguishable on RGB LEDs. Near-greys (saturation < 0.1) are returned unchanged.
    pub fn vivid(self) -> Rgb {
        let (h, s, _) = self.to_hsv();
        if s < 0.1 {
            return self;
        }
        Rgb::from_hsv(h, s.max(0.85), 1.0)
    }
}

impl Serialize for Rgb {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(&format!("#{}", self.to_hex()))
    }
}

impl<'de> Deserialize<'de> for Rgb {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Rgb, D::Error> {
        let s = String::deserialize(deserializer)?;
        Rgb::parse(&s).map_err(serde::de::Error::custom)
    }
}

/// Test helper: every channel of `a` within `tol` of `b`.
#[cfg(test)]
#[track_caller]
pub fn assert_close(a: Rgb, b: Rgb, tol: u8) {
    let ok = a.r.abs_diff(b.r) <= tol && a.g.abs_diff(b.g) <= tol && a.b.abs_diff(b.b) <= tol;
    assert!(ok, "{a:?} not within {tol} of {b:?}");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_hex_with_and_without_hash() {
        assert_eq!(Rgb::parse("#cba6f7").unwrap(), Rgb::new(0xcb, 0xa6, 0xf7));
        assert_eq!(Rgb::parse("CBA6F7").unwrap(), Rgb::new(0xcb, 0xa6, 0xf7));
    }

    #[test]
    fn rejects_bad_hex() {
        for s in ["", "#fff", "#gggggg", "#1234567", "12345"] {
            assert!(Rgb::parse(s).is_err(), "{s}");
        }
    }

    #[test]
    fn formats_uppercase_hex() {
        assert_eq!(Rgb::new(0xcb, 0xa6, 0xf7).to_hex(), "CBA6F7");
    }

    #[test]
    fn scales_with_rounding() {
        assert_eq!(Rgb::new(255, 128, 0).scale(50), Rgb::new(128, 64, 0));
        assert_eq!(Rgb::WHITE.scale(0), Rgb::BLACK);
        assert_eq!(Rgb::new(1, 2, 3).scale(100), Rgb::new(1, 2, 3));
    }

    #[test]
    fn lerp_endpoints_and_mid() {
        assert_eq!(Rgb::lerp(Rgb::BLACK, Rgb::WHITE, 0.0), Rgb::BLACK);
        assert_eq!(Rgb::lerp(Rgb::BLACK, Rgb::WHITE, 1.0), Rgb::WHITE);
        assert_eq!(
            Rgb::lerp(Rgb::BLACK, Rgb::WHITE, 0.5),
            Rgb::new(128, 128, 128)
        );
    }

    #[test]
    fn hsv_roundtrip_within_one() {
        for c in [
            Rgb::new(203, 166, 247),
            Rgb::new(255, 0, 0),
            Rgb::new(17, 200, 90),
        ] {
            let (h, s, v) = c.to_hsv();
            assert_close(Rgb::from_hsv(h, s, v), c, 1);
        }
    }

    #[test]
    fn vivid_boosts_saturation_keeps_hue() {
        assert_close(
            Rgb::new(0xcb, 0xa6, 0xf7).vivid(),
            Rgb::new(0x89, 0x26, 0xff),
            1,
        );
    }

    #[test]
    fn vivid_leaves_greys() {
        assert_eq!(Rgb::new(128, 128, 128).vivid(), Rgb::new(128, 128, 128));
    }

    #[test]
    fn serde_uses_hash_hex_string() {
        assert_eq!(
            serde_json::to_string(&Rgb::new(1, 2, 255)).unwrap(),
            "\"#0102FF\""
        );
        assert_eq!(
            serde_json::from_str::<Rgb>("\"#0102ff\"").unwrap(),
            Rgb::new(1, 2, 255)
        );
        assert!(serde_json::from_str::<Rgb>("\"nope\"").is_err());
    }
}
