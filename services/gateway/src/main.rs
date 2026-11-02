// ============================================================================
// Kestrel cold-path WebSocket gateway (Milestone E1 "connected mode").
//
// Closes the ADR-0007 deferral: the console now talks to a gateway over a
// WebSocket instead of bundling data. The gateway streams the JSONL snapshot
// projection produced by the C++ cold path (tools/kestrel_journal_export,
// schema kestrel.demo/1 — the SAME file the standalone demo console plays).
//
// Honest boundary (see ADR-0007 / docs/ARCHITECTURE.md):
//   * Default mode streams a RECORDED projection with playback control
//     (play / pause / step / seek / speed) — timings preserved from the
//     journal, scaled by --speed.
//   * `--follow` tails a growing JSONL file and pushes snapshots as they
//     appear. This is the seam where a live bridge publisher attaches; no
//     real-time engine feed is fabricated here.
//
// Protocol (JSON text frames):
//   server -> client:
//     {"type":"hello","mode":"playback"|"follow","count":N,"schema":S,"source":P}
//     {"type":"snapshot","i":I,"data":{... kestrel.demo/1 snapshot ...}}
//     {"type":"end","count":N}                     (playback reached the end)
//     {"type":"error","message":M}
//   client -> server:
//     {"type":"play"} {"type":"pause"} {"type":"step"}
//     {"type":"seek","index":I} {"type":"speed","value":X}
//
// Endpoints:  GET /ws (WebSocket) · GET /api/health · GET /* (static files
// from --static, default: web/console-dist if built, else web/).
//
// Usage:
//   kestrel-gateway [--jsonl web/demo-data/demo.jsonl] [--static web/console-dist]
//                   [--host 127.0.0.1] [--port 8902] [--speed 1.0] [--follow]
// ============================================================================

use axum::{
    extract::{
        ws::{Message, WebSocket, WebSocketUpgrade},
        State,
    },
    http::StatusCode,
    response::IntoResponse,
    routing::get,
    Router,
};
use serde_json::{json, Value};
use std::{
    path::{Path, PathBuf},
    process::ExitCode,
    sync::Arc,
    time::Duration,
};
use tower_http::services::ServeDir;

const MIN_DELAY: Duration = Duration::from_millis(1);
const MAX_DELAY: Duration = Duration::from_millis(2000);
const FOLLOW_POLL: Duration = Duration::from_millis(100);

/// Loaded projection: parsed snapshots plus their journal timestamps, so the
/// playback loop can reproduce recorded pacing exactly (scaled by speed).
struct Snapshots {
    items: Vec<Value>,
    ts_ns: Vec<u64>,
    schema: String,
    source: PathBuf,
}

impl Snapshots {
    fn load(path: &Path) -> Result<Self, String> {
        let raw = std::fs::read_to_string(path)
            .map_err(|e| format!("cannot read {}: {e}", path.display()))?;
        let mut items = Vec::new();
        let mut ts_ns = Vec::new();
        for (ln, line) in raw.lines().enumerate() {
            let line = line.trim();
            if line.is_empty() {
                continue;
            }
            let v: Value = serde_json::from_str(line)
                .map_err(|e| format!("{}:{}: bad JSON: {e}", path.display(), ln + 1))?;
            if v.get("schema").and_then(Value::as_str) != Some("kestrel.demo/1") {
                return Err(format!("{}:{}: not a kestrel.demo/1 snapshot", path.display(), ln + 1));
            }
            let ts = v
                .get("ts_ns")
                .and_then(Value::as_u64)
                .ok_or_else(|| format!("{}:{}: missing ts_ns", path.display(), ln + 1))?;
            ts_ns.push(ts);
            items.push(v);
        }
        if items.is_empty() {
            return Err(format!("{}: no snapshots found", path.display()));
        }
        Ok(Self {
            items,
            ts_ns,
            schema: "kestrel.demo/1".to_string(),
            source: path.to_path_buf(),
        })
    }

    fn count(&self) -> usize {
        self.items.len()
    }

    /// Inter-snapshot wall delay as recorded, scaled by `speed` and clamped so
    /// pacing is visible for humans while staying faithful in relative terms.
    fn delay_between(&self, prev: usize, next: usize, speed: f64) -> Duration {
        let dt_ns = self.ts_ns[next].saturating_sub(self.ts_ns[prev]);
        let speed = if speed <= 0.0 { 1.0 } else { speed };
        let scaled_ns = (dt_ns as f64 / speed).max(0.0) as u64;
        Duration::from_nanos(scaled_ns.clamp(
            MIN_DELAY.as_nanos() as u64,
            MAX_DELAY.as_nanos() as u64,
        ))
    }
}

#[derive(Clone)]
struct AppState {
    snaps: Arc<Snapshots>,
    follow: bool,
    base_speed: f64,
}

#[tokio::main]
async fn main() -> ExitCode {
    let mut args = std::env::args().skip(1);
    let mut jsonl = PathBuf::from("web/demo-data/demo.jsonl");
    let mut static_dir = PathBuf::from("web/console-dist");
    let mut host = "127.0.0.1".to_string();
    let mut port: u16 = 8902;
    let mut follow = false;
    let mut base_speed = 1.0f64;
    while let Some(a) = args.next() {
        match a.as_str() {
            "--jsonl" => jsonl = args.next().map(PathBuf::from).unwrap_or(jsonl),
            "--static" => static_dir = args.next().map(PathBuf::from).unwrap_or(static_dir),
            "--host" => host = args.next().unwrap_or(host),
            "--port" => {
                port = args
                    .next()
                    .and_then(|v| v.parse().ok())
                    .unwrap_or(port)
            }
            "--speed" => {
                base_speed = args.next().and_then(|v| v.parse().ok()).unwrap_or(1.0)
            }
            "--follow" => follow = true,
            other => {
                eprintln!("unknown argument: {other}");
                return ExitCode::from(2);
            }
        }
    }

    let snaps = match Snapshots::load(&jsonl) {
        Ok(s) => Arc::new(s),
        Err(e) => {
            eprintln!("kestrel-gateway: {e}");
            return ExitCode::FAILURE;
        }
    };

    // Serve the built React console when present; fall back to the legacy
    // single-file demo directory so the gateway alone is still usable.
    let index = static_dir.join("index.html");
    let serve_dir = if index.is_file() {
        static_dir.clone()
    } else {
        PathBuf::from("web")
    };

    let state = AppState {
        snaps,
        follow,
        base_speed,
    };

    let snap_count = state.snaps.count();
    let app = Router::new()
        .route("/ws", get(ws_handler))
        .route("/api/health", get(health))
        .fallback_service(ServeDir::new(&serve_dir).append_index_html_on_directories(true))
        .with_state(state);

    let listener = match tokio::net::TcpListener::bind((host.as_str(), port)).await {
        Ok(l) => l,
        Err(e) => {
            eprintln!("kestrel-gateway: bind {host}:{port} failed: {e}");
            return ExitCode::FAILURE;
        }
    };
    eprintln!(
        "kestrel-gateway: listening on http://{host}:{port}/  ws={host}:{port}/ws  mode={}  snapshots={}  static={}",
        if follow { "follow" } else { "playback" },
        snap_count,
        serve_dir.display()
    );
    match axum::serve(listener, app).await {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("kestrel-gateway: server error: {e}");
            ExitCode::FAILURE
        }
    }
}

async fn health(State(st): State<AppState>) -> impl IntoResponse {
    (
        StatusCode::OK,
        axum::Json(json!({
            "ok": true,
            "schema": st.snaps.schema,
            "mode": if st.follow { "follow" } else { "playback" },
            "snapshots": st.snaps.count(),
            "source": st.snaps.source.display().to_string(),
        })),
    )
}

async fn ws_handler(
    ws: WebSocketUpgrade,
    State(st): State<AppState>,
) -> impl IntoResponse {
    ws.on_upgrade(move |socket| handle_socket(socket, st))
}

fn snap_msg(st: &Snapshots, i: usize) -> Message {
    Message::Text(
        json!({"type":"snapshot", "i": i, "data": st.items[i]}).to_string().into(),
    )
}

async fn handle_socket(mut socket: WebSocket, st: AppState) {
    let n = st.snaps.count();
    let mode = if st.follow { "follow" } else { "playback" };
    if socket
        .send(Message::Text(
            json!({"type":"hello","mode":mode,"count":n,
                   "schema":st.snaps.schema,"source":st.snaps.source.display().to_string()})
            .to_string()
            .into(),
        ))
        .await
        .is_err()
    {
        return;
    }

    if st.follow {
        follow_loop(&mut socket, &st).await;
    } else {
        playback_loop(&mut socket, &st).await;
    }
    let _ = socket.send(Message::Close(None)).await;
}

/// Recorded-projection playback: honours play/pause/step/seek/speed.
async fn playback_loop(socket: &mut WebSocket, st: &AppState) {
    let n = st.snaps.count();
    let mut playing = false;
    let mut speed = st.base_speed.max(0.001);
    let mut idx: usize = 0; // index of the NEXT snapshot to send
    let mut delay = Duration::ZERO;

    loop {
        tokio::select! {
            maybe = socket.recv() => {
                match maybe {
                    Some(Ok(Message::Text(text))) => {
                        let Ok(cmd) = serde_json::from_str::<Value>(&text) else {
                            let _ = socket.send(Message::Text(
                                json!({"type":"error","message":"unparseable command"}).to_string().into()
                            )).await;
                            continue;
                        };
                        match cmd.get("type").and_then(Value::as_str) {
                            Some("play") => {
                                playing = true;
                                delay = if idx < n { st.snaps.delay_between(idx.saturating_sub(1).min(n - 1), idx, speed) } else { Duration::ZERO };
                                // First frame after play should arrive promptly.
                                if idx == 0 { delay = Duration::ZERO; }
                                if idx >= n { playing = false; }
                            }
                            Some("pause") => playing = false,
                            Some("step") => {
                                playing = false;
                                if idx < n {
                                    if socket.send(snap_msg(&st.snaps, idx)).await.is_err() { return; }
                                    idx += 1;
                                } else {
                                    let _ = socket.send(Message::Text(json!({"type":"end","count":n}).to_string().into())).await;
                                }
                            }
                            Some("seek") => {
                                let want = cmd.get("index").and_then(Value::as_u64).unwrap_or(0) as usize;
                                idx = want.min(n.saturating_sub(1));
                                if socket.send(snap_msg(&st.snaps, idx)).await.is_err() { return; }
                                idx += 1;
                                if idx >= n {
                                    // Seek-to-last is terminal, same as natural end.
                                    playing = false;
                                    let _ = socket.send(Message::Text(json!({"type":"end","count":n}).to_string().into())).await;
                                }
                            }
                            Some("speed") => {
                                speed = cmd.get("value").and_then(Value::as_f64).unwrap_or(1.0).max(0.001);
                            }
                            _ => {
                                let _ = socket.send(Message::Text(
                                    json!({"type":"error","message":"unknown command type"}).to_string().into()
                                )).await;
                            }
                        }
                    }
                    Some(Ok(Message::Close(_))) | None => return,
                    Some(Ok(_)) => {} // ping/pong/binary: ignore
                    Some(Err(_)) => return,
                }
            }
            _ = tokio::time::sleep(delay), if playing && idx < n => {
                if socket.send(snap_msg(&st.snaps, idx)).await.is_err() { return; }
                idx += 1;
                if idx >= n {
                    playing = false;
                    let _ = socket.send(Message::Text(json!({"type":"end","count":n}).to_string().into())).await;
                } else {
                    delay = st.snaps.delay_between(idx - 1, idx, speed);
                }
            }
        }
    }
}

/// Live-seam mode: tail a growing JSONL file and push snapshots as they land.
/// Pause/resume are honoured; seek/speed do not apply (nothing is rewound).
async fn follow_loop(socket: &mut WebSocket, st: &AppState) {
    let path = st.snaps.source.clone();
    let mut file = match tokio::fs::File::open(&path).await {
        Ok(f) => f,
        Err(e) => {
            let _ = socket.send(Message::Text(
                json!({"type":"error","message":format!("follow open failed: {e}")}).to_string().into()
            )).await;
            return;
        }
    };
    let mut offset: u64 = match file.metadata().await {
        Ok(m) => m.len(),
        Err(_) => 0,
    };
    let mut paused = false;
    let mut buf = Vec::new();
    let mut pending = String::new();

    loop {
        tokio::select! {
            maybe = socket.recv() => {
                match maybe {
                    Some(Ok(Message::Text(text))) => {
                        if let Ok(cmd) = serde_json::from_str::<Value>(&text) {
                            match cmd.get("type").and_then(Value::as_str) {
                                Some("pause") => paused = true,
                                Some("play") => paused = false,
                                _ => {
                                    let _ = socket.send(Message::Text(
                                        json!({"type":"error","message":"follow mode supports play/pause only"}).to_string().into()
                                    )).await;
                                }
                            }
                        }
                    }
                    Some(Ok(Message::Close(_))) | None => return,
                    _ => {}
                }
            }
            _ = tokio::time::sleep(FOLLOW_POLL), if !paused => {
                use tokio::io::{AsyncReadExt, AsyncSeekExt};
                let meta = match file.metadata().await { Ok(m) => m, Err(_) => continue };
                if meta.len() > offset {
                    if file.seek(std::io::SeekFrom::Start(offset)).await.is_err() { return; }
                    buf.clear();
                    if (file.read_to_end(&mut buf).await).is_err() { return; }
                    offset += buf.len() as u64;
                    pending.push_str(&String::from_utf8_lossy(&buf));
                    // Only complete lines are emitted; a partial tail is kept.
                    if let Some(at) = pending.rfind('\n') {
                        let complete: String = pending.drain(..=at).collect();
                        for line in complete.lines().filter(|l| !l.trim().is_empty()) {
                            if let Ok(v) = serde_json::from_str::<Value>(line) {
                                if socket.send(Message::Text(
                                    json!({"type":"snapshot","i":null,"data":v}).to_string().into()
                                )).await.is_err() { return; }
                            }
                        }
                    }
                }
            }
        }
    }
}
