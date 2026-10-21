//! Integration tests: the real daemon loop against the mock backend, over the real socket.
//! Real time (not tokio's paused clock): backend calls run on blocking threads.

use std::path::PathBuf;
use std::time::{Duration, Instant};

use omen_kbd::backend::{BackendError, BackendKind, MockHandle, MockOp, mock};
use omen_kbd::config::Config;
use omen_kbd::core::color::Rgb;
use omen_kbd::core::palette::Palette;
use omen_kbd::daemon::model::{FactUpdate, Model};
use omen_kbd::daemon::run::{Event, RunOptions, event_channel, run_with};
use omen_kbd::ipc::{PowerAction, Request, Response, Status, bind, request, subscribe};
use omen_kbd::state::PersistedState;
use tokio::sync::mpsc;

const STATIC_CFG: &str = r##"
[hints]
timeout_s = 1
[hints.modifiers]
super = "#0000ff"
[[profiles]]
name = "plain"
colors = ["#101010", "#202020", "#303030", "#404040"]
"##;

const WAVE_CFG: &str = r##"
[effects]
enabled = true
fps = 10
[hints]
timeout_s = 1
[[profiles]]
name = "wavy"
colors = ["#ff0000", "#00ff00", "#0000ff", "#ffffff"]
effect = "wave"
speed = 50
"##;

struct Daemon {
    h: MockHandle,
    sock: PathBuf,
    events: mpsc::Sender<Event>,
    dir: tempfile::TempDir,
}

impl Daemon {
    fn zone_writes(&self) -> Vec<[Rgb; 4]> {
        self.h
            .ops()
            .into_iter()
            .filter_map(|o| match o {
                MockOp::Zones(z) => Some(z),
                MockOp::Backlight(_) => None,
            })
            .collect()
    }

    async fn req(&self, r: Request) -> Response {
        request(&self.sock, &r).await.expect("daemon reachable")
    }

    async fn status(&self) -> Status {
        self.req(Request::Status).await.status.expect("status")
    }

    fn config_path(&self) -> PathBuf {
        self.dir.path().join("config.toml")
    }

    fn state_path(&self) -> PathBuf {
        self.dir.path().join("state.json")
    }
}

async fn start(cfg_toml: &str) -> Daemon {
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(dir.path().join("config.toml"), cfg_toml).unwrap();
    let config = Config::parse(cfg_toml).unwrap();
    let first = config.profiles[0].name.clone();
    let model = Model::new(
        config,
        Palette::default(),
        PersistedState::default_for(&first),
        "mock",
    );
    let (backend, h) = mock(false);
    let sock = dir.path().join("omen-kbd.sock");
    let listener = bind(&sock).await.unwrap();
    let (events, rx) = event_channel();
    let opts = RunOptions {
        backend: BackendKind::Mock,
        sys_root: dir.path().to_path_buf(),
        config_path: dir.path().join("config.toml"),
        state_path: dir.path().join("state.json"),
        socket_path: sock.clone(),
        mock_render: false,
        with_sources: false,
    };
    tokio::spawn(run_with(model, backend, listener, rx, opts));
    let d = Daemon {
        h,
        sock,
        events,
        dir,
    };
    wait_until(|| !d.zone_writes().is_empty(), Duration::from_secs(2)).await;
    d
}

async fn wait_until(mut cond: impl FnMut() -> bool, timeout: Duration) -> bool {
    let end = Instant::now() + timeout;
    while Instant::now() < end {
        if cond() {
            return true;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    cond()
}

fn rgb(hex: &str) -> Rgb {
    Rgb::parse(hex).unwrap()
}

fn set(colors: &[&str]) -> Request {
    Request::Set {
        colors: colors.iter().map(|c| c.to_string()).collect(),
    }
}

#[tokio::test(flavor = "multi_thread")]
async fn startup_applies_the_profile() {
    let d = start(STATIC_CFG).await;
    // logical [10,20,30,40] → hardware [right, middle, left, wasd]
    assert_eq!(
        d.zone_writes(),
        vec![[rgb("303030"), rgb("202020"), rgb("101010"), rgb("404040")]]
    );
    assert_eq!(d.h.ops()[0], MockOp::Backlight(true));
}

#[tokio::test(flavor = "multi_thread")]
async fn set_request_writes_once_in_hw_order_and_replies_ok() {
    let d = start(STATIC_CFG).await;
    let before = d.zone_writes().len();
    let r = d
        .req(set(&["#ff0000", "#00ff00", "#0000ff", "#ffffff"]))
        .await;
    assert!(r.ok, "{r:?}");
    let zones = d.zone_writes();
    assert_eq!(zones.len(), before + 1, "exactly one write");
    assert_eq!(
        zones.last().unwrap(),
        &[rgb("0000ff"), rgb("00ff00"), rgb("ff0000"), rgb("ffffff")]
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn burst_of_100_sets_coalesces_to_latest() {
    let d = start(STATIC_CFG).await;
    d.h.set_delay(Duration::from_millis(20));
    let mut tasks = Vec::new();
    for i in 0..100u32 {
        let sock = d.sock.clone();
        tasks.push(tokio::spawn(async move {
            request(&sock, &set(&[&format!("#{:02x}{:02x}00", i, 255 - i)]))
                .await
                .unwrap()
        }));
    }
    for t in tasks {
        let r = t.await.unwrap();
        assert!(r.ok, "{r:?}");
    }
    let writes = d.zone_writes();
    assert!(writes.len() <= 21, "{} writes for 100 sets", writes.len());
    let shown = d.status().await.zones.map(|z| rgb(&z));
    let hw = [shown[2], shown[1], shown[0], shown[3]];
    assert_eq!(
        writes.last().unwrap(),
        &hw,
        "the keyboard ends where the daemon says it is"
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn write_error_reaches_cli_response() {
    let d = start(STATIC_CFG).await;
    d.h.fail_with(Some(BackendError::Io("boom".into())));
    let r = d.req(set(&["#ff0000"])).await;
    assert!(!r.ok);
    assert!(r.error.unwrap().contains("boom"));
}

#[tokio::test(flavor = "multi_thread")]
async fn failed_write_is_retried_on_next_change_not_in_a_loop() {
    let d = start(STATIC_CFG).await;
    let before = d.zone_writes().len();
    d.h.fail_with(Some(BackendError::Io("eacces".into())));
    assert!(!d.req(set(&["#ff0000"])).await.ok);
    d.h.fail_with(None);
    tokio::time::sleep(Duration::from_millis(300)).await;
    assert_eq!(
        d.zone_writes().len(),
        before,
        "no retry loop after a failure"
    );
    let r = d.req(Request::Brightness { value: "50".into() }).await;
    assert!(r.ok, "{r:?}");
    assert_eq!(d.zone_writes().last().unwrap(), &[rgb("800000"); 4]);
}

#[tokio::test(flavor = "multi_thread")]
async fn off_turns_backlight_off_without_zone_write() {
    let d = start(STATIC_CFG).await;
    let before = d.h.ops();
    let r = d
        .req(Request::Power {
            action: PowerAction::Off,
        })
        .await;
    assert!(r.ok, "{r:?}");
    let mut expected = before.clone();
    expected.push(MockOp::Backlight(false));
    assert_eq!(d.h.ops(), expected);
}

#[tokio::test(flavor = "multi_thread")]
async fn effect_ticks_at_configured_fps() {
    let d = start(WAVE_CFG).await;
    tokio::time::sleep(Duration::from_millis(300)).await;
    let a = d.zone_writes().len();
    tokio::time::sleep(Duration::from_secs(1)).await;
    let n = d.zone_writes().len() - a;
    assert!((8..=12).contains(&n), "{n} writes in 1 s at 10 fps");
}

#[tokio::test(flavor = "multi_thread")]
async fn fn_key_off_pauses_effect() {
    let d = start(WAVE_CFG).await;
    d.h.set_hw_backlight(false);
    tokio::time::sleep(Duration::from_secs(4)).await;
    let a = d.zone_writes().len();
    tokio::time::sleep(Duration::from_secs(1)).await;
    assert_eq!(
        d.zone_writes().len(),
        a,
        "paused while the backlight is off"
    );
    d.h.set_hw_backlight(true);
    assert!(
        wait_until(|| d.zone_writes().len() > a + 3, Duration::from_secs(5)).await,
        "resumes after the Fn key turns it back on"
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn budget_error_drops_to_static() {
    let d = start(WAVE_CFG).await;
    d.h.fail_with(Some(BackendError::Budget));
    tokio::time::sleep(Duration::from_millis(500)).await;
    let s = d.status().await;
    assert!(s.budget_exhausted);
    assert!(!s.animated);
    d.h.fail_with(None);
    let a = d.zone_writes().len();
    tokio::time::sleep(Duration::from_millis(500)).await;
    assert_eq!(d.zone_writes().len(), a, "no more ticking");
}

#[tokio::test(flavor = "multi_thread")]
async fn resume_forces_identical_rewrite() {
    let d = start(STATIC_CFG).await;
    let before = d.zone_writes();
    d.events.send(Event::Sleep(true)).await.unwrap();
    d.events.send(Event::Sleep(false)).await.unwrap();
    assert!(
        wait_until(
            || d.zone_writes().len() == before.len() + 1,
            Duration::from_secs(2)
        )
        .await
    );
    tokio::time::sleep(Duration::from_millis(200)).await;
    let after = d.zone_writes();
    assert_eq!(after.len(), before.len() + 1, "exactly one extra write");
    assert_eq!(after.last(), before.last(), "identical frame");
}

#[tokio::test(flavor = "multi_thread")]
async fn bad_reload_keeps_previous_config() {
    let d = start(STATIC_CFG).await;
    std::fs::write(d.config_path(), "[[profiles]\nthis is not toml").unwrap();
    let r = d.req(Request::Reload).await;
    assert!(!r.ok);
    assert!(r.error.unwrap().contains("config.toml"));
    assert_eq!(d.status().await.profile, "plain");
}

#[tokio::test(flavor = "multi_thread")]
async fn hint_clears_after_timeout() {
    let d = start(STATIC_CFG).await;
    let r = d
        .req(Request::Hint {
            mods: "super".into(),
        })
        .await;
    assert!(r.ok, "{r:?}");
    assert_eq!(d.status().await.hint.as_deref(), Some("super"));
    assert_eq!(d.zone_writes().last().unwrap(), &[rgb("0000ff"); 4]);
    tokio::time::sleep(Duration::from_millis(1300)).await;
    assert_eq!(d.status().await.hint, None);
    assert_eq!(d.zone_writes().last().unwrap()[2], rgb("101010"));
}

#[tokio::test(flavor = "multi_thread")]
async fn persist_debounced_one_second() {
    let d = start(STATIC_CFG).await;
    for v in ["90", "80", "70"] {
        let r = d.req(Request::Brightness { value: v.into() }).await;
        assert!(r.ok, "{r:?}");
    }
    tokio::time::sleep(Duration::from_millis(500)).await;
    assert!(!d.state_path().exists(), "not saved before the debounce");
    tokio::time::sleep(Duration::from_millis(1000)).await;
    let (s, w) = PersistedState::load(&d.state_path(), "plain");
    assert_eq!(w, None);
    assert_eq!(s.brightness, 70);
}

#[tokio::test(flavor = "multi_thread")]
async fn facts_change_the_frame() {
    let cfg = format!(
        "{STATIC_CFG}\n[[rules]]\nname = 'ps'\nwhen = {{ power_profile = 'power-saver' }}\nbrightness_cap = 50\n"
    );
    let d = start(&cfg).await;
    d.events
        .send(Event::Fact(FactUpdate::PowerProfile("power-saver".into())))
        .await
        .unwrap();
    assert!(
        wait_until(
            || d.zone_writes().last()
                == Some(&[rgb("181818"), rgb("101010"), rgb("080808"), rgb("202020")]),
            Duration::from_secs(2)
        )
        .await,
        "{:?}",
        d.zone_writes().last()
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn subscribe_streams_status_changes() {
    let d = start(STATIC_CFG).await;
    let mut sub = subscribe(&d.sock).await.unwrap();
    let first = sub.next_status().await.unwrap().unwrap();
    assert_eq!(first.brightness, 100);
    d.req(Request::Brightness { value: "40".into() }).await;
    let next = tokio::time::timeout(Duration::from_secs(2), sub.next_status())
        .await
        .unwrap()
        .unwrap()
        .unwrap();
    assert_eq!(next.brightness, 40);
}
