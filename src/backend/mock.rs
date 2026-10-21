//! In-memory backend for tests and `--backend mock` demos. Never touches hardware.

use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use super::{Backend, BackendError, Rgb};

/// One recorded backend call.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MockOp {
    Zones([Rgb; 4]),
    Backlight(bool),
}

struct State {
    ops: Vec<MockOp>,
    fail: Option<BackendError>,
    hw_backlight: bool,
    delay: Duration,
}

/// Test/demo handle onto a mock backend's shared state.
#[derive(Clone)]
pub struct MockHandle(Arc<Mutex<State>>);

impl MockHandle {
    fn state(&self) -> MutexGuard<'_, State> {
        self.0.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Every successful call so far, in order.
    pub fn ops(&self) -> Vec<MockOp> {
        self.state().ops.clone()
    }

    /// Makes every call fail with `e` until called again with `None`.
    pub fn fail_with(&self, e: Option<BackendError>) {
        self.state().fail = e;
    }

    /// Simulates the Fn backlight key: the value `read_backlight` returns from now on.
    pub fn set_hw_backlight(&self, on: bool) {
        self.state().hw_backlight = on;
    }

    /// Each write sleeps `d` (exercises the writer's coalescing).
    pub fn set_delay(&self, d: Duration) {
        self.state().delay = d;
    }
}

struct MockBackend {
    state: Arc<Mutex<State>>,
    render: bool,
}

impl MockBackend {
    fn state(&self) -> MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Sleeps the configured delay (outside the lock), then returns the configured failure, if any.
    fn before_call(&self) -> Result<(), BackendError> {
        let (delay, fail) = {
            let s = self.state();
            (s.delay, s.fail.clone())
        };
        if !delay.is_zero() {
            std::thread::sleep(delay);
        }
        fail.map_or(Ok(()), Err)
    }
}

/// A mock backend and its handle. With `render`, each zone write prints a truecolor line.
pub fn mock(render: bool) -> (Box<dyn Backend>, MockHandle) {
    let state = Arc::new(Mutex::new(State {
        ops: Vec::new(),
        fail: None,
        hw_backlight: true,
        delay: Duration::ZERO,
    }));
    (
        Box::new(MockBackend {
            state: state.clone(),
            render,
        }),
        MockHandle(state),
    )
}

impl Backend for MockBackend {
    fn name(&self) -> &'static str {
        "mock"
    }

    fn write_zones(&mut self, hw: [Rgb; 4]) -> Result<(), BackendError> {
        self.before_call()?;
        self.state().ops.push(MockOp::Zones(hw));
        if self.render {
            let line: Vec<String> = hw
                .iter()
                .enumerate()
                .map(|(i, c)| {
                    format!(
                        "hw{i} \x1b[48;2;{};{};{}m   \x1b[0m #{}",
                        c.r,
                        c.g,
                        c.b,
                        c.to_hex()
                    )
                })
                .collect();
            println!("mock: {}", line.join("  "));
        }
        Ok(())
    }

    fn set_backlight(&mut self, on: bool) -> Result<(), BackendError> {
        self.before_call()?;
        let mut s = self.state();
        s.ops.push(MockOp::Backlight(on));
        s.hw_backlight = on;
        drop(s);
        if self.render {
            println!("mock: backlight {}", if on { "on" } else { "off" });
        }
        Ok(())
    }

    fn read_backlight(&mut self) -> Result<bool, BackendError> {
        let fail = self.state().fail.clone();
        if let Some(e) = fail {
            return Err(e);
        }
        Ok(self.state().hw_backlight)
    }
}
