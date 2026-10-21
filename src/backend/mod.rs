//! Hardware backends. The daemon talks to the keyboard only through [`Backend`]; the writer task is
//! its sole owner. Real backends write sysfs attributes under a configurable `sys_root` so tests can
//! use fake trees.

use std::io::Write;
use std::path::Path;

pub use crate::core::color::Rgb;

mod hp_omen_kbd;
mod mainline_led;
mod mock;

pub use mock::{MockHandle, MockOp, mock};

#[derive(thiserror::Error, Debug, Clone, PartialEq)]
pub enum BackendError {
    #[error("hourly write budget exhausted")]
    Budget,
    #[error("hp_omen_kbd is read-only (readonly=1)")]
    ReadOnly,
    #[error("permission denied on {0} (are you in group omen-kbd?)")]
    Permission(String),
    #[error("backend missing: {0}")]
    Missing(String),
    #[error("I/O error: {0}")]
    Io(String),
}

/// A keyboard backend. Colors are in **hardware** zone order.
pub trait Backend: Send {
    fn name(&self) -> &'static str;
    fn write_zones(&mut self, hw: [Rgb; 4]) -> Result<(), BackendError>;
    fn set_backlight(&mut self, on: bool) -> Result<(), BackendError>;
    fn read_backlight(&mut self) -> Result<bool, BackendError>;
}

/// Backend selection. `Auto` only ever picks a real backend, never the mock.
#[derive(clap::ValueEnum, Clone, Copy, Debug, PartialEq, Eq)]
pub enum BackendKind {
    Auto,
    Mock,
    HpOmenKbd,
    MainlineLed,
}

pub fn open(kind: BackendKind, sys_root: &Path) -> Result<Box<dyn Backend>, BackendError> {
    match kind {
        BackendKind::Mock => Ok(mock(false).0),
        BackendKind::HpOmenKbd => Ok(Box::new(hp_omen_kbd::HpOmenKbd::open(sys_root)?)),
        BackendKind::MainlineLed => Ok(Box::new(mainline_led::MainlineLed::open(sys_root)?)),
        BackendKind::Auto => {
            match (
                hp_omen_kbd::present(sys_root),
                mainline_led::present(sys_root),
            ) {
                (true, true) => Err(BackendError::Io(
                    "both hp-omen-kbd and mainline LEDs present; remove the hp-omen-kbd DKMS module"
                        .into(),
                )),
                (true, false) => open(BackendKind::HpOmenKbd, sys_root),
                (false, true) => open(BackendKind::MainlineLed, sys_root),
                (false, false) => Err(BackendError::Missing(
                    "no keyboard backend (is hp_omen_kbd loaded?)".into(),
                )),
            }
        }
    }
}

/// Maps the errnos the kernel module uses onto backend errors.
pub fn map_io_error(e: &std::io::Error, path: &Path) -> BackendError {
    let p = path.display().to_string();
    match e.raw_os_error() {
        Some(libc::EBUSY) => BackendError::Budget,
        Some(libc::EPERM) => BackendError::ReadOnly,
        Some(libc::EACCES) => BackendError::Permission(p),
        Some(libc::ENOENT) => BackendError::Missing(p),
        _ => BackendError::Io(format!("{p}: {e}")),
    }
}

/// Writes a sysfs attribute in one `write` call. Never creates files.
pub(crate) fn write_attr(path: &Path, content: &str) -> Result<(), BackendError> {
    let mut f = std::fs::OpenOptions::new()
        .write(true)
        .truncate(true)
        .open(path)
        .map_err(|e| map_io_error(&e, path))?;
    f.write_all(content.as_bytes())
        .map_err(|e| map_io_error(&e, path))
}

pub(crate) fn read_attr(path: &Path) -> Result<String, BackendError> {
    std::fs::read_to_string(path).map_err(|e| map_io_error(&e, path))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io;
    use std::time::{Duration, Instant};

    const HP: &str = "devices/platform/hp-omen-kbd";
    const LEDS: [&str; 4] = [
        "class/leds/hp::kbd_zoned_backlight-right",
        "class/leds/hp::kbd_zoned_backlight-center",
        "class/leds/hp::kbd_zoned_backlight-left",
        "class/leds/hp::kbd_zoned_backlight-wasd",
    ];

    fn add_hp(root: &Path) {
        std::fs::create_dir_all(root.join(HP)).unwrap();
        std::fs::write(root.join(HP).join("zones"), "").unwrap();
        std::fs::write(root.join(HP).join("backlight"), "1\n").unwrap();
    }

    fn add_mainline(root: &Path) {
        for d in LEDS {
            std::fs::create_dir_all(root.join(d)).unwrap();
            std::fs::write(root.join(d).join("multi_intensity"), "0 0 0\n").unwrap();
            std::fs::write(root.join(d).join("brightness"), "255\n").unwrap();
        }
    }

    fn fake_hp_tree() -> tempfile::TempDir {
        let d = tempfile::tempdir().unwrap();
        add_hp(d.path());
        d
    }

    fn fake_mainline_tree() -> tempfile::TempDir {
        let d = tempfile::tempdir().unwrap();
        add_mainline(d.path());
        d
    }

    fn fake_both_tree() -> tempfile::TempDir {
        let d = tempfile::tempdir().unwrap();
        add_hp(d.path());
        add_mainline(d.path());
        d
    }

    fn read(root: &Path, rel: &str) -> String {
        std::fs::read_to_string(root.join(rel)).unwrap()
    }

    #[test]
    fn hp_writes_hw_order_line() {
        let root = fake_hp_tree();
        let mut b = open(BackendKind::HpOmenKbd, root.path()).unwrap();
        b.write_zones([
            Rgb::new(1, 2, 3),
            Rgb::new(255, 0, 0),
            Rgb::new(0, 255, 0),
            Rgb::new(0, 0, 255),
        ])
        .unwrap();
        assert_eq!(
            read(root.path(), "devices/platform/hp-omen-kbd/zones"),
            "010203 FF0000 00FF00 0000FF\n"
        );
    }

    #[test]
    fn hp_backlight_roundtrip() {
        let root = fake_hp_tree();
        let mut b = open(BackendKind::HpOmenKbd, root.path()).unwrap();
        assert!(b.read_backlight().unwrap());
        b.set_backlight(false).unwrap();
        assert_eq!(
            read(root.path(), "devices/platform/hp-omen-kbd/backlight"),
            "0\n"
        );
        assert!(!b.read_backlight().unwrap());
    }

    #[test]
    fn mainline_writes_each_led() {
        let root = fake_mainline_tree();
        let mut b = open(BackendKind::MainlineLed, root.path()).unwrap();
        b.write_zones([Rgb::new(1, 2, 3), Rgb::BLACK, Rgb::BLACK, Rgb::WHITE])
            .unwrap();
        assert_eq!(
            read(
                root.path(),
                "class/leds/hp::kbd_zoned_backlight-right/multi_intensity"
            ),
            "1 2 3\n"
        );
        assert_eq!(
            read(
                root.path(),
                "class/leds/hp::kbd_zoned_backlight-wasd/multi_intensity"
            ),
            "255 255 255\n"
        );
    }

    #[test]
    fn mainline_backlight_writes_every_zone() {
        let root = fake_mainline_tree();
        let mut b = open(BackendKind::MainlineLed, root.path()).unwrap();
        b.set_backlight(false).unwrap();
        for d in LEDS {
            assert_eq!(read(root.path(), &format!("{d}/brightness")), "0\n");
        }
        assert!(!b.read_backlight().unwrap());
        b.set_backlight(true).unwrap();
        assert_eq!(
            read(root.path(), &format!("{}/brightness", LEDS[2])),
            "255\n"
        );
        assert!(b.read_backlight().unwrap());
    }

    #[test]
    fn auto_selection() {
        assert_eq!(
            open(BackendKind::Auto, fake_hp_tree().path())
                .unwrap()
                .name(),
            "hp-omen-kbd"
        );
        assert_eq!(
            open(BackendKind::Auto, fake_mainline_tree().path())
                .unwrap()
                .name(),
            "mainline-led"
        );
        assert!(matches!(
            open(BackendKind::Auto, tempfile::tempdir().unwrap().path()),
            Err(BackendError::Missing(_))
        ));
        assert!(open(BackendKind::Auto, fake_both_tree().path()).is_err());
    }

    #[test]
    fn explicit_backend_missing_is_an_error() {
        let empty = tempfile::tempdir().unwrap();
        assert!(matches!(
            open(BackendKind::HpOmenKbd, empty.path()),
            Err(BackendError::Missing(_))
        ));
        assert!(matches!(
            open(BackendKind::MainlineLed, empty.path()),
            Err(BackendError::Missing(_))
        ));
    }

    #[test]
    fn kind_cli_names() {
        use clap::ValueEnum;
        assert!(matches!(
            BackendKind::from_str("hp-omen-kbd", false),
            Ok(BackendKind::HpOmenKbd)
        ));
        assert!(matches!(
            BackendKind::from_str("mainline-led", false),
            Ok(BackendKind::MainlineLed)
        ));
    }

    #[test]
    fn errno_mapping() {
        let p = Path::new("/x");
        assert_eq!(
            map_io_error(&io::Error::from_raw_os_error(libc::EBUSY), p),
            BackendError::Budget
        );
        assert_eq!(
            map_io_error(&io::Error::from_raw_os_error(libc::EPERM), p),
            BackendError::ReadOnly
        );
        assert!(matches!(
            map_io_error(&io::Error::from_raw_os_error(libc::EACCES), p),
            BackendError::Permission(_)
        ));
        assert!(matches!(
            map_io_error(&io::Error::from_raw_os_error(libc::ENOENT), p),
            BackendError::Missing(_)
        ));
        assert!(matches!(
            map_io_error(&io::Error::from_raw_os_error(libc::EIO), p),
            BackendError::Io(_)
        ));
    }

    #[test]
    fn mock_records_and_fails_on_demand() {
        let (mut b, h) = mock(false);
        b.set_backlight(true).unwrap();
        h.fail_with(Some(BackendError::Budget));
        assert_eq!(b.write_zones([Rgb::BLACK; 4]), Err(BackendError::Budget));
        assert_eq!(h.ops(), vec![MockOp::Backlight(true)]);
        h.fail_with(None);
        b.write_zones([Rgb::WHITE; 4]).unwrap();
        assert_eq!(
            h.ops(),
            vec![MockOp::Backlight(true), MockOp::Zones([Rgb::WHITE; 4])]
        );
    }

    #[test]
    fn mock_simulates_fn_key_and_delay() {
        let (mut b, h) = mock(false);
        assert!(b.read_backlight().unwrap(), "initially on");
        b.set_backlight(false).unwrap();
        assert!(
            !b.read_backlight().unwrap(),
            "follows the last set_backlight"
        );
        h.set_hw_backlight(true);
        assert!(b.read_backlight().unwrap(), "Fn key turned it back on");
        h.set_delay(Duration::from_millis(30));
        let start = Instant::now();
        b.write_zones([Rgb::BLACK; 4]).unwrap();
        assert!(start.elapsed() >= Duration::from_millis(30));
    }
}
