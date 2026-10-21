//! CLI ↔ daemon protocol: newline-delimited JSON over a user-only Unix socket
//! (`$XDG_RUNTIME_DIR/omen-kbd.sock`, mode 0600). One request per line, one response per line,
//! in order; `subscribe` turns the connection into a status stream.

use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use tokio::io::{AsyncBufRead, AsyncBufReadExt, AsyncWrite, AsyncWriteExt, BufReader, Lines};
use tokio::net::unix::{OwnedReadHalf, OwnedWriteHalf};
use tokio::net::{UnixListener, UnixStream};

pub use crate::core::rules::Facts;

#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
#[serde(tag = "cmd", rename_all = "kebab-case")]
pub enum Request {
    Status,
    Set { colors: Vec<String> },
    Profile { select: String },
    Brightness { value: String },
    Power { action: PowerAction },
    Effect { name: String, speed: Option<u8> },
    Hint { mods: String },
    Idle { on: bool },
    Flag { name: String, on: bool },
    Reload,
    Subscribe,
}

#[derive(Serialize, Deserialize, Debug, Clone, Copy, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum PowerAction {
    On,
    Off,
    Toggle,
}

/// What `status`, Waybar and `doctor` see. `zones` are logical-order `RRGGBB` after compositing.
#[derive(Serialize, Deserialize, Debug, Clone, PartialEq, Default)]
pub struct Status {
    pub profile: String,
    pub effect: String,
    pub effects_enabled: bool,
    pub animated: bool,
    pub brightness: u8,
    pub on: bool,
    pub backlight: bool,
    pub zones: [String; 4],
    pub hint: Option<String>,
    pub active_rules: Vec<String>,
    pub facts: Facts,
    pub backend: String,
    pub last_error: Option<String>,
    pub budget_exhausted: bool,
    pub lifetime_writes: u64,
    pub warnings: Vec<String>,
}

#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
pub struct Response {
    pub ok: bool,
    pub error: Option<String>,
    pub status: Option<Status>,
}

impl Response {
    pub fn ok(status: Option<Status>) -> Response {
        Response {
            ok: true,
            error: None,
            status,
        }
    }

    pub fn err(message: String) -> Response {
        Response {
            ok: false,
            error: Some(message),
            status: None,
        }
    }
}

#[derive(thiserror::Error, Debug)]
pub enum IpcError {
    #[error("omen-kbd daemon already running")]
    AlreadyRunning,
    #[error("daemon not running")]
    NotRunning,
    #[error("{0}")]
    Io(#[from] std::io::Error),
    #[error("protocol: {0}")]
    Protocol(String),
}

/// `$XDG_RUNTIME_DIR/omen-kbd.sock` (falls back to `/tmp/omen-kbd-<uid>.sock`).
pub fn socket_path() -> PathBuf {
    match std::env::var_os("XDG_RUNTIME_DIR").filter(|v| !v.is_empty()) {
        Some(dir) => PathBuf::from(dir).join("omen-kbd.sock"),
        None => PathBuf::from(format!("/tmp/omen-kbd-{}.sock", unsafe { libc::getuid() })),
    }
}

/// Binds the daemon socket. A live daemon on `path` ⇒ `AlreadyRunning`; a stale socket file left
/// by a crashed daemon is replaced. The socket is made user-only (0600).
pub async fn bind(path: &Path) -> Result<UnixListener, IpcError> {
    if path.exists() {
        if UnixStream::connect(path).await.is_ok() {
            return Err(IpcError::AlreadyRunning);
        }
        std::fs::remove_file(path)?;
    }
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let listener = UnixListener::bind(path)?;
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600))?;
    Ok(listener)
}

async fn connect(path: &Path) -> Result<UnixStream, IpcError> {
    UnixStream::connect(path).await.map_err(|e| match e.kind() {
        std::io::ErrorKind::NotFound | std::io::ErrorKind::ConnectionRefused => {
            IpcError::NotRunning
        }
        _ => IpcError::Io(e),
    })
}

async fn send(w: &mut (impl AsyncWrite + Unpin), req: &Request) -> Result<(), IpcError> {
    let mut line = serde_json::to_string(req).map_err(|e| IpcError::Protocol(e.to_string()))?;
    line.push('\n');
    w.write_all(line.as_bytes()).await?;
    w.flush().await?;
    Ok(())
}

/// Sends one request and waits for its response.
pub async fn request(path: &Path, req: &Request) -> Result<Response, IpcError> {
    let stream = connect(path).await?;
    let (r, mut w) = stream.into_split();
    send(&mut w, req).await?;
    let mut lines = BufReader::new(r).lines();
    let line = lines
        .next_line()
        .await?
        .ok_or_else(|| IpcError::Protocol("daemon closed the connection".into()))?;
    serde_json::from_str(&line).map_err(|e| IpcError::Protocol(e.to_string()))
}

/// A status stream from `subscribe`. Owns both socket halves, so dropping it closes the connection.
pub struct Subscription {
    lines: Lines<BufReader<OwnedReadHalf>>,
    _writer: OwnedWriteHalf,
}

impl Subscription {
    /// The next status pushed by the daemon; `Ok(None)` when the daemon closed the stream.
    pub async fn next_status(&mut self) -> Result<Option<Status>, IpcError> {
        let Some(line) = self.lines.next_line().await? else {
            return Ok(None);
        };
        let resp: Response =
            serde_json::from_str(&line).map_err(|e| IpcError::Protocol(e.to_string()))?;
        match resp {
            Response {
                status: Some(s), ..
            } => Ok(Some(s)),
            Response { error: Some(e), .. } => Err(IpcError::Protocol(e)),
            _ => Err(IpcError::Protocol(
                "subscription message without status".into(),
            )),
        }
    }
}

/// Sends `subscribe` and returns the stream of statuses that follows.
pub async fn subscribe(path: &Path) -> Result<Subscription, IpcError> {
    let stream = connect(path).await?;
    let (r, mut w) = stream.into_split();
    send(&mut w, &Request::Subscribe).await?;
    Ok(Subscription {
        lines: BufReader::new(r).lines(),
        _writer: w,
    })
}

/// Reads the next request line. `Ok(None)` at end of stream.
pub async fn read_request(
    r: &mut (impl AsyncBufRead + Unpin),
) -> Result<Option<Request>, IpcError> {
    let mut line = String::new();
    if r.read_line(&mut line).await? == 0 {
        return Ok(None);
    }
    serde_json::from_str(line.trim_end())
        .map(Some)
        .map_err(|e| IpcError::Protocol(format!("bad request: {e}")))
}

pub async fn write_response(
    w: &mut (impl AsyncWrite + Unpin),
    resp: &Response,
) -> Result<(), IpcError> {
    let mut line = serde_json::to_string(resp).map_err(|e| IpcError::Protocol(e.to_string()))?;
    line.push('\n');
    w.write_all(line.as_bytes()).await?;
    w.flush().await?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    #[test]
    fn request_wire_format() {
        assert_eq!(
            serde_json::to_string(&Request::Set {
                colors: vec!["#ff0000".into()]
            })
            .unwrap(),
            r##"{"cmd":"set","colors":["#ff0000"]}"##
        );
        assert_eq!(
            serde_json::to_string(&Request::Power {
                action: PowerAction::Toggle
            })
            .unwrap(),
            r#"{"cmd":"power","action":"toggle"}"#
        );
    }

    #[tokio::test]
    async fn bind_sets_mode_0600() {
        let d = tempfile::tempdir().unwrap();
        let p = d.path().join("s.sock");
        let _l = bind(&p).await.unwrap();
        let mode = std::fs::metadata(&p).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o600);
    }

    #[tokio::test]
    async fn bind_replaces_stale_socket() {
        let d = tempfile::tempdir().unwrap();
        let p = d.path().join("s.sock");
        drop(std::os::unix::net::UnixListener::bind(&p).unwrap());
        assert!(p.exists(), "a stale socket file is left behind");
        assert!(bind(&p).await.is_ok());
    }

    #[tokio::test]
    async fn bind_refuses_when_running() {
        let d = tempfile::tempdir().unwrap();
        let p = d.path().join("s.sock");
        let _first = bind(&p).await.unwrap();
        assert!(matches!(bind(&p).await, Err(IpcError::AlreadyRunning)));
    }

    #[tokio::test]
    async fn request_roundtrip() {
        let d = tempfile::tempdir().unwrap();
        let p = d.path().join("s.sock");
        let listener = bind(&p).await.unwrap();
        let server = tokio::spawn(async move {
            let (stream, _) = listener.accept().await.unwrap();
            let (r, mut w) = stream.into_split();
            let mut r = tokio::io::BufReader::new(r);
            let req = read_request(&mut r).await.unwrap().unwrap();
            write_response(&mut w, &Response::ok(None)).await.unwrap();
            req
        });
        let resp = request(&p, &Request::Reload).await.unwrap();
        assert_eq!(resp, Response::ok(None));
        assert_eq!(server.await.unwrap(), Request::Reload);
    }

    #[tokio::test]
    async fn read_request_rejects_garbage() {
        let mut input: &[u8] = b"{\"cmd\":\"explode\"}\n";
        assert!(matches!(
            read_request(&mut input).await,
            Err(IpcError::Protocol(_))
        ));
        let mut empty: &[u8] = b"";
        assert!(read_request(&mut empty).await.unwrap().is_none());
    }

    #[tokio::test]
    async fn subscription_yields_statuses_until_close() {
        let d = tempfile::tempdir().unwrap();
        let p = d.path().join("s.sock");
        let listener = bind(&p).await.unwrap();
        tokio::spawn(async move {
            let (stream, _) = listener.accept().await.unwrap();
            let (r, mut w) = stream.into_split();
            let mut r = tokio::io::BufReader::new(r);
            assert_eq!(
                read_request(&mut r).await.unwrap(),
                Some(Request::Subscribe)
            );
            for b in [10u8, 20] {
                let s = Status {
                    brightness: b,
                    ..Status::default()
                };
                write_response(&mut w, &Response::ok(Some(s)))
                    .await
                    .unwrap();
            }
        });
        let mut sub = subscribe(&p).await.unwrap();
        assert_eq!(sub.next_status().await.unwrap().unwrap().brightness, 10);
        assert_eq!(sub.next_status().await.unwrap().unwrap().brightness, 20);
        assert!(sub.next_status().await.unwrap().is_none(), "server closed");
    }

    #[tokio::test]
    async fn request_without_daemon_is_not_running() {
        let d = tempfile::tempdir().unwrap();
        assert!(matches!(
            request(&d.path().join("nobody.sock"), &Request::Status).await,
            Err(IpcError::NotRunning)
        ));
    }
}
