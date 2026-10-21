//! Theme palettes (from the rice's theming files) and color references
//! (`#rrggbb` or `theme:<name>`).

use std::collections::HashMap;
use std::fmt;
use std::path::Path;

use serde::{Deserialize, Deserializer, Serialize, Serializer};

pub use crate::core::color::{ColorError, Rgb};

/// Named colors of the current desktop theme.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Palette {
    colors: HashMap<String, Rgb>,
}

impl Palette {
    /// Parses `@define-color <name> #rrggbb;` lines; anything else (comments, references to other
    /// colors, malformed values) is skipped.
    pub fn from_waybar_css(s: &str) -> Palette {
        let mut colors = HashMap::new();
        for line in s.lines() {
            let Some(rest) = line.trim().strip_prefix("@define-color") else {
                continue;
            };
            let mut parts = rest.split_whitespace();
            let (Some(name), Some(value)) = (parts.next(), parts.next()) else {
                continue;
            };
            if let Ok(c) = Rgb::parse(value.trim_end_matches(';')) {
                colors.insert(name.to_string(), c);
            }
        }
        Palette { colors }
    }

    /// Parses an `omen-kbd.toml` theme file: a `[palette]` table of `name = "#rrggbb"`.
    pub fn from_theme_toml(s: &str) -> Result<Palette, String> {
        #[derive(Deserialize)]
        #[serde(deny_unknown_fields)]
        struct File {
            #[serde(default)]
            palette: HashMap<String, Rgb>,
        }
        let f: File = toml::from_str(s).map_err(|e| e.to_string())?;
        Ok(Palette { colors: f.palette })
    }

    /// Loads `<theme_dir>/omen-kbd.toml` if present, else `<theme_dir>/waybar-colors.css`.
    pub fn load(theme_dir: &Path) -> Result<Palette, String> {
        let toml_path = theme_dir.join("omen-kbd.toml");
        match std::fs::read_to_string(&toml_path) {
            Ok(s) => {
                return Palette::from_theme_toml(&s)
                    .map_err(|e| format!("{}: {e}", toml_path.display()));
            }
            Err(e) if e.kind() != std::io::ErrorKind::NotFound => {
                return Err(format!("{}: {e}", toml_path.display()));
            }
            Err(_) => {}
        }
        let css_path = theme_dir.join("waybar-colors.css");
        std::fs::read_to_string(&css_path)
            .map(|s| Palette::from_waybar_css(&s))
            .map_err(|e| format!("no theme palette in {}: {e}", theme_dir.display()))
    }

    pub fn get(&self, name: &str) -> Option<Rgb> {
        self.colors.get(name).copied()
    }

    pub fn len(&self) -> usize {
        self.colors.len()
    }

    pub fn is_empty(&self) -> bool {
        self.colors.is_empty()
    }
}

/// How `theme:` colors are rendered: boosted for LEDs (`vivid`) or as the theme defines them.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ThemeColors {
    #[default]
    Vivid,
    Exact,
}

/// A color as written in config: an explicit `#rrggbb` or a `theme:<name>` palette reference.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ColorRef {
    Hex(Rgb),
    Theme(String),
}

impl ColorRef {
    pub fn parse(s: &str) -> Result<ColorRef, ColorError> {
        if let Some(name) = s.strip_prefix("theme:") {
            let name = name.trim();
            if name.is_empty() {
                return Err(ColorError(format!(
                    "invalid color {s:?}: theme color needs a name"
                )));
            }
            return Ok(ColorRef::Theme(name.to_string()));
        }
        Rgb::parse(s).map(ColorRef::Hex).map_err(|_| {
            ColorError(format!(
                "invalid color {s:?}: expected #RRGGBB or theme:<name>"
            ))
        })
    }

    /// Resolves against the palette. `Err` carries the missing theme color's name.
    /// Vivid mode applies to theme references only, never to explicit hex colors.
    pub fn resolve(&self, p: &Palette, mode: ThemeColors) -> Result<Rgb, String> {
        match self {
            ColorRef::Hex(c) => Ok(*c),
            ColorRef::Theme(name) => {
                let c = p.get(name).ok_or_else(|| name.clone())?;
                Ok(match mode {
                    ThemeColors::Vivid => c.vivid(),
                    ThemeColors::Exact => c,
                })
            }
        }
    }
}

impl fmt::Display for ColorRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ColorRef::Hex(c) => write!(f, "#{}", c.to_hex()),
            ColorRef::Theme(name) => write!(f, "theme:{name}"),
        }
    }
}

impl Serialize for ColorRef {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(&self.to_string())
    }
}

impl<'de> Deserialize<'de> for ColorRef {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<ColorRef, D::Error> {
        let s = String::deserialize(deserializer)?;
        ColorRef::parse(&s).map_err(serde::de::Error::custom)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::color::assert_close;

    const CSS: &str = "/* palette */\n@define-color mauve #cba6f7;\n@define-color blue #89b4fa;\n@define-color bad #zzz;\n";

    #[test]
    fn parses_waybar_css_skipping_malformed() {
        let p = Palette::from_waybar_css(CSS);
        assert_eq!(p.get("mauve"), Some(Rgb::new(0xcb, 0xa6, 0xf7)));
        assert_eq!(p.len(), 2);
    }

    #[test]
    fn theme_toml_preferred_over_css() {
        let d = tempfile::tempdir().unwrap();
        std::fs::write(d.path().join("waybar-colors.css"), CSS).unwrap();
        std::fs::write(
            d.path().join("omen-kbd.toml"),
            "[palette]\nmauve = \"#ff00ff\"\n",
        )
        .unwrap();
        assert_eq!(
            Palette::load(d.path()).unwrap().get("mauve"),
            Some(Rgb::new(255, 0, 255))
        );
    }

    #[test]
    fn load_errors_when_no_palette_file() {
        assert!(Palette::load(tempfile::tempdir().unwrap().path()).is_err());
    }

    #[test]
    fn color_ref_parse() {
        assert_eq!(
            ColorRef::parse("theme:mauve").unwrap(),
            ColorRef::Theme("mauve".into())
        );
        assert_eq!(
            ColorRef::parse("#ff0000").unwrap(),
            ColorRef::Hex(Rgb::new(255, 0, 0))
        );
        assert!(ColorRef::parse("theme:").is_err());
    }

    #[test]
    fn resolve_vivid_exact_and_missing() {
        let p = Palette::from_waybar_css(CSS);
        let r = ColorRef::Theme("mauve".into());
        assert_eq!(
            r.resolve(&p, ThemeColors::Exact).unwrap(),
            Rgb::new(0xcb, 0xa6, 0xf7)
        );
        assert_close(
            r.resolve(&p, ThemeColors::Vivid).unwrap(),
            Rgb::new(0x89, 0x26, 0xff),
            1,
        );
        assert_eq!(
            ColorRef::Hex(Rgb::new(0xcb, 0xa6, 0xf7))
                .resolve(&p, ThemeColors::Vivid)
                .unwrap(),
            Rgb::new(0xcb, 0xa6, 0xf7)
        );
        assert_eq!(
            ColorRef::Theme("nope".into()).resolve(&p, ThemeColors::Vivid),
            Err("nope".into())
        );
    }

    #[test]
    fn serde_forms() {
        assert_eq!(
            serde_json::from_str::<ColorRef>("\"theme:teal\"").unwrap(),
            ColorRef::Theme("teal".into())
        );
        assert_eq!(
            serde_json::to_string(&ColorRef::Hex(Rgb::new(255, 0, 0))).unwrap(),
            "\"#FF0000\""
        );
        assert!(serde_json::from_str::<ColorRef>("\"red\"").is_err());
        assert_eq!(
            serde_json::from_str::<ThemeColors>("\"exact\"").unwrap(),
            ThemeColors::Exact
        );
        assert_eq!(ThemeColors::default(), ThemeColors::Vivid);
    }
}
