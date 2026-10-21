//! The daemon's async loop: IPC requests, system events and writer outcomes drive the pure
//! [`Model`]; every change recomputes one frame and hands it to the writer.

use std::path::PathBuf;
use std::time::{Duration, Instant};

use anyhow::Context;
use tokio::io::BufReader;
use tokio::net::{UnixListener, UnixStream};
use tokio::signal::unix::{SignalKind, signal};
use tokio::sync::{broadcast, mpsc, oneshot, watch};

use crate::backend::{self, Backend, BackendKind};
use crate::config::Config;
use crate::core::compositor::{Output, compose};
use crate::core::palette::Palette;
use crate::daemon::model::{Change, FactUpdate, Model};
use crate::daemon::writer::{WriteJob, WriterMsg, spawn_writer};
use crate::ipc::{self, IpcError, Request, Response, Status};
use crate::state::PersistedState;

/// Something that happened outside the daemon (system D-Bus, Hyprland, theme switch, …).
#[derive(Debug, Clone, PartialEq)]
pub enum Event {
    Fact(FactUpdate),
    /// logind `PrepareForSleep`: `true` before suspend, `false` after resume.
    Sleep(bool),
    Submap(Option<String>),
    ThemeChanged,
    HwBacklight(bool),
}

#[derive(Debug, Clone)]
pub struct RunOptions {
    pub backend: BackendKind,
    pub sys_root: PathBuf,
    pub config_path: PathBuf,
    pub state_path: PathBuf,
    pub socket_path: PathBuf,
    pub mock_render: bool,
    pub with_sources: bool,
}

pub fn event_channel() -> (mpsc::Sender<Event>, mpsc::Receiver<Event>) {
    mpsc::channel(64)
}

const REPLY_TIMEOUT: Duration = Duration::from_secs(2);
const PROBE_EVERY: Duration = Duration::from_secs(3);
const SAVE_USER_CHANGE: Duration = Duration::from_secs(1);
const SAVE_COUNTER_ONLY: Duration = Duration::from_secs(60);

/// Loads everything, opens the backend and the socket, then runs until SIGTERM/SIGINT.
pub async fn run(opts: RunOptions) -> anyhow::Result<()> {
    let config = Config::load(&opts.config_path).map_err(|e| anyhow::anyhow!(e.0))?;
    let first = config.profiles[0].name.clone();
    let (palette, palette_warning) = match Palette::load(&config.theme.dir) {
        Ok(p) => (p, None),
        Err(e) => (Palette::default(), Some(e)),
    };
    let (persisted, state_warning) = PersistedState::load(&opts.state_path, &first);
    let backend: Box<dyn Backend> = match opts.backend {
        BackendKind::Mock => backend::mock(opts.mock_render).0,
        kind => backend::open(kind, &opts.sys_root)?,
    };
    let mut model = Model::new(config, palette, persisted, backend.name());
    for w in [palette_warning, state_warning].into_iter().flatten() {
        log::warn!("{w}");
        model.add_warning(w);
    }
    let listener = ipc::bind(&opts.socket_path)
        .await
        .with_context(|| format!("binding {}", opts.socket_path.display()))?;
    let (_events_tx, events_rx) = event_channel();
    run_with(model, backend, listener, events_rx, opts).await
}

/// The daemon loop with injected parts (tests pass a mock backend and their own event sender).
pub async fn run_with(
    model: Model,
    backend: Box<dyn Backend>,
    listener: UnixListener,
    mut events: mpsc::Receiver<Event>,
    opts: RunOptions,
) -> anyhow::Result<()> {
    let (job_tx, job_rx) = watch::channel(None);
    let (probe_tx, probe_rx) = mpsc::channel(4);
    let (result_tx, mut results) = mpsc::channel(64);
    let writer = spawn_writer(backend, job_rx, probe_rx, result_tx);
    let (status_tx, _) = broadcast::channel(64);
    let (req_tx, mut requests) = mpsc::channel::<(Request, oneshot::Sender<Response>)>(64);
    let accept = tokio::spawn(accept_loop(listener, req_tx, status_tx.clone()));
    let mut sigterm = signal(SignalKind::terminate())?;
    let mut sigint = signal(SignalKind::interrupt())?;

    let mut d = Daemon {
        model,
        opts,
        start: Instant::now(),
        job_tx,
        probe_tx,
        status_tx,
        generation: 0,
        last_job: None,
        force_next: false,
        last_output: None,
        last_status: None,
        pending: Vec::new(),
        save_at: None,
        next_tick: None,
        next_probe: None,
    };
    d.recompute();

    let mut events_open = true;
    loop {
        let wake = d.next_wake();
        tokio::select! {
            Some((req, reply)) = requests.recv() => d.on_request(req, reply),
            ev = events.recv(), if events_open => match ev {
                Some(ev) => d.on_event(ev),
                None => events_open = false,
            },
            Some(msg) = results.recv() => d.on_writer(msg),
            _ = tokio::time::sleep_until(wake.into()) => d.on_wake(),
            _ = sigterm.recv() => break,
            _ = sigint.recv() => break,
        }
    }

    d.save();
    accept.abort();
    let _ = std::fs::remove_file(&d.opts.socket_path);
    drop(d);
    let _ = writer.await;
    Ok(())
}

/// A state-changing request waiting for the writer to confirm its frame.
struct Pending {
    generation: u64,
    reply: oneshot::Sender<Response>,
    deadline: Instant,
}

struct Daemon {
    model: Model,
    opts: RunOptions,
    start: Instant,
    job_tx: watch::Sender<Option<WriteJob>>,
    probe_tx: mpsc::Sender<()>,
    status_tx: broadcast::Sender<Status>,
    generation: u64,
    /// The last frame handed to the writer, as `(backlight, hardware zones)`.
    last_job: Option<(bool, [crate::core::color::Rgb; 4])>,
    force_next: bool,
    last_output: Option<Output>,
    last_status: Option<Status>,
    pending: Vec<Pending>,
    save_at: Option<Instant>,
    next_tick: Option<Instant>,
    next_probe: Option<Instant>,
}

impl Daemon {
    /// Recomputes the frame; sends a job if it changed. Returns the generation sent, if any.
    fn recompute(&mut self) -> Option<u64> {
        let now = Instant::now();
        let mut sent = None;
        match self.model.inputs(now) {
            None => {
                self.next_tick = None;
            }
            Some(inputs) => {
                let out = compose(&inputs, now.duration_since(self.start).as_secs_f64());
                let hw = self.model.config().device.zone_map.to_hw(&out.zones);
                let key = (out.backlight, hw);
                if self.force_next || self.last_job != Some(key) {
                    self.generation += 1;
                    let job = WriteJob {
                        generation: self.generation,
                        backlight: out.backlight,
                        hw,
                        force: self.force_next,
                    };
                    self.force_next = false;
                    self.last_job = Some(key);
                    let _ = self.job_tx.send(Some(job));
                    sent = Some(self.generation);
                }
                self.next_tick = out.animated.then(|| now + self.tick_period());
                self.last_output = Some(out);
            }
        }
        self.next_probe = if self.model.wants_animation() {
            Some(self.next_probe.unwrap_or(now + PROBE_EVERY))
        } else {
            None
        };
        self.broadcast();
        sent
    }

    fn tick_period(&self) -> Duration {
        Duration::from_millis(1000 / u64::from(self.model.config().effects.fps.max(1)))
    }

    fn status(&self) -> Status {
        self.model.status(self.last_output.as_ref())
    }

    fn broadcast(&mut self) {
        let s = self.status();
        if self.last_status.as_ref() != Some(&s) {
            let _ = self.status_tx.send(s.clone());
            self.last_status = Some(s);
        }
    }

    fn schedule_save(&mut self, within: Duration) {
        let at = Instant::now() + within;
        self.save_at = Some(self.save_at.map_or(at, |t| t.min(at)));
    }

    fn save(&mut self) {
        self.save_at = None;
        if let Err(e) = self.model.persisted().save(&self.opts.state_path) {
            log::warn!("saving {}: {e}", self.opts.state_path.display());
        }
    }

    fn reply_ok(&self, reply: oneshot::Sender<Response>) {
        let _ = reply.send(Response::ok(Some(self.status())));
    }

    fn on_request(&mut self, req: Request, reply: oneshot::Sender<Response>) {
        match req {
            Request::Status | Request::Subscribe => self.reply_ok(reply),
            Request::Reload => match Config::load(&self.opts.config_path) {
                Ok(config) => {
                    let palette = match Palette::load(&config.theme.dir) {
                        Ok(p) => p,
                        Err(e) => {
                            self.model.add_warning(e);
                            self.model.palette().clone()
                        }
                    };
                    self.model.replace_config(config, palette);
                    self.recompute();
                    self.reply_ok(reply);
                }
                Err(e) => {
                    let _ = reply.send(Response::err(e.0));
                }
            },
            other => match self.model.handle(&other, Instant::now()) {
                Err(msg) => {
                    let _ = reply.send(Response::err(msg));
                }
                Ok(Change::None) => self.reply_ok(reply),
                Ok(change) => {
                    if change == Change::OutputAndPersist {
                        self.schedule_save(SAVE_USER_CHANGE);
                    }
                    match self.recompute() {
                        Some(generation) => self.pending.push(Pending {
                            generation,
                            reply,
                            deadline: Instant::now() + REPLY_TIMEOUT,
                        }),
                        None => self.reply_ok(reply),
                    }
                }
            },
        }
    }

    fn on_event(&mut self, ev: Event) {
        match ev {
            Event::Fact(f) => self.model.apply_fact(f),
            Event::Sleep(true) => {
                self.model.set_suspended(true);
                self.save();
            }
            Event::Sleep(false) => {
                self.model.set_suspended(false);
                self.force_next = true;
            }
            Event::Submap(s) => self.model.set_submap(s),
            Event::ThemeChanged => match Palette::load(&self.model.config().theme.dir) {
                Ok(p) => self.model.set_palette(p),
                Err(e) => self.model.add_warning(e),
            },
            Event::HwBacklight(on) => self.model.set_hw_backlight(on),
        }
        self.recompute();
    }

    fn on_writer(&mut self, msg: WriterMsg) {
        match msg {
            WriterMsg::Outcome(o) => {
                self.model.record_write(&o.result);
                match &o.result {
                    Ok(true) => self.schedule_save(SAVE_COUNTER_ONLY),
                    Ok(false) => {}
                    // Forget the frame so the next change re-sends it.
                    Err(_) => self.last_job = None,
                }
                let (done, waiting): (Vec<_>, Vec<_>) = std::mem::take(&mut self.pending)
                    .into_iter()
                    .partition(|p| p.generation <= o.generation);
                self.pending = waiting;
                let resp = match &o.result {
                    Ok(_) => Response::ok(Some(self.status())),
                    Err(e) => Response::err(e.to_string()),
                };
                for p in done {
                    let _ = p.reply.send(resp.clone());
                }
                // Never recompute here: after an error that would re-send the same frame at once
                // and spin while the error persists. The next change (or, during an effect, the
                // next tick — which shows the static frame once a budget error stops animation)
                // retries.
                self.broadcast();
            }
            WriterMsg::HwBacklight(Ok(on)) => {
                self.model.set_hw_backlight(on);
                self.recompute();
            }
            WriterMsg::HwBacklight(Err(e)) => log::debug!("backlight probe: {e}"),
        }
    }

    fn next_wake(&self) -> Instant {
        [
            self.next_tick,
            self.model.hint_deadline(),
            self.save_at,
            self.next_probe,
            self.pending.iter().map(|p| p.deadline).min(),
        ]
        .into_iter()
        .flatten()
        .min()
        .unwrap_or_else(|| Instant::now() + Duration::from_secs(3600))
    }

    fn on_wake(&mut self) {
        let now = Instant::now();
        let due = |t: Option<Instant>| t.is_some_and(|t| t <= now);

        if due(self.next_probe) {
            let _ = self.probe_tx.try_send(());
            self.next_probe = Some(now + PROBE_EVERY);
        }
        if due(self.save_at) {
            self.save();
        }
        let expired: Vec<Pending>;
        (expired, self.pending) = std::mem::take(&mut self.pending)
            .into_iter()
            .partition(|p| p.deadline <= now);
        for p in expired {
            let _ = p.reply.send(Response::err(
                "timed out waiting for the keyboard write".into(),
            ));
        }
        if due(self.next_tick) || due(self.model.hint_deadline()) {
            self.recompute();
        }
    }
}

async fn accept_loop(
    listener: UnixListener,
    requests: mpsc::Sender<(Request, oneshot::Sender<Response>)>,
    statuses: broadcast::Sender<Status>,
) {
    loop {
        match listener.accept().await {
            Ok((stream, _)) => {
                tokio::spawn(serve_connection(stream, requests.clone(), statuses.clone()));
            }
            Err(e) => {
                log::warn!("accept: {e}");
                tokio::time::sleep(Duration::from_millis(100)).await;
            }
        }
    }
}

async fn serve_connection(
    stream: UnixStream,
    requests: mpsc::Sender<(Request, oneshot::Sender<Response>)>,
    statuses: broadcast::Sender<Status>,
) {
    let (r, mut w) = stream.into_split();
    let mut r = BufReader::new(r);
    loop {
        let req = match ipc::read_request(&mut r).await {
            Ok(Some(req)) => req,
            Ok(None) => return,
            Err(IpcError::Protocol(e)) => {
                if ipc::write_response(&mut w, &Response::err(e))
                    .await
                    .is_err()
                {
                    return;
                }
                continue;
            }
            Err(_) => return,
        };
        let subscribing = req == Request::Subscribe;
        // Subscribe before asking for the current status so no change is missed in between.
        let mut updates = subscribing.then(|| statuses.subscribe());
        let (tx, rx) = oneshot::channel();
        if requests.send((req, tx)).await.is_err() {
            return;
        }
        let resp = rx
            .await
            .unwrap_or_else(|_| Response::err("daemon shutting down".into()));
        if ipc::write_response(&mut w, &resp).await.is_err() {
            return;
        }
        if let Some(updates) = updates.as_mut() {
            loop {
                match updates.recv().await {
                    Ok(s) => {
                        if ipc::write_response(&mut w, &Response::ok(Some(s)))
                            .await
                            .is_err()
                        {
                            return;
                        }
                    }
                    Err(broadcast::error::RecvError::Lagged(_)) => continue,
                    Err(broadcast::error::RecvError::Closed) => return,
                }
            }
        }
    }
}
