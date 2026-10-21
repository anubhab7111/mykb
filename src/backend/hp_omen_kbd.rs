//! Our own kernel module's sysfs interface: one write sets all four zones atomically.

use std::path::{Path, PathBuf};

use super::{Backend, BackendError, Rgb, read_attr, write_attr};

const BASE: &str = "devices/platform/hp-omen-kbd";

pub(super) fn present(sys_root: &Path) -> bool {
    sys_root.join(BASE).is_dir()
}

pub(super) struct HpOmenKbd {
    dir: PathBuf,
}

impl HpOmenKbd {
    pub(super) fn open(sys_root: &Path) -> Result<HpOmenKbd, BackendError> {
        let dir = sys_root.join(BASE);
        if !dir.is_dir() {
            return Err(BackendError::Missing(format!(
                "{} (is hp_omen_kbd loaded?)",
                dir.display()
            )));
        }
        Ok(HpOmenKbd { dir })
    }
}

impl Backend for HpOmenKbd {
    fn name(&self) -> &'static str {
        "hp-omen-kbd"
    }

    fn write_zones(&mut self, hw: [Rgb; 4]) -> Result<(), BackendError> {
        let line = format!(
            "{} {} {} {}\n",
            hw[0].to_hex(),
            hw[1].to_hex(),
            hw[2].to_hex(),
            hw[3].to_hex()
        );
        write_attr(&self.dir.join("zones"), &line)
    }

    fn set_backlight(&mut self, on: bool) -> Result<(), BackendError> {
        write_attr(&self.dir.join("backlight"), if on { "1\n" } else { "0\n" })
    }

    fn read_backlight(&mut self) -> Result<bool, BackendError> {
        let path = self.dir.join("backlight");
        match read_attr(&path)?.trim() {
            "1" => Ok(true),
            "0" => Ok(false),
            other => Err(BackendError::Io(format!(
                "{}: unexpected value {other:?}",
                path.display()
            ))),
        }
    }
}
