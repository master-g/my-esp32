use std::{
    collections::VecDeque,
    env, fs,
    net::SocketAddr,
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicU64, AtomicUsize, Ordering},
    },
    time::Duration,
};

use anyhow::{Context, Result, anyhow};
use axum::{
    Json, Router,
    extract::{
        State, WebSocketUpgrade,
        ws::{Message, WebSocket},
    },
    response::IntoResponse,
    routing::{get, post},
};
use directories::ProjectDirs;
use futures_util::SinkExt;
use tokio::{
    net::TcpListener,
    sync::{Mutex, broadcast},
    time::timeout,
};
use tracing::{debug, info, warn};

use crate::{
    model::{
        BridgeEnvelope, ErrorEnvelope, HelloMessage, LocalHookEvent, PersistedState, Snapshot,
        StatusResponse,
    },
    normalizer::{apply_notification_status, materially_equal, normalize},
};

const DEFAULT_ADMIN_ADDR: &str = "127.0.0.1:37125";
const DEFAULT_WS_ADDR: &str = "0.0.0.0:8765";
const RING_BUFFER_CAPACITY: usize = 64;
const PRE_TOOL_COALESCE_MS: u64 = 300;

#[derive(Debug, Clone)]
pub struct Config {
    pub admin_addr: SocketAddr,
    pub ws_addr: SocketAddr,
    pub admin_addr_raw: String,
    pub ws_addr_raw: String,
    pub psk: String,
    pub state_path: PathBuf,
}

impl Config {
    pub fn from_env() -> Result<Self> {
        let admin_addr_raw =
            env::var("CLAUDE_BRIDGE_ADMIN_ADDR").unwrap_or_else(|_| DEFAULT_ADMIN_ADDR.to_string());
        let ws_addr_raw =
            env::var("CLAUDE_BRIDGE_WS_ADDR").unwrap_or_else(|_| DEFAULT_WS_ADDR.to_string());
        let psk = env::var("CLAUDE_BRIDGE_PSK")
            .context("CLAUDE_BRIDGE_PSK must be set before starting daemon")?;
        let admin_addr = admin_addr_raw
            .parse()
            .with_context(|| format!("invalid admin addr: {admin_addr_raw}"))?;
        let ws_addr = ws_addr_raw
            .parse()
            .with_context(|| format!("invalid websocket addr: {ws_addr_raw}"))?;

        let state_dir = if let Ok(state_dir) = env::var("CLAUDE_BRIDGE_STATE_DIR") {
            PathBuf::from(state_dir)
        } else {
            let project_dirs = ProjectDirs::from("local", "claude-bridge", "claude-bridge")
                .ok_or_else(|| anyhow!("failed to resolve project directories"))?;
            project_dirs.data_dir().to_path_buf()
        };
        fs::create_dir_all(&state_dir).context("failed to create state directory")?;
        let state_path = state_dir.join("state.json");

        Ok(Self {
            admin_addr,
            ws_addr,
            admin_addr_raw,
            ws_addr_raw,
            psk,
            state_path,
        })
    }
}

#[derive(Debug)]
struct PendingToolEvent {
    generation: u64,
    event: LocalHookEvent,
}

#[derive(Debug)]
struct BridgeState {
    seq: u64,
    snapshot: Snapshot,
    history: VecDeque<Snapshot>,
    pending_tool: Option<PendingToolEvent>,
}

impl BridgeState {
    fn new(initial: Snapshot) -> Self {
        let seq = initial.seq;
        Self {
            seq,
            snapshot: initial,
            history: VecDeque::with_capacity(RING_BUFFER_CAPACITY),
            pending_tool: None,
        }
    }
}

#[derive(Clone)]
pub struct AppState {
    config: Config,
    state: Arc<Mutex<BridgeState>>,
    tx: broadcast::Sender<BridgeEnvelope>,
    connected_clients: Arc<AtomicUsize>,
    pending_generation: Arc<AtomicU64>,
}

pub async fn run(config: Config) -> Result<()> {
    let app_state = build_app_state(config.clone());
    let (admin_router, ws_router) = build_routers(app_state);

    let admin_listener = TcpListener::bind(config.admin_addr)
        .await
        .with_context(|| format!("failed to bind admin addr {}", config.admin_addr_raw))?;
    let ws_listener = TcpListener::bind(config.ws_addr)
        .await
        .with_context(|| format!("failed to bind websocket addr {}", config.ws_addr_raw))?;

    info!("admin endpoint listening on {}", config.admin_addr_raw);
    info!("websocket endpoint listening on {}", config.ws_addr_raw);

    let admin_task = tokio::spawn(async move { axum::serve(admin_listener, admin_router).await });
    let ws_task = tokio::spawn(async move { axum::serve(ws_listener, ws_router).await });

    tokio::select! {
        res = admin_task => {
            res.context("admin server join error")??;
        }
        res = ws_task => {
            res.context("websocket server join error")??;
        }
        _ = tokio::signal::ctrl_c() => {
            info!("received ctrl-c, shutting down");
        }
    }

    Ok(())
}

impl AppState {
    pub async fn ingest(self: &Arc<Self>, event: LocalHookEvent) {
        match event.hook_event_name.as_str() {
            "PreToolUse" => self.handle_pre_tool(event).await,
            "PostToolUse" => self.handle_post_tool(event).await,
            _ => {
                self.flush_pending_tool().await;
                self.process_event(event).await;
            }
        }
    }

    async fn handle_pre_tool(self: &Arc<Self>, event: LocalHookEvent) {
        self.flush_pending_tool().await;
        let generation = self.pending_generation.fetch_add(1, Ordering::SeqCst) + 1;
        {
            let mut guard = self.state.lock().await;
            guard.pending_tool = Some(PendingToolEvent {
                generation,
                event: event.clone(),
            });
        }

        let app = self.clone();
        tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(PRE_TOOL_COALESCE_MS)).await;
            app.flush_pending_tool_generation(generation).await;
        });
    }

    async fn handle_post_tool(self: &Arc<Self>, event: LocalHookEvent) {
        let matched_pending = {
            let mut guard = self.state.lock().await;
            if let Some(pending) = guard.pending_tool.as_ref() {
                if pending.event.tool_use_id == event.tool_use_id && event.tool_use_id.is_some() {
                    guard.pending_tool = None;
                    true
                } else {
                    false
                }
            } else {
                false
            }
        };

        if !matched_pending {
            self.flush_pending_tool().await;
        }
        self.process_event(event).await;
    }

    async fn flush_pending_tool_generation(self: &Arc<Self>, generation: u64) {
        let pending = {
            let mut guard = self.state.lock().await;
            if let Some(pending) = guard.pending_tool.take() {
                if pending.generation == generation {
                    Some(pending.event)
                } else {
                    guard.pending_tool = Some(pending);
                    None
                }
            } else {
                None
            }
        };

        if let Some(event) = pending {
            self.process_event(event).await;
        }
    }

    async fn flush_pending_tool(self: &Arc<Self>) {
        let pending = {
            let mut guard = self.state.lock().await;
            guard.pending_tool.take().map(|pending| pending.event)
        };

        if let Some(event) = pending {
            self.process_event(event).await;
        }
    }

    async fn process_event(self: &Arc<Self>, event: LocalHookEvent) {
        let (persist, broadcast) = {
            let mut guard = self.state.lock().await;
            let current = guard.snapshot.clone();
            let mut next = normalize(&event, &current);
            if event.hook_event_name == "Notification" {
                next = apply_notification_status(next, &current, &event);
            }

            if materially_equal(&next, &current) {
                guard.snapshot.ts = next.ts;
                debug!(
                    "coalesced duplicate state for event {}",
                    event.hook_event_name
                );
                return;
            }

            guard.seq += 1;
            next.seq = guard.seq;
            guard.snapshot = next.clone();
            guard.history.push_back(next.clone());
            while guard.history.len() > RING_BUFFER_CAPACITY {
                guard.history.pop_front();
            }

            (
                PersistedState {
                    seq: guard.seq,
                    snapshot: next.clone(),
                },
                BridgeEnvelope {
                    r#type: "delta".to_string(),
                    payload: next,
                },
            )
        };

        if let Err(err) = persist_state(&self.config.state_path, &persist) {
            warn!("failed to persist state: {err:#}");
        }

        let _ = self.tx.send(broadcast);
    }

    async fn snapshot(&self) -> Snapshot {
        self.state.lock().await.snapshot.clone()
    }

    fn connected_clients(&self) -> usize {
        self.connected_clients.load(Ordering::Relaxed)
    }
}

async fn post_event(
    State(state): State<AppState>,
    Json(event): Json<LocalHookEvent>,
) -> impl IntoResponse {
    Arc::new(state).ingest(event).await;
    Json(serde_json::json!({ "ok": true }))
}

async fn get_status(State(state): State<AppState>) -> impl IntoResponse {
    let snapshot = state.snapshot().await;
    Json(StatusResponse {
        admin_addr: state.config.admin_addr_raw.clone(),
        ws_addr: state.config.ws_addr_raw.clone(),
        current_seq: snapshot.seq,
        connected_clients: state.connected_clients(),
        snapshot,
    })
}

async fn get_snapshot(State(state): State<AppState>) -> impl IntoResponse {
    Json(state.snapshot().await)
}

async fn ws_handler(ws: WebSocketUpgrade, State(state): State<AppState>) -> impl IntoResponse {
    ws.on_upgrade(move |socket| handle_ws(socket, state))
}

async fn handle_ws(mut socket: WebSocket, state: AppState) {
    let hello = match timeout(Duration::from_secs(5), socket.recv()).await {
        Ok(Some(Ok(Message::Text(text)))) => serde_json::from_str::<HelloMessage>(&text).ok(),
        Ok(Some(Ok(Message::Binary(bytes)))) => serde_json::from_slice::<HelloMessage>(&bytes).ok(),
        _ => None,
    };

    let Some(hello) = hello else {
        let _ = socket
            .send(Message::Text(
                serde_json::to_string(&ErrorEnvelope {
                    r#type: "error".into(),
                    code: "invalid_hello".into(),
                })
                .unwrap()
                .into(),
            ))
            .await;
        let _ = socket.close().await;
        return;
    };

    if hello.r#type != "hello" || hello.token != state.config.psk {
        let _ = socket
            .send(Message::Text(
                serde_json::to_string(&ErrorEnvelope {
                    r#type: "error".into(),
                    code: "auth_failed".into(),
                })
                .unwrap()
                .into(),
            ))
            .await;
        let _ = socket.close().await;
        return;
    }

    state.connected_clients.fetch_add(1, Ordering::Relaxed);
    let mut rx = state.tx.subscribe();
    let mut snapshot = state.snapshot().await;
    if snapshot.seq == 0 {
        snapshot = Snapshot::empty(now_epoch());
    }
    if socket
        .send(json_message(&BridgeEnvelope {
            r#type: "snapshot".into(),
            payload: snapshot.clone(),
        }))
        .await
        .is_err()
    {
        state.connected_clients.fetch_sub(1, Ordering::Relaxed);
        return;
    }

    let mut last_sent_seq = snapshot.seq.max(hello.last_seq);

    loop {
        tokio::select! {
            recv = socket.recv() => {
                match recv {
                    Some(Ok(Message::Close(_))) | None => break,
                    Some(Ok(Message::Ping(payload))) => {
                        if socket.send(Message::Pong(payload)).await.is_err() {
                            break;
                        }
                    }
                    Some(Ok(_)) => {}
                    Some(Err(_)) => break,
                }
            }
            delta = rx.recv() => {
                match delta {
                    Ok(delta) => {
                        if delta.payload.seq <= last_sent_seq {
                            continue;
                        }
                        last_sent_seq = delta.payload.seq;
                        if socket.send(json_message(&delta)).await.is_err() {
                            break;
                        }
                    }
                    Err(broadcast::error::RecvError::Lagged(_)) => {
                        let snapshot = state.snapshot().await;
                        if socket.send(json_message(&BridgeEnvelope { r#type: "snapshot".into(), payload: snapshot })).await.is_err() {
                            break;
                        }
                    }
                    Err(broadcast::error::RecvError::Closed) => break,
                }
            }
            _ = tokio::time::sleep(Duration::from_secs(15)) => {
                if socket.send(Message::Ping(Vec::new().into())).await.is_err() {
                    break;
                }
            }
        }
    }

    state.connected_clients.fetch_sub(1, Ordering::Relaxed);
}

fn json_message<T: serde::Serialize>(value: &T) -> Message {
    Message::Text(serde_json::to_string(value).unwrap().into())
}

fn now_epoch() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

fn load_state(path: &PathBuf) -> Option<PersistedState> {
    let contents = fs::read_to_string(path).ok()?;
    serde_json::from_str(&contents).ok()
}

fn persist_state(path: &PathBuf, state: &PersistedState) -> Result<()> {
    let json = serde_json::to_string_pretty(state)?;
    fs::write(path, json)?;
    Ok(())
}

fn build_app_state(config: Config) -> AppState {
    let initial_snapshot = load_state(&config.state_path)
        .map(|persisted| persisted.snapshot)
        .unwrap_or_else(|| Snapshot::empty(now_epoch()));
    let (tx, _) = broadcast::channel(256);

    AppState {
        config,
        state: Arc::new(Mutex::new(BridgeState::new(initial_snapshot))),
        tx,
        connected_clients: Arc::new(AtomicUsize::new(0)),
        pending_generation: Arc::new(AtomicU64::new(0)),
    }
}

fn build_routers(app_state: AppState) -> (Router, Router) {
    let admin_router = Router::new()
        .route("/v1/events", post(post_event))
        .route("/v1/status", get(get_status))
        .route("/v1/snapshot", get(get_snapshot))
        .with_state(app_state.clone());

    let ws_router = Router::new()
        .route("/ws", get(ws_handler))
        .with_state(app_state);

    (admin_router, ws_router)
}

#[cfg(test)]
mod tests {
    use std::{fs, time::SystemTime};

    use anyhow::Result;
    use futures_util::{SinkExt, StreamExt};
    use reqwest::Client;
    use tokio::sync::oneshot;
    use tokio_tungstenite::{connect_async, tungstenite::Message as WsMessage};

    use super::*;
    use crate::model::{BridgeEnvelope, LocalHookEvent};

    #[tokio::test]
    async fn websocket_receives_snapshot_then_delta_after_event_ingest() -> Result<()> {
        let unique = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)?
            .as_nanos();
        let state_dir = std::env::temp_dir().join(format!("claude-bridge-ws-test-{unique}"));
        fs::create_dir_all(&state_dir)?;

        let config = Config {
            admin_addr: "127.0.0.1:0".parse()?,
            ws_addr: "127.0.0.1:0".parse()?,
            admin_addr_raw: "127.0.0.1:0".to_string(),
            ws_addr_raw: "127.0.0.1:0".to_string(),
            psk: "test-psk".to_string(),
            state_path: state_dir.join("state.json"),
        };

        let app_state = build_app_state(config);
        let (admin_router, ws_router) = build_routers(app_state.clone());

        let admin_listener = TcpListener::bind("127.0.0.1:0").await?;
        let admin_addr = admin_listener.local_addr()?;
        let ws_listener = TcpListener::bind("127.0.0.1:0").await?;
        let ws_addr = ws_listener.local_addr()?;

        let (admin_tx, admin_rx) = oneshot::channel::<()>();
        let (ws_tx, ws_rx) = oneshot::channel::<()>();

        let admin_task = tokio::spawn(async move {
            axum::serve(admin_listener, admin_router)
                .with_graceful_shutdown(async {
                    let _ = admin_rx.await;
                })
                .await
        });
        let ws_task = tokio::spawn(async move {
            axum::serve(ws_listener, ws_router)
                .with_graceful_shutdown(async {
                    let _ = ws_rx.await;
                })
                .await
        });

        let (mut ws_stream, _) = connect_async(format!("ws://{ws_addr}/ws")).await?;
        ws_stream
            .send(WsMessage::Text(
                serde_json::json!({
                    "type": "hello",
                    "token": "test-psk",
                    "device_id": "esp32-dashboard-test",
                    "last_seq": 0_u64
                })
                .to_string()
                .into(),
            ))
            .await?;

        let snapshot_message = timeout(Duration::from_secs(2), ws_stream.next())
            .await?
            .expect("websocket should yield initial snapshot")?;
        let snapshot_text = snapshot_message.into_text()?;
        let snapshot_envelope: BridgeEnvelope = serde_json::from_str(&snapshot_text)?;
        assert_eq!(snapshot_envelope.r#type, "snapshot");
        assert_eq!(snapshot_envelope.payload.seq, 0);
        assert_eq!(snapshot_envelope.payload.title, "No data yet");

        Client::new()
            .post(format!("http://{admin_addr}/v1/events"))
            .json(&LocalHookEvent {
                session_id: "sess-1".into(),
                cwd: "/tmp/project-foo".into(),
                hook_event_name: "SessionStart".into(),
                message: None,
                prompt_preview: None,
                tool_name: None,
                tool_use_id: None,
                permission_mode: "default".into(),
                recv_ts: now_epoch(),
            })
            .send()
            .await?
            .error_for_status()?;

        let delta_message = timeout(Duration::from_secs(2), ws_stream.next())
            .await?
            .expect("websocket should yield a delta after ingest")?;
        let delta_text = delta_message.into_text()?;
        let delta_envelope: BridgeEnvelope = serde_json::from_str(&delta_text)?;
        assert_eq!(delta_envelope.r#type, "delta");
        assert_eq!(delta_envelope.payload.seq, 1);
        assert_eq!(delta_envelope.payload.event, "SessionStart");
        assert_eq!(delta_envelope.payload.status, "waiting_for_input");
        assert_eq!(delta_envelope.payload.workspace, "project-foo");

        let _ = admin_tx.send(());
        let _ = ws_tx.send(());
        let _ = ws_stream.close(None).await;
        admin_task.await??;
        ws_task.await??;

        Ok(())
    }
}
