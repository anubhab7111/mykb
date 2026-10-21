//! The writer actor: the only owner of the backend. Jobs arrive on a `watch` channel, so a burst
//! of changes collapses to the latest frame. Backend calls run on blocking threads.

use tokio::sync::{mpsc, watch};
use tokio::task::JoinHandle;

use crate::backend::{Backend, BackendError};
use crate::core::color::Rgb;

/// A frame to apply, in hardware zone order.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct WriteJob {
    pub generation: u64,
    pub backlight: bool,
    pub hw: [Rgb; 4],
    /// Write the zones even if they equal the last written frame (e.g. after resume).
    pub force: bool,
}

#[derive(Debug, Clone, PartialEq)]
pub struct WriteOutcome {
    pub generation: u64,
    /// `Ok(true)` = zones were written; `Ok(false)` = nothing (or only the backlight) changed.
    pub result: Result<bool, BackendError>,
}

#[derive(Debug, Clone, PartialEq)]
pub enum WriterMsg {
    Outcome(WriteOutcome),
    HwBacklight(Result<bool, BackendError>),
}

/// What the writer believes the hardware currently shows (updated only on success).
struct Hw {
    backend: Box<dyn Backend>,
    backlight: Option<bool>,
    zones: Option<[Rgb; 4]>,
}

impl Hw {
    fn apply(&mut self, job: &WriteJob) -> Result<bool, BackendError> {
        if !job.backlight {
            if self.backlight != Some(false) {
                self.backend.set_backlight(false)?;
                self.backlight = Some(false);
            }
            return Ok(false);
        }
        if self.backlight != Some(true) {
            self.backend.set_backlight(true)?;
            self.backlight = Some(true);
        }
        if job.force || self.zones != Some(job.hw) {
            self.backend.write_zones(job.hw)?;
            self.zones = Some(job.hw);
            return Ok(true);
        }
        Ok(false)
    }
}

pub fn spawn_writer(
    backend: Box<dyn Backend>,
    mut jobs: watch::Receiver<Option<WriteJob>>,
    mut probes: mpsc::Receiver<()>,
    results: mpsc::Sender<WriterMsg>,
) -> JoinHandle<()> {
    tokio::spawn(async move {
        let mut hw = Some(Hw {
            backend,
            backlight: None,
            zones: None,
        });
        loop {
            let msg = tokio::select! {
                changed = jobs.changed() => {
                    if changed.is_err() {
                        break;
                    }
                    let Some(job) = *jobs.borrow_and_update() else { continue };
                    let mut state = hw.take().expect("writer state present");
                    let (state, result) = tokio::task::spawn_blocking(move || {
                        let r = state.apply(&job);
                        (state, r)
                    })
                    .await
                    .expect("backend call panicked");
                    hw = Some(state);
                    WriterMsg::Outcome(WriteOutcome { generation: job.generation, result })
                }
                Some(()) = probes.recv() => {
                    let mut state = hw.take().expect("writer state present");
                    let (state, result) = tokio::task::spawn_blocking(move || {
                        let r = state.backend.read_backlight();
                        (state, r)
                    })
                    .await
                    .expect("backend call panicked");
                    hw = Some(state);
                    WriterMsg::HwBacklight(result)
                }
            };
            if results.send(msg).await.is_err() {
                break;
            }
        }
    })
}
