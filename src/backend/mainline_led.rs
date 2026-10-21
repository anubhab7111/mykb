//! The upstream hp-wmi multicolor LED interface (pending patch series): one LED per zone,
//! `hp::kbd_zoned_backlight-{right,center,left,wasd}` = hardware zones 0..3. Four writes per frame.

use std::path::{Path, PathBuf};

use super::{Backend, BackendError, Rgb, read_attr, write_attr};

const ZONE_DIRS: [&str; 4] = [
    "class/leds/hp::kbd_zoned_backlight-right",
    "class/leds/hp::kbd_zoned_backlight-center",
    "class/leds/hp::kbd_zoned_backlight-left",
    "class/leds/hp::kbd_zoned_backlight-wasd",
];

pub(super) fn present(sys_root: &Path) -> bool {
    ZONE_DIRS.iter().all(|d| sys_root.join(d).is_dir())
}

pub(super) struct MainlineLed {
    dirs: [PathBuf; 4],
}

impl MainlineLed {
    pub(super) fn open(sys_root: &Path) -> Result<MainlineLed, BackendError> {
        if !present(sys_root) {
            return Err(BackendError::Missing(format!(
                "{}/hp::kbd_zoned_backlight-* (kernel without HP keyboard LED support)",
                sys_root.join("class/leds").display()
            )));
        }
        Ok(MainlineLed {
            dirs: ZONE_DIRS.map(|d| sys_root.join(d)),
        })
    }
}

impl Backend for MainlineLed {
    fn name(&self) -> &'static str {
        "mainline-led"
    }

    fn write_zones(&mut self, hw: [Rgb; 4]) -> Result<(), BackendError> {
        for (dir, c) in self.dirs.iter().zip(hw) {
            write_attr(
                &dir.join("multi_intensity"),
                &format!("{} {} {}\n", c.r, c.g, c.b),
            )?;
        }
        Ok(())
    }

    fn set_backlight(&mut self, on: bool) -> Result<(), BackendError> {
        for dir in &self.dirs {
            write_attr(&dir.join("brightness"), if on { "255\n" } else { "0\n" })?;
        }
        Ok(())
    }

    fn read_backlight(&mut self) -> Result<bool, BackendError> {
        for dir in &self.dirs {
            let path = dir.join("brightness");
            let v: u32 = read_attr(&path)?
                .trim()
                .parse()
                .map_err(|_| BackendError::Io(format!("{}: not a number", path.display())))?;
            if v > 0 {
                return Ok(true);
            }
        }
        Ok(false)
    }
}
