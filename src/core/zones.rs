//! Keyboard zones. Software always uses the logical order `[left, middle, right, wasd]`;
//! [`ZoneMap`] translates a logical [`Frame`] into the firmware's hardware zone order.

use serde::{Deserialize, Deserializer, Serialize, Serializer};

pub use crate::core::color::Rgb;

/// One of the four lighting zones, in logical order.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Zone {
    Left,
    Middle,
    Right,
    Wasd,
}

impl Zone {
    pub const ALL: [Zone; 4] = [Zone::Left, Zone::Middle, Zone::Right, Zone::Wasd];

    /// Logical index 0..=3.
    pub fn index(self) -> usize {
        self as usize
    }

    pub fn name(self) -> &'static str {
        match self {
            Zone::Left => "left",
            Zone::Middle => "middle",
            Zone::Right => "right",
            Zone::Wasd => "wasd",
        }
    }

    /// Case-insensitive; `center` is accepted as an alias of `middle`.
    pub fn from_name(s: &str) -> Option<Zone> {
        match s.to_ascii_lowercase().as_str() {
            "left" => Some(Zone::Left),
            "middle" | "center" => Some(Zone::Middle),
            "right" => Some(Zone::Right),
            "wasd" => Some(Zone::Wasd),
            _ => None,
        }
    }
}

impl Serialize for Zone {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(self.name())
    }
}

impl<'de> Deserialize<'de> for Zone {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Zone, D::Error> {
        let s = String::deserialize(deserializer)?;
        Zone::from_name(&s).ok_or_else(|| {
            serde::de::Error::custom(format!(
                "unknown zone {s:?} (expected left, middle, right or wasd)"
            ))
        })
    }
}

/// Four colors in logical zone order `[left, middle, right, wasd]`.
pub type Frame = [Rgb; 4];

/// Logical zone → hardware zone index. A permutation of 0..=3.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ZoneMap {
    hw: [usize; 4],
}

impl Default for ZoneMap {
    /// left→2, middle→1, right→0, wasd→3 (omen-space's classic mapping and the upstream patch's order).
    fn default() -> Self {
        ZoneMap { hw: [2, 1, 0, 3] }
    }
}

impl ZoneMap {
    pub fn new(left: usize, middle: usize, right: usize, wasd: usize) -> Result<ZoneMap, String> {
        let hw = [left, middle, right, wasd];
        let mut seen = [false; 4];
        for &i in &hw {
            if i > 3 || seen[i] {
                return Err(format!(
                    "zone_map must use each hardware index 0-3 exactly once, got {hw:?}"
                ));
            }
            seen[i] = true;
        }
        Ok(ZoneMap { hw })
    }

    /// Reorders a logical frame into hardware order.
    pub fn to_hw(&self, f: &Frame) -> [Rgb; 4] {
        let mut out = [Rgb::BLACK; 4];
        for (logical, &hw) in self.hw.iter().enumerate() {
            out[hw] = f[logical];
        }
        out
    }
}

impl<'de> Deserialize<'de> for ZoneMap {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<ZoneMap, D::Error> {
        #[derive(Deserialize)]
        #[serde(deny_unknown_fields)]
        struct Raw {
            left: usize,
            middle: usize,
            right: usize,
            wasd: usize,
        }
        let r = Raw::deserialize(deserializer)?;
        ZoneMap::new(r.left, r.middle, r.right, r.wasd).map_err(serde::de::Error::custom)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const R: Rgb = Rgb::new(255, 0, 0);
    const G: Rgb = Rgb::new(0, 255, 0);
    const B: Rgb = Rgb::new(0, 0, 255);

    #[test]
    fn default_map_matches_spec() {
        assert_eq!(
            ZoneMap::default().to_hw(&[R, G, B, Rgb::WHITE]),
            [B, G, R, Rgb::WHITE]
        );
    }

    #[test]
    fn rejects_non_permutation() {
        assert!(ZoneMap::new(0, 0, 1, 2).is_err());
        assert!(ZoneMap::new(0, 1, 2, 4).is_err());
    }

    #[test]
    fn zone_names() {
        assert_eq!(Zone::from_name("WASD"), Some(Zone::Wasd));
        assert_eq!(Zone::from_name("center"), Some(Zone::Middle));
        assert_eq!(Zone::from_name("top"), None);
    }

    #[test]
    fn zone_map_from_toml() {
        let m: ZoneMap = toml::from_str("left = 0\nmiddle = 1\nright = 2\nwasd = 3").unwrap();
        assert_eq!(m.to_hw(&[R, G, B, Rgb::WHITE]), [R, G, B, Rgb::WHITE]);
        assert!(toml::from_str::<ZoneMap>("left = 0\nmiddle = 0\nright = 2\nwasd = 3").is_err());
    }

    #[test]
    fn zone_serde_is_lowercase_name_with_center_alias() {
        assert_eq!(serde_json::to_string(&Zone::Middle).unwrap(), "\"middle\"");
        assert_eq!(
            serde_json::from_str::<Zone>("\"center\"").unwrap(),
            Zone::Middle
        );
        assert!(serde_json::from_str::<Zone>("\"top\"").is_err());
    }
}
