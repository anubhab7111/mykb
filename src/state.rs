//! Runtime state the daemon persists between restarts (`~/.local/state/omen-kbd/state.json`).
//! Writes are atomic (tmp file + fsync + rename) so a crash can never leave a half-written file.

use std::io::Write;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

pub use crate::core::effects::Effect;

/// A runtime override of the active profile's effect (`omen-kbd effect …`).
#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq)]
pub struct EffectOverride {
    pub effect: Effect,
    pub speed: u8,
}

/// Unknown fields are ignored and missing ones get defaults, so files from other versions load.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct PersistedState {
    #[serde(default)]
    pub active_profile: String,
    /// Colors set with `omen-kbd set` (1 or 4 color strings), shown as profile `custom`.
    #[serde(default)]
    pub custom: Option<Vec<String>>,
    #[serde(default)]
    pub effect_override: Option<EffectOverride>,
    #[serde(default = "full")]
    pub brightness: u8,
    #[serde(default = "yes")]
    pub on: bool,
    #[serde(default)]
    pub lifetime_writes: u64,
}

fn full() -> u8 {
    100
}

fn yes() -> bool {
    true
}

impl PersistedState {
    pub fn default_for(first_profile: &str) -> PersistedState {
        PersistedState {
            active_profile: first_profile.to_string(),
            custom: None,
            effect_override: None,
            brightness: 100,
            on: true,
            lifetime_writes: 0,
        }
    }

    /// Loads the state. A missing file gives defaults; a corrupt one is renamed to
    /// `<name>.bad` and reported in the returned warning.
    pub fn load(path: &Path, first_profile: &str) -> (PersistedState, Option<String>) {
        let text = match std::fs::read_to_string(path) {
            Ok(t) => t,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                return (PersistedState::default_for(first_profile), None);
            }
            Err(e) => {
                return (
                    PersistedState::default_for(first_profile),
                    Some(format!("cannot read {}: {e}", path.display())),
                );
            }
        };
        match serde_json::from_str::<PersistedState>(&text) {
            Ok(mut s) => {
                if s.active_profile.is_empty() {
                    s.active_profile = first_profile.to_string();
                }
                (s, None)
            }
            Err(e) => {
                let bad = with_suffix(path, ".bad");
                let moved = std::fs::rename(path, &bad).is_ok();
                let note = if moved {
                    format!("moved it to {}", bad.display())
                } else {
                    "could not move it aside".to_string()
                };
                (
                    PersistedState::default_for(first_profile),
                    Some(format!(
                        "corrupt state file {} ({e}); {note}; using defaults",
                        path.display()
                    )),
                )
            }
        }
    }

    /// Atomically replaces the file: write `<name>.tmp`, fsync, rename.
    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        let tmp = with_suffix(path, ".tmp");
        let json = serde_json::to_string_pretty(self).map_err(std::io::Error::other)?;
        let mut f = std::fs::File::create(&tmp)?;
        f.write_all(json.as_bytes())?;
        f.write_all(b"\n")?;
        f.sync_all()?;
        drop(f);
        std::fs::rename(&tmp, path)
    }

    /// `$XDG_STATE_HOME/omen-kbd/state.json` (default `~/.local/state/omen-kbd/state.json`).
    pub fn path() -> PathBuf {
        let base = std::env::var_os("XDG_STATE_HOME")
            .filter(|v| !v.is_empty())
            .map(PathBuf::from)
            .unwrap_or_else(|| crate::config::expand_home("~/.local/state"));
        base.join("omen-kbd/state.json")
    }
}

fn with_suffix(path: &Path, suffix: &str) -> PathBuf {
    let mut s = path.as_os_str().to_owned();
    s.push(suffix);
    PathBuf::from(s)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn missing_file_gives_default() {
        let d = tempfile::tempdir().unwrap();
        let (s, warning) = PersistedState::load(&d.path().join("state.json"), "default");
        assert_eq!(warning, None);
        assert_eq!(
            s,
            PersistedState {
                active_profile: "default".into(),
                custom: None,
                effect_override: None,
                brightness: 100,
                on: true,
                lifetime_writes: 0,
            }
        );
    }

    #[test]
    fn corrupt_file_is_moved_aside() {
        let d = tempfile::tempdir().unwrap();
        let p = d.path().join("state.json");
        std::fs::write(&p, "{not json").unwrap();
        let (s, warning) = PersistedState::load(&p, "default");
        assert_eq!(s, PersistedState::default_for("default"));
        assert!(warning.is_some());
        assert_eq!(
            std::fs::read_to_string(d.path().join("state.json.bad")).unwrap(),
            "{not json"
        );
        assert!(!p.exists());
    }

    #[test]
    fn save_roundtrips_and_leaves_no_tmp() {
        let d = tempfile::tempdir().unwrap();
        let p = d.path().join("a/b/state.json");
        let s = PersistedState {
            active_profile: "custom".into(),
            custom: Some(vec!["#ff0000".into(), "theme:blue".into()]),
            effect_override: Some(EffectOverride {
                effect: Effect::Breathe,
                speed: 30,
            }),
            brightness: 40,
            on: false,
            lifetime_writes: 12345,
        };
        s.save(&p).unwrap();
        assert_eq!(PersistedState::load(&p, "default"), (s, None));
        assert!(!d.path().join("a/b/state.json.tmp").exists());
    }

    #[test]
    fn unknown_and_missing_fields_are_tolerated() {
        let d = tempfile::tempdir().unwrap();
        let p = d.path().join("state.json");
        std::fs::write(&p, r#"{"active_profile":"game","future_field":1}"#).unwrap();
        let (s, warning) = PersistedState::load(&p, "default");
        assert_eq!(warning, None);
        assert_eq!(s.active_profile, "game");
        assert_eq!(s.brightness, 100);
        assert!(s.on);
    }
}
